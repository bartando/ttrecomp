// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <system_error>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/graphics/flags.h>
#include <rex/logging.h>
#include <rex/ui/flags.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/immediate_drawer.h>
#include <rex/ui/presenter.h>
#include <rex/ui/windowed_app_context.h>

REXCVAR_DECLARE(bool, ps5_pad_rumble);
REXCVAR_DECLARE(bool, ps5_show_fps);

// The title's own settings menu, opened with the touchpad click. The game
// keeps running underneath; while the menu is open it sees an idle pad.
namespace tabletennis::ps5 {

// scePad button bits.
inline constexpr uint32_t kPadUp = 0x00000010, kPadDown = 0x00000040, kPadLeft = 0x00000080,
                          kPadRight = 0x00000020, kPadCross = 0x00004000,
                          kPadCircle = 0x00002000, kPadTouchpad = 0x00100000;

struct MenuFonts {
  ImFont* text = nullptr;
  ImFont* title = nullptr;
};

// Sony's system font when the title can read it, else the drawer's Inter.
inline void AddSystemFonts(ImFontAtlas* atlas, MenuFonts& fonts) {
  const auto add = [atlas](const char* path) -> ImFont* {
    std::error_code error;
    return std::filesystem::exists(path, error) ? atlas->AddFontFromFileTTF(path, 32.0f)
                                                : nullptr;
  };
  fonts.text = add("/preinst/common/font/SST-Roman.otf");
  fonts.title = add("/preinst/common/font/SST-Medium.otf");
  if (!fonts.title) fonts.title = fonts.text;
  REXLOG_INFO("Settings menu font: {}", fonts.text ? "SST" : "Inter (no system font access)");
}

class SettingsMenu {
 public:
  SettingsMenu(rex::ui::WindowedAppContext& context, rex::ui::ImGuiDrawer& drawer,
               rex::ui::Presenter& presenter, const MenuFonts& fonts,
               std::filesystem::path settings_path)
      : context_(context), drawer_(drawer), presenter_(presenter), fonts_(fonts),
        settings_path_(std::move(settings_path)) {
    fsr_ = REXCVAR_GET(present_effect) == "fsr";
    high_resolution_ = REXCVAR_GET(resolution_scale) >= 2;
    running_high_resolution_ = high_resolution_;
    rumble_ = REXCVAR_GET(ps5_pad_rumble);
    if (REXCVAR_GET(ps5_show_fps)) SetFpsBadge(true);
    REXLOG_INFO("Settings menu: starting with fsr={} scale={} fps={} rumble={}", fsr_,
                REXCVAR_GET(resolution_scale), fps_badge_ != nullptr, rumble_);
  }

  void SetRumbleSink(std::function<void(bool)> sink) { set_rumble_ = std::move(sink); }

  // Pad thread: the raw scePad buttons of every read. True while the menu owns
  // the pad, including until the buttons that closed it are released.
  bool OnPadButtons(uint32_t buttons) {
    const uint32_t pressed = buttons & ~last_buttons_;
    last_buttons_ = buttons;
    if (pressed & kPadTouchpad) {
      const bool open = !open_.load();
      open_.store(open);
      swallow_until_release_ = true;
      context_.CallInUIThread([this, open] { open ? Show() : Hide(); });
      return true;
    }
    if (!open_.load()) {
      if (swallow_until_release_ && buttons) return true;
      swallow_until_release_ = false;
      return false;
    }
    swallow_until_release_ = true;
    if (pressed) context_.CallInUIThread([this, pressed] { OnPressed(pressed); });
    return true;
  }

 private:
  enum Item { kUpscaler, kResolution, kFps, kRumble, kClose, kItemCount };

  class MenuDialog : public rex::ui::ImGuiDialog {
   public:
    MenuDialog(rex::ui::ImGuiDrawer* drawer, SettingsMenu& menu)
        : ImGuiDialog(drawer), menu_(menu) {}

   protected:
    void OnDraw(ImGuiIO& io) override { menu_.Draw(io); }

   private:
    SettingsMenu& menu_;
  };

  class FpsBadge : public rex::ui::ImGuiDialog {
   public:
    FpsBadge(rex::ui::ImGuiDrawer* drawer, SettingsMenu& menu)
        : ImGuiDialog(drawer), menu_(menu) {}
    bool WantsContinuousRepaint() const override { return false; }

   protected:
    void OnDraw(ImGuiIO& io) override { menu_.DrawFps(io); }

   private:
    SettingsMenu& menu_;
  };

  // UI thread from here on.

  void Show() {
    if (!dialog_) dialog_ = std::make_unique<MenuDialog>(&drawer_, *this);
    selected_ = kUpscaler;
  }

  void Hide() {
    dialog_.reset();
    open_.store(false);
  }

  void OnPressed(uint32_t pressed) {
    if (!dialog_) return;
    if (pressed & kPadCircle) {
      Hide();
      return;
    }
    if (pressed & kPadUp) selected_ = (selected_ + kItemCount - 1) % kItemCount;
    if (pressed & kPadDown) selected_ = (selected_ + 1) % kItemCount;
    if (pressed & (kPadCross | kPadLeft | kPadRight)) Change(Item(selected_));
  }

  void Change(Item item) {
    switch (item) {
      case kUpscaler: {
        fsr_ = !fsr_;
        rex::cvar::SetFlagByName("present_effect", fsr_ ? "fsr" : "bilinear");
        auto config = presenter_.GetGuestOutputPaintConfigFromUIThread();
        config.SetEffect(fsr_ ? rex::ui::Presenter::GuestOutputPaintConfig::Effect::kFsr
                              : rex::ui::Presenter::GuestOutputPaintConfig::Effect::kBilinear);
        presenter_.SetGuestOutputPaintConfigFromUIThread(config);
        break;
      }
      case kResolution:
        // Render targets are sized at startup: saved for the next launch only.
        high_resolution_ = !high_resolution_;
        break;
      case kFps:
        SetFpsBadge(!fps_badge_);
        break;
      case kRumble:
        rumble_ = !rumble_;
        rex::cvar::SetFlagByName("ps5_pad_rumble", rumble_ ? "true" : "false");
        if (set_rumble_) set_rumble_(rumble_);
        break;
      case kClose:
        Hide();
        return;
      default:
        return;
    }
    Save();
  }

  void SetFpsBadge(bool shown) {
    if (shown && !fps_badge_) fps_badge_ = std::make_unique<FpsBadge>(&drawer_, *this);
    if (!shown) fps_badge_.reset();
    presenter_.SetGuestFrameStatsEnabled(shown);
  }

  // Flat `name = value` lines, loaded at startup before ps5.toml.
  void Save() {
    const int scale = high_resolution_ ? 2 : 1;
    const std::filesystem::path temporary = settings_path_.string() + ".tmp";
    FILE* file = std::fopen(temporary.c_str(), "w");
    if (!file) {
      REXLOG_WARN("Settings menu: cannot write {}", temporary.string());
      return;
    }
    std::fprintf(file,
                 "present_effect = \"%s\"\nresolution_scale = %d\n"
                 "draw_resolution_scale_x = %d\ndraw_resolution_scale_y = %d\n"
                 "ps5_show_fps = %s\nps5_pad_rumble = %s\n",
                 fsr_ ? "fsr" : "bilinear", scale, scale, scale, fps_badge_ ? "true" : "false",
                 rumble_ ? "true" : "false");
    std::fclose(file);
    std::error_code error;
    std::filesystem::rename(temporary, settings_path_, error);
    REXLOG_INFO("Settings menu: saved fsr={} scale={} fps={} rumble={} to {} ({})", fsr_, scale,
                fps_badge_ != nullptr, rumble_, settings_path_.string(),
                error ? error.message() : "ok");
  }

  ImFont* TextFont() const { return fonts_.text ? fonts_.text : drawer_.ui_font(); }
  ImFont* TitleFont() const { return fonts_.title ? fonts_.title : drawer_.ui_font_semibold(); }

  // Layout in 1080p units, scaled to the display.
  void Draw(ImGuiIO& io) {
    const float s = io.DisplaySize.y / 1080.0f;
    const ImU32 accent = IM_COL32(224, 54, 44, 255);
    ImDrawList* background = ImGui::GetBackgroundDrawList();
    background->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 110));

    const ImVec2 size(680 * s, 470 * s);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(size);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(16, 18, 24, 240));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 18 * s);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(36 * s, 30 * s));
    if (ImGui::Begin("##ps5_settings", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                         ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings)) {
      ImDrawList* draw = ImGui::GetWindowDrawList();
      const ImVec2 origin = ImGui::GetWindowPos();
      const float inner = size.x - 72 * s;

      ImGui::PushFont(TextFont(), 17 * s);
      ImGui::TextColored(ImVec4(1, 1, 1, 0.45f), "TABLE TENNIS RECOMPILED");
      ImGui::PopFont();
      ImGui::PushFont(TitleFont(), 38 * s);
      ImGui::TextUnformatted("Settings");
      ImGui::PopFont();
      ImGui::Dummy(ImVec2(0, 14 * s));

      const std::array<std::pair<const char*, std::string>, kItemCount> rows = {{
          {"Upscaling", fsr_ ? "FSR 1" : "Bilinear"},
          {"Render resolution", std::string(high_resolution_ ? "1440p" : "720p") +
                                    (high_resolution_ != running_high_resolution_
                                         ? "  (next launch)"
                                         : "")},
          {"FPS counter", fps_badge_ ? "On" : "Off"},
          {"Controller rumble", rumble_ ? "On" : "Off"},
          {"Close", ""},
      }};
      const float row_height = 54 * s;
      ImGui::PushFont(TextFont(), 24 * s);
      for (int i = 0; i < kItemCount; ++i) {
        const ImVec2 row = ImGui::GetCursorScreenPos();
        const bool selected = i == selected_;
        if (selected) {
          draw->AddRectFilled(row, ImVec2(row.x + inner, row.y + row_height),
                              IM_COL32(255, 255, 255, 24), 10 * s);
          draw->AddRectFilled(row, ImVec2(row.x + 5 * s, row.y + row_height), accent, 3 * s);
        }
        const float text_y = row.y + (row_height - ImGui::GetFontSize()) * 0.5f;
        const ImU32 label_color = selected ? IM_COL32(255, 255, 255, 255)
                                           : IM_COL32(255, 255, 255, 170);
        draw->AddText(ImVec2(row.x + 22 * s, text_y), label_color, rows[i].first);
        if (!rows[i].second.empty()) {
          const std::string value =
              selected ? "<  " + rows[i].second + "  >" : rows[i].second;
          const float width = ImGui::CalcTextSize(value.c_str()).x;
          draw->AddText(ImVec2(row.x + inner - 22 * s - width, text_y),
                        selected ? accent : IM_COL32(255, 255, 255, 200), value.c_str());
        }
        ImGui::Dummy(ImVec2(inner, row_height));
      }
      ImGui::PopFont();

      ImGui::PushFont(TextFont(), 17 * s);
      const char* hint = "Cross  change      Circle  close";
      draw->AddText(ImVec2(origin.x + 36 * s, origin.y + size.y - 46 * s),
                    IM_COL32(255, 255, 255, 110), hint);
      ImGui::PopFont();
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor();
  }

  void DrawFps(ImGuiIO& io) {
    const float s = io.DisplaySize.y / 1080.0f;
    const auto stats = presenter_.GetGuestFrameStats();
    char text[32];
    if (stats.frame_count > 0 && stats.fps > 0.0) {
      std::snprintf(text, sizeof text, "%.0f FPS", stats.fps);
    } else {
      std::snprintf(text, sizeof text, "-- FPS");
    }
    ImGui::PushFont(TitleFont(), 22 * s);
    const ImVec2 text_size = ImGui::CalcTextSize(text);
    const ImVec2 pad(14 * s, 8 * s);
    const ImVec2 max(io.DisplaySize.x - 28 * s, 28 * s + text_size.y + pad.y * 2);
    const ImVec2 min(max.x - text_size.x - pad.x * 2, 28 * s);
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    draw->AddRectFilled(min, max, IM_COL32(16, 18, 24, 190), 8 * s);
    draw->AddText(ImVec2(min.x + pad.x, min.y + pad.y), IM_COL32(255, 255, 255, 235), text);
    ImGui::PopFont();
  }

  rex::ui::WindowedAppContext& context_;
  rex::ui::ImGuiDrawer& drawer_;
  rex::ui::Presenter& presenter_;
  // Filled by the drawer's font setup, which may run after construction.
  const MenuFonts& fonts_;
  std::filesystem::path settings_path_;
  std::function<void(bool)> set_rumble_;

  // Pad thread.
  std::atomic<bool> open_{false};
  uint32_t last_buttons_ = 0;
  bool swallow_until_release_ = false;

  // UI thread.
  std::unique_ptr<MenuDialog> dialog_;
  std::unique_ptr<FpsBadge> fps_badge_;
  int selected_ = kUpscaler;
  bool fsr_ = true;
  bool high_resolution_ = true;
  bool running_high_resolution_ = true;
  bool rumble_ = true;
};

}  // namespace tabletennis::ps5
