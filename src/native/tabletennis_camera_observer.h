#pragma once

#include "native/tabletennis_camera.h"

#include <cstdint>

namespace tabletennis::native {

// Observe the authoritative completed constant block at sub_82152E80 EXIT.
// Only the render context currently stored in the title's main-context global
// is kept.
void ObserveRenderContextCamera(uint8_t* guest_base, uint32_t render_context);

// Cross-check the later table draw's staged constants against the latest
// authoritative render-context snapshot. A successful comparison stamps the
// draw camera with the producer context that proved it.
bool VerifyDrawCameraAgainstRenderContext(uint8_t* guest_base,
                                          CapturedCamera& draw_camera);

}  // namespace tabletennis::native
