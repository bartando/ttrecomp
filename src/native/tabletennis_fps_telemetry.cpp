#include "native/tabletennis_fps_telemetry.h"

#include <rex/cvar.h>
#include <rex/graphics/graphics_system.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/ui/presenter.h>

REXCVAR_DEFINE_UINT32(
    tabletennis_guest_fps_log_interval, 0, "Table Tennis",
    "Guest swaps between FPS/performance summaries (0 = disabled).")
    .range(0, 100000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace tabletennis::native {
namespace {

rex::ui::Presenter *GetPresenter() {
  rex::Runtime *runtime = rex::Runtime::instance();
  if (!runtime || !runtime->graphics_system()) {
    return nullptr;
  }

  auto *graphics_system =
      static_cast<rex::graphics::GraphicsSystem *>(runtime->graphics_system());
  return graphics_system->presenter();
}

} // namespace

void UpdateGuestFpsTelemetry(uint64_t title_frame_sequence) {
  const uint32_t interval = REXCVAR_GET(tabletennis_guest_fps_log_interval);
  if (interval == 0) {
    return;
  }

  rex::ui::Presenter *presenter = GetPresenter();
  if (!presenter) {
    return;
  }

  // Stats collection is normally enabled by the on-screen FPS/debug overlays.
  // The log-only path must work without forcing either UI overlay on.
  if (!presenter->GuestFrameStatsEnabled()) {
    presenter->SetGuestFrameStatsEnabled(true);
  }

  if (title_frame_sequence % interval != 0) {
    return;
  }

  const rex::ui::Presenter::GuestFrameStats stats =
      presenter->GetGuestFrameStats();
  REXLOG_INFO("Table Tennis guest performance: title_frame={} guest_frames={} "
              "fps={:.2f} frame_ms={:.2f} wait_ms={:.2f} gpu_ms={:.2f} "
              "gpu_draw_ms={:.2f} gpu_resolve_ms={:.2f} gpu_dump_ms={:.2f}",
              title_frame_sequence, stats.frame_count, stats.fps,
              stats.frame_time_ms, stats.wait_ms, stats.gpu_ms,
              stats.gpu_draw_ms, stats.gpu_resolve_ms, stats.gpu_dump_ms);
}

} // namespace tabletennis::native
