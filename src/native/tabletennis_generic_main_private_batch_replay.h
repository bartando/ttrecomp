#pragma once

#include <cstdint>

namespace rex::graphics {
struct NativeGuestOutputRenderContext;
}

namespace tabletennis::native {

enum class GenericMainPrivateBatchReplayStatus : uint8_t {
  kSucceeded = 0,
  kInvalidContext,
  kGuardedBatchQueryFailed,
  kNormalizationContractRejected,
  kAttachmentMismatch,
  kTargetPreparationFailed,
  kReplayRejected,
  kReadbackQueueFailed,
};

// One private replay of every MAIN family the backend proved this frame,
// whether or not that family was ever reverse engineered by hand.
//
// Coverage is reported rather than required: `covered_draw_count` against
// `observed_draw_count` is how much of the guarded MAIN pass this plan
// actually contains. A partial plan still records, because the point of the
// generic path is to measure and shrink the remainder. Nothing here suppresses
// or presents guest rendering.
struct GenericMainPrivateBatchReplayResult {
  GenericMainPrivateBatchReplayStatus status =
      GenericMainPrivateBatchReplayStatus::kInvalidContext;
  uint64_t backend_frame_sequence = 0;
  uint32_t discovered_family_count = 0;
  uint32_t proven_family_count = 0;
  uint32_t observed_draw_count = 0;
  uint32_t covered_draw_count = 0;
  uint32_t recorded_draw_count = 0;
  // The backend's own verdict when `status` is `kReplayRejected`, so a
  // rejection names the failing contract instead of only the stage.
  const char *replay_result_name = "";

  bool succeeded() const {
    return status == GenericMainPrivateBatchReplayStatus::kSucceeded;
  }
};

// `queue_readback` requests the private diagnostic resolve/readback. It costs
// a GPU copy, so callers ask for it on the frames they intend to inspect
// rather than on every frame they replay.
GenericMainPrivateBatchReplayResult
ReplayCurrentFrameGenericMainPrivateBatch(
    const rex::graphics::NativeGuestOutputRenderContext &context,
    bool queue_readback);

const char *GenericMainPrivateBatchReplayStatusName(
    GenericMainPrivateBatchReplayStatus status);

// Writes one bounded per-family census for the current frame: shader pair,
// observed draws, replayed draws, and the reject mask for families that
// contributed nothing. This is the triage input for deciding which few draws
// still need bespoke handling.
//
// Emitted the first time gameplay families are proven and again whenever the
// discovered family set grows, so the remainder stays visible as coverage
// changes without logging every frame.
void LogGenericMainFamilyCensusOnChange(
    const rex::graphics::NativeGuestOutputRenderContext &context);

} // namespace tabletennis::native
