#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace tabletennis::native {

constexpr size_t kMaxVertexDeclarationElements = 32;

struct VertexDeclarationElement {
  uint16_t stream = 0;
  uint16_t byte_offset = 0;
  uint32_t packed_type = 0;
  uint8_t method = 0;
  uint8_t usage = 0;
  uint8_t usage_index = 0;
  // The title's declaration builder leaves this byte uninitialized.
  uint8_t unused_padding = 0;

  uint8_t format() const {
    return static_cast<uint8_t>(packed_type & 0x3Fu);
  }
  bool is_signed() const { return (packed_type & 0x100u) != 0; }
  bool normalized() const { return (packed_type & 0x200u) == 0; }
};

struct VertexDeclarationProbe {
  uint32_t guest_address = 0;
  uint32_t element_count = 0;
  uint32_t max_stream = 0;
  uint64_t stream_mask_lo = 0;
  uint64_t stream_mask_hi = 0;
  uint32_t cache_id = 0;
  std::array<VertexDeclarationElement, kMaxVertexDeclarationElements>
      elements{};
  uint32_t copy_failures = 0;
  bool valid = false;
};

// Reads the title's immutable vertex-declaration object through guarded guest
// copies. The returned structure is passive telemetry; it never changes title
// state or serves data to the renderer.
VertexDeclarationProbe ProbeVertexDeclaration(uint8_t* base,
                                               uint32_t declaration);

}  // namespace tabletennis::native
