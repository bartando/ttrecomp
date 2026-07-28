#pragma once

#include "native/tabletennis_scene_owner_observer.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace tabletennis::native {

// Identity selected by grmShaderFx::ApplyPass. All addresses are guest
// addresses and are observer data only.
struct SceneCatalogPassIdentity {
  uint32_t runtime_state = 0;
  uint32_t pass_descriptor = 0;
  uint32_t program_pair = 0;
  uint32_t vertex_shader_reference = 0;
  uint32_t vertex_shader = 0;
  uint32_t vertex_shader_ucode_bytes = 0;
  uint64_t vertex_shader_hash = 0;
  uint32_t pixel_shader_reference = 0;
  uint32_t pixel_shader = 0;
  uint32_t pixel_shader_ucode_bytes = 0;
  uint64_t pixel_shader_hash = 0;
  uint32_t guest_read_failures = 0;
  bool shader_fingerprints_valid = false;
  bool valid = false;
};

// One unique grmShaderFx::DrawModelGeometry scope observed during a frame.
struct SceneCatalogScopeSignature {
  uint32_t shader = 0;
  uint32_t model = 0;
  uint32_t geometry_index = 0;
  uint32_t lod = 0;
  uint32_t occurrence_count = 0;
  bool alternate_pass = false;
};

// Draw-time mesh binding selected by grmModelGeom immediately before the
// indexed submission. This is the live selection, not a guessed model walk.
struct SceneCatalogMeshIdentity {
  uint32_t vertex_aggregate = 0;
  uint32_t vertex_declaration = 0;
  uint32_t stream_selector = 0;
  uint32_t primary_vertex_stream = 0;
  uint32_t secondary_vertex_stream = 0;
  uint32_t vertex_buffer_resource = 0;
  uint32_t vertex_buffer_alias = 0;
  uint32_t vertex_buffer_bytes = 0;
  uint32_t vertex_stride = 0;
  uint32_t index_buffer_wrapper = 0;
  uint32_t index_buffer_resource = 0;
  uint32_t index_buffer_alias = 0;
  uint32_t index_buffer_bytes = 0;
  uint32_t index_element_count = 0;
  uint32_t index_element_size = 0;
  uint32_t index_data = 0;
  uint16_t aggregate_primitive_type = 0;
  uint16_t aggregate_index_count = 0;
  uint8_t vertex_fetch_type = 0;
  uint8_t vertex_endian = 0;
  bool index_is_32_bit = false;
  bool alternate_primary_stream = false;
  bool global_stream_selector = false;
  uint32_t guest_read_failures = 0;
  bool valid = false;
};

// Immutable device state sampled immediately before DrawIndexedPrimitive.
// These are values, never retained guest pointers: later draws overwrite the
// same device banks.
struct SceneCatalogDrawState {
  uint32_t vertex_declaration = 0;
  std::array<std::array<uint32_t, 6>, 2> texture_fetches{};
  std::array<float, 28> vertex_constants_0_6{};
  std::array<float, 16> vertex_constants_12_15{};
  std::array<float, 4> pixel_constant_20{};
  std::array<float, 4> pixel_constant_46{};
  std::array<float, 4> pixel_constant_254{};
  std::array<float, 4> pixel_constant_255{};
  uint32_t guest_read_failures = 0;
  bool valid = false;
};

// Actual title device shader objects observed at the low-level bind
// postconditions. ApplyPass metadata can be stale when a later COMP path
// rebinds shaders outside the grmShaderFx scope, so draw-family proofs that
// need current shader identity must use this state.
struct SceneCatalogBoundShaderState {
  uint32_t device = 0;
  uint32_t vertex_shader = 0;
  uint32_t vertex_shader_ucode_bytes = 0;
  uint64_t vertex_shader_hash = 0;
  uint32_t pixel_shader = 0;
  uint32_t pixel_shader_ucode_bytes = 0;
  uint64_t pixel_shader_hash = 0;
  bool vertex_shader_valid = false;
  bool pixel_shader_valid = false;
};

// One authoritative ordered DrawIndexedPrimitive occurrence. Each record owns
// its own draw-time constants; deduplicated signatures below are summaries
// only and must never replace this ledger.
struct SceneCatalogDrawOccurrence {
  // Authoritative title frame being built when this draw was observed. Family
  // observers must use this value rather than maintaining parallel counters
  // that can diverge when capture is hot-enabled.
  uint64_t frame_sequence = 0;
  uint32_t ordinal = 0;
  // Exact semantic owner from a synchronous title render callback. This is
  // independent of player ownership because a paddle is submitted below a
  // pongPlayer draw while retaining its own renderable identity.
  SceneOwnerToken owner{};
  // Exact owner from the synchronous pongPlayer render scope. Zero means the
  // draw did not originate below pongPlayer_Render.
  uint32_t player = 0;
  bool scope_valid = false;
  SceneCatalogScopeSignature scope{};
  SceneCatalogPassIdentity pass{};
  SceneCatalogMeshIdentity mesh{};
  SceneCatalogDrawState state{};
  SceneCatalogBoundShaderState bound_shaders{};
  uint32_t device = 0;
  uint32_t primitive_type = 0;
  uint32_t submitted_index_count = 0;
  std::array<float, 16> world{};
  std::array<float, 16> world_view_projection{};
  uint64_t world_hash = 0;
  uint64_t world_view_projection_hash = 0;
  bool world_valid = false;
  bool world_view_projection_valid = false;
};

// Deduplicated logging summary for the ordered records above.
struct SceneCatalogDrawSignature {
  SceneOwnerToken owner{};
  uint32_t player = 0;
  bool scope_valid = false;
  SceneCatalogScopeSignature scope{};
  SceneCatalogPassIdentity pass{};
  SceneCatalogMeshIdentity mesh{};
  uint32_t device = 0;
  uint32_t primitive_type = 0;
  uint32_t submitted_index_count = 0;
  uint32_t first_ordinal = 0;
  uint32_t occurrence_count = 0;
  uint64_t world_hash = 0;
  uint64_t world_view_projection_hash = 0;
};

struct SceneDrawCatalogFrame {
  static constexpr size_t kMaxUniqueScopes = 128;
  static constexpr size_t kMaxUniquePasses = 128;
  static constexpr size_t kMaxUniqueDraws = 256;
  static constexpr size_t kMaxOrderedDraws = 2048;

  uint64_t sequence = 0;
  uint32_t model_geometry_scope_count = 0;
  uint32_t unique_scope_count = 0;
  uint32_t dropped_unique_scopes = 0;
  uint32_t pass_apply_count = 0;
  uint32_t unique_pass_count = 0;
  uint32_t dropped_unique_passes = 0;
  uint32_t total_indexed_hook_count = 0;
  uint32_t scoped_indexed_draw_count = 0;
  uint32_t unscoped_indexed_draw_count = 0;
  uint32_t player_indexed_draw_count = 0;
  uint32_t ordered_draw_count = 0;
  uint32_t dropped_ordered_draws = 0;
  uint32_t unique_draw_count = 0;
  uint32_t dropped_unique_draws = 0;
  uint32_t guest_read_failures = 0;
  std::array<SceneCatalogScopeSignature, kMaxUniqueScopes> unique_scopes{};
  std::array<SceneCatalogPassIdentity, kMaxUniquePasses> unique_passes{};
  std::array<SceneCatalogDrawOccurrence, kMaxOrderedDraws> ordered_draws{};
  std::array<SceneCatalogDrawSignature, kMaxUniqueDraws> unique_draws{};
};

// Bracket every grmShaderFx::DrawModelGeometry call. The catalog is generic:
// unlike the table material observer, it deliberately does not filter model,
// shader, geometry, or pass identity.
void BeginSceneDrawCatalogScope(uint32_t shader, uint32_t model,
                                uint32_t geometry_index, uint32_t lod,
                                bool alternate_pass);
void EndSceneDrawCatalogScope();

// pongPlayer's embedded renderable is at player+8. Bracketing its render
// callback gives every downstream model/material/draw hook an exact player
// owner without matching addresses heuristically after the fact.
void BeginSceneDrawCatalogPlayerScope(uint32_t player);
void EndSceneDrawCatalogPlayerScope();

// Observe the exact pass and indexed-draw hooks while the scope is active.
// Guest reads use the same fault-recovery layer as the existing observers.
void ObserveSceneDrawCatalogPass(uint8_t *guest_base, uint32_t runtime_state,
                                 uint32_t pass_descriptor);

// Low-level shader binding postconditions. These track the real shader objects
// active on the title device even when no ApplyPass scope is active.
void ObserveSceneDrawCatalogBoundVertexShader(uint8_t *guest_base,
                                              uint32_t device,
                                              uint32_t shader);
void ObserveSceneDrawCatalogBoundPixelShader(uint8_t *guest_base,
                                             uint32_t device,
                                             uint32_t shader);

// Bracket the two proven grmModelGeom draw helpers. The direct helper receives
// its selector at entry; the global helper reads the live selector used by the
// original routine. Their selected binding remains active through the nested
// DrawIndexedPrimitive call.
void BeginSceneDrawCatalogMeshSelection(uint8_t *guest_base,
                                        uint32_t vertex_aggregate,
                                        uint32_t stream_selector,
                                        uint32_t secondary_vertex_stream,
                                        bool alternate_primary_stream);
void BeginSceneDrawCatalogGlobalMeshSelection(uint8_t *guest_base,
                                              uint32_t vertex_aggregate);
void EndSceneDrawCatalogMeshSelection();

void ObserveSceneDrawCatalogIndexedDraw(uint8_t *guest_base, uint32_t device,
                                        uint32_t primitive_type,
                                        uint32_t submitted_index_count);

// Publish at the title swap boundary and start a fresh bounded frame.
void SceneDrawCatalogFrameEnd();
SceneDrawCatalogFrame LatestSceneDrawCatalogFrame();
std::shared_ptr<const SceneDrawCatalogFrame>
LatestSceneDrawCatalogFrameSnapshot();

} // namespace tabletennis::native
