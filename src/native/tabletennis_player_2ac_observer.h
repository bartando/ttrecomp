#pragma once

#include "native/tabletennis_player_2ac_payload.h"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace rex::graphics {
struct NativeGuestDrawContext;
}

namespace tabletennis::native {

struct SceneCatalogDrawOccurrence;

enum class Player2ACTitleKind : uint8_t {
  kUnknown = 0,
  kSingleStream32,
  kSingleStream96,
  kSkinned36,
  kSkinned44,
};

struct Player2ACDrawIdentity {
  uint32_t primitive_type = 0;
  uint32_t submitted_index_count = 0;
  uint32_t guest_index_base = 0;

  bool valid() const {
    return primitive_type != 0 && submitted_index_count != 0 &&
           guest_index_base != 0;
  }
  bool operator==(const Player2ACDrawIdentity &) const = default;
};

struct Player2ACFetchMetadata {
  uint32_t slot = 0;
  uint32_t physical_address = 0;
  uint32_t byte_count = 0;
  uint32_t endian = 0;
  uint32_t stride = 0;
  uint32_t record_count = 0;
  uint64_t payload_fingerprint = 0;
  bool valid = false;
};

struct Player2ACTitleProgram {
  uint32_t pass_descriptor = 0;
  uint32_t program_pair = 0;
  uint32_t vertex_shader = 0;
  uint32_t pixel_shader = 0;
  uint64_t vertex_shader_hash = 0;
  uint64_t pixel_shader_hash = 0;
  uint32_t bound_vertex_shader = 0;
  uint64_t bound_vertex_shader_hash = 0;
  uint32_t bound_pixel_shader = 0;
  uint64_t bound_pixel_shader_hash = 0;
  bool shader_fingerprints_valid = false;
  bool bound_vertex_shader_valid = false;
  bool bound_pixel_shader_valid = false;

  bool valid() const {
    return pass_descriptor != 0 && program_pair != 0 && vertex_shader != 0 &&
           pixel_shader != 0 && vertex_shader_hash != 0 &&
           pixel_shader_hash != 0 && shader_fingerprints_valid &&
           bound_pixel_shader != 0 && bound_pixel_shader_hash != 0 &&
           bound_pixel_shader_valid;
  }
  bool operator==(const Player2ACTitleProgram &) const = default;
};

struct Player2ACTitleDrawMetadata {
  Player2ACDrawIdentity identity{};
  Player2ACTitleProgram program{};
  Player2ACFetchMetadata vertices{};
  Player2ACFetchMetadata palette{};
  std::shared_ptr<const Player2ACDrawPayload> payload;
  Player2ACTitleKind kind = Player2ACTitleKind::kUnknown;
  uint32_t ordinal = 0;
  uint32_t player = 0;
  uint32_t owner_kind = 0;
  uint32_t owner = 0;
  bool valid = false;
};

struct Player2ACBackendContract {
  struct FetchIdentity {
    uint32_t physical_address = 0;
    uint32_t byte_count = 0;
    uint32_t endian = 0;
    bool valid = false;

    bool operator==(const FetchIdentity &) const = default;
  };

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
  FetchIdentity primary_vertex_fetch{};
  FetchIdentity palette_vertex_fetch{};
  bool primitive_restart_enabled = false;
  bool rasterizer_mode_control_valid = false;
  bool valid = false;
};

struct Player2ACDrawProof {
  Player2ACTitleDrawMetadata title{};
  Player2ACBackendContract backend{};
  uint32_t backend_occurrence_count = 0;
  std::array<uint32_t, 2> rasterizer_modes{};

  bool valid() const;
};

struct Player2ACPaletteOwnershipGroup {
  uint32_t player = 0;
  uint32_t physical_address = 0;
  uint32_t byte_count = 0;
  uint32_t record_count = 0;
  uint64_t payload_fingerprint = 0;
  uint32_t draw_count = 0;

  bool valid() const {
    return physical_address != 0 && byte_count != 0 && record_count != 0 &&
           payload_fingerprint != 0 && draw_count != 0;
  }
};

struct Player2ACFrameSnapshot {
  uint64_t sequence = 0;
  uint64_t backend_frame_sequence = 0;
  uint32_t title_candidate_count = 0;
  uint32_t logical_draw_count = 0;
  uint32_t backend_event_count = 0;
  uint32_t total_index_count = 0;
  uint32_t unmatched_title_candidate_count = 0;
  uint32_t dropped_title_candidate_count = 0;
  uint32_t guest_read_failures = 0;
  uint32_t ambiguous_match_count = 0;
  uint32_t sequence_mismatch_count = 0;
  std::vector<Player2ACDrawProof> draws;
  std::vector<Player2ACPaletteOwnershipGroup> palette_groups;

  bool valid() const;
};

struct Player2ACObserverTelemetry {
  uint64_t title_frames = 0;
  uint64_t title_draws_observed = 0;
  uint64_t title_structural_candidates = 0;
  uint64_t title_single_stream_32 = 0;
  uint64_t title_single_stream_96 = 0;
  uint64_t title_skinned_36 = 0;
  uint64_t title_skinned_44 = 0;
  uint64_t title_guest_read_failures = 0;
  uint64_t title_candidates_dropped = 0;
  uint64_t backend_draws_observed = 0;
  uint64_t backend_pixel_hash_matches = 0;
  uint64_t backend_vertex_hash_matches = 0;
  uint64_t backend_contract_matches = 0;
  uint64_t backend_events = 0;
  uint64_t backend_events_dropped = 0;
  uint64_t ambiguous_frames = 0;
  uint64_t sequence_mismatches = 0;
  uint64_t finalized_frames = 0;
  uint64_t valid_frames = 0;
  uint64_t expired_frames = 0;
  uint64_t latest_title_sequence = 0;
  uint64_t latest_backend_sequence = 0;
  uint64_t latest_published_sequence = 0;
  uint32_t pending_frames = 0;
  uint32_t queued_backend_events = 0;
};

bool Player2ACObserverEnabled();

// Low-level bound-shader title capture. Fixed title addresses and cached
// ApplyPass hashes never participate in admission; the current pixel shader
// identifies the 2AC family, and an exact translated-backend join still must
// prove every draw before publication.
void ObservePlayer2ACTitleDraw(uint8_t *guest_base,
                               const SceneCatalogDrawOccurrence &draw);

// Exact backend proof tap. This observer never returns a replacement route,
// changes GPU state, suppresses a guest draw, or serves native output.
void ObservePlayer2ACBackendDraw(
    const rex::graphics::NativeGuestDrawContext &context);

void Player2ACObserverFrameEnd();

std::shared_ptr<const Player2ACFrameSnapshot> LatestPlayer2ACFrameSnapshot();
Player2ACObserverTelemetry LatestPlayer2ACObserverTelemetry();

} // namespace tabletennis::native
