#pragma once

#include "native/tabletennis_camera.h"

#include <cstdint>

namespace tabletennis::native {

// Bracket one title-side grmModelGeom submission. Every submission is pushed,
// including non-table submissions, so nested rendering cannot inherit an
// outer table association accidentally.
void BeginModelDrawScope(bool observed_table_model, uint32_t model,
                         uint32_t shader_group);
void EndModelDrawScope();

// Returns the shader group only while the caller is inside the exact observed
// table-model submission for `model`. Deep/overflowed nesting never inherits
// an outer table scope.
bool CurrentObservedTableModelDraw(uint32_t model, uint32_t& shader_group);

// Observer-only snapshot of the title's final vertex constant shadow bank at
// the indexed draw call. The original draw always runs unchanged.
void ObserveIndexedDrawConstants(uint8_t* guest_base, uint32_t device,
                                 uint32_t primitive_type,
                                 uint32_t submitted_index_count);

// Called at the title's swap boundary. Returns the one camera snapshot that
// passed all matrix cross-checks during this frame and arms the next frame.
CapturedCamera ConsumeObservedCamera();

}  // namespace tabletennis::native
