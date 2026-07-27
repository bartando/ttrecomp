#pragma once

#include "native/tabletennis_player_skin_snapshot.h"

#include <array>
#include <cstdint>
#include <memory>

namespace rex::graphics {
struct NativeGuestDrawContext;
struct NativeGuestDrawEligibilityContext;
}

namespace tabletennis::native {

enum class PlayerReplacementPhase : uint8_t {
  kDepthAlphaPrepass,
  kBlendedColor,
};

// One immutable, one-shot token for an exact observed CA9 submission. Tokens
// are diagnostics until the borrowed guest-depth and alpha-to-coverage
// contracts are solved and a renderer route is explicitly registered.
struct PlayerReplacementCandidate {
  uint64_t generation = 0;
  uint64_t phase_pair_generation = 0;
  uint64_t frame_sequence = 0;
  uint32_t draw_index = 0;
  uint32_t counterpart_draw_index = 0;
  PlayerReplacementPhase phase = PlayerReplacementPhase::kDepthAlphaPrepass;
  PlayerSkinMeshIdentity mesh{};
  uint64_t vertex_shader_hash = 0;
  uint64_t pixel_shader_hash = 0;
  uint32_t primitive_type = 0;
  uint32_t submitted_index_count = 0;
  uint32_t guest_index_base = 0;
  uint64_t palette_fingerprint = 0;
  uint64_t constants_fingerprint = 0;
  uint64_t material_fingerprint = 0;
  std::array<uint64_t, 3> texture_fingerprints{};
  std::shared_ptr<const PlayerSkinFrameSnapshot> frame;

  bool valid() const;
};

struct PlayerReplacementCandidateTelemetry {
  uint64_t published_frames = 0;
  uint64_t published_phase_pairs = 0;
  uint64_t published_candidates = 0;
  uint64_t rejected_frames = 0;
  uint64_t partial_phase_groups = 0;
  uint64_t ambiguous_phase_groups = 0;
  uint64_t phase_pair_mismatches = 0;
  uint64_t payload_pair_mismatches = 0;
  uint64_t backend_identity_collisions = 0;
  uint64_t match_probes = 0;
  uint64_t matched = 0;
  uint64_t consumed = 0;
  uint64_t abandoned_matches = 0;
  uint64_t unavailable_rejections = 0;
  uint64_t stale_rejections = 0;
  uint64_t consume_identity_rejections = 0;
  uint64_t prewarm_attempts = 0;
  uint64_t prewarm_succeeded = 0;
  uint64_t prewarm_failed = 0;
  uint64_t live_title_tokens_published = 0;
  uint64_t live_title_tokens_overwritten = 0;
  uint64_t backend_pre_gate_callbacks = 0;
  uint64_t backend_pre_gate_eligible = 0;
  uint64_t backend_pre_gate_missing_index = 0;
  uint64_t backend_pre_gate_shader_32bit_index = 0;
  uint64_t backend_pre_gate_memexport = 0;
  uint64_t backend_pre_gate_missing_rhi = 0;
  uint64_t backend_pre_gate_missing_replacer = 0;
  uint64_t backend_pre_gate_non_host_render_targets = 0;
  uint64_t backend_hash_callbacks = 0;
  uint64_t backend_route_matcher_visits = 0;
  uint64_t backend_early_callbacks = 0;
  uint64_t backend_late_callbacks = 0;
  uint64_t backend_contract_rejections = 0;
  uint64_t backend_pre_title_callbacks = 0;
  uint64_t backend_without_live_title_frame = 0;
  uint64_t backend_observer_probes = 0;
  uint64_t backend_observer_matched = 0;
  uint64_t backend_observer_mismatched = 0;
  uint64_t backend_observer_stale = 0;
  uint64_t backend_observer_ambiguous = 0;
  uint64_t backend_events_queued = 0;
  uint64_t backend_events_dropped = 0;
  uint64_t backend_tile_blocks_matched = 0;
  uint64_t backend_frames_verified = 0;
  uint64_t immutable_parity_frames = 0;
  uint64_t immutable_parity_matched = 0;
  uint64_t immutable_parity_mismatched = 0;
  uint64_t immutable_parity_stale = 0;
  uint64_t immutable_parity_ambiguous = 0;
  uint64_t latest_title_generation = 0;
  uint64_t latest_title_frame_sequence = 0;
  uint64_t latest_candidate_frame_sequence = 0;
  uint32_t live_candidates = 0;
  uint32_t live_unconsumed_candidates = 0;
  uint32_t live_title_tokens = 0;
  uint32_t live_title_frames = 0;
  uint32_t live_backend_events = 0;
};

// Publishes a value-only token synchronously from the verified title draw
// capture. All guest-backed payloads have already been copied through the
// fault-safe snapshot layer; no guest pointer escapes into this ledger.
void PublishPlayerReplacementLiveTitleDraw(
    uint64_t frame_sequence, const PlayerSkinDrawSnapshot &draw);

// Dispatcher-level observer tap for the asynchronously translated backend
// draw. It must run before route selection so an unrelated earlier route
// cannot hide a CA9 callback. The bounded ledger correlates the three ordered
// EDRAM tile blocks with earlier verified title tokens. This never selects or
// claims a replacement route.
void ObservePlayerReplacementBackendDraw(
    const rex::graphics::NativeGuestDrawContext &context);

// Default-off Vulkan pre-gate diagnostic. It proves whether exact CA9/77AF
// draws reach primitive processing and records every eligibility predicate
// before the backend decides whether to invoke the replacement dispatcher.
bool PlayerReplacementGateDiagnosticEnabled();
void ObservePlayerReplacementDrawEligibility(
    const rex::graphics::NativeGuestDrawEligibilityContext &context,
    void *user_data);

// Called only after the complete immutable player frame has been published.
// Partial or ambiguous mesh phase groups never produce candidates.
void PublishPlayerReplacementCandidates(
    const std::shared_ptr<const PlayerSkinFrameSnapshot> &frame);

// Dispatcher-compatible matcher. Matching consumes the observed generation
// immediately so a failed borrowed-scope acquisition cannot retarget a later
// coincidental draw. Only the non-claiming prewarm route calls this today.
bool MatchPlayerReplacementCandidate(
    const rex::graphics::NativeGuestDrawContext &context, void *user_data);

// Default-off diagnostic route. It selects the exact late CA9 candidate,
// preflights its borrowed PSO/descriptors, and always returns false so the
// guest draw remains authoritative.
bool PlayerReplacementPrewarmEnabled();
bool MatchPlayerReplacementPrewarm(
    const rex::graphics::NativeGuestDrawContext &context, void *user_data);
bool RenderPlayerReplacementPrewarm(
    const rex::graphics::NativeGuestDrawContext &context, void *user_data);

// A future renderer calls this before recording anything. Failure means the
// original guest draw must remain authoritative.
std::shared_ptr<const PlayerReplacementCandidate>
ConsumeMatchedPlayerReplacementCandidate(
    const rex::graphics::NativeGuestDrawContext &context);

PlayerReplacementCandidateTelemetry LatestPlayerReplacementCandidateTelemetry();
void ResetPlayerReplacementCandidates();

} // namespace tabletennis::native
