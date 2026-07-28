#pragma once

#include "native/tabletennis_exact_main_targets.h"
#include "native/tabletennis_main_coverage_ledger.h"
#include "native/tabletennis_translated_shader_artifact_store.h"

#include <cstdint>
#include <memory>

namespace rex::graphics {
struct NativeGuestDrawContext;
struct NativeGuestOutputRenderContext;
struct NativeGuestTranslatedReplayTokenContext;
} // namespace rex::graphics

namespace tabletennis::native {

enum class Phase0RectangleReplayMissing : uint32_t {
  kNone = 0,
  kCapturedPayload = 1u << 0,
  kCurrentFrameToken = 1u << 1,
  kTranslatedVertexArtifact = 1u << 2,
  kTranslatedPixelArtifact = 1u << 3,
  kExactMainTargetContract = 1u << 4,
  kPrivateTargetsPrepared = 1u << 5,
};

constexpr Phase0RectangleReplayMissing
operator|(Phase0RectangleReplayMissing lhs,
          Phase0RectangleReplayMissing rhs) {
  return static_cast<Phase0RectangleReplayMissing>(
      static_cast<uint32_t>(lhs) | static_cast<uint32_t>(rhs));
}

// Immutable observer-side join for the first translated replay tracer bullet.
// The opaque token remains backend-owned and current-frame-only. Private
// target preparation is retained as evidence only; no attachment pointer
// escapes the output callback that validated it.
struct Phase0RectangleReplaySnapshot {
  uint64_t backend_frame_sequence = 0;
  MainCoverageVertexColorRectangleProof payload{};
  std::shared_ptr<
      const rex::graphics::NativeGuestTranslatedReplayTokenContext>
      replay_token;
  std::shared_ptr<const TranslatedShaderArtifact> vertex_shader;
  std::shared_ptr<const TranslatedShaderArtifact> pixel_shader;
  ExactMainTargetContract target_contract{};
  bool private_targets_prepared_in_output_context = false;
  Phase0RectangleReplayMissing missing =
      Phase0RectangleReplayMissing::kCapturedPayload |
      Phase0RectangleReplayMissing::kCurrentFrameToken |
      Phase0RectangleReplayMissing::kTranslatedVertexArtifact |
      Phase0RectangleReplayMissing::kTranslatedPixelArtifact |
      Phase0RectangleReplayMissing::kExactMainTargetContract |
      Phase0RectangleReplayMissing::kPrivateTargetsPrepared;

  bool valid() const;
  // All title-side observer inputs are joined. This is deliberately not an
  // executable replay promise: the current backend token identifies mutable
  // descriptor/ring-buffer resources that cannot be retained for deferred
  // submission.
  bool observer_inputs_ready() const {
    return valid() && missing == Phase0RectangleReplayMissing::kNone;
  }
  bool deferred_replay_safe() const;
  bool replay_ready() const {
    // Backend execution does not dereference the title-owned payload copy.
    // The guarded translated pipeline can therefore be exercised even when
    // the selective draw callback didn't expose the rectangle bytes in this
    // frame. Payload parity remains a separate promotion/readback gate.
    constexpr uint32_t kExecutionRequired =
        static_cast<uint32_t>(Phase0RectangleReplayMissing::kCurrentFrameToken) |
        static_cast<uint32_t>(
            Phase0RectangleReplayMissing::kTranslatedVertexArtifact) |
        static_cast<uint32_t>(
            Phase0RectangleReplayMissing::kTranslatedPixelArtifact) |
        static_cast<uint32_t>(
            Phase0RectangleReplayMissing::kExactMainTargetContract) |
        static_cast<uint32_t>(
            Phase0RectangleReplayMissing::kPrivateTargetsPrepared);
    return valid() &&
           (static_cast<uint32_t>(missing) & kExecutionRequired) == 0 &&
           deferred_replay_safe();
  }
};

// Called from the existing draw observer path. It copies the exact 84-byte
// rectangle payload and never requests replacement.
void ObservePhase0RectangleDraw(
    const rex::graphics::NativeGuestDrawContext &context);

// Called by the translated-token observer after backend state is complete.
// It joins only the same backend frame and exact shader modifications.
void ObservePhase0RectangleReplayToken(
    const rex::graphics::NativeGuestTranslatedReplayTokenContext &context);

// Optional target priming entry point for the future backend replay call.
// Allocation is private and preparation-only: no pass, draw, resolve,
// suppression, guest-output write, or presentation is performed.
ExactMainTargetsResult PreparePhase0RectanglePrivateTargets(
    const rex::graphics::NativeGuestOutputRenderContext &context,
    ExactMainPreparedTargets &targets_out);

std::shared_ptr<const Phase0RectangleReplaySnapshot>
LatestPhase0RectangleReplaySnapshot();
void ShutdownPhase0RectangleReplay();

} // namespace tabletennis::native
