#include "native/tabletennis_mesh_payload_probe.h"

#include "generated/default/tabletennis_init.h"
#include "native/tabletennis_guest_memory.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <vector>

namespace tabletennis::native {
namespace {

constexpr uint8_t kVertexEndian8In32 = 2;
constexpr uint8_t kVertexFormatFloat4 = 38;
constexpr uint8_t kPositionUsage = 0;
constexpr uint32_t kMaximumPayloadBytes = 16 * 1024 * 1024;
constexpr uint32_t kMaximumVertexStride = 1024;

uint16_t LoadBeU16(const uint8_t* bytes) {
  uint16_t value;
  std::memcpy(&value, bytes, sizeof(value));
  return std::byteswap(value);
}

float LoadBeF32(const uint8_t* bytes) {
  uint32_t value;
  std::memcpy(&value, bytes, sizeof(value));
  return std::bit_cast<float>(std::byteswap(value));
}

uint64_t Fingerprint(const std::vector<uint8_t>& bytes) {
  uint64_t hash = 1469598103934665603ull;
  for (uint8_t value : bytes) {
    hash = (hash ^ value) * 1099511628211ull;
  }
  return hash;
}

const VertexDeclarationElement* FindPosition(
    const VertexDeclarationProbe& declaration) {
  if (!declaration.valid) {
    return nullptr;
  }
  for (uint32_t index = 0; index < declaration.element_count; ++index) {
    const VertexDeclarationElement& element = declaration.elements[index];
    if (element.stream == 0 && element.usage == kPositionUsage) {
      return &element;
    }
  }
  return nullptr;
}

}  // namespace

MeshPayloadProbe ProbeMeshPayload(
    uint8_t* base, const MeshPayloadDescriptor& descriptor) {
  MeshPayloadProbe probe;
  probe.stream_selector = descriptor.stream_selector;
  probe.vertex_alias = descriptor.vertex_alias;
  probe.vertex_bytes = descriptor.vertex_bytes;
  probe.vertex_stride = descriptor.vertex_stride;
  probe.index_alias = descriptor.index_alias;
  probe.index_bytes = descriptor.index_bytes;
  probe.index_count = descriptor.submitted_index_count;

  const VertexDeclarationElement* position =
      FindPosition(descriptor.declaration);
  if (base == nullptr || descriptor.vertex_alias == 0 ||
      descriptor.index_alias == 0 || descriptor.vertex_bytes == 0 ||
      descriptor.index_bytes == 0 || descriptor.vertex_stride == 0 ||
      descriptor.vertex_stride > kMaximumVertexStride ||
      descriptor.vertex_bytes > kMaximumPayloadBytes ||
      descriptor.index_bytes > kMaximumPayloadBytes ||
      descriptor.vertex_bytes % descriptor.vertex_stride != 0 ||
      descriptor.vertex_endian != kVertexEndian8In32 ||
      descriptor.index_is_32_bit || descriptor.index_element_size != 2 ||
      descriptor.submitted_index_count == 0 ||
      descriptor.submitted_index_count >
          descriptor.index_bytes / descriptor.index_element_size ||
      position == nullptr || position->format() != kVertexFormatFloat4 ||
      static_cast<uint32_t>(position->byte_offset) + sizeof(float) * 4 >
          descriptor.vertex_stride) {
    return probe;
  }

  probe.vertex_count = descriptor.vertex_bytes / descriptor.vertex_stride;
  if (probe.vertex_count == 0) {
    return probe;
  }

  std::vector<uint8_t> vertex_bytes(descriptor.vertex_bytes);
  std::vector<uint8_t> index_bytes(
      static_cast<size_t>(descriptor.submitted_index_count) *
      descriptor.index_element_size);
  if (!GuestTryCopy(vertex_bytes.data(), REX_RAW_ADDR(descriptor.vertex_alias),
                    vertex_bytes.size())) {
    ++probe.copy_failures;
    return probe;
  }
  if (!GuestTryCopy(index_bytes.data(), REX_RAW_ADDR(descriptor.index_alias),
                    index_bytes.size())) {
    ++probe.copy_failures;
    return probe;
  }
  probe.vertex_fingerprint = Fingerprint(vertex_bytes);
  probe.index_fingerprint = Fingerprint(index_bytes);

  probe.minimum_index = std::numeric_limits<uint32_t>::max();
  for (uint32_t index = 0; index < descriptor.submitted_index_count; ++index) {
    const uint32_t vertex_index =
        LoadBeU16(index_bytes.data() + static_cast<size_t>(index) * 2);
    probe.minimum_index = std::min(probe.minimum_index, vertex_index);
    probe.maximum_index = std::max(probe.maximum_index, vertex_index);
    probe.out_of_range_indices += vertex_index >= probe.vertex_count;
  }
  if (descriptor.submitted_index_count >= 3) {
    for (uint32_t index = 0; index + 2 < descriptor.submitted_index_count;
         index += 3) {
      const uint16_t a =
          LoadBeU16(index_bytes.data() + static_cast<size_t>(index) * 2);
      const uint16_t b =
          LoadBeU16(index_bytes.data() + static_cast<size_t>(index + 1) * 2);
      const uint16_t c =
          LoadBeU16(index_bytes.data() + static_cast<size_t>(index + 2) * 2);
      probe.degenerate_triangles += a == b || b == c || a == c;
    }
  }

  probe.bounds_min.fill(std::numeric_limits<float>::infinity());
  probe.bounds_max.fill(-std::numeric_limits<float>::infinity());
  for (uint32_t vertex = 0; vertex < probe.vertex_count; ++vertex) {
    const uint8_t* source =
        vertex_bytes.data() + static_cast<size_t>(vertex) *
                                  descriptor.vertex_stride +
        position->byte_offset;
    float decoded[4];
    bool xyz_finite = true;
    for (uint32_t component = 0; component < 4; ++component) {
      decoded[component] = LoadBeF32(source + component * sizeof(float));
      if (component < 3) {
        xyz_finite &= std::isfinite(decoded[component]);
      }
    }
    probe.non_finite_position_w += !std::isfinite(decoded[3]);
    if (vertex == 0) {
      std::copy(std::begin(decoded), std::end(decoded),
                probe.first_position.begin());
    }
    // RAGE float4 storage commonly leaves the unused W padding lane as NaN.
    // Only XYZ participates in this mesh's position transform.
    if (!xyz_finite) {
      ++probe.non_finite_positions;
      continue;
    }
    ++probe.finite_positions;
    for (uint32_t axis = 0; axis < 3; ++axis) {
      probe.bounds_min[axis] = std::min(probe.bounds_min[axis], decoded[axis]);
      probe.bounds_max[axis] = std::max(probe.bounds_max[axis], decoded[axis]);
    }
  }

  probe.valid = probe.copy_failures == 0 &&
                probe.out_of_range_indices == 0 &&
                probe.non_finite_positions == 0 &&
                probe.finite_positions == probe.vertex_count;
  return probe;
}

}  // namespace tabletennis::native
