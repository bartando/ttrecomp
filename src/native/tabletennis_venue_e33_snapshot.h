#pragma once

#include "native/tabletennis_texture_snapshot.h"
#include "native/tabletennis_vertex_declaration.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace tabletennis::native {

struct SceneCatalogDrawOccurrence;

struct VenueE33DrawIdentity {
  uint32_t primitive_type = 0;
  uint32_t submitted_index_count = 0;
  uint32_t guest_index_base = 0;

  bool valid() const {
    return primitive_type != 0 && submitted_index_count != 0 &&
           guest_index_base != 0;
  }
  bool operator==(const VenueE33DrawIdentity &) const = default;
};

// Dynamic guest addresses selected by grmShaderFx::ApplyPass. The observer
// learns these only after an independent translated-backend E33 proof.
struct VenueE33TitleProgramIdentity {
  uint32_t pass_descriptor = 0;
  uint32_t program_pair = 0;
  uint32_t vertex_shader = 0;
  uint32_t pixel_shader = 0;

  bool valid() const {
    return pass_descriptor != 0 && program_pair != 0 && vertex_shader != 0 &&
           pixel_shader != 0;
  }
  bool operator==(const VenueE33TitleProgramIdentity &) const = default;
};

struct VenueE33VertexElementIdentity {
  uint16_t stream = 0;
  uint16_t byte_offset = 0;
  uint32_t packed_type = 0;
  uint8_t method = 0;
  uint8_t usage = 0;
  uint8_t usage_index = 0;

  bool operator==(const VenueE33VertexElementIdentity &) const = default;
};

struct VenueE33VertexDeclarationIdentity {
  static constexpr size_t kElementCount = 5;

  uint32_t element_count = 0;
  uint32_t max_stream = 0;
  uint64_t stream_mask_lo = 0;
  uint64_t stream_mask_hi = 0;
  std::array<VenueE33VertexElementIdentity, kElementCount> elements{};

  bool valid() const;
  bool operator==(const VenueE33VertexDeclarationIdentity &) const = default;
};

// Cheap title-side metadata retained during the learning phase. No guest
// payload is copied until a previous complete backend proof has learned the
// exact ordered title program identities.
struct VenueE33TitleCandidate {
  VenueE33DrawIdentity identity{};
  VenueE33TitleProgramIdentity program{};
  VenueE33VertexDeclarationIdentity vertex_declaration{};
  uint32_t ordinal = 0;
  uint32_t owner_kind = 0;
  uint32_t owner = 0;
  uint32_t owner_renderable = 0;
  bool eligible = false;
};

// Raw immutable copies are deliberate. The vertex layout is trace-proven,
// while decode and native rendering remain outside this observer-only module.
struct VenueE33VertexPayload {
  static constexpr uint32_t kStride = 32;
  static constexpr uint32_t kEndian8In32 = 2;

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

struct VenueE33IndexPayload {
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

struct VenueE33MaterialSnapshot {
  static constexpr size_t kTextureCount = 3;
  static constexpr size_t kConstantWordCount = 256 * 4;

  uint32_t vertex_declaration = 0;
  VertexDeclarationProbe vertex_declaration_probe{};
  std::array<std::array<uint32_t, 6>, kTextureCount> texture_fetches{};
  std::array<std::shared_ptr<const TextureSnapshot>, kTextureCount> textures{};
  // Preserve every draw-time constant bit. Trace comparison already proves
  // that E33 constant values change between frames and must not be hardcoded.
  std::array<uint32_t, kConstantWordCount> vertex_constant_words{};
  std::array<uint32_t, kConstantWordCount> pixel_constant_words{};
  bool valid = false;
};

struct VenueE33TitleDrawSnapshot {
  VenueE33DrawIdentity identity{};
  VenueE33TitleProgramIdentity program{};
  VenueE33VertexDeclarationIdentity vertex_declaration{};
  std::shared_ptr<const VenueE33VertexPayload> vertices;
  std::shared_ptr<const VenueE33IndexPayload> indices;
  VenueE33MaterialSnapshot material{};
  uint32_t ordinal = 0;
  uint32_t owner_kind = 0;
  uint32_t owner = 0;
  uint32_t owner_renderable = 0;
  bool valid = false;
};

struct VenueE33TitleCapture {
  std::shared_ptr<const VenueE33TitleDrawSnapshot> snapshot;
  uint32_t guest_read_failures = 0;
  uint32_t payload_copy_failures = 0;
  uint32_t texture_capture_failures = 0;
  uint32_t material_validation_failures = 0;
  // Renderer-contract telemetry only. These counts do not classify E33.
  uint32_t renderer_full_mip_texture_count = 0;
  uint32_t renderer_texture_shape_match_count = 0;
};

VenueE33TitleCandidate
ClassifyVenueE33TitleCandidate(uint8_t *guest_base,
                               const SceneCatalogDrawOccurrence &draw);

// Called only after a previous complete backend proof has learned that this
// title program/count pair belongs to E33.
VenueE33TitleCapture
CaptureVenueE33TitleDraw(uint8_t *guest_base,
                         const SceneCatalogDrawOccurrence &draw,
                         const VenueE33TitleCandidate &candidate);

} // namespace tabletennis::native
