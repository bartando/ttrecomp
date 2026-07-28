#include "native/tabletennis_venue_526a_observer.h"

#include "generated/default/tabletennis_init.h"
#include "native/tabletennis_guest_memory.h"
#include "native/tabletennis_scene_draw_catalog.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <string_view>
#include <utility>
#include <vector>

#include <rex/cvar.h>
#include <rex/graphics/native_guest_renderer.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(
    tabletennis_native_venue_526a_observer, false, "Table Tennis",
    "Capture and prove the mixed-layout 526A MAIN family from real title "
    "draws and exact same-frame backend tile repetition. Observer-only; "
    "never suppresses or replaces a draw.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace tabletennis::native {
namespace {

namespace nrhi = rex::graphics::nrhi;

constexpr uint64_t kVertexShader37F2 = 0x37F2AEC8A23E44E0ull;
constexpr uint64_t kVertexShader1FCF = 0x1FCFF2D75A7DCA98ull;
constexpr uint64_t kPixelShader526A = 0x526A35DC94475116ull;
constexpr uint32_t kGrmShaderFxVtable = 0x8202F2DC;
constexpr uint32_t kGameplayRenderPassKey = 0x0000000E;
constexpr uint32_t kGameplaySurfacePitch = 1280;
constexpr uint32_t kTriangleStripPrimitive = 6;
constexpr uint32_t kNormalizedDepthControl = 0x00700736;
constexpr uint32_t kNormalizedColorMask = 0x00000007;
constexpr uint32_t kColorControl = 0x87000005;
constexpr uint32_t kOpaqueBlendControl = 0x00010001;
constexpr uint32_t kLateBlendControl = 0x07060706;
constexpr uint32_t kRasterizerModeControl = 0x00018002;
constexpr uint32_t kStride37F2 = 32;
constexpr uint32_t kStride1FCF = 40;
constexpr uint32_t kEndian8In32 = 2;
constexpr uint32_t kPhysicalAddressMask = 0x1FFFFFFF;
constexpr uint32_t kHighPhysicalHeapBase = 0xE0000000;
constexpr uint32_t kHighPhysicalHeapHostPageOffset = 0x1000;
constexpr size_t kMaximumCandidatesPerFrame = 64;
constexpr size_t kMaximumBackendEventsPerFrame = 192;
constexpr size_t kMaximumRetainedFrames = 12;

struct BackendEvent {
  Venue526ADrawIdentity identity{};
  Venue526ABackendContract contract{};
};

struct FrameLedger {
  uint64_t sequence = 0;
  bool title_closed = false;
  bool finalized = false;
  uint32_t guest_read_failures = 0;
  uint32_t sequence_mismatches = 0;
  std::vector<Venue526ATitleContract> title_candidates;
  std::vector<BackendEvent> backend_events;
};

std::mutex g_observer_mutex;
std::deque<FrameLedger> g_frames;
std::shared_ptr<const Venue526AFrameSnapshot> g_published_frame;
Venue526AObserverTelemetry g_telemetry;
uint64_t g_latest_title_sequence = 0;
uint64_t g_latest_backend_sequence = 0;
bool g_logged_first_rejection = false;

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

bool TryReadBeU32(uint8_t *guest_base, uint32_t address, uint32_t &value) {
  if (guest_base == nullptr || address == 0) {
    return false;
  }
  uint8_t *base = guest_base;
  std::array<std::byte, sizeof(uint32_t)> bytes{};
  if (!GuestTryCopy(bytes.data(), REX_RAW_ADDR(address), bytes.size())) {
    return false;
  }
  std::memcpy(&value, bytes.data(), sizeof(value));
  value = std::byteswap(value);
  return true;
}

bool SupportedVertexShader(uint64_t hash) {
  return hash == kVertexShader37F2 || hash == kVertexShader1FCF;
}

uint64_t ExpectedVertexShader(uint32_t stride) {
  if (stride == kStride37F2) {
    return kVertexShader37F2;
  }
  if (stride == kStride1FCF) {
    return kVertexShader1FCF;
  }
  return 0;
}

bool SupportedDepthFormat(nrhi::Format format) {
  return format == nrhi::Format::kD24_UNORM_S8_UINT ||
         format == nrhi::Format::kD32_FLOAT_S8_UINT;
}

bool SupportedBlendControl(uint32_t value) {
  return value == kOpaqueBlendControl || value == kLateBlendControl;
}

bool IsExactBackendDraw(
    const rex::graphics::NativeGuestDrawContext &context) {
  return context.backend == rex::graphics::NativeGuestOutputBackend::kVulkan &&
         context.backend_frame_sequence != 0 && context.render_pass_key_valid &&
         context.render_pass_key == kGameplayRenderPassKey &&
         context.surface_pitch == kGameplaySurfacePitch && context.indexed &&
         context.guest_index_base_valid && context.guest_index_base != 0 &&
         context.draw_state_contract_valid &&
         context.rasterizer_mode_control_valid &&
         context.borrowed_attachment_contract_valid &&
         SupportedVertexShader(context.vertex_shader_hash) &&
         context.pixel_shader_hash == kPixelShader526A &&
         context.primitive_type == kTriangleStripPrimitive &&
         context.guest_vertex_or_index_count != 0 &&
         !context.primitive_restart_enabled &&
         context.normalized_depth_control == kNormalizedDepthControl &&
         context.normalized_color_mask == kNormalizedColorMask &&
         context.color_control == kColorControl &&
         SupportedBlendControl(context.blend_control_0) &&
         context.rasterizer_mode_control == kRasterizerModeControl &&
         context.color_attachment_count == 1 &&
         context.color_attachment_formats[0] ==
             nrhi::Format::kR8G8B8A8_UNORM &&
         SupportedDepthFormat(context.depth_attachment_format) &&
         context.stencil_attachment_format ==
             context.depth_attachment_format &&
         context.sample_count == 4 &&
         context.sample_mask == std::numeric_limits<uint64_t>::max();
}

Venue526ABackendContract CaptureBackendContract(
    const rex::graphics::NativeGuestDrawContext &context) {
  Venue526ABackendContract contract;
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
  contract.valid = IsExactBackendDraw(context);
  return contract;
}

Venue526ADrawIdentity CaptureBackendIdentity(
    const rex::graphics::NativeGuestDrawContext &context) {
  Venue526ADrawIdentity identity;
  identity.primitive_type = context.primitive_type;
  identity.submitted_index_count = context.guest_vertex_or_index_count;
  identity.guest_index_base = context.guest_index_base;
  identity.vertex_shader_hash = context.vertex_shader_hash;
  identity.pixel_shader_hash = context.pixel_shader_hash;
  return identity;
}

uint64_t AuthoritativeTitlePixelHash(
    const SceneCatalogDrawOccurrence &draw) {
  if (draw.bound_shaders.pixel_shader_valid) {
    return draw.bound_shaders.pixel_shader_hash;
  }
  return draw.pass.shader_fingerprints_valid ? draw.pass.pixel_shader_hash : 0;
}

bool IsStructuralTitleCandidate(const SceneCatalogDrawOccurrence &draw) {
  const uint64_t index_bytes =
      static_cast<uint64_t>(draw.submitted_index_count) * sizeof(uint16_t);
  return draw.player == 0 && draw.scope_valid && draw.pass.valid &&
         draw.mesh.valid && draw.state.valid && draw.scope.shader != 0 &&
         draw.scope.model != 0 && draw.pass.pass_descriptor != 0 &&
         draw.pass.program_pair != 0 && draw.pass.vertex_shader != 0 &&
         draw.pass.pixel_shader != 0 &&
         AuthoritativeTitlePixelHash(draw) == kPixelShader526A &&
         draw.primitive_type == kTriangleStripPrimitive &&
         draw.mesh.aggregate_primitive_type == kTriangleStripPrimitive &&
         draw.submitted_index_count != 0 &&
         ExpectedVertexShader(draw.mesh.vertex_stride) != 0 &&
         draw.mesh.vertex_endian == kEndian8In32 &&
         draw.mesh.vertex_buffer_alias != 0 &&
         draw.mesh.vertex_buffer_bytes >= draw.mesh.vertex_stride &&
         draw.mesh.index_buffer_alias != 0 &&
         draw.mesh.index_element_size == sizeof(uint16_t) &&
         !draw.mesh.index_is_32_bit && index_bytes != 0 &&
         index_bytes <= draw.mesh.index_buffer_bytes &&
         PhysicalAddressForVirtualAlias(draw.mesh.index_buffer_alias) != 0 &&
         draw.world_valid && draw.world_view_projection_valid;
}

Venue526ATitleContract CaptureTitleContract(
    uint8_t *guest_base, const SceneCatalogDrawOccurrence &draw,
    uint32_t &guest_read_failures) {
  Venue526ATitleContract title;
  title.sequence = draw.frame_sequence;
  title.ordinal = draw.ordinal;
  title.owner = draw.owner;
  title.material_shader = draw.scope.shader;
  title.model = draw.scope.model;
  title.geometry_index = draw.scope.geometry_index;
  title.lod = draw.scope.lod;
  title.alternate_pass = draw.scope.alternate_pass;
  title.pass_descriptor = draw.pass.pass_descriptor;
  title.program_pair = draw.pass.program_pair;
  title.title_vertex_shader = draw.pass.vertex_shader;
  title.title_pixel_shader = draw.pass.pixel_shader;
  title.title_pixel_shader_hash = AuthoritativeTitlePixelHash(draw);
  title.vertex_aggregate = draw.mesh.vertex_aggregate;
  title.vertex_declaration = draw.mesh.vertex_declaration;
  title.vertex_buffer_alias = draw.mesh.vertex_buffer_alias;
  title.vertex_buffer_bytes = draw.mesh.vertex_buffer_bytes;
  title.vertex_stride = draw.mesh.vertex_stride;
  title.index_buffer_alias = draw.mesh.index_buffer_alias;
  title.index_buffer_bytes = draw.mesh.index_buffer_bytes;
  title.index_element_size = draw.mesh.index_element_size;
  title.texture_fetch_0 = draw.state.texture_fetches[0];
  title.world = draw.world;
  title.world_view_projection = draw.world_view_projection;

  if (!TryReadBeU32(guest_base, title.material_shader,
                    title.material_shader_vtable)) {
    ++guest_read_failures;
    return title;
  }
  title.valid = title.sequence != 0 && title.ordinal != 0 &&
                title.material_shader_vtable == kGrmShaderFxVtable &&
                title.title_pixel_shader_hash == kPixelShader526A &&
                ExpectedVertexShader(title.vertex_stride) != 0;
  return title;
}

FrameLedger *FindFrameLocked(uint64_t sequence) {
  const auto found =
      std::ranges::find_if(g_frames, [sequence](const FrameLedger &frame) {
        return frame.sequence == sequence;
      });
  return found == g_frames.end() ? nullptr : &*found;
}

FrameLedger &FindOrCreateFrameLocked(uint64_t sequence) {
  if (FrameLedger *frame = FindFrameLocked(sequence)) {
    return *frame;
  }
  auto insertion = std::ranges::find_if(
      g_frames, [sequence](const FrameLedger &frame) {
        return frame.sequence > sequence;
      });
  return *g_frames.insert(insertion, FrameLedger{.sequence = sequence});
}

bool SameBackendEvent(const BackendEvent &left, const BackendEvent &right) {
  return left.identity == right.identity && left.contract == right.contract;
}

bool TitleMatchesBackend(const Venue526ATitleContract &title,
                         const BackendEvent &backend) {
  return title.valid && backend.identity.valid() && backend.contract.valid &&
         backend.identity.primitive_type == kTriangleStripPrimitive &&
         backend.identity.submitted_index_count != 0 &&
         backend.identity.guest_index_base ==
             PhysicalAddressForVirtualAlias(title.index_buffer_alias) &&
         backend.identity.submitted_index_count <=
             title.index_buffer_bytes / sizeof(uint16_t) &&
         backend.identity.vertex_shader_hash ==
             ExpectedVertexShader(title.vertex_stride) &&
         backend.identity.pixel_shader_hash == title.title_pixel_shader_hash;
}

void LogRejectedFrame(const FrameLedger &frame, std::string_view reason) {
  if (g_logged_first_rejection) {
    return;
  }
  g_logged_first_rejection = true;
  REXLOG_INFO(
      "Table Tennis 526A observer: rejected frame={} reason={} "
      "title_candidates={} backend_events={} reads={} "
      "observer_only=true guest_suppressed=false",
      frame.sequence, reason, frame.title_candidates.size(),
      frame.backend_events.size(), frame.guest_read_failures);
}

void RejectFrameLocked(FrameLedger &frame, std::string_view reason) {
  frame.finalized = true;
  ++g_telemetry.finalized_frames;
  ++g_telemetry.rejected_frames;
  LogRejectedFrame(frame, reason);
}

void LogPublishedFrame(const Venue526AFrameSnapshot &frame) {
  REXLOG_INFO(
      "Table Tennis 526A observer: published immutable frame={} draws={} "
      "indices={} backend_events={} tiles={} title_candidates={} "
      "observer_only=true guest_suppressed=false",
      frame.sequence, frame.logical_draw_count, frame.logical_index_count,
      frame.backend_event_count, frame.backend_tile_blocks_matched,
      frame.title_candidate_count);
  for (const Venue526ADrawSnapshot &draw : frame.draws) {
    const Venue526ATitleContract &title = draw.title;
    REXLOG_INFO(
        "  526A draw: ordinal={} owner[valid={} kind={} role={} "
        "owner={:08X} renderable={:08X} vtable={:08X}] "
        "material={:08X}/vt={:08X} model={:08X} geometry={} lod={} "
        "pass={:08X} pair={:08X} shaders={:016X}/{:016X} "
        "aggregate={:08X} declaration={:08X} stride={} "
        "vb={:08X}/{} ib={:08X}/{} count={} blend={:08X} "
        "raster={:08X}",
        title.ordinal, title.owner.valid,
        static_cast<uint32_t>(title.owner.kind),
        static_cast<uint32_t>(title.owner.role), title.owner.owner,
        title.owner.renderable, title.owner.renderable_vtable,
        title.material_shader, title.material_shader_vtable, title.model,
        title.geometry_index, title.lod, title.pass_descriptor,
        title.program_pair, draw.backend_identity.vertex_shader_hash,
        draw.backend_identity.pixel_shader_hash, title.vertex_aggregate,
        title.vertex_declaration, title.vertex_stride,
        title.vertex_buffer_alias, title.vertex_buffer_bytes,
        title.index_buffer_alias, title.index_buffer_bytes,
        draw.backend_identity.submitted_index_count,
        draw.backend.blend_control_0, draw.backend.rasterizer_mode_control);
  }
}

void FinalizeFrameLocked(FrameLedger &frame) {
  if (frame.finalized || !frame.title_closed) {
    return;
  }
  ++g_telemetry.backend_frames_analyzed;
  if (frame.guest_read_failures != 0 || frame.title_candidates.empty()) {
    RejectFrameLocked(frame, frame.guest_read_failures != 0
                                 ? "title guest read failure"
                                 : "no title candidates");
    return;
  }
  if (frame.backend_events.empty() ||
      frame.backend_events.size() % 3 != 0) {
    RejectFrameLocked(frame, "backend event count is not three tile blocks");
    return;
  }

  const size_t block_size = frame.backend_events.size() / 3;
  if (block_size == 0 || block_size != frame.title_candidates.size()) {
    RejectFrameLocked(frame, "title/backend logical count mismatch");
    return;
  }
  for (size_t tile = 1; tile < 3; ++tile) {
    for (size_t index = 0; index < block_size; ++index) {
      if (!SameBackendEvent(frame.backend_events[index],
                            frame.backend_events[tile * block_size + index])) {
        RejectFrameLocked(frame, "backend tile block mismatch");
        return;
      }
    }
  }

  auto published = std::make_shared<Venue526AFrameSnapshot>();
  published->sequence = frame.sequence;
  published->backend_frame_sequence = frame.sequence;
  published->title_candidate_count =
      static_cast<uint32_t>(frame.title_candidates.size());
  published->backend_event_count =
      static_cast<uint32_t>(frame.backend_events.size());
  published->backend_tile_blocks_matched = 3;
  published->guest_read_failures = frame.guest_read_failures;
  published->sequence_mismatches = frame.sequence_mismatches;
  published->draws.reserve(block_size);
  for (size_t index = 0; index < block_size; ++index) {
    const Venue526ATitleContract &title = frame.title_candidates[index];
    const BackendEvent &backend = frame.backend_events[index];
    if (!TitleMatchesBackend(title, backend)) {
      RejectFrameLocked(frame, "ordered title/backend identity mismatch");
      return;
    }
    Venue526ADrawSnapshot draw;
    draw.backend_identity = backend.identity;
    draw.backend = backend.contract;
    draw.title = title;
    if (!draw.valid()) {
      RejectFrameLocked(frame, "snapshot invariant");
      return;
    }
    published->logical_index_count +=
        backend.identity.submitted_index_count;
    published->draws.push_back(std::move(draw));
  }
  published->logical_draw_count =
      static_cast<uint32_t>(published->draws.size());
  published->valid = published->logical_draw_count != 0 &&
                     published->logical_draw_count ==
                         published->title_candidate_count;

  frame.finalized = true;
  ++g_telemetry.finalized_frames;
  ++g_telemetry.valid_frames;
  g_telemetry.backend_tile_blocks_matched += 3;
  g_telemetry.latest_published_sequence = frame.sequence;
  g_published_frame = std::move(published);
  LogPublishedFrame(*g_published_frame);
}

void FinalizeOlderFramesLocked(uint64_t newer_backend_sequence) {
  for (FrameLedger &frame : g_frames) {
    if (!frame.finalized && frame.title_closed &&
        frame.sequence < newer_backend_sequence) {
      FinalizeFrameLocked(frame);
    }
  }
}

void TrimFramesLocked() {
  while (g_frames.size() > kMaximumRetainedFrames) {
    if (!g_frames.front().finalized) {
      ++g_telemetry.expired_frames;
    }
    g_frames.pop_front();
  }
  g_telemetry.pending_frames = static_cast<uint32_t>(
      std::ranges::count_if(g_frames, [](const FrameLedger &frame) {
        return !frame.finalized &&
               (!frame.title_candidates.empty() ||
                !frame.backend_events.empty());
      }));
}

} // namespace

bool Venue526AObserverEnabled() {
  // This family isn't consumed by the four-family private transaction yet.
  // Keep it explicitly armed so the normal transaction path doesn't pay for
  // an unrelated full title/backend capture.
  return REXCVAR_GET(tabletennis_native_venue_526a_observer);
}

void ObserveVenue526ATitleDraw(
    uint8_t *guest_base, const SceneCatalogDrawOccurrence &draw) {
  if (!Venue526AObserverEnabled()) {
    return;
  }

  std::lock_guard lock(g_observer_mutex);
  ++g_telemetry.title_draws_observed;
  const uint64_t pixel_hash = AuthoritativeTitlePixelHash(draw);
  if (pixel_hash != kPixelShader526A) {
    return;
  }
  ++g_telemetry.title_pixel_hash_matches;
  if (!IsStructuralTitleCandidate(draw)) {
    return;
  }
  ++g_telemetry.title_structural_matches;

  FrameLedger &frame = FindOrCreateFrameLocked(draw.frame_sequence);
  if (frame.finalized ||
      frame.title_candidates.size() == kMaximumCandidatesPerFrame) {
    ++frame.sequence_mismatches;
    return;
  }
  uint32_t guest_read_failures = 0;
  Venue526ATitleContract title =
      CaptureTitleContract(guest_base, draw, guest_read_failures);
  frame.guest_read_failures += guest_read_failures;
  g_telemetry.title_guest_read_failures += guest_read_failures;
  if (!title.valid) {
    return;
  }
  frame.title_candidates.push_back(std::move(title));
  ++g_telemetry.title_candidates;
  g_latest_title_sequence =
      std::max(g_latest_title_sequence, draw.frame_sequence);
  TrimFramesLocked();
}

void ObserveVenue526ABackendDraw(
    const rex::graphics::NativeGuestDrawContext &context) {
  if (!Venue526AObserverEnabled()) {
    return;
  }

  std::lock_guard lock(g_observer_mutex);
  ++g_telemetry.backend_draws_observed;
  if (context.pixel_shader_hash != kPixelShader526A) {
    return;
  }
  ++g_telemetry.backend_hash_matches;
  if (!IsExactBackendDraw(context)) {
    return;
  }
  ++g_telemetry.backend_contract_matches;

  const uint64_t sequence = context.backend_frame_sequence;
  if (sequence > g_latest_backend_sequence) {
    FinalizeOlderFramesLocked(sequence);
    g_latest_backend_sequence = sequence;
  }
  FrameLedger &frame = FindOrCreateFrameLocked(sequence);
  if (frame.finalized ||
      frame.backend_events.size() == kMaximumBackendEventsPerFrame) {
    ++frame.sequence_mismatches;
    return;
  }
  BackendEvent event;
  event.identity = CaptureBackendIdentity(context);
  event.contract = CaptureBackendContract(context);
  frame.backend_events.push_back(std::move(event));
  ++g_telemetry.backend_events;
  TrimFramesLocked();
}

void Venue526AObserverFrameEnd() {
  if (!Venue526AObserverEnabled()) {
    return;
  }

  std::lock_guard lock(g_observer_mutex);
  if (g_latest_title_sequence != 0) {
    if (FrameLedger *frame = FindFrameLocked(g_latest_title_sequence)) {
      frame->title_closed = true;
    }
  }
  if (g_latest_backend_sequence != 0) {
    FinalizeOlderFramesLocked(g_latest_backend_sequence);
  }
  TrimFramesLocked();
}

std::shared_ptr<const Venue526AFrameSnapshot>
LatestVenue526AFrameSnapshot() {
  std::lock_guard lock(g_observer_mutex);
  return g_published_frame;
}

Venue526AObserverTelemetry LatestVenue526AObserverTelemetry() {
  std::lock_guard lock(g_observer_mutex);
  return g_telemetry;
}

} // namespace tabletennis::native
