#pragma once

#include "native/tabletennis_player_skin_observer.h"
#include "native/tabletennis_texture_snapshot.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace tabletennis::native {

struct SceneCatalogDrawOccurrence;

// Complete immutable vf95 payload for one verified 44-byte CA9 player stream.
// The bytes stay in guest byte order so later vertex-decoder work can be
// checked directly against the trace without losing packed attributes.
struct PlayerSkinVertexPayload {
  PlayerSkinVertexFetchObservation fetch{};
  uint32_t source_virtual_alias = 0;
  uint32_t stride = 0;
  uint32_t vertex_count = 0;
  uint64_t payload_fingerprint = 0;
  std::vector<uint8_t> raw_bytes;

  bool valid() const {
    return fetch.valid && source_virtual_alias != 0 && stride != 0 &&
           vertex_count != 0 &&
           raw_bytes.size() == static_cast<size_t>(vertex_count) * stride;
  }
};

// Exact index payload submitted by the current indexed draw. Raw guest bytes
// are retained beside a host-endian decode so both replay and verification can
// use the same immutable capture.
struct PlayerSkinIndexPayload {
  uint32_t source_virtual_alias = 0;
  uint32_t physical_address = 0;
  uint32_t element_size = 0;
  uint32_t submitted_index_count = 0;
  uint64_t payload_fingerprint = 0;
  std::vector<uint8_t> raw_bytes;
  std::vector<uint32_t> indices;

  bool valid() const {
    return source_virtual_alias != 0 && physical_address != 0 &&
           (element_size == 2 || element_size == 4) &&
           submitted_index_count != 0 &&
           raw_bytes.size() ==
               static_cast<size_t>(submitted_index_count) * element_size &&
           indices.size() == submitted_index_count;
  }
};

struct PlayerSkinPaletteRecord {
  std::array<float, 4> quaternion{};
  std::array<float, 3> translation{};
};

// Complete vf92 palette captured once per unique live palette per frame.
struct PlayerSkinPalettePayload {
  PlayerSkinVertexFetchObservation fetch{};
  uint32_t record_count = 0;
  uint64_t payload_fingerprint = 0;
  std::vector<uint8_t> raw_bytes;
  std::vector<PlayerSkinPaletteRecord> records;

  bool valid() const {
    return fetch.valid && record_count != 0 &&
           raw_bytes.size() == static_cast<size_t>(record_count) * 28 &&
           records.size() == record_count;
  }
};

struct PlayerSkinMaterialSnapshot {
  std::array<std::array<uint32_t, 6>, 3> texture_fetches{};
  std::array<uint32_t, 3> texture_view_swizzles{};
  std::array<std::shared_ptr<const TextureSnapshot>, 3> textures{};
  std::array<float, 16> vertex_constants_12_15{};
  std::array<float, 4> vertex_constant_19{};
  std::array<float, 36> vertex_constants_46_54{};
  std::array<float, 4> vertex_constant_255{};
  std::array<float, 92> pixel_constants_46_68{};
  std::array<float, 4> pixel_constant_254{};
  std::array<float, 4> pixel_constant_255{};
  bool pixel_control_constants_valid = false;
  bool texture_view_swizzles_valid = false;
  bool valid = false;
};

struct PlayerSkinPayloadCapture {
  std::shared_ptr<const PlayerSkinVertexPayload> vertices;
  std::shared_ptr<const PlayerSkinIndexPayload> indices;
  std::shared_ptr<const PlayerSkinPalettePayload> palette;
  std::array<std::shared_ptr<const TextureSnapshot>, 3> textures{};
  uint32_t copy_failures = 0;

  bool valid() const {
    if (copy_failures != 0 || vertices == nullptr || !vertices->valid() ||
        indices == nullptr || !indices->valid() || palette == nullptr ||
        !palette->valid()) {
      return false;
    }
    for (const auto &texture : textures) {
      if (texture == nullptr || !texture->valid()) {
        return false;
      }
    }
    return true;
  }
};

struct PlayerSkinDrawSnapshot {
  std::shared_ptr<const PlayerSkinVertexPayload> vertices;
  std::shared_ptr<const PlayerSkinIndexPayload> indices;
  std::shared_ptr<const PlayerSkinPalettePayload> palette;
  PlayerSkinMaterialSnapshot material{};
  uint32_t ordinal = 0;
  uint32_t player = 0;
  uint32_t shader = 0;
  uint32_t model = 0;
  uint32_t geometry_index = 0;
  uint32_t pass_descriptor = 0;
  uint32_t program_pair = 0;
  uint32_t vertex_shader = 0;
  uint32_t pixel_shader = 0;
  uint32_t primitive_type = 0;
  uint32_t submitted_index_count = 0;
  bool alternate_pass = false;
  bool valid = false;
};

// Strong immutable mesh identity shared by observer selection and future
// in-order replacement tokens. Title model/geometry scope fields are not
// sufficient for CA9: both may be zero across an entire player block.
struct PlayerSkinMeshIdentity {
  uint32_t player = 0;
  uint32_t vertex_physical_address = 0;
  uint32_t vertex_size = 0;
  uint64_t vertex_fingerprint = 0;
  uint32_t index_physical_address = 0;
  uint32_t index_count = 0;
  uint32_t index_element_size = 0;
  uint64_t index_fingerprint = 0;

  bool valid() const {
    return player != 0 && vertex_physical_address != 0 && vertex_size != 0 &&
           index_physical_address != 0 && index_count != 0 &&
           (index_element_size == 2 || index_element_size == 4);
  }

  bool operator==(const PlayerSkinMeshIdentity &) const = default;
};

struct PlayerSkinFrameSnapshot {
  uint64_t sequence = 0;
  uint32_t admitted_draw_count = 0;
  uint32_t dropped_draw_count = 0;
  uint32_t copy_failures = 0;
  std::vector<PlayerSkinDrawSnapshot> draws;

  bool valid() const {
    if (sequence == 0 || draws.empty() || dropped_draw_count != 0 ||
        copy_failures != 0 || admitted_draw_count != draws.size()) {
      return false;
    }
    for (const PlayerSkinDrawSnapshot &draw : draws) {
      if (!draw.valid) {
        return false;
      }
    }
    return true;
  }
};

PlayerSkinMeshIdentity
PlayerSkinMeshIdentityForDraw(const PlayerSkinDrawSnapshot &draw);

// Only the six pass descriptors from the trace-verified visible CA9 player
// block are admissible. This is intentionally narrower than shader identity.
bool IsVerifiedPlayerSkinPassDescriptor(uint32_t pass_descriptor);

// Capture immutable resources for a draw whose scalar fetch/material contract
// has already been validated. Capture remains observer-only.
PlayerSkinPayloadCapture
CapturePlayerSkinPayloads(uint8_t *guest_base,
                          const SceneCatalogDrawOccurrence &draw,
                          const PlayerSkinDrawObservation &observation);

// Publish one fully validated draw into the current observer frame.
void ObservePlayerSkinSnapshot(const SceneCatalogDrawOccurrence &draw,
                               const PlayerSkinDrawObservation &observation,
                               const PlayerSkinPayloadCapture &payloads);
void PlayerSkinSnapshotFrameEnd();

std::shared_ptr<const PlayerSkinFrameSnapshot> LatestPlayerSkinFrameSnapshot();

} // namespace tabletennis::native
