#pragma once

#include "native/tabletennis_vertex_declaration.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace tabletennis::native {

struct SceneCatalogDrawOccurrence;

// Immutable metadata captured at fxCrowdGfx's drawable-submit callback.
// Keeping the exact drawable/model-record pair beside every draw proves that
// the payload came from the game's crowd system rather than a shader guess.
struct CrowdOwnerSnapshot {
  uint32_t crowd = 0;
  uint32_t vtable = 0;
  uint32_t visible_instance_count = 0;
  uint32_t crowd_resource = 0;
  uint32_t crowd_state = 0;
  std::array<uint32_t, 2> drawables{};
  std::array<uint32_t, 2> models{};
  uint32_t submitted_drawable = 0;
  uint32_t submitted_model = 0;
  uint32_t guest_read_failures = 0;
  bool valid = false;
};

struct CrowdVertexFetchSnapshot {
  uint32_t slot = 0;
  uint32_t physical_address = 0;
  uint32_t size = 0;
  uint32_t endian = 0;
  std::array<uint32_t, 2> words{};
  bool valid = false;
};

// The 36-byte vf95 stream is intentionally retained in guest byte order.
// Later serving code can decode every packed attribute against the original
// Xenos shader without losing information.
struct CrowdVertexPayload {
  CrowdVertexFetchSnapshot fetch{};
  uint32_t source_virtual_alias = 0;
  uint32_t stride = 0;
  uint32_t vertex_count = 0;
  uint64_t payload_fingerprint = 0;
  bool has_nonzero_data = false;
  std::vector<uint8_t> raw_bytes;

  bool valid() const {
    return fetch.valid && source_virtual_alias != 0 && stride == 36 &&
           vertex_count != 0 && has_nonzero_data &&
           raw_bytes.size() == static_cast<size_t>(vertex_count) * stride;
  }
};

struct CrowdIndexPayload {
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

struct CrowdPaletteRecord {
  std::array<float, 4> quaternion{};
  std::array<float, 3> translation{};
};

// vf92 is a 28-byte quaternion + translation palette selected by the packed
// byte in the primary vertex. It is a live GPU stream, not a guessed bone
// walk through unrelated title objects.
struct CrowdPalettePayload {
  CrowdVertexFetchSnapshot fetch{};
  uint32_t record_count = 0;
  uint64_t payload_fingerprint = 0;
  bool has_nonzero_data = false;
  std::vector<uint8_t> raw_bytes;
  std::vector<CrowdPaletteRecord> records;

  bool valid() const {
    return fetch.valid && record_count != 0 && has_nonzero_data &&
           raw_bytes.size() == static_cast<size_t>(record_count) * 28 &&
           records.size() == record_count;
  }
};

// Raw mip-zero payload for the title's layered crowd atlas. Live gameplay
// uses a true tiled 3D DXT1 volume; retaining the dimension explicitly keeps
// volume and stacked-2D layouts from being conflated again.
struct CrowdTextureArrayPayload {
  std::array<uint32_t, 6> fetch_words{};
  uint32_t physical_address = 0;
  uint32_t byte_size = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t layers = 0;
  uint32_t dimension = 0;
  uint32_t format = 0;
  uint32_t endianness = 0;
  uint32_t pitch_blocks = 0;
  uint32_t fetch_swizzle = 0;
  uint64_t payload_fingerprint = 0;
  bool tiled = false;
  bool stacked = false;
  bool volume = false;
  std::vector<uint8_t> raw_bytes;

  bool valid() const {
    return physical_address != 0 && byte_size != 0 && width != 0 &&
           height != 0 && layers != 0 && (stacked != volume) &&
           raw_bytes.size() == byte_size;
  }
};

struct CrowdMaterialSnapshot {
  VertexDeclarationProbe declaration{};
  std::array<uint32_t, 6> texture_fetch{};
  std::shared_ptr<const CrowdTextureArrayPayload> texture;
  std::array<float, 16> instance_transform{};       // c32-c35
  std::array<float, 16> view_projection{};          // c36-c39
  std::array<float, 24> lighting_spheres{};         // c136-c141
  std::array<float, 4> ambient{};                   // c145
  std::array<float, 4> decode_constants{};          // c255
  bool decode_constants_verified = false;
  bool valid = false;
};

struct CrowdDrawSnapshot {
  CrowdOwnerSnapshot owner{};
  uint32_t ordinal = 0;
  uint32_t shader = 0;
  uint32_t model = 0;
  uint32_t geometry_index = 0;
  uint32_t pass_descriptor = 0;
  uint32_t program_pair = 0;
  uint32_t vertex_shader = 0;
  uint32_t pixel_shader = 0;
  uint32_t primitive_type = 0;
  uint32_t submitted_index_count = 0;
  CrowdVertexFetchSnapshot vertex_fetch{};
  CrowdVertexFetchSnapshot palette_fetch{};
  std::shared_ptr<const CrowdVertexPayload> vertices;
  std::shared_ptr<const CrowdIndexPayload> indices;
  std::shared_ptr<const CrowdPalettePayload> palette;
  CrowdMaterialSnapshot material{};
  uint32_t copy_failures = 0;
  bool valid = false;
};

// Capture one draw while all guest pointers are synchronously live below
// fxCrowdGfx's proven drawable/model-record submit pair. This never mutates or
// suppresses title rendering.
CrowdDrawSnapshot CaptureCrowdDrawSnapshot(
    uint8_t* guest_base, const SceneCatalogDrawOccurrence& draw,
    const CrowdOwnerSnapshot& owner);

// Dynamic palette streams may be rewritten every frame. Static geometry and
// texture payloads remain cached by their complete binding identity.
void CrowdSnapshotFrameEnd();

}  // namespace tabletennis::native
