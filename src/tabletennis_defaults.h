// Settings Table Tennis needs to run correctly, baked in so a release works
// without a tabletennis.toml next to the app.

#pragma once

namespace tabletennis {

// Replaces the SDK defaults, so command-line flags and a tabletennis.toml
// (loaded later) still win, and saved settings equal to these stay saved.
void ApplyAppDefaults();

}  // namespace tabletennis
