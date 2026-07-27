#include "native/tabletennis_scene_owner_observer.h"

#include "generated/default/tabletennis_init.h"
#include "native/tabletennis_frame_scene.h"
#include "native/tabletennis_guest_memory.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <string_view>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_UINT32(
    tabletennis_native_scene_owner_log_interval, 0, "Table Tennis",
    "Guest frames between observer-only table/ball/paddle owner reports "
    "(0 disables capture).")
    .range(0, 100000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace tabletennis::native {
namespace {

constexpr uint32_t kLvlTableRenderableVtable = 0x8204F6D4;
constexpr uint32_t kGmBallNodeRenderableVtable = 0x82057288;
constexpr uint32_t kPongPaddleRenderableVtable = 0x82071678;
constexpr uint32_t kEmbeddedRenderableOffset = 0x60;
constexpr uint32_t kBallSpinRenderableOffset = 0xB4;
constexpr uint32_t kBallSplashObjectOffset = 0xBC;
constexpr uint32_t kBallSplashRenderableOffset = 4;
constexpr size_t kMaxRenderScopeDepth = 12;
constexpr size_t kMaxSubmissionScopeDepth = 4;
constexpr size_t kMaxQueuedMappings = 16;

struct SubmissionExpected {
  SceneOwnerToken token{};
};

struct SubmissionScope {
  bool active = false;
  SceneOwnerKind kind = SceneOwnerKind::kNone;
  uint32_t owner = 0;
  uint32_t expected_count = 0;
  std::array<SubmissionExpected, 2> expected{};
};

struct QueuedOwnerMapping {
  SceneOwnerToken token{};
  uint64_t frame_sequence = 0;
};

thread_local std::array<SceneOwnerToken, kMaxRenderScopeDepth>
    g_render_scope_stack;
thread_local size_t g_render_scope_depth = 0;
thread_local std::array<SubmissionScope, kMaxSubmissionScopeDepth>
    g_submission_scope_stack;
thread_local size_t g_submission_scope_depth = 0;

std::mutex g_owner_mutex;
SceneOwnerObserverFrame g_building;
SceneOwnerObserverFrame g_published;
std::array<QueuedOwnerMapping, kMaxQueuedMappings> g_queued_mappings;
uint64_t g_frame_sequence = 0;
bool g_was_enabled = false;

bool CheckedGuestOffset(uint32_t address, size_t offset, size_t size,
                        uint32_t &result) {
  if (address == 0) {
    return false;
  }
  const uint64_t start = static_cast<uint64_t>(address) + offset;
  const uint64_t end = start + size;
  if (start > std::numeric_limits<uint32_t>::max() ||
      end > (uint64_t{1} << 32)) {
    return false;
  }
  result = static_cast<uint32_t>(start);
  return true;
}

uint32_t LoadBeU32(const std::byte *bytes) {
  uint32_t value = 0;
  std::memcpy(&value, bytes, sizeof(value));
  return std::byteswap(value);
}

bool TryReadBeU32(uint8_t *guest_base, uint32_t address, size_t offset,
                  uint32_t &value) {
  uint32_t guest_address = 0;
  if (guest_base == nullptr ||
      !CheckedGuestOffset(address, offset, sizeof(uint32_t), guest_address)) {
    return false;
  }
  std::array<std::byte, sizeof(uint32_t)> bytes{};
  const void *host_address =
      guest_base + guest_address + REX_PHYS_HOST_OFFSET(guest_address);
  if (!GuestTryCopy(bytes.data(), host_address, bytes.size())) {
    return false;
  }
  value = LoadBeU32(bytes.data());
  return true;
}

std::string_view KindName(SceneOwnerKind kind) {
  switch (kind) {
  case SceneOwnerKind::kNone:
    return "none";
  case SceneOwnerKind::kTable:
    return "table";
  case SceneOwnerKind::kBall:
    return "ball";
  case SceneOwnerKind::kPaddle:
    return "paddle";
  }
  return "unknown";
}

std::string_view RoleName(SceneOwnerRole role) {
  switch (role) {
  case SceneOwnerRole::kNone:
    return "none";
  case SceneOwnerRole::kTableRenderable:
    return "table-renderable";
  case SceneOwnerRole::kBallTransformSource:
    return "ball-transform";
  case SceneOwnerRole::kBallNodeRenderable:
    return "ball-node";
  case SceneOwnerRole::kBallSpinRenderable:
    return "ball-spin";
  case SceneOwnerRole::kBallSplashRenderable:
    return "ball-splash";
  case SceneOwnerRole::kPaddleRenderable:
    return "paddle-renderable";
  }
  return "unknown";
}

bool SameIdentity(const SceneOwnerToken &left,
                  const SceneOwnerToken &right) {
  return left.kind == right.kind && left.role == right.role &&
         left.owner == right.owner && left.renderable == right.renderable &&
         left.renderable_vtable == right.renderable_vtable;
}

SceneOwnerIdentityStats *FindOrAppendIdentityLocked(
    const SceneOwnerToken &token) {
  if (!token.valid) {
    return nullptr;
  }
  const auto begin = g_building.identities.begin();
  const auto end = begin + g_building.identity_count;
  const auto existing = std::find_if(begin, end, [&](const auto &candidate) {
    return SameIdentity(candidate.token, token);
  });
  if (existing != end) {
    return &*existing;
  }
  if (g_building.identity_count == g_building.identities.size()) {
    ++g_building.dropped_identity_count;
    return nullptr;
  }
  SceneOwnerIdentityStats &identity =
      g_building.identities[g_building.identity_count++];
  identity.token = token;
  return &identity;
}

SceneOwnerToken CaptureDirectToken(uint8_t *guest_base, SceneOwnerKind kind,
                                   SceneOwnerRole role, uint32_t owner,
                                   uint32_t renderable,
                                   uint32_t expected_vtable) {
  SceneOwnerToken token;
  token.kind = kind;
  token.role = role;
  token.owner = owner;
  token.renderable = renderable;
  if (!TryReadBeU32(guest_base, renderable, 0,
                    token.renderable_vtable)) {
    std::lock_guard lock(g_owner_mutex);
    ++g_building.guest_read_failure_count;
    return token;
  }
  token.valid = owner != 0 && renderable != 0 &&
                token.renderable_vtable == expected_vtable;
  return token;
}

void PushRenderScope(const SceneOwnerToken &token) {
  if (g_render_scope_depth < g_render_scope_stack.size()) {
    g_render_scope_stack[g_render_scope_depth] = token;
  }
  ++g_render_scope_depth;
}

SubmissionScope *CurrentSubmissionScope() {
  if (g_submission_scope_depth == 0 ||
      g_submission_scope_depth > g_submission_scope_stack.size()) {
    return nullptr;
  }
  return &g_submission_scope_stack[g_submission_scope_depth - 1];
}

void StoreQueuedMappingLocked(const SceneOwnerToken &token) {
  auto existing = std::find_if(
      g_queued_mappings.begin(), g_queued_mappings.end(),
      [&](const auto &mapping) {
        return mapping.token.renderable == token.renderable;
      });
  if (existing == g_queued_mappings.end()) {
    existing = std::find_if(g_queued_mappings.begin(),
                            g_queued_mappings.end(),
                            [](const auto &mapping) {
                              return !mapping.token.valid;
                            });
  }
  if (existing == g_queued_mappings.end()) {
    existing = std::min_element(
        g_queued_mappings.begin(), g_queued_mappings.end(),
        [](const auto &left, const auto &right) {
          return left.frame_sequence < right.frame_sequence;
        });
  }
  existing->token = token;
  existing->frame_sequence = g_frame_sequence;
}

SceneOwnerToken FindCurrentMappingLocked(uint32_t renderable) {
  const auto mapping = std::find_if(
      g_queued_mappings.begin(), g_queued_mappings.end(),
      [&](const auto &candidate) {
        return candidate.token.valid &&
               candidate.token.renderable == renderable &&
               candidate.frame_sequence == g_frame_sequence;
      });
  return mapping == g_queued_mappings.end() ? SceneOwnerToken{}
                                            : mapping->token;
}

void RecordRenderScope(const SceneOwnerToken &token) {
  if (!token.valid) {
    return;
  }
  std::lock_guard lock(g_owner_mutex);
  switch (token.kind) {
  case SceneOwnerKind::kTable:
    ++g_building.table_render_scope_count;
    break;
  case SceneOwnerKind::kBall:
    ++g_building.ball_render_scope_count;
    break;
  case SceneOwnerKind::kPaddle:
    ++g_building.paddle_render_scope_count;
    break;
  case SceneOwnerKind::kNone:
    break;
  }
  if (SceneOwnerIdentityStats *identity =
          FindOrAppendIdentityLocked(token)) {
    ++identity->render_scope_count;
  }
}

void BeginDirectRenderScope(uint8_t *guest_base, SceneOwnerKind kind,
                            SceneOwnerRole role, uint32_t owner,
                            uint32_t renderable, uint32_t expected_vtable) {
  SceneOwnerToken token;
  if (SceneOwnerObserverEnabled()) {
    token = CaptureDirectToken(guest_base, kind, role, owner, renderable,
                               expected_vtable);
  }
  PushRenderScope(token);
  RecordRenderScope(token);
}

void PushSubmissionScope(const SubmissionScope &scope) {
  if (g_submission_scope_depth < g_submission_scope_stack.size()) {
    g_submission_scope_stack[g_submission_scope_depth] = scope;
  }
  ++g_submission_scope_depth;
}

void LogFrame(const SceneOwnerObserverFrame &frame) {
  REXLOG_INFO(
      "Table Tennis scene owners: frame={} table_scopes={} "
      "ball_submissions={} ball_transforms={} ball_scopes={} "
      "paddle_submissions={} "
      "paddle_scopes={} tagged_queue={} unmatched_queue={} "
      "catalog_draws={} model_tokens={} identities={} dropped_identities={} "
      "read_failures={} observer_only=true",
      frame.sequence, frame.table_render_scope_count,
      frame.ball_submission_count, frame.ball_transform_count,
      frame.ball_render_scope_count,
      frame.paddle_submission_count, frame.paddle_render_scope_count,
      frame.tagged_queue_count, frame.unmatched_queue_count,
      frame.tagged_catalog_draw_count, frame.tagged_model_token_count,
      frame.identity_count, frame.dropped_identity_count,
      frame.guest_read_failure_count);
  for (uint32_t index = 0; index < frame.identity_count; ++index) {
    const SceneOwnerIdentityStats &identity = frame.identities[index];
    REXLOG_INFO(
        "  scene_owner[{}] kind={} role={} owner={:08X} "
        "renderable={:08X} vtable={:08X} queued={} scopes={} "
        "catalog_draws={} model_tokens={}",
        index, KindName(identity.token.kind), RoleName(identity.token.role),
        identity.token.owner, identity.token.renderable,
        identity.token.renderable_vtable, identity.queue_count,
        identity.render_scope_count, identity.catalog_draw_count,
        identity.model_token_count);
  }
}

} // namespace

bool SceneOwnerObserverEnabled() {
  return REXCVAR_GET(tabletennis_native_scene_owner_log_interval) != 0 ||
         NativeFrameSceneCaptureEnabled();
}

void BeginSceneOwnerTableRenderScope(uint8_t *guest_base,
                                     uint32_t renderable) {
  const uint32_t owner =
      renderable >= kEmbeddedRenderableOffset
          ? renderable - kEmbeddedRenderableOffset
          : 0;
  BeginDirectRenderScope(guest_base, SceneOwnerKind::kTable,
                         SceneOwnerRole::kTableRenderable, owner, renderable,
                         kLvlTableRenderableVtable);
}

void BeginSceneOwnerBallNodeRenderScope(uint8_t *guest_base,
                                        uint32_t renderable) {
  const uint32_t owner =
      renderable >= kEmbeddedRenderableOffset
          ? renderable - kEmbeddedRenderableOffset
          : 0;
  BeginDirectRenderScope(guest_base, SceneOwnerKind::kBall,
                         SceneOwnerRole::kBallNodeRenderable, owner,
                         renderable, kGmBallNodeRenderableVtable);
}

void BeginSceneOwnerPaddleRenderScope(uint8_t *guest_base,
                                      uint32_t paddle) {
  BeginDirectRenderScope(guest_base, SceneOwnerKind::kPaddle,
                         SceneOwnerRole::kPaddleRenderable, paddle, paddle,
                         kPongPaddleRenderableVtable);
}

void BeginSceneOwnerMappedBallRenderScope(uint8_t *guest_base,
                                          uint32_t renderable) {
  SceneOwnerToken token;
  if (SceneOwnerObserverEnabled()) {
    {
      std::lock_guard lock(g_owner_mutex);
      token = FindCurrentMappingLocked(renderable);
    }
    uint32_t live_vtable = 0;
    if (token.valid &&
        (!TryReadBeU32(guest_base, renderable, 0, live_vtable) ||
         live_vtable != token.renderable_vtable)) {
      token = {};
      std::lock_guard lock(g_owner_mutex);
      ++g_building.guest_read_failure_count;
    }
  }
  PushRenderScope(token);
  RecordRenderScope(token);
}

void EndSceneOwnerRenderScope() {
  if (g_render_scope_depth != 0) {
    --g_render_scope_depth;
  }
}

void BeginSceneOwnerBallSubmission(uint8_t *guest_base, uint32_t ball) {
  SubmissionScope scope;
  scope.active = SceneOwnerObserverEnabled() && ball != 0;
  scope.kind = SceneOwnerKind::kBall;
  scope.owner = ball;
  if (scope.active) {
    {
      std::lock_guard lock(g_owner_mutex);
      ++g_building.ball_submission_count;
    }

    uint32_t spin = 0;
    if (TryReadBeU32(guest_base, ball, kBallSpinRenderableOffset, spin) &&
        spin != 0) {
      SceneOwnerToken &token =
          scope.expected[scope.expected_count++].token;
      token.kind = SceneOwnerKind::kBall;
      token.role = SceneOwnerRole::kBallSpinRenderable;
      token.owner = ball;
      token.renderable = spin;
    } else {
      std::lock_guard lock(g_owner_mutex);
      ++g_building.guest_read_failure_count;
    }

    uint32_t splash = 0;
    if (TryReadBeU32(guest_base, ball, kBallSplashObjectOffset, splash) &&
        splash != 0 &&
        splash <=
            std::numeric_limits<uint32_t>::max() -
                kBallSplashRenderableOffset) {
      SceneOwnerToken &token =
          scope.expected[scope.expected_count++].token;
      token.kind = SceneOwnerKind::kBall;
      token.role = SceneOwnerRole::kBallSplashRenderable;
      token.owner = ball;
      token.renderable = splash + kBallSplashRenderableOffset;
    } else {
      std::lock_guard lock(g_owner_mutex);
      ++g_building.guest_read_failure_count;
    }
  }
  PushSubmissionScope(scope);
}

void BeginSceneOwnerPaddleSubmission(uint8_t *guest_base,
                                      uint32_t paddle) {
  SubmissionScope scope;
  scope.active = SceneOwnerObserverEnabled() && paddle != 0;
  scope.kind = SceneOwnerKind::kPaddle;
  scope.owner = paddle;
  if (scope.active) {
    {
      std::lock_guard lock(g_owner_mutex);
      ++g_building.paddle_submission_count;
    }
    SceneOwnerToken &token = scope.expected[0].token;
    token.kind = SceneOwnerKind::kPaddle;
    token.role = SceneOwnerRole::kPaddleRenderable;
    token.owner = paddle;
    token.renderable = paddle;
    scope.expected_count = 1;
    uint32_t vtable = 0;
    if (!TryReadBeU32(guest_base, paddle, 0, vtable) ||
        vtable != kPongPaddleRenderableVtable) {
      scope.active = false;
      std::lock_guard lock(g_owner_mutex);
      ++g_building.guest_read_failure_count;
    }
  }
  PushSubmissionScope(scope);
}

void EndSceneOwnerSubmission() {
  if (g_submission_scope_depth != 0) {
    --g_submission_scope_depth;
  }
}

void ObserveSceneOwnerDrawBucketEntry(uint32_t renderable,
                                      uint32_t vtable) {
  if (!SceneOwnerObserverEnabled()) {
    return;
  }
  SubmissionScope *scope = CurrentSubmissionScope();
  if (scope == nullptr || !scope->active) {
    return;
  }
  const auto begin = scope->expected.begin();
  const auto end = begin + scope->expected_count;
  const auto expected = std::find_if(
      begin, end, [&](const auto &candidate) {
        return candidate.token.renderable == renderable;
      });
  if (expected == end || renderable == 0 || vtable == 0) {
    std::lock_guard lock(g_owner_mutex);
    ++g_building.unmatched_queue_count;
    return;
  }

  SceneOwnerToken token = expected->token;
  token.renderable_vtable = vtable;
  token.valid = true;
  std::lock_guard lock(g_owner_mutex);
  ++g_building.tagged_queue_count;
  StoreQueuedMappingLocked(token);
  if (SceneOwnerIdentityStats *identity =
          FindOrAppendIdentityLocked(token)) {
    ++identity->queue_count;
  }
}

void ObserveSceneOwnerBallTransform(uint32_t ball) {
  if (!SceneOwnerObserverEnabled() || ball == 0) {
    return;
  }
  SceneOwnerToken token;
  token.kind = SceneOwnerKind::kBall;
  token.role = SceneOwnerRole::kBallTransformSource;
  token.owner = ball;
  token.valid = true;
  std::lock_guard lock(g_owner_mutex);
  ++g_building.ball_transform_count;
  FindOrAppendIdentityLocked(token);
}

SceneOwnerToken CurrentSceneOwnerToken() {
  if (g_render_scope_depth == 0 ||
      g_render_scope_depth > g_render_scope_stack.size()) {
    return {};
  }
  return g_render_scope_stack[g_render_scope_depth - 1];
}

void ObserveSceneOwnerCatalogDraw(const SceneOwnerToken &token) {
  if (!SceneOwnerObserverEnabled() || !token.valid) {
    return;
  }
  std::lock_guard lock(g_owner_mutex);
  ++g_building.tagged_catalog_draw_count;
  if (SceneOwnerIdentityStats *identity =
          FindOrAppendIdentityLocked(token)) {
    ++identity->catalog_draw_count;
  }
}

void ObserveSceneOwnerModelTokens(const SceneOwnerToken &token,
                                  uint32_t token_count) {
  if (!SceneOwnerObserverEnabled() || !token.valid || token_count == 0) {
    return;
  }
  std::lock_guard lock(g_owner_mutex);
  g_building.tagged_model_token_count += token_count;
  if (SceneOwnerIdentityStats *identity =
          FindOrAppendIdentityLocked(token)) {
    identity->model_token_count += token_count;
  }
}

void SceneOwnerObserverFrameEnd() {
  const uint32_t interval =
      REXCVAR_GET(tabletennis_native_scene_owner_log_interval);
  const bool capture_enabled = SceneOwnerObserverEnabled();

  std::lock_guard lock(g_owner_mutex);
  ++g_frame_sequence;
  if (!capture_enabled) {
    g_building = {};
    g_queued_mappings = {};
    if (g_was_enabled) {
      g_published = {};
      g_published.sequence = g_frame_sequence;
    }
    g_was_enabled = false;
    return;
  }

  g_building.sequence = g_frame_sequence;
  g_published = g_building;
  g_building = {};
  g_queued_mappings = {};
  g_was_enabled = true;
  if (interval != 0 && g_published.sequence % interval == 0) {
    LogFrame(g_published);
  }
}

SceneOwnerObserverFrame LatestSceneOwnerObserverFrame() {
  std::lock_guard lock(g_owner_mutex);
  return g_published;
}

} // namespace tabletennis::native
