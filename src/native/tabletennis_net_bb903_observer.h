#pragma once

#include "native/tabletennis_mesh_snapshot.h"
#include "native/tabletennis_texture_snapshot.h"

#include <array>
#include <cstdint>
#include <memory>

namespace rex::graphics {
struct NativeGuestDrawContext;
struct NativeGuestDrawEligibilityContext;
} // namespace rex::graphics

namespace tabletennis::native {

struct SceneCatalogDrawOccurrence;

struct NetBB903TitleDrawSnapshot {
  uint32_t ordinal = 0;
  uint32_t owner = 0;
  uint32_t renderable = 0;
  uint32_t shader = 0;
  uint32_t model = 0;
  uint32_t pass_descriptor = 0;
  uint32_t program_pair = 0;
  uint32_t title_vertex_shader = 0;
  uint32_t title_pixel_shader = 0;
  uint32_t primitive_type = 0;
  uint32_t submitted_index_count = 0;
  uint32_t physical_index_base = 0;
  uint64_t world_hash = 0;
  uint64_t world_view_projection_hash = 0;
  std::array<float, 16> world{};
  std::array<float, 16> world_view_projection{};
  std::array<float, 16> vertex_constants_12_15{};
  std::array<std::array<uint32_t, 6>, 2> texture_fetches{};
  std::shared_ptr<const TableMeshSnapshot> mesh;
  std::array<std::shared_ptr<const TableTextureSnapshot>, 2> textures{};
  bool valid = false;
};

// Exact borrowed attachment identity for one indexed BB903 content draw.
// The depth format is retained as the host NRHI enum value because Vulkan may
// expose the guest D24S8 attachment as either D24S8 or D32S8.
struct NetBB903AttachmentContract {
  uint32_t render_pass_key = 0;
  uint32_t surface_pitch = 0;
  std::array<uint32_t, 4> color_attachment_formats{};
  uint32_t color_attachment_count = 0;
  uint32_t depth_attachment_format = 0;
  uint32_t stencil_attachment_format = 0;
  uint32_t sample_count = 0;
  uint64_t sample_mask = 0;
  bool valid = false;

  bool operator==(const NetBB903AttachmentContract &) const = default;
};

// Captured verbatim, but deliberately not approved as a serving contract.
// The historic BB903 trace predates raster-register logging.
struct NetBB903RasterTuple {
  uint32_t normalized_depth_control = 0;
  uint32_t normalized_color_mask = 0;
  uint32_t color_control = 0;
  uint32_t blend_control_0 = 0;
  uint32_t primitive_restart_index = 0;
  bool primitive_restart_enabled = false;
  bool observed = false;
  bool proven = false;

  bool operator==(const NetBB903RasterTuple &) const = default;
};

struct NetBB903ContentBackendContract {
  uint64_t vertex_shader_hash = 0;
  uint64_t pixel_shader_hash = 0;
  uint32_t primitive_type = 0;
  uint32_t submitted_index_count = 0;
  uint32_t host_index_count = 0;
  uint32_t guest_index_base = 0;
  NetBB903AttachmentContract attachments{};
  NetBB903RasterTuple raster{};
  bool valid = false;

  bool operator==(const NetBB903ContentBackendContract &) const = default;
};

struct NetBB903FrameSnapshot {
  static constexpr uint32_t kContentDrawCount = 2;
  static constexpr uint32_t kRenderableIndexCount = 9360;
  static constexpr uint32_t kTileBlockCount = 3;

  uint64_t sequence = 0;
  uint64_t backend_frame_sequence = 0;
  uint32_t title_candidate_count = 0;
  uint32_t valid_title_draw_count = 0;
  uint32_t dropped_title_draw_count = 0;
  uint32_t missing_mesh_count = 0;
  uint32_t missing_texture_count = 0;
  uint32_t backend_event_count = 0;
  uint32_t backend_draws_per_tile = 0;
  uint32_t backend_tile_blocks_matched = 0;
  uint32_t backend_sequence_mismatches = 0;
  std::shared_ptr<const TableMeshSnapshot> mesh;
  std::array<std::shared_ptr<const TableTextureSnapshot>, 2> textures{};
  std::array<NetBB903TitleDrawSnapshot, kContentDrawCount> title_draws{};
  std::array<NetBB903ContentBackendContract, kContentDrawCount>
      content_backend{};

  bool observer_complete() const;

  // Deliberately false until a trace with BB903-specific raster registers is
  // compared against the live tuples and a renderer/suppression path exists.
  bool ready_to_serve() const { return false; }
};

struct NetBB903ObserverTelemetry {
  uint64_t title_frames = 0;
  uint64_t title_draws_observed = 0;
  uint64_t title_shape_matches = 0;
  uint64_t title_material_matches = 0;
  uint64_t title_owner_matches = 0;
  uint64_t title_anchor_matches = 0;
  uint64_t title_identity_matches = 0;
  uint64_t title_sibling_matches = 0;
  uint64_t title_scope_matches = 0;
  uint64_t title_pass_matches = 0;
  uint64_t title_mesh_matches = 0;
  uint64_t title_state_matches = 0;
  uint64_t title_transform_matches = 0;
  uint64_t title_physical_index_matches = 0;
  uint64_t title_candidates = 0;
  uint64_t valid_title_draws = 0;
  uint64_t title_missing_mesh = 0;
  uint64_t title_missing_textures = 0;
  uint64_t finalized_frames = 0;
  uint64_t observer_complete_frames = 0;
  uint64_t rejected_frames = 0;
  uint64_t expired_frames = 0;
  uint64_t backend_content_callbacks = 0;
  uint64_t backend_content_hash_matches = 0;
  uint64_t backend_content_early_probes = 0;
  uint64_t backend_content_contract_matches = 0;
  uint64_t backend_eligibility_callbacks = 0;
  uint64_t backend_eligibility_pixel_hash_matches = 0;
  uint64_t backend_eligibility_content_hash_matches = 0;
  uint64_t backend_eligibility_content_shape_matches = 0;
  uint64_t backend_eligibility_indexed_matches = 0;
  uint64_t backend_eligibility_eligible_matches = 0;
  uint64_t latest_backend_frame_sequence = 0;
  uint64_t backend_events_joined = 0;
  uint64_t backend_events_without_title_frame = 0;
  uint64_t backend_events_dropped = 0;
  uint64_t backend_frames_analyzed = 0;
  uint64_t backend_tile_blocks_matched = 0;
  uint64_t backend_sequence_mismatches = 0;
  uint64_t latest_title_sequence = 0;
  uint64_t latest_published_sequence = 0;
  uint32_t pending_frames = 0;
  uint32_t queued_backend_events = 0;
  bool raster_tuple_observed = false;
  bool raster_tuple_proven = false;
  bool serving_enabled = false;
};

bool NetBB903ObserverEnabled();

// Must be called synchronously at the ordered catalog draw hook. The first
// exact net-geometry draw with the direct lvlTable owner learns the frame-local
// scope/pass/program identity. Only its later ownerless sibling with that exact
// identity is admitted. The observer retains the existing immutable
// TableMeshSnapshot and captures the two immutable texture payloads from this
// draw's authoritative fetch state.
void ObserveNetBB903TitleDraw(
    uint8_t *guest_base, const SceneCatalogDrawOccurrence &draw);

// Feed normal matcher callbacks here. Early probes are measured and ignored;
// late contexts with the exact borrowed attachment contract are bucketed by
// authoritative backend frame. The completed frame derives its first tile
// from the live event count and proves two identical subsequent tile blocks.
void ObserveNetBB903BackendDraw(
    const rex::graphics::NativeGuestDrawContext &context);

// Feed the Vulkan pre-gate eligibility stream here. This is census telemetry
// only. Live evidence proves it sees the same six indexed content submissions
// (two draws replayed for three tiles), not the trace-only three-index command
// rows.
void ObserveNetBB903BackendEligibility(
    const rex::graphics::NativeGuestDrawEligibilityContext &context);

void NetBB903ObserverFrameEnd();

std::shared_ptr<const NetBB903FrameSnapshot> LatestNetBB903FrameSnapshot();
NetBB903ObserverTelemetry LatestNetBB903ObserverTelemetry();

} // namespace tabletennis::native
