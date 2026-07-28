#pragma once

#include <cstdint>
#include <memory>

namespace rex::graphics {
struct NativeGuestOutputRenderContext;
}

namespace tabletennis::native {

struct VenueE33FrameSnapshot;

// Bounded comparison renderer. It consumes only immutable E33 snapshots that
// survived the bidirectional title/backend proof, draws over guest output, and
// has no route capable of suppressing or replacing guest work.
bool VenueE33RendererEnabled();

bool PrepareVenueE33Observer(
    const rex::graphics::NativeGuestOutputRenderContext& context,
    const std::shared_ptr<const VenueE33FrameSnapshot>& frame);

uint32_t RenderVenueE33Observer(
    const rex::graphics::NativeGuestOutputRenderContext& context,
    const std::shared_ptr<const VenueE33FrameSnapshot>& frame);

void ShutdownVenueE33Renderer();

}  // namespace tabletennis::native
