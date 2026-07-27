#pragma once

#include <cstddef>

namespace tabletennis::native {

// Fault-guarded copy from the guest mapping. Streaming may revoke a captured
// range between submission and observation; a failed read is expected and
// must skip the candidate rather than crash the title.
bool GuestTryCopy(void* destination, const void* source, size_t size);

}  // namespace tabletennis::native
