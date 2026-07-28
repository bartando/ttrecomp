#include "native/tabletennis_player_a406_snapshot.h"

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
#include <memory>
#include <vector>

namespace tabletennis::native {
namespace {

constexpr uint64_t kPixelShaderHash = 0x0F9CCE179F32DA36ull;
constexpr uint32_t kTriangleListPrimitive = 4;
constexpr uint32_t kFetchBankOffset = 0x480;
constexpr uint32_t kVertexConstantBankOffset = 0x780;
constexpr uint32_t kPixelConstantBankOffset = 0x1780;
constexpr size_t kConstantRowBytes = sizeof(float) * 4;
constexpr uint32_t kPhysicalAliasBase = 0xA0000000;
constexpr uint32_t kPhysicalAddressMask = 0x1FFFFFFF;
constexpr uint32_t kHighPhysicalHeapBase = 0xE0000000;
constexpr uint32_t kHighPhysicalHeapHostPageOffset = 0x1000;
constexpr size_t kMaximumVertexPayloadBytes = 64 * 1024 * 1024;
constexpr size_t kMaximumIndexPayloadBytes = 16 * 1024 * 1024;
constexpr uint64_t kFnvOffsetBasis = 1469598103934665603ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;

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

bool TryCopyPhysical(uint8_t *guest_base, uint32_t physical_address,
                     void *destination, size_t size) {
  if (guest_base == nullptr || physical_address == 0 ||
      physical_address > kPhysicalAddressMask || destination == nullptr ||
      size == 0 ||
      size >
          static_cast<uint64_t>(kPhysicalAddressMask) + 1 - physical_address) {
    return false;
  }
  const uint32_t alias = kPhysicalAliasBase | physical_address;
  return GuestTryCopy(destination,
                      guest_base + alias + REX_PHYS_HOST_OFFSET(alias), size);
}

bool CaptureStablePhysicalBytes(uint8_t *guest_base, uint32_t physical_address,
                                size_t size, std::vector<uint8_t> &bytes) {
  bytes.resize(size);
  std::vector<uint8_t> verification(size);
  for (uint32_t attempt = 0; attempt < 4; ++attempt) {
    if (!TryCopyPhysical(guest_base, physical_address, bytes.data(), size) ||
        !TryCopyPhysical(guest_base, physical_address, verification.data(),
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

bool TryReadBeU32(uint8_t *guest_base, uint32_t address, uint32_t &value) {
  if (guest_base == nullptr || address == 0) {
    return false;
  }
  std::array<std::byte, sizeof(uint32_t)> bytes{};
  if (!GuestTryCopy(bytes.data(),
                    guest_base + address + REX_PHYS_HOST_OFFSET(address),
                    bytes.size())) {
    return false;
  }
  std::memcpy(&value, bytes.data(), sizeof(value));
  value = std::byteswap(value);
  return true;
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

uint32_t LoadBeU32(const std::byte *source) {
  uint32_t value = 0;
  std::memcpy(&value, source, sizeof(value));
  return std::byteswap(value);
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

template <size_t Size>
bool CaptureBeFloats(uint8_t *guest_base, uint32_t address, size_t offset,
                     std::array<float, Size> &values) {
  std::array<uint32_t, Size> words{};
  if (!CaptureBeWords(guest_base, address, offset, words)) {
    return false;
  }
  for (size_t index = 0; index < Size; ++index) {
    values[index] = std::bit_cast<float>(words[index]);
  }
  return true;
}

uint16_t LoadBeU16(const uint8_t *source) {
  uint16_t value = 0;
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

template <size_t Size> bool AllFinite(const std::array<float, Size> &values) {
  return std::ranges::all_of(values,
                             [](float value) { return std::isfinite(value); });
}

uint64_t AuthoritativeVertexHash(const SceneCatalogDrawOccurrence &draw) {
  if (draw.bound_shaders.vertex_shader_valid) {
    return draw.bound_shaders.vertex_shader_hash;
  }
  return draw.pass.shader_fingerprints_valid ? draw.pass.vertex_shader_hash : 0;
}

uint64_t AuthoritativePixelHash(const SceneCatalogDrawOccurrence &draw) {
  if (draw.bound_shaders.pixel_shader_valid) {
    return draw.bound_shaders.pixel_shader_hash;
  }
  return draw.pass.shader_fingerprints_valid ? draw.pass.pixel_shader_hash : 0;
}

PlayerA406VertexDeclarationIdentity
DeclarationIdentityForProbe(const VertexDeclarationProbe &probe) {
  PlayerA406VertexDeclarationIdentity identity;
  if (!probe.valid || probe.element_count !=
                          PlayerA406VertexDeclarationIdentity::kElementCount) {
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

bool IsStructuralCandidate(const SceneCatalogDrawOccurrence &draw) {
  const uint64_t index_bytes =
      static_cast<uint64_t>(draw.submitted_index_count) * sizeof(uint16_t);
  return draw.frame_sequence != 0 && draw.player != 0 && draw.scope_valid &&
         draw.scope.shader != 0 && draw.scope.model != 0 && draw.pass.valid &&
         draw.mesh.valid && draw.mesh.vertex_aggregate != 0 &&
         draw.state.valid && AuthoritativePixelHash(draw) == kPixelShaderHash &&
         draw.primitive_type == kTriangleListPrimitive &&
         draw.mesh.aggregate_primitive_type == kTriangleListPrimitive &&
         draw.submitted_index_count != 0 &&
         draw.mesh.vertex_stride == PlayerA406VertexPayload::kStride &&
         draw.mesh.vertex_endian == PlayerA406VertexPayload::kEndian8In32 &&
         draw.mesh.vertex_buffer_alias != 0 &&
         draw.mesh.vertex_buffer_bytes >= PlayerA406VertexPayload::kStride &&
         draw.mesh.vertex_buffer_bytes % PlayerA406VertexPayload::kStride ==
             0 &&
         draw.mesh.index_buffer_alias != 0 &&
         draw.mesh.index_element_size == sizeof(uint16_t) &&
         !draw.mesh.index_is_32_bit && index_bytes != 0 &&
         index_bytes <= draw.mesh.index_buffer_bytes &&
         PhysicalAddressForVirtualAlias(draw.mesh.vertex_buffer_alias) != 0 &&
         PhysicalAddressForVirtualAlias(draw.mesh.index_buffer_alias) != 0 &&
         draw.world_valid && draw.world_view_projection_valid;
}

std::shared_ptr<const PlayerA406VertexPayload>
CaptureVertices(uint8_t *guest_base, const SceneCatalogDrawOccurrence &draw) {
  const uint32_t physical =
      PhysicalAddressForVirtualAlias(draw.mesh.vertex_buffer_alias);
  const uint32_t byte_count = draw.mesh.vertex_buffer_bytes;
  if (physical == 0 || byte_count == 0 ||
      byte_count > kMaximumVertexPayloadBytes ||
      byte_count % PlayerA406VertexPayload::kStride != 0) {
    return nullptr;
  }
  auto payload = std::make_shared<PlayerA406VertexPayload>();
  payload->source_virtual_alias = draw.mesh.vertex_buffer_alias;
  payload->physical_address = physical;
  payload->byte_count = byte_count;
  payload->vertex_count = byte_count / PlayerA406VertexPayload::kStride;
  if (!CaptureStablePhysicalBytes(guest_base, physical, byte_count,
                                  payload->raw_bytes)) {
    return nullptr;
  }
  payload->payload_fingerprint = Fingerprint(payload->raw_bytes);
  return payload->valid() ? payload : nullptr;
}

std::shared_ptr<const PlayerA406IndexPayload>
CaptureIndices(uint8_t *guest_base, const SceneCatalogDrawOccurrence &draw,
               uint32_t vertex_count) {
  const uint64_t requested_bytes =
      static_cast<uint64_t>(draw.submitted_index_count) * sizeof(uint16_t);
  const uint32_t physical =
      PhysicalAddressForVirtualAlias(draw.mesh.index_buffer_alias);
  if (physical == 0 || vertex_count == 0 || requested_bytes == 0 ||
      requested_bytes > kMaximumIndexPayloadBytes ||
      requested_bytes > draw.mesh.index_buffer_bytes) {
    return nullptr;
  }
  auto payload = std::make_shared<PlayerA406IndexPayload>();
  payload->source_virtual_alias = draw.mesh.index_buffer_alias;
  payload->physical_address = physical;
  payload->submitted_index_count = draw.submitted_index_count;
  if (!CaptureStablePhysicalBytes(guest_base, physical,
                                  static_cast<size_t>(requested_bytes),
                                  payload->raw_bytes)) {
    return nullptr;
  }
  payload->payload_fingerprint = Fingerprint(payload->raw_bytes);
  payload->minimum_index = std::numeric_limits<uint16_t>::max();
  payload->indices.resize(draw.submitted_index_count);
  for (uint32_t index = 0; index < draw.submitted_index_count; ++index) {
    const uint16_t decoded =
        LoadBeU16(payload->raw_bytes.data() + static_cast<size_t>(index) * 2);
    if (decoded >= vertex_count) {
      return nullptr;
    }
    payload->minimum_index = std::min(payload->minimum_index, decoded);
    payload->maximum_index = std::max(payload->maximum_index, decoded);
    payload->indices[index] = decoded;
  }
  return payload->valid() ? payload : nullptr;
}

bool CaptureMaterial(uint8_t *guest_base,
                     const SceneCatalogDrawOccurrence &draw,
                     const PlayerA406VertexDeclarationIdentity &expected,
                     PlayerA406MaterialSnapshot &material,
                     uint32_t &guest_read_failures,
                     uint32_t &texture_capture_failures) {
  material.vertex_declaration = draw.state.vertex_declaration != 0
                                    ? draw.state.vertex_declaration
                                    : draw.mesh.vertex_declaration;
  material.vertex_declaration_probe =
      ProbeVertexDeclaration(guest_base, material.vertex_declaration);
  guest_read_failures += material.vertex_declaration_probe.copy_failures;

  std::array<uint32_t, PlayerA406MaterialSnapshot::kTextureFetchCount * 6>
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

  auto capture_constants = [&](size_t bank_offset, size_t first_row,
                               auto &destination) {
    if (!CaptureBeFloats(guest_base, draw.device,
                         bank_offset + first_row * kConstantRowBytes,
                         destination)) {
      ++guest_read_failures;
    }
  };
  capture_constants(kVertexConstantBankOffset, 0,
                    material.vertex_constants_0_3);
  capture_constants(kVertexConstantBankOffset, 12,
                    material.vertex_constants_12_15);
  capture_constants(kVertexConstantBankOffset, 19, material.vertex_constant_19);
  capture_constants(kVertexConstantBankOffset, 29,
                    material.vertex_constants_29_36);
  capture_constants(kVertexConstantBankOffset, 46,
                    material.vertex_constants_46_47);
  capture_constants(kPixelConstantBankOffset, 19, material.pixel_constant_19);
  capture_constants(kPixelConstantBankOffset, 21,
                    material.pixel_constants_21_27);
  capture_constants(kPixelConstantBankOffset, 46,
                    material.pixel_constants_46_73);
  capture_constants(kPixelConstantBankOffset, 254,
                    material.pixel_constants_254_255);

  const bool fetches_valid =
      std::ranges::all_of(material.texture_fetches, [](const auto &fetch) {
        return (fetch[0] & 0x3u) == 2 && fetch[1] != 0;
      });
  static constexpr std::array<uint32_t,
                              PlayerA406MaterialSnapshot::kOwnedTextureCount>
      kOwnedSlots = {0, 1, 2, 6};
  const uint32_t texture_owner = draw.bound_shaders.pixel_shader_valid
                                     ? draw.bound_shaders.pixel_shader
                                     : draw.pass.pixel_shader;
  for (size_t index = 0; index < kOwnedSlots.size(); ++index) {
    const uint32_t slot = kOwnedSlots[index];
    material.owned_textures[index] = CaptureTextureSnapshot(
        guest_base, texture_owner, slot, material.texture_fetches[slot],
        TextureMipCapture::kFullFetchRange);
    texture_capture_failures += material.owned_textures[index] == nullptr;
  }
  const bool owned_textures_valid =
      std::ranges::all_of(material.owned_textures, [](const auto &texture) {
        return texture != nullptr && texture->valid() &&
               texture->full_mip_chain();
      });
  material.valid =
      draw.state.valid && material.vertex_declaration != 0 &&
      DeclarationIdentityForProbe(material.vertex_declaration_probe) ==
          expected &&
      guest_read_failures == 0 && fetches_valid && owned_textures_valid &&
      AllFinite(material.vertex_constants_0_3) &&
      AllFinite(material.vertex_constants_12_15) &&
      AllFinite(material.vertex_constant_19) &&
      AllFinite(material.vertex_constants_29_36) &&
      AllFinite(material.vertex_constants_46_47) &&
      AllFinite(material.pixel_constant_19) &&
      AllFinite(material.pixel_constants_21_27) &&
      AllFinite(material.pixel_constants_46_73) &&
      AllFinite(material.pixel_constants_254_255);
  return material.valid;
}

} // namespace

bool PlayerA406TitleProgramIdentity::valid() const {
  return vertex_shader != 0 && pixel_shader != 0 && vertex_shader_hash != 0 &&
         pixel_shader_hash == kPixelShaderHash;
}

bool PlayerA406VertexDeclarationIdentity::valid() const {
  static constexpr std::array<uint16_t, kElementCount> kStreams = {
      0, 0, 0, 0, 0, 0, 3, 3};
  static constexpr std::array<uint16_t, kElementCount> kOffsets = {
      0, 16, 32, 48, 64, 80, 0, 16};
  static constexpr std::array<uint32_t, kElementCount> kPackedTypes = {
      0x001A23A6, 0x001A23A6, 0x001A23A6, 0x001A23A6,
      0x001A23A6, 0x00182886, 0x001A23A6, 0x002A23B9};
  static constexpr std::array<uint8_t, kElementCount> kUsages = {
      0, 3, 6, 5, 1, 2, 0, 0};
  static constexpr std::array<uint8_t, kElementCount> kUsageIndices = {
      0, 0, 0, 0, 0, 0, 2, 3};
  if (element_count != kElementCount || max_stream != 3 ||
      stream_mask_lo != 0xFF0000FF00000000ull || stream_mask_hi != 0) {
    return false;
  }
  for (size_t index = 0; index < elements.size(); ++index) {
    const PlayerA406VertexElementIdentity &element = elements[index];
    if (element.stream != kStreams[index] ||
        element.byte_offset != kOffsets[index] ||
        element.packed_type != kPackedTypes[index] || element.method != 0 ||
        element.usage != kUsages[index] ||
        element.usage_index != kUsageIndices[index]) {
      return false;
    }
  }
  return true;
}

PlayerA406TitleCandidate
ClassifyPlayerA406TitleCandidate(uint8_t *guest_base,
                                 const SceneCatalogDrawOccurrence &draw) {
  PlayerA406TitleCandidate candidate;
  if (guest_base == nullptr || !IsStructuralCandidate(draw)) {
    return candidate;
  }
  candidate.identity = {
      .primitive_type = draw.primitive_type,
      .submitted_index_count = draw.submitted_index_count,
      .guest_index_base =
          PhysicalAddressForVirtualAlias(draw.mesh.index_buffer_alias),
      .guest_vertex_base =
          PhysicalAddressForVirtualAlias(draw.mesh.vertex_buffer_alias),
      .guest_vertex_bytes = draw.mesh.vertex_buffer_bytes,
      .guest_vertex_endian = draw.mesh.vertex_endian,
  };
  candidate.program = {
      .pass_descriptor = draw.pass.pass_descriptor,
      .program_pair = draw.pass.program_pair,
      .vertex_shader = draw.bound_shaders.vertex_shader_valid
                           ? draw.bound_shaders.vertex_shader
                           : draw.pass.vertex_shader,
      .pixel_shader = draw.bound_shaders.pixel_shader_valid
                          ? draw.bound_shaders.pixel_shader
                          : draw.pass.pixel_shader,
      .vertex_shader_hash = AuthoritativeVertexHash(draw),
      .pixel_shader_hash = AuthoritativePixelHash(draw),
      .vertex_hash_from_bound_shader = draw.bound_shaders.vertex_shader_valid,
      .pixel_hash_from_bound_shader = draw.bound_shaders.pixel_shader_valid,
  };
  const uint32_t declaration = draw.state.vertex_declaration != 0
                                   ? draw.state.vertex_declaration
                                   : draw.mesh.vertex_declaration;
  candidate.vertex_declaration = DeclarationIdentityForProbe(
      ProbeVertexDeclaration(guest_base, declaration));
  candidate.sequence = draw.frame_sequence;
  candidate.ordinal = draw.ordinal;
  candidate.player = draw.player;
  candidate.material_shader = draw.scope.shader;
  candidate.model = draw.scope.model;
  candidate.vertex_aggregate = draw.mesh.vertex_aggregate;
  const bool material_vtable_valid =
      TryReadBeU32(guest_base, candidate.material_shader,
                   candidate.material_shader_vtable) &&
      candidate.material_shader_vtable != 0;
  candidate.eligible =
      candidate.identity.valid() && candidate.program.valid() &&
      candidate.vertex_declaration.valid() && candidate.player != 0 &&
      candidate.material_shader != 0 && material_vtable_valid &&
      candidate.model != 0 && candidate.vertex_aggregate != 0;
  return candidate;
}

PlayerA406TitleCapture
CapturePlayerA406TitleDraw(uint8_t *guest_base,
                           const SceneCatalogDrawOccurrence &draw,
                           const PlayerA406TitleCandidate &candidate) {
  PlayerA406TitleCapture capture;
  const PlayerA406TitleCandidate current =
      ClassifyPlayerA406TitleCandidate(guest_base, draw);
  if (!candidate.eligible || !current.eligible ||
      !(candidate.identity == current.identity) ||
      !(candidate.program == current.program) ||
      !(candidate.vertex_declaration == current.vertex_declaration) ||
      candidate.sequence != current.sequence ||
      candidate.ordinal != current.ordinal ||
      candidate.player != current.player ||
      candidate.material_shader != current.material_shader ||
      candidate.material_shader_vtable != current.material_shader_vtable ||
      candidate.model != current.model ||
      candidate.vertex_aggregate != current.vertex_aggregate) {
    return capture;
  }

  auto snapshot = std::make_shared<PlayerA406TitleDrawSnapshot>();
  snapshot->identity = candidate.identity;
  snapshot->program = candidate.program;
  snapshot->vertex_declaration = candidate.vertex_declaration;
  snapshot->sequence = candidate.sequence;
  snapshot->ordinal = candidate.ordinal;
  snapshot->player = candidate.player;
  snapshot->owner = draw.owner;
  snapshot->material_shader = candidate.material_shader;
  snapshot->material_shader_vtable = candidate.material_shader_vtable;
  snapshot->model = candidate.model;
  snapshot->geometry_index = draw.scope.geometry_index;
  snapshot->lod = draw.scope.lod;
  snapshot->alternate_pass = draw.scope.alternate_pass;
  snapshot->vertex_aggregate = candidate.vertex_aggregate;
  snapshot->world = draw.world;
  snapshot->world_view_projection = draw.world_view_projection;

  snapshot->vertices = CaptureVertices(guest_base, draw);
  capture.payload_copy_failures += snapshot->vertices == nullptr;
  if (snapshot->vertices != nullptr) {
    snapshot->indices =
        CaptureIndices(guest_base, draw, snapshot->vertices->vertex_count);
  }
  capture.payload_copy_failures += snapshot->indices == nullptr;
  const bool material_valid = CaptureMaterial(
      guest_base, draw, candidate.vertex_declaration, snapshot->material,
      capture.guest_read_failures, capture.texture_capture_failures);
  capture.material_validation_failures += !material_valid;

  snapshot->valid =
      capture.guest_read_failures == 0 && capture.payload_copy_failures == 0 &&
      capture.texture_capture_failures == 0 &&
      capture.material_validation_failures == 0 && snapshot->identity.valid() &&
      snapshot->program.valid() && snapshot->vertex_declaration.valid() &&
      snapshot->player != 0 && snapshot->material_shader != 0 &&
      snapshot->material_shader_vtable != 0 && snapshot->model != 0 &&
      snapshot->vertex_aggregate != 0 && snapshot->vertices != nullptr &&
      snapshot->vertices->valid() &&
      snapshot->vertices->physical_address ==
          snapshot->identity.guest_vertex_base &&
      snapshot->vertices->byte_count == snapshot->identity.guest_vertex_bytes &&
      snapshot->indices != nullptr && snapshot->indices->valid() &&
      snapshot->indices->physical_address ==
          snapshot->identity.guest_index_base &&
      snapshot->indices->submitted_index_count ==
          snapshot->identity.submitted_index_count &&
      snapshot->material.valid && AllFinite(snapshot->world) &&
      AllFinite(snapshot->world_view_projection);
  if (snapshot->valid) {
    capture.snapshot = std::move(snapshot);
  }
  return capture;
}

} // namespace tabletennis::native
