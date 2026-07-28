#pragma once

#include <cstdint>

namespace tabletennis::native {

enum class GuardedVenueTitleCaptureFamily : uint8_t {
  kPs328 = 0,
  kVenue9E,
};

// One-way, family-local title capture latches. They never alter the RexGlue
// guarded token filter/query path.
bool RetireGuardedVenueTitleCapture(
    GuardedVenueTitleCaptureFamily family, uint64_t proof_frame);
bool GuardedVenueTitleCaptureRetired(
    GuardedVenueTitleCaptureFamily family);
void ResetGuardedVenueTitleCaptureRetirement();

} // namespace tabletennis::native
