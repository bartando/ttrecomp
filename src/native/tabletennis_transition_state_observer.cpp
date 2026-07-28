#include "native/tabletennis_transition_state_observer.h"

#include "native/tabletennis_late_phase_ledger.h"

#include <algorithm>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(
    tabletennis_native_transition_state_observer, false, "Table Tennis",
    "Capture the exact post-pipeline MAIN-to-COMP transition state. "
    "Observer-only; exposes no command list and never renders or suppresses.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace tabletennis::native {
namespace {

namespace nrhi = rex::graphics::nrhi;

constexpr uint64_t kTransitionVertexShader = 0xFA14ACFDF2DE3ED0ull;
constexpr uint64_t kTransitionPixelShader = 0x5E11FC7AE2F1C2BFull;
constexpr uint32_t kFullscreenPrimitive = 8;
constexpr uint32_t kFullscreenVertexCount = 3;
constexpr size_t kMaximumCandidatesPerFrame = 8;
constexpr size_t kMaximumRetainedFrames = 8;

struct BackendFrame {
  uint64_t sequence = 0;
  uint32_t dropped_candidates = 0;
  std::vector<rex::graphics::NativeGuestDrawStateContext> candidates;
};

std::mutex g_mutex;
std::deque<BackendFrame> g_frames;
std::shared_ptr<const MainToCompTransitionStateSnapshot> g_published;
MainToCompTransitionStateTelemetry g_telemetry;
uint64_t g_latest_sequence = 0;
bool g_logged_publication = false;
bool g_logged_rejection = false;

bool ExactShaderIdentity(
    const rex::graphics::NativeGuestDrawStateContext &context) {
  return context.draw.vertex_shader_hash == kTransitionVertexShader &&
         context.draw.pixel_shader_hash == kTransitionPixelShader &&
         context.draw.primitive_type == kFullscreenPrimitive &&
         context.draw.guest_vertex_or_index_count == kFullscreenVertexCount;
}

bool RawTargetStateMatchesDecoded(
    const rex::graphics::NativeGuestDrawContext::RenderTargetState &state) {
  constexpr uint32_t kEdramBaseMask = (1u << 12) - 1;
  constexpr uint32_t kSurfacePitchMask = (1u << 14) - 1;
  constexpr uint32_t kEdramModeMask = (1u << 3) - 1;
  return state.valid &&
         (state.rb_color_info_0 & kEdramBaseMask) ==
             state.color_edram_base &&
         (state.rb_depth_info & kEdramBaseMask) ==
             state.depth_edram_base &&
         (state.rb_surface_info & kSurfacePitchMask) ==
             state.surface_pitch &&
         (state.rb_modecontrol & kEdramModeMask) == state.edram_mode;
}

bool ExactPostPipelineContract(
    const rex::graphics::NativeGuestDrawStateContext &context) {
  const auto &draw = context.draw;
  return context.valid &&
         draw.backend ==
             rex::graphics::NativeGuestOutputBackend::kVulkan &&
         draw.backend_frame_sequence != 0 && ExactShaderIdentity(context) &&
         draw.render_pass_key_valid && draw.draw_state_contract_valid &&
         RawTargetStateMatchesDecoded(draw.render_target_state) &&
         draw.borrowed_attachment_contract_valid &&
         draw.color_attachment_count != 0 &&
         draw.color_attachment_formats[0] != nrhi::Format::kUnknown &&
         draw.sample_count != 0 && context.viewport_scissor_valid &&
         context.texture_fetches_valid && draw.device == nullptr &&
         draw.cmd == nullptr;
}

BackendFrame &FindOrCreateFrame(uint64_t sequence) {
  const auto found =
      std::ranges::find(g_frames, sequence, &BackendFrame::sequence);
  if (found != g_frames.end()) {
    return *found;
  }
  if (g_frames.size() == kMaximumRetainedFrames) {
    g_telemetry.dropped_candidates +=
        g_frames.front().candidates.size() +
        g_frames.front().dropped_candidates;
    g_frames.pop_front();
  }
  g_frames.push_back({.sequence = sequence});
  return g_frames.back();
}

void LogContract(const MainToCompTransitionStateSnapshot &snapshot) {
  const auto &state = snapshot.state;
  const auto &draw = state.draw;
  const auto &target = draw.render_target_state;
  REXLOG_INFO(
      "Table Tennis MAIN-to-COMP transition state: frame={} candidates={} "
      "shader={:016X}/{:016X} primitive={} guest_count={} host_count={} "
      "indexed={} pass={:08X} "
      "target_raw[color={:08X} depth={:08X} surface={:08X} mode={:08X}] "
      "target_decoded[color_base={:03X} depth_base={:03X} pitch={} mode={}] "
      "draw_state[depth={:08X} mask={:08X} color={:08X} blend={:08X} "
      "raster={:08X} restart={}/{}] "
      "attachments[color_count={} color0={} depth={} stencil={} samples={} "
      "sample_mask={:016X}] "
      "viewport[offset={}/{} extent={}/{} depth={}/{}] "
      "scissor[offset={}/{} extent={}/{}] textures={:08X} "
      "slot0={:08X},{:08X},{:08X},{:08X},{:08X},{:08X} "
      "observer_only=true guest_suppressed=false",
      snapshot.sequence, snapshot.candidate_count, draw.vertex_shader_hash,
      draw.pixel_shader_hash, draw.primitive_type,
      draw.guest_vertex_or_index_count, draw.vertex_or_index_count,
      draw.indexed, draw.render_pass_key, target.rb_color_info_0,
      target.rb_depth_info, target.rb_surface_info, target.rb_modecontrol,
      target.color_edram_base, target.depth_edram_base,
      target.surface_pitch, target.edram_mode,
      draw.normalized_depth_control, draw.normalized_color_mask,
      draw.color_control, draw.blend_control_0,
      draw.rasterizer_mode_control, draw.primitive_restart_enabled,
      draw.primitive_restart_index, draw.color_attachment_count,
      static_cast<uint32_t>(draw.color_attachment_formats[0]),
      static_cast<uint32_t>(draw.depth_attachment_format),
      static_cast<uint32_t>(draw.stencil_attachment_format),
      draw.sample_count, draw.sample_mask, state.viewport_offset[0],
      state.viewport_offset[1], state.viewport_extent[0],
      state.viewport_extent[1], state.viewport_min_depth,
      state.viewport_max_depth, state.scissor_offset[0],
      state.scissor_offset[1], state.scissor_extent[0],
      state.scissor_extent[1], state.active_texture_fetch_mask,
      state.texture_fetch_words[0][0], state.texture_fetch_words[0][1],
      state.texture_fetch_words[0][2], state.texture_fetch_words[0][3],
      state.texture_fetch_words[0][4], state.texture_fetch_words[0][5]);
}

void FinalizeOlderFrames(uint64_t newer_sequence) {
  for (BackendFrame &frame : g_frames) {
    if (frame.sequence >= newer_sequence || frame.candidates.empty()) {
      continue;
    }
    if (frame.dropped_candidates != 0 || frame.candidates.size() != 1 ||
        !ExactPostPipelineContract(frame.candidates.front())) {
      ++g_telemetry.rejected_frames;
      if (!g_logged_rejection) {
        g_logged_rejection = true;
        REXLOG_INFO(
            "Table Tennis MAIN-to-COMP transition state rejected: frame={} "
            "candidates={} dropped={} observer_only=true "
            "guest_suppressed=false",
            frame.sequence, frame.candidates.size(),
            frame.dropped_candidates);
      }
      frame.candidates.clear();
      continue;
    }
    auto published =
        std::make_shared<MainToCompTransitionStateSnapshot>();
    published->sequence = frame.sequence;
    published->candidate_count = 1;
    published->state = frame.candidates.front();
    published->state.draw.device = nullptr;
    published->state.draw.cmd = nullptr;
    frame.candidates.clear();
    if (!published->valid()) {
      ++g_telemetry.rejected_frames;
      continue;
    }
    ++g_telemetry.published_frames;
    g_telemetry.latest_published_sequence = published->sequence;
    g_published = std::move(published);
    if (!g_logged_publication) {
      g_logged_publication = true;
      LogContract(*g_published);
    }
  }
  while (!g_frames.empty() && g_frames.front().sequence < newer_sequence &&
         g_frames.front().candidates.empty()) {
    g_frames.pop_front();
  }
}

} // namespace

bool MainToCompTransitionStateSnapshot::valid() const {
  return sequence != 0 && candidate_count == 1 &&
         state.draw.backend_frame_sequence == sequence &&
         ExactPostPipelineContract(state);
}

bool MainToCompTransitionStateObserverEnabled() {
  return REXCVAR_GET(tabletennis_native_transition_state_observer) ||
         LatePhaseLedgerEnabled();
}

void ObserveMainToCompTransitionState(
    const rex::graphics::NativeGuestDrawStateContext &context) {
  if (!MainToCompTransitionStateObserverEnabled()) {
    return;
  }
  std::lock_guard lock(g_mutex);
  ++g_telemetry.callbacks;
  g_telemetry.latest_backend_sequence =
      std::max(g_telemetry.latest_backend_sequence,
               context.draw.backend_frame_sequence);
  if (!ExactShaderIdentity(context)) {
    return;
  }
  ++g_telemetry.shader_pair_matches;
  const uint64_t sequence = context.draw.backend_frame_sequence;
  if (sequence == 0) {
    ++g_telemetry.dropped_candidates;
    return;
  }
  if (sequence > g_latest_sequence) {
    FinalizeOlderFrames(sequence);
    g_latest_sequence = sequence;
  }
  BackendFrame &frame = FindOrCreateFrame(sequence);
  if (frame.candidates.size() == kMaximumCandidatesPerFrame) {
    ++frame.dropped_candidates;
    ++g_telemetry.dropped_candidates;
    return;
  }
  rex::graphics::NativeGuestDrawStateContext snapshot = context;
  snapshot.draw.device = nullptr;
  snapshot.draw.cmd = nullptr;
  frame.candidates.push_back(std::move(snapshot));
  ++g_telemetry.candidates;
  g_telemetry.retained_frames = static_cast<uint32_t>(g_frames.size());
}

std::shared_ptr<const MainToCompTransitionStateSnapshot>
LatestMainToCompTransitionStateSnapshot() {
  std::lock_guard lock(g_mutex);
  return g_published;
}

MainToCompTransitionStateTelemetry
LatestMainToCompTransitionStateTelemetry() {
  std::lock_guard lock(g_mutex);
  MainToCompTransitionStateTelemetry telemetry = g_telemetry;
  telemetry.retained_frames = static_cast<uint32_t>(g_frames.size());
  return telemetry;
}

} // namespace tabletennis::native
