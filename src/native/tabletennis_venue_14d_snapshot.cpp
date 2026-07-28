#include "native/tabletennis_venue_14d_snapshot.h"

#include "generated/default/tabletennis_init.h"
#include "native/tabletennis_guest_memory.h"
#include "native/tabletennis_scene_draw_catalog.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstring>
#include <limits>
#include <mutex>

namespace tabletennis::native {
namespace {

constexpr uint32_t kFetchBankOffset = 0x480;
constexpr uint32_t kVertexConstantBankOffset = 0x780;
constexpr uint32_t kPixelConstantBankOffset = 0x1780;
constexpr uint32_t kTriangleStripPrimitive = 6;
constexpr uint32_t kPhysicalAliasBase = 0xA0000000;
constexpr uint32_t kPhysicalAddressMask = 0x1FFFFFFF;
constexpr uint32_t kHighPhysicalHeapBase = 0xE0000000;
constexpr uint32_t kHighPhysicalHeapHostPageOffset = 0x1000;
constexpr uint32_t kVertexStride40 = 40;
constexpr uint32_t kVertexStride48 = 48;

constexpr uint32_t kEarlyPass = 0x402E78A8;
constexpr uint32_t kEarlyProgram = 0x402EAE80;
constexpr uint32_t kEarlyVertexShader = 0x402E5510;
constexpr uint32_t kEarlyPixelShader = 0x402E6500;
constexpr uint32_t kMainPass = 0x402DD218;
constexpr uint32_t kMainProgram = 0x402E07B0;
constexpr uint32_t kMainVertexShader = 0x402DB350;
constexpr uint32_t kMainPixelShader = 0x402DC340;
constexpr uint32_t kLatePass = 0x402F20A8;
constexpr uint32_t kLateProgram = 0x402F5680;
constexpr uint32_t kLateVertexShader = 0x402EFCE0;
constexpr uint32_t kLatePixelShader = 0x402F0CD0;

constexpr size_t kMaximumVertexPayloadBytes = 64 * 1024 * 1024;
constexpr size_t kMaximumIndexPayloadBytes = 16 * 1024 * 1024;
constexpr size_t kMaximumCachedVertices = 256;
constexpr size_t kMaximumCachedIndices = 512;
constexpr size_t kMaximumVertexCacheBytes = 128 * 1024 * 1024;
constexpr size_t kMaximumIndexCacheBytes = 64 * 1024 * 1024;
constexpr uint64_t kFnvOffsetBasis = 1469598103934665603ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;

struct VertexCacheKey {
  uint32_t physical_address = 0;
  uint32_t byte_count = 0;
  uint32_t source_virtual_alias = 0;
  uint32_t stride = 0;

  bool operator==(const VertexCacheKey &) const = default;
};

struct IndexCacheKey {
  uint32_t physical_address = 0;
  uint32_t source_virtual_alias = 0;
  uint32_t submitted_index_count = 0;

  bool operator==(const IndexCacheKey &) const = default;
};

template <typename Key, typename Payload> struct CachedPayload {
  Key key{};
  std::shared_ptr<const Payload> payload;
};

std::mutex g_cache_mutex;
std::vector<CachedPayload<VertexCacheKey, Venue14DVertexPayload>>
    g_vertex_cache;
std::vector<CachedPayload<IndexCacheKey, Venue14DIndexPayload>> g_index_cache;
size_t g_vertex_cache_bytes = 0;
size_t g_index_cache_bytes = 0;

bool CheckedGuestOffset(uint32_t address, size_t offset, size_t size,
                        uint32_t &result) {
  if (address == 0 || size == 0) {
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

bool TryCopyGuest(uint8_t *guest_base, uint32_t address, size_t offset,
                  void *destination, size_t size) {
  uint32_t guest_address = 0;
  return guest_base != nullptr && destination != nullptr &&
         CheckedGuestOffset(address, offset, size, guest_address) &&
         GuestTryCopy(destination,
                      guest_base + guest_address +
                          REX_PHYS_HOST_OFFSET(guest_address),
                      size);
}

bool TryCopyGuestPhysical(uint8_t *guest_base, uint32_t physical_address,
                          void *destination, size_t size) {
  return physical_address != 0 && physical_address <= kPhysicalAddressMask &&
         TryCopyGuest(guest_base, kPhysicalAliasBase | physical_address, 0,
                      destination, size);
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

uint16_t LoadBeU16(const uint8_t *source) {
  uint16_t value = 0;
  std::memcpy(&value, source, sizeof(value));
  return std::byteswap(value);
}

uint32_t LoadBeU32(const std::byte *source) {
  uint32_t value = 0;
  std::memcpy(&value, source, sizeof(value));
  return std::byteswap(value);
}

uint64_t Fingerprint(const std::vector<uint8_t> &bytes) {
  uint64_t fingerprint = kFnvOffsetBasis;
  for (uint8_t value : bytes) {
    fingerprint = (fingerprint ^ value) * kFnvPrime;
  }
  return fingerprint;
}

template <size_t Size>
bool CaptureBeWords(uint8_t *guest_base, uint32_t address, size_t offset,
                    std::array<uint32_t, Size> &words) {
  std::array<std::byte, Size * sizeof(uint32_t)> bytes{};
  if (!TryCopyGuest(guest_base, address, offset, bytes.data(), bytes.size())) {
    return false;
  }
  for (size_t index = 0; index < Size; ++index) {
    words[index] = LoadBeU32(bytes.data() + index * sizeof(uint32_t));
  }
  return true;
}

bool IsExactTitleProgram(const SceneCatalogDrawOccurrence &draw) {
  const bool early = draw.pass.pass_descriptor == kEarlyPass &&
                     draw.pass.program_pair == kEarlyProgram &&
                     draw.pass.vertex_shader == kEarlyVertexShader &&
                     draw.pass.pixel_shader == kEarlyPixelShader;
  const bool main = draw.pass.pass_descriptor == kMainPass &&
                    draw.pass.program_pair == kMainProgram &&
                    draw.pass.vertex_shader == kMainVertexShader &&
                    draw.pass.pixel_shader == kMainPixelShader;
  const bool late = draw.pass.pass_descriptor == kLatePass &&
                    draw.pass.program_pair == kLateProgram &&
                    draw.pass.vertex_shader == kLateVertexShader &&
                    draw.pass.pixel_shader == kLatePixelShader;
  return early || main || late;
}

template <typename Key, typename Payload>
std::shared_ptr<const Payload>
FindCached(const std::vector<CachedPayload<Key, Payload>> &cache,
           const Key &key) {
  const auto found = std::ranges::find_if(
      cache, [&](const auto &entry) { return entry.key == key; });
  return found == cache.end() ? nullptr : found->payload;
}

std::shared_ptr<const Venue14DVertexPayload>
CaptureVertices(uint8_t *guest_base, const SceneCatalogDrawOccurrence &draw) {
  const auto &mesh = draw.mesh;
  const uint32_t physical_address =
      PhysicalAddressForVirtualAlias(mesh.vertex_buffer_alias);
  if (!mesh.valid || physical_address == 0 || mesh.vertex_buffer_bytes == 0 ||
      mesh.vertex_buffer_bytes > kMaximumVertexPayloadBytes ||
      mesh.vertex_stride == 0 || mesh.vertex_stride > 256 ||
      mesh.vertex_buffer_bytes % mesh.vertex_stride != 0) {
    return nullptr;
  }
  const VertexCacheKey key = {
      .physical_address = physical_address,
      .byte_count = mesh.vertex_buffer_bytes,
      .source_virtual_alias = mesh.vertex_buffer_alias,
      .stride = mesh.vertex_stride,
  };
  {
    std::lock_guard lock(g_cache_mutex);
    if (const auto cached = FindCached(g_vertex_cache, key)) {
      return cached;
    }
  }

  auto payload = std::make_shared<Venue14DVertexPayload>();
  payload->source_virtual_alias = mesh.vertex_buffer_alias;
  payload->physical_address = physical_address;
  payload->byte_count = mesh.vertex_buffer_bytes;
  payload->stride = mesh.vertex_stride;
  payload->vertex_count = mesh.vertex_buffer_bytes / mesh.vertex_stride;
  payload->endian = mesh.vertex_endian;
  if (!CaptureStablePhysicalBytes(guest_base, physical_address,
                                  mesh.vertex_buffer_bytes,
                                  payload->raw_bytes)) {
    return nullptr;
  }
  payload->payload_fingerprint = Fingerprint(payload->raw_bytes);
  if (!payload->valid()) {
    return nullptr;
  }

  std::lock_guard lock(g_cache_mutex);
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

std::shared_ptr<const Venue14DIndexPayload>
CaptureIndices(uint8_t *guest_base, const SceneCatalogDrawOccurrence &draw,
               uint32_t vertex_count) {
  const uint64_t requested_size =
      static_cast<uint64_t>(draw.submitted_index_count) * sizeof(uint16_t);
  const uint32_t physical_address =
      PhysicalAddressForVirtualAlias(draw.mesh.index_buffer_alias);
  if (!draw.mesh.valid || physical_address == 0 ||
      draw.submitted_index_count == 0 ||
      draw.mesh.index_element_size != sizeof(uint16_t) ||
      draw.mesh.index_is_32_bit || requested_size == 0 ||
      requested_size > kMaximumIndexPayloadBytes ||
      requested_size > draw.mesh.index_buffer_bytes) {
    return nullptr;
  }
  const IndexCacheKey key = {
      .physical_address = physical_address,
      .source_virtual_alias = draw.mesh.index_buffer_alias,
      .submitted_index_count = draw.submitted_index_count,
  };
  {
    std::lock_guard lock(g_cache_mutex);
    if (const auto cached = FindCached(g_index_cache, key)) {
      return cached;
    }
  }

  auto payload = std::make_shared<Venue14DIndexPayload>();
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
        LoadBeU16(payload->raw_bytes.data() +
                  static_cast<size_t>(index) * sizeof(uint16_t));
    if (decoded >= vertex_count) {
      return nullptr;
    }
    payload->indices[index] = decoded;
  }
  if (!payload->valid()) {
    return nullptr;
  }

  std::lock_guard lock(g_cache_mutex);
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

bool CaptureMaterial(uint8_t *guest_base,
                     const SceneCatalogDrawOccurrence &draw,
                     Venue14DMaterialSnapshot &material,
                     uint32_t &guest_read_failures,
                     uint32_t &texture_capture_failures) {
  std::array<uint32_t, Venue14DMaterialSnapshot::kTextureCount * 6>
      texture_words{};
  if (!CaptureBeWords(guest_base, draw.device, kFetchBankOffset,
                      texture_words)) {
    ++guest_read_failures;
  } else {
    for (size_t slot = 0; slot < material.texture_fetches.size(); ++slot) {
      std::copy_n(texture_words.begin() + slot * 6, 6,
                  material.texture_fetches[slot].begin());
    }
  }
  if (!CaptureBeWords(guest_base, draw.device, kVertexConstantBankOffset,
                      material.vertex_constant_words)) {
    ++guest_read_failures;
  }
  if (!CaptureBeWords(guest_base, draw.device, kPixelConstantBankOffset,
                      material.pixel_constant_words)) {
    ++guest_read_failures;
  }

  const bool fetches_valid =
      std::ranges::all_of(material.texture_fetches, [](const auto &fetch) {
        return (fetch[0] & 0x3u) == 2 && fetch[1] != 0;
      });
  for (uint32_t slot = 0; slot < material.textures.size(); ++slot) {
    material.textures[slot] = CaptureTextureSnapshot(
        guest_base, draw.pass.pixel_shader, slot,
        material.texture_fetches[slot], TextureMipCapture::kFullFetchRange);
    texture_capture_failures += material.textures[slot] == nullptr;
  }
  const bool textures_valid =
      std::ranges::all_of(material.textures, [](const auto &texture) {
        return texture != nullptr && texture->valid() &&
               texture->full_mip_chain();
      });
  material.vertex_declaration = draw.state.vertex_declaration;
  material.valid = guest_read_failures == 0 && fetches_valid &&
                   textures_valid && material.vertex_declaration != 0;
  return material.valid;
}

} // namespace

Venue14DTitleCapture
CaptureVenue14DTitleDraw(uint8_t *guest_base,
                         const SceneCatalogDrawOccurrence &draw) {
  Venue14DTitleCapture capture;
  if (guest_base == nullptr || draw.player != 0 || !draw.pass.valid ||
      !IsExactTitleProgram(draw) || !draw.mesh.valid ||
      draw.primitive_type != kTriangleStripPrimitive ||
      draw.mesh.aggregate_primitive_type != kTriangleStripPrimitive ||
      (draw.mesh.vertex_stride != kVertexStride40 &&
       draw.mesh.vertex_stride != kVertexStride48) ||
      draw.mesh.vertex_endian != kVenue14DVertexEndian ||
      draw.mesh.vertex_buffer_bytes < draw.mesh.vertex_stride ||
      draw.mesh.vertex_buffer_bytes % draw.mesh.vertex_stride != 0 ||
      draw.submitted_index_count == 0 ||
      draw.mesh.index_element_size != sizeof(uint16_t) ||
      draw.mesh.index_is_32_bit) {
    return capture;
  }

  const uint32_t index_base =
      PhysicalAddressForVirtualAlias(draw.mesh.index_buffer_alias);
  if (index_base == 0) {
    return capture;
  }
  capture.family_candidate = true;
  capture.identity = {
      .primitive_type = draw.primitive_type,
      .submitted_index_count = draw.submitted_index_count,
      .guest_index_base = index_base,
      .guest_vertex_base =
          PhysicalAddressForVirtualAlias(draw.mesh.vertex_buffer_alias),
      .guest_vertex_bytes = draw.mesh.vertex_buffer_bytes,
      .guest_vertex_endian = draw.mesh.vertex_endian,
  };

  auto snapshot = std::make_shared<Venue14DTitleDrawSnapshot>();
  snapshot->identity = capture.identity;
  snapshot->ordinal = draw.ordinal;
  snapshot->pass_descriptor = draw.pass.pass_descriptor;
  snapshot->program_pair = draw.pass.program_pair;
  snapshot->title_vertex_shader = draw.pass.vertex_shader;
  snapshot->title_pixel_shader = draw.pass.pixel_shader;
  snapshot->owner_kind = static_cast<uint32_t>(draw.owner.kind);
  snapshot->owner = draw.owner.owner;
  snapshot->owner_renderable = draw.owner.renderable;

  snapshot->vertices = CaptureVertices(guest_base, draw);
  capture.payload_copy_failures += snapshot->vertices == nullptr;
  if (snapshot->vertices != nullptr) {
    snapshot->indices =
        CaptureIndices(guest_base, draw, snapshot->vertices->vertex_count);
  }
  capture.payload_copy_failures += snapshot->indices == nullptr;
  CaptureMaterial(guest_base, draw, snapshot->material,
                  capture.guest_read_failures,
                  capture.texture_capture_failures);

  snapshot->valid =
      capture.guest_read_failures == 0 && capture.payload_copy_failures == 0 &&
      capture.texture_capture_failures == 0 && snapshot->identity.valid() &&
      snapshot->vertices != nullptr && snapshot->vertices->valid() &&
      snapshot->indices != nullptr && snapshot->indices->valid() &&
      snapshot->material.valid;
  if (snapshot->valid) {
    capture.snapshot = std::move(snapshot);
  }
  return capture;
}

} // namespace tabletennis::native
