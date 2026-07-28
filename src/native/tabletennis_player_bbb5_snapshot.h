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

inline constexpr uint32_t kPlayerBBB5VertexEndian = 2;

struct PlayerBBB5DrawIdentity {
  uint32_t primitive_type = 0;
  uint32_t submitted_index_count = 0;
  uint32_t guest_index_base = 0;
  uint32_t guest_vertex_base = 0;
  uint32_t guest_vertex_bytes = 0;
  uint32_t guest_vertex_endian = 0;

  bool valid() const {
    return primitive_type != 0 && submitted_index_count != 0 &&
           guest_index_base != 0 && guest_vertex_base != 0 &&
           guest_vertex_bytes != 0 &&
           guest_vertex_endian == kPlayerBBB5VertexEndian;
  }
  bool operator==(const PlayerBBB5DrawIdentity &) const = default;
};

struct PlayerBBB5TitleProgramIdentity {
  uint32_t pass_descriptor = 0;
  uint32_t program_pair = 0;
  uint32_t vertex_shader = 0;
  uint32_t pixel_shader = 0;
  uint64_t vertex_shader_hash = 0;
  uint64_t pixel_shader_hash = 0;
  bool vertex_hash_from_bound_shader = false;
  bool pixel_hash_from_bound_shader = false;

  bool valid() const;
  bool operator==(const PlayerBBB5TitleProgramIdentity &) const = default;
};

struct PlayerBBB5VertexElementIdentity {
  uint16_t stream = 0;
  uint16_t byte_offset = 0;
  uint32_t packed_type = 0;
  uint8_t method = 0;
  uint8_t usage = 0;
  uint8_t usage_index = 0;

  bool operator==(const PlayerBBB5VertexElementIdentity &) const = default;
};

struct PlayerBBB5VertexDeclarationIdentity {
  static constexpr size_t kElementCount = kMaxVertexDeclarationElements;

  uint32_t element_count = 0;
  uint32_t max_stream = 0;
  uint64_t stream_mask_lo = 0;
  uint64_t stream_mask_hi = 0;
  std::array<PlayerBBB5VertexElementIdentity, kElementCount> elements{};

  bool valid() const;
  bool operator==(const PlayerBBB5VertexDeclarationIdentity &) const = default;
};

struct PlayerBBB5VertexPayload {
  static constexpr uint32_t kEndian8In32 = kPlayerBBB5VertexEndian;

  uint32_t source_virtual_alias = 0;
  uint32_t physical_address = 0;
  uint32_t byte_count = 0;
  uint32_t stride = 0;
  uint32_t vertex_count = 0;
  uint64_t payload_fingerprint = 0;
  std::vector<uint8_t> raw_bytes;

  bool valid() const {
    return source_virtual_alias != 0 && physical_address != 0 &&
           byte_count != 0 && vertex_count != 0 &&
           raw_bytes.size() == byte_count && stride != 0 &&
           byte_count == static_cast<size_t>(vertex_count) * stride;
  }
};

struct PlayerBBB5IndexPayload {
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

struct PlayerBBB5MaterialSnapshot {
  static constexpr size_t kTextureFetchCount = 1;
  static constexpr size_t kOwnedTextureCount = 1;

  uint32_t vertex_declaration = 0;
  VertexDeclarationProbe vertex_declaration_probe{};
  std::array<std::array<uint32_t, 6>, kTextureFetchCount> texture_fetches{};
  std::array<std::shared_ptr<const TextureSnapshot>, kOwnedTextureCount>
      owned_textures{};
  std::array<float, 16> vertex_constants_0_3{};
  std::array<float, 16> vertex_constants_12_15{};
  std::array<float, 4> vertex_constant_19{};
  std::array<float, 32> vertex_constants_29_36{};
  std::array<float, 8> vertex_constants_46_47{};
  std::array<float, 4> vertex_constant_255{};
  std::array<float, 4> pixel_constant_19{};
  std::array<float, 28> pixel_constants_21_27{};
  std::array<float, 16> pixel_constants_46_49{};
  std::array<float, 4> pixel_constant_255{};
  bool valid = false;
};

struct PlayerBBB5TitleCandidate {
  PlayerBBB5DrawIdentity identity{};
  PlayerBBB5TitleProgramIdentity program{};
  PlayerBBB5VertexDeclarationIdentity vertex_declaration{};
  uint64_t sequence = 0;
  uint32_t ordinal = 0;
  uint32_t player = 0;
  uint32_t material_shader = 0;
  uint32_t material_shader_vtable = 0;
  uint32_t model = 0;
  uint32_t vertex_aggregate = 0;
  bool eligible = false;
};

struct PlayerBBB5TitleDrawSnapshot {
  PlayerBBB5DrawIdentity identity{};
  PlayerBBB5TitleProgramIdentity program{};
  PlayerBBB5VertexDeclarationIdentity vertex_declaration{};
  uint64_t sequence = 0;
  uint32_t ordinal = 0;
  uint32_t player = 0;
  SceneOwnerToken owner{};
  uint32_t material_shader = 0;
  uint32_t material_shader_vtable = 0;
  uint32_t model = 0;
  uint32_t geometry_index = 0;
  uint32_t lod = 0;
  bool alternate_pass = false;
  uint32_t vertex_aggregate = 0;
  std::shared_ptr<const PlayerBBB5VertexPayload> vertices;
  std::shared_ptr<const PlayerBBB5IndexPayload> indices;
  PlayerBBB5MaterialSnapshot material{};
  std::array<float, 16> world{};
  std::array<float, 16> world_view_projection{};
  bool valid = false;
};

struct PlayerBBB5TitleCapture {
  std::shared_ptr<const PlayerBBB5TitleDrawSnapshot> snapshot;
  uint32_t guest_read_failures = 0;
  uint32_t payload_copy_failures = 0;
  uint32_t texture_capture_failures = 0;
  uint32_t material_validation_failures = 0;
};

PlayerBBB5TitleCandidate
ClassifyPlayerBBB5TitleCandidate(uint8_t *guest_base,
                                 const SceneCatalogDrawOccurrence &draw);

PlayerBBB5TitleCapture
CapturePlayerBBB5TitleDraw(uint8_t *guest_base,
                           const SceneCatalogDrawOccurrence &draw,
                           const PlayerBBB5TitleCandidate &candidate);

} // namespace tabletennis::native
