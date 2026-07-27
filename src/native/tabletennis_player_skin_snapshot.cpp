#include "native/tabletennis_player_skin_snapshot.h"

#include "generated/default/tabletennis_init.h"
#include "native/tabletennis_guest_memory.h"
#include "native/tabletennis_main_coverage_ledger.h"
#include "native/tabletennis_player_replacement_candidates.h"
#include "native/tabletennis_scene_draw_catalog.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <utility>

#include <rex/logging.h>

namespace tabletennis::native {
namespace {

constexpr std::array<uint32_t, 6> kVerifiedPassDescriptors = {
    0x40106638, 0x4010664C, 0x4010D638, 0x4010D64C, 0x401145B8, 0x401145CC,
};
constexpr uint32_t kPlayerVertexStride = 44;
constexpr uint32_t kPaletteRecordStride = 28;
constexpr uint32_t kVertexEndian8In32 = 2;
constexpr uint32_t kPhysicalAliasBase = 0xA0000000;
constexpr uint32_t kPhysicalAddressMask = 0x1FFFFFFF;
constexpr uint32_t kHighPhysicalHeapBase = 0xE0000000;
constexpr uint32_t kHighPhysicalHeapHostPageOffset = 0x1000;
constexpr size_t kMaximumVertexPayloadBytes = 64 * 1024 * 1024;
constexpr size_t kMaximumIndexPayloadBytes = 16 * 1024 * 1024;
constexpr size_t kMaximumPalettePayloadBytes = 64 * 1024;
constexpr size_t kMaximumFrameDraws = 256;
constexpr size_t kMaximumCachedVertices = 256;
constexpr size_t kMaximumCachedIndices = 512;
constexpr size_t kMaximumVertexCacheBytes = 128 * 1024 * 1024;
constexpr size_t kMaximumIndexCacheBytes = 64 * 1024 * 1024;
constexpr uint64_t kFnvOffsetBasis = 1469598103934665603ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;

struct VertexCacheKey {
  uint32_t physical_address = 0;
  uint32_t size = 0;
  uint32_t endian = 0;
  uint32_t virtual_alias = 0;
  uint32_t stride = 0;

  bool operator==(const VertexCacheKey &) const = default;
};

struct IndexCacheKey {
  uint32_t physical_address = 0;
  uint32_t virtual_alias = 0;
  uint32_t element_size = 0;
  uint32_t submitted_index_count = 0;

  bool operator==(const IndexCacheKey &) const = default;
};

struct PaletteFrameKey {
  uint32_t player = 0;
  uint32_t physical_address = 0;
  uint32_t size = 0;
  uint32_t endian = 0;

  bool operator==(const PaletteFrameKey &) const = default;
};

template <typename Key, typename Payload> struct CachedPayload {
  Key key{};
  std::shared_ptr<const Payload> payload;
};

std::mutex g_snapshot_mutex;
PlayerSkinFrameSnapshot g_building_frame;
std::shared_ptr<const PlayerSkinFrameSnapshot> g_published_frame;
std::vector<CachedPayload<VertexCacheKey, PlayerSkinVertexPayload>>
    g_vertex_cache;
std::vector<CachedPayload<IndexCacheKey, PlayerSkinIndexPayload>> g_index_cache;
std::vector<CachedPayload<PaletteFrameKey, PlayerSkinPalettePayload>>
    g_frame_palette_cache;
size_t g_vertex_cache_bytes = 0;
size_t g_index_cache_bytes = 0;
size_t g_largest_logged_draw_count = 0;

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

bool CheckedGuestOffset(uint32_t address, size_t offset, size_t size,
                        uint32_t &result) {
  if (address == 0) {
    return false;
  }
  const uint64_t start = static_cast<uint64_t>(address) + offset;
  const uint64_t end = start + size;
  if (start > std::numeric_limits<uint32_t>::max() ||
      end > (uint64_t{1} << 32)) {
    return false;
  }
  result = static_cast<uint32_t>(start);
  return true;
}

bool TryCopyGuestPhysical(uint8_t *guest_base, uint32_t physical_address,
                          void *destination, size_t size) {
  if (guest_base == nullptr || physical_address == 0 ||
      physical_address > kPhysicalAddressMask || destination == nullptr ||
      size == 0) {
    return false;
  }
  uint32_t guest_address = 0;
  const uint32_t physical_alias = kPhysicalAliasBase | physical_address;
  if (!CheckedGuestOffset(physical_alias, 0, size, guest_address)) {
    return false;
  }
  return GuestTryCopy(
      destination,
      guest_base + guest_address + REX_PHYS_HOST_OFFSET(guest_address), size);
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

uint64_t Fingerprint(const std::vector<uint8_t> &bytes) {
  uint64_t hash = kFnvOffsetBasis;
  for (uint8_t value : bytes) {
    hash = (hash ^ value) * kFnvPrime;
  }
  return hash;
}

template <typename Key, typename Payload>
std::shared_ptr<const Payload>
FindCached(const std::vector<CachedPayload<Key, Payload>> &cache,
           const Key &key) {
  const auto found =
      std::find_if(cache.begin(), cache.end(),
                   [&](const auto &entry) { return entry.key == key; });
  return found == cache.end() ? nullptr : found->payload;
}

std::shared_ptr<const PlayerSkinVertexPayload>
CaptureVertices(uint8_t *guest_base, const SceneCatalogDrawOccurrence &draw,
                const PlayerSkinVertexFetchObservation &fetch) {
  if (!fetch.valid || fetch.endian != kVertexEndian8In32 || fetch.size == 0 ||
      fetch.size > kMaximumVertexPayloadBytes ||
      fetch.size % kPlayerVertexStride != 0 ||
      fetch.physical_address !=
          PhysicalAddressForVirtualAlias(draw.mesh.vertex_buffer_alias)) {
    return nullptr;
  }
  const VertexCacheKey key = {
      .physical_address = fetch.physical_address,
      .size = fetch.size,
      .endian = fetch.endian,
      .virtual_alias = draw.mesh.vertex_buffer_alias,
      .stride = draw.mesh.vertex_stride,
  };
  {
    std::lock_guard lock(g_snapshot_mutex);
    if (const auto cached = FindCached(g_vertex_cache, key)) {
      return cached;
    }
  }

  auto payload = std::make_shared<PlayerSkinVertexPayload>();
  payload->fetch = fetch;
  payload->source_virtual_alias = draw.mesh.vertex_buffer_alias;
  payload->stride = draw.mesh.vertex_stride;
  payload->vertex_count = fetch.size / kPlayerVertexStride;
  if (!CaptureStablePhysicalBytes(guest_base, fetch.physical_address,
                                  fetch.size, payload->raw_bytes)) {
    return nullptr;
  }
  payload->payload_fingerprint = Fingerprint(payload->raw_bytes);
  if (!payload->valid()) {
    return nullptr;
  }

  std::lock_guard lock(g_snapshot_mutex);
  if (const auto cached = FindCached(g_vertex_cache, key)) {
    return cached;
  }
  if (g_vertex_cache.size() >= kMaximumCachedVertices ||
      payload->raw_bytes.size() >
          kMaximumVertexCacheBytes -
              std::min(g_vertex_cache_bytes, kMaximumVertexCacheBytes)) {
    return nullptr;
  }
  g_vertex_cache_bytes += payload->raw_bytes.size();
  g_vertex_cache.push_back({key, payload});
  return payload;
}

std::shared_ptr<const PlayerSkinIndexPayload>
CaptureIndices(uint8_t *guest_base, const SceneCatalogDrawOccurrence &draw,
               uint32_t vertex_count) {
  const uint32_t element_size = draw.mesh.index_element_size;
  const uint64_t requested_size =
      static_cast<uint64_t>(draw.submitted_index_count) * element_size;
  const uint32_t physical_address =
      PhysicalAddressForVirtualAlias(draw.mesh.index_buffer_alias);
  if (!draw.mesh.valid || physical_address == 0 ||
      draw.submitted_index_count == 0 ||
      (element_size != 2 && element_size != 4) ||
      draw.mesh.index_is_32_bit != (element_size == 4) || requested_size == 0 ||
      requested_size > kMaximumIndexPayloadBytes ||
      requested_size > draw.mesh.index_buffer_bytes) {
    return nullptr;
  }
  const IndexCacheKey key = {
      .physical_address = physical_address,
      .virtual_alias = draw.mesh.index_buffer_alias,
      .element_size = element_size,
      .submitted_index_count = draw.submitted_index_count,
  };
  {
    std::lock_guard lock(g_snapshot_mutex);
    if (const auto cached = FindCached(g_index_cache, key)) {
      return cached;
    }
  }

  auto payload = std::make_shared<PlayerSkinIndexPayload>();
  payload->source_virtual_alias = draw.mesh.index_buffer_alias;
  payload->physical_address = physical_address;
  payload->element_size = element_size;
  payload->submitted_index_count = draw.submitted_index_count;
  if (!CaptureStablePhysicalBytes(guest_base, physical_address,
                                  static_cast<size_t>(requested_size),
                                  payload->raw_bytes)) {
    return nullptr;
  }
  payload->payload_fingerprint = Fingerprint(payload->raw_bytes);
  payload->indices.resize(draw.submitted_index_count);
  for (uint32_t index = 0; index < draw.submitted_index_count; ++index) {
    const uint8_t *source =
        payload->raw_bytes.data() + static_cast<size_t>(index) * element_size;
    const uint32_t decoded =
        element_size == 2 ? LoadBeU16(source) : LoadBeU32(source);
    if (decoded >= vertex_count) {
      return nullptr;
    }
    payload->indices[index] = decoded;
  }
  if (!payload->valid()) {
    return nullptr;
  }

  std::lock_guard lock(g_snapshot_mutex);
  if (const auto cached = FindCached(g_index_cache, key)) {
    return cached;
  }
  if (g_index_cache.size() >= kMaximumCachedIndices ||
      payload->raw_bytes.size() >
          kMaximumIndexCacheBytes -
              std::min(g_index_cache_bytes, kMaximumIndexCacheBytes)) {
    return nullptr;
  }
  g_index_cache_bytes += payload->raw_bytes.size();
  g_index_cache.push_back({key, payload});
  return payload;
}

std::shared_ptr<const PlayerSkinPalettePayload>
CapturePalette(uint8_t *guest_base, uint32_t player,
               const PlayerSkinVertexFetchObservation &fetch) {
  if (player == 0 || !fetch.valid || fetch.endian != kVertexEndian8In32 ||
      fetch.size == 0 || fetch.size > kMaximumPalettePayloadBytes ||
      fetch.size % kPaletteRecordStride != 0) {
    return nullptr;
  }
  const PaletteFrameKey key = {
      .player = player,
      .physical_address = fetch.physical_address,
      .size = fetch.size,
      .endian = fetch.endian,
  };
  {
    std::lock_guard lock(g_snapshot_mutex);
    if (const auto cached = FindCached(g_frame_palette_cache, key)) {
      return cached;
    }
  }

  auto payload = std::make_shared<PlayerSkinPalettePayload>();
  payload->fetch = fetch;
  payload->record_count = fetch.size / kPaletteRecordStride;
  if (!CaptureStablePhysicalBytes(guest_base, fetch.physical_address,
                                  fetch.size, payload->raw_bytes)) {
    return nullptr;
  }
  payload->payload_fingerprint = Fingerprint(payload->raw_bytes);
  payload->records.resize(payload->record_count);
  for (uint32_t record = 0; record < payload->record_count; ++record) {
    const uint8_t *source = payload->raw_bytes.data() +
                            static_cast<size_t>(record) * kPaletteRecordStride;
    PlayerSkinPaletteRecord &destination = payload->records[record];
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
    if (!std::all_of(destination.quaternion.begin(),
                     destination.quaternion.end(),
                     [](float value) { return std::isfinite(value); }) ||
        !std::all_of(destination.translation.begin(),
                     destination.translation.end(),
                     [](float value) { return std::isfinite(value); })) {
      return nullptr;
    }
  }
  if (!payload->valid()) {
    return nullptr;
  }

  std::lock_guard lock(g_snapshot_mutex);
  if (const auto cached = FindCached(g_frame_palette_cache, key)) {
    return cached;
  }
  g_frame_palette_cache.push_back({key, payload});
  return payload;
}

template <size_t Size> bool AllFinite(const std::array<float, Size> &values) {
  return std::all_of(values.begin(), values.end(),
                     [](float value) { return std::isfinite(value); });
}

bool MaterialConstantsValid(const PlayerSkinDrawObservation &observation) {
  return AllFinite(observation.vertex_constants_12_15) &&
         AllFinite(observation.vertex_constant_19) &&
         AllFinite(observation.vertex_constants_46_54) &&
         AllFinite(observation.vertex_constant_255) &&
         AllFinite(observation.pixel_constants_46_68) &&
         AllFinite(observation.pixel_constant_254) &&
         AllFinite(observation.pixel_constant_255) &&
         observation.pixel_control_constants_valid &&
         observation.texture_view_swizzles_valid;
}

} // namespace

PlayerSkinMeshIdentity
PlayerSkinMeshIdentityForDraw(const PlayerSkinDrawSnapshot &draw) {
  if (draw.vertices == nullptr || draw.indices == nullptr) {
    return {};
  }
  return {
      .player = draw.player,
      .vertex_physical_address = draw.vertices->fetch.physical_address,
      .vertex_size = draw.vertices->fetch.size,
      .vertex_fingerprint = draw.vertices->payload_fingerprint,
      .index_physical_address = draw.indices->physical_address,
      .index_count = draw.indices->submitted_index_count,
      .index_element_size = draw.indices->element_size,
      .index_fingerprint = draw.indices->payload_fingerprint,
  };
}

bool IsVerifiedPlayerSkinPassDescriptor(uint32_t pass_descriptor) {
  return std::ranges::find(kVerifiedPassDescriptors, pass_descriptor) !=
         kVerifiedPassDescriptors.end();
}

PlayerSkinPayloadCapture
CapturePlayerSkinPayloads(uint8_t *guest_base,
                          const SceneCatalogDrawOccurrence &draw,
                          const PlayerSkinDrawObservation &observation) {
  PlayerSkinPayloadCapture capture;
  if (guest_base == nullptr || draw.player == 0 ||
      !IsVerifiedPlayerSkinPassDescriptor(draw.pass.pass_descriptor) ||
      !draw.pass.valid || !draw.mesh.valid ||
      draw.mesh.vertex_stride != kPlayerVertexStride ||
      !MaterialConstantsValid(observation)) {
    ++capture.copy_failures;
    return capture;
  }

  capture.vertices = CaptureVertices(guest_base, draw, observation.vertices);
  capture.copy_failures += capture.vertices == nullptr;
  if (capture.vertices != nullptr) {
    capture.indices =
        CaptureIndices(guest_base, draw, capture.vertices->vertex_count);
  }
  capture.copy_failures += capture.indices == nullptr;
  capture.palette =
      CapturePalette(guest_base, draw.player, observation.palette);
  capture.copy_failures += capture.palette == nullptr;

  for (uint32_t slot = 0; slot < capture.textures.size(); ++slot) {
    capture.textures[slot] =
        CaptureTextureSnapshot(guest_base, observation.pixel_shader, slot,
                               observation.texture_fetches[slot]);
    capture.copy_failures += capture.textures[slot] == nullptr;
  }
  return capture;
}

void ObservePlayerSkinSnapshot(const SceneCatalogDrawOccurrence &draw,
                               const PlayerSkinDrawObservation &observation,
                               const PlayerSkinPayloadCapture &payloads) {
  if (!observation.valid || !payloads.valid() ||
      !IsVerifiedPlayerSkinPassDescriptor(observation.pass_descriptor)) {
    if (payloads.copy_failures != 0) {
      std::lock_guard lock(g_snapshot_mutex);
      g_building_frame.copy_failures += payloads.copy_failures;
    }
    return;
  }

  PlayerSkinDrawSnapshot snapshot;
  snapshot.vertices = payloads.vertices;
  snapshot.indices = payloads.indices;
  snapshot.palette = payloads.palette;
  snapshot.material.texture_fetches = observation.texture_fetches;
  snapshot.material.texture_view_swizzles = observation.texture_view_swizzles;
  snapshot.material.textures = payloads.textures;
  snapshot.material.vertex_constants_12_15 = observation.vertex_constants_12_15;
  snapshot.material.vertex_constant_19 = observation.vertex_constant_19;
  snapshot.material.vertex_constants_46_54 = observation.vertex_constants_46_54;
  snapshot.material.vertex_constant_255 = observation.vertex_constant_255;
  snapshot.material.pixel_constants_46_68 = observation.pixel_constants_46_68;
  snapshot.material.pixel_constant_254 = observation.pixel_constant_254;
  snapshot.material.pixel_constant_255 = observation.pixel_constant_255;
  snapshot.material.pixel_control_constants_valid =
      observation.pixel_control_constants_valid;
  snapshot.material.texture_view_swizzles_valid =
      observation.texture_view_swizzles_valid;
  for (size_t slot = 0; slot < snapshot.material.textures.size(); ++slot) {
    const auto &texture = snapshot.material.textures[slot];
    snapshot.material.texture_view_swizzles_valid &=
        texture != nullptr &&
        texture->fetch_words == snapshot.material.texture_fetches[slot] &&
        texture->fetch_swizzle == snapshot.material.texture_view_swizzles[slot];
  }
  snapshot.material.valid =
      MaterialConstantsValid(observation) &&
      snapshot.material.pixel_control_constants_valid &&
      snapshot.material.texture_view_swizzles_valid &&
      std::all_of(snapshot.material.textures.begin(),
                  snapshot.material.textures.end(), [](const auto &texture) {
                    return texture != nullptr && texture->valid();
                  });
  snapshot.ordinal = observation.ordinal;
  snapshot.player = observation.player;
  snapshot.shader = observation.shader;
  snapshot.model = observation.model;
  snapshot.geometry_index = observation.geometry_index;
  snapshot.pass_descriptor = observation.pass_descriptor;
  snapshot.program_pair = observation.program_pair;
  snapshot.vertex_shader = observation.vertex_shader;
  snapshot.pixel_shader = observation.pixel_shader;
  snapshot.primitive_type = draw.primitive_type;
  snapshot.submitted_index_count = observation.submitted_index_count;
  snapshot.alternate_pass = observation.alternate_pass;
  snapshot.valid =
      snapshot.vertices != nullptr && snapshot.vertices->valid() &&
      snapshot.indices != nullptr && snapshot.indices->valid() &&
      snapshot.palette != nullptr && snapshot.palette->valid() &&
      snapshot.material.valid && snapshot.player != 0 &&
      snapshot.submitted_index_count == snapshot.indices->submitted_index_count;
  if (!snapshot.valid) {
    return;
  }

  std::lock_guard lock(g_snapshot_mutex);
  if (draw.frame_sequence == 0) {
    ++g_building_frame.dropped_draw_count;
    return;
  }
  if (g_building_frame.sequence == 0) {
    g_building_frame.sequence = draw.frame_sequence;
  } else if (g_building_frame.sequence != draw.frame_sequence) {
    ++g_building_frame.dropped_draw_count;
    return;
  }
  if (g_building_frame.draws.size() == kMaximumFrameDraws) {
    ++g_building_frame.dropped_draw_count;
    return;
  }
  // The title hook runs before the backend submission for this exact draw.
  // Publish the value-only proof token synchronously. FrameEnd verifies it
  // against the immutable candidate, while the bounded async ledger retains
  // it for the later tiled EDRAM backend replay.
  PublishPlayerReplacementLiveTitleDraw(draw.frame_sequence, snapshot);
  g_building_frame.draws.push_back(std::move(snapshot));
  ++g_building_frame.admitted_draw_count;
}

void PlayerSkinSnapshotFrameEnd() {
  const bool enabled = PlayerSkinObserverEnabled();
  std::lock_guard lock(g_snapshot_mutex);
  if (!enabled) {
    g_building_frame = {};
    g_published_frame.reset();
    g_frame_palette_cache.clear();
    g_largest_logged_draw_count = 0;
    PublishPlayerReplacementCandidates(nullptr);
    return;
  }

  g_published_frame = std::make_shared<const PlayerSkinFrameSnapshot>(
      std::move(g_building_frame));
  ObserveMainCoverageFamilyFrame(g_published_frame);
  g_building_frame = {};
  g_frame_palette_cache.clear();

  if (g_published_frame->valid() &&
      g_published_frame->draws.size() > g_largest_logged_draw_count) {
    g_largest_logged_draw_count = g_published_frame->draws.size();
    const PlayerSkinDrawSnapshot &first = g_published_frame->draws.front();
    REXLOG_INFO("Table Tennis player skin snapshot: frame={} draws={} "
                "vf95={:08X}/{} vertices={} indices={} vf92={:08X}/{} "
                "palette_records={} textures=3 pass={:08X} observer_only=true",
                g_published_frame->sequence, g_published_frame->draws.size(),
                first.vertices->fetch.physical_address,
                first.vertices->raw_bytes.size(), first.vertices->vertex_count,
                first.indices->submitted_index_count,
                first.palette->fetch.physical_address,
                first.palette->raw_bytes.size(), first.palette->record_count,
                first.pass_descriptor);
  }
  PublishPlayerReplacementCandidates(g_published_frame);
}

std::shared_ptr<const PlayerSkinFrameSnapshot> LatestPlayerSkinFrameSnapshot() {
  std::lock_guard lock(g_snapshot_mutex);
  return g_published_frame;
}

} // namespace tabletennis::native
