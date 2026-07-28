#pragma once

#include "native/tabletennis_scene_owner_observer.h"
#include "native/tabletennis_texture_snapshot.h"
#include "native/tabletennis_vertex_declaration.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace tabletennis::native {

struct SceneCatalogDrawOccurrence;

inline constexpr uint32_t kVenue9EVertexStride = 32;
inline constexpr uint32_t kVenue9EVertexEndian = 2;

struct Venue9EDrawIdentity {
  uint32_t primitive_type = 0;
  uint32_t submitted_index_count = 0;
  uint32_t guest_index_base = 0;
  uint32_t guest_vertex_base = 0;
  uint32_t guest_vertex_bytes = 0;
  uint32_t guest_vertex_endian = 0;

  bool valid() const {
    return primitive_type != 0 && submitted_index_count != 0 &&
           guest_index_base != 0 && guest_vertex_base != 0 &&
           guest_vertex_bytes >= kVenue9EVertexStride &&
           guest_vertex_bytes % kVenue9EVertexStride == 0 &&
           guest_vertex_endian == kVenue9EVertexEndian;
  }
  bool operator==(const Venue9EDrawIdentity &) const = default;
};

struct Venue9ETitleProgramIdentity {
  uint32_t pass_descriptor = 0;
  uint32_t program_pair = 0;
  uint32_t vertex_shader = 0;
  uint32_t pixel_shader = 0;
  uint64_t vertex_shader_hash = 0;
  uint64_t pixel_shader_hash = 0;
  bool vertex_hash_from_bound_shader = false;
  bool pixel_hash_from_bound_shader = false;

  bool valid() const;
  bool operator==(const Venue9ETitleProgramIdentity &) const = default;
};

struct Venue9EVertexElementIdentity {
  uint16_t stream = 0;
  uint16_t byte_offset = 0;
  uint32_t packed_type = 0;
  uint8_t method = 0;
  uint8_t usage = 0;
  uint8_t usage_index = 0;

  bool operator==(const Venue9EVertexElementIdentity &) const = default;
};

struct Venue9EVertexDeclarationIdentity {
  static constexpr size_t kElementCount = 5;

  uint32_t element_count = 0;
  uint32_t max_stream = 0;
  uint64_t stream_mask_lo = 0;
  uint64_t stream_mask_hi = 0;
  std::array<Venue9EVertexElementIdentity, kElementCount> elements{};

  bool valid() const;
  bool operator==(const Venue9EVertexDeclarationIdentity &) const = default;
};

struct Venue9EVertexPayload {
  static constexpr uint32_t kStride = kVenue9EVertexStride;
  static constexpr uint32_t kEndian8In32 = kVenue9EVertexEndian;

  uint32_t source_virtual_alias = 0;
  uint32_t physical_address = 0;
  uint32_t byte_count = 0;
  uint32_t vertex_count = 0;
  uint64_t payload_fingerprint = 0;
  std::vector<uint8_t> raw_bytes;

  bool valid() const {
    return source_virtual_alias != 0 && physical_address != 0 &&
           byte_count != 0 && vertex_count != 0 &&
           raw_bytes.size() == byte_count &&
           byte_count == static_cast<size_t>(vertex_count) * kStride;
  }
};

struct Venue9EIndexPayload {
  uint32_t source_virtual_alias = 0;
  uint32_t physical_address = 0;
  uint32_t submitted_index_count = 0;
  uint16_t minimum_index = 0;
  uint16_t maximum_index = 0;
  uint64_t payload_fingerprint = 0;
  std::vector<uint8_t> raw_bytes;
  std::vector<uint16_t> indices;

  bool valid() const {
    return source_virtual_alias != 0 && physical_address != 0 &&
           submitted_index_count != 0 &&
           raw_bytes.size() ==
               static_cast<size_t>(submitted_index_count) * sizeof(uint16_t) &&
           indices.size() == submitted_index_count &&
           minimum_index <= maximum_index;
  }
};

struct Venue9EMaterialSnapshot {
  uint32_t vertex_declaration = 0;
  VertexDeclarationProbe vertex_declaration_probe{};
  std::array<uint32_t, 6> texture_fetch_0{};
  std::shared_ptr<const TextureSnapshot> texture_0;
  std::array<float, 28> vertex_constants_0_6{};
  std::array<float, 16> vertex_constants_12_15{};
  std::array<float, 4> pixel_constant_20{};
  std::array<float, 4> pixel_constant_254{};
  std::array<float, 4> pixel_constant_255{};
  bool exact_texture_contract = false;
  bool valid = false;
};

struct Venue9ETitleCandidate {
  Venue9EDrawIdentity identity{};
  Venue9ETitleProgramIdentity program{};
  Venue9EVertexDeclarationIdentity vertex_declaration{};
  uint64_t sequence = 0;
  uint32_t ordinal = 0;
  bool eligible = false;
};

struct Venue9ETitleDrawSnapshot {
  Venue9EDrawIdentity identity{};
  Venue9ETitleProgramIdentity program{};
  Venue9EVertexDeclarationIdentity vertex_declaration{};
  uint64_t sequence = 0;
  uint32_t ordinal = 0;
  SceneOwnerToken owner{};
  uint32_t material_shader = 0;
  uint32_t material_shader_vtable = 0;
  uint32_t model = 0;
  uint32_t geometry_index = 0;
  uint32_t lod = 0;
  bool alternate_pass = false;
  uint32_t vertex_aggregate = 0;
  std::shared_ptr<const Venue9EVertexPayload> vertices;
  std::shared_ptr<const Venue9EIndexPayload> indices;
  Venue9EMaterialSnapshot material{};
  std::array<float, 16> world{};
  std::array<float, 16> world_view_projection{};
  bool valid = false;
};

struct Venue9ETitleCapture {
  std::shared_ptr<const Venue9ETitleDrawSnapshot> snapshot;
  uint32_t guest_read_failures = 0;
  uint32_t payload_copy_failures = 0;
  uint32_t texture_capture_failures = 0;
  uint32_t material_validation_failures = 0;
};

Venue9ETitleCandidate
ClassifyVenue9ETitleCandidate(uint8_t *guest_base,
                              const SceneCatalogDrawOccurrence &draw);

Venue9ETitleCapture
CaptureVenue9ETitleDraw(uint8_t *guest_base,
                        const SceneCatalogDrawOccurrence &draw,
                        const Venue9ETitleCandidate &candidate);

} // namespace tabletennis::native
