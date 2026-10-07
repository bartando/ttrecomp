// Settings Table Tennis needs to run correctly, baked in so a release works
// without a tabletennis.toml next to the app.

#pragma once

namespace tabletennis {

// Applies the defaults to every cvar still at its SDK default, so command-line
// flags win over them, and a tabletennis.toml (loaded later) wins over both.
void ApplyAppDefaults();

}  // namespace tabletennis
