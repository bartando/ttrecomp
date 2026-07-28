#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace rex::graphics {
struct NativeGuestDrawEligibilityContext;
}

namespace tabletennis::native {

struct HudSwfBackendFrameSnapshot;
struct Player2ACFrameSnapshot;

enum class LatePhaseDrawKind : uint8_t {
  kMainToCompTransition = 0,
  kTransitionSetup,
  kPlayer2AC,
  kPost,
  kGameplayHud,
  kFinalCompositor,
};

enum class LatePhaseRejectReason : uint8_t {
  kNone = 0,
  kMissingDependencyProof,
  kDroppedBackendEvents,
  kHudJoinMissing,
  kHudJoinAmbiguous,
  kHudNotContiguous,
  kPlayer2ACJoinMissing,
  kPlayer2ACJoinAmbiguous,
  kPlayer2ACNotContiguous,
  kTransitionJoinMissing,
  kTransitionJoinAmbiguous,
  kTransitionSetupTooLarge,
  kPostPrefixMissing,
  kPostPrefixAmbiguous,
  kFinalCompositorMissing,
  kFinalCompositorAmbiguous,
  kFinalCompositorNotImmediate,
};

// Value-only copy from the backend pre-gate callback. The callback sees every
// translated draw, including auto-indexed draws that never reach the selective
// replacement matcher.
struct LatePhaseDrawIdentity {
  uint64_t backend_frame_sequence = 0;
  uint64_t vertex_shader_hash = 0;
  uint64_t pixel_shader_hash = 0;
  uint32_t backend_draw_index = 0;
  uint32_t primitive_type = 0;
  uint32_t submitted_count = 0;
  uint32_t host_count = 0;
  uint32_t guest_index_base = 0;
  uint32_t processed_index_buffer_type = 0;
  LatePhaseDrawKind kind = LatePhaseDrawKind::kPost;
  bool processed_index_buffer_present = false;
  bool shader_32bit_index_dma = false;
  bool memexport_writes_possible = false;
  bool host_render_targets = false;
  bool valid = false;
};

// One immutable, observer-only description of the real late frame sequence.
// The vector starts at the MAIN-to-COMP marker and ends at the final
// compositor marker. It deliberately carries no backend objects and grants no
// replacement route.
struct LatePhaseFrameSnapshot {
  uint64_t sequence = 0;
  uint64_t backend_frame_sequence = 0;
  uint32_t backend_frame_draw_count = 0;
  uint32_t draws_before_transition = 0;
  uint32_t transition_draw_index = 0;
  uint32_t transition_candidate_count = 0;
  uint32_t transition_setup_count = 0;
  uint32_t player_2ac_first_backend_index = UINT32_MAX;
  uint32_t player_2ac_last_backend_index = UINT32_MAX;
  uint32_t player_2ac_begin = 0;
  uint32_t player_2ac_count = 0;
  uint32_t post_begin = 0;
  uint32_t post_count = 0;
  uint32_t hud_begin = 0;
  uint32_t hud_count = 0;
  uint32_t final_compositor_candidate_count = 0;
  uint32_t final_compositor_index = UINT32_MAX;
  uint32_t draws_after_late_window = 0;
  uint32_t dropped_event_count = 0;
  uint32_t ambiguous_layout_count = 0;
  LatePhaseRejectReason reject_reason =
      LatePhaseRejectReason::kMissingDependencyProof;
  bool unique_player_2ac_join = false;
  bool unique_hud_join = false;
  bool final_compositor_present = false;
  bool reference_anchors_valid = false;
  std::shared_ptr<const Player2ACFrameSnapshot> player_2ac;
  std::shared_ptr<const HudSwfBackendFrameSnapshot> hud;
  // Bounded evidence immediately preceding the independently joined 2AC
  // sequence. It exists solely to diagnose a changed transition identity.
  std::vector<LatePhaseDrawIdentity> player_2ac_predecessors;
  // Bounded evidence immediately following the joined HUD. A live phase may
  // omit the reference compositor entirely, so these identities prove absence
  // without promoting the first tail draw by ordinal.
  std::vector<LatePhaseDrawIdentity> hud_successors;
  std::vector<LatePhaseDrawIdentity> draws;

  bool observer_complete() const;
  bool replay_ready() const { return false; }
};

struct LatePhaseLedgerTelemetry {
  uint64_t callbacks = 0;
  uint64_t events = 0;
  uint64_t events_dropped = 0;
  uint64_t analyzed_frames = 0;
  uint64_t complete_frames = 0;
  uint64_t rejected_frames = 0;
  uint64_t latest_backend_sequence = 0;
  uint64_t latest_published_sequence = 0;
  uint32_t retained_backend_frames = 0;
  bool replay_ready = false;
};

bool LatePhaseLedgerEnabled();
void ObserveLatePhaseDrawEligibility(
    const rex::graphics::NativeGuestDrawEligibilityContext &context);

// Called after the 2AC and HUD backend observers publish at title Swap.
void LatePhaseLedgerFrameEnd();

std::shared_ptr<const LatePhaseFrameSnapshot> LatestLatePhaseFrameSnapshot();
LatePhaseLedgerTelemetry LatestLatePhaseLedgerTelemetry();
const char *LatePhaseRejectReasonName(LatePhaseRejectReason reason);

} // namespace tabletennis::native
