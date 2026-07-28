#pragma once

#include "native/tabletennis_late_phase_ledger.h"
#include "native/tabletennis_player_2ac_renderer.h"
#include "native/tabletennis_transition_state_observer.h"

#include <cstdint>
#include <memory>

namespace tabletennis::native {

// The first bounded unit of a future late native replay: the real
// MAIN-to-COMP transition followed by one uniquely joined, immutable
// single-stream-32 2AC draw. This is a CPU-side proof only.
enum class LateCompFirstSliceBlocker : uint8_t {
  kNone = 0,
  kMissingTransitionState,
  kMissingLatePhase,
  kMissingCompositePlan,
  kSequenceMismatch,
  kInvalidTransition,
  kInvalidLatePhase,
  kInvalidCompositePlan,
  kTransitionIdentityMismatch,
  kFirstSingleStream32Missing,
  kFirstDrawIdentityMismatch,
  kMissingVertexProgramPort,
  kMissingResolvedSceneFetch,
  kMissingPrivateCompTransaction,
};

struct LateCompFirstSlicePlan {
  uint64_t sequence = 0;
  uint32_t late_transition_backend_index = 0;
  uint32_t late_player_backend_index = 0;
  uint32_t player_draw_index = 0;
  uint32_t submitted_index_count = 0;
  uint32_t referenced_vertex_count = 0;
  uint64_t decoded_geometry_fingerprint = 0;
  LateCompFirstSliceBlocker blocker =
      LateCompFirstSliceBlocker::kMissingTransitionState;

  std::shared_ptr<const MainToCompTransitionStateSnapshot> transition;
  std::shared_ptr<const LatePhaseFrameSnapshot> late_phase;
  std::shared_ptr<const Player2ACCompositeRenderPlan> composite;

  bool transition_state_exact = false;
  bool late_order_exact = false;
  bool first_draw_identity_exact = false;
  bool first_draw_geometry_exact = false;
  bool vertex_program_port_available = false;
  bool resolved_scene_fetch_available = false;
  bool private_comp_transaction_available = false;
  bool gpu_recording_ready = false;

  bool observer_complete() const;
};

bool LateCompFirstSliceObserverEnabled();
void LateCompFirstSliceObserverFrameEnd();
std::shared_ptr<const LateCompFirstSlicePlan> LatestLateCompFirstSlicePlan();
const char *LateCompFirstSliceBlockerName(LateCompFirstSliceBlocker blocker);

} // namespace tabletennis::native
