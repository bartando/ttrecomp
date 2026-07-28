#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace rex::graphics {
struct NativeGuestDrawContext;
struct NativeGuestDrawEligibilityContext;
}

namespace tabletennis::native {

struct HudSwfFrameSnapshot;

// Value-only copy of the guest target identity consumed by one translated
// gameplay-HUD draw. No backend object or guest pointer survives publication.
struct HudSwfBackendTargetContract {
  uint32_t rb_color_info_0 = 0;
  uint32_t rb_depth_info = 0;
  uint32_t rb_surface_info = 0;
  uint32_t rb_modecontrol = 0;
  uint32_t color_edram_base = 0;
  uint32_t depth_edram_base = 0;
  uint32_t surface_pitch = 0;
  uint32_t edram_mode = 0;
  bool valid = false;

  bool operator==(const HudSwfBackendTargetContract &) const = default;
};

// Immutable translated-backend contract for one F0B855/391847 draw. Values
// are observed, not promoted to serving policy. In particular, no blend,
// depth, raster, attachment, or target value is assumed from the old trace.
struct HudSwfBackendDrawContract {
  uint32_t backend = 0;
  uint64_t backend_frame_sequence = 0;
  uint64_t vertex_shader_hash = 0;
  uint64_t pixel_shader_hash = 0;
  uint32_t primitive_type = 0;
  uint32_t submitted_vertex_count = 0;
  uint32_t host_vertex_count = 0;
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
  HudSwfBackendTargetContract target{};
  bool indexed = false;
  bool primitive_restart_enabled = false;
  bool render_pass_key_valid = false;
  bool draw_state_contract_valid = false;
  bool rasterizer_mode_control_valid = false;
  bool attachment_contract_valid = false;
  bool complete = false;

  bool operator==(const HudSwfBackendDrawContract &) const = default;
};

// Pre-gate identity emitted for every translated draw, including ordinary
// auto-indexed triangle lists and strips that don't need a host index buffer
// and therefore never reach the selective replacement matcher.
struct HudSwfBackendEligibilityContract {
  uint32_t backend = 0;
  uint64_t backend_frame_sequence = 0;
  uint64_t vertex_shader_hash = 0;
  uint64_t pixel_shader_hash = 0;
  uint32_t primitive_type = 0;
  uint32_t submitted_vertex_count = 0;
  uint32_t host_vertex_count = 0;
  uint32_t processed_index_buffer_type = 0;
  bool processed_index_buffer_present = false;
  bool shader_32bit_index_dma = false;
  bool memexport_writes_possible = false;
  bool host_render_targets = false;
  bool matcher_eligible = false;
  bool valid = false;

  bool operator==(const HudSwfBackendEligibilityContract &) const = default;
};

// Exact ordered association between one immutable title batch and one live
// translated pre-gate draw. Only matcher-eligible draws may additionally own
// a late state contract; its absence on auto-indexed list/strip draws is
// expected and never upgraded to a full replay-state proof.
struct HudSwfBackendDrawProof {
  uint32_t batch_index = 0;
  uint32_t eligibility_event_index = 0;
  uint64_t title_order = 0;
  uint64_t texture_bind_order = 0;
  uint32_t batch_kind = 0;
  uint32_t decoded_primitive_type = 0;
  uint32_t vertex_count = 0;
  HudSwfBackendEligibilityContract eligibility{};
  HudSwfBackendDrawContract late_backend{};
  bool late_backend_present = false;
  bool valid = false;
};

struct HudSwfBackendFrameSnapshot {
  static constexpr uint32_t kReferenceDrawCount = 55;
  static constexpr uint32_t kReferenceVertexCount = 266;
  static constexpr uint32_t kReferenceFourVertexDrawCount = 32;
  static constexpr uint32_t kReferenceSixVertexDrawCount = 23;
  static constexpr uint32_t kReferenceTriangleListDrawCount = 7;
  static constexpr uint32_t kReferenceTriangleFanDrawCount = 16;
  static constexpr uint32_t kReferenceTriangleStripDrawCount = 32;

  uint64_t sequence = 0;
  uint64_t backend_frame_sequence = 0;
  uint32_t title_draw_count = 0;
  uint32_t title_vertex_count = 0;
  uint32_t title_four_vertex_draw_count = 0;
  uint32_t title_six_vertex_draw_count = 0;
  // Complete pre-gate F0B855/391847 family census for this backend frame.
  uint32_t backend_family_event_count = 0;
  // Partial late matcher stream: the 88-vertex quad prefix and converted
  // triangle fans only. Auto-indexed lists/strips correctly never appear.
  uint32_t late_contract_event_count = 0;
  uint32_t joined_late_contract_count = 0;
  uint32_t missing_late_contract_count = 0;
  uint32_t joined_draw_count = 0;
  uint32_t joined_vertex_count = 0;
  uint32_t unmatched_backend_event_count = 0;
  uint32_t unmatched_backend_prefix_count = 0;
  uint32_t unmatched_backend_suffix_count = 0;
  uint32_t dropped_backend_event_count = 0;
  uint32_t ambiguous_ordered_join_count = 0;
  uint32_t sequence_mismatch_count = 0;
  bool title_reference_shape_valid = false;
  bool unique_ordered_join = false;

  // These inputs do not exist in NativeGuestDrawContext yet. Keeping the
  // blockers explicit prevents a complete observer proof from accidentally
  // becoming a native replay approval.
  bool shader_constants_captured = false;
  bool sampler_state_captured = false;
  bool viewport_scissor_captured = false;
  bool target_handoff_proven = false;

  std::shared_ptr<const HudSwfFrameSnapshot> title;
  std::vector<HudSwfBackendDrawProof> draws;
  std::vector<HudSwfBackendDrawContract> late_contracts;

  bool observer_complete() const;

  // Deliberately false for this observer-only slice. Replay additionally
  // needs constants, sampler state, viewport/scissor, and target handoff.
  bool replay_ready() const { return false; }
};

struct HudSwfBackendObserverTelemetry {
  uint64_t backend_callbacks = 0;
  uint64_t backend_hash_matches = 0;
  uint64_t backend_early_probes = 0;
  uint64_t backend_late_events = 0;
  uint64_t eligibility_callbacks = 0;
  uint64_t eligibility_hash_matches = 0;
  uint64_t eligibility_events = 0;
  uint64_t eligibility_events_dropped = 0;
  uint64_t eligibility_events_without_title_frame = 0;
  uint64_t eligibility_events_joined_to_frame = 0;
  uint64_t backend_events_dropped = 0;
  uint64_t backend_events_without_title_frame = 0;
  uint64_t backend_events_joined_to_frame = 0;
  uint64_t title_frames = 0;
  uint64_t title_nonempty_frames = 0;
  uint64_t title_reference_shape_frames = 0;
  uint64_t analyzed_frames = 0;
  uint64_t observer_complete_frames = 0;
  uint64_t rejected_frames = 0;
  uint64_t ambiguous_frames = 0;
  uint64_t joined_draws = 0;
  uint64_t latest_title_sequence = 0;
  uint64_t latest_backend_sequence = 0;
  uint64_t latest_published_sequence = 0;
  uint32_t pending_frames = 0;
  uint32_t queued_backend_events = 0;
  bool replay_ready = false;
};

bool HudSwfBackendObserverEnabled();

// Feed the normal replacement matcher callback stream. Early probes are
// measured and ignored. Exact late F0B855/391847 events are retained in
// callback order and bucketed by the authoritative backend frame sequence.
void ObserveHudSwfBackendDraw(
    const rex::graphics::NativeGuestDrawContext &context);

// Feed the Vulkan pre-gate stream. Unlike the matcher callback this observes
// all 55 gameplay HUD draws, including the non-converted auto-indexed draws.
void ObserveHudSwfBackendEligibility(
    const rex::graphics::NativeGuestDrawEligibilityContext &context);

// Called immediately after HudSwfCaptureFrameEnd at the title's Swap boundary.
void HudSwfBackendObserverFrameEnd();

std::shared_ptr<const HudSwfBackendFrameSnapshot>
LatestHudSwfBackendFrameSnapshot();
HudSwfBackendObserverTelemetry LatestHudSwfBackendObserverTelemetry();

} // namespace tabletennis::native
