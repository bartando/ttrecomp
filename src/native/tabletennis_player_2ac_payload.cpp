#include "native/tabletennis_player_2ac_payload.h"

#include "generated/default/tabletennis_init.h"
#include "native/tabletennis_guest_memory.h"
#include "native/tabletennis_scene_draw_catalog.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <ranges>

namespace tabletennis::native {
namespace {

constexpr uint32_t kVertexEndian8In32 = 2;
constexpr uint32_t kPaletteRecordStride = 28;
constexpr uint32_t kVertexConstantBankOffset = 0x780;
constexpr uint32_t kPixelConstantBankOffset = 0x1780;
constexpr size_t kConstantRowBytes = sizeof(float) * 4;
constexpr uint32_t kPhysicalAliasBase = 0xA0000000;
constexpr uint32_t kPhysicalAddressMask = 0x1FFFFFFF;
constexpr uint64_t kPhysicalAddressSpaceSize = uint64_t{1} << 29;
constexpr uint32_t kHighPhysicalHeapBase = 0xE0000000;
constexpr uint32_t kHighPhysicalHeapHostPageOffset = 0x1000;
constexpr size_t kMaximumVertexPayloadBytes = 64 * 1024 * 1024;
constexpr size_t kMaximumIndexPayloadBytes = 16 * 1024 * 1024;
constexpr size_t kMaximumPalettePayloadBytes = 64 * 1024;
constexpr size_t kMaximumCachedVertices = 256;
constexpr size_t kMaximumCachedIndices = 512;
constexpr size_t kMaximumVertexCacheBytes = 128 * 1024 * 1024;
constexpr size_t kMaximumIndexCacheBytes = 64 * 1024 * 1024;
constexpr size_t kMaximumOutstandingPayloadBytes = 512 * 1024 * 1024;
constexpr size_t kMaximumFramePalettes = 16;
constexpr uint64_t kFnvOffsetBasis = 1469598103934665603ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;

struct VertexCacheKey {
  uint32_t physical_address = 0;
  uint32_t byte_count = 0;
  uint32_t endian = 0;
  uint32_t virtual_alias = 0;
  uint32_t stride = 0;
  uint32_t vertex_aggregate = 0;
  uint32_t vertex_buffer_resource = 0;
  uint32_t primary_vertex_stream = 0;

  bool operator==(const VertexCacheKey &) const = default;
};

struct IndexCacheKey {
  uint32_t physical_address = 0;
  uint32_t virtual_alias = 0;
  uint32_t submitted_index_count = 0;
  uint32_t vertex_count = 0;
  uint32_t index_buffer_wrapper = 0;
  uint32_t index_buffer_resource = 0;
  uint32_t index_data = 0;

  bool operator==(const IndexCacheKey &) const = default;
};

struct PaletteFrameKey {
  uint64_t frame_sequence = 0;
  uint32_t player = 0;
  uint32_t physical_address = 0;
  uint32_t byte_count = 0;
  uint32_t endian = 0;

  bool operator==(const PaletteFrameKey &) const = default;
};

template <typename Key, typename Payload> struct CachedPayload {
  Key key{};
  std::shared_ptr<const Payload> payload;
  size_t byte_count = 0;
  uint64_t last_validated_frame = 0;
};

std::mutex g_cache_mutex;
std::deque<CachedPayload<VertexCacheKey, Player2ACVertexPayload>>
    g_vertex_cache;
std::deque<CachedPayload<IndexCacheKey, Player2ACIndexPayload>> g_index_cache;
std::vector<CachedPayload<PaletteFrameKey, Player2ACPalettePayload>>
    g_frame_palette_cache;
struct OutstandingPayload {
  std::weak_ptr<const void> payload;
  size_t byte_count = 0;
};
std::vector<OutstandingPayload> g_outstanding_payloads;
size_t g_vertex_cache_bytes = 0;
size_t g_index_cache_bytes = 0;

uint32_t PhysicalAddressForVirtualAlias(uint32_t virtual_address) {
  if (virtual_address < kHighPhysicalHeapBase) {
    return 0;
  }
  const uint64_t physical_address =
      static_cast<uint64_t>(virtual_address - kHighPhysicalHeapBase) +
      kHighPhysicalHeapHostPageOffset;
  return physical_address <= kPhysicalAddressMask
             ? static_cast<uint32_t>(physical_address)
             : 0;
}

bool CheckedGuestOffset(uint32_t address, size_t size, uint32_t &result) {
  if (address == 0 || size == 0) {
    return false;
  }
  const uint64_t end = static_cast<uint64_t>(address) + size;
  if (end > (uint64_t{1} << 32)) {
    return false;
  }
  result = address;
  return true;
}

bool TryCopyGuestPhysical(uint8_t *guest_base, uint32_t physical_address,
                          void *destination, size_t size) {
  if (guest_base == nullptr || destination == nullptr ||
      physical_address == 0 || physical_address > kPhysicalAddressMask ||
      size == 0 || size > kPhysicalAddressSpaceSize - physical_address) {
    return false;
  }
  uint32_t guest_address = 0;
  const uint32_t alias = kPhysicalAliasBase | physical_address;
  return CheckedGuestOffset(alias, size, guest_address) &&
         GuestTryCopy(destination,
                      guest_base + guest_address +
                          REX_PHYS_HOST_OFFSET(guest_address),
                      size);
}

bool CaptureStablePhysicalBytes(uint8_t *guest_base, uint32_t physical_address,
                                size_t size, std::vector<uint8_t> &bytes) {
  if (size == 0) {
    return false;
  }
  bytes.resize(size);
  std::vector<uint8_t> verification(size);
  for (uint32_t attempt = 0; attempt < 4; ++attempt) {
    if (!TryCopyGuestPhysical(guest_base, physical_address, bytes.data(),
                              bytes.size()) ||
        !TryCopyGuestPhysical(guest_base, physical_address, verification.data(),
                              verification.size())) {
      bytes.clear();
      return false;
    }
    if (bytes == verification) {
      return true;
    }
  }
  bytes.clear();
  return false;
}

uint16_t LoadBeU16(const uint8_t *source) {
  uint16_t value = 0;
  std::memcpy(&value, source, sizeof(value));
  return std::byteswap(value);
}

uint32_t LoadBeU32(const uint8_t *source) {
  uint32_t value = 0;
  std::memcpy(&value, source, sizeof(value));
  return std::byteswap(value);
}

float LoadBeF32(const uint8_t *source) {
  return std::bit_cast<float>(LoadBeU32(source));
}

bool TryCopyGuestVirtual(uint8_t *guest_base, uint32_t address, size_t offset,
                         void *destination, size_t size) {
  if (guest_base == nullptr || address == 0 || destination == nullptr ||
      size == 0) {
    return false;
  }
  const uint64_t start = static_cast<uint64_t>(address) + offset;
  const uint64_t end = start + size;
  if (start > std::numeric_limits<uint32_t>::max() ||
      end > (uint64_t{1} << 32)) {
    return false;
  }
  const uint32_t guest_address = static_cast<uint32_t>(start);
  return GuestTryCopy(
      destination,
      guest_base + guest_address + REX_PHYS_HOST_OFFSET(guest_address), size);
}

template <size_t Size>
bool CaptureBeFloats(uint8_t *guest_base, uint32_t address, size_t offset,
                     std::array<float, Size> &destination) {
  std::array<uint8_t, Size * sizeof(float)> bytes{};
  if (!TryCopyGuestVirtual(guest_base, address, offset, bytes.data(),
                           bytes.size())) {
    return false;
  }
  for (size_t index = 0; index < Size; ++index) {
    destination[index] = LoadBeF32(bytes.data() + index * sizeof(uint32_t));
  }
  return std::ranges::all_of(destination,
                             [](float value) { return std::isfinite(value); });
}

Player2ACDrawConstants
CaptureDrawConstants(uint8_t *guest_base,
                     const SceneCatalogDrawOccurrence &draw) {
  Player2ACDrawConstants constants;
  if (draw.device == 0) {
    return constants;
  }
  const bool captured =
      CaptureBeFloats(guest_base, draw.device,
                      kVertexConstantBankOffset + 0 * kConstantRowBytes,
                      constants.vertex_constants_0_20) &&
      CaptureBeFloats(guest_base, draw.device,
                      kVertexConstantBankOffset + 37 * kConstantRowBytes,
                      constants.vertex_constants_37_47) &&
      CaptureBeFloats(guest_base, draw.device,
                      kVertexConstantBankOffset + 254 * kConstantRowBytes,
                      constants.vertex_constants_254_255) &&
      CaptureBeFloats(guest_base, draw.device,
                      kPixelConstantBankOffset + 37 * kConstantRowBytes,
                      constants.pixel_constant_37) &&
      CaptureBeFloats(guest_base, draw.device,
                      kPixelConstantBankOffset + 253 * kConstantRowBytes,
                      constants.pixel_constants_253_255);
  constants.valid = captured;
  return constants;
}

uint64_t Fingerprint(const std::vector<uint8_t> &bytes) {
  uint64_t hash = kFnvOffsetBasis;
  for (const uint8_t value : bytes) {
    hash = (hash ^ value) * kFnvPrime;
  }
  return hash;
}

template <typename Key, typename Payload, typename Container>
std::shared_ptr<const Payload> FindCached(const Container &cache,
                                          const Key &key) {
  const auto found = std::ranges::find_if(
      cache, [&](const auto &entry) { return entry.key == key; });
  return found == cache.end() ? nullptr : found->payload;
}

template <typename Payload>
bool TrackOutstandingPayloadLocked(const std::shared_ptr<Payload> &payload,
                                   size_t byte_count) {
  size_t outstanding_bytes = 0;
  std::erase_if(g_outstanding_payloads, [&](const OutstandingPayload &entry) {
    if (entry.payload.expired()) {
      return true;
    }
    outstanding_bytes += entry.byte_count;
    return false;
  });
  if (byte_count >
      kMaximumOutstandingPayloadBytes -
          std::min(outstanding_bytes, kMaximumOutstandingPayloadBytes)) {
    return false;
  }
  g_outstanding_payloads.push_back(
      {std::weak_ptr<const void>(payload), byte_count});
  return true;
}

template <typename Entry>
void MakeCacheRoom(std::deque<Entry> &cache, size_t &cached_bytes,
                   size_t maximum_entries, size_t maximum_bytes,
                   size_t incoming_bytes) {
  while (!cache.empty() &&
         (cache.size() >= maximum_entries ||
          incoming_bytes >
              maximum_bytes - std::min(cached_bytes, maximum_bytes))) {
    cached_bytes -= std::min(cached_bytes, cache.front().byte_count);
    cache.pop_front();
  }
}

std::shared_ptr<const Player2ACVertexPayload>
CaptureVertices(uint8_t *guest_base, const SceneCatalogDrawOccurrence &draw,
                const Player2ACPayloadFetch &fetch) {
  const uint32_t physical_alias =
      PhysicalAddressForVirtualAlias(draw.mesh.vertex_buffer_alias);
  if (!fetch.valid || fetch.endian != kVertexEndian8In32 ||
      (fetch.stride != 32 && fetch.stride != 36 && fetch.stride != 44 &&
       fetch.stride != 96) ||
      fetch.stride != draw.mesh.vertex_stride || fetch.byte_count == 0 ||
      fetch.byte_count > kMaximumVertexPayloadBytes ||
      fetch.byte_count % fetch.stride != 0 ||
      fetch.physical_address != physical_alias) {
    return nullptr;
  }
  const VertexCacheKey key = {
      .physical_address = fetch.physical_address,
      .byte_count = fetch.byte_count,
      .endian = fetch.endian,
      .virtual_alias = draw.mesh.vertex_buffer_alias,
      .stride = fetch.stride,
      .vertex_aggregate = draw.mesh.vertex_aggregate,
      .vertex_buffer_resource = draw.mesh.vertex_buffer_resource,
      .primary_vertex_stream = draw.mesh.primary_vertex_stream,
  };
  {
    std::lock_guard lock(g_cache_mutex);
    const auto cached = std::ranges::find_if(
        g_vertex_cache, [&](const auto &entry) { return entry.key == key; });
    if (cached != g_vertex_cache.end() &&
        cached->last_validated_frame == draw.frame_sequence) {
      return cached->payload;
    }
  }

  auto payload = std::make_shared<Player2ACVertexPayload>();
  payload->fetch = fetch;
  payload->source_virtual_alias = draw.mesh.vertex_buffer_alias;
  payload->vertex_count = fetch.byte_count / fetch.stride;
  if (!CaptureStablePhysicalBytes(guest_base, fetch.physical_address,
                                  fetch.byte_count, payload->raw_bytes)) {
    return nullptr;
  }
  payload->payload_fingerprint = Fingerprint(payload->raw_bytes);
  if (!payload->valid()) {
    return nullptr;
  }

  std::lock_guard lock(g_cache_mutex);
  const auto cached = std::ranges::find_if(
      g_vertex_cache, [&](const auto &entry) { return entry.key == key; });
  if (cached != g_vertex_cache.end()) {
    if (cached->payload->raw_bytes == payload->raw_bytes) {
      cached->last_validated_frame = draw.frame_sequence;
      return cached->payload;
    }
    g_vertex_cache_bytes -= std::min(g_vertex_cache_bytes, cached->byte_count);
    g_vertex_cache.erase(cached);
  }
  MakeCacheRoom(g_vertex_cache, g_vertex_cache_bytes, kMaximumCachedVertices,
                kMaximumVertexCacheBytes, payload->raw_bytes.size());
  if (payload->raw_bytes.size() > kMaximumVertexCacheBytes ||
      !TrackOutstandingPayloadLocked(payload, payload->raw_bytes.size())) {
    return nullptr;
  }
  g_vertex_cache_bytes += payload->raw_bytes.size();
  g_vertex_cache.push_back(
      {key, payload, payload->raw_bytes.size(), draw.frame_sequence});
  return payload;
}

std::shared_ptr<const Player2ACIndexPayload>
CaptureIndices(uint8_t *guest_base, const SceneCatalogDrawOccurrence &draw,
               uint32_t vertex_count) {
  const uint64_t requested_size =
      static_cast<uint64_t>(draw.submitted_index_count) * sizeof(uint16_t);
  const uint32_t physical_address =
      PhysicalAddressForVirtualAlias(draw.mesh.index_buffer_alias);
  if (!draw.mesh.valid || physical_address == 0 || vertex_count == 0 ||
      draw.submitted_index_count == 0 ||
      draw.mesh.index_element_size != sizeof(uint16_t) ||
      draw.mesh.index_is_32_bit || requested_size == 0 ||
      requested_size > kMaximumIndexPayloadBytes ||
      requested_size > draw.mesh.index_buffer_bytes) {
    return nullptr;
  }
  const IndexCacheKey key = {
      .physical_address = physical_address,
      .virtual_alias = draw.mesh.index_buffer_alias,
      .submitted_index_count = draw.submitted_index_count,
      .vertex_count = vertex_count,
      .index_buffer_wrapper = draw.mesh.index_buffer_wrapper,
      .index_buffer_resource = draw.mesh.index_buffer_resource,
      .index_data = draw.mesh.index_data,
  };
  {
    std::lock_guard lock(g_cache_mutex);
    const auto cached = std::ranges::find_if(
        g_index_cache, [&](const auto &entry) { return entry.key == key; });
    if (cached != g_index_cache.end() &&
        cached->last_validated_frame == draw.frame_sequence) {
      return cached->payload;
    }
  }

  auto payload = std::make_shared<Player2ACIndexPayload>();
  payload->source_virtual_alias = draw.mesh.index_buffer_alias;
  payload->physical_address = physical_address;
  payload->submitted_index_count = draw.submitted_index_count;
  if (!CaptureStablePhysicalBytes(guest_base, physical_address,
                                  static_cast<size_t>(requested_size),
                                  payload->raw_bytes)) {
    return nullptr;
  }
  payload->payload_fingerprint = Fingerprint(payload->raw_bytes);
  payload->indices.resize(draw.submitted_index_count);
  payload->minimum_index = std::numeric_limits<uint16_t>::max();
  for (uint32_t index = 0; index < draw.submitted_index_count; ++index) {
    const uint16_t decoded =
        LoadBeU16(payload->raw_bytes.data() +
                  static_cast<size_t>(index) * sizeof(uint16_t));
    if (decoded >= vertex_count) {
      return nullptr;
    }
    payload->indices[index] = decoded;
    payload->minimum_index = std::min(payload->minimum_index, decoded);
    payload->maximum_index = std::max(payload->maximum_index, decoded);
  }
  if (!payload->valid()) {
    return nullptr;
  }

  std::lock_guard lock(g_cache_mutex);
  const auto cached = std::ranges::find_if(
      g_index_cache, [&](const auto &entry) { return entry.key == key; });
  if (cached != g_index_cache.end()) {
    if (cached->payload->raw_bytes == payload->raw_bytes) {
      cached->last_validated_frame = draw.frame_sequence;
      return cached->payload;
    }
    g_index_cache_bytes -= std::min(g_index_cache_bytes, cached->byte_count);
    g_index_cache.erase(cached);
  }
  const size_t retained_bytes =
      payload->raw_bytes.size() +
      payload->indices.size() * sizeof(payload->indices.front());
  MakeCacheRoom(g_index_cache, g_index_cache_bytes, kMaximumCachedIndices,
                kMaximumIndexCacheBytes, payload->raw_bytes.size());
  if (payload->raw_bytes.size() > kMaximumIndexCacheBytes ||
      !TrackOutstandingPayloadLocked(payload, retained_bytes)) {
    return nullptr;
  }
  g_index_cache_bytes += payload->raw_bytes.size();
  g_index_cache.push_back(
      {key, payload, payload->raw_bytes.size(), draw.frame_sequence});
  return payload;
}

std::shared_ptr<const Player2ACPalettePayload>
CapturePalette(uint8_t *guest_base, const SceneCatalogDrawOccurrence &draw,
               const Player2ACPayloadFetch &fetch) {
  if (draw.frame_sequence == 0 || !fetch.valid ||
      fetch.endian != kVertexEndian8In32 ||
      fetch.stride != kPaletteRecordStride || fetch.byte_count == 0 ||
      fetch.byte_count > kMaximumPalettePayloadBytes ||
      fetch.byte_count % kPaletteRecordStride != 0) {
    return nullptr;
  }
  const PaletteFrameKey key = {
      .frame_sequence = draw.frame_sequence,
      .player = draw.player,
      .physical_address = fetch.physical_address,
      .byte_count = fetch.byte_count,
      .endian = fetch.endian,
  };
  {
    std::lock_guard lock(g_cache_mutex);
    if (const auto cached =
            FindCached<PaletteFrameKey, Player2ACPalettePayload>(
                g_frame_palette_cache, key)) {
      return cached;
    }
  }

  auto payload = std::make_shared<Player2ACPalettePayload>();
  payload->fetch = fetch;
  payload->frame_sequence = draw.frame_sequence;
  payload->player = draw.player;
  payload->record_count = fetch.byte_count / kPaletteRecordStride;
  if (!CaptureStablePhysicalBytes(guest_base, fetch.physical_address,
                                  fetch.byte_count, payload->raw_bytes)) {
    return nullptr;
  }
  payload->payload_fingerprint = Fingerprint(payload->raw_bytes);
  payload->records.resize(payload->record_count);
  for (uint32_t record = 0; record < payload->record_count; ++record) {
    const uint8_t *source = payload->raw_bytes.data() +
                            static_cast<size_t>(record) * kPaletteRecordStride;
    Player2ACPaletteRecord &destination = payload->records[record];
    for (size_t component = 0; component < destination.quaternion.size();
         ++component) {
      destination.quaternion[component] =
          LoadBeF32(source + component * sizeof(float));
    }
    for (size_t component = 0; component < destination.translation.size();
         ++component) {
      destination.translation[component] =
          LoadBeF32(source + 16 + component * sizeof(float));
    }
    if (!std::ranges::all_of(
            destination.quaternion,
            [](float value) { return std::isfinite(value); }) ||
        !std::ranges::all_of(destination.translation, [](float value) {
          return std::isfinite(value);
        })) {
      return nullptr;
    }
  }
  if (!payload->valid()) {
    return nullptr;
  }

  std::lock_guard lock(g_cache_mutex);
  if (const auto cached = FindCached<PaletteFrameKey, Player2ACPalettePayload>(
          g_frame_palette_cache, key)) {
    return cached;
  }
  if (g_frame_palette_cache.size() >= kMaximumFramePalettes) {
    return nullptr;
  }
  const size_t retained_bytes =
      payload->raw_bytes.size() +
      payload->records.size() * sizeof(payload->records.front());
  if (!TrackOutstandingPayloadLocked(payload, retained_bytes)) {
    return nullptr;
  }
  g_frame_palette_cache.push_back({key, payload, payload->raw_bytes.size()});
  return payload;
}

} // namespace

Player2ACPayloadCapture CapturePlayer2ACDrawPayload(
    uint8_t *guest_base, const SceneCatalogDrawOccurrence &draw,
    const Player2ACPayloadFetch &vertices, const Player2ACPayloadFetch &palette,
    bool palette_required) {
  Player2ACPayloadCapture capture;
  if (guest_base == nullptr || !draw.mesh.valid || draw.frame_sequence == 0) {
    ++capture.copy_failures;
    return capture;
  }

  auto payload = std::make_shared<Player2ACDrawPayload>();
  payload->palette_required = palette_required;
  payload->constants = CaptureDrawConstants(guest_base, draw);
  capture.copy_failures += !payload->constants.valid;
  payload->vertices = CaptureVertices(guest_base, draw, vertices);
  capture.copy_failures += payload->vertices == nullptr;
  if (payload->vertices != nullptr) {
    payload->indices =
        CaptureIndices(guest_base, draw, payload->vertices->vertex_count);
    capture.copy_failures += payload->indices == nullptr;
  }
  if (palette_required) {
    payload->palette = CapturePalette(guest_base, draw, palette);
    capture.copy_failures += payload->palette == nullptr;
  }

  if (capture.copy_failures == 0 && payload->valid()) {
    capture.payload = std::move(payload);
  }
  return capture;
}

void Player2ACPayloadFrameEnd() {
  std::lock_guard lock(g_cache_mutex);
  g_frame_palette_cache.clear();
}

} // namespace tabletennis::native
