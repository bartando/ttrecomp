#include "native/tabletennis_net_bb903_observer.h"

#include "native/tabletennis_frame_scene.h"
#include "native/tabletennis_main_coverage_ledger.h"
#include "native/tabletennis_material_observer.h"
#include "native/tabletennis_scene_draw_catalog.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <mutex>
#include <utility>
#include <vector>

#include <rex/cvar.h>
#include <rex/graphics/native_guest_renderer.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(
    tabletennis_native_net_bb903_observer, false, "Table Tennis",
    "Observe the real BB903 net payload and prove its two indexed content "
    "draws repeat identically across all three MAIN tiles. Observer-only; "
    "never suppresses or replaces a draw.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace tabletennis::native {
namespace {

namespace nrhi = rex::graphics::nrhi;

constexpr uint32_t kLvlTableRenderableVtable = 0x8204F6D4;
constexpr uint32_t kTriangleListPrimitive = 4;
constexpr uint32_t kNetIndexCount = 4680;
constexpr uint32_t kNetVertexCount = 948;
constexpr uint32_t kNetVertexStride = 96;
constexpr uint32_t kNetStreamSelector = 2;
constexpr uint32_t kGameplayRenderPassKey = 0x0000000E;
constexpr uint32_t kGameplaySurfacePitch = 1280;
constexpr uint32_t kBc3TextureFormat = 20;
constexpr uint32_t kHighPhysicalHeapBase = 0xE0000000;
constexpr uint32_t kHighPhysicalHeapHostPageOffset = 0x1000;
constexpr uint32_t kPhysicalAddressMask = 0x1FFFFFFF;
constexpr uint64_t kContentVertexShaderHash = 0x20EA5AF4BA4E164Bull;
constexpr uint64_t kPixelShaderHash = 0xBB90345BFEEE544Bull;
constexpr size_t kMaximumRetainedFrames = 8;
constexpr size_t kMaximumQueuedBackendEvents = 512;
constexpr std::array<uint32_t, 2> kTextureHandles = {0x00080002, 0x00100006};
constexpr std::array<uint32_t, 2> kTextureWidths = {512, 512};
constexpr std::array<uint32_t, 2> kTextureHeights = {512, 256};

struct BackendEvent {
  uint64_t backend_frame_sequence = 0;
  NetBB903ContentBackendContract content{};
};

struct TitleIdentity {
  uint32_t shader = 0;
  uint32_t model = 0;
  uint32_t geometry_index = 0;
  uint32_t lod = 0;
  bool alternate_pass = false;
  uint32_t pass_descriptor = 0;
  uint32_t program_pair = 0;
  uint32_t vertex_shader = 0;
  uint32_t pixel_shader = 0;
  uint32_t vertex_buffer_alias = 0;
  uint32_t index_buffer_alias = 0;
  bool valid = false;

  bool operator==(const TitleIdentity &) const = default;
};

struct BuildingFrame {
  uint64_t sequence = 0;
  uint32_t title_candidate_count = 0;
  uint32_t dropped_title_draw_count = 0;
  uint32_t missing_mesh_count = 0;
  uint32_t missing_texture_count = 0;
  TitleIdentity title_identity{};
  std::array<NetBB903TitleDrawSnapshot,
             NetBB903FrameSnapshot::kContentDrawCount>
      title_draws{};
};

struct FrameLedger {
  uint64_t sequence = 0;
  BuildingFrame title{};
  uint32_t backend_tile_blocks_matched = 0;
  uint32_t backend_sequence_mismatches = 0;
  bool finalized = false;
  std::vector<BackendEvent> backend_events;
  std::array<NetBB903ContentBackendContract,
             NetBB903FrameSnapshot::kContentDrawCount>
      first_content_contracts{};
};

std::mutex g_observer_mutex;
BuildingFrame g_building_frame;
std::deque<FrameLedger> g_frames;
std::deque<BackendEvent> g_backend_events;
std::shared_ptr<const NetBB903FrameSnapshot> g_published_frame;
NetBB903ObserverTelemetry g_telemetry;
uint64_t g_latest_catalog_sequence = 0;
bool g_announced_complete = false;
bool g_announced_rejection = false;
bool g_announced_raster_tuple = false;
uint32_t g_title_shape_sample_logs = 0;
uint32_t g_eligibility_tuple_sample_logs = 0;
uint32_t g_backend_join_sample_logs = 0;
bool g_announced_backend_mismatch = false;

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

bool ContentHashesMatch(const rex::graphics::NativeGuestDrawContext &context) {
  return context.vertex_shader_hash == kContentVertexShaderHash &&
         context.pixel_shader_hash == kPixelShaderHash;
}

bool ContentShapeMatches(const rex::graphics::NativeGuestDrawContext &context) {
  return ContentHashesMatch(context) &&
         context.primitive_type == kTriangleListPrimitive &&
         context.guest_vertex_or_index_count == kNetIndexCount &&
         context.vertex_or_index_count == kNetIndexCount;
}

bool ExactContentContract(
    const rex::graphics::NativeGuestDrawContext &context) {
  return context.backend == rex::graphics::NativeGuestOutputBackend::kVulkan &&
         ContentShapeMatches(context) && context.indexed &&
         context.guest_index_base_valid && context.guest_index_base != 0 &&
         context.render_pass_key_valid &&
         context.render_pass_key == kGameplayRenderPassKey &&
         context.surface_pitch == kGameplaySurfacePitch &&
         context.draw_state_contract_valid &&
         context.borrowed_attachment_contract_valid &&
         context.color_attachment_count == 1 &&
         context.color_attachment_formats[0] == nrhi::Format::kR8G8B8A8_UNORM &&
         SupportedDepthFormat(context.depth_attachment_format) &&
         context.stencil_attachment_format == context.depth_attachment_format &&
         context.sample_count == 4 &&
         context.sample_mask == std::numeric_limits<uint64_t>::max();
}

NetBB903ContentBackendContract
CaptureContentContract(const rex::graphics::NativeGuestDrawContext &context) {
  NetBB903ContentBackendContract contract;
  contract.vertex_shader_hash = context.vertex_shader_hash;
  contract.pixel_shader_hash = context.pixel_shader_hash;
  contract.primitive_type = context.primitive_type;
  contract.submitted_index_count = context.guest_vertex_or_index_count;
  contract.host_index_count = context.vertex_or_index_count;
  contract.guest_index_base = context.guest_index_base;
  contract.attachments.render_pass_key = context.render_pass_key;
  contract.attachments.surface_pitch = context.surface_pitch;
  for (size_t index = 0;
       index < contract.attachments.color_attachment_formats.size(); ++index) {
    contract.attachments.color_attachment_formats[index] =
        static_cast<uint32_t>(context.color_attachment_formats[index]);
  }
  contract.attachments.color_attachment_count = context.color_attachment_count;
  contract.attachments.depth_attachment_format =
      static_cast<uint32_t>(context.depth_attachment_format);
  contract.attachments.stencil_attachment_format =
      static_cast<uint32_t>(context.stencil_attachment_format);
  contract.attachments.sample_count = context.sample_count;
  contract.attachments.sample_mask = context.sample_mask;
  contract.attachments.valid =
      context.render_pass_key_valid &&
      context.borrowed_attachment_contract_valid &&
      context.render_pass_key == kGameplayRenderPassKey &&
      context.surface_pitch == kGameplaySurfacePitch &&
      context.color_attachment_count == 1 &&
      context.color_attachment_formats[0] == nrhi::Format::kR8G8B8A8_UNORM &&
      SupportedDepthFormat(context.depth_attachment_format) &&
      context.stencil_attachment_format == context.depth_attachment_format &&
      context.sample_count == 4 &&
      context.sample_mask == std::numeric_limits<uint64_t>::max();

  contract.raster.normalized_depth_control = context.normalized_depth_control;
  contract.raster.normalized_color_mask = context.normalized_color_mask;
  contract.raster.color_control = context.color_control;
  contract.raster.blend_control_0 = context.blend_control_0;
  contract.raster.primitive_restart_index = context.primitive_restart_index;
  contract.raster.primitive_restart_enabled = context.primitive_restart_enabled;
  contract.raster.observed = context.draw_state_contract_valid;
  // No BB903-specific TRACE_RASTER reference exists yet. Repetition across
  // the three tiles proves stability, not correctness against the title.
  contract.raster.proven = false;
  contract.valid = ExactContentContract(context);
  return contract;
}

bool EligibilityContentHashesMatch(
    const rex::graphics::NativeGuestDrawEligibilityContext &context) {
  return context.vertex_shader_hash == kContentVertexShaderHash &&
         context.pixel_shader_hash == kPixelShaderHash;
}

bool EligibilityContentShapeMatches(
    const rex::graphics::NativeGuestDrawEligibilityContext &context) {
  return EligibilityContentHashesMatch(context) &&
         context.primitive_type == kTriangleListPrimitive &&
         context.guest_vertex_or_index_count == kNetIndexCount &&
         context.vertex_or_index_count == kNetIndexCount;
}

bool EligibilityIndexedContractMatches(
    const rex::graphics::NativeGuestDrawEligibilityContext &context) {
  return context.backend == rex::graphics::NativeGuestOutputBackend::kVulkan &&
         EligibilityContentShapeMatches(context) &&
         context.guest_index_base != 0 &&
         context.processed_index_buffer_type != 0 &&
         context.processed_index_buffer_present &&
         !context.shader_32bit_index_dma &&
         !context.memexport_writes_possible;
}

bool MeshMatchesTitleDraw(const TableMeshSnapshot &mesh,
                          const SceneCatalogDrawOccurrence &draw) {
  return mesh.valid() && mesh.positions.size() == kNetVertexCount &&
         mesh.indices.size() == kNetIndexCount &&
         mesh.stream_selector == kNetStreamSelector &&
         mesh.source_vertex_alias == draw.mesh.vertex_buffer_alias &&
         mesh.source_index_alias == draw.mesh.index_buffer_alias;
}

std::array<std::shared_ptr<const TableTextureSnapshot>, 2>
CaptureExactTextures(uint8_t *guest_base,
                     const SceneCatalogDrawOccurrence &draw) {
  std::array<std::shared_ptr<const TableTextureSnapshot>, 2> selected{};
  for (size_t slot = 0; slot < selected.size(); ++slot) {
    const auto texture = CaptureTextureSnapshot(
        guest_base, draw.scope.shader, kTextureHandles[slot],
        draw.state.texture_fetches[slot]);
    if (texture != nullptr && texture->valid() &&
        texture->owner_shader == draw.scope.shader &&
        texture->encoded_handle == kTextureHandles[slot] &&
        texture->width == kTextureWidths[slot] &&
        texture->height == kTextureHeights[slot] &&
        texture->format == kBc3TextureFormat &&
        texture->fetch_words == draw.state.texture_fetches[slot]) {
      selected[slot] = texture;
    }
  }
  return selected;
}

bool DirectTableOwnerMatches(const SceneCatalogDrawOccurrence &draw) {
  return draw.owner.valid && draw.owner.kind == SceneOwnerKind::kTable &&
         draw.owner.role == SceneOwnerRole::kTableRenderable &&
         draw.owner.renderable_vtable == kLvlTableRenderableVtable;
}

bool ExactTitleStructure(const SceneCatalogDrawOccurrence &draw) {
  return draw.player == 0 && draw.scope_valid && draw.scope.shader != 0 &&
         draw.scope.model != 0 && draw.scope.geometry_index == 0 &&
         draw.scope.lod == 0 && draw.scope.alternate_pass && draw.pass.valid &&
         draw.pass.pass_descriptor != 0 && draw.pass.program_pair != 0 &&
         draw.pass.vertex_shader != 0 && draw.pass.pixel_shader != 0 &&
         draw.mesh.valid && draw.primitive_type == kTriangleListPrimitive &&
         draw.mesh.aggregate_primitive_type == kTriangleListPrimitive &&
         draw.submitted_index_count == kNetIndexCount &&
         draw.mesh.aggregate_index_count == kNetIndexCount &&
         draw.mesh.stream_selector == kNetStreamSelector &&
         draw.mesh.vertex_stride == kNetVertexStride &&
         draw.mesh.index_element_size == sizeof(uint16_t) &&
         !draw.mesh.index_is_32_bit && draw.state.valid && draw.world_valid &&
         draw.world_view_projection_valid &&
         PhysicalAddressForVirtualAlias(draw.mesh.index_buffer_alias) != 0;
}

TitleIdentity IdentityForDraw(const SceneCatalogDrawOccurrence &draw) {
  TitleIdentity identity;
  identity.shader = draw.scope.shader;
  identity.model = draw.scope.model;
  identity.geometry_index = draw.scope.geometry_index;
  identity.lod = draw.scope.lod;
  identity.alternate_pass = draw.scope.alternate_pass;
  identity.pass_descriptor = draw.pass.pass_descriptor;
  identity.program_pair = draw.pass.program_pair;
  identity.vertex_shader = draw.pass.vertex_shader;
  identity.pixel_shader = draw.pass.pixel_shader;
  identity.vertex_buffer_alias = draw.mesh.vertex_buffer_alias;
  identity.index_buffer_alias = draw.mesh.index_buffer_alias;
  identity.valid = true;
  return identity;
}

NetBB903TitleDrawSnapshot
CaptureTitleDraw(uint8_t *guest_base,
                 const SceneCatalogDrawOccurrence &draw, bool &missing_mesh,
                 uint32_t &missing_textures) {
  NetBB903TitleDrawSnapshot snapshot;
  snapshot.ordinal = draw.ordinal;
  snapshot.owner = draw.owner.owner;
  snapshot.renderable = draw.owner.renderable;
  snapshot.shader = draw.scope.shader;
  snapshot.model = draw.scope.model;
  snapshot.pass_descriptor = draw.pass.pass_descriptor;
  snapshot.program_pair = draw.pass.program_pair;
  snapshot.title_vertex_shader = draw.pass.vertex_shader;
  snapshot.title_pixel_shader = draw.pass.pixel_shader;
  snapshot.primitive_type = draw.primitive_type;
  snapshot.submitted_index_count = draw.submitted_index_count;
  snapshot.physical_index_base =
      PhysicalAddressForVirtualAlias(draw.mesh.index_buffer_alias);
  snapshot.world_hash = draw.world_hash;
  snapshot.world_view_projection_hash = draw.world_view_projection_hash;
  snapshot.world = draw.world;
  snapshot.world_view_projection = draw.world_view_projection;
  snapshot.vertex_constants_12_15 = draw.state.vertex_constants_12_15;
  snapshot.texture_fetches = draw.state.texture_fetches;

  snapshot.mesh = LatestTableMeshSnapshot();
  missing_mesh =
      snapshot.mesh == nullptr || !MeshMatchesTitleDraw(*snapshot.mesh, draw);
  if (missing_mesh) {
    snapshot.mesh.reset();
  }

  snapshot.textures = CaptureExactTextures(guest_base, draw);
  missing_textures = static_cast<uint32_t>(std::ranges::count(
      snapshot.textures, std::shared_ptr<const TableTextureSnapshot>{}));
  snapshot.valid = !missing_mesh && missing_textures == 0 &&
                   snapshot.physical_index_base != 0;
  return snapshot;
}

FrameLedger *FindPendingFrameLocked(uint64_t sequence) {
  const auto found = std::ranges::find_if(
      g_frames, [sequence](const FrameLedger &frame) {
        return !frame.finalized && frame.sequence == sequence &&
               frame.title.title_candidate_count != 0;
      });
  return found == g_frames.end() ? nullptr : &*found;
}

bool MatchesContentSlot(const FrameLedger &frame, uint32_t slot,
                        const BackendEvent &event) {
  if (event.backend_frame_sequence != frame.sequence ||
      slot >= NetBB903FrameSnapshot::kContentDrawCount ||
      !event.content.valid) {
    return false;
  }
  const NetBB903TitleDrawSnapshot &title = frame.title.title_draws[slot];
  return title.physical_index_base != 0 &&
         title.physical_index_base == event.content.guest_index_base;
}

void RecordMismatch(FrameLedger &frame) {
  ++frame.backend_sequence_mismatches;
  ++g_telemetry.backend_sequence_mismatches;
}

void PublishFrameLocked(FrameLedger &frame) {
  auto published = std::make_shared<NetBB903FrameSnapshot>();
  published->sequence = frame.sequence;
  published->backend_frame_sequence = frame.sequence;
  published->title_candidate_count = frame.title.title_candidate_count;
  published->dropped_title_draw_count = frame.title.dropped_title_draw_count;
  published->missing_mesh_count = frame.title.missing_mesh_count;
  published->missing_texture_count = frame.title.missing_texture_count;
  published->backend_event_count =
      static_cast<uint32_t>(frame.backend_events.size());
  if (published->backend_event_count %
          NetBB903FrameSnapshot::kTileBlockCount ==
      0) {
    published->backend_draws_per_tile =
        published->backend_event_count /
        NetBB903FrameSnapshot::kTileBlockCount;
  }
  published->backend_tile_blocks_matched = frame.backend_tile_blocks_matched;
  published->backend_sequence_mismatches = frame.backend_sequence_mismatches;
  published->title_draws = frame.title.title_draws;
  published->content_backend = frame.first_content_contracts;
  for (const NetBB903TitleDrawSnapshot &title : published->title_draws) {
    published->valid_title_draw_count += title.valid;
    if (published->mesh == nullptr && title.mesh != nullptr) {
      published->mesh = title.mesh;
      published->textures = title.textures;
    }
  }

  g_published_frame = std::move(published);
  ObserveMainCoverageFamilyFrame(g_published_frame);
  frame.finalized = true;
  ++g_telemetry.finalized_frames;
  g_telemetry.latest_published_sequence = frame.sequence;
  g_telemetry.raster_tuple_observed =
      g_published_frame->content_backend[0].raster.observed &&
      g_published_frame->content_backend[1].raster.observed;
  // Intentionally never promoted from observation in this module.
  g_telemetry.raster_tuple_proven = false;
  g_telemetry.serving_enabled = false;

  if (g_published_frame->observer_complete()) {
    ++g_telemetry.observer_complete_frames;
    if (!g_announced_complete) {
      g_announced_complete = true;
      REXLOG_INFO("Table Tennis BB903 net observer: frame={} backend_frame={} "
                  "title_draws=2 "
                  "mesh_vertices=948 mesh_indices=4680 "
                  "backend_events=6 draws_per_tile=2 tile_blocks=3 "
                  "renderable_draws=2 renderable_indices=9360 "
                  "wvp_hash_equal={} "
                  "raster_observed=true raster_proven=false "
                  "observer_only=true guest_suppressed=false serve_ready=false",
                  g_published_frame->sequence,
                  g_published_frame->backend_frame_sequence,
                  g_published_frame->title_draws[0]
                          .world_view_projection_hash ==
                      g_published_frame->title_draws[1]
                          .world_view_projection_hash);
    }
  } else {
    ++g_telemetry.rejected_frames;
    if (!g_announced_rejection) {
      g_announced_rejection = true;
      REXLOG_INFO(
          "Table Tennis BB903 net observer: rejected frame={} "
          "title_candidates={} valid_title={} dropped={} missing_mesh={} "
          "missing_textures={} backend_events={} draws_per_tile={} "
          "tile_blocks={} mismatches={} "
          "raster_proven=false observer_only=true guest_suppressed=false",
          g_published_frame->sequence, g_published_frame->title_candidate_count,
          g_published_frame->valid_title_draw_count,
          g_published_frame->dropped_title_draw_count,
          g_published_frame->missing_mesh_count,
          g_published_frame->missing_texture_count,
          g_published_frame->backend_event_count,
          g_published_frame->backend_draws_per_tile,
          g_published_frame->backend_tile_blocks_matched,
          g_published_frame->backend_sequence_mismatches);
    }
  }

  if (!g_announced_raster_tuple && g_telemetry.raster_tuple_observed) {
    g_announced_raster_tuple = true;
    const NetBB903RasterTuple &first =
        g_published_frame->content_backend[0].raster;
    const NetBB903RasterTuple &second =
        g_published_frame->content_backend[1].raster;
    REXLOG_INFO("Table Tennis BB903 net observer: unproven raster tuples "
                "draw0[depth={:08X} mask={:08X} color={:08X} blend={:08X} "
                "restart={:08X}/{}] "
                "draw1[depth={:08X} mask={:08X} color={:08X} blend={:08X} "
                "restart={:08X}/{}] verification_required=true",
                first.normalized_depth_control, first.normalized_color_mask,
                first.color_control, first.blend_control_0,
                first.primitive_restart_index, first.primitive_restart_enabled,
                second.normalized_depth_control, second.normalized_color_mask,
                second.color_control, second.blend_control_0,
                second.primitive_restart_index,
                second.primitive_restart_enabled);
  }
}

void AnnounceBackendMismatch(const FrameLedger &frame, const char *reason,
                             size_t event_index,
                             const BackendEvent *expected,
                             const BackendEvent *observed) {
  if (g_announced_backend_mismatch) {
    return;
  }
  g_announced_backend_mismatch = true;
  const BackendEvent empty{};
  const BackendEvent &expected_event =
      expected != nullptr ? *expected : empty;
  const BackendEvent &observed_event =
      observed != nullptr ? *observed : empty;
  REXLOG_INFO(
      "Table Tennis BB903 net observer: frame-bucket mismatch "
      "title_frame={} backend_frame={} reason={} event={} "
      "backend_events={} title_candidates={} "
      "expected[guest_count={} host_count={} base={:08X}] "
      "observed[guest_count={} host_count={} base={:08X}] "
      "observer_only=true",
      frame.sequence, observed_event.backend_frame_sequence, reason,
      event_index, frame.backend_events.size(),
      frame.title.title_candidate_count,
      expected_event.content.submitted_index_count,
      expected_event.content.host_index_count,
      expected_event.content.guest_index_base,
      observed_event.content.submitted_index_count,
      observed_event.content.host_index_count,
      observed_event.content.guest_index_base);
}

void AnalyzeFrameLocked(FrameLedger &frame) {
  ++g_telemetry.backend_frames_analyzed;
  const size_t event_count = frame.backend_events.size();
  if (event_count == 0 ||
      event_count % NetBB903FrameSnapshot::kTileBlockCount != 0) {
    AnnounceBackendMismatch(frame, "event-count-not-three-blocks", 0, nullptr,
                            nullptr);
    RecordMismatch(frame);
    PublishFrameLocked(frame);
    return;
  }

  const size_t draws_per_tile =
      event_count / NetBB903FrameSnapshot::kTileBlockCount;
  if (draws_per_tile != frame.title.title_candidate_count ||
      draws_per_tile != NetBB903FrameSnapshot::kContentDrawCount) {
    AnnounceBackendMismatch(frame, "draw-count-title-mismatch", 0, nullptr,
                            &frame.backend_events.front());
    RecordMismatch(frame);
    PublishFrameLocked(frame);
    return;
  }

  // The first block is derived from this completed backend frame. The next
  // two blocks must repeat the exact immutable draw and state contracts.
  for (size_t tile = 1; tile < NetBB903FrameSnapshot::kTileBlockCount; ++tile) {
    for (size_t draw = 0; draw < draws_per_tile; ++draw) {
      const BackendEvent &expected = frame.backend_events[draw];
      const BackendEvent &observed =
          frame.backend_events[tile * draws_per_tile + draw];
      if (expected.backend_frame_sequence != frame.sequence ||
          observed.backend_frame_sequence != frame.sequence ||
          expected.content != observed.content) {
        AnnounceBackendMismatch(frame, "tile-block-contract",
                                tile * draws_per_tile + draw, &expected,
                                &observed);
        RecordMismatch(frame);
        PublishFrameLocked(frame);
        return;
      }
    }
  }

  for (size_t draw = 0; draw < draws_per_tile; ++draw) {
    const BackendEvent &event = frame.backend_events[draw];
    if (!MatchesContentSlot(frame, static_cast<uint32_t>(draw), event)) {
      AnnounceBackendMismatch(frame, "backend-title-index-base", draw, nullptr,
                              &event);
      RecordMismatch(frame);
      PublishFrameLocked(frame);
      return;
    }
    frame.first_content_contracts[draw] = event.content;
  }

  frame.backend_tile_blocks_matched = NetBB903FrameSnapshot::kTileBlockCount;
  g_telemetry.backend_tile_blocks_matched +=
      NetBB903FrameSnapshot::kTileBlockCount;
  PublishFrameLocked(frame);
}

void ReconcileBackendEventsLocked() {
  auto event = g_backend_events.begin();
  while (event != g_backend_events.end()) {
    FrameLedger *frame =
        FindPendingFrameLocked(event->backend_frame_sequence);
    if (frame != nullptr) {
      if (frame->backend_events.empty() && g_backend_join_sample_logs < 8) {
        ++g_backend_join_sample_logs;
        REXLOG_INFO(
            "Table Tennis BB903 exact frame join: title_frame={} "
            "backend_frame={} equal={} observer_only=true",
            frame->sequence, event->backend_frame_sequence,
            frame->sequence == event->backend_frame_sequence);
      }
      ++g_telemetry.backend_events_joined;
      frame->backend_events.push_back(std::move(*event));
      event = g_backend_events.erase(event);
      continue;
    }
    if (event->backend_frame_sequence <= g_latest_catalog_sequence) {
      ++g_telemetry.backend_events_without_title_frame;
      event = g_backend_events.erase(event);
      continue;
    }
    ++event;
  }
}

void AnalyzeCompletedFramesLocked() {
  const uint64_t completed_before =
      g_telemetry.latest_backend_frame_sequence;
  for (FrameLedger &frame : g_frames) {
    if (!frame.finalized && frame.sequence < completed_before) {
      AnalyzeFrameLocked(frame);
    }
  }
}

void QueueBackendEventLocked(BackendEvent event) {
  if (g_backend_events.size() == kMaximumQueuedBackendEvents) {
    g_backend_events.pop_front();
    ++g_telemetry.backend_events_dropped;
  }
  g_backend_events.push_back(std::move(event));
  ReconcileBackendEventsLocked();
  AnalyzeCompletedFramesLocked();
}

void ExpireFramesLocked() {
  while (g_frames.size() > kMaximumRetainedFrames) {
    if (!g_frames.front().finalized) {
      ++g_telemetry.expired_frames;
    }
    g_frames.pop_front();
  }
}

void UpdatePendingCountsLocked() {
  g_telemetry.pending_frames = static_cast<uint32_t>(
      std::ranges::count_if(g_frames, [](const FrameLedger &frame) {
        return !frame.finalized && frame.title.title_candidate_count != 0;
      }));
  g_telemetry.queued_backend_events =
      static_cast<uint32_t>(g_backend_events.size());
}

void ResetObserverLocked() {
  g_building_frame = {};
  g_frames.clear();
  g_backend_events.clear();
  g_published_frame.reset();
  g_telemetry = {};
  g_latest_catalog_sequence = 0;
  g_announced_complete = false;
  g_announced_rejection = false;
  g_announced_raster_tuple = false;
  g_announced_backend_mismatch = false;
  g_title_shape_sample_logs = 0;
  g_eligibility_tuple_sample_logs = 0;
  g_backend_join_sample_logs = 0;
}

} // namespace

bool NetBB903FrameSnapshot::observer_complete() const {
  if (sequence == 0 || backend_frame_sequence != sequence ||
      title_candidate_count != kContentDrawCount ||
      valid_title_draw_count != kContentDrawCount ||
      dropped_title_draw_count != 0 || missing_mesh_count != 0 ||
      missing_texture_count != 0 ||
      backend_event_count != kContentDrawCount * kTileBlockCount ||
      backend_draws_per_tile != kContentDrawCount ||
      backend_tile_blocks_matched != kTileBlockCount ||
      backend_sequence_mismatches != 0 || mesh == nullptr || !mesh->valid() ||
      mesh->positions.size() != kNetVertexCount ||
      mesh->indices.size() != kNetIndexCount || textures[0] == nullptr ||
      textures[1] == nullptr || !textures[0]->valid() ||
      !textures[1]->valid()) {
    return false;
  }
  if (title_draws[0].ordinal >= title_draws[1].ordinal ||
      content_backend[0].attachments != content_backend[1].attachments) {
    return false;
  }
  for (size_t index = 0; index < title_draws.size(); ++index) {
    const NetBB903TitleDrawSnapshot &draw = title_draws[index];
    if (!draw.valid || draw.mesh != mesh ||
        draw.submitted_index_count != kNetIndexCount ||
        draw.physical_index_base != content_backend[index].guest_index_base) {
      return false;
    }
    for (size_t slot = 0; slot < textures.size(); ++slot) {
      if (draw.textures[slot] != textures[slot]) {
        return false;
      }
    }
  }
  for (const NetBB903ContentBackendContract &draw : content_backend) {
    if (!draw.valid || !draw.attachments.valid || !draw.raster.observed ||
        draw.raster.proven ||
        draw.vertex_shader_hash != kContentVertexShaderHash ||
        draw.pixel_shader_hash != kPixelShaderHash ||
        draw.submitted_index_count != kNetIndexCount ||
        draw.host_index_count != kNetIndexCount) {
      return false;
    }
  }
  return true;
}

bool NetBB903ObserverEnabled() {
  return REXCVAR_GET(tabletennis_native_net_bb903_observer) ||
         NativeFrameSceneCaptureEnabled();
}

void ObserveNetBB903TitleDraw(
    uint8_t *guest_base, const SceneCatalogDrawOccurrence &draw) {
  if (!NetBB903ObserverEnabled()) {
    return;
  }
  {
    std::lock_guard lock(g_observer_mutex);
    ++g_telemetry.title_draws_observed;
    g_latest_catalog_sequence =
        std::max(g_latest_catalog_sequence, draw.frame_sequence);
  }

  const bool shape_matches =
      draw.player == 0 &&
      draw.primitive_type == kTriangleListPrimitive &&
      draw.submitted_index_count == kNetIndexCount;
  if (!shape_matches) {
    return;
  }
  const bool material_matches = CurrentTableVisibleMaterialPass();
  const bool owner_matches = DirectTableOwnerMatches(draw);
  const bool scope_matches =
      draw.scope_valid && draw.scope.shader != 0 &&
      draw.scope.model != 0 && draw.scope.geometry_index == 0 &&
      draw.scope.lod == 0 && draw.scope.alternate_pass;
  const bool pass_matches =
      draw.pass.valid &&
      draw.pass.pass_descriptor != 0 && draw.pass.program_pair != 0 &&
      draw.pass.vertex_shader != 0 && draw.pass.pixel_shader != 0;
  const bool mesh_matches =
      draw.mesh.valid &&
      draw.mesh.aggregate_primitive_type == kTriangleListPrimitive &&
      draw.mesh.aggregate_index_count == kNetIndexCount &&
      draw.mesh.stream_selector == kNetStreamSelector &&
      draw.mesh.vertex_stride == kNetVertexStride &&
      draw.mesh.index_element_size == sizeof(uint16_t) &&
      !draw.mesh.index_is_32_bit;
  const bool state_matches = draw.state.valid;
  const bool transform_matches =
      draw.world_valid && draw.world_view_projection_valid;
  const bool physical_index_matches =
      PhysicalAddressForVirtualAlias(draw.mesh.index_buffer_alias) != 0;
  bool log_shape_sample = false;
  {
    std::lock_guard lock(g_observer_mutex);
    ++g_telemetry.title_shape_matches;
    g_telemetry.title_material_matches += material_matches;
    g_telemetry.title_owner_matches += owner_matches;
    g_telemetry.title_scope_matches += scope_matches;
    g_telemetry.title_pass_matches += pass_matches;
    g_telemetry.title_mesh_matches += mesh_matches;
    g_telemetry.title_state_matches += state_matches;
    g_telemetry.title_transform_matches += transform_matches;
    g_telemetry.title_physical_index_matches +=
        physical_index_matches;
    log_shape_sample = g_title_shape_sample_logs < 8;
    g_title_shape_sample_logs += log_shape_sample;
  }
  if (log_shape_sample) {
    REXLOG_INFO(
        "Table Tennis BB903 title gate sample: ordinal={} material={} "
        "owner[valid={} kind={} role={} vtable={:08X}] "
        "scope[valid={} shader={:08X} model={:08X} geom={} lod={} alt={}] "
        "pass[valid={} desc={:08X} program={:08X}] "
        "mesh[valid={} aggregate_primitive={} aggregate_indices={} "
        "selector={} stride={} index_size={} index32={}] "
        "state={} transforms={}/{} physical_index={} observer_only=true",
        draw.ordinal, material_matches, draw.owner.valid,
        static_cast<uint32_t>(draw.owner.kind),
        static_cast<uint32_t>(draw.owner.role),
        draw.owner.renderable_vtable, draw.scope_valid,
        draw.scope.shader, draw.scope.model, draw.scope.geometry_index,
        draw.scope.lod, draw.scope.alternate_pass, draw.pass.valid,
        draw.pass.pass_descriptor, draw.pass.program_pair, draw.mesh.valid,
        draw.mesh.aggregate_primitive_type,
        draw.mesh.aggregate_index_count, draw.mesh.stream_selector,
        draw.mesh.vertex_stride, draw.mesh.index_element_size,
        draw.mesh.index_is_32_bit, draw.state.valid, draw.world_valid,
        draw.world_view_projection_valid, physical_index_matches);
  }
  if (!physical_index_matches || !ExactTitleStructure(draw)) {
    return;
  }

  uint32_t slot = 0;
  {
    std::lock_guard lock(g_observer_mutex);
    if (draw.frame_sequence == 0) {
      ++g_building_frame.dropped_title_draw_count;
      return;
    }
    if (g_building_frame.sequence == 0) {
      g_building_frame.sequence = draw.frame_sequence;
    } else if (g_building_frame.sequence != draw.frame_sequence) {
      ++g_building_frame.dropped_title_draw_count;
      return;
    }
    const TitleIdentity identity = IdentityForDraw(draw);
    if (!g_building_frame.title_identity.valid) {
      if (!owner_matches) {
        return;
      }
      g_building_frame.title_identity = identity;
      ++g_telemetry.title_anchor_matches;
    } else {
      if (draw.owner.valid || identity != g_building_frame.title_identity) {
        return;
      }
      ++g_telemetry.title_identity_matches;
      ++g_telemetry.title_sibling_matches;
    }

    slot = g_building_frame.title_candidate_count++;
    ++g_telemetry.title_candidates;
    if (slot >= NetBB903FrameSnapshot::kContentDrawCount) {
      ++g_building_frame.dropped_title_draw_count;
      return;
    }
  }

  bool missing_mesh = false;
  uint32_t missing_textures = 0;
  NetBB903TitleDrawSnapshot snapshot =
      CaptureTitleDraw(guest_base, draw, missing_mesh, missing_textures);

  std::lock_guard lock(g_observer_mutex);
  g_telemetry.valid_title_draws += snapshot.valid;
  g_telemetry.title_missing_mesh += missing_mesh;
  g_telemetry.title_missing_textures += missing_textures;
  g_building_frame.missing_mesh_count += missing_mesh;
  g_building_frame.missing_texture_count += missing_textures;
  g_building_frame.title_draws[slot] = std::move(snapshot);
}

void ObserveNetBB903BackendDraw(
    const rex::graphics::NativeGuestDrawContext &context) {
  if (!NetBB903ObserverEnabled()) {
    return;
  }
  const bool hashes_match = ContentHashesMatch(context);
  {
    std::lock_guard lock(g_observer_mutex);
    ++g_telemetry.backend_content_callbacks;
    g_telemetry.latest_backend_frame_sequence =
        std::max(g_telemetry.latest_backend_frame_sequence,
                 context.backend_frame_sequence);
    g_telemetry.backend_content_hash_matches += hashes_match;
    g_telemetry.backend_content_early_probes +=
        hashes_match && (!context.render_pass_key_valid ||
                         !context.borrowed_attachment_contract_valid);
  }
  if (!ExactContentContract(context) ||
      context.backend_frame_sequence == 0) {
    return;
  }

  BackendEvent event;
  event.backend_frame_sequence = context.backend_frame_sequence;
  event.content = CaptureContentContract(context);
  std::lock_guard lock(g_observer_mutex);
  ++g_telemetry.backend_content_contract_matches;
  QueueBackendEventLocked(std::move(event));
  UpdatePendingCountsLocked();
}

void ObserveNetBB903BackendEligibility(
    const rex::graphics::NativeGuestDrawEligibilityContext &context) {
  if (!NetBB903ObserverEnabled()) {
    return;
  }
  const bool pixel_hash_matches =
      context.pixel_shader_hash == kPixelShaderHash;
  const bool hashes_match = EligibilityContentHashesMatch(context);
  const bool shape_matches = EligibilityContentShapeMatches(context);
  const bool indexed_matches = EligibilityIndexedContractMatches(context);
  bool log_tuple = false;
  {
    std::lock_guard lock(g_observer_mutex);
    ++g_telemetry.backend_eligibility_callbacks;
    g_telemetry.latest_backend_frame_sequence =
        std::max(g_telemetry.latest_backend_frame_sequence,
                 context.backend_frame_sequence);
    g_telemetry.backend_eligibility_pixel_hash_matches += pixel_hash_matches;
    g_telemetry.backend_eligibility_content_hash_matches += hashes_match;
    g_telemetry.backend_eligibility_content_shape_matches += shape_matches;
    g_telemetry.backend_eligibility_indexed_matches += indexed_matches;
    g_telemetry.backend_eligibility_eligible_matches +=
        indexed_matches && context.eligible;
    // This callback is one-per-guest-draw after primitive processing. Bounded
    // BB903 samples document that its six live events are the two indexed net
    // draws replayed across three tiles, not the trace viewer's 3-index rows.
    log_tuple =
        pixel_hash_matches && g_eligibility_tuple_sample_logs < 24;
    g_eligibility_tuple_sample_logs += log_tuple;
  }
  if (log_tuple) {
    REXLOG_INFO(
        "Table Tennis BB903 eligibility tuple sample: backend_frame={} "
        "vs={:016X} ps={:016X} primitive={} guest_count={} host_count={} "
        "index_type={} index_present={} shader32_dma={} memexport={} "
        "device={} replacer={} host_targets={} eligible={} observer_only=true",
        context.backend_frame_sequence, context.vertex_shader_hash,
        context.pixel_shader_hash, context.primitive_type,
        context.guest_vertex_or_index_count, context.vertex_or_index_count,
        context.processed_index_buffer_type,
        context.processed_index_buffer_present,
        context.shader_32bit_index_dma, context.memexport_writes_possible,
        context.native_rhi_device_available, context.draw_replacer_available,
        context.host_render_targets, context.eligible);
  }
}

void NetBB903ObserverFrameEnd() {
  const bool enabled = NetBB903ObserverEnabled();
  std::lock_guard lock(g_observer_mutex);
  if (!enabled) {
    ResetObserverLocked();
    return;
  }

  ++g_telemetry.title_frames;
  g_telemetry.latest_title_sequence = g_latest_catalog_sequence;
  if (g_building_frame.title_candidate_count != 0) {
    FrameLedger frame;
    frame.sequence = g_building_frame.sequence;
    frame.title = std::move(g_building_frame);
    g_frames.push_back(std::move(frame));
  }
  g_building_frame = {};

  ReconcileBackendEventsLocked();
  AnalyzeCompletedFramesLocked();
  ExpireFramesLocked();
  UpdatePendingCountsLocked();
}

std::shared_ptr<const NetBB903FrameSnapshot> LatestNetBB903FrameSnapshot() {
  std::lock_guard lock(g_observer_mutex);
  return g_published_frame;
}

NetBB903ObserverTelemetry LatestNetBB903ObserverTelemetry() {
  std::lock_guard lock(g_observer_mutex);
  return g_telemetry;
}

} // namespace tabletennis::native
