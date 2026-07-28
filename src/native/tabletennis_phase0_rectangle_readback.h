#pragma once

#include "native/tabletennis_exact_main_targets.h"

#include <array>
#include <cstdint>

namespace rex::graphics {
struct NativeGuestOutputRenderContext;
}

namespace tabletennis::native {

enum class PrivateReplayDiagnosticKind : uint8_t {
  kRectangle = 0,
  kPs328,
  kPs328Batch,
  kGuardedVenueBatch,
};

struct Phase0RectangleReadbackResult {
  PrivateReplayDiagnosticKind kind = PrivateReplayDiagnosticKind::kRectangle;
  uint64_t backend_frame_sequence = 0;
  uint64_t pixel_hash = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t nonzero_pixel_count = 0;
  std::array<uint32_t, 4> nonzero_bounds{};
  std::array<uint8_t, 4> first_nonzero_rgba{};
  bool valid = false;

  bool nonempty() const { return valid && nonzero_pixel_count != 0; }
};

void PollPhase0RectangleReadback(
    const rex::graphics::NativeGuestOutputRenderContext &context);
bool QueuePhase0RectangleReadback(
    const rex::graphics::NativeGuestOutputRenderContext &context,
    const ExactMainPreparedTargets &targets, uint64_t backend_frame_sequence,
    PrivateReplayDiagnosticKind kind = PrivateReplayDiagnosticKind::kRectangle);

Phase0RectangleReadbackResult LatestPhase0RectangleReadbackResult();
void ShutdownPhase0RectangleReadback();

} // namespace tabletennis::native
