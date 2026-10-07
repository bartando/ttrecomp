// Installs the game files from the user's own Xbox 360 disc image. The
// project ships no game data, so a first launch without them shows a wizard
// that extracts the ISO next to the app.

#pragma once

#include <filesystem>
#include <functional>
#include <optional>

#include <rex/rex_app.h>

namespace rex::ui {
class ImGuiDrawer;
}

namespace tabletennis {

bool IsGameInstalled(const std::filesystem::path& game_root);

// Implements ReXApp::OnFinalizePaths: returns the paths right away when the
// game is installed, otherwise shows the install wizard and calls `resume`
// once it's done. The TABLETENNIS_INSTALL_ISO environment variable installs
// from that ISO without any UI.
std::optional<rex::PathConfig> FinalizeGamePaths(const rex::PathConfig& defaults,
                                                 rex::ui::ImGuiDrawer* drawer,
                                                 std::function<void(rex::PathConfig)> resume);

}  // namespace tabletennis
