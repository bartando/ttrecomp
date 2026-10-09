// Settings Table Tennis needs to run correctly, baked in so a release works
// without a tabletennis.toml next to the app.

#pragma once

#include <filesystem>

namespace tabletennis {

// Replaces the SDK defaults, so command-line flags and a tabletennis.toml
// (loaded later) still win, and saved settings equal to these stay saved.
void ApplyAppDefaults();

// Gives this install its own profile XUID unless one is configured. System
// link peers must differ, and every install used to share the SDK default.
// The XUID is kept in user_data_root, and saves made under the old shared
// XUID are copied over the first time. Call once the user data root is
// known and before the runtime creates the profile.
void EnsureInstallXuid(const std::filesystem::path& user_data_root);

}  // namespace tabletennis
