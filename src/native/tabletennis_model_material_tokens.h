#pragma once

#include "native/tabletennis_scene_owner_observer.h"

#include <array>
#include <cstdint>

namespace tabletennis::native {

// Value-only identity selected by grmModelGeom::Draw for one geometry. No
// guest pointer is dereferenced after publication.
struct ModelMaterialValueToken {
  SceneOwnerToken owner{};
  uint32_t model = 0;
  uint32_t geometry_index = 0;
  uint32_t shader = 0;
  uint32_t shader_vtable = 0;
  uint32_t shader_slot_target = 0;
  uint32_t render_category = 0;
  uint32_t lod = 0;
  bool shader_slot_target_valid = false;
  bool global_shader_override = false;
};

constexpr uint32_t kMaxModelMaterialValueTokens = 2048;

struct ModelMaterialValueTokenFrame {
  uint64_t sequence = 0;
  uint32_t submission_count = 0;
  uint32_t readable_submission_count = 0;
  uint32_t geometry_count = 0;
  uint32_t scanned_geometry_count = 0;
  uint32_t category_rejected_count = 0;
  uint32_t token_count = 0;
  uint32_t valid_slot_target_count = 0;
  uint32_t global_override_submission_count = 0;
  uint32_t dropped_geometry_count = 0;
  uint32_t dropped_token_count = 0;
  uint32_t guest_read_failure_count = 0;
  uint32_t unique_model_count = 0;
  uint32_t unique_shader_count = 0;
  uint32_t unique_shader_vtable_count = 0;
  uint32_t unique_slot_target_count = 0;
  std::array<ModelMaterialValueToken, kMaxModelMaterialValueTokens> tokens{};
};

// Observer-only hook for grmModelGeom::Draw (sub_820F19C8). The title's
// material-index selection is decoded before the original routine executes.
// All guest reads are fault guarded and all retained state is bounded.
void ObserveModelMaterialValueTokens(uint8_t *guest_base, uint32_t model,
                                     uint32_t shader_group,
                                     uint32_t render_category, uint32_t lod);

// Publishes the bounded value-token frame at the title swap boundary.
void ModelMaterialValueTokensFrameEnd();
ModelMaterialValueTokenFrame LatestModelMaterialValueTokenFrame();

} // namespace tabletennis::native
