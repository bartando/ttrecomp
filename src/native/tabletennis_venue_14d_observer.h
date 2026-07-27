#pragma once

#include "native/tabletennis_venue_14d_snapshot.h"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace rex::graphics {
struct NativeGuestDrawContext;
}

namespace tabletennis::native {

struct SceneCatalogDrawOccurrence;

struct Venue14DBackendContract {
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

struct Venue14DDrawSnapshot {
  std::shared_ptr<const Venue14DTitleDrawSnapshot> title;
  Venue14DBackendContract backend{};

  bool valid() const {
    return title != nullptr && title->valid && backend.valid;
  }
};

struct Venue14DFrameSnapshot {
  // Reference-trace values are retained for schema/census comparisons only.
  // Live culling changes the number of submitted family draws.
  static constexpr uint32_t kReferenceDrawCount = 47;
  static constexpr uint32_t kReferenceIndexCount = 24057;

  uint64_t sequence = 0;
  uint64_t backend_frame_sequence = 0;
  uint32_t title_candidate_count = 0;
  uint32_t matched_draw_count = 0;
  uint32_t matched_index_count = 0;
  uint32_t unmatched_title_candidate_count = 0;
  uint32_t dropped_candidate_count = 0;
  uint32_t guest_read_failures = 0;
  uint32_t payload_copy_failures = 0;
  uint32_t texture_capture_failures = 0;
  uint32_t backend_event_count = 0;
  uint32_t backend_draws_per_tile = 0;
  uint32_t backend_indices_per_tile = 0;
  uint32_t backend_tile_blocks_matched = 0;
  uint32_t backend_sequence_mismatches = 0;
  std::vector<Venue14DDrawSnapshot> draws;

  bool valid() const {
    if (sequence == 0 || backend_frame_sequence != sequence ||
        matched_draw_count == 0 ||
        matched_draw_count != backend_draws_per_tile ||
        matched_index_count == 0 ||
        matched_index_count != backend_indices_per_tile ||
        draws.size() != matched_draw_count ||
        title_candidate_count !=
            matched_draw_count + unmatched_title_candidate_count ||
        backend_event_count !=
            backend_draws_per_tile * backend_tile_blocks_matched ||
        dropped_candidate_count != 0 || guest_read_failures != 0 ||
        payload_copy_failures != 0 || texture_capture_failures != 0 ||
        backend_tile_blocks_matched != 3 ||
        backend_sequence_mismatches != 0) {
      return false;
    }
    for (const Venue14DDrawSnapshot& draw : draws) {
      if (!draw.valid()) {
        return false;
      }
    }
    return true;
  }
};

struct Venue14DObserverTelemetry {
  uint64_t title_frames = 0;
  uint64_t finalized_frames = 0;
  uint64_t valid_frames = 0;
  uint64_t rejected_frames = 0;
  uint64_t expired_frames = 0;
  uint64_t title_draws_observed = 0;
  uint64_t title_candidates = 0;
  uint64_t valid_title_snapshots = 0;
  uint64_t title_guest_read_failures = 0;
  uint64_t title_payload_copy_failures = 0;
  uint64_t title_texture_capture_failures = 0;
  uint64_t backend_draws_observed = 0;
  uint64_t latest_backend_frame_sequence = 0;
  uint64_t backend_pixel_hash_matches = 0;
  uint64_t backend_shader_pair_matches = 0;
  uint64_t backend_events = 0;
  uint64_t backend_events_dropped = 0;
  uint64_t backend_frames_analyzed = 0;
  uint64_t backend_events_without_title_frame = 0;
  uint64_t backend_tile_blocks_matched = 0;
  uint64_t backend_sequence_mismatches = 0;
  uint64_t latest_published_sequence = 0;
  uint32_t pending_frames = 0;
  uint32_t queued_backend_events = 0;
};

bool Venue14DObserverEnabled();

// Observer-only title capture. Program identity is a candidate gate; only the
// independent translated-backend hash/order join can publish the family.
void ObserveVenue14DTitleDraw(
    uint8_t* guest_base, const SceneCatalogDrawOccurrence& draw);

// Observer-only dispatcher tap. It never selects a replacement route.
void ObserveVenue14DBackendDraw(
    const rex::graphics::NativeGuestDrawContext& context);

void Venue14DObserverFrameEnd();

std::shared_ptr<const Venue14DFrameSnapshot>
LatestVenue14DFrameSnapshot();
Venue14DObserverTelemetry LatestVenue14DObserverTelemetry();

}  // namespace tabletennis::native
