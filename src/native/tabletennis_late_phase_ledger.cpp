#include "native/tabletennis_late_phase_ledger.h"

#include "native/tabletennis_hud_swf_backend_observer.h"
#include "native/tabletennis_player_2ac_observer.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <ranges>
#include <utility>
#include <vector>

#include <rex/cvar.h>
#include <rex/graphics/native_guest_renderer.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(
    tabletennis_native_late_phase_ledger, false, "Table Tennis",
    "Observe the complete backend draw order from the MAIN-to-COMP marker "
    "through 2AC, post, gameplay HUD and the final compositor. Observer-only; "
    "never records, replaces, serves or suppresses a draw.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_UINT32(tabletennis_native_late_phase_ledger_log_interval, 120,
                      "Table Tennis",
                      "Published late-phase frames between observer reports.")
    .range(1, 100000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace tabletennis::native {
namespace {

constexpr uint64_t kMainToCompVertexShader = 0xFA14ACFDF2DE3ED0ull;
constexpr uint64_t kMainToCompPixelShader = 0x5E11FC7AE2F1C2BFull;
constexpr uint64_t kHudVertexShader = 0xF0B85512865B6E5Eull;
constexpr uint64_t kHudPixelShader = 0x391847433E1601A9ull;
constexpr uint64_t kFinalVertexShader = 0x648EED9302E5A28Cull;
constexpr uint64_t kFinalPixelShader = 0x300BD18022137D26ull;
constexpr uint32_t kFullscreenPrimitive = 8;
constexpr uint32_t kQuadListPrimitive = 13;
constexpr uint32_t kMaximumEventsPerFrame = 4096;
constexpr size_t kMaximumRetainedFrames = 8;
constexpr size_t kMaximumTransitionSetupDraws = 16;
constexpr size_t kMaximumDiagnosticPredecessors = 4;

struct BackendFrame {
  uint64_t sequence = 0;
  uint32_t dropped_event_count = 0;
  std::vector<LatePhaseDrawIdentity> events;
};

std::mutex g_mutex;
std::deque<BackendFrame> g_frames;
std::shared_ptr<const LatePhaseFrameSnapshot> g_published;
LatePhaseLedgerTelemetry g_telemetry;
bool g_announced_complete = false;
bool g_announced_rejection = false;

bool IsMainToComp(const LatePhaseDrawIdentity &draw) {
  return draw.vertex_shader_hash == kMainToCompVertexShader &&
         draw.pixel_shader_hash == kMainToCompPixelShader &&
         draw.primitive_type == kFullscreenPrimitive &&
         draw.submitted_count == 3;
}

bool IsFinalCompositor(const LatePhaseDrawIdentity &draw) {
  return draw.vertex_shader_hash == kFinalVertexShader &&
         draw.pixel_shader_hash == kFinalPixelShader &&
         draw.primitive_type == kFullscreenPrimitive &&
         draw.submitted_count == 3;
}

bool IsHudPrefix(const LatePhaseDrawIdentity &draw) {
  return draw.vertex_shader_hash == kHudVertexShader &&
         draw.pixel_shader_hash == kHudPixelShader &&
         draw.primitive_type == kQuadListPrimitive &&
         draw.submitted_count == 88;
}

bool Matches2AC(const LatePhaseDrawIdentity &event,
                const Player2ACDrawProof &proof) {
  return proof.valid() && event.valid &&
         event.vertex_shader_hash == proof.backend.vertex_shader_hash &&
         event.pixel_shader_hash == proof.backend.pixel_shader_hash &&
         event.primitive_type == proof.title.identity.primitive_type &&
         event.submitted_count == proof.title.identity.submitted_index_count &&
         event.guest_index_base == proof.title.identity.guest_index_base;
}

bool MatchesHud(const LatePhaseDrawIdentity &event,
                const HudSwfBackendDrawProof &proof) {
  const HudSwfBackendEligibilityContract &expected = proof.eligibility;
  return proof.valid && event.valid && expected.valid &&
         event.backend_frame_sequence == expected.backend_frame_sequence &&
         event.vertex_shader_hash == expected.vertex_shader_hash &&
         event.pixel_shader_hash == expected.pixel_shader_hash &&
         event.primitive_type == expected.primitive_type &&
         event.submitted_count == expected.submitted_vertex_count &&
         event.host_count == expected.host_vertex_count &&
         event.processed_index_buffer_type ==
             expected.processed_index_buffer_type &&
         event.processed_index_buffer_present ==
             expected.processed_index_buffer_present &&
         event.shader_32bit_index_dma == expected.shader_32bit_index_dma &&
         event.memexport_writes_possible ==
             expected.memexport_writes_possible &&
         event.host_render_targets == expected.host_render_targets;
}

template <typename Proof, typename Match>
std::optional<std::vector<size_t>>
FindEarliestOrdered(const std::vector<LatePhaseDrawIdentity> &events,
                    const std::vector<Proof> &proofs, Match matches) {
  std::vector<size_t> result;
  result.reserve(proofs.size());
  size_t event_index = 0;
  for (const Proof &proof : proofs) {
    while (event_index < events.size() &&
           !matches(events[event_index], proof)) {
      ++event_index;
    }
    if (event_index == events.size()) {
      return std::nullopt;
    }
    result.push_back(event_index++);
  }
  return result;
}

template <typename Proof, typename Match>
std::optional<std::vector<size_t>>
FindLatestOrdered(const std::vector<LatePhaseDrawIdentity> &events,
                  const std::vector<Proof> &proofs, Match matches) {
  std::vector<size_t> result(proofs.size());
  size_t event_end = events.size();
  for (size_t proof_end = proofs.size(); proof_end != 0; --proof_end) {
    while (event_end != 0 &&
           !matches(events[event_end - 1], proofs[proof_end - 1])) {
      --event_end;
    }
    if (event_end == 0) {
      return std::nullopt;
    }
    result[proof_end - 1] = --event_end;
  }
  return result;
}

BackendFrame *FindOrCreateFrame(uint64_t sequence) {
  const auto found =
      std::ranges::find(g_frames, sequence, &BackendFrame::sequence);
  if (found != g_frames.end()) {
    return &*found;
  }
  if (g_frames.size() == kMaximumRetainedFrames) {
    g_telemetry.events_dropped +=
        g_frames.front().events.size() + g_frames.front().dropped_event_count;
    g_frames.pop_front();
  }
  g_frames.push_back({.sequence = sequence});
  return &g_frames.back();
}

LatePhaseFrameSnapshot
Analyze(const BackendFrame &frame,
        std::shared_ptr<const Player2ACFrameSnapshot> player,
        std::shared_ptr<const HudSwfBackendFrameSnapshot> hud) {
  LatePhaseFrameSnapshot snapshot;
  snapshot.sequence = frame.sequence;
  snapshot.backend_frame_sequence = frame.sequence;
  snapshot.backend_frame_draw_count =
      static_cast<uint32_t>(frame.events.size());
  snapshot.dropped_event_count = frame.dropped_event_count;
  snapshot.player_2ac = std::move(player);
  snapshot.hud = std::move(hud);
  if (frame.dropped_event_count != 0) {
    snapshot.reject_reason = LatePhaseRejectReason::kDroppedBackendEvents;
    return snapshot;
  }
  if (snapshot.player_2ac == nullptr || !snapshot.player_2ac->valid() ||
      snapshot.player_2ac->sequence != frame.sequence ||
      snapshot.hud == nullptr || !snapshot.hud->observer_complete() ||
      snapshot.hud->sequence != frame.sequence || frame.events.empty()) {
    return snapshot;
  }

  const auto earliest_hud =
      FindEarliestOrdered(frame.events, snapshot.hud->draws, MatchesHud);
  const auto latest_hud =
      FindLatestOrdered(frame.events, snapshot.hud->draws, MatchesHud);
  if (!earliest_hud || !latest_hud) {
    snapshot.reject_reason = LatePhaseRejectReason::kHudJoinMissing;
    return snapshot;
  }
  if (*earliest_hud != *latest_hud) {
    snapshot.ambiguous_layout_count = 1;
    snapshot.reject_reason = LatePhaseRejectReason::kHudJoinAmbiguous;
    return snapshot;
  }
  const std::vector<size_t> &hud_mapping = *earliest_hud;
  if (hud_mapping.empty()) {
    return snapshot;
  }
  for (size_t index = 1; index < hud_mapping.size(); ++index) {
    if (hud_mapping[index] != hud_mapping.front() + index) {
      snapshot.reject_reason = LatePhaseRejectReason::kHudNotContiguous;
      return snapshot;
    }
  }
  snapshot.unique_hud_join = true;
  const size_t hud_begin = hud_mapping.front();
  const size_t hud_end = hud_mapping.back() + 1;
  const size_t hud_successor_end =
      std::min(frame.events.size(), hud_end + kMaximumDiagnosticPredecessors);
  snapshot.hud_successors.assign(
      frame.events.begin() + static_cast<std::ptrdiff_t>(hud_end),
      frame.events.begin() + static_cast<std::ptrdiff_t>(hud_successor_end));

  // Join the title-side 2AC proof independently of the transition marker.
  // The old adjacency test made a changed or intervening transition setup
  // draw indistinguishable from a failed 2AC join.
  const auto earliest_player =
      FindEarliestOrdered(frame.events, snapshot.player_2ac->draws, Matches2AC);
  const auto latest_player =
      FindLatestOrdered(frame.events, snapshot.player_2ac->draws, Matches2AC);
  if (!earliest_player || !latest_player) {
    snapshot.reject_reason = LatePhaseRejectReason::kPlayer2ACJoinMissing;
    return snapshot;
  }
  if (*earliest_player != *latest_player) {
    snapshot.ambiguous_layout_count = 1;
    snapshot.reject_reason = LatePhaseRejectReason::kPlayer2ACJoinAmbiguous;
    return snapshot;
  }
  const std::vector<size_t> &player_mapping = *earliest_player;
  if (player_mapping.empty() || player_mapping.back() >= hud_begin) {
    snapshot.reject_reason = LatePhaseRejectReason::kPlayer2ACJoinMissing;
    return snapshot;
  }
  for (size_t index = 1; index < player_mapping.size(); ++index) {
    if (player_mapping[index] != player_mapping.front() + index) {
      snapshot.reject_reason = LatePhaseRejectReason::kPlayer2ACNotContiguous;
      return snapshot;
    }
  }
  const size_t player_begin = player_mapping.front();
  const size_t player_end = player_mapping.back() + 1;
  snapshot.player_2ac_first_backend_index = static_cast<uint32_t>(player_begin);
  snapshot.player_2ac_last_backend_index =
      static_cast<uint32_t>(player_mapping.back());
  const size_t predecessor_begin =
      player_begin > kMaximumDiagnosticPredecessors
          ? player_begin - kMaximumDiagnosticPredecessors
          : 0;
  snapshot.player_2ac_predecessors.assign(
      frame.events.begin() + static_cast<std::ptrdiff_t>(predecessor_begin),
      frame.events.begin() + static_cast<std::ptrdiff_t>(player_begin));

  std::vector<size_t> transition_candidates;
  for (size_t transition = 0; transition < player_begin; ++transition) {
    if (IsMainToComp(frame.events[transition])) {
      transition_candidates.push_back(transition);
    }
  }
  snapshot.transition_candidate_count =
      static_cast<uint32_t>(transition_candidates.size());
  if (transition_candidates.size() != 1) {
    snapshot.ambiguous_layout_count = transition_candidates.size() > 1;
    snapshot.reject_reason =
        transition_candidates.empty()
            ? LatePhaseRejectReason::kTransitionJoinMissing
            : LatePhaseRejectReason::kTransitionJoinAmbiguous;
    return snapshot;
  }

  const size_t transition = transition_candidates.front();
  const size_t transition_setup_count = player_begin - transition - 1;
  snapshot.transition_setup_count =
      static_cast<uint32_t>(transition_setup_count);
  if (transition_setup_count > kMaximumTransitionSetupDraws) {
    snapshot.reject_reason = LatePhaseRejectReason::kTransitionSetupTooLarge;
    return snapshot;
  }
  snapshot.unique_player_2ac_join = true;
  if (player_end >= hud_begin || !IsHudPrefix(frame.events[player_end])) {
    snapshot.reject_reason = LatePhaseRejectReason::kPostPrefixMissing;
    return snapshot;
  }
  const uint32_t hud_prefix_count = static_cast<uint32_t>(std::ranges::count_if(
      frame.events.begin() + static_cast<std::ptrdiff_t>(player_end),
      frame.events.begin() + static_cast<std::ptrdiff_t>(hud_begin),
      IsHudPrefix));
  if (hud_prefix_count != 1) {
    snapshot.ambiguous_layout_count = hud_prefix_count > 1;
    snapshot.reject_reason = hud_prefix_count == 0
                                 ? LatePhaseRejectReason::kPostPrefixMissing
                                 : LatePhaseRejectReason::kPostPrefixAmbiguous;
    return snapshot;
  }

  std::vector<size_t> final_candidates;
  for (size_t index = hud_end; index < frame.events.size(); ++index) {
    if (IsFinalCompositor(frame.events[index])) {
      final_candidates.push_back(index);
    }
  }
  snapshot.final_compositor_candidate_count =
      static_cast<uint32_t>(final_candidates.size());
  if (final_candidates.size() > 1) {
    snapshot.ambiguous_layout_count = 1;
    snapshot.reject_reason = LatePhaseRejectReason::kFinalCompositorAmbiguous;
    return snapshot;
  }
  if (!final_candidates.empty()) {
    if (final_candidates.front() != hud_end) {
      snapshot.reject_reason =
          LatePhaseRejectReason::kFinalCompositorNotImmediate;
      return snapshot;
    }
    snapshot.final_compositor_present = true;
    snapshot.final_compositor_index =
        static_cast<uint32_t>(final_candidates.front() - transition);
  }
  // Live validation proves the reference compositor is conditional: when it
  // disappears, the backend frame loses exactly one draw while the joined HUD
  // end and the 24-draw tail remain unchanged. An absent exact identity is
  // therefore a valid terminal form; no tail draw is promoted by ordinal.
  const size_t late_window_end =
      snapshot.final_compositor_present ? hud_end + 1 : hud_end;

  snapshot.draws_before_transition = static_cast<uint32_t>(transition);
  snapshot.transition_draw_index = 0;
  snapshot.player_2ac_begin = static_cast<uint32_t>(1 + transition_setup_count);
  snapshot.player_2ac_count =
      static_cast<uint32_t>(snapshot.player_2ac->draws.size());
  snapshot.post_begin = static_cast<uint32_t>(player_end - transition);
  snapshot.post_count = static_cast<uint32_t>(hud_begin - player_end);
  snapshot.hud_begin = static_cast<uint32_t>(hud_begin - transition);
  snapshot.hud_count = static_cast<uint32_t>(hud_end - hud_begin);
  snapshot.draws_after_late_window =
      static_cast<uint32_t>(frame.events.size() - late_window_end);
  snapshot.draws.assign(
      frame.events.begin() + static_cast<std::ptrdiff_t>(transition),
      frame.events.begin() + static_cast<std::ptrdiff_t>(late_window_end));
  for (size_t index = 0; index < snapshot.draws.size(); ++index) {
    LatePhaseDrawKind kind = LatePhaseDrawKind::kPost;
    if (index == snapshot.transition_draw_index) {
      kind = LatePhaseDrawKind::kMainToCompTransition;
    } else if (index < snapshot.player_2ac_begin) {
      kind = LatePhaseDrawKind::kTransitionSetup;
    } else if (index >= snapshot.player_2ac_begin &&
               index < snapshot.player_2ac_begin + snapshot.player_2ac_count) {
      kind = LatePhaseDrawKind::kPlayer2AC;
    } else if (index >= snapshot.hud_begin &&
               index < snapshot.hud_begin + snapshot.hud_count) {
      kind = LatePhaseDrawKind::kGameplayHud;
    } else if (snapshot.final_compositor_present &&
               index == snapshot.final_compositor_index) {
      kind = LatePhaseDrawKind::kFinalCompositor;
    }
    snapshot.draws[index].kind = kind;
  }
  snapshot.reference_anchors_valid = true;
  snapshot.reject_reason = LatePhaseRejectReason::kNone;
  return snapshot;
}

void LogSnapshot(const LatePhaseFrameSnapshot &snapshot) {
  REXLOG_INFO(
      "Table Tennis late-phase ledger: frame={} backend_frame={} "
      "backend_draws={} before_transition={} window={} "
      "phases[transition={} setup={} 2ac={}/{} post={}/{} hud={}/{} "
      "final={}/{}] "
      "evidence[transition_candidates={} 2ac_backend={}/{} predecessors={} "
      "final_candidates={} hud_successors={}] "
      "after_window={} joins[2ac={} hud={}] anchors={} reject={} "
      "ambiguous={} dropped={} "
      "observer_complete={} replay_ready=false observer_only=true "
      "guest_suppressed=false",
      snapshot.sequence, snapshot.backend_frame_sequence,
      snapshot.backend_frame_draw_count, snapshot.draws_before_transition,
      snapshot.draws.size(), snapshot.transition_draw_index,
      snapshot.transition_setup_count, snapshot.player_2ac_begin,
      snapshot.player_2ac_count, snapshot.post_begin, snapshot.post_count,
      snapshot.hud_begin, snapshot.hud_count, snapshot.final_compositor_index,
      snapshot.final_compositor_present, snapshot.transition_candidate_count,
      snapshot.player_2ac_first_backend_index,
      snapshot.player_2ac_last_backend_index,
      snapshot.player_2ac_predecessors.size(),
      snapshot.final_compositor_candidate_count, snapshot.hud_successors.size(),
      snapshot.draws_after_late_window, snapshot.unique_player_2ac_join,
      snapshot.unique_hud_join, snapshot.reference_anchors_valid,
      LatePhaseRejectReasonName(snapshot.reject_reason),
      snapshot.ambiguous_layout_count, snapshot.dropped_event_count,
      snapshot.observer_complete());
  for (const LatePhaseDrawIdentity &draw : snapshot.player_2ac_predecessors) {
    REXLOG_INFO("Table Tennis late-phase ledger 2AC predecessor: frame={} "
                "backend_index={} vs={:016X} ps={:016X} primitive={} "
                "submitted={} host={} valid={}",
                snapshot.sequence, draw.backend_draw_index,
                draw.vertex_shader_hash, draw.pixel_shader_hash,
                draw.primitive_type, draw.submitted_count, draw.host_count,
                draw.valid);
  }
  for (const LatePhaseDrawIdentity &draw : snapshot.hud_successors) {
    REXLOG_INFO("Table Tennis late-phase ledger HUD successor: frame={} "
                "backend_index={} vs={:016X} ps={:016X} primitive={} "
                "submitted={} host={} valid={} reference_final={}",
                snapshot.sequence, draw.backend_draw_index,
                draw.vertex_shader_hash, draw.pixel_shader_hash,
                draw.primitive_type, draw.submitted_count, draw.host_count,
                draw.valid, IsFinalCompositor(draw));
  }
}

} // namespace

bool LatePhaseFrameSnapshot::observer_complete() const {
  return sequence != 0 && backend_frame_sequence == sequence &&
         player_2ac != nullptr && player_2ac->valid() &&
         player_2ac->sequence == sequence && hud != nullptr &&
         hud->observer_complete() && hud->sequence == sequence &&
         unique_player_2ac_join && unique_hud_join && reference_anchors_valid &&
         reject_reason == LatePhaseRejectReason::kNone &&
         dropped_event_count == 0 && ambiguous_layout_count == 0 &&
         !draws.empty() && transition_draw_index == 0 &&
         player_2ac_begin == 1 + transition_setup_count &&
         player_2ac_count == player_2ac->draws.size() &&
         post_begin == player_2ac_begin + player_2ac_count && post_count != 0 &&
         hud_begin == post_begin + post_count &&
         hud_count == hud->draws.size() &&
         ((final_compositor_present &&
           final_compositor_index == hud_begin + hud_count &&
           final_compositor_index + 1 == draws.size()) ||
          (!final_compositor_present && final_compositor_candidate_count == 0 &&
           final_compositor_index == UINT32_MAX &&
           hud_begin + hud_count == draws.size()));
}

bool LatePhaseLedgerEnabled() {
  return REXCVAR_GET(tabletennis_native_late_phase_ledger);
}

void ObserveLatePhaseDrawEligibility(
    const rex::graphics::NativeGuestDrawEligibilityContext &context) {
  if (!LatePhaseLedgerEnabled()) {
    return;
  }
  std::lock_guard lock(g_mutex);
  ++g_telemetry.callbacks;
  g_telemetry.latest_backend_sequence = std::max(
      g_telemetry.latest_backend_sequence, context.backend_frame_sequence);
  if (context.backend_frame_sequence == 0) {
    ++g_telemetry.events_dropped;
    return;
  }
  BackendFrame *frame = FindOrCreateFrame(context.backend_frame_sequence);
  if (frame->events.size() == kMaximumEventsPerFrame) {
    ++frame->dropped_event_count;
    ++g_telemetry.events_dropped;
    return;
  }
  frame->events.push_back({
      .backend_frame_sequence = context.backend_frame_sequence,
      .vertex_shader_hash = context.vertex_shader_hash,
      .pixel_shader_hash = context.pixel_shader_hash,
      .backend_draw_index = static_cast<uint32_t>(frame->events.size()),
      .primitive_type = context.primitive_type,
      .submitted_count = context.guest_vertex_or_index_count,
      .host_count = context.vertex_or_index_count,
      .guest_index_base = context.guest_index_base,
      .processed_index_buffer_type = context.processed_index_buffer_type,
      .kind = LatePhaseDrawKind::kPost,
      .processed_index_buffer_present = context.processed_index_buffer_present,
      .shader_32bit_index_dma = context.shader_32bit_index_dma,
      .memexport_writes_possible = context.memexport_writes_possible,
      .host_render_targets = context.host_render_targets,
      .valid =
          context.backend == rex::graphics::NativeGuestOutputBackend::kVulkan &&
          context.vertex_shader_hash != 0 && context.pixel_shader_hash != 0 &&
          context.primitive_type != 0 &&
          context.guest_vertex_or_index_count != 0 &&
          context.vertex_or_index_count != 0 &&
          !context.memexport_writes_possible && context.host_render_targets,
  });
  ++g_telemetry.events;
  g_telemetry.retained_backend_frames = static_cast<uint32_t>(g_frames.size());
}

void LatePhaseLedgerFrameEnd() {
  std::lock_guard lock(g_mutex);
  if (!LatePhaseLedgerEnabled()) {
    g_frames.clear();
    g_published.reset();
    g_telemetry = {};
    g_announced_complete = false;
    g_announced_rejection = false;
    return;
  }
  const std::shared_ptr<const Player2ACFrameSnapshot> player =
      LatestPlayer2ACFrameSnapshot();
  const std::shared_ptr<const HudSwfBackendFrameSnapshot> hud =
      LatestHudSwfBackendFrameSnapshot();
  if (player == nullptr || hud == nullptr ||
      player->sequence != hud->sequence) {
    return;
  }
  const auto frame =
      std::ranges::find(g_frames, player->sequence, &BackendFrame::sequence);
  if (frame == g_frames.end()) {
    return;
  }

  auto snapshot =
      std::make_shared<LatePhaseFrameSnapshot>(Analyze(*frame, player, hud));
  g_published = snapshot;
  ++g_telemetry.analyzed_frames;
  g_telemetry.latest_published_sequence = snapshot->sequence;
  if (snapshot->observer_complete()) {
    ++g_telemetry.complete_frames;
  } else {
    ++g_telemetry.rejected_frames;
  }
  const uint32_t interval =
      REXCVAR_GET(tabletennis_native_late_phase_ledger_log_interval);
  const bool first_complete =
      snapshot->observer_complete() && !g_announced_complete;
  const bool first_rejection =
      !snapshot->observer_complete() && !g_announced_rejection;
  if (first_complete || first_rejection || snapshot->sequence % interval == 0) {
    g_announced_complete |= snapshot->observer_complete();
    g_announced_rejection |= !snapshot->observer_complete();
    LogSnapshot(*snapshot);
  }

  g_frames.erase(g_frames.begin(), std::next(frame));
  g_telemetry.retained_backend_frames = static_cast<uint32_t>(g_frames.size());
}

std::shared_ptr<const LatePhaseFrameSnapshot> LatestLatePhaseFrameSnapshot() {
  std::lock_guard lock(g_mutex);
  return g_published;
}

LatePhaseLedgerTelemetry LatestLatePhaseLedgerTelemetry() {
  std::lock_guard lock(g_mutex);
  LatePhaseLedgerTelemetry telemetry = g_telemetry;
  telemetry.retained_backend_frames = static_cast<uint32_t>(g_frames.size());
  telemetry.replay_ready = false;
  return telemetry;
}

const char *LatePhaseRejectReasonName(LatePhaseRejectReason reason) {
  switch (reason) {
  case LatePhaseRejectReason::kNone:
    return "none";
  case LatePhaseRejectReason::kMissingDependencyProof:
    return "missing_dependency_proof";
  case LatePhaseRejectReason::kDroppedBackendEvents:
    return "dropped_backend_events";
  case LatePhaseRejectReason::kHudJoinMissing:
    return "hud_join_missing";
  case LatePhaseRejectReason::kHudJoinAmbiguous:
    return "hud_join_ambiguous";
  case LatePhaseRejectReason::kHudNotContiguous:
    return "hud_not_contiguous";
  case LatePhaseRejectReason::kPlayer2ACJoinMissing:
    return "player_2ac_join_missing";
  case LatePhaseRejectReason::kPlayer2ACJoinAmbiguous:
    return "player_2ac_join_ambiguous";
  case LatePhaseRejectReason::kPlayer2ACNotContiguous:
    return "player_2ac_not_contiguous";
  case LatePhaseRejectReason::kTransitionJoinMissing:
    return "transition_join_missing";
  case LatePhaseRejectReason::kTransitionJoinAmbiguous:
    return "transition_join_ambiguous";
  case LatePhaseRejectReason::kTransitionSetupTooLarge:
    return "transition_setup_too_large";
  case LatePhaseRejectReason::kPostPrefixMissing:
    return "post_prefix_missing";
  case LatePhaseRejectReason::kPostPrefixAmbiguous:
    return "post_prefix_ambiguous";
  case LatePhaseRejectReason::kFinalCompositorMissing:
    return "final_compositor_missing";
  case LatePhaseRejectReason::kFinalCompositorAmbiguous:
    return "final_compositor_ambiguous";
  case LatePhaseRejectReason::kFinalCompositorNotImmediate:
    return "final_compositor_not_immediate";
  }
  return "unknown";
}

} // namespace tabletennis::native
