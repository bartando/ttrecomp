#pragma once

#include <cstddef>
#include <cstdint>

namespace tabletennis::native {

// Fault-guarded copy from the guest mapping. Streaming may revoke a captured
// range between submission and observation; a failed read is expected and
// must skip the candidate rather than crash the title.
bool GuestTryCopy(void* destination, const void* source, size_t size);

// Canonical title high-heap virtual alias -> GPU physical address conversion.
// Returns zero for an unsupported/non-high-heap alias.
uint32_t GuestPhysicalAddressForVirtualAlias(uint32_t virtual_address);

}  // namespace tabletennis::native
