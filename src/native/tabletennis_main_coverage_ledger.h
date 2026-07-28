#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace rex::graphics {
struct NativeGuestDrawContext;
}

namespace tabletennis::native {

struct CrowdFrameSnapshot;
struct D47PlayerFrameSnapshot;
struct NetBB903FrameSnapshot;
struct Player6AEFrameSnapshot;
struct PlayerA406FrameSnapshot;
struct PlayerBBB5FrameSnapshot;
struct PlayerSkinFrameSnapshot;
struct Venue14DFrameSnapshot;
struct VenueE33FrameSnapshot;
struct VenueFullFamilyFrame;
struct Venue9EFrameSnapshot;

// Value-only identity shared by the title's ordered DrawIndexedPrimitive
// catalog and the translated backend. No guest pointer or live resource is
// retained by this ledger.
struct MainCoverageDrawIdentity {
  uint32_t primitive_type = 0;
  uint32_t submitted_index_count = 0;
  uint32_t physical_index_base = 0;

  bool valid() const {
    return primitive_type != 0 && submitted_index_count != 0 &&
           physical_index_base != 0;
  }
  bool operator==(const MainCoverageDrawIdentity &) const = default;
};

// Complete translated-backend state retained for repetition proof and future
// compositor readiness checks. Formats are stored as enum values so the public
// snapshot stays independent of backend-owned objects.
struct MainCoverageBackendContract {
  uint32_t backend = 0;
  uint32_t host_vertex_or_index_count = 0;
  uint32_t primary_vertex_physical_address = 0;
  uint32_t primary_vertex_byte_count = 0;
  uint32_t primary_vertex_endian = 0;
  uint32_t render_pass_key = 0;
  uint32_t rb_color_info_0 = 0;
  uint32_t rb_depth_info = 0;
  uint32_t rb_surface_info = 0;
  uint32_t rb_modecontrol = 0;
  uint32_t color_edram_base = 0;
  uint32_t depth_edram_base = 0;
  uint32_t edram_mode = 0;
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
  bool indexed = false;
  bool primary_vertex_fetch_valid = false;
  bool primitive_restart_enabled = false;
  bool render_target_state_valid = false;
  bool rasterizer_mode_control_valid = false;
  bool draw_state_contract_valid = false;
  bool attachment_contract_valid = false;
  bool complete = false;

  bool operator==(const MainCoverageBackendContract &) const = default;
};

enum class MainCoverageAssignmentFamily : uint8_t {
  kUnassigned = 0,
  kVenuePs328,
  kVenue14D,
  kVenueE33,
  kCrowdC6,
  kPlayerCa9,
  kPlayerD47,
  kPlayer6AE,
  kNetBB903,
  kVenue9E,
  kPlayerA406,
  kPlayerBBB5,
};

enum class MainCoverageAssignmentProof : uint8_t {
  kUnassigned = 0,
  // Exact current-frame title payload, but no backend proof tied to the same
  // sequence is retained by the source snapshot.
  kTitlePayloadOnly,
  // Backend evidence exists, but its current API is global rather than tied to
  // the published title sequence.
  kBackendEvidenceNotSequenceTied,
  // The source snapshot itself owns an exact same-sequence backend join.
  kSameFrameBackendProof,
};

struct MainCoverageDrawSnapshot {
  uint32_t original_ordinal = 0;
  uint32_t catalog_draw_index = 0;
  MainCoverageDrawIdentity identity{};
  uint64_t vertex_shader_hash = 0;
  uint64_t pixel_shader_hash = 0;
  MainCoverageBackendContract backend{};
  MainCoverageAssignmentFamily assignment =
      MainCoverageAssignmentFamily::kUnassigned;
  MainCoverageAssignmentProof assignment_proof =
      MainCoverageAssignmentProof::kUnassigned;

  bool valid() const {
    return original_ordinal != 0 && identity.valid() &&
           vertex_shader_hash != 0 && pixel_shader_hash != 0 &&
           backend.complete;
  }
};

// Grouped diagnostic for work that has an exact title/backend identity but no
// same-frame proven family snapshot yet.
struct MainCoverageUnassignedShaderFamily {
  uint64_t vertex_shader_hash = 0;
  uint64_t pixel_shader_hash = 0;
  uint32_t draw_count = 0;
  uint32_t submitted_index_count = 0;
  std::vector<uint32_t> original_ordinals;

  bool valid() const {
    return vertex_shader_hash != 0 && pixel_shader_hash != 0 &&
           draw_count != 0 && draw_count == original_ordinals.size();
  }
};

enum class MainCoverageBackendOnlyRegion : uint8_t {
  kPrefix = 0,
  kTile,
  kSuffix,
};

enum class MainCoverageBackendOnlyClassification : uint8_t {
  kUnclassified = 0,
  // Exact auto-indexed Xenos rectangle-list whose vf95 payload is
  // float3-position/float4-color and whose pixel shader forwards that color to
  // MAIN. RexGlue expands the three guest control vertices into a four-index
  // host triangle strip. This is real output work outside the title's indexed
  // DrawIndexedPrimitive catalog.
  kVertexColorRectangleOutput,
};

struct MainCoverageVertexColorRectangleProof {
  static constexpr size_t kGuestControlVertexCount = 3;
  static constexpr size_t kVertexStride = 28;

  uint32_t vertex_physical_address = 0;
  uint32_t vertex_byte_count = 0;
  uint32_t vertex_endian = 0;
  uint64_t payload_fingerprint = 0;
  // Exact value-owned guest bytes after stable double-read. This is retained
  // for translated replay; decoded floats alone would lose the deliberate
  // 0xFFFFFFFF NaN color components.
  std::array<uint8_t, kGuestControlVertexCount * kVertexStride> payload_bytes{};
  std::array<float, kGuestControlVertexCount * 3> positions{};
  std::array<float, kGuestControlVertexCount * 4> colors{};
  // Retained alongside the decoded values because the observed green
  // components are the deliberate Xenos bit pattern 0xFFFFFFFF (negative
  // quiet NaN). A future replay must feed those guest bits through the shader
  // and render-target conversion rather than sanitize them.
  std::array<uint32_t, kGuestControlVertexCount * 4> color_bits{};
  bool payload_stable = false;
  bool positions_finite = false;
  bool rectangle_geometry_valid = false;
  bool color_payload_contract_valid = false;
  bool color_write_enabled = false;

  bool valid() const {
    return vertex_physical_address != 0 &&
           vertex_byte_count >= kGuestControlVertexCount * kVertexStride &&
           vertex_endian == 2 && payload_fingerprint != 0 && payload_stable &&
           positions_finite && rectangle_geometry_valid &&
           color_payload_contract_valid && color_write_enabled;
  }
};

// A MAIN callback not joined to a title ordinal. This includes callbacks
// outside the selected three-tile core and unmatched offsets repeated inside
// every tile. They remain uncovered backend work.
struct MainCoverageBackendOnlyEvent {
  uint32_t backend_event_index = 0;
  MainCoverageDrawIdentity identity{};
  uint64_t vertex_shader_hash = 0;
  uint64_t pixel_shader_hash = 0;
  MainCoverageBackendContract backend{};
  MainCoverageBackendOnlyRegion region = MainCoverageBackendOnlyRegion::kPrefix;
  MainCoverageBackendOnlyClassification classification =
      MainCoverageBackendOnlyClassification::kUnclassified;
  MainCoverageVertexColorRectangleProof vertex_color_rectangle{};
  uint32_t tile_ordinal = 0;
  uint32_t tile_event_offset = 0;
};

enum class MainCoverageRejectReason : uint8_t {
  kNone = 0,
  kMissingCatalog,
  kCatalogFrameMismatch,
  kCatalogIncomplete,
  kMissingBackendEvents,
  kBackendEventOverflow,
  kInvalidBackendEvent,
  kIncompleteTileBlocks,
  kAmbiguousTileBlocks,
  kTileIdentityMismatch,
  kTileStateMismatch,
  kNoOrderedTitleJoin,
  kAmbiguousOrderedTitleJoin,
  kDuplicateOrdinal,
  kFamilyOrderMismatch,
  kFamilyAssignmentOverlap,
  kFamilyIdentityMismatch,
};

struct MainCoverageFrameSnapshot {
  static constexpr uint32_t kRequiredTileBlockCount = 3;

  uint64_t sequence = 0;
  uint64_t backend_frame_sequence = 0;
  uint32_t catalog_draw_count = 0;
  uint32_t catalog_dropped_draw_count = 0;
  uint32_t catalog_guest_read_failures = 0;
  uint32_t backend_event_count = 0;
  // Raw callbacks in one exactly repeated backend tile block, including
  // uncovered interleaved offsets.
  uint32_t backend_tile_event_count = 0;
  uint32_t backend_maximal_phase_count = 0;
  std::vector<uint32_t> backend_maximal_phase_starts;
  // Title-joined logical draws in one tile. This deliberately differs from
  // backend_tile_event_count while interleaved backend-only work exists.
  uint32_t logical_main_draw_count = 0;
  uint32_t backend_tile_blocks_matched = 0;
  uint32_t backend_tile_core_start_event = 0;
  uint32_t backend_prefix_event_count = 0;
  uint32_t backend_interleaved_event_count = 0;
  uint32_t backend_suffix_event_count = 0;
  uint32_t backend_only_event_count = 0;
  uint32_t matched_catalog_draw_count = 0;
  uint32_t assigned_draw_count = 0;
  uint32_t unassigned_draw_count = 0;
  MainCoverageRejectReason reject_reason =
      MainCoverageRejectReason::kMissingCatalog;
  bool tile_blocks_exact = false;
  bool ordered_title_join_exact = false;
  bool family_assignments_exact = false;
  std::vector<MainCoverageDrawSnapshot> draws;
  std::vector<uint32_t> unassigned_ordinals;
  std::vector<MainCoverageUnassignedShaderFamily> unassigned_shader_families;
  std::vector<MainCoverageBackendOnlyEvent> backend_only_events;

  bool valid() const;
  // Coverage labels are not renderer-readiness proof: title-only and
  // sequence-untied assignments deliberately count here.
  bool all_ordinals_assigned() const {
    return valid() && unassigned_draw_count == 0;
  }
  bool all_backend_events_covered() const {
    return valid() && backend_only_event_count == 0;
  }
  // Conservative atomic-readiness input. This still does not authorize
  // serving; it only proves every label came from a same-frame backend join.
  bool all_draws_same_frame_proven() const;
};

struct MainCoverageLedgerTelemetry {
  uint64_t title_frames = 0;
  uint64_t backend_events_observed = 0;
  uint64_t backend_events_dropped = 0;
  uint64_t finalized_frames = 0;
  uint64_t valid_frames = 0;
  uint64_t rejected_frames = 0;
  uint64_t ambiguous_frames = 0;
  uint64_t tile_mismatch_frames = 0;
  uint64_t family_overlap_frames = 0;
  uint64_t latest_title_sequence = 0;
  uint64_t latest_backend_sequence = 0;
  uint64_t latest_published_sequence = 0;
  uint32_t pending_frames = 0;
  uint32_t queued_backend_events = 0;
};

bool MainCoverageLedgerEnabled();

// Read-only translated-backend tap. It never selects a replacement route,
// changes GPU state, renders, or suppresses a guest draw.
void ObserveMainCoverageBackendDraw(
    const rex::graphics::NativeGuestDrawContext &context);

// Standalone phase-zero capture. This has no ledger dependency: callers get a
// value-owned proof only for the exact persistent MAIN rectangle, or an
// invalid proof. It never selects a replacement or mutates GPU state.
MainCoverageVertexColorRectangleProof
CaptureMainVertexColorRectangleProof(
    const rex::graphics::NativeGuestDrawContext &context);

// Family publishers feed these overloads at the moment an immutable snapshot
// becomes visible. The sequence-keyed archive prevents a short-lived delayed
// snapshot from being missed between title swaps.
void ObserveMainCoverageFamilyFrame(
    std::shared_ptr<const VenueFullFamilyFrame> frame);
void ObserveMainCoverageFamilyFrame(
    std::shared_ptr<const Venue14DFrameSnapshot> frame);
void ObserveMainCoverageFamilyFrame(
    std::shared_ptr<const VenueE33FrameSnapshot> frame);
void ObserveMainCoverageFamilyFrame(
    std::shared_ptr<const CrowdFrameSnapshot> frame);
void ObserveMainCoverageFamilyFrame(
    std::shared_ptr<const PlayerSkinFrameSnapshot> frame);
void ObserveMainCoverageFamilyFrame(
    std::shared_ptr<const D47PlayerFrameSnapshot> frame);
void ObserveMainCoverageFamilyFrame(
    std::shared_ptr<const Player6AEFrameSnapshot> frame);
void ObserveMainCoverageFamilyFrame(
    std::shared_ptr<const NetBB903FrameSnapshot> frame);
void ObserveMainCoverageFamilyFrame(
    std::shared_ptr<const Venue9EFrameSnapshot> frame);
void ObserveMainCoverageFamilyFrame(
    std::shared_ptr<const PlayerA406FrameSnapshot> frame);
void ObserveMainCoverageFamilyFrame(
    std::shared_ptr<const PlayerBBB5FrameSnapshot> frame);

// Called after the ordered scene catalog and all family observers publish at
// title Swap. Backend frame N is finalized only after a newer authoritative
// backend sequence proves N is closed.
void MainCoverageLedgerFrameEnd();

std::shared_ptr<const MainCoverageFrameSnapshot>
LatestMainCoverageFrameSnapshot();
MainCoverageLedgerTelemetry LatestMainCoverageLedgerTelemetry();

const char *
MainCoverageAssignmentFamilyName(MainCoverageAssignmentFamily family);
const char *MainCoverageRejectReasonName(MainCoverageRejectReason reason);

} // namespace tabletennis::native
