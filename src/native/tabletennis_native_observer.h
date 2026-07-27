#pragma once

#include <cstdint>

namespace tabletennis::native {

// Observe title-owned renderables without changing guest state or deciding
// what the native renderer should serve.
void ObserveDrawBucketEntry(uint8_t* guest_base, uint32_t renderable,
                            uint32_t vtable);

// Observe a grmModelGeom submission and associate it with the most recently
// observed lvlTable only when its shader-group pointer matches the table's
// live rmcDrawable chain. This records telemetry; it never serves or suppresses
// a guest draw.
// Returns true only when this submission was proven to belong to the observed
// lvlTable through the live shader-group pointer chain.
bool ObserveModelGeometrySubmission(uint8_t* guest_base, uint32_t model,
                                    uint32_t shader_group,
                                    uint32_t render_category, uint32_t lod);
void ObserverFrameEnd();

}  // namespace tabletennis::native
