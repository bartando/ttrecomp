#pragma once

namespace rex::graphics {
struct NativeGuestDrawContext;
}

namespace tabletennis::native {

// Default-off observer route for the next C6 replacement step. A successful
// match only opens the exact borrowed guest scope and resolves the native
// PSO/descriptors. The renderer always returns false, so the guest draw stays
// authoritative.
bool CrowdReplacementPrewarmEnabled();

// Single source of truth for the live late borrowed C6 contract. Both route
// matching and renderer preflight fail closed through this predicate.
bool MatchesCrowdReplacementPrewarmContract(
    const rex::graphics::NativeGuestDrawContext& context);

bool MatchCrowdReplacementPrewarm(
    const rex::graphics::NativeGuestDrawContext& context, void* user_data);
bool RenderCrowdReplacementPrewarm(
    const rex::graphics::NativeGuestDrawContext& context, void* user_data);

}  // namespace tabletennis::native
