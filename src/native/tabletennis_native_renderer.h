#pragma once

namespace tabletennis::native {

// Registers the native guest-output callback. The renderer starts disabled so
// the frontend remains on the complete emulated path until gameplay detection
// is wired from the title's captured render state.
void Install();
void Shutdown();

bool Enabled();
void Toggle();

}  // namespace tabletennis::native
