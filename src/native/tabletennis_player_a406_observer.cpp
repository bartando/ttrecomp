#include "native/tabletennis_player_a406_observer.h"

#include "generated/default/tabletennis_init.h"
#include "native/tabletennis_main_coverage_ledger.h"
#include "native/tabletennis_scene_draw_catalog.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
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
    tabletennis_native_player_a406_observer, false, "Table Tennis",
    "Capture immutable A406/0F9 rigid-player draws and publish only after an "
    "exact same-frame three-tile backend proof. Observer-only; never "
    "suppresses or replaces a draw.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace tabletennis::native {
namespace {

namespace nrhi = rex::graphics::nrhi;

constexpr uint64_t kVertexShaderHash = 0xA406367569E5F6F8ull;
constexpr uint64_t kPixelShaderHash = 0x0F9CCE179F32DA36ull;
constexpr uint32_t kMainRenderPassKey = 0x0000000E;
constexpr uint32_t kMainColorEdramBase = 0x00000400;
constexpr uint32_t kMainDepthEdramBase = 0;
constexpr uint32_t kMainSurfacePitch = 1280;
constexpr uint32_t kColorDepthEdramMode = 4;
constexpr uint32_t kTriangleListPrimitive = 4;
constexpr uint32_t kNormalizedDepthControl = 0x00700736;
constexpr uint32_t kNormalizedColorMask = 0x0000000F;
constexpr uint32_t kColorControl = 0x87000005;
constexpr uint32_t kBlendControl0 = 0x00010001;
constexpr uint32_t kRasterizerModeControl = 0x00018000;
constexpr size_t kMaximumCandidatesPerFrame = 128;
constexpr size_t kMaximumBackendEventsPerFrame =
    kMaximumCandidatesPerFrame *
    PlayerA406FrameSnapshot::kRequiredTileBlockCount;
constexpr size_t kMaximumRetainedFrames = 12;

struct TitleToken {
  PlayerA406TitleCandidate candidate{};
  std::shared_ptr<const PlayerA406TitleDrawSnapshot> snapshot;
  uint32_t guest_read_failures = 0;
  uint32_t payload_copy_failures = 0;
  uint32_t texture_capture_failures = 0;
  uint32_t material_validation_failures = 0;
};

struct BackendEvent {
  uint64_t backend_frame_sequence = 0;
  PlayerA406DrawIdentity identity{};
  PlayerA406BackendContract contract{};

  bool operator==(const BackendEvent &) const = default;
};

struct FrameLedger {
  uint64_t sequence = 0;
  bool title_closed = false;
  bool finalized = false;
  uint32_t dropped_candidate_count = 0;
  uint32_t sequence_mismatches = 0;
  std::vector<TitleToken> title_candidates;
  std::vector<BackendEvent> backend_events;
};

std::mutex g_observer_mutex;
std::deque<FrameLedger> g_frames;
std::shared_ptr<const PlayerA406FrameSnapshot> g_published_frame;
PlayerA406ObserverTelemetry g_telemetry;
uint64_t g_latest_title_sequence = 0;
uint64_t g_latest_backend_sequence = 0;
bool g_was_enabled = false;
bool g_logged_first_rejection = false;
bool g_logged_first_publication = false;
bool g_logged_first_title_gate_rejection = false;
bool g_logged_first_backend_gate_rejection = false;

uint64_t AuthoritativeVertexHash(const SceneCatalogDrawOccurrence &draw) {
  if (draw.bound_shaders.vertex_shader_valid) {
    return draw.bound_shaders.vertex_shader_hash;
  }
  return draw.pass.shader_fingerprints_valid ? draw.pass.vertex_shader_hash : 0;
}

uint64_t AuthoritativePixelHash(const SceneCatalogDrawOccurrence &draw) {
  if (draw.bound_shaders.pixel_shader_valid) {
    return draw.bound_shaders.pixel_shader_hash;
  }
  return draw.pass.shader_fingerprints_valid ? draw.pass.pixel_shader_hash : 0;
}

bool RawTargetStateMatchesDecoded(
    const rex::graphics::NativeGuestDrawContext::RenderTargetState &state) {
  constexpr uint32_t kEdramBaseMask = (1u << 12) - 1;
  constexpr uint32_t kSurfacePitchMask = (1u << 14) - 1;
  constexpr uint32_t kEdramModeMask = (1u << 3) - 1;
  return state.valid &&
         (state.rb_color_info_0 & kEdramBaseMask) == state.color_edram_base &&
         (state.rb_depth_info & kEdramBaseMask) == state.depth_edram_base &&
         (state.rb_surface_info & kSurfacePitchMask) == state.surface_pitch &&
         (state.rb_modecontrol & kEdramModeMask) == state.edram_mode;
}

bool IsExactMainTarget(const rex::graphics::NativeGuestDrawContext &context) {
  const auto &target = context.render_target_state;
  return RawTargetStateMatchesDecoded(target) &&
         target.color_edram_base == kMainColorEdramBase &&
         target.depth_edram_base == kMainDepthEdramBase &&
         target.surface_pitch == kMainSurfacePitch &&
         target.edram_mode == kColorDepthEdramMode &&
         context.surface_pitch == target.surface_pitch;
}

bool SupportedDepthFormat(nrhi::Format format) {
  return format == nrhi::Format::kD24_UNORM_S8_UINT ||
         format == nrhi::Format::kD32_FLOAT_S8_UINT;
}

bool ExactBackendVertexFetch(
    const rex::graphics::NativeGuestDrawContext &context) {
  return context.primary_vertex_fetch.valid &&
         context.primary_vertex_fetch.physical_address != 0 &&
         context.primary_vertex_fetch.byte_count >= kPlayerA406VertexStride &&
         context.primary_vertex_fetch.byte_count % kPlayerA406VertexStride ==
             0 &&
         context.primary_vertex_fetch.endian == kPlayerA406VertexEndian;
}

bool IsExactBackendDraw(const rex::graphics::NativeGuestDrawContext &context) {
  return context.backend == rex::graphics::NativeGuestOutputBackend::kVulkan &&
         context.backend_frame_sequence != 0 && context.render_pass_key_valid &&
         context.render_pass_key == kMainRenderPassKey &&
         IsExactMainTarget(context) && context.indexed &&
         context.guest_index_base_valid && context.guest_index_base != 0 &&
         ExactBackendVertexFetch(context) &&
         context.draw_state_contract_valid &&
         context.rasterizer_mode_control_valid &&
         context.borrowed_attachment_contract_valid &&
         context.vertex_shader_hash == kVertexShaderHash &&
         context.pixel_shader_hash == kPixelShaderHash &&
         context.primitive_type == kTriangleListPrimitive &&
         context.guest_vertex_or_index_count != 0 &&
         !context.primitive_restart_enabled &&
         context.normalized_depth_control == kNormalizedDepthControl &&
         context.normalized_color_mask == kNormalizedColorMask &&
         context.color_control == kColorControl &&
         context.blend_control_0 == kBlendControl0 &&
         context.rasterizer_mode_control == kRasterizerModeControl &&
         context.color_attachment_count == 1 &&
         context.color_attachment_formats[0] == nrhi::Format::kR8G8B8A8_UNORM &&
         SupportedDepthFormat(context.depth_attachment_format) &&
         context.stencil_attachment_format == context.depth_attachment_format &&
         context.sample_count == 4 &&
         context.sample_mask == std::numeric_limits<uint64_t>::max();
}

PlayerA406BackendContract
CaptureBackendContract(const rex::graphics::NativeGuestDrawContext &context) {
  PlayerA406BackendContract contract;
  contract.vertex_shader_hash = context.vertex_shader_hash;
  contract.pixel_shader_hash = context.pixel_shader_hash;
  contract.render_pass_key = context.render_pass_key;
  contract.rb_color_info_0 = context.render_target_state.rb_color_info_0;
  contract.rb_depth_info = context.render_target_state.rb_depth_info;
  contract.rb_surface_info = context.render_target_state.rb_surface_info;
  contract.rb_modecontrol = context.render_target_state.rb_modecontrol;
  contract.color_edram_base = context.render_target_state.color_edram_base;
  contract.depth_edram_base = context.render_target_state.depth_edram_base;
  contract.surface_pitch = context.render_target_state.surface_pitch;
  contract.edram_mode = context.render_target_state.edram_mode;
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
  contract.render_target_state_valid =
      RawTargetStateMatchesDecoded(context.render_target_state);
  contract.rasterizer_mode_control_valid =
      context.rasterizer_mode_control_valid;
  contract.valid = IsExactBackendDraw(context);
  return contract;
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
  const auto insertion =
      std::ranges::find_if(g_frames, [sequence](const FrameLedger &frame) {
        return frame.sequence > sequence;
      });
  return *g_frames.insert(insertion, FrameLedger{.sequence = sequence});
}

bool TitleMatchesBackend(const TitleToken &title, const BackendEvent &backend) {
  return title.candidate.eligible && title.snapshot != nullptr &&
         title.snapshot->valid && backend.contract.valid &&
         backend.identity == title.candidate.identity &&
         backend.contract.pixel_shader_hash ==
             title.candidate.program.pixel_shader_hash;
}

void UpdatePendingFramesLocked() {
  g_telemetry.pending_frames = static_cast<uint32_t>(
      std::ranges::count_if(g_frames, [](const FrameLedger &frame) {
        return !frame.finalized && (!frame.title_candidates.empty() ||
                                    !frame.backend_events.empty());
      }));
}

void RejectFrameLocked(FrameLedger &frame, std::string_view reason) {
  frame.finalized = true;
  ++g_telemetry.finalized_frames;
  ++g_telemetry.rejected_frames;
  if (!g_logged_first_rejection) {
    g_logged_first_rejection = true;
    REXLOG_INFO(
        "Table Tennis A406 observer: rejected frame={} reason={} "
        "title_candidates={} backend_events={} dropped={} mismatches={} "
        "observer_only=true guest_suppressed=false",
        frame.sequence, reason, frame.title_candidates.size(),
        frame.backend_events.size(), frame.dropped_candidate_count,
        frame.sequence_mismatches);
  }
}

void LogPublishedFrame(const PlayerA406FrameSnapshot &frame) {
  REXLOG_INFO(
      "Table Tennis A406 observer: published immutable frame={} draws={} "
      "indices={} title_candidates={} unmatched_title={} backend_events={} "
      "tiles={} owned_textures=0,1,2,6 "
      "constants=vc0-3,vc12-15,vc19,vc29-36,vc46-47,"
      "pc19,pc21-27,pc46-73,pc254-255 "
      "observer_only=true guest_suppressed=false",
      frame.sequence, frame.matched_draw_count, frame.matched_index_count,
      frame.title_candidate_count, frame.unmatched_title_candidate_count,
      frame.backend_event_count, frame.backend_tile_blocks_matched);
  for (const PlayerA406DrawSnapshot &draw : frame.draws) {
    const PlayerA406TitleDrawSnapshot &title = *draw.title;
    const TextureSnapshot &texture = *title.material.owned_textures[0];
    REXLOG_INFO(
        "  A406 draw: ordinal={} player={:08X} "
        "owner[kind={} role={} owner={:08X} "
        "renderable={:08X}] material={:08X}/vt={:08X} model={:08X} "
        "geometry={} lod={} pass={:08X} pair={:08X} "
        "bound_hashes={}/{} vb={:08X}/{} vertices={} ib={:08X}/{} "
        "indices={} range={}-{} texture={}x{} format={} mips={} "
        "blend={:08X} raster={:08X}",
        title.ordinal, title.player, static_cast<uint32_t>(title.owner.kind),
        static_cast<uint32_t>(title.owner.role), title.owner.owner,
        title.owner.renderable, title.material_shader,
        title.material_shader_vtable, title.model, title.geometry_index,
        title.lod, title.program.pass_descriptor, title.program.program_pair,
        title.program.vertex_hash_from_bound_shader,
        title.program.pixel_hash_from_bound_shader,
        title.vertices->physical_address, title.vertices->byte_count,
        title.vertices->vertex_count, title.indices->physical_address,
        title.indices->raw_bytes.size(), title.indices->submitted_index_count,
        title.indices->minimum_index, title.indices->maximum_index,
        texture.width, texture.height, texture.format, texture.mips.size(),
        draw.backend.blend_control_0, draw.backend.rasterizer_mode_control);
  }
}

void FinalizeFrameLocked(FrameLedger &frame) {
  if (frame.finalized || !frame.title_closed) {
    return;
  }
  ++g_telemetry.backend_frames_analyzed;
  if (frame.dropped_candidate_count != 0 || frame.title_candidates.empty()) {
    RejectFrameLocked(frame, frame.dropped_candidate_count != 0
                                 ? "title candidate overflow"
                                 : "no title candidates");
    return;
  }
  if (frame.backend_events.empty() ||
      frame.backend_events.size() %
              PlayerA406FrameSnapshot::kRequiredTileBlockCount !=
          0) {
    RejectFrameLocked(frame, "backend event count is not three tile blocks");
    return;
  }
  const size_t block_size = frame.backend_events.size() /
                            PlayerA406FrameSnapshot::kRequiredTileBlockCount;
  if (block_size == 0 || block_size > frame.title_candidates.size()) {
    RejectFrameLocked(frame, "backend logical count exceeds title candidates");
    return;
  }
  for (size_t tile = 1; tile < PlayerA406FrameSnapshot::kRequiredTileBlockCount;
       ++tile) {
    for (size_t index = 0; index < block_size; ++index) {
      if (!(frame.backend_events[index] ==
            frame.backend_events[tile * block_size + index])) {
        RejectFrameLocked(frame, "backend tile block mismatch");
        return;
      }
    }
  }

  std::vector<size_t> selected;
  selected.reserve(block_size);
  size_t search_begin = 0;
  for (size_t index = 0; index < block_size; ++index) {
    const BackendEvent &backend = frame.backend_events[index];
    const auto found = std::ranges::find_if(
        frame.title_candidates.begin() +
            static_cast<std::ptrdiff_t>(search_begin),
        frame.title_candidates.end(), [&](const TitleToken &title) {
          return TitleMatchesBackend(title, backend);
        });
    if (found == frame.title_candidates.end()) {
      RejectFrameLocked(frame, "ordered title/backend identity mismatch");
      return;
    }
    const size_t selected_index =
        static_cast<size_t>(found - frame.title_candidates.begin());
    selected.push_back(selected_index);
    search_begin = selected_index + 1;
  }
  std::vector<size_t> reverse_selected(block_size);
  size_t reverse_cursor = frame.title_candidates.size();
  for (size_t index = block_size; index-- > 0;) {
    const BackendEvent &backend = frame.backend_events[index];
    bool matched = false;
    while (reverse_cursor != 0) {
      --reverse_cursor;
      if (TitleMatchesBackend(frame.title_candidates[reverse_cursor],
                              backend)) {
        reverse_selected[index] = reverse_cursor;
        matched = true;
        break;
      }
    }
    if (!matched) {
      RejectFrameLocked(frame, "reverse title/backend identity mismatch");
      return;
    }
  }
  if (selected != reverse_selected) {
    RejectFrameLocked(frame, "ambiguous title/backend identity join");
    return;
  }

  auto published = std::make_shared<PlayerA406FrameSnapshot>();
  published->sequence = frame.sequence;
  published->backend_frame_sequence = frame.sequence;
  published->title_candidate_count =
      static_cast<uint32_t>(frame.title_candidates.size());
  published->matched_draw_count = static_cast<uint32_t>(selected.size());
  published->unmatched_title_candidate_count =
      published->title_candidate_count - published->matched_draw_count;
  published->dropped_candidate_count = frame.dropped_candidate_count;
  published->backend_event_count =
      static_cast<uint32_t>(frame.backend_events.size());
  published->backend_draws_per_tile = published->matched_draw_count;
  published->backend_tile_blocks_matched =
      PlayerA406FrameSnapshot::kRequiredTileBlockCount;
  published->sequence_mismatches = frame.sequence_mismatches;
  published->draws.reserve(selected.size());
  for (size_t index = 0; index < selected.size(); ++index) {
    const TitleToken &title = frame.title_candidates[selected[index]];
    const BackendEvent &backend = frame.backend_events[index];
    published->guest_read_failures += title.guest_read_failures;
    published->payload_copy_failures += title.payload_copy_failures;
    published->texture_capture_failures += title.texture_capture_failures;
    published->material_validation_failures +=
        title.material_validation_failures;
    published->matched_index_count += backend.identity.submitted_index_count;
    published->draws.push_back({
        .title = title.snapshot,
        .backend_identity = backend.identity,
        .backend = backend.contract,
    });
  }
  published->backend_indices_per_tile = published->matched_index_count;
  frame.finalized = true;
  ++g_telemetry.finalized_frames;
  if (!published->valid()) {
    ++g_telemetry.rejected_frames;
    if (!g_logged_first_rejection) {
      g_logged_first_rejection = true;
      REXLOG_INFO(
          "Table Tennis A406 observer: rejected frame={} reason=snapshot "
          "invariant draws={} indices={} reads={} payloads={} textures={} "
          "materials={} observer_only=true guest_suppressed=false",
          published->sequence, published->matched_draw_count,
          published->matched_index_count, published->guest_read_failures,
          published->payload_copy_failures, published->texture_capture_failures,
          published->material_validation_failures);
    }
    return;
  }
  ++g_telemetry.valid_frames;
  g_telemetry.backend_tile_blocks_matched +=
      PlayerA406FrameSnapshot::kRequiredTileBlockCount;
  g_telemetry.latest_published_sequence = published->sequence;
  g_published_frame = std::move(published);
  ObserveMainCoverageFamilyFrame(g_published_frame);
  if (!g_logged_first_publication) {
    g_logged_first_publication = true;
    LogPublishedFrame(*g_published_frame);
  }
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
  UpdatePendingFramesLocked();
}

void ResetLocked() {
  g_frames.clear();
  g_published_frame.reset();
  g_telemetry = {};
  g_latest_title_sequence = 0;
  g_latest_backend_sequence = 0;
  g_logged_first_rejection = false;
  g_logged_first_publication = false;
  g_logged_first_title_gate_rejection = false;
  g_logged_first_backend_gate_rejection = false;
}

} // namespace

bool PlayerA406FrameSnapshot::valid() const {
  if (sequence == 0 || backend_frame_sequence != sequence ||
      matched_draw_count == 0 || matched_draw_count != backend_draws_per_tile ||
      matched_index_count == 0 ||
      matched_index_count != backend_indices_per_tile ||
      draws.size() != matched_draw_count ||
      title_candidate_count !=
          matched_draw_count + unmatched_title_candidate_count ||
      backend_event_count !=
          backend_draws_per_tile * backend_tile_blocks_matched ||
      dropped_candidate_count != 0 || guest_read_failures != 0 ||
      payload_copy_failures != 0 || texture_capture_failures != 0 ||
      material_validation_failures != 0 ||
      backend_tile_blocks_matched != kRequiredTileBlockCount ||
      sequence_mismatches != 0) {
    return false;
  }
  return std::ranges::all_of(
      draws, [](const PlayerA406DrawSnapshot &draw) { return draw.valid(); });
}

bool PlayerA406ObserverEnabled() {
  return REXCVAR_GET(tabletennis_native_player_a406_observer);
}

void ObservePlayerA406TitleDraw(uint8_t *guest_base,
                                const SceneCatalogDrawOccurrence &draw) {
  if (!PlayerA406ObserverEnabled()) {
    return;
  }
  const uint64_t pixel_hash = AuthoritativePixelHash(draw);
  const uint64_t vertex_hash = AuthoritativeVertexHash(draw);
  const bool shader_pair_match =
      pixel_hash == kPixelShaderHash && vertex_hash != 0;
  const PlayerA406TitleCandidate candidate =
      ClassifyPlayerA406TitleCandidate(guest_base, draw);
  {
    std::lock_guard lock(g_observer_mutex);
    ++g_telemetry.title_draws_observed;
    g_telemetry.title_pixel_hash_matches += pixel_hash == kPixelShaderHash;
    g_telemetry.title_shader_pair_matches += shader_pair_match;
    g_telemetry.title_structural_matches += candidate.identity.valid();
    g_telemetry.title_declaration_matches +=
        candidate.vertex_declaration.valid();
  }
  if (!candidate.eligible) {
    if (shader_pair_match) {
      std::lock_guard lock(g_observer_mutex);
      if (!g_logged_first_title_gate_rejection) {
        g_logged_first_title_gate_rejection = true;
        REXLOG_INFO(
            "Table Tennis A406 observer: title gate rejected exact shader pair "
            "frame={} ordinal={} player={:08X} scope_valid={} pass_valid={} "
            "mesh_valid={} state_valid={} shader={:08X} model={:08X} "
            "shader_vtable={:08X} aggregate={:08X} "
            "primitive={}/{} indices={} vb={:08X}/{} stride={} endian={} "
            "ib={:08X}/{} element_size={} index32={} world={}/{} "
            "identity_valid={} program_valid={} declaration_valid={} "
            "declaration[masks={:016X}/{:016X}] "
            "observer_only=true guest_suppressed=false",
            draw.frame_sequence, draw.ordinal, draw.player, draw.scope_valid,
            draw.pass.valid, draw.mesh.valid, draw.state.valid,
            draw.scope.shader, draw.scope.model,
            candidate.material_shader_vtable, draw.mesh.vertex_aggregate,
            draw.primitive_type, draw.mesh.aggregate_primitive_type,
            draw.submitted_index_count, draw.mesh.vertex_buffer_alias,
            draw.mesh.vertex_buffer_bytes, draw.mesh.vertex_stride,
            draw.mesh.vertex_endian, draw.mesh.index_buffer_alias,
            draw.mesh.index_buffer_bytes, draw.mesh.index_element_size,
            draw.mesh.index_is_32_bit, draw.world_valid,
            draw.world_view_projection_valid, candidate.identity.valid(),
            candidate.program.valid(), candidate.vertex_declaration.valid(),
            candidate.vertex_declaration.stream_mask_lo,
            candidate.vertex_declaration.stream_mask_hi);
        const uint32_t declaration =
            draw.state.vertex_declaration != 0
                ? draw.state.vertex_declaration
                : draw.mesh.vertex_declaration;
        const VertexDeclarationProbe declaration_probe =
            ProbeVertexDeclaration(guest_base, declaration);
        REXLOG_INFO(
            "  A406 declaration probe: address={:08X} valid={} failures={} "
            "count={} max_stream={} masks={:016X}/{:016X} cache={:08X}",
            declaration, declaration_probe.valid,
            declaration_probe.copy_failures,
            declaration_probe.element_count, declaration_probe.max_stream,
            declaration_probe.stream_mask_lo,
            declaration_probe.stream_mask_hi, declaration_probe.cache_id);
        for (uint32_t index = 0;
             index < declaration_probe.element_count &&
             index < declaration_probe.elements.size();
             ++index) {
          const VertexDeclarationElement &element =
              declaration_probe.elements[index];
          REXLOG_INFO(
              "    A406 declaration element[{}]: stream={} offset={} "
              "type={:08X} format={} signed={} normalized={} method={} "
              "usage={} usage_index={} padding={}",
              index, element.stream, element.byte_offset,
              element.packed_type, element.format(), element.is_signed(),
              element.normalized(), element.method, element.usage,
              element.usage_index, element.unused_padding);
        }
      }
    }
    return;
  }

  PlayerA406TitleCapture capture =
      CapturePlayerA406TitleDraw(guest_base, draw, candidate);
  TitleToken token = {
      .candidate = candidate,
      .snapshot = std::move(capture.snapshot),
      .guest_read_failures = capture.guest_read_failures,
      .payload_copy_failures = capture.payload_copy_failures,
      .texture_capture_failures = capture.texture_capture_failures,
      .material_validation_failures = capture.material_validation_failures,
  };

  std::lock_guard lock(g_observer_mutex);
  g_was_enabled = true;
  FrameLedger &frame = FindOrCreateFrameLocked(draw.frame_sequence);
  if (frame.finalized ||
      frame.title_candidates.size() == kMaximumCandidatesPerFrame) {
    ++frame.dropped_candidate_count;
    ++frame.sequence_mismatches;
    TrimFramesLocked();
    return;
  }
  frame.title_candidates.push_back(std::move(token));
  ++g_telemetry.title_candidates;
  g_telemetry.title_valid_snapshots +=
      frame.title_candidates.back().snapshot != nullptr;
  g_telemetry.title_guest_read_failures += capture.guest_read_failures;
  g_telemetry.title_payload_copy_failures += capture.payload_copy_failures;
  g_telemetry.title_texture_capture_failures +=
      capture.texture_capture_failures;
  g_telemetry.title_material_validation_failures +=
      capture.material_validation_failures;
  g_latest_title_sequence =
      std::max(g_latest_title_sequence, draw.frame_sequence);
  TrimFramesLocked();
}

void ObservePlayerA406BackendDraw(
    const rex::graphics::NativeGuestDrawContext &context) {
  if (!PlayerA406ObserverEnabled()) {
    return;
  }
  const bool pixel_match = context.pixel_shader_hash == kPixelShaderHash;
  const bool pair_match =
      pixel_match && context.vertex_shader_hash == kVertexShaderHash;
  const bool target_match = pair_match && IsExactMainTarget(context);
  const bool exact = IsExactBackendDraw(context);

  std::lock_guard lock(g_observer_mutex);
  g_was_enabled = true;
  ++g_telemetry.backend_draws_observed;
  g_telemetry.backend_pixel_hash_matches += pixel_match;
  g_telemetry.backend_shader_pair_matches += pair_match;
  g_telemetry.backend_target_matches += target_match;
  g_telemetry.backend_contract_matches += exact;
  g_telemetry.latest_backend_sequence = std::max(
      g_telemetry.latest_backend_sequence, context.backend_frame_sequence);
  if (!exact) {
    if (pair_match && !g_logged_first_backend_gate_rejection) {
      g_logged_first_backend_gate_rejection = true;
      REXLOG_INFO(
          "Table Tennis A406 observer: backend gate rejected exact shader pair "
          "frame={} backend={} pass={:08X}/{} target={} indexed={} "
          "ib={:08X}/{} vb={:08X}/{} endian={}/{} draw_state={} raster={} "
          "attachments={} primitive={} count={} restart={} "
          "observer_only=true guest_suppressed=false",
          context.backend_frame_sequence,
          static_cast<uint32_t>(context.backend), context.render_pass_key,
          context.render_pass_key_valid, target_match, context.indexed,
          context.guest_index_base, context.guest_index_base_valid,
          context.primary_vertex_fetch.physical_address,
          context.primary_vertex_fetch.byte_count,
          context.primary_vertex_fetch.endian,
          context.primary_vertex_fetch.valid, context.draw_state_contract_valid,
          context.rasterizer_mode_control_valid,
          context.borrowed_attachment_contract_valid, context.primitive_type,
          context.guest_vertex_or_index_count,
          context.primitive_restart_enabled);
    }
    return;
  }
  g_telemetry.latest_rasterizer_mode_control = context.rasterizer_mode_control;
  const uint64_t sequence = context.backend_frame_sequence;
  if (sequence > g_latest_backend_sequence) {
    FinalizeOlderFramesLocked(sequence);
    g_latest_backend_sequence = sequence;
  }
  FrameLedger &frame = FindOrCreateFrameLocked(sequence);
  if (frame.finalized ||
      frame.backend_events.size() == kMaximumBackendEventsPerFrame) {
    ++frame.sequence_mismatches;
    TrimFramesLocked();
    return;
  }
  frame.backend_events.push_back({
      .backend_frame_sequence = sequence,
      .identity =
          {
              .primitive_type = context.primitive_type,
              .submitted_index_count = context.guest_vertex_or_index_count,
              .guest_index_base = context.guest_index_base,
              .guest_vertex_base =
                  context.primary_vertex_fetch.physical_address,
              .guest_vertex_bytes = context.primary_vertex_fetch.byte_count,
              .guest_vertex_endian = context.primary_vertex_fetch.endian,
          },
      .contract = CaptureBackendContract(context),
  });
  ++g_telemetry.backend_events;
  TrimFramesLocked();
}

void PlayerA406ObserverFrameEnd() {
  const bool enabled = PlayerA406ObserverEnabled();
  std::lock_guard lock(g_observer_mutex);
  if (!enabled) {
    if (g_was_enabled) {
      ResetLocked();
    }
    g_was_enabled = false;
    return;
  }
  g_was_enabled = true;
  ++g_telemetry.title_frames;
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

std::shared_ptr<const PlayerA406FrameSnapshot> LatestPlayerA406FrameSnapshot() {
  std::lock_guard lock(g_observer_mutex);
  return g_published_frame;
}

PlayerA406ObserverTelemetry LatestPlayerA406ObserverTelemetry() {
  std::lock_guard lock(g_observer_mutex);
  return g_telemetry;
}

} // namespace tabletennis::native
