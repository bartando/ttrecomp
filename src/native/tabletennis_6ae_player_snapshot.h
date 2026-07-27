#pragma once

#include "native/tabletennis_texture_snapshot.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace tabletennis::native {

struct SceneCatalogDrawOccurrence;

struct Player6AEVertexFetch {
  uint32_t slot = 0;
  uint32_t physical_address = 0;
  uint32_t size = 0;
  uint32_t endian = 0;
  std::array<uint32_t, 2> words{};
  bool valid = false;
};

struct Player6AEVertexPayload {
  Player6AEVertexFetch fetch{};
  uint32_t source_virtual_alias = 0;
  uint32_t stride = 0;
  uint32_t vertex_count = 0;
  uint64_t payload_fingerprint = 0;
  std::vector<uint8_t> raw_bytes;

  bool valid() const {
    return fetch.valid && source_virtual_alias != 0 && stride == 36 &&
           vertex_count != 0 &&
           raw_bytes.size() == static_cast<size_t>(vertex_count) * stride;
  }
};

struct Player6AEIndexPayload {
  uint32_t source_virtual_alias = 0;
  uint32_t physical_address = 0;
  uint32_t submitted_index_count = 0;
  uint64_t payload_fingerprint = 0;
  std::vector<uint8_t> raw_bytes;
  std::vector<uint16_t> indices;

  bool valid() const {
    return source_virtual_alias != 0 && physical_address != 0 &&
           submitted_index_count != 0 &&
           raw_bytes.size() ==
               static_cast<size_t>(submitted_index_count) * sizeof(uint16_t) &&
           indices.size() == submitted_index_count;
  }
};

struct Player6AEPaletteRecord {
  std::array<float, 4> quaternion{};
  std::array<float, 3> translation{};
};

struct Player6AEPalettePayload {
  Player6AEVertexFetch fetch{};
  uint32_t record_count = 0;
  uint32_t referenced_record_count = 0;
  uint64_t payload_fingerprint = 0;
  std::vector<uint8_t> raw_bytes;
  std::vector<Player6AEPaletteRecord> records;
  bool referenced_records_valid = false;

  bool valid() const {
    return fetch.valid && record_count != 0 && referenced_record_count != 0 &&
           raw_bytes.size() == static_cast<size_t>(record_count) * 28 &&
           records.size() == record_count && referenced_records_valid;
  }
};

struct Player6AEMaterialSnapshot {
  static constexpr size_t kTextureBindingCount = 6;
  static constexpr size_t kMaterialTextureCount = 3;
  static constexpr std::array<uint32_t, kMaterialTextureCount>
      kMaterialTextureBindings = {0, 1, 5};

  std::array<std::array<uint32_t, 6>, kTextureBindingCount> texture_fetches{};
  std::array<uint32_t, kTextureBindingCount> texture_view_swizzles{};
  // Bindings 0, 1 and 5 are the changing material images. Bindings 2-4 are
  // retained as exact descriptors for the shared mask, depth and lookup
  // inputs, without copying those frame-global resources into every draw.
  std::array<std::shared_ptr<const TextureSnapshot>, kMaterialTextureCount>
      material_textures{};

  std::array<float, 16> vertex_constants_12_15{};
  std::array<float, 4> vertex_constant_19{};
  std::array<float, 32> vertex_constants_29_36{};
  std::array<float, 8> vertex_constants_46_47{};
  std::array<float, 4> vertex_constant_255{};

  std::array<float, 4> pixel_constant_19{};
  std::array<float, 28> pixel_constants_21_27{};
  std::array<float, 100> pixel_constants_46_70{};
  std::array<float, 8> pixel_constants_254_255{};
  bool literal_contract_valid = false;
  bool valid = false;
};

struct Player6AEDrawIdentity {
  uint32_t primitive_type = 0;
  uint32_t submitted_index_count = 0;
  uint32_t guest_index_base = 0;

  bool valid() const {
    return primitive_type != 0 && submitted_index_count != 0 &&
           guest_index_base != 0;
  }
  bool operator==(const Player6AEDrawIdentity&) const = default;
};

struct Player6AEBackendContract {
  uint64_t vertex_shader_hash = 0;
  uint64_t pixel_shader_hash = 0;
  uint32_t surface_pitch = 0;
  uint32_t render_pass_key = 0;
  uint32_t normalized_depth_control = 0;
  uint32_t normalized_color_mask = 0;
  uint32_t color_control = 0;
  uint32_t blend_control_0 = 0;
  uint32_t primitive_restart_index = 0;
  std::array<uint32_t, 4> color_attachment_formats{};
  uint32_t color_attachment_count = 0;
  uint32_t depth_attachment_format = 0;
  uint32_t stencil_attachment_format = 0;
  uint32_t sample_count = 0;
  uint64_t sample_mask = 0;
  bool primitive_restart_enabled = false;
  bool valid = false;
};

struct Player6AEDrawSnapshot {
  std::shared_ptr<const Player6AEVertexPayload> vertices;
  std::shared_ptr<const Player6AEIndexPayload> indices;
  std::shared_ptr<const Player6AEPalettePayload> palette;
  Player6AEMaterialSnapshot material{};
  Player6AEBackendContract backend{};
  Player6AEDrawIdentity identity{};
  uint32_t ordinal = 0;
  uint32_t player = 0;
  uint32_t shader = 0;
  uint32_t model = 0;
  uint32_t geometry_index = 0;
  uint32_t pass_descriptor = 0;
  uint32_t program_pair = 0;
  uint32_t title_vertex_shader = 0;
  uint32_t title_pixel_shader = 0;
  bool alternate_pass = false;
  bool valid = false;
};

struct Player6AETitleCapture {
  Player6AEDrawIdentity identity{};
  std::shared_ptr<const Player6AEDrawSnapshot> snapshot;
  Player6AEVertexFetch primary_fetch{};
  Player6AEVertexFetch palette_fetch{};
  uint32_t expected_vertex_physical_address = 0;
  uint32_t index_physical_address = 0;
  uint32_t guest_read_failures = 0;
  uint32_t payload_copy_failures = 0;
  uint32_t texture_capture_failures = 0;
  bool fetch_contract_valid = false;
  bool family_candidate = false;
};

// Cheap family-agnostic player candidate. Shader identity is deliberately
// excluded: only the independently translated backend stream may select a
// structural title draw as 6AE.
bool IsStructuralPlayer6AETitleDraw(
    const SceneCatalogDrawOccurrence& draw);

// Copies both live guest vertex streams, the submitted index range, material
// images/descriptors and all shader constants used by the traced family.
// Nothing here serves or suppresses a title draw.
Player6AETitleCapture CapturePlayer6AETitleDraw(
    uint8_t* guest_base, const SceneCatalogDrawOccurrence& draw);

// Dynamic palettes are frame-owned; immutable mesh/index/texture payloads use
// bounded caches shared across frames.
void Player6AESnapshotFrameEnd();

}  // namespace tabletennis::native
