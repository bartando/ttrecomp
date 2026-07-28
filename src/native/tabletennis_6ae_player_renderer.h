#pragma once

#include <cstdint>
#include <memory>

namespace rex::graphics {
struct NativeGuestOutputRenderContext;
}

namespace tabletennis::native {

struct Player6AEFrameSnapshot;

// Comparison overlay only. Capture remains owned by the separate 6AE
// observer; this renderer consumes one immutable published geometry group.
bool Player6AEObserverOverlayEnabled();
bool PreparePlayer6AEObserverOverlay(
    const rex::graphics::NativeGuestOutputRenderContext &context,
    const std::shared_ptr<const Player6AEFrameSnapshot> &frame);
uint32_t RenderPlayer6AEObserverOverlay(
    const rex::graphics::NativeGuestOutputRenderContext &context,
    const std::shared_ptr<const Player6AEFrameSnapshot> &frame);
void ShutdownPlayer6AEObserverRenderer();

} // namespace tabletennis::native
