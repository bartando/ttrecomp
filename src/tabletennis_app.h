// tabletennis - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <rex/graphics/graphics_system.h>
#include <rex/logging.h>
#include <rex/rex_app.h>
#include <rex/ui/keybinds.h>

#include "generated/default/tabletennis_init.h"
#include "native/tabletennis_native_renderer.h"
#include "tabletennis_defaults.h"
#include "tabletennis_iso_installer.h"
#include "test/tabletennis_frontend_launch_test.h"

#if defined(__APPLE__)
namespace tabletennis {
void SetDockIconMacOS();
}
#endif

class TabletennisApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    tabletennis::ApplyAppDefaults();
    return std::unique_ptr<TabletennisApp>(new TabletennisApp(ctx, "tabletennis",
        tabletennis_PPCImageConfig));
  }

  std::optional<rex::PathConfig> OnFinalizePaths(
      const rex::PathConfig& defaults,
      std::function<void(rex::PathConfig)> resume) override {
#if defined(__APPLE__)
    tabletennis::SetDockIconMacOS();
#endif
    return tabletennis::FinalizeGamePaths(defaults, imgui_drawer(), std::move(resume));
  }

  void OnPostSetup() override {
    tabletennis::native::Install();
    tabletennis::test::InstallFrontendLaunchTest();
    tabletennis::test::SetGameplayTraceRequester([this] {
      if (!runtime() || !runtime()->graphics_system()) {
        REXLOG_ERROR(
            "Gameplay GPU frame trace requested before runtime setup");
        return;
      }
      auto* graphics_system =
          static_cast<rex::graphics::GraphicsSystem*>(
              runtime()->graphics_system());
      graphics_system->RequestFrameTrace();
      REXLOG_INFO(
          "Gameplay GPU frame trace requested from verified title marker");
    });
  }

  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    (void)drawer;
    rex::ui::RegisterBind(
        "bind_tabletennis_frame_trace", "F7", "Capture one GPU frame", [this] {
          if (!runtime() || !runtime()->graphics_system()) {
            REXLOG_WARN("GPU frame trace requested before the runtime was ready");
            return;
          }
          auto* graphics_system =
              static_cast<rex::graphics::GraphicsSystem*>(runtime()->graphics_system());
          graphics_system->RequestFrameTrace();
          REXLOG_INFO("GPU frame trace requested; capturing the next complete frame");
        });
    rex::ui::RegisterBind(
        "bind_tabletennis_native_renderer", "F5",
        "Toggle native/emulated renderer", [] {
          tabletennis::native::Toggle();
        });
  }

  void OnShutdown() override {
    tabletennis::test::SetGameplayTraceRequester({});
    tabletennis::test::ShutdownFrontendLaunchTest();
    tabletennis::native::Shutdown();
    rex::ui::UnregisterBind("bind_tabletennis_native_renderer");
    rex::ui::UnregisterBind("bind_tabletennis_frame_trace");
  }

  // Override virtual hooks for customization:
  // void OnPostInitLogging() override {}
  // void OnPreSetup(rex::RuntimeConfig& config) override {}
  // void OnLoadXexImage(std::string& xex_image) override {}
  // void OnPostSetup() override {}
  // void OnConfigurePaths(rex::PathConfig& paths) override {}
};
