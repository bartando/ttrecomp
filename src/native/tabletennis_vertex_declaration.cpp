#include "native/tabletennis_vertex_declaration.h"

#include "generated/default/tabletennis_init.h"
#include "native/tabletennis_guest_memory.h"

#include <array>
#include <bit>
#include <cstddef>
#include <cstring>
#include <limits>

namespace tabletennis::native {
namespace {

constexpr size_t kDeclarationHeaderBytes = 0x24;
constexpr size_t kElementBytes = 12;

uint16_t LoadBeU16(const std::byte* bytes, size_t offset) {
  uint16_t value;
  std::memcpy(&value, bytes + offset, sizeof(value));
  return std::byteswap(value);
}

uint32_t LoadBeU32(const std::byte* bytes, size_t offset) {
  uint32_t value;
  std::memcpy(&value, bytes + offset, sizeof(value));
  return std::byteswap(value);
}

uint64_t LoadBeU64(const std::byte* bytes, size_t offset) {
  uint64_t value;
  std::memcpy(&value, bytes + offset, sizeof(value));
  return std::byteswap(value);
}

bool CheckedGuestRange(uint32_t address, size_t offset, size_t size,
                       uint32_t& guest_address) {
  if (address == 0) {
    return false;
  }
  const uint64_t start = static_cast<uint64_t>(address) + offset;
  const uint64_t end = start + size;
  if (start > std::numeric_limits<uint32_t>::max() ||
      end > (uint64_t{1} << 32)) {
    return false;
  }
  guest_address = static_cast<uint32_t>(start);
  return true;
}

bool TryCopyGuest(uint8_t* base, uint32_t address, size_t offset,
                  void* destination, size_t size) {
  uint32_t guest_address;
  return base != nullptr &&
         CheckedGuestRange(address, offset, size, guest_address) &&
         GuestTryCopy(destination, REX_RAW_ADDR(guest_address), size);
}

}  // namespace

VertexDeclarationProbe ProbeVertexDeclaration(uint8_t* base,
                                               uint32_t declaration) {
  VertexDeclarationProbe probe;
  probe.guest_address = declaration;
  if (base == nullptr || declaration == 0) {
    return probe;
  }

  std::array<std::byte, kDeclarationHeaderBytes> header;
  if (!TryCopyGuest(base, declaration, 0, header.data(), header.size())) {
    ++probe.copy_failures;
    return probe;
  }

  probe.element_count = LoadBeU32(header.data(), 0x08);
  probe.max_stream = LoadBeU32(header.data(), 0x0C);
  probe.stream_mask_lo = LoadBeU64(header.data(), 0x10);
  probe.stream_mask_hi = LoadBeU64(header.data(), 0x18);
  probe.cache_id = LoadBeU32(header.data(), 0x20);
  if (probe.element_count == 0 ||
      probe.element_count > probe.elements.size()) {
    ++probe.copy_failures;
    return probe;
  }

  std::array<std::byte,
             kMaxVertexDeclarationElements * kElementBytes>
      element_bytes;
  const size_t bytes_to_copy = probe.element_count * kElementBytes;
  if (!TryCopyGuest(base, declaration, kDeclarationHeaderBytes,
                    element_bytes.data(), bytes_to_copy)) {
    ++probe.copy_failures;
    return probe;
  }

  for (uint32_t index = 0; index < probe.element_count; ++index) {
    const std::byte* element = element_bytes.data() + index * kElementBytes;
    VertexDeclarationElement& decoded = probe.elements[index];
    decoded.stream = LoadBeU16(element, 0x00);
    decoded.byte_offset = LoadBeU16(element, 0x02);
    decoded.packed_type = LoadBeU32(element, 0x04);
    decoded.method = std::to_integer<uint8_t>(element[0x08]);
    decoded.usage = std::to_integer<uint8_t>(element[0x09]);
    decoded.usage_index = std::to_integer<uint8_t>(element[0x0A]);
    decoded.unused_padding = std::to_integer<uint8_t>(element[0x0B]);
  }
  probe.valid = true;
  return probe;
}

}  // namespace tabletennis::native
