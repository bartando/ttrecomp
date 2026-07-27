#include "native/tabletennis_venue_e33_snapshot.h"

#include "generated/default/tabletennis_init.h"
#include "native/tabletennis_guest_memory.h"
#include "native/tabletennis_scene_draw_catalog.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstring>
#include <limits>
#include <mutex>
#include <utility>

#include <rex/graphics/xenos.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/logging.h>

namespace tabletennis::native {
namespace {

namespace xenos = rex::graphics::xenos;

constexpr uint32_t kFetchBankOffset = 0x480;
constexpr uint32_t kVertexConstantBankOffset = 0x780;
constexpr uint32_t kPixelConstantBankOffset = 0x1780;
constexpr uint32_t kCapturedBankEndOffset = 0x2780;
constexpr uint32_t kTriangleStripPrimitive = 6;
constexpr uint32_t kPhysicalAliasBase = 0xA0000000;
constexpr uint32_t kPhysicalAddressMask = 0x1FFFFFFF;
constexpr uint32_t kHighPhysicalHeapBase = 0xE0000000;
constexpr uint32_t kHighPhysicalHeapHostPageOffset = 0x1000;
constexpr size_t kMaximumVertexPayloadBytes = 64 * 1024 * 1024;
constexpr size_t kMaximumIndexPayloadBytes = 16 * 1024 * 1024;
constexpr size_t kMaximumCachedVertices = 256;
constexpr size_t kMaximumCachedIndices = 512;
constexpr size_t kMaximumVertexCacheBytes = 128 * 1024 * 1024;
constexpr size_t kMaximumIndexCacheBytes = 64 * 1024 * 1024;
constexpr uint32_t kMaximumPayloadDiagnosticLogs = 8;
constexpr uint32_t kMaximumRendererMaterialDiagnosticLogs = 6;
constexpr uint64_t kFnvOffsetBasis = 1469598103934665603ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;

struct VertexCacheKey {
  uint32_t physical_address = 0;
  uint32_t byte_count = 0;
  uint32_t source_virtual_alias = 0;

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
std::vector<CachedPayload<VertexCacheKey, VenueE33VertexPayload>>
    g_vertex_cache;
std::vector<CachedPayload<IndexCacheKey, VenueE33IndexPayload>> g_index_cache;
size_t g_vertex_cache_bytes = 0;
size_t g_index_cache_bytes = 0;
std::atomic<uint32_t> g_payload_diagnostic_logs = 0;
std::atomic<uint32_t> g_renderer_material_diagnostic_logs = 0;

enum class IndexCaptureFailure {
  kNone,
  kMetadata,
  kGuestCopy,
  kVertexRange,
  kPayloadInvariant,
  kCacheCapacity,
};

struct IndexCaptureResult {
  std::shared_ptr<const VenueE33IndexPayload> payload;
  IndexCaptureFailure failure = IndexCaptureFailure::kNone;
  uint32_t physical_address = 0;
  uint32_t vertex_count = 0;
  uint16_t minimum_index = UINT16_MAX;
  uint16_t maximum_index = 0;
  uint16_t little_endian_maximum = 0;
  uint32_t first_out_of_range_position = UINT32_MAX;
  uint16_t first_out_of_range_value = 0;
};

const char *IndexCaptureFailureName(IndexCaptureFailure failure) {
  switch (failure) {
  case IndexCaptureFailure::kNone:
    return "none";
  case IndexCaptureFailure::kMetadata:
    return "metadata";
  case IndexCaptureFailure::kGuestCopy:
    return "guest-copy";
  case IndexCaptureFailure::kVertexRange:
    return "vertex-range";
  case IndexCaptureFailure::kPayloadInvariant:
    return "payload-invariant";
  case IndexCaptureFailure::kCacheCapacity:
    return "cache-capacity";
  }
  return "unknown";
}

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

template <typename Byte>
bool CaptureStableGuestBytes(uint8_t *guest_base, uint32_t address,
                             size_t offset, std::vector<Byte> &bytes,
                             size_t size) {
  if (size == 0) {
    return false;
  }
  bytes.resize(size);
  std::vector<Byte> verification(size);
  for (uint32_t attempt = 0; attempt < 4; ++attempt) {
    if (!TryCopyGuest(guest_base, address, offset, bytes.data(), size) ||
        !TryCopyGuest(guest_base, address, offset, verification.data(), size)) {
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

bool CaptureStablePhysicalBytes(uint8_t *guest_base, uint32_t physical_address,
                                size_t size, std::vector<uint8_t> &bytes) {
  if (size == 0) {
    return false;
  }
  bytes.resize(size);
  std::vector<uint8_t> verification(size);
  for (uint32_t attempt = 0; attempt < 4; ++attempt) {
    if (!TryCopyGuestPhysical(guest_base, physical_address, bytes.data(),
                              size) ||
        !TryCopyGuestPhysical(guest_base, physical_address, verification.data(),
                              size)) {
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

template <typename Key, typename Payload>
std::shared_ptr<const Payload>
FindCached(const std::vector<CachedPayload<Key, Payload>> &cache,
           const Key &key) {
  const auto found = std::ranges::find_if(
      cache, [&](const auto &entry) { return entry.key == key; });
  return found == cache.end() ? nullptr : found->payload;
}

bool IsStructuralCandidate(const SceneCatalogDrawOccurrence &draw) {
  const uint64_t requested_index_bytes =
      static_cast<uint64_t>(draw.submitted_index_count) * sizeof(uint16_t);
  return draw.player == 0 && draw.pass.valid && draw.mesh.valid &&
         draw.pass.pass_descriptor != 0 && draw.pass.program_pair != 0 &&
         draw.pass.vertex_shader != 0 && draw.pass.pixel_shader != 0 &&
         draw.primitive_type == kTriangleStripPrimitive &&
         draw.mesh.aggregate_primitive_type == kTriangleStripPrimitive &&
         draw.submitted_index_count != 0 &&
         draw.mesh.vertex_stride == VenueE33VertexPayload::kStride &&
         draw.mesh.vertex_endian == VenueE33VertexPayload::kEndian8In32 &&
         draw.mesh.vertex_buffer_bytes != 0 &&
         draw.mesh.vertex_buffer_bytes % VenueE33VertexPayload::kStride == 0 &&
         draw.mesh.index_element_size == sizeof(uint16_t) &&
         !draw.mesh.index_is_32_bit && requested_index_bytes != 0 &&
         requested_index_bytes <= draw.mesh.index_buffer_bytes;
}

std::shared_ptr<const VenueE33VertexPayload>
CaptureVertices(uint8_t *guest_base, const SceneCatalogDrawOccurrence &draw) {
  const auto &mesh = draw.mesh;
  const uint32_t physical_address =
      PhysicalAddressForVirtualAlias(mesh.vertex_buffer_alias);
  if (physical_address == 0 || mesh.vertex_buffer_bytes == 0 ||
      mesh.vertex_buffer_bytes > kMaximumVertexPayloadBytes ||
      mesh.vertex_buffer_bytes % VenueE33VertexPayload::kStride != 0) {
    return nullptr;
  }
  const VertexCacheKey key = {
      .physical_address = physical_address,
      .byte_count = mesh.vertex_buffer_bytes,
      .source_virtual_alias = mesh.vertex_buffer_alias,
  };

  auto payload = std::make_shared<VenueE33VertexPayload>();
  payload->source_virtual_alias = mesh.vertex_buffer_alias;
  payload->physical_address = physical_address;
  payload->byte_count = mesh.vertex_buffer_bytes;
  payload->vertex_count =
      mesh.vertex_buffer_bytes / VenueE33VertexPayload::kStride;
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
    if (cached->payload_fingerprint == payload->payload_fingerprint &&
        cached->raw_bytes == payload->raw_bytes) {
      return cached;
    }
    const auto found = std::ranges::find_if(
        g_vertex_cache, [&](const auto &entry) { return entry.key == key; });
    const size_t retained_bytes =
        g_vertex_cache_bytes - found->payload->raw_bytes.size();
    if (payload->raw_bytes.size() >
        kMaximumVertexCacheBytes -
            std::min(retained_bytes, kMaximumVertexCacheBytes)) {
      return nullptr;
    }
    g_vertex_cache_bytes = retained_bytes + payload->raw_bytes.size();
    found->payload = payload;
    return payload;
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

IndexCaptureResult
CaptureIndices(uint8_t *guest_base, const SceneCatalogDrawOccurrence &draw,
               uint32_t vertex_count) {
  IndexCaptureResult result;
  result.vertex_count = vertex_count;
  const uint64_t requested_size =
      static_cast<uint64_t>(draw.submitted_index_count) * sizeof(uint16_t);
  const uint32_t physical_address =
      PhysicalAddressForVirtualAlias(draw.mesh.index_buffer_alias);
  result.physical_address = physical_address;
  if (physical_address == 0 || draw.submitted_index_count == 0 ||
      requested_size == 0 || requested_size > kMaximumIndexPayloadBytes ||
      requested_size > draw.mesh.index_buffer_bytes) {
    result.failure = IndexCaptureFailure::kMetadata;
    return result;
  }
  const IndexCacheKey key = {
      .physical_address = physical_address,
      .source_virtual_alias = draw.mesh.index_buffer_alias,
      .submitted_index_count = draw.submitted_index_count,
  };

  auto payload = std::make_shared<VenueE33IndexPayload>();
  payload->source_virtual_alias = draw.mesh.index_buffer_alias;
  payload->physical_address = physical_address;
  payload->submitted_index_count = draw.submitted_index_count;
  if (!CaptureStablePhysicalBytes(guest_base, physical_address,
                                  static_cast<size_t>(requested_size),
                                  payload->raw_bytes)) {
    result.failure = IndexCaptureFailure::kGuestCopy;
    return result;
  }
  payload->payload_fingerprint = Fingerprint(payload->raw_bytes);
  payload->indices.resize(draw.submitted_index_count);
  for (uint32_t index = 0; index < draw.submitted_index_count; ++index) {
    const uint8_t *source =
        payload->raw_bytes.data() + static_cast<size_t>(index) * 2;
    const uint16_t decoded = LoadBeU16(source);
    uint16_t little_endian = 0;
    std::memcpy(&little_endian, source, sizeof(little_endian));
    result.minimum_index = std::min(result.minimum_index, decoded);
    result.maximum_index = std::max(result.maximum_index, decoded);
    result.little_endian_maximum =
        std::max(result.little_endian_maximum, little_endian);
    if (decoded >= vertex_count &&
        result.first_out_of_range_position == UINT32_MAX) {
      result.first_out_of_range_position = index;
      result.first_out_of_range_value = decoded;
    }
    payload->indices[index] = decoded;
  }
  if (result.first_out_of_range_position != UINT32_MAX) {
    result.failure = IndexCaptureFailure::kVertexRange;
    return result;
  }
  if (!payload->valid()) {
    result.failure = IndexCaptureFailure::kPayloadInvariant;
    return result;
  }

  std::lock_guard lock(g_cache_mutex);
  if (const auto cached = FindCached(g_index_cache, key)) {
    if (cached->payload_fingerprint == payload->payload_fingerprint &&
        cached->raw_bytes == payload->raw_bytes) {
      result.payload = cached;
      return result;
    }
    const auto found = std::ranges::find_if(
        g_index_cache, [&](const auto &entry) { return entry.key == key; });
    const size_t retained_bytes =
        g_index_cache_bytes - found->payload->raw_bytes.size();
    if (payload->raw_bytes.size() >
        kMaximumIndexCacheBytes -
            std::min(retained_bytes, kMaximumIndexCacheBytes)) {
      result.failure = IndexCaptureFailure::kCacheCapacity;
      return result;
    }
    g_index_cache_bytes = retained_bytes + payload->raw_bytes.size();
    found->payload = payload;
    result.payload = payload;
    return result;
  }
  if (g_index_cache.size() >= kMaximumCachedIndices ||
      payload->raw_bytes.size() >
          kMaximumIndexCacheBytes -
              std::min(g_index_cache_bytes, kMaximumIndexCacheBytes)) {
    result.failure = IndexCaptureFailure::kCacheCapacity;
    return result;
  }
  g_index_cache_bytes += payload->raw_bytes.size();
  g_index_cache.push_back({key, payload});
  result.payload = std::move(payload);
  return result;
}

bool CompatibleTextureFetchLayout(
    const std::array<std::array<uint32_t, 6>,
                     VenueE33MaterialSnapshot::kTextureCount> &fetch_words) {
  std::array<xenos::xe_gpu_texture_fetch_t,
             VenueE33MaterialSnapshot::kTextureCount>
      fetches{};
  for (size_t slot = 0; slot < fetches.size(); ++slot) {
    fetches[slot].dword_0 = fetch_words[slot][0];
    fetches[slot].dword_1 = fetch_words[slot][1];
    fetches[slot].dword_2 = fetch_words[slot][2];
    fetches[slot].dword_3 = fetch_words[slot][3];
    fetches[slot].dword_4 = fetch_words[slot][4];
    fetches[slot].dword_5 = fetch_words[slot][5];
    if (fetches[slot].type != xenos::FetchConstantType::kTexture ||
        fetches[slot].base_address == 0) {
      return false;
    }
  }

  // Texture format, dimension, tiling, swizzle and mip range are material
  // payload, not E33 shader identity. CaptureTextureSnapshot below is the
  // authoritative decoder/support gate for each exact fetch. Requiring the
  // first trace draw's DXT1/DXT5 layout here rejected later valid materials
  // using the same independently proven program.
  return true;
}

xenos::xe_gpu_texture_fetch_t
DecodeTextureFetch(const std::array<uint32_t, 6> &words) {
  xenos::xe_gpu_texture_fetch_t fetch{};
  fetch.dword_0 = words[0];
  fetch.dword_1 = words[1];
  fetch.dword_2 = words[2];
  fetch.dword_3 = words[3];
  fetch.dword_4 = words[4];
  fetch.dword_5 = words[5];
  return fetch;
}

bool RendererTextureShapeSupported(uint32_t slot,
                                   const xenos::xe_gpu_texture_fetch_t &fetch,
                                   const TextureSnapshot &texture) {
  const bool cube = slot == 2;
  const xenos::DataDimension expected_dimension =
      cube ? xenos::DataDimension::kCube
           : xenos::DataDimension::k2DOrStacked;
  const xenos::TextureFormat base_format =
      rex::graphics::GetBaseFormat(fetch.format);
  return slot < VenueE33MaterialSnapshot::kTextureCount &&
         fetch.type == xenos::FetchConstantType::kTexture &&
         fetch.dimension == expected_dimension && !fetch.stacked &&
         texture.fetch_words ==
             std::array<uint32_t, 6>{fetch.dword_0, fetch.dword_1,
                                     fetch.dword_2, fetch.dword_3,
                                     fetch.dword_4, fetch.dword_5} &&
         texture.dimension == static_cast<uint32_t>(expected_dimension) &&
         texture.layer_count == (cube ? 6u : 1u) &&
         (base_format == xenos::TextureFormat::k_DXT1 ||
          base_format == xenos::TextureFormat::k_DXT4_5);
}

VenueE33VertexDeclarationIdentity
DeclarationIdentityForProbe(const VertexDeclarationProbe &probe) {
  VenueE33VertexDeclarationIdentity identity;
  if (!probe.valid ||
      probe.element_count != VenueE33VertexDeclarationIdentity::kElementCount) {
    return identity;
  }
  identity.element_count = probe.element_count;
  identity.max_stream = probe.max_stream;
  identity.stream_mask_lo = probe.stream_mask_lo;
  identity.stream_mask_hi = probe.stream_mask_hi;
  for (size_t index = 0; index < identity.elements.size(); ++index) {
    const VertexDeclarationElement &source = probe.elements[index];
    identity.elements[index] = {
        .stream = source.stream,
        .byte_offset = source.byte_offset,
        .packed_type = source.packed_type,
        .method = source.method,
        .usage = source.usage,
        .usage_index = source.usage_index,
    };
  }
  return identity;
}

bool ExactVertexDeclaration(const VertexDeclarationProbe &probe,
                            const VenueE33VertexDeclarationIdentity &expected) {
  return expected.valid() && DeclarationIdentityForProbe(probe) == expected;
}

bool CaptureMaterial(
    uint8_t *guest_base, const SceneCatalogDrawOccurrence &draw,
    const VenueE33VertexDeclarationIdentity &expected_vertex_declaration,
    VenueE33MaterialSnapshot &material, uint32_t &guest_read_failures,
    uint32_t &texture_capture_failures,
    uint32_t &renderer_full_mip_texture_count,
    uint32_t &renderer_texture_shape_match_count) {
  constexpr size_t kCapturedBankBytes =
      kCapturedBankEndOffset - kFetchBankOffset;
  std::vector<std::byte> bank_bytes;
  if (!CaptureStableGuestBytes(guest_base, draw.device, kFetchBankOffset,
                               bank_bytes, kCapturedBankBytes)) {
    ++guest_read_failures;
    return false;
  }

  auto load_bank_word = [&](uint32_t device_offset) {
    const size_t local_offset = device_offset - kFetchBankOffset;
    return LoadBeU32(bank_bytes.data() + local_offset);
  };
  for (size_t slot = 0; slot < material.texture_fetches.size(); ++slot) {
    for (size_t word = 0; word < material.texture_fetches[slot].size();
         ++word) {
      material.texture_fetches[slot][word] = load_bank_word(
          kFetchBankOffset + static_cast<uint32_t>((slot * 6 + word) * 4));
    }
  }
  for (size_t word = 0; word < material.vertex_constant_words.size(); ++word) {
    material.vertex_constant_words[word] = load_bank_word(
        kVertexConstantBankOffset + static_cast<uint32_t>(word * 4));
    material.pixel_constant_words[word] = load_bank_word(
        kPixelConstantBankOffset + static_cast<uint32_t>(word * 4));
  }

  const bool fetch_layout_valid =
      CompatibleTextureFetchLayout(material.texture_fetches);
  for (uint32_t slot = 0; slot < material.textures.size(); ++slot) {
    const xenos::xe_gpu_texture_fetch_t fetch =
        DecodeTextureFetch(material.texture_fetches[slot]);
    material.textures[slot] = CaptureTextureSnapshot(
        guest_base, draw.pass.pixel_shader, slot, material.texture_fetches[slot],
        TextureMipCapture::kFullFetchRange);
    texture_capture_failures += material.textures[slot] == nullptr;
    const bool full_mip_chain = material.textures[slot] != nullptr &&
                                material.textures[slot]->full_mip_chain();
    renderer_full_mip_texture_count += full_mip_chain;
    const bool shape_supported =
        full_mip_chain &&
        RendererTextureShapeSupported(slot, fetch, *material.textures[slot]);
    renderer_texture_shape_match_count += shape_supported;
    if (g_renderer_material_diagnostic_logs.fetch_add(
            1, std::memory_order_relaxed) <
        kMaximumRendererMaterialDiagnosticLogs) {
      const TextureSnapshot *const texture = material.textures[slot].get();
      REXLOG_INFO(
          "Table Tennis E33 candidate material diagnostic: ordinal={} slot={} "
          "dimension={} layers={} format={} base_format={} shape={}x{} tiled={} "
          "descriptor_mips={}-{} captured_mips={} full_mips={} "
          "clamp={}/{}/{} filter={}/{}/{} aniso={}/{}/{} lod_bias={} "
          "swizzle={:03X} border={}/{} tri_clamp={} renderer_shape={} "
          "observer_only=true",
          draw.ordinal, slot, static_cast<uint32_t>(fetch.dimension),
          texture != nullptr ? texture->layer_count : 0,
          static_cast<uint32_t>(fetch.format),
          static_cast<uint32_t>(rex::graphics::GetBaseFormat(fetch.format)),
          texture != nullptr ? texture->width : 0,
          texture != nullptr ? texture->height : 0,
          static_cast<uint32_t>(fetch.tiled),
          texture != nullptr ? texture->descriptor_mip_min_level
                             : static_cast<uint32_t>(fetch.mip_min_level),
          texture != nullptr ? texture->descriptor_mip_max_level
                             : static_cast<uint32_t>(fetch.mip_max_level),
          texture != nullptr ? texture->mips.size() : 0, full_mip_chain,
          static_cast<uint32_t>(fetch.clamp_x),
          static_cast<uint32_t>(fetch.clamp_y),
          static_cast<uint32_t>(fetch.clamp_z),
          static_cast<uint32_t>(fetch.mag_filter),
          static_cast<uint32_t>(fetch.min_filter),
          static_cast<uint32_t>(fetch.mip_filter),
          static_cast<uint32_t>(fetch.aniso_filter),
          static_cast<uint32_t>(fetch.mag_aniso_walk),
          static_cast<uint32_t>(fetch.min_aniso_walk),
          static_cast<int32_t>(fetch.lod_bias),
          static_cast<uint32_t>(fetch.swizzle),
          static_cast<uint32_t>(fetch.border_color),
          static_cast<uint32_t>(fetch.border_size),
          static_cast<uint32_t>(fetch.tri_clamp), shape_supported);
    }
  }
  const bool texture_payloads_valid =
      std::ranges::all_of(material.textures, [](const auto &texture) {
        return texture != nullptr && texture->valid() &&
               texture->full_mip_chain();
      });
  const uint32_t declaration = draw.state.vertex_declaration != 0
                                   ? draw.state.vertex_declaration
                                   : draw.mesh.vertex_declaration;
  material.vertex_declaration = declaration;
  material.vertex_declaration_probe =
      ProbeVertexDeclaration(guest_base, declaration);
  guest_read_failures += material.vertex_declaration_probe.copy_failures;
  material.valid = guest_read_failures == 0 && fetch_layout_valid &&
                   texture_payloads_valid && declaration != 0 &&
                   ExactVertexDeclaration(material.vertex_declaration_probe,
                                          expected_vertex_declaration);
  return material.valid;
}

} // namespace

bool VenueE33VertexDeclarationIdentity::valid() const {
  static constexpr std::array<uint16_t, kElementCount> kOffsets = {
      0, 12, 16, 20, 28};
  static constexpr std::array<uint32_t, kElementCount> kPackedTypes = {
      0x002A23B9, 0x001A2387, 0x00182886, 0x002C23A5, 0x001A2387};
  static constexpr std::array<uint8_t, kElementCount> kUsages = {
      0, 3, 10, 5, 6};
  if (element_count != kElementCount || max_stream != 0 ||
      stream_mask_lo != 0xFF00000000000000ull || stream_mask_hi != 0) {
    return false;
  }
  for (size_t index = 0; index < elements.size(); ++index) {
    const VenueE33VertexElementIdentity &element = elements[index];
    if (element.stream != 0 || element.byte_offset != kOffsets[index] ||
        element.packed_type != kPackedTypes[index] || element.method != 0 ||
        element.usage != kUsages[index] || element.usage_index != 0) {
      return false;
    }
  }
  return true;
}

VenueE33TitleCandidate
ClassifyVenueE33TitleCandidate(uint8_t *guest_base,
                               const SceneCatalogDrawOccurrence &draw) {
  VenueE33TitleCandidate candidate;
  if (guest_base == nullptr || !IsStructuralCandidate(draw)) {
    return candidate;
  }
  const uint32_t index_base =
      PhysicalAddressForVirtualAlias(draw.mesh.index_buffer_alias);
  if (index_base == 0) {
    return candidate;
  }
  candidate.identity = {
      .primitive_type = draw.primitive_type,
      .submitted_index_count = draw.submitted_index_count,
      .guest_index_base = index_base,
  };
  candidate.program = {
      .pass_descriptor = draw.pass.pass_descriptor,
      .program_pair = draw.pass.program_pair,
      .vertex_shader = draw.pass.vertex_shader,
      .pixel_shader = draw.pass.pixel_shader,
  };
  const uint32_t declaration = draw.state.vertex_declaration != 0
                                   ? draw.state.vertex_declaration
                                   : draw.mesh.vertex_declaration;
  candidate.vertex_declaration = DeclarationIdentityForProbe(
      ProbeVertexDeclaration(guest_base, declaration));
  candidate.ordinal = draw.ordinal;
  candidate.owner_kind = static_cast<uint32_t>(draw.owner.kind);
  candidate.owner = draw.owner.owner;
  candidate.owner_renderable = draw.owner.renderable;
  candidate.eligible = candidate.identity.valid() &&
                       candidate.program.valid() &&
                       candidate.vertex_declaration.valid();
  return candidate;
}

VenueE33TitleCapture
CaptureVenueE33TitleDraw(uint8_t *guest_base,
                         const SceneCatalogDrawOccurrence &draw,
                         const VenueE33TitleCandidate &candidate) {
  VenueE33TitleCapture capture;
  const VenueE33TitleCandidate current =
      ClassifyVenueE33TitleCandidate(guest_base, draw);
  if (guest_base == nullptr || !candidate.eligible || !current.eligible ||
      !(candidate.identity == current.identity) ||
      !(candidate.program == current.program) ||
      !(candidate.vertex_declaration == current.vertex_declaration)) {
    return capture;
  }

  auto snapshot = std::make_shared<VenueE33TitleDrawSnapshot>();
  snapshot->identity = candidate.identity;
  snapshot->program = candidate.program;
  snapshot->vertex_declaration = candidate.vertex_declaration;
  snapshot->ordinal = candidate.ordinal;
  snapshot->owner_kind = candidate.owner_kind;
  snapshot->owner = candidate.owner;
  snapshot->owner_renderable = candidate.owner_renderable;

  snapshot->vertices = CaptureVertices(guest_base, draw);
  capture.payload_copy_failures += snapshot->vertices == nullptr;
  IndexCaptureResult index_capture;
  if (snapshot->vertices != nullptr) {
    index_capture =
        CaptureIndices(guest_base, draw, snapshot->vertices->vertex_count);
    snapshot->indices = std::move(index_capture.payload);
  }
  capture.payload_copy_failures += snapshot->indices == nullptr;
  if (snapshot->indices == nullptr &&
      g_payload_diagnostic_logs.fetch_add(1, std::memory_order_relaxed) <
          kMaximumPayloadDiagnosticLogs) {
    REXLOG_INFO(
        "Table Tennis E33 payload diagnostic: ordinal={} indices={} "
        "failure={} vb[stream={} alt={} global={} resource={:08X} "
        "alias={:08X} physical={:08X} bytes={} stride={} vertices={}] "
        "ib[wrapper={:08X} resource={:08X} alias={:08X} physical={:08X} "
        "bytes={}] decoded[min={} max={} little_max={} first_oob={}:{}] "
        "observer_only=true guest_suppressed=false",
        draw.ordinal, draw.submitted_index_count,
        snapshot->vertices == nullptr
            ? "vertex-capture"
            : IndexCaptureFailureName(index_capture.failure),
        draw.mesh.stream_selector, draw.mesh.alternate_primary_stream,
        draw.mesh.global_stream_selector, draw.mesh.vertex_buffer_resource,
        draw.mesh.vertex_buffer_alias,
        snapshot->vertices != nullptr
            ? snapshot->vertices->physical_address
            : PhysicalAddressForVirtualAlias(draw.mesh.vertex_buffer_alias),
        draw.mesh.vertex_buffer_bytes, draw.mesh.vertex_stride,
        snapshot->vertices != nullptr ? snapshot->vertices->vertex_count : 0,
        draw.mesh.index_buffer_wrapper, draw.mesh.index_buffer_resource,
        draw.mesh.index_buffer_alias, index_capture.physical_address,
        draw.mesh.index_buffer_bytes,
        index_capture.minimum_index == UINT16_MAX
            ? 0
            : index_capture.minimum_index,
        index_capture.maximum_index, index_capture.little_endian_maximum,
        index_capture.first_out_of_range_position,
        index_capture.first_out_of_range_value);
  }
  const bool material_valid =
      CaptureMaterial(guest_base, draw, candidate.vertex_declaration,
                      snapshot->material, capture.guest_read_failures,
                      capture.texture_capture_failures,
                      capture.renderer_full_mip_texture_count,
                      capture.renderer_texture_shape_match_count);
  capture.material_validation_failures += !material_valid;

  snapshot->valid =
      capture.guest_read_failures == 0 && capture.payload_copy_failures == 0 &&
      capture.texture_capture_failures == 0 &&
      capture.material_validation_failures == 0 &&
      snapshot->identity.valid() &&
      snapshot->program.valid() && snapshot->vertex_declaration.valid() &&
      snapshot->vertices != nullptr && snapshot->vertices->valid() &&
      snapshot->indices != nullptr && snapshot->indices->valid() &&
      snapshot->material.valid;
  if (snapshot->valid) {
    capture.snapshot = std::move(snapshot);
  }
  return capture;
}

} // namespace tabletennis::native
