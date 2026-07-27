#include "native/tabletennis_player_replacement_candidates.h"

#include "native/tabletennis_player_observer_renderer.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <limits>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include <rex/cvar.h>
#include <rex/graphics/native_guest_renderer.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(
    tabletennis_native_player_replacement_prewarm, false, "Table Tennis",
    "Observer-only: prewarm exact borrowed CA9 player pipelines and "
    "descriptors, then always execute the original guest draw.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(
    tabletennis_native_player_replacement_gate_diagnostic, false,
    "Table Tennis",
    "Restart-required observer-only CA9 diagnostic: report which Vulkan "
    "selective-replacement eligibility gate prevents the player draw from "
    "reaching the dispatcher.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace tabletennis::native {
namespace {

constexpr uint64_t kPlayerVertexShaderHash = 0xCA9BBF96B0928616ull;
constexpr uint64_t kPlayerPixelShaderHash = 0x77AF85E1AF823D02ull;
constexpr uint32_t kPlayerPrimitiveType = 0x06;
constexpr size_t kMaximumLiveTitleTokens = 512;
constexpr size_t kMaximumBackendEvents = 512;
constexpr size_t kMaximumLiveTitleFrames = 16;
constexpr uint32_t kExpectedBackendTileBlocks = 3;
constexpr uint64_t kMaximumAsyncFrameAge = 12;
constexpr uint64_t kFnvOffsetBasis = 1469598103934665603ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;

struct PhaseDrawState {
  uint32_t normalized_depth_control;
  uint32_t normalized_color_mask;
  uint32_t color_control;
  uint32_t blend_control_0;
};

constexpr PhaseDrawState kPrepassDrawState = {
    .normalized_depth_control = 0x00700736,
    .normalized_color_mask = 0x00000008,
    .color_control = 0x87000015,
    .blend_control_0 = 0x00010001,
};
constexpr PhaseDrawState kColorDrawState = {
    .normalized_depth_control = 0x00700732,
    .normalized_color_mask = 0x0000000F,
    .color_control = 0x8700000C,
    .blend_control_0 = 0x07060706,
};

constexpr std::array<std::pair<uint32_t, uint32_t>, 3> kPhaseDescriptorPairs = {
    {
        {0x40106638, 0x4010664C},
        {0x4010D638, 0x4010D64C},
        {0x401145B8, 0x401145CC},
    }};

struct PhaseInfo {
  PlayerReplacementPhase phase = PlayerReplacementPhase::kDepthAlphaPrepass;
  uint32_t counterpart_descriptor = 0;
};

struct CandidateToken {
  std::shared_ptr<const PlayerReplacementCandidate> candidate;
  bool consumed = false;
};

struct PhaseGroup {
  PlayerSkinMeshIdentity mesh{};
  std::vector<size_t> draw_indices;
  bool eligible = false;
};

struct BackendIdentity {
  uint32_t primitive_type = 0;
  uint32_t submitted_index_count = 0;
  uint32_t guest_index_base = 0;

  bool operator==(const BackendIdentity &) const = default;
};

struct LiveTitleToken {
  uint64_t title_generation = 0;
  uint64_t frame_sequence = 0;
  uint32_t frame_order = 0;
  uint32_t ordinal = 0;
  PlayerReplacementPhase phase =
      PlayerReplacementPhase::kDepthAlphaPrepass;
  PlayerSkinMeshIdentity mesh{};
  uint64_t vertex_shader_hash = 0;
  uint64_t pixel_shader_hash = 0;
  BackendIdentity backend{};
  uint64_t palette_fingerprint = 0;
  uint64_t constants_fingerprint = 0;
  uint64_t material_fingerprint = 0;
  bool immutable_verified = false;
  bool valid = false;
};

struct BackendProofEvent {
  uint64_t event_generation = 0;
  uint64_t observed_title_frame_sequence = 0;
  PlayerReplacementPhase phase =
      PlayerReplacementPhase::kDepthAlphaPrepass;
  BackendIdentity backend{};
  uint32_t render_pass_key = 0;
  uint32_t surface_pitch = 0;
  bool valid = false;
};

struct LiveTitleFrame {
  uint64_t frame_sequence = 0;
  uint64_t first_title_generation = 0;
  uint64_t last_title_generation = 0;
  uint32_t token_count = 0;
  uint32_t immutable_candidate_count = 0;
  uint32_t backend_cursor = 0;
  uint32_t backend_tile_blocks = 0;
  bool immutable_finalized = false;
  bool valid = false;
};

std::mutex g_candidate_mutex;
std::vector<CandidateToken> g_tokens;
std::shared_ptr<const PlayerReplacementCandidate> g_pending_candidate;
std::array<LiveTitleToken, kMaximumLiveTitleTokens> g_live_title_tokens{};
std::array<BackendProofEvent, kMaximumBackendEvents> g_backend_events{};
std::array<LiveTitleFrame, kMaximumLiveTitleFrames> g_live_title_frames{};
PlayerReplacementCandidateTelemetry g_telemetry;
uint64_t g_candidate_generation = 0;
uint64_t g_phase_pair_generation = 0;
uint64_t g_title_generation = 0;
uint64_t g_backend_event_generation = 0;
uint64_t g_announced_contract_signature = 0;
uint64_t g_announced_parity_signature = 0;
bool g_announced_async_backend_proof = false;
bool g_announced_async_backend_mismatch = false;
bool g_announced_async_backend_ambiguity = false;
bool g_announced_backend_hash_callback = false;
bool g_announced_backend_pre_gate_callback = false;
bool g_announced_backend_contract_rejection = false;
bool g_announced_backend_pre_title_callback = false;
bool g_announced_backend_probe_summary = false;
bool g_announced_prewarm_success = false;
bool g_announced_prewarm_failure = false;

struct PrewarmFrameProgress {
  uint64_t frame_sequence = 0;
  uint32_t expected = 0;
  uint32_t attempted = 0;
  uint32_t exact_succeeded = 0;
  uint32_t compatible_succeeded = 0;
  uint32_t failed = 0;
};

PrewarmFrameProgress g_prewarm_frame_progress;

constexpr bool MatchesPhaseDrawState(
    PlayerReplacementPhase phase, uint32_t normalized_depth_control,
    uint32_t normalized_color_mask, uint32_t color_control,
    uint32_t blend_control_0, bool primitive_restart_enabled) {
  const PhaseDrawState& expected =
      phase == PlayerReplacementPhase::kDepthAlphaPrepass
          ? kPrepassDrawState
          : kColorDrawState;
  // PA_SU_SC_MODE_CNTL.multi_prim_ib_ena is clear in the proven CA9 block.
  // VGT_MULTI_PRIM_IB_RESET_INDX has no draw semantics while restart is
  // disabled, so its raw diagnostic value is deliberately not compared.
  return !primitive_restart_enabled &&
         normalized_depth_control == expected.normalized_depth_control &&
         normalized_color_mask == expected.normalized_color_mask &&
         color_control == expected.color_control &&
         blend_control_0 == expected.blend_control_0;
}

static_assert(MatchesPhaseDrawState(
    PlayerReplacementPhase::kDepthAlphaPrepass, 0x00700736, 0x00000008,
    0x87000015, 0x00010001, false));
static_assert(MatchesPhaseDrawState(
    PlayerReplacementPhase::kBlendedColor, 0x00700732, 0x0000000F,
    0x8700000C, 0x07060706, false));
static_assert(!MatchesPhaseDrawState(
    PlayerReplacementPhase::kBlendedColor, 0x00700736, 0x00000008,
    0x87000015, 0x00010001, false));
static_assert(!MatchesPhaseDrawState(
    PlayerReplacementPhase::kDepthAlphaPrepass, 0x00700736, 0x00000008,
    0x87000015, 0x00010001, true));

std::optional<PhaseInfo> PhaseForDescriptor(uint32_t descriptor) {
  for (const auto &[prepass, color] : kPhaseDescriptorPairs) {
    if (descriptor == prepass) {
      return PhaseInfo{
          .phase = PlayerReplacementPhase::kDepthAlphaPrepass,
          .counterpart_descriptor = color,
      };
    }
    if (descriptor == color) {
      return PhaseInfo{
          .phase = PlayerReplacementPhase::kBlendedColor,
          .counterpart_descriptor = prepass,
      };
    }
  }
  return std::nullopt;
}

void FingerprintAppend(uint64_t &fingerprint, uint64_t value) {
  fingerprint = (fingerprint ^ value) * kFnvPrime;
}

template <size_t Size>
void FingerprintFloats(uint64_t &fingerprint,
                       const std::array<float, Size> &values) {
  for (float value : values) {
    FingerprintAppend(fingerprint, std::bit_cast<uint32_t>(value));
  }
}

uint64_t ConstantsFingerprint(const PlayerSkinDrawSnapshot &draw) {
  uint64_t fingerprint = kFnvOffsetBasis;
  FingerprintFloats(fingerprint, draw.material.vertex_constants_12_15);
  FingerprintFloats(fingerprint, draw.material.vertex_constant_19);
  FingerprintFloats(fingerprint, draw.material.vertex_constants_46_54);
  FingerprintFloats(fingerprint, draw.material.vertex_constant_255);
  FingerprintFloats(fingerprint, draw.material.pixel_constants_46_68);
  FingerprintFloats(fingerprint, draw.material.pixel_constant_254);
  FingerprintFloats(fingerprint, draw.material.pixel_constant_255);
  FingerprintAppend(fingerprint,
                    draw.material.pixel_control_constants_valid);
  return fingerprint;
}

uint64_t MaterialFingerprint(const PlayerSkinDrawSnapshot &draw) {
  uint64_t fingerprint = kFnvOffsetBasis;
  for (size_t slot = 0; slot < draw.material.texture_fetches.size(); ++slot) {
    for (uint32_t word : draw.material.texture_fetches[slot]) {
      FingerprintAppend(fingerprint, word);
    }
    FingerprintAppend(fingerprint,
                      draw.material.texture_view_swizzles[slot]);
    const auto &texture = draw.material.textures[slot];
    FingerprintAppend(
        fingerprint,
        texture != nullptr ? texture->payload_fingerprint : uint64_t{0});
  }
  FingerprintAppend(fingerprint,
                    draw.material.texture_view_swizzles_valid);
  return fingerprint;
}

template <typename Entry, size_t Size>
Entry *FindFreeLedgerSlot(std::array<Entry, Size> &entries) {
  const auto found = std::ranges::find_if(
      entries, [](const Entry &entry) { return !entry.valid; });
  return found == entries.end() ? nullptr : &*found;
}

uint32_t CountLiveTitleTokensLocked() {
  return static_cast<uint32_t>(std::ranges::count_if(
      g_live_title_tokens,
      [](const LiveTitleToken &token) { return token.valid; }));
}

uint32_t CountLiveTitleFramesLocked() {
  return static_cast<uint32_t>(std::ranges::count_if(
      g_live_title_frames,
      [](const LiveTitleFrame &frame) { return frame.valid; }));
}

uint32_t CountBackendEventsLocked() {
  return static_cast<uint32_t>(std::ranges::count_if(
      g_backend_events,
      [](const BackendProofEvent &event) { return event.valid; }));
}

void UpdateAsyncLedgerCountsLocked() {
  g_telemetry.live_title_tokens = CountLiveTitleTokensLocked();
  g_telemetry.live_title_frames = CountLiveTitleFramesLocked();
  g_telemetry.live_backend_events = CountBackendEventsLocked();
}

LiveTitleFrame *FindLiveTitleFrameLocked(uint64_t frame_sequence) {
  const auto found = std::ranges::find_if(
      g_live_title_frames, [&](const LiveTitleFrame &frame) {
        return frame.valid && frame.frame_sequence == frame_sequence;
      });
  return found == g_live_title_frames.end() ? nullptr : &*found;
}

LiveTitleFrame *OldestReadyTitleFrameLocked() {
  LiveTitleFrame *oldest = nullptr;
  for (LiveTitleFrame &frame : g_live_title_frames) {
    if (!frame.valid || !frame.immutable_finalized ||
        frame.backend_tile_blocks >= kExpectedBackendTileBlocks) {
      continue;
    }
    if (oldest == nullptr ||
        frame.frame_sequence < oldest->frame_sequence) {
      oldest = &frame;
    }
  }
  return oldest;
}

LiveTitleFrame *OldestLiveTitleFrameLocked() {
  LiveTitleFrame *oldest = nullptr;
  for (LiveTitleFrame &frame : g_live_title_frames) {
    if (!frame.valid) {
      continue;
    }
    if (oldest == nullptr ||
        frame.frame_sequence < oldest->frame_sequence) {
      oldest = &frame;
    }
  }
  return oldest;
}

LiveTitleToken *FindFrameTokenLocked(uint64_t frame_sequence,
                                     uint32_t frame_order) {
  const auto found = std::ranges::find_if(
      g_live_title_tokens, [&](const LiveTitleToken &token) {
        return token.valid && token.frame_sequence == frame_sequence &&
               token.frame_order == frame_order;
      });
  return found == g_live_title_tokens.end() ? nullptr : &*found;
}

BackendProofEvent *OldestBackendEventLocked() {
  BackendProofEvent *oldest = nullptr;
  for (BackendProofEvent &event : g_backend_events) {
    if (!event.valid) {
      continue;
    }
    if (oldest == nullptr ||
        event.event_generation < oldest->event_generation) {
      oldest = &event;
    }
  }
  return oldest;
}

void RemoveTitleFrameLocked(LiveTitleFrame &frame) {
  for (LiveTitleToken &token : g_live_title_tokens) {
    if (token.valid && token.frame_sequence == frame.frame_sequence) {
      token = {};
    }
  }
  frame = {};
  UpdateAsyncLedgerCountsLocked();
}

void AnnounceBackendProbeSummaryLocked(uint64_t expired_frame_sequence) {
  if (g_announced_backend_probe_summary) {
    return;
  }
  g_announced_backend_probe_summary = true;
  REXLOG_INFO(
      "Table Tennis CA9 backend probe summary: reason=title_frame_expired "
      "frame={} pre_gate_callbacks={} pre_gate_eligible={} "
      "missing_index={} shader_32bit_index={} memexport={} missing_rhi={} "
      "missing_replacer={} non_host_rt={} hash_callbacks={} "
      "route_matcher_visits={} early={} late={} "
      "contract_rejections={} before_title={} without_live_title_frame={} "
      "queued_events={} observer_only=true guest_suppressed=false",
      expired_frame_sequence, g_telemetry.backend_pre_gate_callbacks,
      g_telemetry.backend_pre_gate_eligible,
      g_telemetry.backend_pre_gate_missing_index,
      g_telemetry.backend_pre_gate_shader_32bit_index,
      g_telemetry.backend_pre_gate_memexport,
      g_telemetry.backend_pre_gate_missing_rhi,
      g_telemetry.backend_pre_gate_missing_replacer,
      g_telemetry.backend_pre_gate_non_host_render_targets,
      g_telemetry.backend_hash_callbacks,
      g_telemetry.backend_route_matcher_visits,
      g_telemetry.backend_early_callbacks,
      g_telemetry.backend_late_callbacks,
      g_telemetry.backend_contract_rejections,
      g_telemetry.backend_pre_title_callbacks,
      g_telemetry.backend_without_live_title_frame,
      g_telemetry.backend_events_queued);
}

void ClearAsyncProofLedgerLocked() {
  g_live_title_tokens = {};
  g_live_title_frames = {};
  g_backend_events = {};
  UpdateAsyncLedgerCountsLocked();
}

std::optional<PlayerReplacementPhase>
PhaseForBackendContext(
    const rex::graphics::NativeGuestDrawContext &context) {
  if (!context.draw_state_contract_valid) {
    return std::nullopt;
  }
  const bool prepass = MatchesPhaseDrawState(
      PlayerReplacementPhase::kDepthAlphaPrepass,
      context.normalized_depth_control, context.normalized_color_mask,
      context.color_control, context.blend_control_0,
      context.primitive_restart_enabled);
  const bool color = MatchesPhaseDrawState(
      PlayerReplacementPhase::kBlendedColor,
      context.normalized_depth_control, context.normalized_color_mask,
      context.color_control, context.blend_control_0,
      context.primitive_restart_enabled);
  if (prepass == color) {
    return std::nullopt;
  }
  return prepass ? PlayerReplacementPhase::kDepthAlphaPrepass
                 : PlayerReplacementPhase::kBlendedColor;
}

bool HasExactPlayerBackendContract(
    const rex::graphics::NativeGuestDrawContext &context) {
  const auto depth_format = context.depth_attachment_format;
  const bool depth_format_supported =
      depth_format ==
          rex::graphics::nrhi::Format::kD24_UNORM_S8_UINT ||
      depth_format ==
          rex::graphics::nrhi::Format::kD32_FLOAT_S8_UINT;
  return context.backend ==
             rex::graphics::NativeGuestOutputBackend::kVulkan &&
         context.render_pass_key_valid && context.indexed &&
         context.guest_index_base_valid &&
         context.draw_state_contract_valid &&
         context.borrowed_attachment_contract_valid &&
         context.color_attachment_count == 1 &&
         context.color_attachment_formats[0] ==
             rex::graphics::nrhi::Format::kR8G8B8A8_UNORM &&
         depth_format_supported &&
         context.stencil_attachment_format == depth_format &&
         context.sample_count == 4 && context.sample_mask == UINT64_MAX &&
         context.vertex_shader_hash == kPlayerVertexShaderHash &&
         context.pixel_shader_hash == kPlayerPixelShaderHash &&
         context.primitive_type == kPlayerPrimitiveType &&
         context.vertex_or_index_count != 0 &&
         context.guest_index_base != 0 &&
         PhaseForBackendContext(context).has_value();
}

bool SameImmutablePhasePayloads(const PlayerSkinDrawSnapshot &left,
                                const PlayerSkinDrawSnapshot &right) {
  if (left.palette == nullptr || right.palette == nullptr ||
      left.palette->fetch.physical_address !=
          right.palette->fetch.physical_address ||
      left.palette->fetch.size != right.palette->fetch.size ||
      left.palette->payload_fingerprint != right.palette->payload_fingerprint ||
      left.material.texture_view_swizzles !=
          right.material.texture_view_swizzles) {
    return false;
  }
  for (size_t slot = 0; slot < left.material.textures.size(); ++slot) {
    const auto &left_texture = left.material.textures[slot];
    const auto &right_texture = right.material.textures[slot];
    if (left_texture == nullptr || right_texture == nullptr ||
        left_texture->fetch_words != right_texture->fetch_words ||
        left_texture->payload_fingerprint !=
            right_texture->payload_fingerprint) {
      return false;
    }
  }
  return true;
}

BackendIdentity BackendIdentityForDraw(const PlayerSkinDrawSnapshot &draw) {
  return {
      .primitive_type = draw.primitive_type,
      .submitted_index_count = draw.submitted_index_count,
      .guest_index_base =
          draw.indices != nullptr ? draw.indices->physical_address : 0,
  };
}

BackendIdentity BackendIdentityForContext(
    const rex::graphics::NativeGuestDrawContext &context) {
  return {
      .primitive_type = context.primitive_type,
      .submitted_index_count = context.vertex_or_index_count,
      .guest_index_base = context.guest_index_base,
  };
}

bool LiveTitleTokenValid(const LiveTitleToken &token) {
  return token.valid && token.title_generation != 0 &&
         token.frame_sequence != 0 &&
         token.ordinal != 0 && token.mesh.valid() &&
         token.vertex_shader_hash == kPlayerVertexShaderHash &&
         token.pixel_shader_hash == kPlayerPixelShaderHash &&
         token.backend.primitive_type == kPlayerPrimitiveType &&
         token.backend.submitted_index_count != 0 &&
         token.backend.guest_index_base != 0 &&
         token.palette_fingerprint != 0 &&
         token.constants_fingerprint != 0 &&
         token.material_fingerprint != 0;
}

bool LiveTitleTokenMatchesCandidate(
    const LiveTitleToken &live,
    const PlayerReplacementCandidate &candidate) {
  return LiveTitleTokenValid(live) && candidate.valid() &&
         live.frame_sequence == candidate.frame_sequence &&
         live.phase == candidate.phase && live.mesh == candidate.mesh &&
         live.vertex_shader_hash == candidate.vertex_shader_hash &&
         live.pixel_shader_hash == candidate.pixel_shader_hash &&
         live.backend.primitive_type == candidate.primitive_type &&
         live.backend.submitted_index_count ==
             candidate.submitted_index_count &&
         live.backend.guest_index_base == candidate.guest_index_base &&
         live.palette_fingerprint == candidate.palette_fingerprint &&
         live.constants_fingerprint == candidate.constants_fingerprint &&
         live.material_fingerprint == candidate.material_fingerprint;
}

bool BackendEventMatchesToken(const BackendProofEvent &event,
                              const LiveTitleToken &token) {
  return event.valid && token.immutable_verified &&
         LiveTitleTokenValid(token) && event.phase == token.phase &&
         event.backend == token.backend &&
         event.observed_title_frame_sequence >= token.frame_sequence;
}

bool EventMatchesLaterFrameStartLocked(const BackendProofEvent &event,
                                       uint64_t frame_sequence) {
  for (const LiveTitleFrame &frame : g_live_title_frames) {
    if (!frame.valid || !frame.immutable_finalized ||
        frame.frame_sequence <= frame_sequence) {
      continue;
    }
    const LiveTitleToken *token =
        FindFrameTokenLocked(frame.frame_sequence, 0);
    if (token != nullptr && BackendEventMatchesToken(event, *token)) {
      return true;
    }
  }
  return false;
}

void ReconcileAsyncBackendLedgerLocked() {
  while (true) {
    LiveTitleFrame *frame = OldestReadyTitleFrameLocked();
    BackendProofEvent *event = OldestBackendEventLocked();
    if (frame == nullptr || event == nullptr) {
      break;
    }

    LiveTitleToken *expected =
        FindFrameTokenLocked(frame->frame_sequence, frame->backend_cursor);
    if (expected == nullptr || !expected->immutable_verified) {
      ++g_telemetry.backend_observer_ambiguous;
      if (!g_announced_async_backend_ambiguity) {
        g_announced_async_backend_ambiguity = true;
        REXLOG_INFO(
            "Table Tennis CA9 async backend proof ambiguous: "
            "frame={} cursor={} token_count={} reason=missing_verified_token "
            "observer_only=true guest_suppressed=false",
            frame->frame_sequence, frame->backend_cursor,
            frame->token_count);
      }
      RemoveTitleFrameLocked(*frame);
      continue;
    }

    if (!BackendEventMatchesToken(*event, *expected)) {
      if (frame->backend_cursor == 0 &&
          EventMatchesLaterFrameStartLocked(*event,
                                            frame->frame_sequence)) {
        g_telemetry.backend_observer_stale += frame->token_count;
        RemoveTitleFrameLocked(*frame);
        continue;
      }
      if (frame->backend_cursor != 0) {
        ++g_telemetry.backend_observer_ambiguous;
        if (!g_announced_async_backend_ambiguity) {
          g_announced_async_backend_ambiguity = true;
          REXLOG_INFO(
              "Table Tennis CA9 async backend proof ambiguous: "
              "frame={} tile_block={} cursor={} event={} "
              "reason=interrupted_order expected={{phase={},count={},"
              "index={:08X}}} actual={{phase={},count={},index={:08X}}} "
              "observer_only=true guest_suppressed=false",
              frame->frame_sequence, frame->backend_tile_blocks,
              frame->backend_cursor, event->event_generation,
              static_cast<uint32_t>(expected->phase),
              expected->backend.submitted_index_count,
              expected->backend.guest_index_base,
              static_cast<uint32_t>(event->phase),
              event->backend.submitted_index_count,
              event->backend.guest_index_base);
        }
        RemoveTitleFrameLocked(*frame);
        continue;
      }
      if (!g_announced_async_backend_mismatch) {
        g_announced_async_backend_mismatch = true;
        REXLOG_INFO(
            "Table Tennis CA9 async backend proof mismatch: "
            "frame={} event={} expected={{phase={},count={},index={:08X}}} "
            "actual={{phase={},count={},index={:08X}}} "
            "observer_only=true guest_suppressed=false",
            frame->frame_sequence, event->event_generation,
            static_cast<uint32_t>(expected->phase),
            expected->backend.submitted_index_count,
            expected->backend.guest_index_base,
            static_cast<uint32_t>(event->phase),
            event->backend.submitted_index_count,
            event->backend.guest_index_base);
      }
      event->valid = false;
      ++g_telemetry.backend_observer_mismatched;
      continue;
    }

    const uint64_t matched_event_generation = event->event_generation;
    const uint32_t matched_render_pass_key = event->render_pass_key;
    const uint32_t matched_surface_pitch = event->surface_pitch;
    event->valid = false;
    ++g_telemetry.backend_observer_matched;
    ++frame->backend_cursor;
    if (frame->backend_cursor != frame->token_count) {
      continue;
    }

    frame->backend_cursor = 0;
    ++frame->backend_tile_blocks;
    ++g_telemetry.backend_tile_blocks_matched;
    if (!g_announced_async_backend_proof) {
      REXLOG_INFO(
          "Table Tennis CA9 async backend tile block: frame={} "
          "block={}/{} ordered_events={} last_event_generation={} "
          "observer_only=true guest_suppressed=false",
          frame->frame_sequence, frame->backend_tile_blocks,
          kExpectedBackendTileBlocks, frame->token_count,
          matched_event_generation);
    }
    if (frame->backend_tile_blocks != kExpectedBackendTileBlocks) {
      continue;
    }

    ++g_telemetry.backend_frames_verified;
    if (!g_announced_async_backend_proof) {
      g_announced_async_backend_proof = true;
      REXLOG_INFO(
          "Table Tennis CA9 async backend proof: frame={} title_tokens={} "
          "tile_blocks={} backend_events={} first_title_generation={} "
          "last_title_generation={} last_event_generation={} "
          "render_pass_key={:08X} surface_pitch={} exact_phase=true "
          "exact_attachments=true observer_only=true "
          "guest_suppressed=false",
          frame->frame_sequence, frame->token_count,
          frame->backend_tile_blocks,
          frame->token_count * frame->backend_tile_blocks,
          frame->first_title_generation, frame->last_title_generation,
          matched_event_generation, matched_render_pass_key,
          matched_surface_pitch);
    }
    RemoveTitleFrameLocked(*frame);
  }
  UpdateAsyncLedgerCountsLocked();
}

void ExpireAsyncLedgerLocked(uint64_t current_frame_sequence) {
  if (current_frame_sequence == 0) {
    return;
  }
  for (LiveTitleFrame &frame : g_live_title_frames) {
    if (!frame.valid ||
        frame.frame_sequence + kMaximumAsyncFrameAge >=
            current_frame_sequence) {
      continue;
    }
    AnnounceBackendProbeSummaryLocked(frame.frame_sequence);
    g_telemetry.backend_observer_stale += frame.token_count;
    RemoveTitleFrameLocked(frame);
  }
  for (BackendProofEvent &event : g_backend_events) {
    if (!event.valid || event.observed_title_frame_sequence == 0 ||
        event.observed_title_frame_sequence + kMaximumAsyncFrameAge >=
            current_frame_sequence) {
      continue;
    }
    event = {};
    ++g_telemetry.backend_observer_stale;
  }
  UpdateAsyncLedgerCountsLocked();
}

void FinalizeImmutableTitleFrameLocked(
    const std::shared_ptr<const PlayerSkinFrameSnapshot> &frame) {
  if (frame == nullptr) {
    ClearAsyncProofLedgerLocked();
    return;
  }

  g_telemetry.latest_candidate_frame_sequence = frame->sequence;
  LiveTitleFrame *proof_frame =
      FindLiveTitleFrameLocked(frame->sequence);
  if (proof_frame == nullptr && g_tokens.empty()) {
    ReconcileAsyncBackendLedgerLocked();
    ExpireAsyncLedgerLocked(frame->sequence);
    return;
  }

  ++g_telemetry.immutable_parity_frames;
  uint64_t matched = 0;
  uint64_t mismatched = 0;
  uint64_t ambiguous = 0;
  std::vector<bool> candidate_seen(g_tokens.size(), false);
  std::vector<bool> candidate_claimed(g_tokens.size(), false);

  if (proof_frame == nullptr) {
    mismatched = g_tokens.size();
  } else {
    for (LiveTitleToken &live : g_live_title_tokens) {
      if (!live.valid || live.frame_sequence != frame->sequence) {
        continue;
      }
      size_t exact_count = 0;
      size_t exact_index = 0;
      for (size_t candidate_index = 0; candidate_index < g_tokens.size();
           ++candidate_index) {
        if (!LiveTitleTokenMatchesCandidate(
                live, *g_tokens[candidate_index].candidate)) {
          continue;
        }
        exact_index = candidate_index;
        candidate_seen[candidate_index] = true;
        ++exact_count;
      }
      if (exact_count == 0) {
        ++mismatched;
      } else if (exact_count != 1 ||
                 candidate_claimed[exact_index]) {
        ++ambiguous;
      } else {
        candidate_claimed[exact_index] = true;
        live.immutable_verified = true;
        ++matched;
      }
    }
    for (size_t candidate_index = 0; candidate_index < g_tokens.size();
         ++candidate_index) {
      if (!candidate_seen[candidate_index]) {
        ++mismatched;
      }
    }
    proof_frame->immutable_candidate_count =
        static_cast<uint32_t>(g_tokens.size());
    proof_frame->immutable_finalized =
        proof_frame->token_count != 0 &&
        matched == proof_frame->token_count &&
        proof_frame->token_count == g_tokens.size() &&
        mismatched == 0 && ambiguous == 0;
  }

  g_telemetry.immutable_parity_matched += matched;
  g_telemetry.immutable_parity_mismatched += mismatched;
  g_telemetry.immutable_parity_ambiguous += ambiguous;

  uint64_t signature = kFnvOffsetBasis;
  FingerprintAppend(
      signature, proof_frame != nullptr ? proof_frame->token_count : 0);
  FingerprintAppend(signature, g_tokens.size());
  FingerprintAppend(signature, matched);
  FingerprintAppend(signature, mismatched);
  FingerprintAppend(signature, ambiguous);
  if (signature != g_announced_parity_signature) {
    g_announced_parity_signature = signature;
    REXLOG_INFO(
        "Table Tennis CA9 async immutable parity: frame={} "
        "title_tokens={} immutable_candidates={} matched={} "
        "mismatched={} ambiguous={} queued_backend_events={} "
        "observer_only=true guest_suppressed=false",
        frame->sequence,
        proof_frame != nullptr ? proof_frame->token_count : 0,
        g_tokens.size(), matched, mismatched, ambiguous,
        CountBackendEventsLocked());
  }

  if (proof_frame != nullptr && !proof_frame->immutable_finalized) {
    RemoveTitleFrameLocked(*proof_frame);
  }
  ReconcileAsyncBackendLedgerLocked();
  ExpireAsyncLedgerLocked(frame->sequence);
}

bool ContextMatchesCandidate(
    const rex::graphics::NativeGuestDrawContext &context,
    const PlayerReplacementCandidate &candidate) {
  const auto depth_format = context.depth_attachment_format;
  const bool depth_format_supported =
      depth_format ==
          rex::graphics::nrhi::Format::kD24_UNORM_S8_UINT ||
      depth_format ==
          rex::graphics::nrhi::Format::kD32_FLOAT_S8_UINT;
  return candidate.valid() &&
         context.backend == rex::graphics::NativeGuestOutputBackend::kVulkan &&
         context.indexed && context.guest_index_base_valid &&
         context.draw_state_contract_valid &&
         MatchesPhaseDrawState(
             candidate.phase, context.normalized_depth_control,
             context.normalized_color_mask, context.color_control,
             context.blend_control_0, context.primitive_restart_enabled) &&
         context.borrowed_attachment_contract_valid &&
         context.color_attachment_count == 1 &&
         context.color_attachment_formats[0] ==
             rex::graphics::nrhi::Format::kR8G8B8A8_UNORM &&
         depth_format_supported &&
         context.stencil_attachment_format == depth_format &&
         context.sample_count == 4 && context.sample_mask == UINT64_MAX &&
         context.vertex_shader_hash == candidate.vertex_shader_hash &&
         context.pixel_shader_hash == candidate.pixel_shader_hash &&
         context.primitive_type == candidate.primitive_type &&
         context.vertex_or_index_count == candidate.submitted_index_count &&
         context.guest_index_base == candidate.guest_index_base;
}

uint64_t CandidateContractSignature(const std::vector<CandidateToken> &tokens) {
  uint64_t signature = 1469598103934665603ull;
  auto append = [&](uint64_t value) {
    signature = (signature ^ value) * 1099511628211ull;
  };
  append(tokens.size());
  for (const CandidateToken &token : tokens) {
    const PlayerReplacementCandidate &candidate = *token.candidate;
    append(candidate.mesh.player);
    append(candidate.mesh.vertex_physical_address);
    append(candidate.mesh.vertex_fingerprint);
    append(candidate.mesh.index_physical_address);
    append(candidate.mesh.index_fingerprint);
    append(candidate.submitted_index_count);
    append(static_cast<uint8_t>(candidate.phase));
  }
  return signature;
}

uint32_t CountLiveUnconsumedCandidates() {
  return static_cast<uint32_t>(std::count_if(
      g_tokens.begin(), g_tokens.end(),
      [](const CandidateToken &token) { return !token.consumed; }));
}

} // namespace

bool PlayerReplacementCandidate::valid() const {
  if (generation == 0 || phase_pair_generation == 0 || frame_sequence == 0 ||
      frame == nullptr || !frame->valid() ||
      draw_index >= frame->draws.size() ||
      counterpart_draw_index >= frame->draws.size() ||
      draw_index == counterpart_draw_index || !mesh.valid() ||
      vertex_shader_hash != kPlayerVertexShaderHash ||
      pixel_shader_hash != kPlayerPixelShaderHash ||
      primitive_type != kPlayerPrimitiveType || submitted_index_count == 0 ||
      guest_index_base == 0 || palette_fingerprint == 0 ||
      constants_fingerprint == 0 || material_fingerprint == 0) {
    return false;
  }
  const PlayerSkinDrawSnapshot &draw = frame->draws[draw_index];
  const PlayerSkinDrawSnapshot &counterpart =
      frame->draws[counterpart_draw_index];
  const std::optional<PhaseInfo> draw_phase =
      PhaseForDescriptor(draw.pass_descriptor);
  const std::optional<PhaseInfo> counterpart_phase =
      PhaseForDescriptor(counterpart.pass_descriptor);
  if (!draw.valid || !counterpart.valid || !draw_phase.has_value() ||
      !counterpart_phase.has_value() || draw_phase->phase != phase ||
      draw_phase->counterpart_descriptor != counterpart.pass_descriptor ||
      PlayerSkinMeshIdentityForDraw(draw) != mesh ||
      PlayerSkinMeshIdentityForDraw(counterpart) != mesh ||
      draw.primitive_type != primitive_type ||
      draw.submitted_index_count != submitted_index_count ||
      draw.indices == nullptr ||
      draw.indices->physical_address != guest_index_base ||
      draw.palette == nullptr ||
      draw.palette->payload_fingerprint != palette_fingerprint ||
      ConstantsFingerprint(draw) != constants_fingerprint ||
      MaterialFingerprint(draw) != material_fingerprint ||
      !SameImmutablePhasePayloads(draw, counterpart)) {
    return false;
  }
  for (size_t slot = 0; slot < texture_fingerprints.size(); ++slot) {
    if (draw.material.textures[slot] == nullptr ||
        draw.material.textures[slot]->payload_fingerprint !=
            texture_fingerprints[slot]) {
      return false;
    }
  }
  return true;
}

void PublishPlayerReplacementLiveTitleDraw(
    uint64_t frame_sequence, const PlayerSkinDrawSnapshot &draw) {
  const std::optional<PhaseInfo> phase =
      PhaseForDescriptor(draw.pass_descriptor);
  const PlayerSkinMeshIdentity mesh = PlayerSkinMeshIdentityForDraw(draw);
  if (frame_sequence == 0 || !draw.valid || !phase.has_value() ||
      !mesh.valid() || draw.indices == nullptr || !draw.indices->valid() ||
      draw.palette == nullptr || !draw.palette->valid() ||
      !draw.material.valid) {
    return;
  }

  LiveTitleToken token{
      .frame_sequence = frame_sequence,
      .ordinal = draw.ordinal,
      .phase = phase->phase,
      .mesh = mesh,
      .vertex_shader_hash = kPlayerVertexShaderHash,
      .pixel_shader_hash = kPlayerPixelShaderHash,
      .backend = BackendIdentityForDraw(draw),
      .palette_fingerprint = draw.palette->payload_fingerprint,
      .constants_fingerprint = ConstantsFingerprint(draw),
      .material_fingerprint = MaterialFingerprint(draw),
      .valid = true,
  };

  std::lock_guard lock(g_candidate_mutex);
  if (frame_sequence < g_telemetry.latest_title_frame_sequence) {
    ++g_telemetry.immutable_parity_stale;
    return;
  }

  LiveTitleFrame *proof_frame =
      FindLiveTitleFrameLocked(frame_sequence);
  if (proof_frame == nullptr) {
    proof_frame = FindFreeLedgerSlot(g_live_title_frames);
    if (proof_frame == nullptr) {
      proof_frame = OldestLiveTitleFrameLocked();
      if (proof_frame == nullptr) {
        ++g_telemetry.live_title_tokens_overwritten;
        return;
      }
      g_telemetry.live_title_tokens_overwritten +=
          proof_frame->token_count;
      RemoveTitleFrameLocked(*proof_frame);
      proof_frame = FindFreeLedgerSlot(g_live_title_frames);
    }
    *proof_frame = {
        .frame_sequence = frame_sequence,
        .valid = true,
    };
  }

  LiveTitleToken *slot =
      FindFreeLedgerSlot(g_live_title_tokens);
  if (slot == nullptr) {
    LiveTitleFrame *oldest = OldestLiveTitleFrameLocked();
    if (oldest != nullptr) {
      g_telemetry.live_title_tokens_overwritten +=
          oldest->token_count;
      const bool evicted_current =
          oldest->frame_sequence == frame_sequence;
      RemoveTitleFrameLocked(*oldest);
      if (evicted_current) {
        ++g_telemetry.immutable_parity_ambiguous;
        return;
      }
      proof_frame = FindLiveTitleFrameLocked(frame_sequence);
      slot = FindFreeLedgerSlot(g_live_title_tokens);
    }
  }
  if (slot == nullptr || proof_frame == nullptr) {
    ++g_telemetry.live_title_tokens_overwritten;
    return;
  }

  token.title_generation = ++g_title_generation;
  token.frame_order = proof_frame->token_count;
  if (!LiveTitleTokenValid(token)) {
    return;
  }
  *slot = std::move(token);
  if (proof_frame->token_count == 0) {
    proof_frame->first_title_generation = g_title_generation;
  }
  proof_frame->last_title_generation = g_title_generation;
  ++proof_frame->token_count;
  ++g_telemetry.live_title_tokens_published;
  g_telemetry.latest_title_generation = g_title_generation;
  g_telemetry.latest_title_frame_sequence = frame_sequence;
  UpdateAsyncLedgerCountsLocked();
}

void ObservePlayerReplacementBackendDraw(
    const rex::graphics::NativeGuestDrawContext &context) {
  if (context.vertex_shader_hash != kPlayerVertexShaderHash ||
      context.pixel_shader_hash != kPlayerPixelShaderHash) {
    return;
  }

  std::lock_guard lock(g_candidate_mutex);
  ++g_telemetry.backend_hash_callbacks;
  if (context.render_pass_key_valid) {
    ++g_telemetry.backend_late_callbacks;
  } else {
    ++g_telemetry.backend_early_callbacks;
  }
  if (!g_announced_backend_hash_callback) {
    g_announced_backend_hash_callback = true;
    REXLOG_INFO(
        "Table Tennis CA9 backend callback observed: stage={} "
        "primitive={} count={} index={:08X} index_valid={} "
        "draw_state_valid={} observer_only=true guest_suppressed=false",
        context.render_pass_key_valid ? "late" : "early",
        context.primitive_type, context.vertex_or_index_count,
        context.guest_index_base, context.guest_index_base_valid,
        context.draw_state_contract_valid);
  }
  if (!context.render_pass_key_valid) {
    return;
  }

  ++g_telemetry.backend_observer_probes;
  if (g_telemetry.latest_title_frame_sequence == 0) {
    ++g_telemetry.backend_pre_title_callbacks;
    if (!g_announced_backend_pre_title_callback) {
      g_announced_backend_pre_title_callback = true;
      REXLOG_INFO(
          "Table Tennis CA9 backend callback before title publication: "
          "count={} index={:08X} event_dropped=true observer_only=true "
          "guest_suppressed=false",
          context.vertex_or_index_count, context.guest_index_base);
    }
    return;
  }
  if (CountLiveTitleFramesLocked() == 0) {
    ++g_telemetry.backend_without_live_title_frame;
    return;
  }

  const std::optional<PlayerReplacementPhase> phase =
      PhaseForBackendContext(context);
  if (!HasExactPlayerBackendContract(context) ||
      !phase.has_value()) {
    ++g_telemetry.backend_contract_rejections;
    ++g_telemetry.backend_observer_mismatched;
    if (!g_announced_backend_contract_rejection) {
      g_announced_backend_contract_rejection = true;
      REXLOG_INFO(
          "Table Tennis CA9 backend contract rejection: backend={} "
          "indexed={} index_valid={} primitive={} count={} index={:08X} "
          "draw_state_valid={} phase_valid={} attachment_valid={} "
          "colors={} color0={} depth={} stencil={} samples={} "
          "sample_mask={:016X} render_pass_key={:08X} "
          "observer_only=true guest_suppressed=false",
          static_cast<uint32_t>(context.backend), context.indexed,
          context.guest_index_base_valid, context.primitive_type,
          context.vertex_or_index_count, context.guest_index_base,
          context.draw_state_contract_valid, phase.has_value(),
          context.borrowed_attachment_contract_valid,
          context.color_attachment_count,
          static_cast<uint32_t>(context.color_attachment_formats[0]),
          static_cast<uint32_t>(context.depth_attachment_format),
          static_cast<uint32_t>(context.stencil_attachment_format),
          context.sample_count, context.sample_mask,
          context.render_pass_key);
    }
    return;
  }

  BackendProofEvent *slot = FindFreeLedgerSlot(g_backend_events);
  if (slot == nullptr) {
    slot = OldestBackendEventLocked();
    if (slot != nullptr) {
      *slot = {};
      ++g_telemetry.backend_events_dropped;
    }
    slot = FindFreeLedgerSlot(g_backend_events);
  }
  if (slot == nullptr) {
    ++g_telemetry.backend_events_dropped;
    return;
  }

  *slot = {
      .event_generation = ++g_backend_event_generation,
      .observed_title_frame_sequence =
          g_telemetry.latest_title_frame_sequence,
      .phase = *phase,
      .backend = BackendIdentityForContext(context),
      .render_pass_key = context.render_pass_key,
      .surface_pitch = context.surface_pitch,
      .valid = true,
  };
  ++g_telemetry.backend_events_queued;
  ReconcileAsyncBackendLedgerLocked();
  UpdateAsyncLedgerCountsLocked();
}

bool PlayerReplacementGateDiagnosticEnabled() {
  return REXCVAR_GET(
      tabletennis_native_player_replacement_gate_diagnostic);
}

void ObservePlayerReplacementDrawEligibility(
    const rex::graphics::NativeGuestDrawEligibilityContext &context,
    void *) {
  if (context.vertex_shader_hash != kPlayerVertexShaderHash ||
      context.pixel_shader_hash != kPlayerPixelShaderHash) {
    return;
  }

  std::lock_guard lock(g_candidate_mutex);
  ++g_telemetry.backend_pre_gate_callbacks;
  g_telemetry.backend_pre_gate_eligible += context.eligible;
  g_telemetry.backend_pre_gate_missing_index +=
      !context.processed_index_buffer_present;
  g_telemetry.backend_pre_gate_shader_32bit_index +=
      context.shader_32bit_index_dma;
  g_telemetry.backend_pre_gate_memexport +=
      context.memexport_writes_possible;
  g_telemetry.backend_pre_gate_missing_rhi +=
      !context.native_rhi_device_available;
  g_telemetry.backend_pre_gate_missing_replacer +=
      !context.draw_replacer_available;
  g_telemetry.backend_pre_gate_non_host_render_targets +=
      !context.host_render_targets;

  if (!g_announced_backend_pre_gate_callback) {
    g_announced_backend_pre_gate_callback = true;
    REXLOG_INFO(
        "Table Tennis CA9 Vulkan pre-gate: primitive={} count={} "
        "index={:08X} index_buffer_type={} index_present={} "
        "shader_32bit_index_dma={} memexport={} native_rhi={} replacer={} "
        "host_render_targets={} eligible={} observer_only=true "
        "guest_suppressed=false",
        context.primitive_type, context.vertex_or_index_count,
        context.guest_index_base, context.processed_index_buffer_type,
        context.processed_index_buffer_present,
        context.shader_32bit_index_dma,
        context.memexport_writes_possible,
        context.native_rhi_device_available,
        context.draw_replacer_available, context.host_render_targets,
        context.eligible);
  }
}

void PublishPlayerReplacementCandidates(
    const std::shared_ptr<const PlayerSkinFrameSnapshot> &frame) {
  std::lock_guard lock(g_candidate_mutex);
  if (g_pending_candidate != nullptr) {
    ++g_telemetry.abandoned_matches;
    g_pending_candidate.reset();
  }
  g_tokens.clear();

  if (frame == nullptr) {
    g_telemetry.live_candidates = 0;
    g_telemetry.live_unconsumed_candidates = 0;
    FinalizeImmutableTitleFrameLocked(frame);
    return;
  }
  if (!frame->valid()) {
    ++g_telemetry.rejected_frames;
    g_telemetry.live_candidates = 0;
    g_telemetry.live_unconsumed_candidates = 0;
    FinalizeImmutableTitleFrameLocked(frame);
    return;
  }
  ++g_telemetry.published_frames;

  std::vector<PhaseGroup> groups;
  groups.reserve(frame->draws.size());
  for (size_t draw_index = 0; draw_index < frame->draws.size(); ++draw_index) {
    const PlayerSkinMeshIdentity mesh =
        PlayerSkinMeshIdentityForDraw(frame->draws[draw_index]);
    if (!mesh.valid()) {
      ++g_telemetry.ambiguous_phase_groups;
      continue;
    }
    const auto found = std::ranges::find(groups, mesh, &PhaseGroup::mesh);
    if (found == groups.end()) {
      groups.push_back({
          .mesh = mesh,
          .draw_indices = {draw_index},
      });
    } else {
      found->draw_indices.push_back(draw_index);
    }
  }

  for (PhaseGroup &group : groups) {
    if (group.draw_indices.size() != 2) {
      if (group.draw_indices.size() == 1) {
        ++g_telemetry.partial_phase_groups;
      } else {
        ++g_telemetry.ambiguous_phase_groups;
      }
      continue;
    }
    const size_t first_index = group.draw_indices[0];
    const size_t second_index = group.draw_indices[1];
    const PlayerSkinDrawSnapshot &first = frame->draws[first_index];
    const PlayerSkinDrawSnapshot &second = frame->draws[second_index];
    const std::optional<PhaseInfo> first_phase =
        PhaseForDescriptor(first.pass_descriptor);
    const std::optional<PhaseInfo> second_phase =
        PhaseForDescriptor(second.pass_descriptor);
    if (!first_phase.has_value() || !second_phase.has_value() ||
        first_phase->phase != PlayerReplacementPhase::kDepthAlphaPrepass ||
        second_phase->phase != PlayerReplacementPhase::kBlendedColor ||
        first_phase->counterpart_descriptor != second.pass_descriptor ||
        second_phase->counterpart_descriptor != first.pass_descriptor) {
      ++g_telemetry.phase_pair_mismatches;
      continue;
    }
    if (!SameImmutablePhasePayloads(first, second)) {
      ++g_telemetry.payload_pair_mismatches;
      continue;
    }
    group.eligible = true;
  }

  // The backend context exposes the physical index base but not vf95. If two
  // distinct mesh groups collapse to the same backend identity, neither is
  // eligible: immutable fingerprints prove they differ but cannot tell the
  // matcher which one is currently executing.
  for (size_t left = 0; left < groups.size(); ++left) {
    if (!groups[left].eligible) {
      continue;
    }
    const BackendIdentity left_identity =
        BackendIdentityForDraw(frame->draws[groups[left].draw_indices.front()]);
    for (size_t right = left + 1; right < groups.size(); ++right) {
      if (!groups[right].eligible) {
        continue;
      }
      const BackendIdentity right_identity = BackendIdentityForDraw(
          frame->draws[groups[right].draw_indices.front()]);
      if (left_identity == right_identity &&
          groups[left].mesh != groups[right].mesh) {
        groups[left].eligible = false;
        groups[right].eligible = false;
        ++g_telemetry.backend_identity_collisions;
      }
    }
  }

  std::vector<uint64_t> pair_generations(groups.size());
  std::vector<uint32_t> counterpart_indices(frame->draws.size(), UINT32_MAX);
  std::vector<size_t> draw_groups(frame->draws.size(),
                                  std::numeric_limits<size_t>::max());
  for (size_t group_index = 0; group_index < groups.size(); ++group_index) {
    const PhaseGroup &group = groups[group_index];
    if (!group.eligible) {
      continue;
    }
    pair_generations[group_index] = ++g_phase_pair_generation;
    const uint32_t first = static_cast<uint32_t>(group.draw_indices[0]);
    const uint32_t second = static_cast<uint32_t>(group.draw_indices[1]);
    counterpart_indices[first] = second;
    counterpart_indices[second] = first;
    draw_groups[first] = group_index;
    draw_groups[second] = group_index;
    ++g_telemetry.published_phase_pairs;
  }

  g_tokens.reserve(frame->draws.size());
  for (size_t draw_index = 0; draw_index < frame->draws.size(); ++draw_index) {
    const size_t group_index = draw_groups[draw_index];
    if (group_index >= groups.size()) {
      continue;
    }
    const PlayerSkinDrawSnapshot &draw = frame->draws[draw_index];
    const std::optional<PhaseInfo> phase =
        PhaseForDescriptor(draw.pass_descriptor);
    if (!phase.has_value()) {
      continue;
    }
    std::array<uint64_t, 3> texture_fingerprints{};
    for (size_t slot = 0; slot < texture_fingerprints.size(); ++slot) {
      texture_fingerprints[slot] =
          draw.material.textures[slot]->payload_fingerprint;
    }
    auto candidate =
        std::make_shared<PlayerReplacementCandidate>(PlayerReplacementCandidate{
            .generation = ++g_candidate_generation,
            .phase_pair_generation = pair_generations[group_index],
            .frame_sequence = frame->sequence,
            .draw_index = static_cast<uint32_t>(draw_index),
            .counterpart_draw_index = counterpart_indices[draw_index],
            .phase = phase->phase,
            .mesh = groups[group_index].mesh,
            .vertex_shader_hash = kPlayerVertexShaderHash,
            .pixel_shader_hash = kPlayerPixelShaderHash,
            .primitive_type = draw.primitive_type,
            .submitted_index_count = draw.submitted_index_count,
            .guest_index_base = draw.indices->physical_address,
            .palette_fingerprint = draw.palette->payload_fingerprint,
            .constants_fingerprint = ConstantsFingerprint(draw),
            .material_fingerprint = MaterialFingerprint(draw),
            .texture_fingerprints = texture_fingerprints,
            .frame = frame,
        });
    if (!candidate->valid()) {
      ++g_telemetry.ambiguous_phase_groups;
      continue;
    }
    g_tokens.push_back({
        .candidate = std::move(candidate),
    });
    ++g_telemetry.published_candidates;
  }

  g_telemetry.live_candidates = static_cast<uint32_t>(g_tokens.size());
  g_telemetry.live_unconsumed_candidates = CountLiveUnconsumedCandidates();
  const uint64_t signature = CandidateContractSignature(g_tokens);
  if (!g_tokens.empty() && signature != g_announced_contract_signature) {
    g_announced_contract_signature = signature;
    REXLOG_INFO(
        "Table Tennis player replacement candidates: published {} one-shot "
        "CA9 tokens across {} explicit prepass/color pairs "
        "(observer_only=true claiming_route_registered=false "
        "exact_borrowed_attachment_gate=true)",
        g_tokens.size(), g_tokens.size() / 2);
  }
  FinalizeImmutableTitleFrameLocked(frame);
}

bool MatchPlayerReplacementCandidate(
    const rex::graphics::NativeGuestDrawContext &context, void *) {
  if (context.vertex_shader_hash != kPlayerVertexShaderHash ||
      context.pixel_shader_hash != kPlayerPixelShaderHash) {
    return false;
  }
  std::lock_guard lock(g_candidate_mutex);
  ++g_telemetry.match_probes;
  if (g_pending_candidate != nullptr) {
    ++g_telemetry.abandoned_matches;
    g_pending_candidate.reset();
  }

  bool matching_consumed_token = false;
  for (CandidateToken &token : g_tokens) {
    if (!ContextMatchesCandidate(context, *token.candidate)) {
      continue;
    }
    if (token.consumed) {
      matching_consumed_token = true;
      continue;
    }
    token.consumed = true;
    g_pending_candidate = token.candidate;
    ++g_telemetry.matched;
    g_telemetry.live_unconsumed_candidates = CountLiveUnconsumedCandidates();
    return true;
  }
  if (matching_consumed_token) {
    ++g_telemetry.stale_rejections;
  } else {
    ++g_telemetry.unavailable_rejections;
  }
  return false;
}

bool PlayerReplacementPrewarmEnabled() {
  return REXCVAR_GET(
      tabletennis_native_player_replacement_prewarm);
}

bool MatchPlayerReplacementPrewarm(
    const rex::graphics::NativeGuestDrawContext &context, void *user_data) {
  if (context.vertex_shader_hash == kPlayerVertexShaderHash &&
      context.pixel_shader_hash == kPlayerPixelShaderHash) {
    std::lock_guard lock(g_candidate_mutex);
    ++g_telemetry.backend_route_matcher_visits;
  }
  return PlayerReplacementPrewarmEnabled() &&
         MatchPlayerReplacementCandidate(context, user_data);
}

bool RenderPlayerReplacementPrewarm(
    const rex::graphics::NativeGuestDrawContext &context, void *) {
  const std::shared_ptr<const PlayerReplacementCandidate> candidate =
      ConsumeMatchedPlayerReplacementCandidate(context);
  if (candidate == nullptr) {
    return false;
  }
  const PlayerReplacementPreflightResult preflight_result =
      PreflightPlayerReplacementCandidate(context, *candidate);
  const bool succeeded =
      PlayerReplacementPreflightSucceeded(preflight_result);
  {
    std::lock_guard lock(g_candidate_mutex);
    if (g_prewarm_frame_progress.frame_sequence !=
        candidate->frame_sequence) {
      g_prewarm_frame_progress = {
          .frame_sequence = candidate->frame_sequence,
          .expected = static_cast<uint32_t>(std::count_if(
              g_tokens.begin(), g_tokens.end(),
              [&](const CandidateToken &token) {
                return token.candidate != nullptr &&
                       token.candidate->frame_sequence ==
                           candidate->frame_sequence;
              })),
      };
    }
    ++g_prewarm_frame_progress.attempted;
    ++g_telemetry.prewarm_attempts;
    if (succeeded) {
      ++g_telemetry.prewarm_succeeded;
      if (preflight_result ==
          PlayerReplacementPreflightResult::kSucceeded) {
        ++g_prewarm_frame_progress.exact_succeeded;
      } else {
        ++g_prewarm_frame_progress.compatible_succeeded;
      }
      if (!g_announced_prewarm_success) {
        g_announced_prewarm_success = true;
        REXLOG_INFO(
            "Table Tennis CA9 borrowed prewarm succeeded: phase={} "
            "count={} index={:08X} depth_format={} samples={} "
            "mode={} candidate_frame={} observer_only=true "
            "guest_suppressed=false",
            static_cast<uint32_t>(candidate->phase),
            context.vertex_or_index_count, context.guest_index_base,
            static_cast<uint32_t>(context.depth_attachment_format),
            context.sample_count,
            PlayerReplacementPreflightResultName(preflight_result),
            candidate->frame_sequence);
      }
    } else {
      ++g_prewarm_frame_progress.failed;
      ++g_telemetry.prewarm_failed;
      if (!g_announced_prewarm_failure) {
        g_announced_prewarm_failure = true;
        REXLOG_INFO(
            "Table Tennis CA9 borrowed prewarm failed: phase={} count={} "
            "index={:08X} depth_format={} samples={} reason={} "
            "candidate_frame={} observer_only=true guest_suppressed=false",
            static_cast<uint32_t>(candidate->phase),
            context.vertex_or_index_count, context.guest_index_base,
            static_cast<uint32_t>(context.depth_attachment_format),
            context.sample_count,
            PlayerReplacementPreflightResultName(preflight_result),
            candidate->frame_sequence);
      }
    }
    if (g_prewarm_frame_progress.expected != 0 &&
        g_prewarm_frame_progress.attempted ==
            g_prewarm_frame_progress.expected) {
      REXLOG_INFO(
          "Table Tennis CA9 prewarm frame complete: frame={} "
          "candidates={} exact_succeeded={} compatible_succeeded={} "
          "failed={} complete_block_ready={} observer_only=true "
          "guest_suppressed=false",
          g_prewarm_frame_progress.frame_sequence,
          g_prewarm_frame_progress.expected,
          g_prewarm_frame_progress.exact_succeeded,
          g_prewarm_frame_progress.compatible_succeeded,
          g_prewarm_frame_progress.failed,
          g_prewarm_frame_progress.exact_succeeded ==
                  g_prewarm_frame_progress.expected &&
              g_prewarm_frame_progress.failed == 0);
    }
  }
  // Observer-only by contract: even a successful preflight never claims or
  // records this guest draw.
  return false;
}

std::shared_ptr<const PlayerReplacementCandidate>
ConsumeMatchedPlayerReplacementCandidate(
    const rex::graphics::NativeGuestDrawContext &context) {
  std::lock_guard lock(g_candidate_mutex);
  std::shared_ptr<const PlayerReplacementCandidate> candidate =
      std::move(g_pending_candidate);
  if (candidate == nullptr || !ContextMatchesCandidate(context, *candidate)) {
    ++g_telemetry.consume_identity_rejections;
    return nullptr;
  }
  ++g_telemetry.consumed;
  return candidate;
}

PlayerReplacementCandidateTelemetry
LatestPlayerReplacementCandidateTelemetry() {
  std::lock_guard lock(g_candidate_mutex);
  return g_telemetry;
}

void ResetPlayerReplacementCandidates() {
  std::lock_guard lock(g_candidate_mutex);
  g_tokens.clear();
  g_pending_candidate.reset();
  ClearAsyncProofLedgerLocked();
  g_telemetry = {};
  g_candidate_generation = 0;
  g_phase_pair_generation = 0;
  g_title_generation = 0;
  g_backend_event_generation = 0;
  g_announced_contract_signature = 0;
  g_announced_parity_signature = 0;
  g_announced_async_backend_proof = false;
  g_announced_async_backend_mismatch = false;
  g_announced_async_backend_ambiguity = false;
  g_announced_backend_hash_callback = false;
  g_announced_backend_pre_gate_callback = false;
  g_announced_backend_contract_rejection = false;
  g_announced_backend_pre_title_callback = false;
  g_announced_backend_probe_summary = false;
  g_announced_prewarm_success = false;
  g_announced_prewarm_failure = false;
  g_prewarm_frame_progress = {};
}

} // namespace tabletennis::native
