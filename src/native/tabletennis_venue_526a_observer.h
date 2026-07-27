#pragma once

#include "native/tabletennis_scene_owner_observer.h"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace rex::graphics {
struct NativeGuestDrawContext;
}

namespace tabletennis::native {

struct SceneCatalogDrawOccurrence;

struct Venue526ADrawIdentity {
  uint32_t primitive_type = 0;
  uint32_t submitted_index_count = 0;
  uint32_t guest_index_base = 0;
  uint64_t vertex_shader_hash = 0;
  uint64_t pixel_shader_hash = 0;

  bool valid() const {
    return primitive_type != 0 && submitted_index_count != 0 &&
           guest_index_base != 0 && vertex_shader_hash != 0 &&
           pixel_shader_hash != 0;
  }
  bool operator==(const Venue526ADrawIdentity &) const = default;
};

struct Venue526ABackendContract {
  uint32_t render_pass_key = 0;
  uint32_t surface_pitch = 0;
  uint32_t normalized_depth_control = 0;
  uint32_t normalized_color_mask = 0;
  uint32_t color_control = 0;
  uint32_t blend_control_0 = 0;
  uint32_t rasterizer_mode_control = 0;
  uint32_t primitive_restart_index = 0;
  std::array<uint32_t, 4> color_attachment_formats{};
  uint32_t color_attachment_count = 0;
  uint32_t depth_attachment_format = 0;
  uint32_t stencil_attachment_format = 0;
  uint32_t sample_count = 0;
  uint64_t sample_mask = 0;
  bool primitive_restart_enabled = false;
  bool valid = false;

  bool operator==(const Venue526ABackendContract &) const = default;
};

// Value-only title contract sampled synchronously at the real
// grmShaderFx::DrawModelGeometry -> grmModelGeom indexed submission.
struct Venue526ATitleContract {
  uint64_t sequence = 0;
  uint32_t ordinal = 0;
  SceneOwnerToken owner{};
  uint32_t material_shader = 0;
  uint32_t material_shader_vtable = 0;
  uint32_t model = 0;
  uint32_t geometry_index = 0;
  uint32_t lod = 0;
  bool alternate_pass = false;
  uint32_t pass_descriptor = 0;
  uint32_t program_pair = 0;
  uint32_t title_vertex_shader = 0;
  uint32_t title_pixel_shader = 0;
  uint64_t title_pixel_shader_hash = 0;
  uint32_t vertex_aggregate = 0;
  uint32_t vertex_declaration = 0;
  uint32_t vertex_buffer_alias = 0;
  uint32_t vertex_buffer_bytes = 0;
  uint32_t vertex_stride = 0;
  uint32_t index_buffer_alias = 0;
  uint32_t index_buffer_bytes = 0;
  uint32_t index_element_size = 0;
  std::array<uint32_t, 6> texture_fetch_0{};
  std::array<float, 16> world{};
  std::array<float, 16> world_view_projection{};
  bool valid = false;
};

struct Venue526ADrawSnapshot {
  Venue526ADrawIdentity backend_identity{};
  Venue526ABackendContract backend{};
  Venue526ATitleContract title{};

  bool valid() const {
    return backend_identity.valid() && backend.valid && title.valid;
  }
};

struct Venue526AFrameSnapshot {
  uint64_t sequence = 0;
  uint64_t backend_frame_sequence = 0;
  uint32_t title_candidate_count = 0;
  uint32_t logical_draw_count = 0;
  uint32_t logical_index_count = 0;
  uint32_t backend_event_count = 0;
  uint32_t backend_tile_blocks_matched = 0;
  uint32_t guest_read_failures = 0;
  uint32_t sequence_mismatches = 0;
  std::vector<Venue526ADrawSnapshot> draws;
  bool valid = false;
};

struct Venue526AObserverTelemetry {
  uint64_t title_draws_observed = 0;
  uint64_t title_pixel_hash_matches = 0;
  uint64_t title_structural_matches = 0;
  uint64_t title_candidates = 0;
  uint64_t title_guest_read_failures = 0;
  uint64_t backend_draws_observed = 0;
  uint64_t backend_hash_matches = 0;
  uint64_t backend_contract_matches = 0;
  uint64_t backend_events = 0;
  uint64_t backend_frames_analyzed = 0;
  uint64_t backend_tile_blocks_matched = 0;
  uint64_t finalized_frames = 0;
  uint64_t valid_frames = 0;
  uint64_t rejected_frames = 0;
  uint64_t expired_frames = 0;
  uint64_t latest_published_sequence = 0;
  uint32_t pending_frames = 0;
};

bool Venue526AObserverEnabled();

// Observer-only taps. Neither function returns a replacement route or
// suppresses a guest draw.
void ObserveVenue526ATitleDraw(uint8_t *guest_base,
                               const SceneCatalogDrawOccurrence &draw);
void ObserveVenue526ABackendDraw(
    const rex::graphics::NativeGuestDrawContext &context);
void Venue526AObserverFrameEnd();

std::shared_ptr<const Venue526AFrameSnapshot>
LatestVenue526AFrameSnapshot();
Venue526AObserverTelemetry LatestVenue526AObserverTelemetry();

} // namespace tabletennis::native
