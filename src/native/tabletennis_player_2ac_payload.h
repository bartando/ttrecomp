#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace tabletennis::native {

struct SceneCatalogDrawOccurrence;

// Value-only description of a live Xenos vertex fetch. The capture module
// receives this only after the 2AC observer has admitted the actual low-level
// bound pixel shader and validated the fetch descriptor.
struct Player2ACPayloadFetch {
  uint32_t physical_address = 0;
  uint32_t byte_count = 0;
  uint32_t endian = 0;
  uint32_t stride = 0;
  bool valid = false;
};

// Complete immutable vf95 payload. Bytes remain in guest order so a native
// decoder can be checked directly against the title and trace.
struct Player2ACVertexPayload {
  Player2ACPayloadFetch fetch{};
  uint32_t source_virtual_alias = 0;
  uint32_t vertex_count = 0;
  uint64_t payload_fingerprint = 0;
  std::vector<uint8_t> raw_bytes;

  bool valid() const {
    return fetch.valid && source_virtual_alias != 0 &&
           (fetch.stride == 32 || fetch.stride == 36 || fetch.stride == 44 ||
            fetch.stride == 96) &&
           fetch.endian == 2 && fetch.byte_count != 0 && vertex_count != 0 &&
           raw_bytes.size() == fetch.byte_count &&
           raw_bytes.size() ==
               static_cast<size_t>(vertex_count) * fetch.stride &&
           payload_fingerprint != 0;
  }
};

// Exact submitted 16-bit index range, retained both in guest byte order and
// host-endian form. Capture rejects any index outside the current vf95 range.
struct Player2ACIndexPayload {
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
           minimum_index <= maximum_index && payload_fingerprint != 0;
  }
};

struct Player2ACPaletteRecord {
  std::array<float, 4> quaternion{};
  std::array<float, 3> translation{};
};

// Complete immutable vf92 payload. Palettes are dynamic and therefore cached
// only within the title frame that owns them.
struct Player2ACPalettePayload {
  Player2ACPayloadFetch fetch{};
  uint64_t frame_sequence = 0;
  uint32_t player = 0;
  uint32_t record_count = 0;
  uint64_t payload_fingerprint = 0;
  std::vector<uint8_t> raw_bytes;
  std::vector<Player2ACPaletteRecord> records;

  bool valid() const {
    return fetch.valid && fetch.stride == 28 && fetch.endian == 2 &&
           fetch.byte_count != 0 && frame_sequence != 0 && record_count != 0 &&
           raw_bytes.size() == fetch.byte_count &&
           raw_bytes.size() == static_cast<size_t>(record_count) * 28 &&
           records.size() == record_count && payload_fingerprint != 0;
  }
};

// Exact live constant-bank rows consumed by the four traced 2AC vertex
// programs and the shared 2AC pixel program. Rows remain in Xenos register
// order. Keeping the full contiguous spans avoids silently manufacturing
// values for registers that are selected dynamically by the shaders.
struct Player2ACDrawConstants {
  std::array<float, 84> vertex_constants_0_20{};
  std::array<float, 44> vertex_constants_37_47{};
  std::array<float, 8> vertex_constants_254_255{};
  std::array<float, 4> pixel_constant_37{};
  std::array<float, 12> pixel_constants_253_255{};
  bool valid = false;
};

// Renderable-data-ready resources for one title draw. This carries no backend
// route and cannot suppress or replace guest rendering.
struct Player2ACDrawPayload {
  std::shared_ptr<const Player2ACVertexPayload> vertices;
  std::shared_ptr<const Player2ACIndexPayload> indices;
  std::shared_ptr<const Player2ACPalettePayload> palette;
  Player2ACDrawConstants constants{};
  bool palette_required = false;

  bool valid() const {
    return vertices != nullptr && vertices->valid() && indices != nullptr &&
           indices->valid() &&
           indices->maximum_index < vertices->vertex_count && constants.valid &&
           (palette_required ? palette != nullptr && palette->valid()
                             : palette == nullptr);
  }
};

struct Player2ACPayloadCapture {
  std::shared_ptr<const Player2ACDrawPayload> payload;
  uint32_t copy_failures = 0;

  bool valid() const {
    return copy_failures == 0 && payload != nullptr && payload->valid();
  }
};

// Captures payloads only after the caller has admitted the actual bound 2AC
// pixel shader and exact scalar draw/fetch contract.
Player2ACPayloadCapture CapturePlayer2ACDrawPayload(
    uint8_t *guest_base, const SceneCatalogDrawOccurrence &draw,
    const Player2ACPayloadFetch &vertices, const Player2ACPayloadFetch &palette,
    bool palette_required);

// Dynamic vf92 payloads must never leak across title frames. Immutable vf95
// and index payloads remain in bounded reusable caches.
void Player2ACPayloadFrameEnd();

} // namespace tabletennis::native
