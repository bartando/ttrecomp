#include "native/tabletennis_hud_swf_backend_observer.h"

#include "native/tabletennis_hud_swf_capture.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include <rex/cvar.h>
#include <rex/graphics/native_guest_renderer.h>
#include <rex/logging.h>

REXCVAR_DEFINE_UINT32(
    tabletennis_native_hud_swf_backend_log_interval, 120, "Table Tennis",
    "Analyzed gameplay HUD/SWF backend frames between observer reports.")
    .range(1, 100000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace tabletennis::native {
namespace {

constexpr uint64_t kHudVertexShaderHash = 0xF0B85512865B6E5Eull;
constexpr uint64_t kHudPixelShaderHash = 0x391847433E1601A9ull;
constexpr size_t kMaximumRetainedFrames = 8;
constexpr size_t kMaximumQueuedEligibilityEvents = 1024;
constexpr size_t kMaximumQueuedLateEvents = 1024;
constexpr size_t kMaximumEligibilityEventsPerFrame = 256;
constexpr size_t kMaximumLateEventsPerFrame = 256;

// Verified directly from the big-endian dwords at 0x825D4C64, indexed by the
// batch_kind passed to sub_82152A78.
constexpr std::array<uint32_t, 7> kBatchKindPrimitiveTypes = {
    1, // point list
    2, // line list
    3, // line strip
    4, // triangle list
    6, // triangle strip
    5, // triangle fan
    13 // quad list
};

struct LateBackendEvent {
  HudSwfBackendDrawContract contract{};
};

struct EligibilityEvent {
  HudSwfBackendEligibilityContract contract{};
};

struct FrameLedger {
  uint64_t sequence = 0;
  std::shared_ptr<const HudSwfFrameSnapshot> title;
  std::vector<EligibilityEvent> eligibility_events;
  std::vector<LateBackendEvent> late_events;
  uint32_t dropped_eligibility_event_count = 0;
  uint32_t dropped_backend_event_count = 0;
  bool finalized = false;
};

std::mutex g_observer_mutex;
std::deque<FrameLedger> g_frames;
std::deque<EligibilityEvent> g_eligibility_events;
std::deque<LateBackendEvent> g_late_events;
std::shared_ptr<const HudSwfBackendFrameSnapshot> g_published_frame;
HudSwfBackendObserverTelemetry g_telemetry;
uint64_t g_latest_title_sequence = 0;
uint64_t g_latest_backend_sequence = 0;
bool g_announced_hash_callback = false;
bool g_announced_complete = false;
bool g_announced_rejection = false;

bool HashesMatch(const rex::graphics::NativeGuestDrawContext &context) {
  return context.vertex_shader_hash == kHudVertexShaderHash &&
         context.pixel_shader_hash == kHudPixelShaderHash;
}

bool HashesMatch(
    const rex::graphics::NativeGuestDrawEligibilityContext &context) {
  return context.vertex_shader_hash == kHudVertexShaderHash &&
         context.pixel_shader_hash == kHudPixelShaderHash;
}

uint32_t PrimitiveTypeForBatchKind(uint32_t batch_kind) {
  return batch_kind < kBatchKindPrimitiveTypes.size()
             ? kBatchKindPrimitiveTypes[batch_kind]
             : 0;
}

bool RawTargetStateMatchesDecoded(
    const rex::graphics::NativeGuestDrawContext::RenderTargetState &state) {
  constexpr uint32_t kEdramBaseMask = (1u << 12) - 1;
  constexpr uint32_t kSurfacePitchMask = (1u << 14) - 1;
  constexpr uint32_t kEdramModeMask = (1u << 3) - 1;
  return state.valid &&
         (state.rb_color_info_0 & kEdramBaseMask) ==
             state.color_edram_base &&
         (state.rb_depth_info & kEdramBaseMask) ==
             state.depth_edram_base &&
         (state.rb_surface_info & kSurfacePitchMask) ==
             state.surface_pitch &&
         (state.rb_modecontrol & kEdramModeMask) == state.edram_mode;
}

bool ContextContractComplete(
    const rex::graphics::NativeGuestDrawContext &context) {
  return context.backend == rex::graphics::NativeGuestOutputBackend::kVulkan &&
         context.backend_frame_sequence != 0 && HashesMatch(context) &&
         context.render_pass_key_valid && context.primitive_type != 0 &&
         context.guest_vertex_or_index_count != 0 &&
         context.vertex_or_index_count != 0 &&
         context.draw_state_contract_valid &&
         context.rasterizer_mode_control_valid &&
         context.borrowed_attachment_contract_valid &&
         context.color_attachment_count != 0 &&
         context.color_attachment_count <=
             rex::graphics::NativeGuestDrawContext::kMaxColorAttachments &&
         context.sample_count != 0 &&
         RawTargetStateMatchesDecoded(context.render_target_state) &&
         context.surface_pitch == context.render_target_state.surface_pitch;
}

HudSwfBackendDrawContract
CaptureContract(const rex::graphics::NativeGuestDrawContext &context) {
  HudSwfBackendDrawContract contract;
  contract.backend = static_cast<uint32_t>(context.backend);
  contract.backend_frame_sequence = context.backend_frame_sequence;
  contract.vertex_shader_hash = context.vertex_shader_hash;
  contract.pixel_shader_hash = context.pixel_shader_hash;
  contract.primitive_type = context.primitive_type;
  contract.submitted_vertex_count = context.guest_vertex_or_index_count;
  contract.host_vertex_count = context.vertex_or_index_count;
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
  contract.target = {
      .rb_color_info_0 = context.render_target_state.rb_color_info_0,
      .rb_depth_info = context.render_target_state.rb_depth_info,
      .rb_surface_info = context.render_target_state.rb_surface_info,
      .rb_modecontrol = context.render_target_state.rb_modecontrol,
      .color_edram_base = context.render_target_state.color_edram_base,
      .depth_edram_base = context.render_target_state.depth_edram_base,
      .surface_pitch = context.render_target_state.surface_pitch,
      .edram_mode = context.render_target_state.edram_mode,
      .valid = RawTargetStateMatchesDecoded(context.render_target_state),
  };
  contract.indexed = context.indexed;
  contract.primitive_restart_enabled = context.primitive_restart_enabled;
  contract.render_pass_key_valid = context.render_pass_key_valid;
  contract.draw_state_contract_valid = context.draw_state_contract_valid;
  contract.rasterizer_mode_control_valid =
      context.rasterizer_mode_control_valid;
  contract.attachment_contract_valid =
      context.borrowed_attachment_contract_valid;
  contract.complete = ContextContractComplete(context);
  return contract;
}

HudSwfBackendEligibilityContract CaptureEligibilityContract(
    const rex::graphics::NativeGuestDrawEligibilityContext &context) {
  HudSwfBackendEligibilityContract contract;
  contract.backend = static_cast<uint32_t>(context.backend);
  contract.backend_frame_sequence = context.backend_frame_sequence;
  contract.vertex_shader_hash = context.vertex_shader_hash;
  contract.pixel_shader_hash = context.pixel_shader_hash;
  contract.primitive_type = context.primitive_type;
  contract.submitted_vertex_count = context.guest_vertex_or_index_count;
  contract.host_vertex_count = context.vertex_or_index_count;
  contract.processed_index_buffer_type =
      context.processed_index_buffer_type;
  contract.processed_index_buffer_present =
      context.processed_index_buffer_present;
  contract.shader_32bit_index_dma = context.shader_32bit_index_dma;
  contract.memexport_writes_possible = context.memexport_writes_possible;
  contract.host_render_targets = context.host_render_targets;
  contract.matcher_eligible = context.eligible;
  contract.valid =
      context.backend == rex::graphics::NativeGuestOutputBackend::kVulkan &&
      context.backend_frame_sequence != 0 && HashesMatch(context) &&
      context.primitive_type != 0 &&
      context.guest_vertex_or_index_count != 0 &&
      context.vertex_or_index_count != 0 &&
      !context.memexport_writes_possible && context.host_render_targets;
  return contract;
}

uint32_t TitleVertexCount(const HudSwfFrameSnapshot &title) {
  uint64_t total = 0;
  for (const HudSwfVertexBatchSnapshot &batch : title.batches) {
    total += batch.vertex_count;
  }
  return total <= std::numeric_limits<uint32_t>::max()
             ? static_cast<uint32_t>(total)
             : 0;
}

bool TitleReferenceShapeValid(const HudSwfFrameSnapshot &title,
                              uint32_t &four_vertex_draws,
                              uint32_t &six_vertex_draws) {
  four_vertex_draws = 0;
  six_vertex_draws = 0;
  if (!title.valid() ||
      title.batches.size() != HudSwfBackendFrameSnapshot::kReferenceDrawCount ||
      title.captured_vertex_bytes !=
          static_cast<size_t>(
              HudSwfBackendFrameSnapshot::kReferenceVertexCount) *
              HudSwfVertexBatchSnapshot::kVertexStride) {
    return false;
  }

  uint64_t previous_order = 0;
  for (const HudSwfVertexBatchSnapshot &batch : title.batches) {
    if (batch.order <= previous_order || batch.texture_bind_order == 0 ||
        PrimitiveTypeForBatchKind(batch.batch_kind) == 0 ||
        batch.vertex_bytes.size() !=
            static_cast<size_t>(batch.vertex_count) *
                HudSwfVertexBatchSnapshot::kVertexStride) {
      return false;
    }
    previous_order = batch.order;
    four_vertex_draws += batch.vertex_count == 4;
    six_vertex_draws += batch.vertex_count == 6;
    if (batch.vertex_count != 4 && batch.vertex_count != 6) {
      return false;
    }
  }
  return TitleVertexCount(title) ==
             HudSwfBackendFrameSnapshot::kReferenceVertexCount &&
         four_vertex_draws ==
             HudSwfBackendFrameSnapshot::kReferenceFourVertexDrawCount &&
         six_vertex_draws ==
             HudSwfBackendFrameSnapshot::kReferenceSixVertexDrawCount;
}

bool EligibilityMatchesBatch(const HudSwfVertexBatchSnapshot &batch,
                             const EligibilityEvent &event) {
  return event.contract.valid &&
         event.contract.primitive_type ==
             PrimitiveTypeForBatchKind(batch.batch_kind) &&
         event.contract.submitted_vertex_count == batch.vertex_count;
}

std::optional<std::vector<size_t>>
FindEarliestOrderedMatch(const HudSwfFrameSnapshot &title,
                         const std::vector<EligibilityEvent> &events) {
  std::vector<size_t> selected;
  selected.reserve(title.batches.size());
  size_t event_index = 0;
  for (const HudSwfVertexBatchSnapshot &batch : title.batches) {
    while (event_index < events.size() &&
           !EligibilityMatchesBatch(batch, events[event_index])) {
      ++event_index;
    }
    if (event_index == events.size()) {
      return std::nullopt;
    }
    selected.push_back(event_index++);
  }
  return selected;
}

std::optional<std::vector<size_t>>
FindLatestOrderedMatch(const HudSwfFrameSnapshot &title,
                       const std::vector<EligibilityEvent> &events) {
  std::vector<size_t> selected(title.batches.size());
  size_t event_end = events.size();
  for (size_t batch_end = title.batches.size(); batch_end != 0; --batch_end) {
    const size_t batch_index = batch_end - 1;
    while (event_end != 0 &&
           !EligibilityMatchesBatch(title.batches[batch_index],
                                    events[event_end - 1])) {
      --event_end;
    }
    if (event_end == 0) {
      return std::nullopt;
    }
    selected[batch_index] = --event_end;
  }
  return selected;
}

FrameLedger *FindPendingFrameLocked(uint64_t sequence) {
  const auto found = std::ranges::find_if(
      g_frames, [sequence](const FrameLedger &frame) {
        return !frame.finalized && frame.sequence == sequence;
      });
  return found != g_frames.end() ? &*found : nullptr;
}

void UpdatePendingCountsLocked() {
  g_telemetry.pending_frames =
      static_cast<uint32_t>(std::ranges::count_if(
          g_frames,
          [](const FrameLedger &frame) { return !frame.finalized; }));
  g_telemetry.queued_backend_events =
      static_cast<uint32_t>(g_eligibility_events.size() +
                            g_late_events.size());
}

bool LateMatchesEligibility(
    const HudSwfBackendDrawContract &late,
    const HudSwfBackendEligibilityContract &eligibility) {
  return late.backend_frame_sequence == eligibility.backend_frame_sequence &&
         late.vertex_shader_hash == eligibility.vertex_shader_hash &&
         late.pixel_shader_hash == eligibility.pixel_shader_hash &&
         late.primitive_type == eligibility.primitive_type &&
         late.submitted_vertex_count == eligibility.submitted_vertex_count &&
         late.host_vertex_count == eligibility.host_vertex_count;
}

std::optional<std::vector<size_t>>
MapLateContractsToEligibility(const std::vector<EligibilityEvent> &eligibility,
                              const std::vector<LateBackendEvent> &late) {
  std::vector<size_t> mapping(eligibility.size(), SIZE_MAX);
  size_t late_index = 0;
  for (size_t eligibility_index = 0;
       eligibility_index < eligibility.size(); ++eligibility_index) {
    const HudSwfBackendEligibilityContract &event =
        eligibility[eligibility_index].contract;
    if (!event.matcher_eligible) {
      continue;
    }
    if (late_index == late.size() ||
        !LateMatchesEligibility(late[late_index].contract, event)) {
      return std::nullopt;
    }
    mapping[eligibility_index] = late_index++;
  }
  if (late_index != late.size()) {
    return std::nullopt;
  }
  return mapping;
}

void LogSnapshot(const HudSwfBackendFrameSnapshot &snapshot) {
  const HudSwfBackendDrawContract empty{};
  const HudSwfBackendDrawContract &first =
      !snapshot.late_contracts.empty() ? snapshot.late_contracts.front()
                                       : empty;
  REXLOG_INFO(
      "Table Tennis HUD/SWF backend observer: frame={} backend_frame={} "
      "title[draws={} vertices={} four={} six={} reference={}] "
      "backend[eligibility_events={} joined={} vertices={} unmatched={} "
      "prefix={} suffix={} late_events={} late_joined={} late_missing={} "
      "dropped={}] unique={} ambiguous={} mismatches={} "
      "late_state[first primitive={} guest_count={} host_count={} "
      "pass={:08X}/{} pitch={} target={:08X}/{:08X}/{}/{} "
      "depth={:08X} mask={:08X} color={:08X} blend={:08X} "
      "raster={:08X}/{} restart={:08X}/{} attachments={}/{} "
      "color0={} depth_fmt={} stencil_fmt={} samples={} "
      "sample_mask={:016X} complete={}] "
      "observer_complete={} replay_ready=false "
      "missing[constants=true sampler=true viewport_scissor=true "
      "target_handoff=true] observer_only=true guest_suppressed=false",
      snapshot.sequence, snapshot.backend_frame_sequence,
      snapshot.title_draw_count, snapshot.title_vertex_count,
      snapshot.title_four_vertex_draw_count,
      snapshot.title_six_vertex_draw_count,
      snapshot.title_reference_shape_valid,
      snapshot.backend_family_event_count, snapshot.joined_draw_count,
      snapshot.joined_vertex_count, snapshot.unmatched_backend_event_count,
      snapshot.unmatched_backend_prefix_count,
      snapshot.unmatched_backend_suffix_count,
      snapshot.late_contract_event_count,
      snapshot.joined_late_contract_count,
      snapshot.missing_late_contract_count,
      snapshot.dropped_backend_event_count, snapshot.unique_ordered_join,
      snapshot.ambiguous_ordered_join_count,
      snapshot.sequence_mismatch_count, first.primitive_type,
      first.submitted_vertex_count, first.host_vertex_count,
      first.render_pass_key, first.render_pass_key_valid, first.surface_pitch,
      first.target.color_edram_base, first.target.depth_edram_base,
      first.target.surface_pitch, first.target.edram_mode,
      first.normalized_depth_control, first.normalized_color_mask,
      first.color_control, first.blend_control_0,
      first.rasterizer_mode_control, first.rasterizer_mode_control_valid,
      first.primitive_restart_index, first.primitive_restart_enabled,
      first.color_attachment_count, first.attachment_contract_valid,
      first.color_attachment_formats[0], first.depth_attachment_format,
      first.stencil_attachment_format, first.sample_count, first.sample_mask,
      first.complete, snapshot.observer_complete());
}

void PublishRejectedFrameLocked(FrameLedger &frame,
                                HudSwfBackendFrameSnapshot snapshot) {
  frame.finalized = true;
  g_published_frame =
      std::make_shared<HudSwfBackendFrameSnapshot>(std::move(snapshot));
  ++g_telemetry.rejected_frames;
  g_telemetry.latest_published_sequence = frame.sequence;
  const uint32_t interval =
      REXCVAR_GET(tabletennis_native_hud_swf_backend_log_interval);
  if (!g_announced_rejection || frame.sequence % interval == 0) {
    g_announced_rejection = true;
    LogSnapshot(*g_published_frame);
  }
}

void AnalyzeFrameLocked(FrameLedger &frame) {
  ++g_telemetry.analyzed_frames;
  HudSwfBackendFrameSnapshot snapshot;
  snapshot.sequence = frame.sequence;
  snapshot.title = frame.title;
  snapshot.backend_family_event_count =
      static_cast<uint32_t>(frame.eligibility_events.size());
  snapshot.late_contract_event_count =
      static_cast<uint32_t>(frame.late_events.size());
  snapshot.dropped_backend_event_count =
      frame.dropped_eligibility_event_count +
      frame.dropped_backend_event_count;
  snapshot.late_contracts.reserve(frame.late_events.size());
  for (const LateBackendEvent &late : frame.late_events) {
    snapshot.late_contracts.push_back(late.contract);
  }

  if (frame.title == nullptr) {
    PublishRejectedFrameLocked(frame, std::move(snapshot));
    return;
  }

  snapshot.title_draw_count =
      static_cast<uint32_t>(frame.title->batches.size());
  snapshot.title_vertex_count = TitleVertexCount(*frame.title);
  snapshot.title_reference_shape_valid =
      TitleReferenceShapeValid(*frame.title,
                               snapshot.title_four_vertex_draw_count,
                               snapshot.title_six_vertex_draw_count);
  if (!snapshot.title_reference_shape_valid ||
      frame.dropped_eligibility_event_count != 0 ||
      frame.dropped_backend_event_count != 0) {
    PublishRejectedFrameLocked(frame, std::move(snapshot));
    return;
  }

  const auto earliest =
      FindEarliestOrderedMatch(*frame.title, frame.eligibility_events);
  const auto latest =
      FindLatestOrderedMatch(*frame.title, frame.eligibility_events);
  if (!earliest.has_value() || !latest.has_value()) {
    PublishRejectedFrameLocked(frame, std::move(snapshot));
    return;
  }
  if (*earliest != *latest) {
    snapshot.ambiguous_ordered_join_count = 1;
    ++g_telemetry.ambiguous_frames;
    PublishRejectedFrameLocked(frame, std::move(snapshot));
    return;
  }

  const std::vector<size_t> &selection = *earliest;
  snapshot.unique_ordered_join = true;
  snapshot.joined_draw_count = static_cast<uint32_t>(selection.size());
  snapshot.unmatched_backend_event_count =
      static_cast<uint32_t>(frame.eligibility_events.size() -
                            selection.size());
  snapshot.unmatched_backend_prefix_count =
      static_cast<uint32_t>(selection.front());
  snapshot.unmatched_backend_suffix_count = static_cast<uint32_t>(
      frame.eligibility_events.size() - 1 - selection.back());
  snapshot.backend_frame_sequence =
      frame.eligibility_events[selection.front()]
          .contract.backend_frame_sequence;
  snapshot.draws.reserve(selection.size());

  const auto late_mapping = MapLateContractsToEligibility(
      frame.eligibility_events, frame.late_events);
  if (!late_mapping.has_value()) {
    PublishRejectedFrameLocked(frame, std::move(snapshot));
    return;
  }

  bool all_draws_valid = true;
  for (size_t batch_index = 0; batch_index < selection.size();
       ++batch_index) {
    const HudSwfVertexBatchSnapshot &batch =
        frame.title->batches[batch_index];
    const size_t eligibility_index = selection[batch_index];
    const HudSwfBackendEligibilityContract &eligibility =
        frame.eligibility_events[eligibility_index].contract;
    const bool sequence_matches =
        eligibility.backend_frame_sequence == frame.sequence;
    const bool draw_valid =
        sequence_matches && eligibility.valid &&
        eligibility.primitive_type ==
            PrimitiveTypeForBatchKind(batch.batch_kind) &&
        eligibility.submitted_vertex_count == batch.vertex_count;
    const size_t late_index = (*late_mapping)[eligibility_index];
    const bool late_present = late_index != SIZE_MAX;
    const HudSwfBackendDrawContract late =
        late_present ? frame.late_events[late_index].contract
                     : HudSwfBackendDrawContract{};
    snapshot.sequence_mismatch_count += !sequence_matches;
    snapshot.joined_vertex_count += batch.vertex_count;
    snapshot.joined_late_contract_count += late_present;
    all_draws_valid &= draw_valid;
    snapshot.draws.push_back({
        .batch_index = static_cast<uint32_t>(batch_index),
        .eligibility_event_index =
            static_cast<uint32_t>(eligibility_index),
        .title_order = batch.order,
        .texture_bind_order = batch.texture_bind_order,
        .batch_kind = batch.batch_kind,
        .decoded_primitive_type =
            PrimitiveTypeForBatchKind(batch.batch_kind),
        .vertex_count = batch.vertex_count,
        .eligibility = eligibility,
        .late_backend = late,
        .late_backend_present = late_present,
        .valid = draw_valid,
    });
  }
  snapshot.missing_late_contract_count =
      snapshot.joined_draw_count - snapshot.joined_late_contract_count;

  frame.finalized = true;
  g_published_frame =
      std::make_shared<HudSwfBackendFrameSnapshot>(std::move(snapshot));
  g_telemetry.joined_draws += g_published_frame->joined_draw_count;
  g_telemetry.latest_published_sequence = frame.sequence;
  if (all_draws_valid && g_published_frame->observer_complete()) {
    ++g_telemetry.observer_complete_frames;
  } else {
    ++g_telemetry.rejected_frames;
  }

  const uint32_t interval =
      REXCVAR_GET(tabletennis_native_hud_swf_backend_log_interval);
  const bool first_complete =
      g_published_frame->observer_complete() && !g_announced_complete;
  const bool first_rejection =
      !g_published_frame->observer_complete() && !g_announced_rejection;
  if (first_complete || first_rejection || frame.sequence % interval == 0) {
    g_announced_complete |= g_published_frame->observer_complete();
    g_announced_rejection |= !g_published_frame->observer_complete();
    LogSnapshot(*g_published_frame);
  }
}

void ReconcileEligibilityEventsLocked() {
  auto event = g_eligibility_events.begin();
  while (event != g_eligibility_events.end()) {
    const uint64_t sequence = event->contract.backend_frame_sequence;
    if (FrameLedger *frame = FindPendingFrameLocked(sequence)) {
      if (frame->eligibility_events.size() ==
          kMaximumEligibilityEventsPerFrame) {
        ++frame->dropped_eligibility_event_count;
        ++g_telemetry.eligibility_events_dropped;
      } else {
        frame->eligibility_events.push_back(std::move(*event));
        ++g_telemetry.eligibility_events_joined_to_frame;
      }
      event = g_eligibility_events.erase(event);
      continue;
    }
    if (sequence <= g_latest_title_sequence) {
      ++g_telemetry.eligibility_events_without_title_frame;
      event = g_eligibility_events.erase(event);
      continue;
    }
    ++event;
  }
}

void ReconcileLateEventsLocked() {
  auto event = g_late_events.begin();
  while (event != g_late_events.end()) {
    const uint64_t sequence = event->contract.backend_frame_sequence;
    if (FrameLedger *frame = FindPendingFrameLocked(sequence)) {
      if (frame->late_events.size() == kMaximumLateEventsPerFrame) {
        ++frame->dropped_backend_event_count;
        ++g_telemetry.backend_events_dropped;
      } else {
        frame->late_events.push_back(std::move(*event));
        ++g_telemetry.backend_events_joined_to_frame;
      }
      event = g_late_events.erase(event);
      continue;
    }
    if (sequence <= g_latest_title_sequence) {
      ++g_telemetry.backend_events_without_title_frame;
      event = g_late_events.erase(event);
      continue;
    }
    ++event;
  }
}

void ReconcileEventsLocked() {
  ReconcileEligibilityEventsLocked();
  ReconcileLateEventsLocked();
}

void AnalyzeCompletedFramesLocked() {
  for (FrameLedger &frame : g_frames) {
    if (!frame.finalized && frame.sequence < g_latest_backend_sequence) {
      AnalyzeFrameLocked(frame);
    }
  }
}

void ExpireFramesLocked() {
  while (g_frames.size() > kMaximumRetainedFrames) {
    if (!g_frames.front().finalized) {
      HudSwfBackendFrameSnapshot snapshot;
      snapshot.sequence = g_frames.front().sequence;
      snapshot.title = g_frames.front().title;
      snapshot.dropped_backend_event_count =
          g_frames.front().dropped_eligibility_event_count +
          g_frames.front().dropped_backend_event_count;
      PublishRejectedFrameLocked(g_frames.front(), std::move(snapshot));
    }
    g_frames.pop_front();
  }
}

void ResetLocked() {
  g_frames.clear();
  g_eligibility_events.clear();
  g_late_events.clear();
  g_published_frame.reset();
  g_telemetry = {};
  g_latest_title_sequence = 0;
  g_latest_backend_sequence = 0;
  g_announced_hash_callback = false;
  g_announced_complete = false;
  g_announced_rejection = false;
}

} // namespace

bool HudSwfBackendFrameSnapshot::observer_complete() const {
  if (sequence == 0 || backend_frame_sequence != sequence || title == nullptr ||
      !title->valid() || !title_reference_shape_valid ||
      title_draw_count != kReferenceDrawCount ||
      title_vertex_count != kReferenceVertexCount ||
      title_four_vertex_draw_count != kReferenceFourVertexDrawCount ||
      title_six_vertex_draw_count != kReferenceSixVertexDrawCount ||
      backend_family_event_count != kReferenceDrawCount + 1 ||
      late_contract_event_count != kReferenceTriangleFanDrawCount + 1 ||
      joined_late_contract_count != kReferenceTriangleFanDrawCount ||
      missing_late_contract_count !=
          kReferenceDrawCount - kReferenceTriangleFanDrawCount ||
      joined_draw_count != kReferenceDrawCount ||
      joined_vertex_count != kReferenceVertexCount ||
      draws.size() != kReferenceDrawCount || !unique_ordered_join ||
      unmatched_backend_event_count != 1 ||
      unmatched_backend_prefix_count != 1 ||
      unmatched_backend_suffix_count != 0 ||
      dropped_backend_event_count != 0 ||
      ambiguous_ordered_join_count != 0 || sequence_mismatch_count != 0) {
    return false;
  }
  uint32_t triangle_lists = 0;
  uint32_t triangle_fans = 0;
  uint32_t triangle_strips = 0;
  for (size_t index = 0; index < draws.size(); ++index) {
    const HudSwfBackendDrawProof &draw = draws[index];
    if (!draw.valid || draw.batch_index != index ||
        draw.eligibility_event_index != index + 1 ||
        !draw.eligibility.valid ||
        draw.eligibility.backend_frame_sequence != sequence ||
        draw.eligibility.vertex_shader_hash != kHudVertexShaderHash ||
        draw.eligibility.pixel_shader_hash != kHudPixelShaderHash ||
        draw.vertex_count != title->batches[index].vertex_count ||
        draw.title_order != title->batches[index].order ||
        draw.texture_bind_order !=
            title->batches[index].texture_bind_order ||
        draw.batch_kind != title->batches[index].batch_kind ||
        draw.decoded_primitive_type !=
            PrimitiveTypeForBatchKind(draw.batch_kind) ||
        draw.eligibility.primitive_type != draw.decoded_primitive_type ||
        draw.eligibility.submitted_vertex_count != draw.vertex_count ||
        draw.late_backend_present != draw.eligibility.matcher_eligible) {
      return false;
    }
    if (draw.late_backend_present &&
        (!draw.late_backend.complete ||
         !LateMatchesEligibility(draw.late_backend, draw.eligibility))) {
      return false;
    }
    triangle_lists += draw.decoded_primitive_type == 4;
    triangle_fans += draw.decoded_primitive_type == 5;
    triangle_strips += draw.decoded_primitive_type == 6;
  }
  return triangle_lists == kReferenceTriangleListDrawCount &&
         triangle_fans == kReferenceTriangleFanDrawCount &&
         triangle_strips == kReferenceTriangleStripDrawCount;
}

bool HudSwfBackendObserverEnabled() { return HudSwfCaptureEnabled(); }

void ObserveHudSwfBackendDraw(
    const rex::graphics::NativeGuestDrawContext &context) {
  if (!HudSwfBackendObserverEnabled()) {
    return;
  }

  const bool hashes_match = HashesMatch(context);
  std::lock_guard lock(g_observer_mutex);
  ++g_telemetry.backend_callbacks;
  g_latest_backend_sequence =
      std::max(g_latest_backend_sequence, context.backend_frame_sequence);
  g_telemetry.latest_backend_sequence = g_latest_backend_sequence;
  g_telemetry.backend_hash_matches += hashes_match;
  g_telemetry.backend_early_probes +=
      hashes_match && !context.render_pass_key_valid;

  if (hashes_match && !g_announced_hash_callback) {
    g_announced_hash_callback = true;
    REXLOG_INFO(
        "Table Tennis HUD/SWF backend hash callback: stage={} frame={} "
        "vs={:016X} ps={:016X} primitive={} guest_count={} host_count={} "
        "pass={:08X}/{} target={:08X}/{:08X}/{}/{} "
        "state={}/{} attachments={}/{} observer_only=true "
        "guest_suppressed=false",
        context.render_pass_key_valid ? "late" : "early",
        context.backend_frame_sequence, context.vertex_shader_hash,
        context.pixel_shader_hash, context.primitive_type,
        context.guest_vertex_or_index_count, context.vertex_or_index_count,
        context.render_pass_key, context.render_pass_key_valid,
        context.render_target_state.color_edram_base,
        context.render_target_state.depth_edram_base,
        context.render_target_state.surface_pitch,
        context.render_target_state.edram_mode,
        context.draw_state_contract_valid,
        context.rasterizer_mode_control_valid,
        context.color_attachment_count,
        context.borrowed_attachment_contract_valid);
  }

  if (!hashes_match || !context.render_pass_key_valid) {
    AnalyzeCompletedFramesLocked();
    UpdatePendingCountsLocked();
    return;
  }

  ++g_telemetry.backend_late_events;
  if (g_late_events.size() == kMaximumQueuedLateEvents) {
    g_late_events.pop_front();
    ++g_telemetry.backend_events_dropped;
  }
  g_late_events.push_back({.contract = CaptureContract(context)});
  ReconcileEventsLocked();
  AnalyzeCompletedFramesLocked();
  UpdatePendingCountsLocked();
}

void ObserveHudSwfBackendEligibility(
    const rex::graphics::NativeGuestDrawEligibilityContext &context) {
  if (!HudSwfBackendObserverEnabled()) {
    return;
  }

  const bool hashes_match = HashesMatch(context);
  std::lock_guard lock(g_observer_mutex);
  ++g_telemetry.eligibility_callbacks;
  g_latest_backend_sequence =
      std::max(g_latest_backend_sequence, context.backend_frame_sequence);
  g_telemetry.latest_backend_sequence = g_latest_backend_sequence;
  g_telemetry.eligibility_hash_matches += hashes_match;
  if (!hashes_match) {
    AnalyzeCompletedFramesLocked();
    UpdatePendingCountsLocked();
    return;
  }

  ++g_telemetry.eligibility_events;
  if (g_eligibility_events.size() == kMaximumQueuedEligibilityEvents) {
    g_eligibility_events.pop_front();
    ++g_telemetry.eligibility_events_dropped;
  }
  g_eligibility_events.push_back(
      {.contract = CaptureEligibilityContract(context)});
  ReconcileEventsLocked();
  AnalyzeCompletedFramesLocked();
  UpdatePendingCountsLocked();
}

void HudSwfBackendObserverFrameEnd() {
  const bool enabled = HudSwfBackendObserverEnabled();
  const std::shared_ptr<const HudSwfFrameSnapshot> title =
      LatestHudSwfFrameSnapshot();
  std::lock_guard lock(g_observer_mutex);
  if (!enabled) {
    ResetLocked();
    return;
  }
  if (title == nullptr || title->sequence == 0) {
    UpdatePendingCountsLocked();
    return;
  }

  ++g_telemetry.title_frames;
  g_telemetry.title_nonempty_frames += !title->batches.empty();
  g_latest_title_sequence = std::max(g_latest_title_sequence, title->sequence);
  g_telemetry.latest_title_sequence = g_latest_title_sequence;

  uint32_t four_vertex_draws = 0;
  uint32_t six_vertex_draws = 0;
  const bool reference_shape =
      TitleReferenceShapeValid(*title, four_vertex_draws, six_vertex_draws);
  g_telemetry.title_reference_shape_frames += reference_shape;
  if (!title->batches.empty()) {
    const bool duplicate =
        std::ranges::any_of(g_frames, [title](const FrameLedger &frame) {
          return frame.sequence == title->sequence;
        });
    if (!duplicate) {
      FrameLedger frame;
      frame.sequence = title->sequence;
      frame.title = title;
      g_frames.push_back(std::move(frame));
    }
  }

  ReconcileEventsLocked();
  AnalyzeCompletedFramesLocked();
  ExpireFramesLocked();
  UpdatePendingCountsLocked();
}

std::shared_ptr<const HudSwfBackendFrameSnapshot>
LatestHudSwfBackendFrameSnapshot() {
  std::lock_guard lock(g_observer_mutex);
  return g_published_frame;
}

HudSwfBackendObserverTelemetry LatestHudSwfBackendObserverTelemetry() {
  std::lock_guard lock(g_observer_mutex);
  HudSwfBackendObserverTelemetry telemetry = g_telemetry;
  telemetry.latest_title_sequence = g_latest_title_sequence;
  telemetry.latest_backend_sequence = g_latest_backend_sequence;
  telemetry.pending_frames =
      static_cast<uint32_t>(std::ranges::count_if(
          g_frames,
          [](const FrameLedger &frame) { return !frame.finalized; }));
  telemetry.queued_backend_events =
      static_cast<uint32_t>(g_eligibility_events.size() +
                            g_late_events.size());
  telemetry.replay_ready = false;
  return telemetry;
}

} // namespace tabletennis::native
