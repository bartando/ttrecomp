#include "native/tabletennis_main_coverage_ledger.h"

#include "native/tabletennis_6ae_player_observer.h"
#include "native/tabletennis_crowd_observer.h"
#include "native/tabletennis_d47_player_observer.h"
#include "native/tabletennis_net_bb903_observer.h"
#include "native/tabletennis_player_skin_snapshot.h"
#include "native/tabletennis_scene_draw_catalog.h"
#include "native/tabletennis_venue_14d_observer.h"
#include "native/tabletennis_venue_e33_observer.h"
#include "native/tabletennis_venue_full_family.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <ranges>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <rex/cvar.h>
#include <rex/graphics/native_guest_renderer.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(
    tabletennis_native_main_coverage_ledger, false, "Table Tennis",
    "Join the exact ordered title catalog to translated-backend MAIN events, "
    "prove dynamic repeated tile blocks, and report uncovered shader families. "
    "Observer-only; never renders, replaces, or suppresses a draw.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_UINT32(
    tabletennis_native_main_coverage_log_interval, 120, "Table Tennis",
    "Finalized MAIN coverage frames between observer-only coverage reports.")
    .range(1, 100000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace tabletennis::native {
namespace {

namespace nrhi = rex::graphics::nrhi;

constexpr uint32_t kMainRenderPassKey = 0x0000000E;
constexpr uint32_t kMainSurfacePitch = 1280;
constexpr uint32_t kRequiredSampleCount = 4;
constexpr uint32_t kPhysicalAddressMask = 0x1FFFFFFF;
constexpr uint32_t kHighPhysicalHeapBase = 0xE0000000;
constexpr uint32_t kHighPhysicalHeapHostPageOffset = 0x1000;
constexpr size_t kMaximumRetainedFrames = 8;
constexpr size_t kMaximumBackendEventsPerFrame = 4096;
constexpr uint64_t kCrowdVertexShaderHash = 0xBD4B1DF972B828B7ull;
constexpr uint64_t kCrowdPixelShaderHash = 0xC6CEFDA3753CF2BAull;
constexpr uint64_t kPlayerCa9VertexShaderHash = 0xCA9BBF96B0928616ull;
constexpr uint64_t kPlayerCa9PixelShaderHash = 0x77AF85E1AF823D02ull;

struct BackendEvent {
  uint64_t sequence = 0;
  MainCoverageDrawIdentity identity{};
  uint64_t vertex_shader_hash = 0;
  uint64_t pixel_shader_hash = 0;
  MainCoverageBackendContract contract{};
};

struct TitleToken {
  uint32_t catalog_draw_index = 0;
  uint32_t ordinal = 0;
  MainCoverageDrawIdentity identity{};
  uint64_t pixel_shader_hash = 0;
};

struct FamilyAssignment {
  uint32_t ordinal = 0;
  MainCoverageDrawIdentity identity{};
  uint64_t vertex_shader_hash = 0;
  uint64_t pixel_shader_hash = 0;
  bool compare_vertex_shader_hash = true;
  MainCoverageAssignmentFamily family =
      MainCoverageAssignmentFamily::kUnassigned;
  MainCoverageAssignmentProof proof = MainCoverageAssignmentProof::kUnassigned;
};

constexpr size_t kAssignmentFamilyCount =
    static_cast<size_t>(MainCoverageAssignmentFamily::kNetBB903) + 1;

struct FrameLedger {
  uint64_t sequence = 0;
  std::shared_ptr<const SceneDrawCatalogFrame> catalog;
  std::vector<BackendEvent> backend_events;
  std::vector<FamilyAssignment> assignments;
  std::array<bool, kAssignmentFamilyCount> attached_families{};
  uint32_t dropped_backend_events = 0;
  bool family_order_mismatch = false;
  bool family_assignment_overlap = false;
  bool finalized = false;
};

struct ObservedFamilyFrames {
  std::shared_ptr<const VenueFullFamilyFrame> venue_ps328;
  std::shared_ptr<const Venue14DFrameSnapshot> venue_14d;
  std::shared_ptr<const VenueE33FrameSnapshot> venue_e33;
  std::shared_ptr<const CrowdFrameSnapshot> crowd_c6;
  std::shared_ptr<const PlayerSkinFrameSnapshot> player_ca9;
  std::shared_ptr<const D47PlayerFrameSnapshot> player_d47;
  std::shared_ptr<const Player6AEFrameSnapshot> player_6ae;
  std::shared_ptr<const NetBB903FrameSnapshot> net_bb903;
};

std::mutex g_ledger_mutex;
std::deque<FrameLedger> g_frames;
std::shared_ptr<const MainCoverageFrameSnapshot> g_published_frame;
MainCoverageLedgerTelemetry g_telemetry;
uint64_t g_latest_backend_sequence = 0;
bool g_was_enabled = false;
bool g_logged_first_valid_frame = false;
MainCoverageRejectReason g_last_logged_reject_reason =
    MainCoverageRejectReason::kNone;

uint32_t PhysicalAddressForVirtualAlias(uint32_t virtual_address) {
  if (virtual_address < kHighPhysicalHeapBase) {
    return 0;
  }
  const uint64_t physical_address =
      static_cast<uint64_t>(virtual_address - kHighPhysicalHeapBase) +
      kHighPhysicalHeapHostPageOffset;
  return physical_address <= kPhysicalAddressMask
             ? static_cast<uint32_t>(physical_address)
             : 0;
}

bool SupportedDepthFormat(nrhi::Format format) {
  return format == nrhi::Format::kD24_UNORM_S8_UINT ||
         format == nrhi::Format::kD32_FLOAT_S8_UINT;
}

bool IsMainPassCallback(const rex::graphics::NativeGuestDrawContext &context) {
  return context.render_pass_key_valid &&
         context.render_pass_key == kMainRenderPassKey;
}

bool IsCompleteMainEvent(const rex::graphics::NativeGuestDrawContext &context,
                         const MainCoverageDrawIdentity &identity) {
  return context.backend == rex::graphics::NativeGuestOutputBackend::kVulkan &&
         context.backend_frame_sequence != 0 && identity.valid() &&
         context.surface_pitch == kMainSurfacePitch && context.indexed &&
         context.guest_index_base_valid && context.vertex_shader_hash != 0 &&
         context.pixel_shader_hash != 0 && context.draw_state_contract_valid &&
         context.rasterizer_mode_control_valid &&
         context.borrowed_attachment_contract_valid &&
         context.color_attachment_count == 1 &&
         context.color_attachment_formats[0] == nrhi::Format::kR8G8B8A8_UNORM &&
         SupportedDepthFormat(context.depth_attachment_format) &&
         context.stencil_attachment_format == context.depth_attachment_format &&
         context.sample_count == kRequiredSampleCount &&
         context.sample_mask == std::numeric_limits<uint64_t>::max();
}

MainCoverageBackendContract
CaptureBackendContract(const rex::graphics::NativeGuestDrawContext &context,
                       const MainCoverageDrawIdentity &identity) {
  MainCoverageBackendContract contract;
  contract.backend = static_cast<uint32_t>(context.backend);
  contract.render_pass_key = context.render_pass_key;
  contract.surface_pitch = context.surface_pitch;
  contract.normalized_depth_control = context.normalized_depth_control;
  contract.normalized_color_mask = context.normalized_color_mask;
  contract.color_control = context.color_control;
  contract.blend_control_0 = context.blend_control_0;
  contract.rasterizer_mode_control = context.rasterizer_mode_control;
  contract.primitive_restart_index = context.primitive_restart_index;
  for (size_t index = 0; index < contract.color_attachment_formats.size();
       ++index) {
    contract.color_attachment_formats[index] =
        static_cast<uint32_t>(context.color_attachment_formats[index]);
  }
  contract.color_attachment_count = context.color_attachment_count;
  contract.depth_attachment_format =
      static_cast<uint32_t>(context.depth_attachment_format);
  contract.stencil_attachment_format =
      static_cast<uint32_t>(context.stencil_attachment_format);
  contract.sample_count = context.sample_count;
  contract.sample_mask = context.sample_mask;
  contract.primitive_restart_enabled = context.primitive_restart_enabled;
  contract.rasterizer_mode_control_valid =
      context.rasterizer_mode_control_valid;
  contract.draw_state_contract_valid = context.draw_state_contract_valid;
  contract.attachment_contract_valid =
      context.borrowed_attachment_contract_valid;
  contract.complete = IsCompleteMainEvent(context, identity);
  return contract;
}

MainCoverageDrawIdentity
CatalogIdentity(const SceneCatalogDrawOccurrence &draw) {
  return {
      .primitive_type = draw.primitive_type,
      .submitted_index_count = draw.submitted_index_count,
      .physical_index_base =
          PhysicalAddressForVirtualAlias(draw.mesh.index_buffer_alias),
  };
}

uint64_t CatalogPixelShaderHash(const SceneCatalogDrawOccurrence &draw) {
  if (draw.bound_shaders.pixel_shader_valid &&
      draw.bound_shaders.pixel_shader_hash != 0) {
    return draw.bound_shaders.pixel_shader_hash;
  }
  return draw.pass.shader_fingerprints_valid ? draw.pass.pixel_shader_hash : 0;
}

bool SameDraw(const BackendEvent &backend, const TitleToken &title) {
  return backend.contract.complete && backend.identity == title.identity &&
         backend.pixel_shader_hash == title.pixel_shader_hash;
}

template <typename Identity>
MainCoverageDrawIdentity FamilyIdentity(const Identity &identity) {
  return {
      .primitive_type = identity.primitive_type,
      .submitted_index_count = identity.submitted_index_count,
      .physical_index_base = identity.guest_index_base,
  };
}

FrameLedger *EnsureFrameLocked(uint64_t sequence) {
  if (sequence == 0 || sequence <= g_telemetry.latest_published_sequence) {
    return nullptr;
  }
  const auto position =
      std::ranges::lower_bound(g_frames, sequence, {}, &FrameLedger::sequence);
  if (position != g_frames.end() && position->sequence == sequence) {
    return &*position;
  }
  return &*g_frames.insert(position, FrameLedger{.sequence = sequence});
}

void AppendEventLocked(BackendEvent event) {
  FrameLedger *frame = EnsureFrameLocked(event.sequence);
  if (frame == nullptr || frame->finalized) {
    ++g_telemetry.backend_events_dropped;
    return;
  }
  if (frame->backend_events.size() >= kMaximumBackendEventsPerFrame) {
    ++frame->dropped_backend_events;
    ++g_telemetry.backend_events_dropped;
    return;
  }
  frame->backend_events.push_back(std::move(event));
}

bool StrictlyIncreasingOrdinals(std::span<const FamilyAssignment> assignments) {
  for (size_t index = 1; index < assignments.size(); ++index) {
    if (assignments[index - 1].ordinal >= assignments[index].ordinal) {
      return false;
    }
  }
  return true;
}

void AttachAssignmentsLocked(uint64_t sequence,
                             MainCoverageAssignmentFamily family,
                             MainCoverageAssignmentProof proof,
                             std::vector<FamilyAssignment> assignments) {
  FrameLedger *frame = EnsureFrameLocked(sequence);
  if (frame == nullptr || frame->finalized || assignments.empty()) {
    return;
  }
  const size_t family_index = static_cast<size_t>(family);
  if (family_index >= frame->attached_families.size() ||
      frame->attached_families[family_index]) {
    return;
  }
  frame->attached_families[family_index] = true;
  if (!StrictlyIncreasingOrdinals(assignments)) {
    frame->family_order_mismatch = true;
    return;
  }
  for (FamilyAssignment &assignment : assignments) {
    assignment.family = family;
    assignment.proof = proof;
    if (assignment.ordinal == 0 || !assignment.identity.valid() ||
        (assignment.compare_vertex_shader_hash &&
         assignment.vertex_shader_hash == 0) ||
        assignment.pixel_shader_hash == 0) {
      frame->family_order_mismatch = true;
      return;
    }
    if (std::ranges::find(frame->assignments, assignment.ordinal,
                          &FamilyAssignment::ordinal) !=
        frame->assignments.end()) {
      frame->family_assignment_overlap = true;
      return;
    }
    frame->assignments.push_back(std::move(assignment));
  }
}

void AttachVenuePs328Locked(
    const std::shared_ptr<const VenueFullFamilyFrame> &family) {
  if (family == nullptr || !family->valid()) {
    return;
  }
  std::vector<FamilyAssignment> assignments;
  assignments.reserve(family->draws.size());
  for (const VenueFullFamilyDrawSnapshot &draw : family->draws) {
    const uint64_t pixel_shader_hash = CatalogPixelShaderHash(draw.source);
    if (pixel_shader_hash == 0) {
      return;
    }
    assignments.push_back({
        .ordinal = draw.source.ordinal,
        .identity = CatalogIdentity(draw.source),
        .pixel_shader_hash = pixel_shader_hash,
        .compare_vertex_shader_hash = false,
    });
  }
  AttachAssignmentsLocked(
      family->sequence, MainCoverageAssignmentFamily::kVenuePs328,
      MainCoverageAssignmentProof::kTitlePayloadOnly, std::move(assignments));
}

void AttachVenue14DLocked(
    const std::shared_ptr<const Venue14DFrameSnapshot> &family) {
  if (family == nullptr || !family->valid()) {
    return;
  }
  std::vector<FamilyAssignment> assignments;
  assignments.reserve(family->draws.size());
  for (const Venue14DDrawSnapshot &draw : family->draws) {
    assignments.push_back({
        .ordinal = draw.title->ordinal,
        .identity = FamilyIdentity(draw.title->identity),
        .vertex_shader_hash = draw.backend.vertex_shader_hash,
        .pixel_shader_hash = draw.backend.pixel_shader_hash,
    });
  }
  AttachAssignmentsLocked(family->sequence,
                          MainCoverageAssignmentFamily::kVenue14D,
                          MainCoverageAssignmentProof::kSameFrameBackendProof,
                          std::move(assignments));
}

void AttachVenueE33Locked(
    const std::shared_ptr<const VenueE33FrameSnapshot> &family) {
  if (family == nullptr || !family->valid()) {
    return;
  }
  std::vector<FamilyAssignment> assignments;
  assignments.reserve(family->draws.size());
  for (const VenueE33DrawSnapshot &draw : family->draws) {
    assignments.push_back({
        .ordinal = draw.title->ordinal,
        .identity = FamilyIdentity(draw.backend_identity),
        .vertex_shader_hash = draw.backend.vertex_shader_hash,
        .pixel_shader_hash = draw.backend.pixel_shader_hash,
    });
  }
  AttachAssignmentsLocked(family->sequence,
                          MainCoverageAssignmentFamily::kVenueE33,
                          MainCoverageAssignmentProof::kSameFrameBackendProof,
                          std::move(assignments));
}

void AttachCrowdC6Locked(
    const std::shared_ptr<const CrowdFrameSnapshot> &family) {
  if (family == nullptr || !family->valid()) {
    return;
  }
  std::vector<FamilyAssignment> assignments;
  assignments.reserve(family->draws.size());
  for (const CrowdDrawSnapshot &draw : family->draws) {
    assignments.push_back({
        .ordinal = draw.ordinal,
        .identity =
            {
                .primitive_type = draw.primitive_type,
                .submitted_index_count = draw.submitted_index_count,
                .physical_index_base = draw.indices != nullptr
                                           ? draw.indices->physical_address
                                           : 0,
            },
        .vertex_shader_hash = kCrowdVertexShaderHash,
        .pixel_shader_hash = kCrowdPixelShaderHash,
    });
  }
  AttachAssignmentsLocked(family->sequence,
                          MainCoverageAssignmentFamily::kCrowdC6,
                          MainCoverageAssignmentProof::kSameFrameBackendProof,
                          std::move(assignments));
}

void AttachPlayerCa9Locked(
    const std::shared_ptr<const PlayerSkinFrameSnapshot> &family) {
  if (family == nullptr || !family->valid()) {
    return;
  }
  std::vector<FamilyAssignment> assignments;
  assignments.reserve(family->draws.size());
  for (const PlayerSkinDrawSnapshot &draw : family->draws) {
    assignments.push_back({
        .ordinal = draw.ordinal,
        .identity =
            {
                .primitive_type = draw.primitive_type,
                .submitted_index_count = draw.submitted_index_count,
                .physical_index_base = draw.indices != nullptr
                                           ? draw.indices->physical_address
                                           : 0,
            },
        .vertex_shader_hash = kPlayerCa9VertexShaderHash,
        .pixel_shader_hash = kPlayerCa9PixelShaderHash,
    });
  }
  AttachAssignmentsLocked(
      family->sequence, MainCoverageAssignmentFamily::kPlayerCa9,
      MainCoverageAssignmentProof::kBackendEvidenceNotSequenceTied,
      std::move(assignments));
}

void AttachPlayerD47Locked(
    const std::shared_ptr<const D47PlayerFrameSnapshot> &family) {
  if (family == nullptr || !family->valid()) {
    return;
  }
  std::vector<FamilyAssignment> assignments;
  assignments.reserve(family->draws.size());
  for (const D47PlayerDrawSnapshot &draw : family->draws) {
    assignments.push_back({
        .ordinal = draw.ordinal,
        .identity = FamilyIdentity(draw.identity),
        .vertex_shader_hash = draw.backend.vertex_shader_hash,
        .pixel_shader_hash = draw.backend.pixel_shader_hash,
    });
  }
  AttachAssignmentsLocked(family->sequence,
                          MainCoverageAssignmentFamily::kPlayerD47,
                          MainCoverageAssignmentProof::kSameFrameBackendProof,
                          std::move(assignments));
}

void AttachPlayer6AELocked(
    const std::shared_ptr<const Player6AEFrameSnapshot> &family) {
  if (family == nullptr || !family->valid()) {
    return;
  }
  std::vector<FamilyAssignment> assignments;
  assignments.reserve(family->draws.size());
  for (const Player6AEDrawSnapshot &draw : family->draws) {
    assignments.push_back({
        .ordinal = draw.ordinal,
        .identity = FamilyIdentity(draw.identity),
        .vertex_shader_hash = draw.backend.vertex_shader_hash,
        .pixel_shader_hash = draw.backend.pixel_shader_hash,
    });
  }
  AttachAssignmentsLocked(family->sequence,
                          MainCoverageAssignmentFamily::kPlayer6AE,
                          MainCoverageAssignmentProof::kSameFrameBackendProof,
                          std::move(assignments));
}

void AttachNetBB903Locked(
    const std::shared_ptr<const NetBB903FrameSnapshot> &family) {
  if (family == nullptr || !family->observer_complete()) {
    return;
  }
  std::vector<FamilyAssignment> assignments;
  assignments.reserve(family->title_draws.size());
  for (size_t index = 0; index < family->title_draws.size(); ++index) {
    const NetBB903TitleDrawSnapshot &draw = family->title_draws[index];
    const NetBB903ContentBackendContract &backend =
        family->content_backend[index];
    assignments.push_back({
        .ordinal = draw.ordinal,
        .identity =
            {
                .primitive_type = draw.primitive_type,
                .submitted_index_count = draw.submitted_index_count,
                .physical_index_base = draw.physical_index_base,
            },
        .vertex_shader_hash = backend.vertex_shader_hash,
        .pixel_shader_hash = backend.pixel_shader_hash,
    });
  }
  AttachAssignmentsLocked(family->sequence,
                          MainCoverageAssignmentFamily::kNetBB903,
                          MainCoverageAssignmentProof::kSameFrameBackendProof,
                          std::move(assignments));
}

void AttachObservedFamiliesLocked(const ObservedFamilyFrames &families) {
  AttachVenuePs328Locked(families.venue_ps328);
  AttachVenue14DLocked(families.venue_14d);
  AttachVenueE33Locked(families.venue_e33);
  AttachCrowdC6Locked(families.crowd_c6);
  AttachPlayerCa9Locked(families.player_ca9);
  AttachPlayerD47Locked(families.player_d47);
  AttachPlayer6AELocked(families.player_6ae);
  AttachNetBB903Locked(families.net_bb903);
}

struct TileBlockExtraction {
  size_t start_event = 0;
  size_t draws_per_tile = 0;
  std::span<const BackendEvent> first_block;
};

struct TileTitleMatch {
  size_t backend_offset = 0;
  size_t title_index = 0;
};

struct TileTitleAlignment {
  uint32_t matched_count = 0;
  // Saturated at two because the ledger only distinguishes unique from
  // ambiguous optimal mappings.
  uint8_t optimal_alignment_count = 0;
  std::vector<TileTitleMatch> matches;
};

bool SameBackendIdentity(const BackendEvent &expected,
                         const BackendEvent &observed) {
  return expected.identity == observed.identity &&
         expected.vertex_shader_hash == observed.vertex_shader_hash &&
         expected.pixel_shader_hash == observed.pixel_shader_hash;
}

bool BackendEventObservable(const BackendEvent &event, uint64_t sequence) {
  return event.sequence == sequence && event.identity.valid() &&
         event.vertex_shader_hash != 0 && event.pixel_shader_hash != 0;
}

bool CoreEventComplete(const BackendEvent &event, uint64_t sequence) {
  return BackendEventObservable(event, sequence) && event.contract.complete;
}

bool RepeatedBlockIdentityMatches(std::span<const BackendEvent> events,
                                  size_t start, size_t draws_per_tile) {
  for (size_t index = 0; index < draws_per_tile; ++index) {
    const BackendEvent &expected = events[start + index];
    if (!SameBackendIdentity(expected,
                             events[start + draws_per_tile + index]) ||
        !SameBackendIdentity(expected,
                             events[start + draws_per_tile * 2 + index])) {
      return false;
    }
  }
  return true;
}

// Retains every phase of the largest contiguous A,A,A sequence. Title order is
// the independent evidence that selects a phase later; rejecting phase
// ambiguity here would discard that evidence before it can be applied.
MainCoverageRejectReason
ProveTileBlocks(const FrameLedger &frame,
                std::vector<TileBlockExtraction> &extractions) {
  extractions.clear();
  if (frame.backend_events.empty()) {
    return MainCoverageRejectReason::kMissingBackendEvents;
  }
  if (frame.dropped_backend_events != 0) {
    return MainCoverageRejectReason::kBackendEventOverflow;
  }
  const std::span<const BackendEvent> events(frame.backend_events);
  if (events.size() < MainCoverageFrameSnapshot::kRequiredTileBlockCount) {
    return MainCoverageRejectReason::kIncompleteTileBlocks;
  }

  for (size_t draws_per_tile =
           events.size() / MainCoverageFrameSnapshot::kRequiredTileBlockCount;
       draws_per_tile != 0; --draws_per_tile) {
    const size_t core_event_count =
        draws_per_tile * MainCoverageFrameSnapshot::kRequiredTileBlockCount;
    std::vector<size_t> exact_starts;
    for (size_t start = 0; start + core_event_count <= events.size(); ++start) {
      if (!RepeatedBlockIdentityMatches(events, start, draws_per_tile)) {
        continue;
      }
      exact_starts.push_back(start);
    }
    if (exact_starts.empty()) {
      continue;
    }
    extractions.reserve(exact_starts.size());
    for (const size_t start : exact_starts) {
      extractions.push_back({
          .start_event = start,
          .draws_per_tile = draws_per_tile,
          .first_block = events.subspan(start, draws_per_tile),
      });
    }
    return MainCoverageRejectReason::kNone;
  }
  return MainCoverageRejectReason::kTileIdentityMismatch;
}

std::vector<TitleToken> BuildTitleTokens(const SceneDrawCatalogFrame &catalog) {
  std::vector<TitleToken> tokens;
  tokens.reserve(catalog.ordered_draw_count);
  for (uint32_t index = 0; index < catalog.ordered_draw_count; ++index) {
    const SceneCatalogDrawOccurrence &draw = catalog.ordered_draws[index];
    const MainCoverageDrawIdentity identity = CatalogIdentity(draw);
    const uint64_t pixel_shader_hash = CatalogPixelShaderHash(draw);
    if (!identity.valid() || draw.ordinal == 0 || pixel_shader_hash == 0) {
      continue;
    }
    tokens.push_back({
        .catalog_draw_index = index,
        .ordinal = draw.ordinal,
        .identity = identity,
        .pixel_shader_hash = pixel_shader_hash,
    });
  }
  return tokens;
}

// Computes a longest monotone matching with gaps permitted on both sides. The
// count table uses inclusion-exclusion so different skip paths leading to the
// same set of matched index pairs are not mistaken for distinct alignments.
// Counts saturate at two: zero, unique, and ambiguous are the only states the
// fail-closed observer needs.
TileTitleAlignment AlignTileToTitle(std::span<const BackendEvent> backend,
                                    std::span<const TitleToken> title) {
  TileTitleAlignment alignment;
  if (backend.empty() || title.empty()) {
    return alignment;
  }

  const size_t columns = title.size() + 1;
  const size_t cell_count = (backend.size() + 1) * columns;
  std::vector<uint32_t> lengths(cell_count, 0);
  std::vector<uint8_t> counts(cell_count, 1);
  const auto cell_index = [columns](size_t backend_index, size_t title_index) {
    return backend_index * columns + title_index;
  };

  for (size_t backend_index = backend.size(); backend_index-- != 0;) {
    for (size_t title_index = title.size(); title_index-- != 0;) {
      const size_t current = cell_index(backend_index, title_index);
      const size_t skip_backend = cell_index(backend_index + 1, title_index);
      const size_t skip_title = cell_index(backend_index, title_index + 1);
      const size_t diagonal = cell_index(backend_index + 1, title_index + 1);
      uint32_t length = std::max(lengths[skip_backend], lengths[skip_title]);
      if (SameDraw(backend[backend_index], title[title_index])) {
        length = std::max(length, lengths[diagonal] + 1);
      }
      lengths[current] = length;
    }
  }

  for (size_t backend_index = backend.size(); backend_index-- != 0;) {
    for (size_t title_index = title.size(); title_index-- != 0;) {
      const size_t current = cell_index(backend_index, title_index);
      const uint32_t length = lengths[current];
      if (length == 0) {
        counts[current] = 1;
        continue;
      }

      const size_t skip_backend = cell_index(backend_index + 1, title_index);
      const size_t skip_title = cell_index(backend_index, title_index + 1);
      const size_t diagonal = cell_index(backend_index + 1, title_index + 1);
      const bool can_skip_backend = lengths[skip_backend] == length;
      const bool can_skip_title = lengths[skip_title] == length;
      const bool can_take =
          SameDraw(backend[backend_index], title[title_index]) &&
          lengths[diagonal] + 1 == length;

      int count = 0;
      if (can_take) {
        count += counts[diagonal];
      }
      if (can_skip_backend) {
        count += counts[skip_backend];
      }
      if (can_skip_title) {
        count += counts[skip_title];
      }
      // The skip-backend and skip-title sets overlap exactly in alignments
      // that skip both current indices. A taken current pair is disjoint.
      if (can_skip_backend && can_skip_title && lengths[diagonal] == length) {
        count -= counts[diagonal];
      }
      counts[current] = static_cast<uint8_t>(std::clamp(count, 0, 2));
    }
  }

  alignment.matched_count = lengths.front();
  alignment.optimal_alignment_count = counts.front();
  if (alignment.matched_count == 0 || alignment.optimal_alignment_count != 1) {
    return alignment;
  }

  size_t backend_index = 0;
  size_t title_index = 0;
  alignment.matches.reserve(alignment.matched_count);
  while (lengths[cell_index(backend_index, title_index)] != 0) {
    const uint32_t length = lengths[cell_index(backend_index, title_index)];
    const size_t diagonal = cell_index(backend_index + 1, title_index + 1);
    if (SameDraw(backend[backend_index], title[title_index]) &&
        lengths[diagonal] + 1 == length) {
      alignment.matches.push_back({
          .backend_offset = backend_index,
          .title_index = title_index,
      });
      ++backend_index;
      ++title_index;
      continue;
    }
    if (lengths[cell_index(backend_index + 1, title_index)] == length) {
      ++backend_index;
      continue;
    }
    if (lengths[cell_index(backend_index, title_index + 1)] == length) {
      ++title_index;
      continue;
    }
    alignment.matches.clear();
    alignment.optimal_alignment_count = 0;
    break;
  }
  return alignment;
}

void BuildUnassignedFamilyReport(MainCoverageFrameSnapshot &snapshot) {
  snapshot.unassigned_ordinals.clear();
  snapshot.unassigned_shader_families.clear();
  for (const MainCoverageDrawSnapshot &draw : snapshot.draws) {
    if (draw.assignment != MainCoverageAssignmentFamily::kUnassigned) {
      continue;
    }
    snapshot.unassigned_ordinals.push_back(draw.original_ordinal);
    const auto family = std::ranges::find_if(
        snapshot.unassigned_shader_families,
        [&](const MainCoverageUnassignedShaderFamily &candidate) {
          return candidate.vertex_shader_hash == draw.vertex_shader_hash &&
                 candidate.pixel_shader_hash == draw.pixel_shader_hash;
        });
    MainCoverageUnassignedShaderFamily *summary = nullptr;
    if (family == snapshot.unassigned_shader_families.end()) {
      snapshot.unassigned_shader_families.push_back({
          .vertex_shader_hash = draw.vertex_shader_hash,
          .pixel_shader_hash = draw.pixel_shader_hash,
      });
      summary = &snapshot.unassigned_shader_families.back();
    } else {
      summary = &*family;
    }
    ++summary->draw_count;
    summary->submitted_index_count += draw.identity.submitted_index_count;
    summary->original_ordinals.push_back(draw.original_ordinal);
  }
  std::ranges::sort(snapshot.unassigned_shader_families,
                    [](const MainCoverageUnassignedShaderFamily &left,
                       const MainCoverageUnassignedShaderFamily &right) {
                      if (left.draw_count != right.draw_count) {
                        return left.draw_count > right.draw_count;
                      }
                      if (left.pixel_shader_hash != right.pixel_shader_hash) {
                        return left.pixel_shader_hash < right.pixel_shader_hash;
                      }
                      return left.vertex_shader_hash < right.vertex_shader_hash;
                    });
}

void CaptureBackendOnlyEvents(const FrameLedger &frame,
                              const TileBlockExtraction &extraction,
                              const TileTitleAlignment &alignment,
                              MainCoverageFrameSnapshot &snapshot) {
  const size_t core_event_count =
      extraction.draws_per_tile *
      MainCoverageFrameSnapshot::kRequiredTileBlockCount;
  const size_t core_end = extraction.start_event + core_event_count;
  std::vector<bool> matched_offsets(extraction.draws_per_tile, false);
  for (const TileTitleMatch &match : alignment.matches) {
    matched_offsets[match.backend_offset] = true;
  }

  snapshot.backend_tile_core_start_event =
      static_cast<uint32_t>(extraction.start_event);
  snapshot.backend_prefix_event_count =
      static_cast<uint32_t>(extraction.start_event);
  snapshot.backend_interleaved_event_count = static_cast<uint32_t>(
      (extraction.draws_per_tile - alignment.matches.size()) *
      MainCoverageFrameSnapshot::kRequiredTileBlockCount);
  snapshot.backend_suffix_event_count =
      static_cast<uint32_t>(frame.backend_events.size() - core_end);
  snapshot.backend_only_event_count = snapshot.backend_prefix_event_count +
                                      snapshot.backend_interleaved_event_count +
                                      snapshot.backend_suffix_event_count;
  snapshot.backend_only_events.reserve(snapshot.backend_only_event_count);

  const auto append = [&](size_t index, MainCoverageBackendOnlyRegion region,
                          uint32_t tile_ordinal, uint32_t tile_event_offset) {
    const BackendEvent &event = frame.backend_events[index];
    snapshot.backend_only_events.push_back({
        .backend_event_index = static_cast<uint32_t>(index),
        .identity = event.identity,
        .vertex_shader_hash = event.vertex_shader_hash,
        .pixel_shader_hash = event.pixel_shader_hash,
        .backend = event.contract,
        .region = region,
        .tile_ordinal = tile_ordinal,
        .tile_event_offset = tile_event_offset,
    });
  };
  for (size_t index = 0; index < extraction.start_event; ++index) {
    append(index, MainCoverageBackendOnlyRegion::kPrefix, 0, 0);
  }
  for (uint32_t tile = 0;
       tile < MainCoverageFrameSnapshot::kRequiredTileBlockCount; ++tile) {
    for (size_t offset = 0; offset < extraction.draws_per_tile; ++offset) {
      if (matched_offsets[offset]) {
        continue;
      }
      append(extraction.start_event +
                 static_cast<size_t>(tile) * extraction.draws_per_tile + offset,
             MainCoverageBackendOnlyRegion::kTile, tile + 1,
             static_cast<uint32_t>(offset));
    }
  }
  for (size_t index = core_end; index < frame.backend_events.size(); ++index) {
    append(index, MainCoverageBackendOnlyRegion::kSuffix, 0, 0);
  }
}

std::shared_ptr<MainCoverageFrameSnapshot>
FinalizeFrame(const FrameLedger &frame) {
  auto snapshot = std::make_shared<MainCoverageFrameSnapshot>();
  snapshot->sequence = frame.sequence;
  snapshot->backend_frame_sequence = frame.sequence;
  if (frame.catalog == nullptr) {
    snapshot->reject_reason = MainCoverageRejectReason::kMissingCatalog;
    return snapshot;
  }
  const SceneDrawCatalogFrame &catalog = *frame.catalog;
  snapshot->catalog_draw_count = catalog.ordered_draw_count;
  snapshot->catalog_dropped_draw_count = catalog.dropped_ordered_draws;
  snapshot->catalog_guest_read_failures = catalog.guest_read_failures;
  snapshot->backend_event_count =
      static_cast<uint32_t>(frame.backend_events.size());
  if (catalog.sequence != frame.sequence) {
    snapshot->reject_reason = MainCoverageRejectReason::kCatalogFrameMismatch;
    return snapshot;
  }
  if (catalog.ordered_draw_count == 0 ||
      catalog.ordered_draw_count > catalog.ordered_draws.size() ||
      catalog.dropped_ordered_draws != 0) {
    snapshot->reject_reason = MainCoverageRejectReason::kCatalogIncomplete;
    return snapshot;
  }
  for (uint32_t index = 0; index < catalog.ordered_draw_count; ++index) {
    if (catalog.ordered_draws[index].frame_sequence != frame.sequence) {
      snapshot->reject_reason = MainCoverageRejectReason::kCatalogFrameMismatch;
      return snapshot;
    }
  }

  std::vector<TileBlockExtraction> extractions;
  snapshot->reject_reason = ProveTileBlocks(frame, extractions);
  if (snapshot->reject_reason != MainCoverageRejectReason::kNone) {
    return snapshot;
  }

  snapshot->backend_tile_event_count =
      static_cast<uint32_t>(extractions.front().draws_per_tile);
  snapshot->backend_maximal_phase_count =
      static_cast<uint32_t>(extractions.size());
  snapshot->backend_maximal_phase_starts.reserve(extractions.size());
  for (const TileBlockExtraction &extraction : extractions) {
    snapshot->backend_maximal_phase_starts.push_back(
        static_cast<uint32_t>(extraction.start_event));
  }

  const std::vector<TitleToken> title = BuildTitleTokens(catalog);
  std::vector<TileTitleAlignment> alignments;
  alignments.reserve(extractions.size());
  uint32_t greatest_match_count = 0;
  size_t greatest_phase_index = 0;
  size_t greatest_phase_count = 0;
  for (size_t phase_index = 0; phase_index < extractions.size();
       ++phase_index) {
    alignments.push_back(
        AlignTileToTitle(extractions[phase_index].first_block, title));
    const uint32_t matched_count = alignments.back().matched_count;
    if (matched_count > greatest_match_count) {
      greatest_match_count = matched_count;
      greatest_phase_index = phase_index;
      greatest_phase_count = 1;
    } else if (matched_count == greatest_match_count) {
      ++greatest_phase_count;
    }
  }
  if (greatest_match_count == 0) {
    snapshot->reject_reason = MainCoverageRejectReason::kNoOrderedTitleJoin;
    return snapshot;
  }
  if (greatest_phase_count != 1) {
    snapshot->reject_reason = MainCoverageRejectReason::kAmbiguousTileBlocks;
    return snapshot;
  }

  const TileBlockExtraction &extraction = extractions[greatest_phase_index];
  const TileTitleAlignment &alignment = alignments[greatest_phase_index];
  snapshot->backend_tile_core_start_event =
      static_cast<uint32_t>(extraction.start_event);
  snapshot->logical_main_draw_count = alignment.matched_count;
  if (alignment.optimal_alignment_count != 1 ||
      alignment.matches.size() != alignment.matched_count) {
    snapshot->reject_reason =
        MainCoverageRejectReason::kAmbiguousOrderedTitleJoin;
    return snapshot;
  }
  for (const TileTitleMatch &match : alignment.matches) {
    const BackendEvent &first =
        frame.backend_events[extraction.start_event + match.backend_offset];
    const BackendEvent &second = frame.backend_events
        [extraction.start_event + extraction.draws_per_tile +
         match.backend_offset];
    const BackendEvent &third = frame.backend_events
        [extraction.start_event + extraction.draws_per_tile * 2 +
         match.backend_offset];
    if (!CoreEventComplete(first, frame.sequence) ||
        !CoreEventComplete(second, frame.sequence) ||
        !CoreEventComplete(third, frame.sequence) ||
        first.contract != second.contract || first.contract != third.contract) {
      snapshot->reject_reason = MainCoverageRejectReason::kTileStateMismatch;
      return snapshot;
    }
  }

  CaptureBackendOnlyEvents(frame, extraction, alignment, *snapshot);
  snapshot->tile_blocks_exact = true;
  snapshot->backend_tile_blocks_matched =
      MainCoverageFrameSnapshot::kRequiredTileBlockCount;
  snapshot->ordered_title_join_exact = true;
  snapshot->matched_catalog_draw_count = alignment.matched_count;
  snapshot->draws.reserve(alignment.matches.size());
  for (const TileTitleMatch &match : alignment.matches) {
    const BackendEvent &event = extraction.first_block[match.backend_offset];
    const TitleToken &token = title[match.title_index];
    if (!snapshot->draws.empty() &&
        snapshot->draws.back().original_ordinal >= token.ordinal) {
      snapshot->reject_reason = MainCoverageRejectReason::kDuplicateOrdinal;
      snapshot->draws.clear();
      return snapshot;
    }
    snapshot->draws.push_back({
        .original_ordinal = token.ordinal,
        .catalog_draw_index = token.catalog_draw_index,
        .identity = event.identity,
        .vertex_shader_hash = event.vertex_shader_hash,
        .pixel_shader_hash = event.pixel_shader_hash,
        .backend = event.contract,
    });
  }

  if (frame.family_order_mismatch) {
    snapshot->reject_reason = MainCoverageRejectReason::kFamilyOrderMismatch;
    return snapshot;
  }
  if (frame.family_assignment_overlap) {
    snapshot->reject_reason =
        MainCoverageRejectReason::kFamilyAssignmentOverlap;
    return snapshot;
  }
  for (const FamilyAssignment &assignment : frame.assignments) {
    const auto draw =
        std::ranges::find(snapshot->draws, assignment.ordinal,
                          &MainCoverageDrawSnapshot::original_ordinal);
    if (draw == snapshot->draws.end() ||
        draw->identity != assignment.identity ||
        (assignment.compare_vertex_shader_hash &&
         draw->vertex_shader_hash != assignment.vertex_shader_hash) ||
        draw->pixel_shader_hash != assignment.pixel_shader_hash) {
      snapshot->reject_reason =
          MainCoverageRejectReason::kFamilyIdentityMismatch;
      return snapshot;
    }
    if (draw->assignment != MainCoverageAssignmentFamily::kUnassigned) {
      snapshot->reject_reason =
          MainCoverageRejectReason::kFamilyAssignmentOverlap;
      return snapshot;
    }
    draw->assignment = assignment.family;
    draw->assignment_proof = assignment.proof;
  }
  snapshot->assigned_draw_count = static_cast<uint32_t>(std::ranges::count_if(
      snapshot->draws, [](const MainCoverageDrawSnapshot &draw) {
        return draw.assignment != MainCoverageAssignmentFamily::kUnassigned;
      }));
  snapshot->unassigned_draw_count =
      snapshot->logical_main_draw_count - snapshot->assigned_draw_count;
  BuildUnassignedFamilyReport(*snapshot);
  snapshot->family_assignments_exact = true;
  snapshot->reject_reason = MainCoverageRejectReason::kNone;
  return snapshot;
}

std::string OrdinalList(std::span<const uint32_t> ordinals) {
  std::string result;
  result.reserve(ordinals.size() * 5);
  for (size_t index = 0; index < ordinals.size(); ++index) {
    if (index != 0) {
      result.push_back(',');
    }
    result += std::to_string(ordinals[index]);
  }
  return result;
}

const char *BackendOnlyRegionName(MainCoverageBackendOnlyRegion region) {
  switch (region) {
  case MainCoverageBackendOnlyRegion::kPrefix:
    return "prefix";
  case MainCoverageBackendOnlyRegion::kTile:
    return "tile";
  case MainCoverageBackendOnlyRegion::kSuffix:
    return "suffix";
  }
  return "unknown";
}

void LogBackendOnlyEvents(const MainCoverageFrameSnapshot &snapshot) {
  for (const MainCoverageBackendOnlyEvent &event :
       snapshot.backend_only_events) {
    REXLOG_INFO(
        "  MAIN backend-only event: index={} region={} tile={} offset={} "
        "primitive={} count={} base={:08X} vs={:016X} ps={:016X} "
        "contract_complete={} covered=false",
        event.backend_event_index, BackendOnlyRegionName(event.region),
        event.tile_ordinal, event.tile_event_offset,
        event.identity.primitive_type, event.identity.submitted_index_count,
        event.identity.physical_index_base, event.vertex_shader_hash,
        event.pixel_shader_hash, event.backend.complete);
  }
}

void LogSnapshot(const MainCoverageFrameSnapshot &snapshot) {
  if (!snapshot.valid()) {
    REXLOG_INFO(
        "Table Tennis MAIN coverage ledger: frame={} rejected={} catalog={} "
        "catalog_read_failures={} backend_events={} raw_tile_events={} "
        "maximal_phases={}[starts={}] tile_core_start={} "
        "backend_only={}[prefix={} interleaved={} suffix={}] "
        "observer_only=true",
        snapshot.sequence, MainCoverageRejectReasonName(snapshot.reject_reason),
        snapshot.catalog_draw_count, snapshot.catalog_guest_read_failures,
        snapshot.backend_event_count, snapshot.backend_tile_event_count,
        snapshot.backend_maximal_phase_count,
        OrdinalList(snapshot.backend_maximal_phase_starts),
        snapshot.backend_tile_core_start_event,
        snapshot.backend_only_event_count, snapshot.backend_prefix_event_count,
        snapshot.backend_interleaved_event_count,
        snapshot.backend_suffix_event_count);
    LogBackendOnlyEvents(snapshot);
    return;
  }
  REXLOG_INFO(
      "Table Tennis MAIN coverage ledger: frame={} logical_main={} "
      "backend_events={} raw_tile_events={} maximal_phases={}[starts={}] "
      "tile_core_start={} tile_blocks={} "
      "backend_only={}[prefix={} interleaved={} suffix={}] "
      "catalog_matched={} catalog_read_failures={} assigned={} unassigned={} "
      "all_ordinals_assigned={} backend_events_covered={} "
      "same_frame_proven={} observer_only=true",
      snapshot.sequence, snapshot.logical_main_draw_count,
      snapshot.backend_event_count, snapshot.backend_tile_event_count,
      snapshot.backend_maximal_phase_count,
      OrdinalList(snapshot.backend_maximal_phase_starts),
      snapshot.backend_tile_core_start_event,
      snapshot.backend_tile_blocks_matched, snapshot.backend_only_event_count,
      snapshot.backend_prefix_event_count,
      snapshot.backend_interleaved_event_count,
      snapshot.backend_suffix_event_count, snapshot.matched_catalog_draw_count,
      snapshot.catalog_guest_read_failures, snapshot.assigned_draw_count,
      snapshot.unassigned_draw_count, snapshot.all_ordinals_assigned(),
      snapshot.all_backend_events_covered(),
      snapshot.all_draws_same_frame_proven());
  LogBackendOnlyEvents(snapshot);
  for (const MainCoverageUnassignedShaderFamily &family :
       snapshot.unassigned_shader_families) {
    REXLOG_INFO("  MAIN unassigned family: vs={:016X} ps={:016X} draws={} "
                "indices={} ordinals={}",
                family.vertex_shader_hash, family.pixel_shader_hash,
                family.draw_count, family.submitted_index_count,
                OrdinalList(family.original_ordinals));
  }
}

void NoteFinalizedSnapshotLocked(
    std::shared_ptr<MainCoverageFrameSnapshot> snapshot) {
  ++g_telemetry.finalized_frames;
  g_telemetry.latest_published_sequence = snapshot->sequence;
  if (snapshot->valid()) {
    ++g_telemetry.valid_frames;
  } else {
    ++g_telemetry.rejected_frames;
    g_telemetry.ambiguous_frames +=
        snapshot->reject_reason ==
            MainCoverageRejectReason::kAmbiguousOrderedTitleJoin ||
        snapshot->reject_reason ==
            MainCoverageRejectReason::kAmbiguousTileBlocks;
    g_telemetry.tile_mismatch_frames +=
        snapshot->reject_reason ==
            MainCoverageRejectReason::kTileIdentityMismatch ||
        snapshot->reject_reason == MainCoverageRejectReason::kTileStateMismatch;
    g_telemetry.family_overlap_frames +=
        snapshot->reject_reason ==
        MainCoverageRejectReason::kFamilyAssignmentOverlap;
  }

  const uint32_t interval =
      REXCVAR_GET(tabletennis_native_main_coverage_log_interval);
  const bool first_valid = snapshot->valid() && !g_logged_first_valid_frame;
  const bool changed_reject =
      !snapshot->valid() &&
      snapshot->reject_reason != g_last_logged_reject_reason;
  if (first_valid || changed_reject || snapshot->sequence % interval == 0) {
    LogSnapshot(*snapshot);
  }
  g_logged_first_valid_frame |= snapshot->valid();
  if (!snapshot->valid()) {
    g_last_logged_reject_reason = snapshot->reject_reason;
  }
  g_published_frame = std::move(snapshot);
}

void FinalizeClosedFramesLocked() {
  for (FrameLedger &frame : g_frames) {
    if (frame.finalized || frame.sequence >= g_latest_backend_sequence) {
      continue;
    }
    frame.finalized = true;
    NoteFinalizedSnapshotLocked(FinalizeFrame(frame));
  }
  while (g_frames.size() > kMaximumRetainedFrames) {
    if (!g_frames.front().finalized) {
      auto rejected = std::make_shared<MainCoverageFrameSnapshot>();
      rejected->sequence = g_frames.front().sequence;
      rejected->reject_reason = MainCoverageRejectReason::kMissingBackendEvents;
      NoteFinalizedSnapshotLocked(std::move(rejected));
    }
    g_frames.pop_front();
  }
  g_telemetry.pending_frames = static_cast<uint32_t>(std::ranges::count_if(
      g_frames, [](const FrameLedger &frame) { return !frame.finalized; }));
  g_telemetry.queued_backend_events = 0;
  for (const FrameLedger &frame : g_frames) {
    g_telemetry.queued_backend_events +=
        static_cast<uint32_t>(frame.backend_events.size());
  }
}

void ResetLocked() {
  g_frames.clear();
  g_published_frame.reset();
  g_telemetry = {};
  g_latest_backend_sequence = 0;
  g_logged_first_valid_frame = false;
  g_last_logged_reject_reason = MainCoverageRejectReason::kNone;
}

ObservedFamilyFrames CaptureObservedFamilyFrames() {
  return {
      .venue_ps328 = LatestVenueFullFamilyFrame(),
      .venue_14d = LatestVenue14DFrameSnapshot(),
      .venue_e33 = LatestVenueE33FrameSnapshot(),
      .crowd_c6 = LatestCrowdBackendProofFrameSnapshot(),
      .player_ca9 = LatestPlayerSkinFrameSnapshot(),
      .player_d47 = LatestD47PlayerFrameSnapshot(),
      .player_6ae = LatestPlayer6AEFrameSnapshot(),
      .net_bb903 = LatestNetBB903FrameSnapshot(),
  };
}

} // namespace

bool MainCoverageFrameSnapshot::valid() const {
  if (sequence == 0 || backend_frame_sequence != sequence ||
      reject_reason != MainCoverageRejectReason::kNone ||
      catalog_draw_count == 0 || catalog_dropped_draw_count != 0 ||
      backend_event_count == 0 || backend_tile_event_count == 0 ||
      backend_maximal_phase_count == 0 ||
      backend_maximal_phase_starts.size() != backend_maximal_phase_count ||
      logical_main_draw_count == 0 ||
      logical_main_draw_count > backend_tile_event_count ||
      backend_event_count != logical_main_draw_count * kRequiredTileBlockCount +
                                 backend_only_event_count ||
      backend_tile_blocks_matched != kRequiredTileBlockCount ||
      backend_tile_core_start_event != backend_prefix_event_count ||
      backend_interleaved_event_count !=
          (backend_tile_event_count - logical_main_draw_count) *
              kRequiredTileBlockCount ||
      backend_prefix_event_count + backend_interleaved_event_count +
              backend_suffix_event_count !=
          backend_only_event_count ||
      backend_only_events.size() != backend_only_event_count ||
      matched_catalog_draw_count != logical_main_draw_count ||
      draws.size() != logical_main_draw_count ||
      assigned_draw_count + unassigned_draw_count != logical_main_draw_count ||
      unassigned_ordinals.size() != unassigned_draw_count ||
      !tile_blocks_exact || !ordered_title_join_exact ||
      !family_assignments_exact) {
    return false;
  }

  const uint32_t core_end = backend_tile_core_start_event +
                            backend_tile_event_count * kRequiredTileBlockCount;
  if (core_end > backend_event_count ||
      backend_suffix_event_count != backend_event_count - core_end) {
    return false;
  }

  bool selected_phase_present = false;
  uint32_t previous_phase_start = 0;
  for (size_t index = 0; index < backend_maximal_phase_starts.size(); ++index) {
    const uint32_t phase_start = backend_maximal_phase_starts[index];
    if ((index != 0 && phase_start <= previous_phase_start) ||
        phase_start + backend_tile_event_count * kRequiredTileBlockCount >
            backend_event_count) {
      return false;
    }
    selected_phase_present |= phase_start == backend_tile_core_start_event;
    previous_phase_start = phase_start;
  }
  if (!selected_phase_present) {
    return false;
  }

  uint32_t prefix_count = 0;
  std::array<uint32_t, kRequiredTileBlockCount> tile_counts{};
  uint32_t suffix_count = 0;
  std::vector<bool> first_tile_uncovered(backend_tile_event_count, false);
  uint32_t previous_backend_index = 0;
  for (size_t index = 0; index < backend_only_events.size(); ++index) {
    const MainCoverageBackendOnlyEvent &event = backend_only_events[index];
    if ((index != 0 && event.backend_event_index <= previous_backend_index) ||
        event.backend_event_index >= backend_event_count) {
      return false;
    }
    previous_backend_index = event.backend_event_index;

    switch (event.region) {
    case MainCoverageBackendOnlyRegion::kPrefix:
      if (event.backend_event_index != prefix_count ||
          event.tile_ordinal != 0 || event.tile_event_offset != 0) {
        return false;
      }
      ++prefix_count;
      break;
    case MainCoverageBackendOnlyRegion::kTile: {
      if (event.backend_event_index < backend_tile_core_start_event ||
          event.backend_event_index >= core_end) {
        return false;
      }
      const uint32_t relative_index =
          event.backend_event_index - backend_tile_core_start_event;
      const uint32_t expected_tile =
          relative_index / backend_tile_event_count + 1;
      const uint32_t expected_offset =
          relative_index % backend_tile_event_count;
      if (event.tile_ordinal != expected_tile ||
          event.tile_event_offset != expected_offset || expected_tile == 0 ||
          expected_tile > kRequiredTileBlockCount) {
        return false;
      }
      const size_t tile_index = expected_tile - 1;
      if (tile_index == 0) {
        first_tile_uncovered[expected_offset] = true;
      } else if (!first_tile_uncovered[expected_offset]) {
        return false;
      }
      ++tile_counts[tile_index];
      break;
    }
    case MainCoverageBackendOnlyRegion::kSuffix:
      if (event.backend_event_index != core_end + suffix_count ||
          event.tile_ordinal != 0 || event.tile_event_offset != 0) {
        return false;
      }
      ++suffix_count;
      break;
    default:
      return false;
    }
  }

  const uint32_t uncovered_offsets =
      backend_tile_event_count - logical_main_draw_count;
  if (prefix_count != backend_prefix_event_count ||
      suffix_count != backend_suffix_event_count ||
      std::ranges::any_of(tile_counts, [uncovered_offsets](uint32_t count) {
        return count != uncovered_offsets;
      })) {
    return false;
  }

  uint32_t previous_ordinal = 0;
  size_t unassigned_index = 0;
  for (const MainCoverageDrawSnapshot &draw : draws) {
    const bool assigned =
        draw.assignment != MainCoverageAssignmentFamily::kUnassigned;
    if (!draw.valid() || draw.original_ordinal <= previous_ordinal ||
        assigned != (draw.assignment_proof !=
                     MainCoverageAssignmentProof::kUnassigned)) {
      return false;
    }
    if (!assigned &&
        (unassigned_index >= unassigned_ordinals.size() ||
         unassigned_ordinals[unassigned_index++] != draw.original_ordinal)) {
      return false;
    }
    previous_ordinal = draw.original_ordinal;
  }
  uint32_t summarized_unassigned_draws = 0;
  if (std::ranges::any_of(unassigned_shader_families,
                          [](const MainCoverageUnassignedShaderFamily &family) {
                            return !family.valid();
                          })) {
    return false;
  }
  for (const MainCoverageUnassignedShaderFamily &family :
       unassigned_shader_families) {
    summarized_unassigned_draws += family.draw_count;
  }
  return unassigned_index == unassigned_ordinals.size() &&
         summarized_unassigned_draws == unassigned_draw_count;
}

bool MainCoverageFrameSnapshot::all_draws_same_frame_proven() const {
  return all_ordinals_assigned() && all_backend_events_covered() &&
         std::ranges::all_of(draws, [](const MainCoverageDrawSnapshot &draw) {
           return draw.assignment_proof ==
                  MainCoverageAssignmentProof::kSameFrameBackendProof;
         });
}

bool MainCoverageLedgerEnabled() {
  return REXCVAR_GET(tabletennis_native_main_coverage_ledger);
}

void ObserveMainCoverageBackendDraw(
    const rex::graphics::NativeGuestDrawContext &context) {
  if (!MainCoverageLedgerEnabled() || !IsMainPassCallback(context)) {
    return;
  }
  MainCoverageDrawIdentity identity = {
      .primitive_type = context.primitive_type,
      .submitted_index_count = context.guest_vertex_or_index_count,
      .physical_index_base =
          context.guest_index_base_valid ? context.guest_index_base : 0,
  };
  BackendEvent event = {
      .sequence = context.backend_frame_sequence,
      .identity = identity,
      .vertex_shader_hash = context.vertex_shader_hash,
      .pixel_shader_hash = context.pixel_shader_hash,
      .contract = CaptureBackendContract(context, identity),
  };

  std::lock_guard lock(g_ledger_mutex);
  ++g_telemetry.backend_events_observed;
  if (event.sequence == 0) {
    ++g_telemetry.backend_events_dropped;
    return;
  }
  g_latest_backend_sequence =
      std::max(g_latest_backend_sequence, event.sequence);
  g_telemetry.latest_backend_sequence = g_latest_backend_sequence;
  AppendEventLocked(std::move(event));
}

void ObserveMainCoverageFamilyFrame(
    std::shared_ptr<const VenueFullFamilyFrame> frame) {
  if (!MainCoverageLedgerEnabled() || frame == nullptr) {
    return;
  }
  std::lock_guard lock(g_ledger_mutex);
  AttachVenuePs328Locked(frame);
}

void ObserveMainCoverageFamilyFrame(
    std::shared_ptr<const Venue14DFrameSnapshot> frame) {
  if (!MainCoverageLedgerEnabled() || frame == nullptr) {
    return;
  }
  std::lock_guard lock(g_ledger_mutex);
  AttachVenue14DLocked(frame);
}

void ObserveMainCoverageFamilyFrame(
    std::shared_ptr<const VenueE33FrameSnapshot> frame) {
  if (!MainCoverageLedgerEnabled() || frame == nullptr) {
    return;
  }
  std::lock_guard lock(g_ledger_mutex);
  AttachVenueE33Locked(frame);
}

void ObserveMainCoverageFamilyFrame(
    std::shared_ptr<const CrowdFrameSnapshot> frame) {
  if (!MainCoverageLedgerEnabled() || frame == nullptr) {
    return;
  }
  std::lock_guard lock(g_ledger_mutex);
  AttachCrowdC6Locked(frame);
}

void ObserveMainCoverageFamilyFrame(
    std::shared_ptr<const PlayerSkinFrameSnapshot> frame) {
  if (!MainCoverageLedgerEnabled() || frame == nullptr) {
    return;
  }
  std::lock_guard lock(g_ledger_mutex);
  AttachPlayerCa9Locked(frame);
}

void ObserveMainCoverageFamilyFrame(
    std::shared_ptr<const D47PlayerFrameSnapshot> frame) {
  if (!MainCoverageLedgerEnabled() || frame == nullptr) {
    return;
  }
  std::lock_guard lock(g_ledger_mutex);
  AttachPlayerD47Locked(frame);
}

void ObserveMainCoverageFamilyFrame(
    std::shared_ptr<const Player6AEFrameSnapshot> frame) {
  if (!MainCoverageLedgerEnabled() || frame == nullptr) {
    return;
  }
  std::lock_guard lock(g_ledger_mutex);
  AttachPlayer6AELocked(frame);
}

void ObserveMainCoverageFamilyFrame(
    std::shared_ptr<const NetBB903FrameSnapshot> frame) {
  if (!MainCoverageLedgerEnabled() || frame == nullptr) {
    return;
  }
  std::lock_guard lock(g_ledger_mutex);
  AttachNetBB903Locked(frame);
}

void MainCoverageLedgerFrameEnd() {
  const bool enabled = MainCoverageLedgerEnabled();
  const std::shared_ptr<const SceneDrawCatalogFrame> catalog =
      enabled ? LatestSceneDrawCatalogFrameSnapshot() : nullptr;
  const ObservedFamilyFrames families =
      enabled ? CaptureObservedFamilyFrames() : ObservedFamilyFrames{};

  std::lock_guard lock(g_ledger_mutex);
  if (!enabled) {
    if (g_was_enabled) {
      ResetLocked();
    }
    g_was_enabled = false;
    return;
  }
  g_was_enabled = true;
  ++g_telemetry.title_frames;
  if (catalog != nullptr && catalog->sequence != 0) {
    g_telemetry.latest_title_sequence =
        std::max(g_telemetry.latest_title_sequence, catalog->sequence);

    FrameLedger *frame = EnsureFrameLocked(catalog->sequence);
    if (frame != nullptr && frame->catalog == nullptr) {
      frame->catalog = catalog;
    }
  }

  AttachObservedFamiliesLocked(families);
  FinalizeClosedFramesLocked();
}

std::shared_ptr<const MainCoverageFrameSnapshot>
LatestMainCoverageFrameSnapshot() {
  std::lock_guard lock(g_ledger_mutex);
  return g_published_frame;
}

MainCoverageLedgerTelemetry LatestMainCoverageLedgerTelemetry() {
  std::lock_guard lock(g_ledger_mutex);
  MainCoverageLedgerTelemetry telemetry = g_telemetry;
  telemetry.pending_frames = static_cast<uint32_t>(std::ranges::count_if(
      g_frames, [](const FrameLedger &frame) { return !frame.finalized; }));
  telemetry.queued_backend_events = 0;
  for (const FrameLedger &frame : g_frames) {
    telemetry.queued_backend_events +=
        static_cast<uint32_t>(frame.backend_events.size());
  }
  return telemetry;
}

const char *
MainCoverageAssignmentFamilyName(MainCoverageAssignmentFamily family) {
  switch (family) {
  case MainCoverageAssignmentFamily::kUnassigned:
    return "unassigned";
  case MainCoverageAssignmentFamily::kVenuePs328:
    return "venue_ps328";
  case MainCoverageAssignmentFamily::kVenue14D:
    return "venue_14d";
  case MainCoverageAssignmentFamily::kVenueE33:
    return "venue_e33";
  case MainCoverageAssignmentFamily::kCrowdC6:
    return "crowd_c6";
  case MainCoverageAssignmentFamily::kPlayerCa9:
    return "player_ca9";
  case MainCoverageAssignmentFamily::kPlayerD47:
    return "player_d47";
  case MainCoverageAssignmentFamily::kPlayer6AE:
    return "player_6ae";
  case MainCoverageAssignmentFamily::kNetBB903:
    return "net_bb903";
  }
  return "unknown";
}

const char *MainCoverageRejectReasonName(MainCoverageRejectReason reason) {
  switch (reason) {
  case MainCoverageRejectReason::kNone:
    return "none";
  case MainCoverageRejectReason::kMissingCatalog:
    return "missing catalog";
  case MainCoverageRejectReason::kCatalogFrameMismatch:
    return "catalog frame mismatch";
  case MainCoverageRejectReason::kCatalogIncomplete:
    return "catalog incomplete";
  case MainCoverageRejectReason::kMissingBackendEvents:
    return "missing backend events";
  case MainCoverageRejectReason::kBackendEventOverflow:
    return "backend event overflow";
  case MainCoverageRejectReason::kInvalidBackendEvent:
    return "invalid backend event";
  case MainCoverageRejectReason::kIncompleteTileBlocks:
    return "incomplete tile blocks";
  case MainCoverageRejectReason::kAmbiguousTileBlocks:
    return "ambiguous tile blocks";
  case MainCoverageRejectReason::kTileIdentityMismatch:
    return "tile identity mismatch";
  case MainCoverageRejectReason::kTileStateMismatch:
    return "tile state mismatch";
  case MainCoverageRejectReason::kNoOrderedTitleJoin:
    return "no ordered title join";
  case MainCoverageRejectReason::kAmbiguousOrderedTitleJoin:
    return "ambiguous ordered title join";
  case MainCoverageRejectReason::kDuplicateOrdinal:
    return "duplicate ordinal";
  case MainCoverageRejectReason::kFamilyOrderMismatch:
    return "family order mismatch";
  case MainCoverageRejectReason::kFamilyAssignmentOverlap:
    return "family assignment overlap";
  case MainCoverageRejectReason::kFamilyIdentityMismatch:
    return "family identity mismatch";
  }
  return "unknown";
}

} // namespace tabletennis::native
