#include "tabletennis_defaults.h"

#include <rex/cvar.h>
#include <rex/logging.h>

namespace tabletennis {
namespace {

struct Default {
  const char* name;
  const char* value;
};

constexpr Default kDefaults[] = {
    // The game advances a fixed step per guest frame, so its speed follows the
    // guest vblank. Host-clock pacing keeps that vblank at a real 60 Hz;
    // without it the intro and menus run too fast.
    {"vsync", "true"},
    {"vblank_host_clock_pacing", "true"},
    {"video_mode_refresh_rate", "60"},
    // Stops host-page watches from re-uploading unrelated textures every frame.
    {"shared_memory_filter_gpu_page_only_watches", "true"},
    {"async_shader_compilation", "true"},
    // Playable without a controller.
    {"mnk_mode", "true"},
    {"fullscreen", "false"},
    // Keep normal play sessions useful for investigating stutters.
    {"frame_hitch_diagnostics", "true"},
    {"tabletennis_guest_fps_log_interval", "300"},
    {"log_flush_interval", "1"},
    // The SDK supersamples at 2x2, which costs gameplay its 60 fps here.
    {"resolution_scale", "1"},
    {"draw_resolution_scale_x", "1"},
    {"draw_resolution_scale_y", "1"},
};

}  // namespace

void ApplyAppDefaults() {
  for (const Default& entry : kDefaults) {
    if (!rex::cvar::SetDefaultValue(entry.name, entry.value)) {
      REXLOG_WARN("Could not apply app default {}={}", entry.name, entry.value);
    }
  }
}

}  // namespace tabletennis
