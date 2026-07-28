#include "native/tabletennis_guarded_venue_capture_retirement.h"

#include <array>
#include <atomic>
#include <cstddef>

#include <rex/logging.h>

namespace tabletennis::native {
namespace {

constexpr size_t kFamilyCount = 2;
std::array<std::atomic<bool>, kFamilyCount> g_retired{};

size_t FamilyIndex(GuardedVenueTitleCaptureFamily family) {
  return static_cast<size_t>(family);
}

const char *FamilyName(GuardedVenueTitleCaptureFamily family) {
  switch (family) {
  case GuardedVenueTitleCaptureFamily::kPs328:
    return "PS328";
  case GuardedVenueTitleCaptureFamily::kVenue9E:
    return "Venue9E";
  }
  return "unknown";
}

} // namespace

bool RetireGuardedVenueTitleCapture(
    GuardedVenueTitleCaptureFamily family, uint64_t proof_frame) {
  const size_t index = FamilyIndex(family);
  if (proof_frame == 0 || index >= g_retired.size()) {
    return false;
  }
  if (!g_retired[index].exchange(true, std::memory_order_acq_rel)) {
    REXLOG_INFO(
        "Table Tennis {} title capture retired proof_frame={} "
        "backend_guarded_filter_query_active=true family_local=true "
        "explicit_observer_and_main_overrides_active=true",
        FamilyName(family), proof_frame);
  }
  return true;
}

bool GuardedVenueTitleCaptureRetired(
    GuardedVenueTitleCaptureFamily family) {
  const size_t index = FamilyIndex(family);
  return index < g_retired.size() &&
         g_retired[index].load(std::memory_order_acquire);
}

void ResetGuardedVenueTitleCaptureRetirement() {
  for (std::atomic<bool> &retired : g_retired) {
    retired.store(false, std::memory_order_release);
  }
}

} // namespace tabletennis::native
