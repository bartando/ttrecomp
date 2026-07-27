#include "native/tabletennis_native_renderer.h"

#include "native/tabletennis_draw_replacer_dispatcher.h"
#include "native/tabletennis_native_capture.h"
#include "native/tabletennis_net_bb903_observer.h"
#include "native/tabletennis_observer_overlay.h"
#include "native/tabletennis_player_replacement_candidates.h"

#include <atomic>

#include <rex/cvar.h>
#include <rex/graphics/native_guest_renderer.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(
    tabletennis_native_render, false, "Table Tennis",
    "Diagnostic full-suppression benchmark that replaces detected gameplay "
    "with a flat clear. This does not render game content; F5 toggles it live.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace tabletennis::native {
namespace {

using rex::graphics::NativeGuestOutputBackend;
using rex::graphics::NativeGuestOutputRenderContext;
namespace nrhi = rex::graphics::nrhi;

std::atomic<bool> g_announced_serving = false;

void ObserveDrawEligibility(
    const rex::graphics::NativeGuestDrawEligibilityContext &context, void *) {
  ObserveNetBB903BackendEligibility(context);
  if (PlayerReplacementGateDiagnosticEnabled()) {
    ObservePlayerReplacementDrawEligibility(context, nullptr);
  }
}

bool Render(const NativeGuestOutputRenderContext &context, void *) {
  if (!Enabled() || context.cmd == nullptr || context.guest_output == nullptr ||
      (context.backend != NativeGuestOutputBackend::kD3D12 &&
       context.backend != NativeGuestOutputBackend::kVulkan)) {
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
  rex::graphics::SetNativeGuestOutputRenderer(enabled ? &Render : nullptr,
                                              nullptr);
  REXLOG_INFO("Table Tennis native renderer {}",
              enabled ? "enabled" : "disabled");
}

void Install() {
  if (Enabled()) {
    rex::graphics::SetNativeGuestOutputRenderer(&Render, nullptr);
  }
  InstallObserverOverlay();
  rex::graphics::SetNativeGuestDrawReplacer(&MatchDispatchedDrawReplacement,
                                            &RenderDispatchedDrawReplacement,
                                            nullptr);
  if (NetBB903ObserverEnabled() ||
      PlayerReplacementGateDiagnosticEnabled()) {
    rex::graphics::SetNativeGuestDrawEligibilityObserver(
        &ObserveDrawEligibility, nullptr);
  }
  if (PlayerReplacementGateDiagnosticEnabled()) {
    REXLOG_INFO(
        "Table Tennis CA9 Vulkan pre-gate diagnostic enabled "
        "(observer_only=true)");
  }
  REXLOG_INFO("Table Tennis native renderer installed (F5 toggles takeover)");
}

void Shutdown() {
  rex::graphics::SetNativeGuestDrawEligibilityObserver(nullptr, nullptr);
  rex::graphics::SetNativeGuestDrawReplacer(nullptr, nullptr, nullptr);
  ResetDrawReplacerDispatcher();
  ShutdownObserverOverlay();
  g_announced_serving.store(false);
  rex::graphics::SetNativeGuestOutputRenderer(nullptr, nullptr);
}

} // namespace tabletennis::native
