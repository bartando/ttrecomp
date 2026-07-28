#pragma once

#include "native/tabletennis_texture_snapshot.h"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace tabletennis::native {

struct SceneCatalogDrawOccurrence;

inline constexpr uint64_t kVenue14DPixelShaderHash =
    0x14D6B61CBC3D853Cull;
inline constexpr uint64_t kVenue14DVertexShader40Hash =
    0x4EAEC701E97DCDADull;
inline constexpr uint64_t kVenue14DVertexShader48Hash =
    0x08D6210341AD63F6ull;
inline constexpr uint32_t kVenue14DVertexEndian = 2;

constexpr uint64_t Venue14DVertexShaderForLayout(uint32_t stride,
                                                 uint32_t endian) {
  if (endian != kVenue14DVertexEndian) {
    return 0;
  }
  if (stride == 40) {
    return kVenue14DVertexShader40Hash;
  }
  if (stride == 48) {
    return kVenue14DVertexShader48Hash;
  }
  return 0;
}

struct Venue14DDrawIdentity {
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
           guest_vertex_endian == kVenue14DVertexEndian;
  }
  bool operator==(const Venue14DDrawIdentity&) const = default;
};

// Raw immutable copies are intentional here. The 14D backend family uses two
// vertex variants, and format decode is not admitted until the independent
// backend join identifies which title submissions belong to each variant.
struct Venue14DVertexPayload {
  uint32_t source_virtual_alias = 0;
  uint32_t physical_address = 0;
  uint32_t byte_count = 0;
  uint32_t stride = 0;
  uint32_t vertex_count = 0;
  uint32_t endian = 0;
  uint64_t payload_fingerprint = 0;
  std::vector<uint8_t> raw_bytes;

  bool valid() const {
    return source_virtual_alias != 0 && physical_address != 0 &&
           byte_count != 0 && stride != 0 && vertex_count != 0 &&
           raw_bytes.size() == byte_count &&
           byte_count == static_cast<size_t>(vertex_count) * stride;
  }
};

struct Venue14DIndexPayload {
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

struct Venue14DMaterialSnapshot {
  static constexpr size_t kTextureCount = 5;
  static constexpr size_t kConstantWordCount = 256 * 4;

  uint32_t vertex_declaration = 0;
  std::array<std::array<uint32_t, 6>, kTextureCount> texture_fetches{};
  std::array<std::shared_ptr<const TextureSnapshot>, kTextureCount> textures{};
  // Preserve every draw-time constant bit, not a guessed subset. Shader
  // disassembly will later narrow this to the actual live ranges.
  std::array<uint32_t, kConstantWordCount> vertex_constant_words{};
  std::array<uint32_t, kConstantWordCount> pixel_constant_words{};
  bool valid = false;
};

struct Venue14DTitleDrawSnapshot {
  Venue14DDrawIdentity identity{};
  std::shared_ptr<const Venue14DVertexPayload> vertices;
  std::shared_ptr<const Venue14DIndexPayload> indices;
  Venue14DMaterialSnapshot material{};
  uint32_t ordinal = 0;
  uint32_t pass_descriptor = 0;
  uint32_t program_pair = 0;
  uint32_t title_vertex_shader = 0;
  uint32_t title_pixel_shader = 0;
  uint32_t owner_kind = 0;
  uint32_t owner = 0;
  uint32_t owner_renderable = 0;
  bool valid = false;
};

struct Venue14DTitleCapture {
  Venue14DDrawIdentity identity{};
  std::shared_ptr<const Venue14DTitleDrawSnapshot> snapshot;
  uint32_t guest_read_failures = 0;
  uint32_t payload_copy_failures = 0;
  uint32_t texture_capture_failures = 0;
  bool family_candidate = false;
};

// Exact guest pass/program identities are correlated in
// docs/venue_14d_capture_contract.md. Admission to the published family still
// requires the independent backend hash/order proof.
Venue14DTitleCapture CaptureVenue14DTitleDraw(
    uint8_t* guest_base, const SceneCatalogDrawOccurrence& draw);

}  // namespace tabletennis::native
