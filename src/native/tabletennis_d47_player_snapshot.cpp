#include "native/tabletennis_d47_player_snapshot.h"

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

namespace tabletennis::native {
namespace {

constexpr uint32_t kFetchBankOffset = 0x480;
constexpr uint32_t kVertexConstantBankOffset = 0x780;
constexpr uint32_t kPixelConstantBankOffset = 0x1780;
constexpr uint32_t kPrimaryVertexFetchSlot = 95;
constexpr uint32_t kPaletteVertexFetchSlot = 92;
constexpr uint32_t kVertexEndian8In32 = 2;
constexpr uint32_t kPaletteRecordBytes = 28;
constexpr uint32_t kMaximumPaletteRecordsPerHalf = 256;
constexpr uint32_t kTriangleStripPrimitive = 6;
constexpr uint32_t kPhysicalAliasBase = 0xA0000000;
constexpr uint32_t kPhysicalAddressMask = 0x1FFFFFFF;
constexpr uint32_t kHighPhysicalHeapBase = 0xE0000000;
constexpr uint32_t kHighPhysicalHeapHostPageOffset = 0x1000;
constexpr uint32_t kTextureViewSwizzleShift = 1;
constexpr uint32_t kTextureViewSwizzleMask = 0xFFF;
constexpr size_t kConstantRowBytes = sizeof(float) * 4;
constexpr size_t kMaximumVertexPayloadBytes = 64 * 1024 * 1024;
constexpr size_t kMaximumIndexPayloadBytes = 16 * 1024 * 1024;
constexpr size_t kMaximumPalettePayloadBytes = 64 * 1024;
constexpr size_t kMaximumCachedVertices = 256;
constexpr size_t kMaximumCachedIndices = 512;
constexpr size_t kMaximumVertexCacheBytes = 128 * 1024 * 1024;
constexpr size_t kMaximumIndexCacheBytes = 64 * 1024 * 1024;
constexpr uint64_t kFnvOffsetBasis = 1469598103934665603ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;

struct VertexCacheKey {
  uint32_t physical_address = 0;
  uint32_t size = 0;
  uint32_t virtual_alias = 0;
  uint32_t stride = 0;

  bool operator==(const VertexCacheKey&) const = default;
};

struct IndexCacheKey {
  uint32_t physical_address = 0;
  uint32_t virtual_alias = 0;
  uint32_t submitted_index_count = 0;

  bool operator==(const IndexCacheKey&) const = default;
};

struct PaletteFrameKey {
  uint32_t player = 0;
  uint32_t physical_address = 0;
  std::array<uint64_t, 2> write_sequences{};

  bool operator==(const PaletteFrameKey&) const = default;
};

template <typename Key, typename Payload>
struct CachedPayload {
  Key key{};
  std::shared_ptr<const Payload> payload;
};

std::mutex g_cache_mutex;
std::vector<CachedPayload<VertexCacheKey, D47PlayerVertexPayload>>
    g_vertex_cache;
std::vector<CachedPayload<IndexCacheKey, D47PlayerIndexPayload>>
    g_index_cache;
std::vector<CachedPayload<PaletteFrameKey, D47PlayerPalettePayload>>
    g_frame_palette_cache;
size_t g_vertex_cache_bytes = 0;
size_t g_index_cache_bytes = 0;

bool CheckedGuestOffset(uint32_t address, size_t offset, size_t size,
                        uint32_t& result) {
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

bool TryCopyGuest(uint8_t* guest_base, uint32_t address, size_t offset,
                  void* destination, size_t size) {
  uint32_t guest_address = 0;
  return guest_base != nullptr && destination != nullptr &&
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

template <size_t Size>
bool CaptureBeWords(uint8_t* guest_base, uint32_t address, size_t offset,
                    std::array<uint32_t, Size>& words) {
  std::array<std::byte, Size * sizeof(uint32_t)> bytes{};
  if (!TryCopyGuest(guest_base, address, offset, bytes.data(), bytes.size())) {
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
  std::array<uint32_t, Size> words{};
  if (!CaptureBeWords(guest_base, address, offset, words)) {
    return false;
  }
  for (size_t index = 0; index < Size; ++index) {
    values[index] = std::bit_cast<float>(words[index]);
  }
  return true;
}

template <size_t Size>
bool AllFinite(const std::array<float, Size>& values) {
  return std::ranges::all_of(
      values, [](float value) { return std::isfinite(value); });
}

uint64_t Fingerprint(const uint8_t* bytes, size_t size) {
  uint64_t fingerprint = kFnvOffsetBasis;
  for (size_t index = 0; index < size; ++index) {
    fingerprint = (fingerprint ^ bytes[index]) * kFnvPrime;
  }
  return fingerprint;
}

uint64_t Fingerprint(const std::vector<uint8_t>& bytes) {
  return Fingerprint(bytes.data(), bytes.size());
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

uint32_t PaletteRecordsPerHalf(uint32_t fetch_byte_count) {
  constexpr uint32_t kBytesPerRecordPair = kPaletteRecordBytes * 2;
  if (fetch_byte_count == 0 ||
      fetch_byte_count % kBytesPerRecordPair != 0) {
    return 0;
  }
  const uint32_t record_count =
      fetch_byte_count / kBytesPerRecordPair;
  return record_count >= 1 &&
                 record_count <= kMaximumPaletteRecordsPerHalf
             ? record_count
             : 0;
}

D47PlayerVertexFetch CaptureVertexFetch(uint8_t* guest_base,
                                        uint32_t device, uint32_t slot) {
  D47PlayerVertexFetch fetch;
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
  const auto found = std::ranges::find_if(
      cache, [&](const auto& entry) { return entry.key == key; });
  return found == cache.end() ? nullptr : found->payload;
}

std::shared_ptr<const D47PlayerVertexPayload> CaptureVertices(
    uint8_t* guest_base, const SceneCatalogDrawOccurrence& draw,
    const D47PlayerVertexFetch& fetch) {
  const uint32_t stride = draw.mesh.vertex_stride;
  if (!fetch.valid || fetch.endian != kVertexEndian8In32 ||
      (stride != 36 && stride != 44) || fetch.size == 0 ||
      fetch.size > kMaximumVertexPayloadBytes || fetch.size % stride != 0 ||
      fetch.physical_address !=
          PhysicalAddressForVirtualAlias(draw.mesh.vertex_buffer_alias)) {
    return nullptr;
  }
  const VertexCacheKey key = {
      .physical_address = fetch.physical_address,
      .size = fetch.size,
      .virtual_alias = draw.mesh.vertex_buffer_alias,
      .stride = stride,
  };
  {
    std::lock_guard lock(g_cache_mutex);
    if (const auto cached = FindCached(g_vertex_cache, key)) {
      return cached;
    }
  }

  auto payload = std::make_shared<D47PlayerVertexPayload>();
  payload->fetch = fetch;
  payload->source_virtual_alias = draw.mesh.vertex_buffer_alias;
  payload->stride = stride;
  payload->vertex_count = fetch.size / stride;
  if (!CaptureStablePhysicalBytes(guest_base, fetch.physical_address,
                                  fetch.size, payload->raw_bytes)) {
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

std::shared_ptr<const D47PlayerIndexPayload> CaptureIndices(
    uint8_t* guest_base, const SceneCatalogDrawOccurrence& draw,
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
      .virtual_alias = draw.mesh.index_buffer_alias,
      .submitted_index_count = draw.submitted_index_count,
  };
  {
    std::lock_guard lock(g_cache_mutex);
    if (const auto cached = FindCached(g_index_cache, key)) {
      return cached;
    }
  }

  auto payload = std::make_shared<D47PlayerIndexPayload>();
  payload->source_virtual_alias = draw.mesh.index_buffer_alias;
  payload->physical_address = physical_address;
  payload->submitted_index_count = draw.submitted_index_count;
  if (!CaptureStablePhysicalBytes(
          guest_base, physical_address, static_cast<size_t>(requested_size),
          payload->raw_bytes)) {
    return nullptr;
  }
  payload->payload_fingerprint = Fingerprint(payload->raw_bytes);
  payload->indices.resize(draw.submitted_index_count);
  for (uint32_t index = 0; index < draw.submitted_index_count; ++index) {
    const uint16_t decoded = LoadBeU16(
        payload->raw_bytes.data() +
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

std::shared_ptr<const D47PlayerPalettePayload> CapturePalette(
    uint8_t* guest_base, const D47PlayerVertexFetch& fetch,
    const D47PaletteWritePairProof& write_proof) {
  const uint32_t records_per_half =
      PaletteRecordsPerHalf(fetch.size);
  if (!fetch.valid || fetch.endian != kVertexEndian8In32 ||
      records_per_half == 0 ||
      fetch.size > kMaximumPalettePayloadBytes) {
    return nullptr;
  }
  if (write_proof.valid &&
      (!write_proof.primary_cache_valid ||
       !write_proof.alternate_cache_valid ||
       write_proof.physical_fetch_base != fetch.physical_address ||
       write_proof.fetch_byte_count != fetch.size ||
       write_proof.record_count_per_half != records_per_half)) {
    return nullptr;
  }
  const PaletteFrameKey key = {
      .player = write_proof.valid ? write_proof.player : 0,
      .physical_address = fetch.physical_address,
      .write_sequences = write_proof.write_sequences,
  };
  {
    std::lock_guard lock(g_cache_mutex);
    if (const auto cached = FindCached(g_frame_palette_cache, key)) {
      return cached;
    }
  }

  auto payload = std::make_shared<D47PlayerPalettePayload>();
  payload->fetch = fetch;
  payload->write_proof = write_proof;
  payload->record_count = records_per_half * 2;
  if (!CaptureStablePhysicalBytes(guest_base, fetch.physical_address,
                                  fetch.size, payload->raw_bytes)) {
    return nullptr;
  }
  const size_t half_bytes =
      static_cast<size_t>(records_per_half) * kPaletteRecordBytes;
  if (write_proof.valid && write_proof.write_sequences[0] != 0 &&
      Fingerprint(payload->raw_bytes.data(), half_bytes) !=
          write_proof.payload_fingerprints[0]) {
    return nullptr;
  }
  payload->payload_fingerprint = Fingerprint(payload->raw_bytes);
  payload->records.resize(payload->record_count);
  for (uint32_t record = 0; record < payload->record_count; ++record) {
    const uint8_t* source =
        payload->raw_bytes.data() +
        static_cast<size_t>(record) * kPaletteRecordBytes;
    D47PlayerPaletteRecord& destination = payload->records[record];
    float quaternion_norm_squared = 0.0f;
    for (size_t component = 0; component < destination.quaternion.size();
         ++component) {
      const float value = LoadBeF32(source + component * sizeof(float));
      if (!std::isfinite(value)) {
        return nullptr;
      }
      destination.quaternion[component] = value;
      quaternion_norm_squared += value * value;
    }
    for (size_t component = 0; component < destination.translation.size();
         ++component) {
      const float value =
          LoadBeF32(source + 16 + component * sizeof(float));
      if (!std::isfinite(value)) {
        return nullptr;
      }
      destination.translation[component] = value;
    }
    if (quaternion_norm_squared < 0.95f * 0.95f ||
        quaternion_norm_squared > 1.05f * 1.05f) {
      return nullptr;
    }
  }
  const bool payload_shape_valid =
      payload->fetch.valid && payload->record_count != 0 &&
      payload->raw_bytes.size() ==
          static_cast<size_t>(payload->record_count) * kPaletteRecordBytes &&
      payload->records.size() == payload->record_count;
  if (!payload_shape_valid || (write_proof.valid && !payload->valid())) {
    return nullptr;
  }

  std::lock_guard lock(g_cache_mutex);
  if (const auto cached = FindCached(g_frame_palette_cache, key)) {
    return cached;
  }
  g_frame_palette_cache.push_back({key, payload});
  return payload;
}

bool CaptureMaterial(uint8_t* guest_base,
                     const SceneCatalogDrawOccurrence& draw,
                     D47PlayerMaterialSnapshot& material,
                     uint32_t& guest_read_failures,
                     uint32_t& texture_capture_failures) {
  std::array<uint32_t,
             D47PlayerMaterialSnapshot::kTextureBindingCount * 6>
      texture_words{};
  if (!CaptureBeWords(guest_base, draw.device, kFetchBankOffset,
                      texture_words)) {
    ++guest_read_failures;
  } else {
    for (size_t slot = 0; slot < material.texture_fetches.size(); ++slot) {
      std::copy_n(texture_words.begin() + slot * 6, 6,
                  material.texture_fetches[slot].begin());
      material.texture_view_swizzles[slot] =
          (material.texture_fetches[slot][3] >>
           kTextureViewSwizzleShift) &
          kTextureViewSwizzleMask;
    }
  }

  auto capture_constants = [&](size_t bank_offset, size_t first_row,
                               auto& destination) {
    if (!CaptureBeFloats(
            guest_base, draw.device,
            bank_offset + first_row * kConstantRowBytes, destination)) {
      ++guest_read_failures;
    }
  };
  capture_constants(kVertexConstantBankOffset, 12,
                    material.vertex_constants_12_15);
  capture_constants(kVertexConstantBankOffset, 19,
                    material.vertex_constant_19);
  capture_constants(kVertexConstantBankOffset, 29,
                    material.vertex_constants_29_36);
  capture_constants(kVertexConstantBankOffset, 46,
                    material.vertex_constants_46_48);
  capture_constants(kVertexConstantBankOffset, 255,
                    material.vertex_constant_255);
  capture_constants(kPixelConstantBankOffset, 19,
                    material.pixel_constant_19);
  capture_constants(kPixelConstantBankOffset, 21,
                    material.pixel_constants_21_27);
  capture_constants(kPixelConstantBankOffset, 46,
                    material.pixel_constants_46_77);
  capture_constants(kPixelConstantBankOffset, 252,
                    material.pixel_constants_252_255);

  const bool fetches_valid = std::ranges::all_of(
      material.texture_fetches, [](const auto& fetch) {
        return (fetch[0] & 0x3u) == 2 && fetch[1] != 0;
      });
  for (uint32_t slot = 0; slot < material.material_textures.size(); ++slot) {
    material.material_textures[slot] = CaptureTextureSnapshot(
        guest_base, draw.scope.shader, slot, material.texture_fetches[slot]);
    texture_capture_failures +=
        material.material_textures[slot] == nullptr;
  }

  const bool material_textures_valid = std::ranges::all_of(
      material.material_textures, [](const auto& texture) {
        return texture != nullptr && texture->valid();
      });
  material.valid =
      guest_read_failures == 0 && fetches_valid &&
      material_textures_valid &&
      AllFinite(material.vertex_constants_12_15) &&
      AllFinite(material.vertex_constant_19) &&
      AllFinite(material.vertex_constants_29_36) &&
      AllFinite(material.vertex_constants_46_48) &&
      AllFinite(material.vertex_constant_255) &&
      AllFinite(material.pixel_constant_19) &&
      AllFinite(material.pixel_constants_21_27) &&
      AllFinite(material.pixel_constants_46_77) &&
      AllFinite(material.pixel_constants_252_255);
  return material.valid;
}

}  // namespace

bool IsStructuralD47PlayerTitleDraw(
    const SceneCatalogDrawOccurrence& draw) {
  return draw.player != 0 && draw.pass.valid && draw.mesh.valid &&
         draw.pass.pass_descriptor != 0 && draw.pass.program_pair != 0 &&
         draw.pass.vertex_shader != 0 && draw.pass.pixel_shader != 0 &&
         draw.primitive_type == kTriangleStripPrimitive &&
         draw.mesh.aggregate_primitive_type == kTriangleStripPrimitive &&
         draw.submitted_index_count != 0 &&
         draw.mesh.index_element_size == sizeof(uint16_t) &&
         !draw.mesh.index_is_32_bit &&
         (draw.mesh.vertex_stride == 36 || draw.mesh.vertex_stride == 44);
}

D47PlayerTitleCapture CaptureD47PlayerTitleDraw(
    uint8_t* guest_base, const SceneCatalogDrawOccurrence& draw) {
  D47PlayerTitleCapture capture;
  if (guest_base == nullptr || !IsStructuralD47PlayerTitleDraw(draw)) {
    return capture;
  }

  const D47PlayerVertexFetch vertices =
      CaptureVertexFetch(guest_base, draw.device, kPrimaryVertexFetchSlot);
  const D47PlayerVertexFetch palette =
      CaptureVertexFetch(guest_base, draw.device, kPaletteVertexFetchSlot);
  capture.diagnostic.guest_read_failures += !vertices.valid;
  capture.diagnostic.guest_read_failures += !palette.valid;
  const uint32_t palette_records_per_half =
      PaletteRecordsPerHalf(palette.size);
  const uint32_t index_base =
      PhysicalAddressForVirtualAlias(draw.mesh.index_buffer_alias);
  const bool fetch_contract_valid =
      capture.diagnostic.guest_read_failures == 0 &&
      vertices.endian == kVertexEndian8In32 &&
      vertices.size % draw.mesh.vertex_stride == 0 &&
      vertices.physical_address ==
          PhysicalAddressForVirtualAlias(draw.mesh.vertex_buffer_alias) &&
      palette.endian == kVertexEndian8In32 &&
      palette_records_per_half != 0 && index_base != 0;
  if (!fetch_contract_valid) {
    return capture;
  }

  capture.family_candidate = true;
  capture.identity = {
      .primitive_type = draw.primitive_type,
      .submitted_index_count = draw.submitted_index_count,
      .guest_index_base = index_base,
  };

  const D47PaletteWritePairProof write_proof =
      CurrentD47PaletteWritePairProof(draw.player, palette.physical_address,
                                      palette.size);
  if (!write_proof.valid) {
    capture.diagnostic.failure_mask |=
        D47PlayerTitleCaptureDiagnostic::kCacheProofFailure;
  }

  auto snapshot = std::make_shared<D47PlayerDrawSnapshot>();
  snapshot->identity = capture.identity;
  snapshot->ordinal = draw.ordinal;
  snapshot->player = draw.player;
  snapshot->shader = draw.scope.shader;
  snapshot->model = draw.scope.model;
  snapshot->geometry_index = draw.scope.geometry_index;
  snapshot->pass_descriptor = draw.pass.pass_descriptor;
  snapshot->program_pair = draw.pass.program_pair;
  snapshot->title_vertex_shader = draw.pass.vertex_shader;
  snapshot->title_pixel_shader = draw.pass.pixel_shader;
  snapshot->vertex_stride = draw.mesh.vertex_stride;
  snapshot->alternate_pass = draw.scope.alternate_pass;
  snapshot->palette_fetch = palette;

  snapshot->vertices = CaptureVertices(guest_base, draw, vertices);
  capture.diagnostic.payload_copy_failures +=
      snapshot->vertices == nullptr;
  if (snapshot->vertices == nullptr) {
    capture.diagnostic.failure_mask |=
        D47PlayerTitleCaptureDiagnostic::kVertexPayloadFailure;
  }
  if (snapshot->vertices != nullptr) {
    snapshot->indices =
        CaptureIndices(guest_base, draw, snapshot->vertices->vertex_count);
    if (snapshot->indices == nullptr) {
      capture.diagnostic.failure_mask |=
          D47PlayerTitleCaptureDiagnostic::kIndexPayloadFailure;
    }
  }
  capture.diagnostic.payload_copy_failures +=
      snapshot->indices == nullptr;
  if (write_proof.valid) {
    snapshot->palette = CapturePalette(guest_base, palette, write_proof);
    capture.diagnostic.payload_copy_failures +=
        snapshot->palette == nullptr;
    if (snapshot->palette == nullptr) {
      capture.diagnostic.failure_mask |=
          D47PlayerTitleCaptureDiagnostic::kPalettePayloadFailure;
    }
  }
  if (!CaptureMaterial(guest_base, draw, snapshot->material,
                       capture.diagnostic.guest_read_failures,
                       capture.diagnostic.texture_capture_failures)) {
    capture.diagnostic.failure_mask |=
        D47PlayerTitleCaptureDiagnostic::kMaterialFailure;
  }
  if (capture.diagnostic.guest_read_failures != 0) {
    capture.diagnostic.failure_mask |=
        D47PlayerTitleCaptureDiagnostic::kGuestReadFailure;
  }
  if (capture.diagnostic.texture_capture_failures != 0) {
    capture.diagnostic.failure_mask |=
        D47PlayerTitleCaptureDiagnostic::kTextureFailure;
  }

  const bool captured_payloads_valid =
      capture.diagnostic.guest_read_failures == 0 &&
      capture.diagnostic.payload_copy_failures == 0 &&
      capture.diagnostic.texture_capture_failures == 0 &&
      snapshot->vertices != nullptr && snapshot->vertices->valid() &&
      snapshot->indices != nullptr && snapshot->indices->valid() &&
      (!write_proof.valid || snapshot->palette != nullptr) &&
      snapshot->material.valid && snapshot->identity.valid() &&
      snapshot->player != 0;
  snapshot->valid =
      captured_payloads_valid && write_proof.valid &&
      snapshot->palette->valid();
  if (captured_payloads_valid) {
    capture.snapshot = std::move(snapshot);
  }
  return capture;
}

std::shared_ptr<const D47PlayerDrawSnapshot>
FinalizeD47PlayerDrawSnapshot(
    uint8_t* guest_base, const D47PlayerDrawSnapshot& captured,
    const D47PaletteWritePairProof& write_proof) {
  if (!write_proof.valid || !write_proof.primary_cache_valid ||
      !write_proof.alternate_cache_valid || guest_base == nullptr ||
      captured.player == 0 || write_proof.player != captured.player ||
      write_proof.physical_fetch_base !=
          captured.palette_fetch.physical_address ||
      write_proof.fetch_byte_count != captured.palette_fetch.size) {
    return nullptr;
  }

  const std::shared_ptr<const D47PlayerPalettePayload> palette =
      CapturePalette(guest_base, captured.palette_fetch, write_proof);
  if (palette == nullptr || !palette->valid()) {
    return nullptr;
  }

  auto finalized = std::make_shared<D47PlayerDrawSnapshot>(captured);
  finalized->palette = palette;
  finalized->valid =
      finalized->vertices != nullptr && finalized->vertices->valid() &&
      finalized->indices != nullptr && finalized->indices->valid() &&
      finalized->palette->valid() && finalized->material.valid &&
      finalized->identity.valid() && finalized->player != 0;
  return finalized->valid ? std::move(finalized) : nullptr;
}

void D47PlayerSnapshotFrameEnd() {
  std::lock_guard lock(g_cache_mutex);
  g_frame_palette_cache.clear();
}

}  // namespace tabletennis::native
