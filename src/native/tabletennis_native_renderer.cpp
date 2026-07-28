#include "native/tabletennis_native_renderer.h"

#include "native/tabletennis_draw_replacer_dispatcher.h"
#include "native/tabletennis_generic_main_private_batch_replay.h"
#include "native/tabletennis_guarded_venue_capture_retirement.h"
#include "native/tabletennis_guarded_venue_private_batch_replay.h"
#include "native/tabletennis_hud_swf_backend_observer.h"
#include "native/tabletennis_late_phase_ledger.h"
#include "native/tabletennis_native_capture.h"
#include "native/tabletennis_net_bb903_observer.h"
#include "native/tabletennis_observer_overlay.h"
#include "native/tabletennis_phase0_rectangle_readback.h"
#include "native/tabletennis_phase0_rectangle_replay.h"
#include "native/tabletennis_player_bbb5_observer.h"
#include "native/tabletennis_player_replacement_candidates.h"
#include "native/tabletennis_ps328_tile_invariance.h"
#include "native/tabletennis_transition_state_observer.h"
#include "native/tabletennis_translated_shader_artifact_store.h"

#include <atomic>
#include <chrono>

#include <rex/cvar.h>
#include <rex/graphics/native_guest_renderer.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(
    tabletennis_native_render, false, "Table Tennis",
    "Diagnostic full-suppression benchmark that replaces detected gameplay "
    "with a flat clear. This does not render game content; F5 toggles it live.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(
    tabletennis_native_private_translated_replay, false, "Table Tennis",
    "Execute private translated-draw replay diagnostics. Artifact and guarded "
    "token capture remain observer-only when disabled.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace tabletennis::native {
namespace {

using rex::graphics::NativeGuestOutputBackend;
using rex::graphics::NativeGuestOutputRenderContext;
namespace nrhi = rex::graphics::nrhi;

bool PrivateTranslatedReplayEnabled() {
  return REXCVAR_GET(tabletennis_native_private_translated_replay);
}

std::atomic<bool> g_announced_serving = false;
std::atomic<bool> g_phase0_replay_succeeded = false;
std::atomic<bool> g_phase0_replay_failure_announced = false;
std::atomic<bool> g_phase0_readback_queue_failure_announced = false;
std::atomic<bool> g_guarded_venue_replay_succeeded = false;
std::atomic<uint32_t> g_guarded_venue_last_blocker = UINT32_MAX;
std::atomic<bool> g_generic_main_replay_succeeded = false;
std::atomic<uint32_t> g_generic_main_last_blocker = UINT32_MAX;
std::atomic<uint32_t> g_generic_main_best_draw_count = 0;
uint32_t g_ps328_retired_heartbeat_callbacks = 0;
std::chrono::steady_clock::time_point g_ps328_retired_heartbeat_start;

void ObservePs328RetiredOutputHeartbeat(uint64_t backend_frame_sequence) {
  const bool takeover_active = Enabled();
  const bool ps328_retired = Ps328TitleCaptureRetired();
  if (!takeover_active && !ps328_retired) {
    g_ps328_retired_heartbeat_callbacks = 0;
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  if (g_ps328_retired_heartbeat_callbacks == 0) {
    g_ps328_retired_heartbeat_start = now;
  }
  ++g_ps328_retired_heartbeat_callbacks;
  if (g_ps328_retired_heartbeat_callbacks <= 3) {
    REXLOG_INFO("Table Tennis native output progress callback={} "
                "backend_frame={} takeover_active={} ps328_retired={}",
                g_ps328_retired_heartbeat_callbacks, backend_frame_sequence,
                takeover_active, ps328_retired);
  }
  constexpr uint32_t kHeartbeatCallbacks = 120;
  if (g_ps328_retired_heartbeat_callbacks < kHeartbeatCallbacks) {
    return;
  }
  const std::chrono::duration<double> elapsed =
      now - g_ps328_retired_heartbeat_start;
  const double seconds = elapsed.count();
  constexpr uint32_t kMeasuredCallbackIntervals = kHeartbeatCallbacks - 1;
  const double fps =
      seconds > 0.0 ? double(kMeasuredCallbackIntervals) / seconds : 0.0;
  REXLOG_INFO("Table Tennis native output heartbeat "
              "callbacks={} elapsed_seconds={:.3f} fps={:.2f} "
              "gpu_waits=false takeover_active={} ps328_retired={}",
              kHeartbeatCallbacks, seconds, fps, takeover_active,
              ps328_retired);
  g_ps328_retired_heartbeat_callbacks = 0;
  g_ps328_retired_heartbeat_start = now;
}

void TryReplayPhase0Rectangle(const NativeGuestOutputRenderContext &context) {
  PollPhase0RectangleReadback(context);
  if (!TranslatedShaderArtifactStoreEnabled() ||
      g_phase0_replay_succeeded.load(std::memory_order_acquire)) {
    return;
  }

  ExactMainPreparedTargets private_targets;
  const ExactMainTargetsResult prepare_result =
      PreparePhase0RectanglePrivateTargets(context, private_targets);
  if (prepare_result != ExactMainTargetsResult::kSucceeded) {
    return;
  }

  const auto snapshot = LatestPhase0RectangleReplaySnapshot();
  if (snapshot == nullptr || !snapshot->replay_ready() ||
      !private_targets.valid(context)) {
    return;
  }

  rex::graphics::NativeGuestTranslatedReplayTarget target;
  target.color = private_targets.attachments.color;
  target.depth_stencil = private_targets.attachments.depth;
  target.width = private_targets.attachments.width;
  target.height = private_targets.attachments.height;
  target.color_format = nrhi::Format::kR8G8B8A8_UNORM;
  target.depth_stencil_format = snapshot->target_contract.depth_stencil_format;
  target.sample_count = snapshot->target_contract.sample_count;
  target.clear_color = true;
  target.clear_depth_stencil = true;
  target.clear_color_value = {0.0f, 0.0f, 0.0f, 0.0f};
  target.clear_depth_value = 1.0f;
  target.clear_stencil_value = 0;

  const auto replay_result = rex::graphics::TryReplayNativeGuestTranslatedDraw(
      context, *snapshot->replay_token, target);
  if (replay_result ==
      rex::graphics::NativeGuestTranslatedReplayResult::kSucceeded) {
    const bool readback_queued = QueuePhase0RectangleReadback(
        context, private_targets, snapshot->backend_frame_sequence);
    if (!readback_queued) {
      if (!g_phase0_readback_queue_failure_announced.exchange(
              true, std::memory_order_acq_rel)) {
        REXLOG_WARN(
            "Table Tennis phase-0 rectangle private readback queue failed "
            "frame={} token={} guest_untouched=true",
            snapshot->backend_frame_sequence,
            snapshot->replay_token->opaque_token);
      }
      return;
    }
    if (!g_phase0_replay_succeeded.exchange(true, std::memory_order_acq_rel)) {
      REXLOG_INFO("Table Tennis phase-0 rectangle translated replay recorded "
                  "frame={} token={} readback_queued={} private_target=true "
                  "guest_untouched=true",
                  snapshot->backend_frame_sequence,
                  snapshot->replay_token->opaque_token, readback_queued);
    }
    return;
  }

  if (!g_phase0_replay_failure_announced.exchange(true,
                                                  std::memory_order_acq_rel)) {
    REXLOG_WARN(
        "Table Tennis phase-0 rectangle translated replay rejected "
        "frame={} token={} result={} guest_untouched=true",
        snapshot->backend_frame_sequence, snapshot->replay_token->opaque_token,
        rex::graphics::NativeGuestTranslatedReplayResultName(replay_result));
  }
}

// The generic path replays whatever the backend proved this frame, so it is
// attempted independently of the four-family venue batch. It stays a private
// diagnostic: a partial plan is expected while families are still being
// admitted, and coverage is reported rather than presented.
void TryReplayGenericMainBatch(
    const NativeGuestOutputRenderContext &context) {
  if (!TranslatedShaderArtifactStoreEnabled()) {
    return;
  }
  LogGenericMainFamilyCensusOnChange(context);
  // Unlike the four-family batch, this keeps replaying after its first
  // success: coverage is the measurement, and it only grows as more families
  // finish their proofs. The private readback is queued once, since its GPU
  // copy is not worth paying for on frames nobody inspects.
  const bool first_success =
      !g_generic_main_replay_succeeded.load(std::memory_order_acquire);
  const GenericMainPrivateBatchReplayResult batch =
      ReplayCurrentFrameGenericMainPrivateBatch(context, first_success);
  if (!batch.succeeded()) {
    const uint32_t blocker = static_cast<uint32_t>(batch.status);
    if (g_generic_main_last_blocker.exchange(
            blocker, std::memory_order_acq_rel) != blocker) {
      REXLOG_INFO(
          "Table Tennis generic MAIN private batch replay waiting reason={} "
          "replay_result={} frame={} families={} proven={} observed_draws={} "
          "covered_draws={} observer_only=true guest_untouched=true",
          GenericMainPrivateBatchReplayStatusName(batch.status),
          batch.replay_result_name, batch.backend_frame_sequence,
          batch.discovered_family_count, batch.proven_family_count,
          batch.observed_draw_count, batch.covered_draw_count);
    }
    return;
  }
  g_generic_main_replay_succeeded.store(true, std::memory_order_release);
  g_generic_main_last_blocker.store(UINT32_MAX, std::memory_order_release);
  // Report the first success and then only when the plan grows, so a settled
  // frame rate is not paid in log lines.
  const uint32_t previous_best = g_generic_main_best_draw_count.load(
      std::memory_order_acquire);
  if (!first_success && batch.recorded_draw_count <= previous_best) {
    return;
  }
  g_generic_main_best_draw_count.store(batch.recorded_draw_count,
                                       std::memory_order_release);
  REXLOG_INFO(
      "Table Tennis generic MAIN private batch replay recorded frame={} "
      "families={} proven={} observed_draws={} covered_draws={} "
      "draw_count={} readback_queued={} private_target=true "
      "global_token_order=true one_scope=true clear_once=true "
      "normalized_scissor=1280x720 guest_untouched=true",
      batch.backend_frame_sequence, batch.discovered_family_count,
      batch.proven_family_count, batch.observed_draw_count,
      batch.covered_draw_count, batch.recorded_draw_count, first_success);
}

void TryReplayGuardedVenueBatch(
    const NativeGuestOutputRenderContext &context) {
  if (!TranslatedShaderArtifactStoreEnabled() ||
      g_guarded_venue_replay_succeeded.load(
          std::memory_order_acquire)) {
    return;
  }
  const Phase0RectangleReadbackResult readback =
      LatestPhase0RectangleReadbackResult();
  if (!readback.valid ||
      readback.kind != PrivateReplayDiagnosticKind::kRectangle) {
    return;
  }

  const GuardedVenuePrivateBatchReplayResult batch =
      ReplayCurrentFrameGuardedVenuePrivateBatch(context);
  if (!batch.succeeded()) {
    const uint32_t blocker = static_cast<uint32_t>(batch.status);
    if (g_guarded_venue_last_blocker.exchange(
            blocker, std::memory_order_acq_rel) != blocker) {
      REXLOG_INFO(
          "Table Tennis guarded venue private batch replay waiting reason={} "
          "frame={} ps328={} venue9e={} crowd_c6={} venue14d={} recorded={} "
          "observer_only=true guest_untouched=true",
          GuardedVenuePrivateBatchReplayStatusName(batch.status),
          batch.backend_frame_sequence, batch.ps328_draw_count,
          batch.venue_9e_draw_count, batch.crowd_c6_draw_count,
          batch.venue_14d_draw_count,
          batch.recorded_draw_count);
    }
    return;
  }
  g_guarded_venue_replay_succeeded.store(
      true, std::memory_order_release);
  REXLOG_INFO(
      "Table Tennis guarded venue private batch replay recorded frame={} "
      "ps328={} venue9e={} crowd_c6={} venue14d={} draw_count={} "
      "readback_queued=true "
      "private_target=true global_token_order=true one_scope=true "
      "clear_once=true normalized_scissor=1280x720 guest_untouched=true",
      batch.backend_frame_sequence, batch.ps328_draw_count,
      batch.venue_9e_draw_count, batch.crowd_c6_draw_count,
      batch.venue_14d_draw_count,
      batch.recorded_draw_count);
}

void RetireCompletedGuardedVenueTitleCapture() {
  const Phase0RectangleReadbackResult readback =
      LatestPhase0RectangleReadbackResult();
  if (!readback.nonempty() ||
      readback.kind !=
          PrivateReplayDiagnosticKind::kGuardedVenueBatch) {
    return;
  }
  RetireGuardedVenueTitleCapture(
      GuardedVenueTitleCaptureFamily::kPs328,
      readback.backend_frame_sequence);
  RetireGuardedVenueTitleCapture(
      GuardedVenueTitleCaptureFamily::kVenue9E,
      readback.backend_frame_sequence);
}

void ObserveDrawEligibility(
    const rex::graphics::NativeGuestDrawEligibilityContext &context, void *) {
  ObserveLatePhaseDrawEligibility(context);
  ObserveHudSwfBackendEligibility(context);
  ObserveNetBB903BackendEligibility(context);
  if (PlayerReplacementGateDiagnosticEnabled()) {
    ObservePlayerReplacementDrawEligibility(context, nullptr);
  }
}

void ObserveDrawState(const rex::graphics::NativeGuestDrawStateContext &context,
                      void *) {
  ObserveMainToCompTransitionState(context);
  ObservePlayerBBB5BackendDraw(context.draw);
}

bool FilterDrawState(uint64_t vertex_shader_hash, uint64_t pixel_shader_hash,
                     void *) {
  constexpr uint64_t kTransitionVertexShader = 0xFA14ACFDF2DE3ED0ull;
  constexpr uint64_t kTransitionPixelShader = 0x5E11FC7AE2F1C2BFull;
  constexpr uint64_t kBBB5VertexShader = 0xBBB5B0E2C39FB1CEull;
  constexpr uint64_t kBBB5PixelShader = 0x2D7133947D0D8685ull;
  return (MainToCompTransitionStateObserverEnabled() &&
          vertex_shader_hash == kTransitionVertexShader &&
          pixel_shader_hash == kTransitionPixelShader) ||
         (PlayerBBB5ObserverEnabled() &&
          vertex_shader_hash == kBBB5VertexShader &&
          pixel_shader_hash == kBBB5PixelShader);
}

bool Render(const NativeGuestOutputRenderContext &context, void *) {
  if (context.cmd == nullptr || context.guest_output == nullptr ||
      (context.backend != NativeGuestOutputBackend::kD3D12 &&
       context.backend != NativeGuestOutputBackend::kVulkan)) {
    return false;
  }

  if (PrivateTranslatedReplayEnabled()) {
    TryReplayPhase0Rectangle(context);
    RetireCompletedGuardedVenueTitleCapture();
    TryReplayGuardedVenueBatch(context);
    TryReplayGenericMainBatch(context);
  }
  ObservePs328RetiredOutputHeartbeat(context.backend_frame_sequence);
  if (!Enabled()) {
    return false;
  }

  const CapturedFrame frame = LatestCapturedFrame();
  if (!frame.gameplay_active) {
    // Keep complete guest rendering for the frontend and loading screens.
    // Gameplay activation is driven by the title's own pongPlayer submissions,
    // the same high-level-data strategy used by Skate 3's native renderer.
    return false;
  }
  if (!g_announced_serving.exchange(true)) {
    REXLOG_INFO("Table Tennis native renderer serving gameplay frame {} "
                "(players={:08X},{:08X}, submissions={})",
                frame.sequence, frame.players[0], frame.players[1],
                frame.submission_count);
  }

  nrhi::Cmd *const cmd = context.cmd;
  cmd->ProfileRegion(nrhi::ProfileStage::kMain);
  cmd->Barrier(context.guest_output, nrhi::ResourceState::kGuestOutput,
               nrhi::ResourceState::kRenderTarget);
  cmd->FlushBarriers();
  constexpr float kSuppressionBenchmarkColor[4] = {0.035f, 0.075f, 0.12f, 1.0f};
  cmd->ClearRenderTarget(context.guest_output, kSuppressionBenchmarkColor);
  cmd->Barrier(context.guest_output, nrhi::ResourceState::kRenderTarget,
               nrhi::ResourceState::kGuestOutput);
  cmd->FlushBarriers();
  cmd->ProfileRegion(nrhi::ProfileStage::kTail);
  return true;
}

} // namespace

bool Enabled() { return REXCVAR_GET(tabletennis_native_render); }

void Toggle() {
  const bool enabled = !Enabled();
  REXCVAR_SET(tabletennis_native_render, enabled);
  rex::graphics::SetNativeGuestOutputRenderer(
      enabled || PrivateTranslatedReplayEnabled() ? &Render : nullptr,
      nullptr);
  REXLOG_INFO("Table Tennis native renderer {}",
              enabled ? "enabled" : "disabled");
}

void Install() {
  InstallTranslatedShaderArtifactStore();
  if (Enabled() || PrivateTranslatedReplayEnabled()) {
    rex::graphics::SetNativeGuestOutputRenderer(&Render, nullptr);
  }
  InstallObserverOverlay();
  rex::graphics::SetNativeGuestDrawReplacer(&MatchDispatchedDrawReplacement,
                                            &RenderDispatchedDrawReplacement,
                                            nullptr);
  if (LatePhaseLedgerEnabled() || HudSwfBackendObserverEnabled() ||
      NetBB903ObserverEnabled() || PlayerReplacementGateDiagnosticEnabled()) {
    rex::graphics::SetNativeGuestDrawEligibilityObserver(
        &ObserveDrawEligibility, nullptr);
  }
  if (MainToCompTransitionStateObserverEnabled() ||
      PlayerBBB5ObserverEnabled()) {
    rex::graphics::SetNativeGuestDrawStateObserver(&ObserveDrawState,
                                                   &FilterDrawState, nullptr);
  }
  if (PlayerReplacementGateDiagnosticEnabled()) {
    REXLOG_INFO("Table Tennis CA9 Vulkan pre-gate diagnostic enabled "
                "(observer_only=true)");
  }
  REXLOG_INFO("Table Tennis native renderer installed (F5 toggles takeover)");
}

void Shutdown() {
  // Stop new native output callbacks before artifact-store shutdown clears
  // the retained PS328 proof and its family-local retirement latch.
  rex::graphics::SetNativeGuestOutputRenderer(nullptr, nullptr);
  ShutdownPhase0RectangleReadback();
  ShutdownPhase0RectangleReplay();
  ShutdownTranslatedShaderArtifactStore();
  rex::graphics::SetNativeGuestDrawStateObserver(nullptr, nullptr, nullptr);
  rex::graphics::SetNativeGuestDrawEligibilityObserver(nullptr, nullptr);
  rex::graphics::SetNativeGuestDrawReplacer(nullptr, nullptr, nullptr);
  ResetDrawReplacerDispatcher();
  ShutdownObserverOverlay();
  g_announced_serving.store(false);
  g_phase0_replay_succeeded.store(false);
  g_phase0_replay_failure_announced.store(false);
  g_phase0_readback_queue_failure_announced.store(false);
  g_guarded_venue_replay_succeeded.store(false);
  g_guarded_venue_last_blocker.store(UINT32_MAX);
  g_generic_main_replay_succeeded.store(false);
  g_generic_main_last_blocker.store(UINT32_MAX);
  g_generic_main_best_draw_count.store(0);
  g_ps328_retired_heartbeat_callbacks = 0;
  g_ps328_retired_heartbeat_start = {};
}

} // namespace tabletennis::native
