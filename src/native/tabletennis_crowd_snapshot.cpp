#include "native/tabletennis_crowd_snapshot.h"

#include "generated/default/tabletennis_init.h"
#include "native/tabletennis_guest_memory.h"
#include "native/tabletennis_scene_draw_catalog.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <mutex>
#include <utility>

#include <rex/graphics/pipeline/texture/info.h>

namespace tabletennis::native {
namespace {

namespace xenos = rex::graphics::xenos;

constexpr uint32_t kFetchBankOffset = 0x480;
constexpr uint32_t kVertexConstantBankOffset = 0x780;
constexpr uint32_t kPrimaryVertexFetchSlot = 95;
constexpr uint32_t kPaletteVertexFetchSlot = 92;
constexpr uint32_t kVertexStride = 36;
constexpr uint32_t kPaletteRecordStride = 28;
constexpr uint32_t kVertexEndian8In32 = 2;
constexpr uint32_t kPhysicalAliasBase = 0xA0000000;
constexpr uint32_t kPhysicalAddressMask = 0x1FFFFFFF;
constexpr uint32_t kHighPhysicalHeapBase = 0xE0000000;
constexpr uint32_t kHighPhysicalHeapHostPageOffset = 0x1000;
constexpr size_t kConstantRowBytes = sizeof(float) * 4;
constexpr size_t kMaximumVertexPayloadBytes = 16 * 1024 * 1024;
constexpr size_t kMaximumIndexPayloadBytes = 4 * 1024 * 1024;
constexpr size_t kMaximumPalettePayloadBytes = 64 * 1024;
constexpr size_t kMaximumTexturePayloadBytes = 16 * 1024 * 1024;
constexpr size_t kMaximumCachedVertices = 64;
constexpr size_t kMaximumCachedIndices = 64;
constexpr size_t kMaximumCachedTextures = 64;
constexpr size_t kMaximumVertexCacheBytes = 128 * 1024 * 1024;
constexpr size_t kMaximumIndexCacheBytes = 32 * 1024 * 1024;
constexpr size_t kMaximumTextureCacheBytes = 128 * 1024 * 1024;
constexpr uint64_t kFnvOffsetBasis = 1469598103934665603ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;

struct VertexKey {
  uint32_t physical_address = 0;
  uint32_t size = 0;
  uint32_t virtual_alias = 0;

  bool operator==(const VertexKey&) const = default;
};

struct IndexKey {
  uint32_t physical_address = 0;
  uint32_t virtual_alias = 0;
  uint32_t index_count = 0;

  bool operator==(const IndexKey&) const = default;
};

struct PaletteKey {
  uint32_t physical_address = 0;
  uint32_t size = 0;

  bool operator==(const PaletteKey&) const = default;
};

struct TextureKey {
  std::array<uint32_t, 6> fetch_words{};

  bool operator==(const TextureKey&) const = default;
};

template <typename Key, typename Payload>
struct CachedPayload {
  Key key{};
  std::shared_ptr<const Payload> payload;
};

std::mutex g_snapshot_mutex;
std::vector<CachedPayload<VertexKey, CrowdVertexPayload>> g_vertex_cache;
std::vector<CachedPayload<IndexKey, CrowdIndexPayload>> g_index_cache;
std::vector<CachedPayload<PaletteKey, CrowdPalettePayload>>
    g_frame_palette_cache;
std::vector<CachedPayload<TextureKey, CrowdTextureArrayPayload>>
    g_texture_cache;
size_t g_vertex_cache_bytes = 0;
size_t g_index_cache_bytes = 0;
size_t g_texture_cache_bytes = 0;

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
                        uint32_t& result) {
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

bool TryCopyGuest(uint8_t* guest_base, uint32_t address, size_t offset,
                  void* destination, size_t size) {
  uint32_t guest_address = 0;
  return guest_base != nullptr &&
         CheckedGuestOffset(address, offset, size, guest_address) &&
         GuestTryCopy(
             destination,
             guest_base + guest_address + REX_PHYS_HOST_OFFSET(guest_address),
             size);
}

bool TryCopyGuestPhysical(uint8_t* guest_base, uint32_t physical_address,
                          void* destination, size_t size) {
  if (physical_address == 0 || physical_address > kPhysicalAddressMask) {
    return false;
  }
  return TryCopyGuest(guest_base, kPhysicalAliasBase | physical_address, 0,
                      destination, size);
}

bool CaptureStablePhysicalBytes(uint8_t* guest_base,
                                uint32_t physical_address, size_t size,
                                std::vector<uint8_t>& bytes) {
  if (size == 0) {
    return false;
  }
  bytes.resize(size);
  std::vector<uint8_t> verification(size);
  for (uint32_t attempt = 0; attempt < 4; ++attempt) {
    if (!TryCopyGuestPhysical(guest_base, physical_address, bytes.data(),
                              bytes.size()) ||
        !TryCopyGuestPhysical(guest_base, physical_address,
                              verification.data(), verification.size())) {
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

uint16_t LoadBeU16(const uint8_t* source) {
  uint16_t value = 0;
  std::memcpy(&value, source, sizeof(value));
  return std::byteswap(value);
}

uint32_t LoadBeU32(const std::byte* source) {
  uint32_t value = 0;
  std::memcpy(&value, source, sizeof(value));
  return std::byteswap(value);
}

float LoadBeF32(const uint8_t* source) {
  uint32_t value = 0;
  std::memcpy(&value, source, sizeof(value));
  return std::bit_cast<float>(std::byteswap(value));
}

uint64_t Fingerprint(const std::vector<uint8_t>& bytes) {
  uint64_t hash = kFnvOffsetBasis;
  for (uint8_t value : bytes) {
    hash = (hash ^ value) * kFnvPrime;
  }
  return hash;
}

bool HasNonzeroData(const std::vector<uint8_t>& bytes) {
  return std::ranges::any_of(bytes,
                             [](uint8_t value) { return value != 0; });
}

template <size_t Size>
bool CaptureBeWords(uint8_t* guest_base, uint32_t address, size_t offset,
                    std::array<uint32_t, Size>& words) {
  std::array<std::byte, Size * sizeof(uint32_t)> bytes;
  if (!TryCopyGuest(guest_base, address, offset, bytes.data(),
                    bytes.size())) {
    return false;
  }
  for (size_t index = 0; index < Size; ++index) {
    words[index] =
        LoadBeU32(bytes.data() + index * sizeof(uint32_t));
  }
  return true;
}

template <size_t Size>
bool CaptureBeFloats(uint8_t* guest_base, uint32_t address, size_t offset,
                     std::array<float, Size>& values) {
  std::array<uint32_t, Size> words;
  if (!CaptureBeWords(guest_base, address, offset, words)) {
    return false;
  }
  for (size_t index = 0; index < Size; ++index) {
    values[index] = std::bit_cast<float>(words[index]);
  }
  return true;
}

CrowdVertexFetchSnapshot CaptureVertexFetch(uint8_t* guest_base,
                                            uint32_t device,
                                            uint32_t slot) {
  CrowdVertexFetchSnapshot fetch;
  fetch.slot = slot;
  if (!CaptureBeWords(
          guest_base, device,
          kFetchBankOffset + slot * sizeof(uint32_t) * 2, fetch.words)) {
    return fetch;
  }
  fetch.physical_address = fetch.words[0] & ~uint32_t{3};
  fetch.endian = fetch.words[1] & 0x3u;
  fetch.size = fetch.words[1] & 0x03FFFFFCu;
  fetch.valid = (fetch.words[0] & 0x3u) == 3 &&
                fetch.physical_address != 0 && fetch.size != 0;
  return fetch;
}

template <typename Key, typename Payload>
std::shared_ptr<const Payload> FindCached(
    const std::vector<CachedPayload<Key, Payload>>& cache, const Key& key) {
  const auto found =
      std::ranges::find_if(cache, [&](const auto& cached) {
        return cached.key == key;
      });
  return found == cache.end() ? nullptr : found->payload;
}

std::shared_ptr<const CrowdVertexPayload> CaptureVertices(
    uint8_t* guest_base, const SceneCatalogDrawOccurrence& draw,
    const CrowdVertexFetchSnapshot& fetch) {
  const uint32_t expected_physical =
      PhysicalAddressForVirtualAlias(draw.mesh.vertex_buffer_alias);
  if (!fetch.valid || fetch.endian != kVertexEndian8In32 ||
      fetch.physical_address != expected_physical ||
      fetch.size > kMaximumVertexPayloadBytes ||
      fetch.size % kVertexStride != 0) {
    return nullptr;
  }

  const VertexKey key = {
      .physical_address = fetch.physical_address,
      .size = fetch.size,
      .virtual_alias = draw.mesh.vertex_buffer_alias,
  };
  {
    std::lock_guard lock(g_snapshot_mutex);
    if (const auto cached = FindCached(g_vertex_cache, key)) {
      return cached;
    }
  }

  auto payload = std::make_shared<CrowdVertexPayload>();
  payload->fetch = fetch;
  payload->source_virtual_alias = draw.mesh.vertex_buffer_alias;
  payload->stride = kVertexStride;
  payload->vertex_count = fetch.size / kVertexStride;
  if (!CaptureStablePhysicalBytes(guest_base, fetch.physical_address,
                                  fetch.size, payload->raw_bytes)) {
    return nullptr;
  }
  payload->payload_fingerprint = Fingerprint(payload->raw_bytes);
  payload->has_nonzero_data = HasNonzeroData(payload->raw_bytes);
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

std::shared_ptr<const CrowdIndexPayload> CaptureIndices(
    uint8_t* guest_base, const SceneCatalogDrawOccurrence& draw,
    uint32_t vertex_count) {
  const uint64_t requested_size =
      static_cast<uint64_t>(draw.submitted_index_count) * sizeof(uint16_t);
  const uint32_t physical_address =
      PhysicalAddressForVirtualAlias(draw.mesh.index_buffer_alias);
  if (physical_address == 0 || draw.mesh.index_is_32_bit ||
      draw.mesh.index_element_size != sizeof(uint16_t) ||
      requested_size == 0 || requested_size > kMaximumIndexPayloadBytes ||
      requested_size > draw.mesh.index_buffer_bytes) {
    return nullptr;
  }

  const IndexKey key = {
      .physical_address = physical_address,
      .virtual_alias = draw.mesh.index_buffer_alias,
      .index_count = draw.submitted_index_count,
  };
  {
    std::lock_guard lock(g_snapshot_mutex);
    if (const auto cached = FindCached(g_index_cache, key)) {
      return cached;
    }
  }

  auto payload = std::make_shared<CrowdIndexPayload>();
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
  for (uint32_t index = 0; index < draw.submitted_index_count; ++index) {
    const uint16_t decoded =
        LoadBeU16(payload->raw_bytes.data() + static_cast<size_t>(index) * 2);
    if (decoded >= vertex_count) {
      return nullptr;
    }
    payload->indices[index] = decoded;
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

std::shared_ptr<const CrowdPalettePayload> CapturePalette(
    uint8_t* guest_base, const CrowdVertexFetchSnapshot& fetch) {
  if (!fetch.valid || fetch.endian != kVertexEndian8In32 ||
      fetch.size > kMaximumPalettePayloadBytes ||
      fetch.size % kPaletteRecordStride != 0) {
    return nullptr;
  }
  const PaletteKey key = {
      .physical_address = fetch.physical_address,
      .size = fetch.size,
  };
  {
    std::lock_guard lock(g_snapshot_mutex);
    if (const auto cached = FindCached(g_frame_palette_cache, key)) {
      return cached;
    }
  }

  auto payload = std::make_shared<CrowdPalettePayload>();
  payload->fetch = fetch;
  payload->record_count = fetch.size / kPaletteRecordStride;
  if (!CaptureStablePhysicalBytes(guest_base, fetch.physical_address,
                                  fetch.size, payload->raw_bytes)) {
    return nullptr;
  }
  payload->payload_fingerprint = Fingerprint(payload->raw_bytes);
  payload->has_nonzero_data = HasNonzeroData(payload->raw_bytes);
  payload->records.resize(payload->record_count);
  for (uint32_t record = 0; record < payload->record_count; ++record) {
    const uint8_t* source =
        payload->raw_bytes.data() +
        static_cast<size_t>(record) * kPaletteRecordStride;
    CrowdPaletteRecord& destination = payload->records[record];
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
    if (!std::ranges::all_of(destination.quaternion, [](float value) {
          return std::isfinite(value);
        }) ||
        !std::ranges::all_of(destination.translation, [](float value) {
          return std::isfinite(value);
        })) {
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

std::shared_ptr<const CrowdTextureArrayPayload> CaptureTextureArray(
    uint8_t* guest_base, const std::array<uint32_t, 6>& fetch_words) {
  const TextureKey key{.fetch_words = fetch_words};
  {
    std::lock_guard lock(g_snapshot_mutex);
    if (const auto cached = FindCached(g_texture_cache, key)) {
      return cached;
    }
  }

  xenos::xe_gpu_texture_fetch_t fetch{};
  fetch.dword_0 = fetch_words[0];
  fetch.dword_1 = fetch_words[1];
  fetch.dword_2 = fetch_words[2];
  fetch.dword_3 = fetch_words[3];
  fetch.dword_4 = fetch_words[4];
  fetch.dword_5 = fetch_words[5];
  rex::graphics::TextureInfo info;
  const bool prepared =
      fetch.type == xenos::FetchConstantType::kTexture &&
      rex::graphics::TextureInfo::Prepare(fetch, &info);
  const bool stacked_2d =
      prepared &&
      info.dimension == xenos::DataDimension::k2DOrStacked &&
      info.is_stacked;
  const bool volume_3d =
      prepared && info.dimension == xenos::DataDimension::k3D &&
      !info.is_stacked;
  if (fetch.type != xenos::FetchConstantType::kTexture ||
      !prepared || (!stacked_2d && !volume_3d) ||
      info.memory.base_address == 0 ||
      info.memory.base_size == 0 ||
      info.memory.base_size > kMaximumTexturePayloadBytes) {
    return nullptr;
  }

  auto payload = std::make_shared<CrowdTextureArrayPayload>();
  payload->fetch_words = fetch_words;
  payload->physical_address = info.memory.base_address;
  payload->byte_size = info.memory.base_size;
  payload->width = info.width + 1;
  payload->height = info.height + 1;
  payload->layers = info.depth + 1;
  payload->dimension = static_cast<uint32_t>(info.dimension);
  payload->format = static_cast<uint32_t>(info.format);
  payload->endianness = static_cast<uint32_t>(info.endianness);
  payload->pitch_blocks = info.extent.block_pitch_h;
  payload->fetch_swizzle = fetch.swizzle;
  payload->tiled = info.is_tiled;
  payload->stacked = info.is_stacked;
  payload->volume = volume_3d;
  if (!CaptureStablePhysicalBytes(guest_base, info.memory.base_address,
                                  info.memory.base_size,
                                  payload->raw_bytes)) {
    return nullptr;
  }
  payload->payload_fingerprint = Fingerprint(payload->raw_bytes);
  if (!payload->valid()) {
    return nullptr;
  }

  std::lock_guard lock(g_snapshot_mutex);
  if (const auto cached = FindCached(g_texture_cache, key)) {
    return cached;
  }
  if (g_texture_cache.size() >= kMaximumCachedTextures ||
      payload->raw_bytes.size() >
          kMaximumTextureCacheBytes -
              std::min(g_texture_cache_bytes, kMaximumTextureCacheBytes)) {
    return nullptr;
  }
  g_texture_cache_bytes += payload->raw_bytes.size();
  g_texture_cache.push_back({key, payload});
  return payload;
}

template <size_t Size>
bool AllFinite(const std::array<float, Size>& values) {
  return std::ranges::all_of(values,
                             [](float value) { return std::isfinite(value); });
}

bool DecodeConstantsMatch(const std::array<float, 4>& values) {
  constexpr std::array<float, 4> expected = {
      0.0f, 1.0f, 255.001953125f, 0.0f};
  for (size_t index = 0; index < values.size(); ++index) {
    if (!std::isfinite(values[index]) ||
        std::abs(values[index] - expected[index]) > 0.001f) {
      return false;
    }
  }
  return true;
}

}  // namespace

CrowdDrawSnapshot CaptureCrowdDrawSnapshot(
    uint8_t* guest_base, const SceneCatalogDrawOccurrence& draw,
    const CrowdOwnerSnapshot& owner) {
  CrowdDrawSnapshot snapshot;
  snapshot.owner = owner;
  snapshot.ordinal = draw.ordinal;
  if (draw.scope_valid) {
    snapshot.shader = draw.scope.shader;
    snapshot.model = draw.scope.model;
    snapshot.geometry_index = draw.scope.geometry_index;
  }
  if (draw.pass.valid) {
    snapshot.pass_descriptor = draw.pass.pass_descriptor;
    snapshot.program_pair = draw.pass.program_pair;
    snapshot.vertex_shader = draw.pass.vertex_shader;
    snapshot.pixel_shader = draw.pass.pixel_shader;
  }
  snapshot.primitive_type = draw.primitive_type;
  snapshot.submitted_index_count = draw.submitted_index_count;

  const CrowdVertexFetchSnapshot vertices =
      CaptureVertexFetch(guest_base, draw.device, kPrimaryVertexFetchSlot);
  const CrowdVertexFetchSnapshot palette =
      CaptureVertexFetch(guest_base, draw.device, kPaletteVertexFetchSlot);
  snapshot.vertex_fetch = vertices;
  snapshot.palette_fetch = palette;
  snapshot.copy_failures += !vertices.valid;
  snapshot.copy_failures += !palette.valid;

  snapshot.material.declaration =
      ProbeVertexDeclaration(guest_base, draw.state.vertex_declaration);
  snapshot.material.texture_fetch = draw.state.texture_fetches[0];
  auto capture_constants = [&](size_t first_row, auto& destination) {
    if (!CaptureBeFloats(
            guest_base, draw.device,
            kVertexConstantBankOffset + first_row * kConstantRowBytes,
            destination)) {
      ++snapshot.copy_failures;
    }
  };
  capture_constants(32, snapshot.material.instance_transform);
  capture_constants(36, snapshot.material.view_projection);
  capture_constants(136, snapshot.material.lighting_spheres);
  capture_constants(145, snapshot.material.ambient);
  capture_constants(255, snapshot.material.decode_constants);
  snapshot.material.decode_constants_verified =
      DecodeConstantsMatch(snapshot.material.decode_constants);

  snapshot.material.texture =
      CaptureTextureArray(guest_base, snapshot.material.texture_fetch);
  snapshot.copy_failures += snapshot.material.texture == nullptr;

  // fxCrowdGfx submits its specialized model records directly through
  // sub_820EE910. It deliberately bypasses grmShaderFx::DrawModelGeometry, so
  // SceneCatalogDrawOccurrence::scope/pass are not part of the crowd proof.
  // Semantic ownership comes from the exact fxCrowdGfx drawable/model pair;
  // the remaining checks validate the live draw-time GPU contract.
  const bool scalar_contract_valid =
      guest_base != nullptr && owner.valid &&
      owner.submitted_drawable != 0 && owner.submitted_model != 0 &&
      draw.mesh.valid && draw.state.valid &&
      draw.mesh.vertex_stride == kVertexStride &&
      draw.mesh.vertex_endian == kVertexEndian8In32 &&
      !draw.mesh.index_is_32_bit &&
      draw.mesh.index_element_size == sizeof(uint16_t) &&
      vertices.valid && vertices.endian == kVertexEndian8In32 &&
      vertices.size % kVertexStride == 0 &&
      vertices.physical_address ==
          PhysicalAddressForVirtualAlias(draw.mesh.vertex_buffer_alias) &&
      palette.valid && palette.endian == kVertexEndian8In32 &&
      palette.size % kPaletteRecordStride == 0 &&
      snapshot.material.declaration.valid &&
      snapshot.material.texture != nullptr &&
      snapshot.material.texture->format ==
          static_cast<uint32_t>(xenos::TextureFormat::k_DXT1) &&
      (snapshot.material.texture->width == 128 ||
       snapshot.material.texture->width == 256) &&
      (snapshot.material.texture->height == 128 ||
       snapshot.material.texture->height == 256) &&
      snapshot.material.texture->layers == 8 &&
      snapshot.material.texture->volume &&
      snapshot.material.texture->tiled &&
      snapshot.material.texture->fetch_swizzle == 0x688 &&
      AllFinite(snapshot.material.instance_transform) &&
      AllFinite(snapshot.material.view_projection) &&
      AllFinite(snapshot.material.lighting_spheres) &&
      AllFinite(snapshot.material.ambient) &&
      snapshot.material.decode_constants_verified;

  if (scalar_contract_valid) {
    snapshot.vertices = CaptureVertices(guest_base, draw, vertices);
    snapshot.copy_failures += snapshot.vertices == nullptr;
    if (snapshot.vertices != nullptr) {
      snapshot.indices =
          CaptureIndices(guest_base, draw, snapshot.vertices->vertex_count);
    }
    snapshot.copy_failures += snapshot.indices == nullptr;
    snapshot.palette = CapturePalette(guest_base, palette);
    snapshot.copy_failures += snapshot.palette == nullptr;
  }

  snapshot.material.valid =
      scalar_contract_valid && snapshot.material.texture != nullptr;
  snapshot.valid =
      snapshot.copy_failures == 0 && snapshot.material.valid &&
      snapshot.vertices != nullptr && snapshot.vertices->valid() &&
      snapshot.indices != nullptr && snapshot.indices->valid() &&
      snapshot.palette != nullptr && snapshot.palette->valid();
  return snapshot;
}

void CrowdSnapshotFrameEnd() {
  std::lock_guard lock(g_snapshot_mutex);
  g_frame_palette_cache.clear();
}

}  // namespace tabletennis::native
