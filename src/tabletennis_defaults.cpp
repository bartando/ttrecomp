#include "tabletennis_defaults.h"

#include <cinttypes>
#include <cstdio>
#include <fstream>
#include <random>
#include <string>
#include <system_error>

#include <rex/cvar.h>
#include <rex/logging.h>

namespace tabletennis {
namespace {

struct Default {
  const char* name;
  const char* value;
};

constexpr Default kDefaults[] = {
    // The native renderers only run on Vulkan; "auto" would pick D3D12 first
    // on Windows. Startup still falls back to D3D12 if Vulkan fails to start.
    {"gpu_backend", "vulkan"},
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
#if defined(_WIN32)
    // Borderless; F11 or Alt+Enter switches to a window, Alt+F4 quits.
    {"fullscreen", "true"},
#else
    {"fullscreen", "false"},
#endif
    // Keep normal play sessions useful for investigating stutters.
    {"frame_hitch_diagnostics", "true"},
    {"tabletennis_guest_fps_log_interval", "300"},
    {"log_flush_interval", "1"},
#if defined(_WIN32)
    // 2560x1440 with FSR 1 to the display: a locked 60 in matches on an
    // RTX 3080. At 2x the host-pixel half-pixel offset leaves uncovered strips
    // along resolve edges; the guest-pixel offset with edge fill is clean.
    {"resolution_scale", "2"},
    {"draw_resolution_scale_x", "2"},
    {"draw_resolution_scale_y", "2"},
    {"draw_resolution_scaled_half_pixel_offset", "false"},
    {"present_effect", "fsr"},
#else
    // The SDK supersamples at 2x2, which costs gameplay its 60 fps here.
    {"resolution_scale", "1"},
    {"draw_resolution_scale_x", "1"},
    {"draw_resolution_scale_y", "1"},
#endif
    // Online play is System Link (the game's own LAN mode, see
    // native/tabletennis_system_link.cpp); the menu only offers it to a
    // signed-in Live profile.
    {"tabletennis_system_link", "true"},
    {"user_live_signed_in", "true"},
};

// The SDK's default profile XUID, which every install shared before.
constexpr const char* kSharedXuid = "B13E07DFF9AB6772";
constexpr const char* kXuidFileName = "profile_xuid.txt";

std::string NewXuid() {
  std::random_device random;
  char text[17];
  std::snprintf(text, sizeof(text), "B13E07DF%08" PRIX32, uint32_t(random()));
  return text;
}

}  // namespace

void ApplyAppDefaults() {
  for (const Default& entry : kDefaults) {
    if (!rex::cvar::SetDefaultValue(entry.name, entry.value)) {
      REXLOG_WARN("Could not apply app default {}={}", entry.name, entry.value);
    }
  }
}

void EnsureInstallXuid(const std::filesystem::path& user_data_root) {
  if (rex::cvar::GetFlagByName("user_profile_xuid") != kSharedXuid) {
    return;  // Chosen by the user.
  }
  const auto xuid_file = user_data_root / kXuidFileName;
  std::string xuid;
  if (std::ifstream in(xuid_file); in) {
    std::getline(in, xuid);
  }
  if (xuid.size() != 16) {
    xuid = NewXuid();
    std::error_code error;
    std::filesystem::create_directories(user_data_root, error);
    std::ofstream(xuid_file) << xuid << "\n";
    // Copy, not move: the old profile stays usable if the XUID is reset.
    const auto shared_saves = user_data_root / kSharedXuid;
    const auto own_saves = user_data_root / xuid;
    if (std::filesystem::is_directory(shared_saves) && !std::filesystem::exists(own_saves)) {
      std::filesystem::copy(shared_saves, own_saves, std::filesystem::copy_options::recursive,
                            error);
      if (error) {
        REXLOG_WARN("Could not copy saves to the new profile {}: {}", xuid, error.message());
      }
    }
    REXLOG_INFO("Profile XUID for this install: {}", xuid);
  }
  rex::cvar::SetFlagByName("user_profile_xuid", xuid);
}

}  // namespace tabletennis
