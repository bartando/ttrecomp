#include "native/tabletennis_6ae_player_snapshot.h"

#include "generated/default/tabletennis_init.h"
#include "native/tabletennis_guest_memory.h"
#include "native/tabletennis_scene_draw_catalog.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
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
constexpr uint32_t kVertexStride = 36;
constexpr uint32_t kPaletteRecordStride = 28;
constexpr uint32_t kVertexEndian8In32 = 2;
constexpr uint32_t kTriangleStripPrimitive = 6;
constexpr uint32_t kTextureViewSwizzleShift = 1;
constexpr uint32_t kTextureViewSwizzleMask = 0xFFF;
constexpr uint32_t kMaterialTextureViewSwizzle = 0x688;
constexpr uint32_t kPhysicalAliasBase = 0xA0000000;
constexpr uint32_t kPhysicalAddressMask = 0x1FFFFFFF;
constexpr uint32_t kHighPhysicalHeapBase = 0xE0000000;
constexpr uint32_t kHighPhysicalHeapHostPageOffset = 0x1000;
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
constexpr std::array<uint32_t, 4> kInfluenceByteOrder = {2, 1, 0, 3};

constexpr std::array<float, 4> kExpectedVertexConstant255 = {
    1.0f, 255.00195f, 0.0f, 0.0f};
constexpr std::array<float, 4> kExpectedPixelConstant254 = {
    1.0f, 0.25f, 4.0f, 0.0f};
constexpr std::array<float, 4> kExpectedPixelConstant255 = {
    0.5f, -1.0f, 0.125f, 1.0f / 12.0f};

struct VertexCacheKey {
  uint32_t physical_address = 0;
  uint32_t size = 0;
  uint32_t virtual_alias = 0;

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
  uint32_t size = 0;

  bool operator==(const PaletteFrameKey&) const = default;
};

template <typename Key, typename Payload>
struct CachedPayload {
  Key key{};
  std::shared_ptr<const Payload> payload;
};

std::mutex g_cache_mutex;
std::vector<CachedPayload<VertexCacheKey, Player6AEVertexPayload>>
    g_vertex_cache;
std::vector<CachedPayload<IndexCacheKey, Player6AEIndexPayload>>
    g_index_cache;
std::vector<CachedPayload<PaletteFrameKey, Player6AEPalettePayload>>
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

uint32_t LoadBeU32(const uint8_t* source) {
  return LoadBeU32(reinterpret_cast<const std::byte*>(source));
}

float LoadBeF32(const uint8_t* source) {
  return std::bit_cast<float>(LoadBeU32(source));
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

template <size_t Size>
bool ApproximatelyEqual(const std::array<float, Size>& values,
                        const std::array<float, Size>& expected) {
  for (size_t index = 0; index < Size; ++index) {
    if (!std::isfinite(values[index])) {
      return false;
    }
    const float tolerance =
        0.00002f * std::max(1.0f, std::abs(expected[index]));
    if (std::abs(values[index] - expected[index]) > tolerance) {
      return false;
    }
  }
  return true;
}

uint64_t Fingerprint(const std::vector<uint8_t>& bytes) {
  uint64_t fingerprint = kFnvOffsetBasis;
  for (uint8_t value : bytes) {
    fingerprint = (fingerprint ^ value) * kFnvPrime;
  }
  return fingerprint;
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

Player6AEVertexFetch CaptureVertexFetch(uint8_t* guest_base,
                                        uint32_t device, uint32_t slot) {
  Player6AEVertexFetch fetch;
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

std::shared_ptr<const Player6AEVertexPayload> CaptureVertices(
    uint8_t* guest_base, const SceneCatalogDrawOccurrence& draw,
    const Player6AEVertexFetch& fetch) {
  if (!fetch.valid || fetch.endian != kVertexEndian8In32 ||
      fetch.size == 0 || fetch.size > kMaximumVertexPayloadBytes ||
      fetch.size % kVertexStride != 0 ||
      fetch.physical_address !=
          PhysicalAddressForVirtualAlias(draw.mesh.vertex_buffer_alias)) {
    return nullptr;
  }
  const VertexCacheKey key = {
      .physical_address = fetch.physical_address,
      .size = fetch.size,
      .virtual_alias = draw.mesh.vertex_buffer_alias,
  };
  {
    std::lock_guard lock(g_cache_mutex);
    if (const auto cached = FindCached(g_vertex_cache, key)) {
      return cached;
    }
  }

  auto payload = std::make_shared<Player6AEVertexPayload>();
  payload->fetch = fetch;
  payload->source_virtual_alias = draw.mesh.vertex_buffer_alias;
  payload->stride = kVertexStride;
  payload->vertex_count = fetch.size / kVertexStride;
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

std::shared_ptr<const Player6AEIndexPayload> CaptureIndices(
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

  auto payload = std::make_shared<Player6AEIndexPayload>();
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

bool ValidateReferencedPaletteRecords(
    const Player6AEVertexPayload& vertices,
    const Player6AEPalettePayload& palette,
    uint32_t& referenced_record_count) {
  std::vector<bool> referenced(palette.record_count, false);
  for (uint32_t vertex = 0; vertex < vertices.vertex_count; ++vertex) {
    const uint8_t* source =
        vertices.raw_bytes.data() +
        static_cast<size_t>(vertex) * vertices.stride;
    const uint32_t weight_word = LoadBeU32(source + 12);
    const uint32_t index_word = LoadBeU32(source + 16);
    for (size_t influence = 0; influence < kInfluenceByteOrder.size();
         ++influence) {
      const uint32_t shift = kInfluenceByteOrder[influence] * 8;
      if (((weight_word >> shift) & 0xFFu) == 0) {
        continue;
      }
      const uint32_t palette_index = (index_word >> shift) & 0xFFu;
      if (palette_index >= palette.record_count) {
        return false;
      }
      referenced[palette_index] = true;
    }
  }

  referenced_record_count = static_cast<uint32_t>(
      std::ranges::count(referenced, true));
  if (referenced_record_count == 0) {
    return false;
  }
  for (size_t record = 0; record < referenced.size(); ++record) {
    if (!referenced[record]) {
      continue;
    }
    const auto& quaternion = palette.records[record].quaternion;
    float norm_squared = 0.0f;
    for (float value : quaternion) {
      norm_squared += value * value;
    }
    if (!std::isfinite(norm_squared) || norm_squared < 0.9f * 0.9f ||
        norm_squared > 1.1f * 1.1f) {
      return false;
    }
  }
  return true;
}

std::shared_ptr<const Player6AEPalettePayload> CapturePalette(
    uint8_t* guest_base, uint32_t player,
    const Player6AEVertexFetch& fetch,
    const Player6AEVertexPayload& vertices) {
  if (player == 0 || !fetch.valid ||
      fetch.endian != kVertexEndian8In32 || fetch.size == 0 ||
      fetch.size > kMaximumPalettePayloadBytes ||
      fetch.size % kPaletteRecordStride != 0) {
    return nullptr;
  }
  const PaletteFrameKey key = {
      .player = player,
      .physical_address = fetch.physical_address,
      .size = fetch.size,
  };
  {
    std::lock_guard lock(g_cache_mutex);
    if (const auto cached = FindCached(g_frame_palette_cache, key)) {
      uint32_t referenced_record_count = 0;
      return ValidateReferencedPaletteRecords(
                 vertices, *cached, referenced_record_count)
                 ? cached
                 : nullptr;
    }
  }

  auto payload = std::make_shared<Player6AEPalettePayload>();
  payload->fetch = fetch;
  payload->record_count = fetch.size / kPaletteRecordStride;
  if (!CaptureStablePhysicalBytes(guest_base, fetch.physical_address,
                                  fetch.size, payload->raw_bytes)) {
    return nullptr;
  }
  payload->payload_fingerprint = Fingerprint(payload->raw_bytes);
  payload->records.resize(payload->record_count);
  for (uint32_t record = 0; record < payload->record_count; ++record) {
    const uint8_t* source =
        payload->raw_bytes.data() +
        static_cast<size_t>(record) * kPaletteRecordStride;
    Player6AEPaletteRecord& destination = payload->records[record];
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
    if (!AllFinite(destination.quaternion) ||
        !AllFinite(destination.translation)) {
      return nullptr;
    }
  }
  payload->referenced_records_valid =
      ValidateReferencedPaletteRecords(
          vertices, *payload, payload->referenced_record_count);
  if (!payload->valid()) {
    return nullptr;
  }

  std::lock_guard lock(g_cache_mutex);
  if (const auto cached = FindCached(g_frame_palette_cache, key)) {
    uint32_t referenced_record_count = 0;
    return ValidateReferencedPaletteRecords(
               vertices, *cached, referenced_record_count)
               ? cached
               : nullptr;
  }
  g_frame_palette_cache.push_back({key, payload});
  return payload;
}

bool CaptureMaterial(uint8_t* guest_base,
                     const SceneCatalogDrawOccurrence& draw,
                     Player6AEMaterialSnapshot& material,
                     uint32_t& guest_read_failures,
                     uint32_t& texture_capture_failures) {
  std::array<uint32_t,
             Player6AEMaterialSnapshot::kTextureBindingCount * 6>
      texture_words{};
  if (!CaptureBeWords(guest_base, draw.device, kFetchBankOffset,
                      texture_words)) {
    ++guest_read_failures;
  } else {
    for (size_t binding = 0; binding < material.texture_fetches.size();
         ++binding) {
      std::copy_n(texture_words.begin() + binding * 6, 6,
                  material.texture_fetches[binding].begin());
      material.texture_view_swizzles[binding] =
          (material.texture_fetches[binding][3] >>
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
                    material.vertex_constants_46_47);
  capture_constants(kVertexConstantBankOffset, 255,
                    material.vertex_constant_255);
  capture_constants(kPixelConstantBankOffset, 19,
                    material.pixel_constant_19);
  capture_constants(kPixelConstantBankOffset, 21,
                    material.pixel_constants_21_27);
  capture_constants(kPixelConstantBankOffset, 46,
                    material.pixel_constants_46_70);
  capture_constants(kPixelConstantBankOffset, 254,
                    material.pixel_constants_254_255);

  const bool fetches_valid = std::ranges::all_of(
      material.texture_fetches, [](const auto& fetch) {
        return (fetch[0] & 0x3u) == 2 && fetch[1] != 0;
      });
  for (size_t material_slot = 0;
       material_slot < material.material_textures.size(); ++material_slot) {
    const uint32_t binding =
        Player6AEMaterialSnapshot::kMaterialTextureBindings[material_slot];
    material.material_textures[material_slot] = CaptureTextureSnapshot(
        guest_base, draw.pass.pass_descriptor, binding,
        material.texture_fetches[binding]);
    texture_capture_failures +=
        material.material_textures[material_slot] == nullptr;
  }

  bool material_textures_valid = true;
  for (size_t material_slot = 0;
       material_slot < material.material_textures.size(); ++material_slot) {
    const uint32_t binding =
        Player6AEMaterialSnapshot::kMaterialTextureBindings[material_slot];
    const auto& texture = material.material_textures[material_slot];
    material_textures_valid &=
        texture != nullptr && texture->valid() &&
        texture->fetch_words == material.texture_fetches[binding] &&
        texture->fetch_swizzle == kMaterialTextureViewSwizzle &&
        material.texture_view_swizzles[binding] ==
            kMaterialTextureViewSwizzle;
  }

  std::array<float, 4> pixel_constant_254{};
  std::array<float, 4> pixel_constant_255{};
  std::copy_n(material.pixel_constants_254_255.begin(), 4,
              pixel_constant_254.begin());
  std::copy_n(material.pixel_constants_254_255.begin() + 4, 4,
              pixel_constant_255.begin());
  material.literal_contract_valid =
      ApproximatelyEqual(material.vertex_constant_255,
                         kExpectedVertexConstant255) &&
      ApproximatelyEqual(pixel_constant_254, kExpectedPixelConstant254) &&
      ApproximatelyEqual(pixel_constant_255, kExpectedPixelConstant255);
  material.valid =
      guest_read_failures == 0 && fetches_valid &&
      material_textures_valid && material.literal_contract_valid &&
      AllFinite(material.vertex_constants_12_15) &&
      AllFinite(material.vertex_constant_19) &&
      AllFinite(material.vertex_constants_29_36) &&
      AllFinite(material.vertex_constants_46_47) &&
      AllFinite(material.vertex_constant_255) &&
      AllFinite(material.pixel_constant_19) &&
      AllFinite(material.pixel_constants_21_27) &&
      AllFinite(material.pixel_constants_46_70) &&
      AllFinite(material.pixel_constants_254_255);
  return material.valid;
}

}  // namespace

bool IsStructuralPlayer6AETitleDraw(
    const SceneCatalogDrawOccurrence& draw) {
  return draw.player != 0 && draw.pass.valid && draw.mesh.valid &&
         draw.pass.pass_descriptor != 0 && draw.pass.program_pair != 0 &&
         draw.pass.vertex_shader != 0 && draw.pass.pixel_shader != 0 &&
         draw.primitive_type == kTriangleStripPrimitive &&
         draw.mesh.aggregate_primitive_type == kTriangleStripPrimitive &&
         draw.submitted_index_count != 0 &&
         draw.mesh.index_element_size == sizeof(uint16_t) &&
         !draw.mesh.index_is_32_bit &&
         draw.mesh.vertex_stride == kVertexStride;
}

Player6AETitleCapture CapturePlayer6AETitleDraw(
    uint8_t* guest_base, const SceneCatalogDrawOccurrence& draw) {
  Player6AETitleCapture capture;
  if (guest_base == nullptr || !IsStructuralPlayer6AETitleDraw(draw)) {
    return capture;
  }
  capture.family_candidate = true;

  capture.primary_fetch =
      CaptureVertexFetch(guest_base, draw.device, kPrimaryVertexFetchSlot);
  capture.palette_fetch =
      CaptureVertexFetch(guest_base, draw.device, kPaletteVertexFetchSlot);
  const Player6AEVertexFetch& vertices = capture.primary_fetch;
  const Player6AEVertexFetch& palette = capture.palette_fetch;
  capture.guest_read_failures += !vertices.valid;
  capture.guest_read_failures += !palette.valid;
  capture.expected_vertex_physical_address =
      PhysicalAddressForVirtualAlias(draw.mesh.vertex_buffer_alias);
  capture.index_physical_address =
      PhysicalAddressForVirtualAlias(draw.mesh.index_buffer_alias);
  capture.fetch_contract_valid =
      capture.guest_read_failures == 0 &&
      vertices.endian == kVertexEndian8In32 &&
      vertices.size % kVertexStride == 0 &&
      vertices.physical_address ==
          capture.expected_vertex_physical_address &&
      palette.endian == kVertexEndian8In32 &&
      palette.size != 0 && palette.size % kPaletteRecordStride == 0 &&
      capture.index_physical_address != 0;
  if (!capture.fetch_contract_valid) {
    return capture;
  }

  capture.identity = {
      .primitive_type = draw.primitive_type,
      .submitted_index_count = draw.submitted_index_count,
      .guest_index_base = capture.index_physical_address,
  };

  auto snapshot = std::make_shared<Player6AEDrawSnapshot>();
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
  snapshot->alternate_pass = draw.scope.alternate_pass;

  snapshot->vertices = CaptureVertices(guest_base, draw, vertices);
  capture.payload_copy_failures += snapshot->vertices == nullptr;
  if (snapshot->vertices != nullptr) {
    snapshot->indices =
        CaptureIndices(guest_base, draw, snapshot->vertices->vertex_count);
    snapshot->palette =
        CapturePalette(guest_base, draw.player, palette, *snapshot->vertices);
  }
  capture.payload_copy_failures += snapshot->indices == nullptr;
  capture.payload_copy_failures += snapshot->palette == nullptr;
  CaptureMaterial(guest_base, draw, snapshot->material,
                  capture.guest_read_failures,
                  capture.texture_capture_failures);

  snapshot->valid =
      capture.guest_read_failures == 0 &&
      capture.payload_copy_failures == 0 &&
      capture.texture_capture_failures == 0 &&
      snapshot->vertices != nullptr && snapshot->vertices->valid() &&
      snapshot->indices != nullptr && snapshot->indices->valid() &&
      snapshot->palette != nullptr && snapshot->palette->valid() &&
      snapshot->material.valid && snapshot->identity.valid() &&
      snapshot->player != 0;
  if (snapshot->valid) {
    capture.snapshot = std::move(snapshot);
  }
  return capture;
}

void Player6AESnapshotFrameEnd() {
  std::lock_guard lock(g_cache_mutex);
  g_frame_palette_cache.clear();
}

}  // namespace tabletennis::native
