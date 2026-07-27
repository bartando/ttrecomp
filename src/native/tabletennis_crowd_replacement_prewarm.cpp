#include "native/tabletennis_crowd_replacement_prewarm.h"

#include "native/tabletennis_crowd_observer.h"
#include "native/tabletennis_crowd_observer_renderer.h"

#include <cstdint>
#include <memory>
#include <mutex>

#include <rex/cvar.h>
#include <rex/graphics/native_guest_renderer.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(
    tabletennis_native_crowd_replacement_prewarm, false, "Table Tennis",
    "Observer-only: prewarm the exact borrowed RGBA8/D32S8 4x C6 crowd "
    "pipeline and captured resources, then always execute the guest draw.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_UINT32(
    tabletennis_native_crowd_replacement_serve_draws, 0, "Table Tennis",
    "Serve only the first N exact C6 crowd draws of a later ordered tile "
    "block, after a complete prior block preflighted successfully. 0 keeps "
    "every guest draw authoritative.")
    .range(0, 2048)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace tabletennis::native {
namespace {

constexpr uint64_t kCrowdVertexShaderHash = 0xBD4B1DF972B828B7ull;
constexpr uint64_t kCrowdPixelShaderHash = 0xC6CEFDA3753CF2BAull;
constexpr uint32_t kTriangleStripPrimitive = 0x06;
constexpr uint32_t kCrowdDepthControl = 0x00700736;
constexpr uint32_t kCrowdColorMask = 0x00000007;
constexpr uint32_t kCrowdColorControl = 0x87000005;
constexpr uint32_t kCrowdBlendControl = 0x00010001;
constexpr uint32_t kCrowdSurfacePitch = 1280;
constexpr uint32_t kCrowdRenderPassKey = 0x0000000E;

std::mutex g_prewarm_mutex;
std::shared_ptr<const CrowdReplacementCandidate> g_pending_candidate;
uint64_t g_attempt_count = 0;
uint64_t g_success_count = 0;
uint64_t g_failure_count = 0;
bool g_announced_success = false;
bool g_announced_failure = false;
bool g_announced_complete_block = false;
bool g_announced_incomplete_block = false;
bool g_complete_block_ready = false;
bool g_announced_serving = false;
uint64_t g_served_draw_count = 0;

struct PrewarmBlockProgress {
  uint64_t title_generation = 0;
  uint64_t backend_block_sequence = 0;
  uint32_t tile_ordinal = 0;
  uint32_t expected = 0;
  uint32_t attempted = 0;
  uint32_t succeeded = 0;
  uint32_t failed = 0;
  uint32_t next_draw_index = 0;
  bool ordered = true;
};

PrewarmBlockProgress g_block_progress;

bool CompleteBlock(const PrewarmBlockProgress& progress) {
  return progress.expected != 0 &&
         progress.attempted == progress.expected &&
         progress.succeeded == progress.expected &&
         progress.failed == 0 && progress.ordered &&
         progress.next_draw_index == progress.expected;
}

void LogBlockProgressIfUseful(bool force) {
  if (g_block_progress.attempted == 0) {
    return;
  }
  const bool complete = CompleteBlock(g_block_progress);
  if ((!force && !complete) ||
      (complete && g_announced_complete_block) ||
      (!complete && g_announced_incomplete_block)) {
    return;
  }
  if (complete) {
    g_announced_complete_block = true;
    g_complete_block_ready = true;
  } else {
    g_announced_incomplete_block = true;
  }
  REXLOG_INFO(
      "Table Tennis crowd prewarm block: title_generation={} tile={} "
      "backend_block={} expected={} attempted={} succeeded={} failed={} "
      "ordered={} "
      "complete_block_ready={} observer_only=true",
      g_block_progress.title_generation, g_block_progress.tile_ordinal,
      g_block_progress.backend_block_sequence,
      g_block_progress.expected, g_block_progress.attempted,
      g_block_progress.succeeded, g_block_progress.failed,
      g_block_progress.ordered, complete);
}

void RecordBlockAttempt(const CrowdReplacementCandidate& candidate,
                        bool succeeded) {
  if (g_block_progress.title_generation != candidate.title_generation ||
      g_block_progress.backend_block_sequence !=
          candidate.backend_block_sequence ||
      g_block_progress.tile_ordinal != candidate.tile_ordinal) {
    LogBlockProgressIfUseful(true);
    g_block_progress = {
        .title_generation = candidate.title_generation,
        .backend_block_sequence =
            candidate.backend_block_sequence,
        .tile_ordinal = candidate.tile_ordinal,
        .expected =
            static_cast<uint32_t>(candidate.frame->draws.size()),
    };
  }
  g_block_progress.ordered &=
      candidate.draw_index == g_block_progress.next_draw_index;
  g_block_progress.next_draw_index = candidate.draw_index + 1;
  ++g_block_progress.attempted;
  if (succeeded) {
    ++g_block_progress.succeeded;
  } else {
    ++g_block_progress.failed;
  }
  LogBlockProgressIfUseful(false);
}

bool ContextMatchesCandidate(
    const rex::graphics::NativeGuestDrawContext& context,
    const CrowdReplacementCandidate& candidate) {
  return candidate.valid() &&
         MatchesCrowdReplacementPrewarmContract(context) &&
         context.render_pass_key == candidate.render_pass_key &&
         context.primitive_type == candidate.primitive_type &&
         context.vertex_or_index_count == candidate.submitted_index_count &&
         context.guest_index_base == candidate.guest_index_base;
}

}  // namespace

bool CrowdReplacementPrewarmEnabled() {
  return REXCVAR_GET(tabletennis_native_crowd_replacement_prewarm) ||
         REXCVAR_GET(
             tabletennis_native_crowd_replacement_serve_draws) != 0;
}

bool MatchesCrowdReplacementPrewarmContract(
    const rex::graphics::NativeGuestDrawContext& context) {
  namespace nrhi = rex::graphics::nrhi;
  const bool depth_format_supported =
      context.depth_attachment_format ==
          nrhi::Format::kD24_UNORM_S8_UINT ||
      context.depth_attachment_format ==
          nrhi::Format::kD32_FLOAT_S8_UINT;
  return context.backend ==
             rex::graphics::NativeGuestOutputBackend::kVulkan &&
         context.render_pass_key_valid &&
         context.render_pass_key == kCrowdRenderPassKey &&
         context.surface_pitch == kCrowdSurfacePitch &&
         context.indexed && context.guest_index_base_valid &&
         context.draw_state_contract_valid &&
         context.normalized_depth_control == kCrowdDepthControl &&
         context.normalized_color_mask == kCrowdColorMask &&
         context.color_control == kCrowdColorControl &&
         context.blend_control_0 == kCrowdBlendControl &&
         !context.primitive_restart_enabled &&
         context.borrowed_attachment_contract_valid &&
         context.color_attachment_count == 1 &&
         context.color_attachment_formats[0] ==
             nrhi::Format::kR8G8B8A8_UNORM &&
         depth_format_supported &&
         context.stencil_attachment_format ==
             context.depth_attachment_format &&
         context.sample_count == 4 &&
         context.sample_mask == UINT64_MAX &&
         context.vertex_shader_hash == kCrowdVertexShaderHash &&
         context.pixel_shader_hash == kCrowdPixelShaderHash &&
         context.primitive_type == kTriangleStripPrimitive &&
         context.vertex_or_index_count != 0 &&
         context.guest_index_base != 0;
}

bool MatchCrowdReplacementPrewarm(
    const rex::graphics::NativeGuestDrawContext& context, void*) {
  if (!CrowdReplacementPrewarmEnabled() ||
      !MatchesCrowdReplacementPrewarmContract(context)) {
    return false;
  }

  std::shared_ptr<const CrowdReplacementCandidate> candidate =
      ConsumeCrowdReplacementCandidate(context);
  if (candidate == nullptr ||
      !ContextMatchesCandidate(context, *candidate)) {
    return false;
  }
  std::lock_guard lock(g_prewarm_mutex);
  g_pending_candidate = std::move(candidate);
  return true;
}

bool RenderCrowdReplacementPrewarm(
    const rex::graphics::NativeGuestDrawContext& context, void*) {
  std::shared_ptr<const CrowdReplacementCandidate> candidate;
  {
    std::lock_guard lock(g_prewarm_mutex);
    candidate = std::move(g_pending_candidate);
  }
  if (candidate == nullptr ||
      !ContextMatchesCandidate(context, *candidate)) {
    return false;
  }

  const CrowdReplacementPreflightResult result =
      PreflightCrowdReplacementCandidate(context, *candidate);
  const bool succeeded =
      result == CrowdReplacementPreflightResult::kSucceeded;
  bool ready_for_serving = false;
  {
    std::lock_guard lock(g_prewarm_mutex);
    // A block may make the proof complete on its final draw, but serving must
    // begin only on a later block. This prevents a readiness transition from
    // replacing a suffix of the block that established readiness.
    ready_for_serving = g_complete_block_ready;
    ++g_attempt_count;
    RecordBlockAttempt(*candidate, succeeded);
    if (succeeded) {
      ++g_success_count;
      if (!g_announced_success) {
        g_announced_success = true;
        REXLOG_INFO(
            "Table Tennis crowd prewarm: exact borrowed C6 pipeline and "
            "captured resource tuple preflighted "
            "(title_generation={} tile={} draw={}/{} pass={:08X} "
            "attempts={} succeeded={} failed={} guest_authoritative=true)",
            candidate->title_generation, candidate->tile_ordinal,
            candidate->draw_index + 1,
            candidate->frame->draws.size(), candidate->render_pass_key,
            g_attempt_count, g_success_count, g_failure_count);
      }
    } else {
      ++g_failure_count;
      if (!g_announced_failure) {
        g_announced_failure = true;
        REXLOG_WARN(
            "Table Tennis crowd prewarm: exact candidate reached the "
            "borrowed scope but resource/PSO preflight failed; guest draw "
            "kept (reason={} title_generation={} tile={} draw={}/{} "
            "attempts={} succeeded={} failed={})",
            CrowdReplacementPreflightResultName(result),
            candidate->title_generation, candidate->tile_ordinal,
            candidate->draw_index + 1, candidate->frame->draws.size(),
            g_attempt_count, g_success_count, g_failure_count);
      }
    }
  }

  const uint32_t serve_draws = REXCVAR_GET(
      tabletennis_native_crowd_replacement_serve_draws);
  if (succeeded && ready_for_serving &&
      candidate->draw_index < serve_draws &&
      DrawPreflightedCrowdReplacementCandidate(
          context, *candidate)) {
    std::lock_guard lock(g_prewarm_mutex);
    ++g_served_draw_count;
    if (!g_announced_serving) {
      g_announced_serving = true;
      REXLOG_INFO(
          "Table Tennis crowd replacement: serving first {} exact C6 "
          "draws from later verified blocks "
          "(title_generation={} tile={} first_draw={} "
          "served_total={} checked_draw=true)",
          serve_draws, candidate->title_generation,
          candidate->tile_ordinal, candidate->draw_index + 1,
          g_served_draw_count);
    }
    return true;
  }

  // PreflightDraw records no draw. A disabled/not-ready/failed checked draw
  // returns false so the original guest C6 draw remains authoritative.
  return false;
}

}  // namespace tabletennis::native
