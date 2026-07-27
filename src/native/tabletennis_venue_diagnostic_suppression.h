#pragma once

#include <rex/graphics/native_guest_renderer.h>

namespace tabletennis::native {

// Benchmark-only suppression for the exact PS328 venue shader family. This
// records no replacement draw and must never be enabled during normal play.
bool MatchVenueDiagnosticSuppression(
    const rex::graphics::NativeGuestDrawContext& context, void* user_data);
bool RenderVenueDiagnosticSuppression(
    const rex::graphics::NativeGuestDrawContext& context, void* user_data);

}  // namespace tabletennis::native
