#pragma once

#include "native/tabletennis_venue_e33_snapshot.h"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace rex::graphics {
struct NativeGuestDrawContext;
}

namespace tabletennis::native {

struct SceneCatalogDrawOccurrence;

struct VenueE33BackendContract {
  uint64_t vertex_shader_hash = 0;
  uint64_t pixel_shader_hash = 0;
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
  bool rasterizer_mode_control_valid = false;
  bool valid = false;
};

struct VenueE33LearnedIdentitySnapshot {
  uint64_t generation = 0;
  uint64_t proof_sequence = 0;
  std::vector<VenueE33TitleProgramIdentity> ordered_draw_programs;
  std::vector<VenueE33VertexDeclarationIdentity> ordered_vertex_declarations;
  uint32_t unique_program_count = 0;

  bool valid() const {
    if (generation == 0 || proof_sequence == 0 || unique_program_count == 0 ||
        ordered_draw_programs.empty() ||
        ordered_draw_programs.size() != ordered_vertex_declarations.size() ||
        unique_program_count > ordered_draw_programs.size()) {
      return false;
    }
    for (size_t index = 0; index < ordered_draw_programs.size(); ++index) {
      if (!ordered_draw_programs[index].valid() ||
          !ordered_vertex_declarations[index].valid()) {
        return false;
      }
    }
    return true;
  }
};

struct VenueE33DrawSnapshot {
  std::shared_ptr<const VenueE33TitleDrawSnapshot> title;
  VenueE33DrawIdentity backend_identity{};
  VenueE33BackendContract backend{};

  bool valid() const {
    return title != nullptr && title->valid && backend_identity.valid() &&
           title->identity == backend_identity && backend.valid;
  }
};

struct VenueE33FrameSnapshot {
  // Census values from the reference trace only. Live venue culling changes
  // both values, so neither participates in publication readiness.
  static constexpr uint32_t kReferenceDrawCount = 23;
  static constexpr uint32_t kReferenceIndexCount = 13598;
  static constexpr uint32_t kRequiredTileBlockCount = 3;
  static constexpr std::array<uint32_t, kReferenceDrawCount>
      kReferenceIndexCounts = {
          613, 499, 541, 1083, 966, 540, 872, 896, 442, 541, 367, 721,
          724, 276, 32,  721,  718, 58,  687, 736, 687, 736, 142,
  };

  uint64_t sequence = 0;
  uint64_t backend_frame_sequence = 0;
  uint64_t learned_generation = 0;
  std::shared_ptr<const VenueE33LearnedIdentitySnapshot> learned_identity;
  uint32_t title_candidate_count = 0;
  uint32_t matched_draw_count = 0;
  uint32_t matched_index_count = 0;
  uint32_t unmatched_title_candidate_count = 0;
  uint32_t dropped_candidate_count = 0;
  uint32_t guest_read_failures = 0;
  uint32_t payload_copy_failures = 0;
  uint32_t texture_capture_failures = 0;
  uint32_t material_validation_failures = 0;
  // Retained for telemetry ABI compatibility. Current-frame capture has no
  // learner generation token, so this is always zero.
  uint32_t capture_generation_mismatches = 0;
  uint32_t backend_event_count = 0;
  uint32_t backend_draws_per_tile = 0;
  uint32_t backend_indices_per_tile = 0;
  uint32_t backend_tile_blocks_matched = 0;
  uint32_t backend_sequence_mismatches = 0;
  std::vector<VenueE33DrawSnapshot> draws;

  bool valid() const;
};

struct VenueE33ObserverTelemetry {
  uint64_t title_frames = 0;
  uint64_t title_draws_observed = 0;
  uint64_t title_base_structure_matches = 0;
  uint64_t title_topology_matches = 0;
  uint64_t title_stride_matches = 0;
  uint64_t title_endian_matches = 0;
  uint64_t title_index_layout_matches = 0;
  uint64_t title_buffer_bounds_matches = 0;
  uint64_t title_identity_matches = 0;
  uint64_t title_declaration_matches = 0;
  uint64_t title_candidates = 0;
  uint64_t title_capture_attempts = 0;
  uint64_t valid_title_snapshots = 0;
  uint64_t title_guest_read_failures = 0;
  uint64_t title_payload_copy_failures = 0;
  uint64_t title_texture_capture_failures = 0;
  uint64_t title_material_validation_failures = 0;
  uint64_t title_renderer_full_mip_textures = 0;
  uint64_t title_renderer_texture_shape_matches = 0;
  uint64_t capture_generation_mismatches = 0;
  uint64_t backend_draws_observed = 0;
  uint64_t backend_pixel_hash_matches = 0;
  uint64_t backend_shader_pair_matches = 0;
  uint64_t backend_contract_matches = 0;
  uint64_t backend_rasterizer_contract_matches = 0;
  uint64_t backend_events = 0;
  uint64_t backend_events_dropped = 0;
  uint64_t backend_events_stale = 0;
  uint64_t latest_backend_frame_sequence = 0;
  uint64_t backend_frames_analyzed = 0;
  uint64_t backend_events_without_title_frame = 0;
  uint64_t backend_tile_blocks_matched = 0;
  uint64_t backend_sequence_mismatches = 0;
  uint64_t proof_frames = 0;
  uint64_t learned_generations = 0;
  uint64_t capture_frames_rejected = 0;
  uint64_t finalized_frames = 0;
  uint64_t valid_frames = 0;
  uint64_t expired_frames = 0;
  uint64_t latest_published_sequence = 0;
  uint64_t learned_generation = 0;
  uint32_t learned_unique_program_count = 0;
  uint32_t latest_rasterizer_mode_control = 0;
  uint32_t pending_frames = 0;
  uint32_t queued_backend_events = 0;
};

bool VenueE33ObserverEnabled();

// Copies immutable payloads for structurally exact stride-32 candidates in
// the current title frame. Only a later exact backend-frame hash/order join
// can select and publish them as E33. This function never suppresses a guest
// draw.
void ObserveVenueE33TitleDraw(uint8_t *guest_base,
                              const SceneCatalogDrawOccurrence &draw);

// Exact translated-backend observer tap. It returns no route and performs no
// rendering or guest draw suppression.
void ObserveVenueE33BackendDraw(
    const rex::graphics::NativeGuestDrawContext &context);

void VenueE33ObserverFrameEnd();

std::shared_ptr<const VenueE33FrameSnapshot> LatestVenueE33FrameSnapshot();
std::shared_ptr<const VenueE33LearnedIdentitySnapshot>
LatestVenueE33LearnedIdentitySnapshot();
VenueE33ObserverTelemetry LatestVenueE33ObserverTelemetry();

} // namespace tabletennis::native
