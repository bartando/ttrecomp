#include "native/tabletennis_mesh_snapshot.h"

#include "generated/default/tabletennis_init.h"
#include "native/tabletennis_guest_memory.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <mutex>
#include <utility>

#include <rex/logging.h>

namespace tabletennis::native {
namespace {

constexpr uint8_t kVertexEndian8In32 = 2;
constexpr uint8_t kVertexFormatFloat4 = 38;
constexpr uint8_t kVertexFormat8_8_8_8 = 6;
constexpr uint8_t kPositionUsage = 0;
constexpr uint8_t kTexcoordUsage = 5;
constexpr uint8_t kColorUsage = 10;
constexpr uint32_t kMaximumPayloadBytes = 16 * 1024 * 1024;
constexpr uint32_t kMaximumVertexStride = 1024;

std::atomic<bool> g_capture_claimed = false;
std::mutex g_snapshot_mutex;
std::shared_ptr<const TableMeshSnapshot> g_snapshot;

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

const VertexDeclarationElement* FindElement(
    const VertexDeclarationProbe& declaration, uint8_t usage,
    uint8_t usage_index) {
  if (!declaration.valid) {
    return nullptr;
  }
  for (uint32_t index = 0; index < declaration.element_count; ++index) {
    const VertexDeclarationElement& element = declaration.elements[index];
    if (element.stream == 0 && element.usage == usage &&
        element.usage_index == usage_index) {
      return &element;
    }
  }
  return nullptr;
}

bool DescriptorIsSupported(const MeshPayloadDescriptor& descriptor,
                           const VertexDeclarationElement* position,
                           const VertexDeclarationElement* texcoord0,
                           const VertexDeclarationElement* texcoord1,
                           const VertexDeclarationElement* color) {
  return descriptor.vertex_alias != 0 && descriptor.index_alias != 0 &&
         descriptor.vertex_bytes != 0 && descriptor.index_bytes != 0 &&
         descriptor.vertex_stride != 0 &&
         descriptor.vertex_stride <= kMaximumVertexStride &&
         descriptor.vertex_bytes <= kMaximumPayloadBytes &&
         descriptor.index_bytes <= kMaximumPayloadBytes &&
         descriptor.vertex_bytes % descriptor.vertex_stride == 0 &&
         descriptor.vertex_endian == kVertexEndian8In32 &&
         !descriptor.index_is_32_bit &&
         descriptor.index_element_size == sizeof(uint16_t) &&
         descriptor.submitted_index_count >= 3 &&
         descriptor.submitted_index_count % 3 == 0 &&
         descriptor.submitted_index_count <=
             descriptor.index_bytes / sizeof(uint16_t) &&
         position != nullptr && position->format() == kVertexFormatFloat4 &&
         static_cast<uint32_t>(position->byte_offset) + sizeof(float) * 4 <=
             descriptor.vertex_stride &&
         texcoord0 != nullptr &&
         texcoord0->format() == kVertexFormatFloat4 &&
         static_cast<uint32_t>(texcoord0->byte_offset) + sizeof(float) * 4 <=
             descriptor.vertex_stride &&
         texcoord1 != nullptr &&
         texcoord1->format() == kVertexFormatFloat4 &&
         static_cast<uint32_t>(texcoord1->byte_offset) + sizeof(float) * 4 <=
             descriptor.vertex_stride &&
         color != nullptr && color->format() == kVertexFormat8_8_8_8 &&
         static_cast<uint32_t>(color->byte_offset) + sizeof(uint32_t) <=
             descriptor.vertex_stride;
}

}  // namespace

void TryCaptureTableMeshSnapshot(
    uint8_t* base, const MeshPayloadDescriptor& descriptor) {
  if (base == nullptr || g_capture_claimed.exchange(true)) {
    return;
  }

  const VertexDeclarationElement* position =
      FindPosition(descriptor.declaration);
  const VertexDeclarationElement* texcoord0 =
      FindElement(descriptor.declaration, kTexcoordUsage, 0);
  const VertexDeclarationElement* texcoord1 =
      FindElement(descriptor.declaration, kTexcoordUsage, 1);
  const VertexDeclarationElement* color =
      FindElement(descriptor.declaration, kColorUsage, 0);
  if (!DescriptorIsSupported(descriptor, position, texcoord0, texcoord1,
                             color)) {
    g_capture_claimed.store(false);
    return;
  }

  const uint32_t vertex_count =
      descriptor.vertex_bytes / descriptor.vertex_stride;
  std::vector<uint8_t> vertex_bytes(descriptor.vertex_bytes);
  std::vector<uint8_t> index_bytes(
      static_cast<size_t>(descriptor.submitted_index_count) *
      sizeof(uint16_t));
  if (vertex_count == 0 ||
      !GuestTryCopy(vertex_bytes.data(), REX_RAW_ADDR(descriptor.vertex_alias),
                    vertex_bytes.size()) ||
      !GuestTryCopy(index_bytes.data(), REX_RAW_ADDR(descriptor.index_alias),
                    index_bytes.size())) {
    g_capture_claimed.store(false);
    return;
  }

  auto snapshot = std::make_shared<TableMeshSnapshot>();
  snapshot->positions.resize(vertex_count);
  snapshot->texcoords0.resize(vertex_count);
  snapshot->texcoords1.resize(vertex_count);
  snapshot->colors.resize(vertex_count);
  snapshot->indices.resize(descriptor.submitted_index_count);
  for (uint32_t vertex = 0; vertex < vertex_count; ++vertex) {
    const uint8_t* source =
        vertex_bytes.data() + static_cast<size_t>(vertex) *
                                  descriptor.vertex_stride +
        position->byte_offset;
    std::array<float, 3>& decoded = snapshot->positions[vertex];
    for (size_t axis = 0; axis < decoded.size(); ++axis) {
      decoded[axis] = LoadBeF32(source + axis * sizeof(float));
      if (!std::isfinite(decoded[axis]) ||
          std::abs(decoded[axis]) > 1000000.0f) {
        g_capture_claimed.store(false);
        return;
      }
    }
    const uint8_t* texcoord_source =
        vertex_bytes.data() + static_cast<size_t>(vertex) *
                                  descriptor.vertex_stride +
        texcoord0->byte_offset;
    std::array<float, 2>& decoded_texcoord = snapshot->texcoords0[vertex];
    for (size_t axis = 0; axis < decoded_texcoord.size(); ++axis) {
      decoded_texcoord[axis] =
          LoadBeF32(texcoord_source + axis * sizeof(float));
      if (!std::isfinite(decoded_texcoord[axis]) ||
          std::abs(decoded_texcoord[axis]) > 1000000.0f) {
        g_capture_claimed.store(false);
        return;
      }
    }
    const uint8_t* texcoord1_source =
        vertex_bytes.data() + static_cast<size_t>(vertex) *
                                  descriptor.vertex_stride +
        texcoord1->byte_offset;
    std::array<float, 2>& decoded_texcoord1 =
        snapshot->texcoords1[vertex];
    for (size_t axis = 0; axis < decoded_texcoord1.size(); ++axis) {
      decoded_texcoord1[axis] =
          LoadBeF32(texcoord1_source + axis * sizeof(float));
      if (!std::isfinite(decoded_texcoord1[axis]) ||
          std::abs(decoded_texcoord1[axis]) > 1000000.0f) {
        g_capture_claimed.store(false);
        return;
      }
    }

    // The net VS fetches COLOR0 as 8_8_8_8 UNORM with k8in32 endian and
    // applies .zyxw. For raw guest bytes [a,b,c,d], that yields
    // [b,c,d,a].
    const uint8_t* color_source =
        vertex_bytes.data() + static_cast<size_t>(vertex) *
                                  descriptor.vertex_stride +
        color->byte_offset;
    snapshot->colors[vertex] = {
        color_source[1] / 255.0f,
        color_source[2] / 255.0f,
        color_source[3] / 255.0f,
        color_source[0] / 255.0f,
    };
  }
  for (uint32_t index = 0; index < descriptor.submitted_index_count;
       ++index) {
    const uint16_t decoded =
        LoadBeU16(index_bytes.data() + static_cast<size_t>(index) * 2);
    if (decoded >= vertex_count) {
      g_capture_claimed.store(false);
      return;
    }
    snapshot->indices[index] = decoded;
  }

  snapshot->stream_selector = descriptor.stream_selector;
  snapshot->source_vertex_alias = descriptor.vertex_alias;
  snapshot->source_index_alias = descriptor.index_alias;
  snapshot->vertex_fingerprint = Fingerprint(vertex_bytes);
  snapshot->index_fingerprint = Fingerprint(index_bytes);
  {
    std::lock_guard lock(g_snapshot_mutex);
    g_snapshot = std::move(snapshot);
  }

  const std::shared_ptr<const TableMeshSnapshot> published =
      LatestTableMeshSnapshot();
  REXLOG_INFO(
      "Table Tennis mesh snapshot: captured real table geometry "
      "vertices={} indices={} selector={} vb={:08X} ib={:08X} "
      "observer-only",
      published->positions.size(), published->indices.size(),
      published->stream_selector, published->source_vertex_alias,
      published->source_index_alias);
}

bool HasTableMeshSnapshot() {
  std::lock_guard lock(g_snapshot_mutex);
  return g_snapshot != nullptr;
}

std::shared_ptr<const TableMeshSnapshot> LatestTableMeshSnapshot() {
  std::lock_guard lock(g_snapshot_mutex);
  return g_snapshot;
}

}  // namespace tabletennis::native
