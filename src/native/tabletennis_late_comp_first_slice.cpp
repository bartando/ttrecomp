#include "native/tabletennis_late_comp_first_slice.h"

#include <mutex>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(
    tabletennis_native_late_comp_first_slice_observer, false, "Table Tennis",
    "Join the exact MAIN-to-COMP transition to the first real stride-32 2AC "
    "draw. Observer-only; records no commands and never suppresses guest work.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace tabletennis::native {
namespace {

constexpr uint64_t kTransitionVertexShader = 0xFA14ACFDF2DE3ED0ull;
constexpr uint64_t kTransitionPixelShader = 0x5E11FC7AE2F1C2BFull;
constexpr uint64_t kSingleStream32VertexShader = 0x4761A30F65AA309Cull;
constexpr uint64_t kPlayerCompositePixelShader = 0x2AC059EB5C7A942Full;
constexpr uint32_t kFullscreenPrimitive = 8;
constexpr uint32_t kFullscreenVertexCount = 3;

std::mutex g_mutex;
std::shared_ptr<const LateCompFirstSlicePlan> g_published;
uint64_t g_last_transition_sequence = UINT64_MAX;
uint64_t g_last_late_sequence = UINT64_MAX;
uint64_t g_last_composite_sequence = UINT64_MAX;
LateCompFirstSliceBlocker g_last_logged_blocker =
    LateCompFirstSliceBlocker::kNone;
bool g_announced_complete = false;

bool ExactTransitionIdentity(const LatePhaseDrawIdentity &draw) {
  return draw.valid && draw.kind == LatePhaseDrawKind::kMainToCompTransition &&
         draw.vertex_shader_hash == kTransitionVertexShader &&
         draw.pixel_shader_hash == kTransitionPixelShader &&
         draw.primitive_type == kFullscreenPrimitive &&
         draw.submitted_count == kFullscreenVertexCount;
}

bool SamePlayerIdentity(const LatePhaseDrawIdentity &late,
                        const Player2ACDrawProof &proof) {
  return late.valid && late.kind == LatePhaseDrawKind::kPlayer2AC &&
         late.vertex_shader_hash == kSingleStream32VertexShader &&
         late.pixel_shader_hash == kPlayerCompositePixelShader &&
         proof.valid() &&
         proof.title.kind == Player2ACTitleKind::kSingleStream32 &&
         proof.backend.vertex_shader_hash == late.vertex_shader_hash &&
         proof.backend.pixel_shader_hash == late.pixel_shader_hash &&
         proof.title.identity.primitive_type == late.primitive_type &&
         proof.title.identity.submitted_index_count == late.submitted_count &&
         proof.title.identity.guest_index_base == late.guest_index_base;
}

LateCompFirstSlicePlan
BuildPlan(std::shared_ptr<const MainToCompTransitionStateSnapshot> transition,
          std::shared_ptr<const LatePhaseFrameSnapshot> late_phase,
          std::shared_ptr<const Player2ACCompositeRenderPlan> composite) {
  LateCompFirstSlicePlan plan;
  plan.transition = std::move(transition);
  plan.late_phase = std::move(late_phase);
  plan.composite = std::move(composite);
  if (plan.transition == nullptr) {
    return plan;
  }
  plan.sequence = plan.transition->sequence;
  if (plan.late_phase == nullptr) {
    plan.blocker = LateCompFirstSliceBlocker::kMissingLatePhase;
    return plan;
  }
  if (plan.composite == nullptr) {
    plan.blocker = LateCompFirstSliceBlocker::kMissingCompositePlan;
    return plan;
  }
  if (plan.transition->sequence != plan.late_phase->sequence ||
      plan.transition->sequence != plan.composite->readiness.sequence) {
    plan.blocker = LateCompFirstSliceBlocker::kSequenceMismatch;
    return plan;
  }
  if (!plan.transition->valid()) {
    plan.blocker = LateCompFirstSliceBlocker::kInvalidTransition;
    return plan;
  }
  plan.transition_state_exact = true;
  if (!plan.late_phase->observer_complete()) {
    plan.blocker = LateCompFirstSliceBlocker::kInvalidLatePhase;
    return plan;
  }
  plan.late_order_exact = true;
  if (!plan.composite->observer_valid()) {
    plan.blocker = LateCompFirstSliceBlocker::kInvalidCompositePlan;
    return plan;
  }
  if (plan.late_phase->draws.empty() ||
      plan.late_phase->transition_draw_index >= plan.late_phase->draws.size() ||
      !ExactTransitionIdentity(
          plan.late_phase->draws[plan.late_phase->transition_draw_index])) {
    plan.blocker = LateCompFirstSliceBlocker::kTransitionIdentityMismatch;
    return plan;
  }
  plan.late_transition_backend_index =
      plan.late_phase->draws[plan.late_phase->transition_draw_index]
          .backend_draw_index;

  const Player2ACFrameSnapshot &player = *plan.composite->frame;
  size_t first_draw_index = player.draws.size();
  for (size_t draw_index = 0; draw_index < player.draws.size(); ++draw_index) {
    if (player.draws[draw_index].title.kind ==
        Player2ACTitleKind::kSingleStream32) {
      first_draw_index = draw_index;
      break;
    }
  }
  if (first_draw_index == player.draws.size() ||
      first_draw_index >= plan.composite->draws.size()) {
    plan.blocker = LateCompFirstSliceBlocker::kFirstSingleStream32Missing;
    return plan;
  }
  const size_t late_draw_index =
      static_cast<size_t>(plan.late_phase->player_2ac_begin) + first_draw_index;
  if (late_draw_index >= plan.late_phase->draws.size() ||
      !SamePlayerIdentity(plan.late_phase->draws[late_draw_index],
                          player.draws[first_draw_index])) {
    plan.blocker = LateCompFirstSliceBlocker::kFirstDrawIdentityMismatch;
    return plan;
  }
  const Player2ACDecodedDrawGeometry &geometry =
      plan.composite->draws[first_draw_index];
  if (!geometry.valid || geometry.kind != Player2ACTitleKind::kSingleStream32 ||
      geometry.decoded_fingerprint == 0 ||
      geometry.submitted_index_count !=
          player.draws[first_draw_index].title.identity.submitted_index_count) {
    plan.blocker = LateCompFirstSliceBlocker::kInvalidCompositePlan;
    return plan;
  }

  plan.player_draw_index = static_cast<uint32_t>(first_draw_index);
  plan.late_player_backend_index =
      plan.late_phase->draws[late_draw_index].backend_draw_index;
  plan.submitted_index_count = geometry.submitted_index_count;
  plan.referenced_vertex_count = geometry.referenced_vertex_count;
  plan.decoded_geometry_fingerprint = geometry.decoded_fingerprint;
  plan.first_draw_identity_exact = true;
  plan.first_draw_geometry_exact = true;

  // Promotion is intentionally one prerequisite at a time. The immutable
  // draw is real and ordered, but there is no exact 4761 native VS port, no
  // captured sampleable resolved-MAIN fetch for this draw, and no private
  // one-sample COMP transaction. Serving before all three exist would be a
  // fabricated renderer.
  plan.blocker = LateCompFirstSliceBlocker::kMissingVertexProgramPort;
  return plan;
}

void LogPlan(const LateCompFirstSlicePlan &plan) {
  REXLOG_INFO(
      "Table Tennis late COMP first slice: frame={} transition_backend={} "
      "player_backend={} player_draw={} indices={} referenced_vertices={} "
      "geometry={:016X} exact[transition={} order={} identity={} geometry={}] "
      "available[vs_port={} resolved_scene_fetch={} private_comp={}] "
      "blocker={} gpu_recording_ready={} observer_only=true "
      "guest_suppressed=false",
      plan.sequence, plan.late_transition_backend_index,
      plan.late_player_backend_index, plan.player_draw_index,
      plan.submitted_index_count, plan.referenced_vertex_count,
      plan.decoded_geometry_fingerprint, plan.transition_state_exact,
      plan.late_order_exact, plan.first_draw_identity_exact,
      plan.first_draw_geometry_exact, plan.vertex_program_port_available,
      plan.resolved_scene_fetch_available,
      plan.private_comp_transaction_available,
      LateCompFirstSliceBlockerName(plan.blocker), plan.gpu_recording_ready);
}

} // namespace

bool LateCompFirstSlicePlan::observer_complete() const {
  return sequence != 0 && transition != nullptr && late_phase != nullptr &&
         composite != nullptr && transition_state_exact && late_order_exact &&
         first_draw_identity_exact && first_draw_geometry_exact &&
         submitted_index_count != 0 && referenced_vertex_count != 0 &&
         decoded_geometry_fingerprint != 0 &&
         blocker == LateCompFirstSliceBlocker::kMissingVertexProgramPort &&
         !gpu_recording_ready;
}

bool LateCompFirstSliceObserverEnabled() {
  return REXCVAR_GET(tabletennis_native_late_comp_first_slice_observer);
}

void LateCompFirstSliceObserverFrameEnd() {
  std::lock_guard lock(g_mutex);
  if (!LateCompFirstSliceObserverEnabled()) {
    g_published.reset();
    g_last_transition_sequence = UINT64_MAX;
    g_last_late_sequence = UINT64_MAX;
    g_last_composite_sequence = UINT64_MAX;
    g_last_logged_blocker = LateCompFirstSliceBlocker::kNone;
    g_announced_complete = false;
    return;
  }

  const auto transition = LatestMainToCompTransitionStateSnapshot();
  const auto late_phase = LatestLatePhaseFrameSnapshot();
  const auto composite = LatestPlayer2ACCompositeRenderPlan();
  const uint64_t transition_sequence =
      transition != nullptr ? transition->sequence : 0;
  const uint64_t late_sequence =
      late_phase != nullptr ? late_phase->sequence : 0;
  const uint64_t composite_sequence =
      composite != nullptr ? composite->readiness.sequence : 0;
  if (transition_sequence == g_last_transition_sequence &&
      late_sequence == g_last_late_sequence &&
      composite_sequence == g_last_composite_sequence) {
    return;
  }
  g_last_transition_sequence = transition_sequence;
  g_last_late_sequence = late_sequence;
  g_last_composite_sequence = composite_sequence;
  auto plan = std::make_shared<LateCompFirstSlicePlan>(
      BuildPlan(transition, late_phase, composite));
  g_published = plan;
  const bool first_complete =
      plan->observer_complete() && !g_announced_complete;
  if (first_complete || plan->blocker != g_last_logged_blocker) {
    g_announced_complete |= plan->observer_complete();
    g_last_logged_blocker = plan->blocker;
    LogPlan(*plan);
  }
}

std::shared_ptr<const LateCompFirstSlicePlan> LatestLateCompFirstSlicePlan() {
  std::lock_guard lock(g_mutex);
  return g_published;
}

const char *LateCompFirstSliceBlockerName(LateCompFirstSliceBlocker blocker) {
  switch (blocker) {
  case LateCompFirstSliceBlocker::kNone:
    return "none";
  case LateCompFirstSliceBlocker::kMissingTransitionState:
    return "missing transition state";
  case LateCompFirstSliceBlocker::kMissingLatePhase:
    return "missing late phase";
  case LateCompFirstSliceBlocker::kMissingCompositePlan:
    return "missing composite plan";
  case LateCompFirstSliceBlocker::kSequenceMismatch:
    return "sequence mismatch";
  case LateCompFirstSliceBlocker::kInvalidTransition:
    return "invalid transition";
  case LateCompFirstSliceBlocker::kInvalidLatePhase:
    return "invalid late phase";
  case LateCompFirstSliceBlocker::kInvalidCompositePlan:
    return "invalid composite plan";
  case LateCompFirstSliceBlocker::kTransitionIdentityMismatch:
    return "transition identity mismatch";
  case LateCompFirstSliceBlocker::kFirstSingleStream32Missing:
    return "first single-stream-32 draw missing";
  case LateCompFirstSliceBlocker::kFirstDrawIdentityMismatch:
    return "first draw identity mismatch";
  case LateCompFirstSliceBlocker::kMissingVertexProgramPort:
    return "missing 4761 vertex program port";
  case LateCompFirstSliceBlocker::kMissingResolvedSceneFetch:
    return "missing resolved MAIN scene fetch";
  case LateCompFirstSliceBlocker::kMissingPrivateCompTransaction:
    return "missing private COMP transaction";
  }
  return "unknown";
}

} // namespace tabletennis::native
