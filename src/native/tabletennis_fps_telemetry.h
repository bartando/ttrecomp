#pragma once

#include <cstdint>

namespace tabletennis::native {

// Samples the presenter's guest-swap statistics and periodically writes them
// to the normal runtime log. Call once at the title-side frame boundary.
void UpdateGuestFpsTelemetry(uint64_t title_frame_sequence);

} // namespace tabletennis::native
