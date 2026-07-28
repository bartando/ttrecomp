#pragma once

#include <cstdint>

namespace rex::graphics {
struct NativeGuestOutputRenderContext;
}

namespace tabletennis::native {

enum class GuardedVenuePrivateBatchReplayStatus : uint8_t {
  kSucceeded = 0,
  kInvalidContext,
  kGuardedBatchQueryFailed,
  kNormalizationContractRejected,
  kAttachmentMismatch,
  kTargetPreparationFailed,
  kReplayRejected,
  kReadbackQueueFailed,
};

struct GuardedVenuePrivateBatchReplayResult {
  GuardedVenuePrivateBatchReplayStatus status =
      GuardedVenuePrivateBatchReplayStatus::kInvalidContext;
  uint64_t backend_frame_sequence = 0;
  uint32_t ps328_draw_count = 0;
  uint32_t venue_9e_draw_count = 0;
  uint32_t crowd_c6_draw_count = 0;
  uint32_t venue_14d_draw_count = 0;
  uint32_t recorded_draw_count = 0;

  bool succeeded() const {
    return status == GuardedVenuePrivateBatchReplayStatus::kSucceeded;
  }
};

GuardedVenuePrivateBatchReplayResult
ReplayCurrentFrameGuardedVenuePrivateBatch(
    const rex::graphics::NativeGuestOutputRenderContext &context);

const char *GuardedVenuePrivateBatchReplayStatusName(
    GuardedVenuePrivateBatchReplayStatus status);

} // namespace tabletennis::native
