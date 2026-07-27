#pragma once

#include <rex/graphics/native_guest_renderer.h>

namespace tabletennis::native {

// Benchmark-only C6 suppression. This deliberately records no native draw and
// exists solely to measure how much of the guest GPU cost belongs to the crowd
// family while the real observer overlay remains available for visual checks.
bool MatchCrowdDiagnosticSuppression(
    const rex::graphics::NativeGuestDrawContext& context,
    void* user_data);
bool RenderCrowdDiagnosticSuppression(
    const rex::graphics::NativeGuestDrawContext& context,
    void* user_data);

}  // namespace tabletennis::native
