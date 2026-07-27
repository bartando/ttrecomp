#pragma once

namespace rex::graphics {
struct NativeGuestOutputRenderContext;
}

namespace tabletennis::native {

// Records the currently proven native families into one private offscreen
// scene in the title's original draw order. The pass is always discarded:
// this is an observer-side transaction proof, never native output serving.
bool NativeSceneTransactionObserverEnabled();
void ObserveNativeSceneTransaction(
    const rex::graphics::NativeGuestOutputRenderContext &context);

} // namespace tabletennis::native
