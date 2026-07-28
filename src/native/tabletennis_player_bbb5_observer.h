#pragma once

#include "native/tabletennis_player_bbb5_snapshot.h"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace rex::graphics {
struct NativeGuestDrawContext;
}

namespace tabletennis::native {

struct SceneCatalogDrawOccurrence;

struct PlayerBBB5BackendContract {
  uint64_t vertex_shader_hash = 0;
  uint64_t pixel_shader_hash = 0;
  uint32_t render_pass_key = 0;
  uint32_t rb_color_info_0 = 0;
  uint32_t rb_depth_info = 0;
  uint32_t rb_surface_info = 0;
  uint32_t rb_modecontrol = 0;
  uint32_t color_edram_base = 0;
  uint32_t depth_edram_base = 0;
  uint32_t surface_pitch = 0;
  uint32_t edram_mode = 0;
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
  bool render_target_state_valid = false;
  bool rasterizer_mode_control_valid = false;
  bool valid = false;

  bool operator==(const PlayerBBB5BackendContract &) const = default;
};

struct PlayerBBB5DrawSnapshot {
  std::shared_ptr<const PlayerBBB5TitleDrawSnapshot> title;
  PlayerBBB5DrawIdentity backend_identity{};
  PlayerBBB5BackendContract backend{};

  bool valid() const {
    return title != nullptr && title->valid && backend_identity.valid() &&
           title->identity == backend_identity && backend.valid;
  }
};

struct PlayerBBB5FrameSnapshot {
  static constexpr uint32_t kRequiredTileBlockCount = 3;

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
  uint32_t material_validation_failures = 0;
  uint32_t backend_event_count = 0;
  uint32_t backend_draws_per_tile = 0;
  uint32_t backend_indices_per_tile = 0;
  uint32_t backend_tile_blocks_matched = 0;
  uint32_t sequence_mismatches = 0;
  std::vector<PlayerBBB5DrawSnapshot> draws;

  bool valid() const;
};

struct PlayerBBB5ObserverTelemetry {
  uint64_t title_frames = 0;
  uint64_t title_draws_observed = 0;
  uint64_t title_pixel_hash_matches = 0;
  uint64_t title_shader_pair_matches = 0;
  uint64_t title_structural_matches = 0;
  uint64_t title_declaration_matches = 0;
  uint64_t title_candidates = 0;
  uint64_t title_valid_snapshots = 0;
  uint64_t title_guest_read_failures = 0;
  uint64_t title_payload_copy_failures = 0;
  uint64_t title_texture_capture_failures = 0;
  uint64_t title_material_validation_failures = 0;
  uint64_t backend_draws_observed = 0;
  uint64_t backend_pixel_hash_matches = 0;
  uint64_t backend_shader_pair_matches = 0;
  uint64_t backend_target_matches = 0;
  uint64_t backend_contract_matches = 0;
  uint64_t backend_events = 0;
  uint64_t backend_frames_analyzed = 0;
  uint64_t backend_tile_blocks_matched = 0;
  uint64_t finalized_frames = 0;
  uint64_t valid_frames = 0;
  uint64_t rejected_frames = 0;
  uint64_t expired_frames = 0;
  uint64_t latest_published_sequence = 0;
  uint64_t latest_backend_sequence = 0;
  uint32_t latest_rasterizer_mode_control = 0;
  uint32_t pending_frames = 0;
};

bool PlayerBBB5ObserverEnabled();

void ObservePlayerBBB5TitleDraw(uint8_t *guest_base,
                                const SceneCatalogDrawOccurrence &draw);
void ObservePlayerBBB5BackendDraw(
    const rex::graphics::NativeGuestDrawContext &context);
void PlayerBBB5ObserverFrameEnd();

std::shared_ptr<const PlayerBBB5FrameSnapshot> LatestPlayerBBB5FrameSnapshot();
PlayerBBB5ObserverTelemetry LatestPlayerBBB5ObserverTelemetry();

} // namespace tabletennis::native
