#include "native/tabletennis_venue_9e_snapshot.h"

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

#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/xenos.h>

namespace tabletennis::native {
namespace {

namespace xenos = rex::graphics::xenos;

constexpr uint64_t kVertexShaderHash = 0x37F2AEC8A23E44E0ull;
constexpr uint64_t kPixelShaderHash = 0x9E1AF02A96682354ull;
constexpr uint32_t kTriangleStripPrimitive = 6;
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
      size == 0) {
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
  if (!GuestTryCopy(
          bytes.data(),
          guest_base + address + REX_PHYS_HOST_OFFSET(address),
          bytes.size())) {
    return false;
  }
  std::memcpy(&value, bytes.data(), sizeof(value));
  value = std::byteswap(value);
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

Venue9EVertexDeclarationIdentity
DeclarationIdentityForProbe(const VertexDeclarationProbe &probe) {
  Venue9EVertexDeclarationIdentity identity;
  if (!probe.valid ||
      probe.element_count != Venue9EVertexDeclarationIdentity::kElementCount) {
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
  return draw.frame_sequence != 0 && draw.player == 0 && draw.pass.valid &&
         draw.mesh.valid && draw.state.valid &&
         AuthoritativeVertexHash(draw) == kVertexShaderHash &&
         AuthoritativePixelHash(draw) == kPixelShaderHash &&
         draw.primitive_type == kTriangleStripPrimitive &&
         draw.mesh.aggregate_primitive_type == kTriangleStripPrimitive &&
         draw.submitted_index_count != 0 &&
         draw.mesh.vertex_stride == Venue9EVertexPayload::kStride &&
         draw.mesh.vertex_endian == Venue9EVertexPayload::kEndian8In32 &&
         draw.mesh.vertex_buffer_alias != 0 &&
         draw.mesh.vertex_buffer_bytes >= Venue9EVertexPayload::kStride &&
         draw.mesh.vertex_buffer_bytes % Venue9EVertexPayload::kStride == 0 &&
         draw.mesh.index_buffer_alias != 0 &&
         draw.mesh.index_element_size == sizeof(uint16_t) &&
         !draw.mesh.index_is_32_bit && index_bytes != 0 &&
         index_bytes <= draw.mesh.index_buffer_bytes &&
         PhysicalAddressForVirtualAlias(draw.mesh.vertex_buffer_alias) != 0 &&
         PhysicalAddressForVirtualAlias(draw.mesh.index_buffer_alias) != 0 &&
         draw.world_valid && draw.world_view_projection_valid;
}

std::shared_ptr<const Venue9EVertexPayload>
CaptureVertices(uint8_t *guest_base, const SceneCatalogDrawOccurrence &draw) {
  const uint32_t physical =
      PhysicalAddressForVirtualAlias(draw.mesh.vertex_buffer_alias);
  const uint32_t byte_count = draw.mesh.vertex_buffer_bytes;
  if (physical == 0 || byte_count == 0 ||
      byte_count > kMaximumVertexPayloadBytes ||
      byte_count % Venue9EVertexPayload::kStride != 0) {
    return nullptr;
  }
  auto payload = std::make_shared<Venue9EVertexPayload>();
  payload->source_virtual_alias = draw.mesh.vertex_buffer_alias;
  payload->physical_address = physical;
  payload->byte_count = byte_count;
  payload->vertex_count = byte_count / Venue9EVertexPayload::kStride;
  if (!CaptureStablePhysicalBytes(guest_base, physical, byte_count,
                                  payload->raw_bytes)) {
    return nullptr;
  }
  payload->payload_fingerprint = Fingerprint(payload->raw_bytes);
  return payload->valid() ? payload : nullptr;
}

std::shared_ptr<const Venue9EIndexPayload>
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
  auto payload = std::make_shared<Venue9EIndexPayload>();
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

bool ExactTextureFetchContract(const xenos::xe_gpu_texture_fetch_t &fetch) {
  return fetch.type == xenos::FetchConstantType::kTexture &&
         fetch.sign_x == xenos::TextureSign::kUnsigned &&
         fetch.sign_y == xenos::TextureSign::kUnsigned &&
         fetch.sign_z == xenos::TextureSign::kUnsigned &&
         fetch.sign_w == xenos::TextureSign::kUnsigned &&
         fetch.clamp_x == xenos::ClampMode::kRepeat &&
         fetch.clamp_y == xenos::ClampMode::kRepeat &&
         fetch.clamp_z == xenos::ClampMode::kRepeat && fetch.pitch == 4 &&
         fetch.tiled && fetch.endianness == xenos::Endian::k8in16 &&
         !fetch.stacked &&
         rex::graphics::GetBaseFormat(fetch.format) ==
             xenos::TextureFormat::k_DXT1 &&
         fetch.num_format == 0 && fetch.swizzle == 0x688 &&
         fetch.exp_adjust == 0 &&
         fetch.mag_filter == xenos::TextureFilter::kLinear &&
         fetch.min_filter == xenos::TextureFilter::kLinear &&
         fetch.mip_filter == xenos::TextureFilter::kPoint &&
         fetch.aniso_filter == xenos::AnisoFilter::kMax_2_1 &&
         fetch.arbitrary_filter == xenos::ArbitraryFilter::k2x4Sym &&
         fetch.border_size == 0 && fetch.vol_mag_filter == 1 &&
         fetch.vol_min_filter == 1 && fetch.mip_min_level == 0 &&
         fetch.mag_aniso_walk == 1 && fetch.min_aniso_walk == 1 &&
         fetch.lod_bias == 0 && fetch.grad_exp_adjust_h == 0 &&
         fetch.grad_exp_adjust_v == 0 &&
         fetch.border_color == xenos::BorderColor::k_ABGR_Black &&
         fetch.force_bc_w_to_max == 0 && fetch.tri_clamp == 3 &&
         fetch.aniso_bias == 0 &&
         fetch.dimension == xenos::DataDimension::k2DOrStacked &&
         fetch.packed_mips && fetch.base_address != 0 &&
         fetch.mip_address != 0;
}

bool CaptureMaterial(uint8_t *guest_base,
                     const SceneCatalogDrawOccurrence &draw,
                     const Venue9EVertexDeclarationIdentity &expected,
                     Venue9EMaterialSnapshot &material,
                     uint32_t &guest_read_failures,
                     uint32_t &texture_capture_failures) {
  material.vertex_declaration = draw.state.vertex_declaration != 0
                                    ? draw.state.vertex_declaration
                                    : draw.mesh.vertex_declaration;
  material.vertex_declaration_probe =
      ProbeVertexDeclaration(guest_base, material.vertex_declaration);
  guest_read_failures += material.vertex_declaration_probe.copy_failures;
  material.texture_fetch_0 = draw.state.texture_fetches[0];
  const xenos::xe_gpu_texture_fetch_t fetch =
      DecodeTextureFetch(material.texture_fetch_0);
  material.exact_texture_contract = ExactTextureFetchContract(fetch);
  const uint32_t texture_owner =
      draw.bound_shaders.pixel_shader_valid
          ? draw.bound_shaders.pixel_shader
          : draw.pass.pixel_shader;
  material.texture_0 = CaptureTextureSnapshot(
      guest_base, texture_owner, 0, material.texture_fetch_0,
      TextureMipCapture::kFullFetchRange);
  texture_capture_failures += material.texture_0 == nullptr;
  material.vertex_constants_0_6 = draw.state.vertex_constants_0_6;
  material.vertex_constants_12_15 = draw.state.vertex_constants_12_15;
  material.pixel_constant_20 = draw.state.pixel_constant_20;
  material.pixel_constant_254 = draw.state.pixel_constant_254;
  material.pixel_constant_255 = draw.state.pixel_constant_255;
  material.valid =
      draw.state.valid && material.vertex_declaration != 0 &&
      DeclarationIdentityForProbe(material.vertex_declaration_probe) ==
      expected &&
      material.exact_texture_contract && material.texture_0 != nullptr &&
      material.texture_0->full_mip_chain() &&
      AllFinite(material.vertex_constants_0_6) &&
      AllFinite(material.vertex_constants_12_15) &&
      AllFinite(material.pixel_constant_20) &&
      AllFinite(material.pixel_constant_254) &&
      AllFinite(material.pixel_constant_255);
  return material.valid;
}

} // namespace

bool Venue9ETitleProgramIdentity::valid() const {
  return vertex_shader != 0 && pixel_shader != 0 &&
         vertex_shader_hash == kVertexShaderHash &&
         pixel_shader_hash == kPixelShaderHash;
}

bool Venue9EVertexDeclarationIdentity::valid() const {
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
    const Venue9EVertexElementIdentity &element = elements[index];
    if (element.stream != 0 || element.byte_offset != kOffsets[index] ||
        element.packed_type != kPackedTypes[index] || element.method != 0 ||
        element.usage != kUsages[index] || element.usage_index != 0) {
      return false;
    }
  }
  return true;
}

Venue9ETitleCandidate
ClassifyVenue9ETitleCandidate(uint8_t *guest_base,
                              const SceneCatalogDrawOccurrence &draw) {
  Venue9ETitleCandidate candidate;
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
      .vertex_hash_from_bound_shader =
          draw.bound_shaders.vertex_shader_valid,
      .pixel_hash_from_bound_shader = draw.bound_shaders.pixel_shader_valid,
  };
  const uint32_t declaration = draw.state.vertex_declaration != 0
                                   ? draw.state.vertex_declaration
                                   : draw.mesh.vertex_declaration;
  candidate.vertex_declaration = DeclarationIdentityForProbe(
      ProbeVertexDeclaration(guest_base, declaration));
  candidate.sequence = draw.frame_sequence;
  candidate.ordinal = draw.ordinal;
  candidate.eligible = candidate.identity.valid() &&
                       candidate.program.valid() &&
                       candidate.vertex_declaration.valid();
  return candidate;
}

Venue9ETitleCapture
CaptureVenue9ETitleDraw(uint8_t *guest_base,
                        const SceneCatalogDrawOccurrence &draw,
                        const Venue9ETitleCandidate &candidate) {
  Venue9ETitleCapture capture;
  const Venue9ETitleCandidate current =
      ClassifyVenue9ETitleCandidate(guest_base, draw);
  if (!candidate.eligible || !current.eligible ||
      !(candidate.identity == current.identity) ||
      !(candidate.program == current.program) ||
      !(candidate.vertex_declaration == current.vertex_declaration) ||
      candidate.sequence != current.sequence ||
      candidate.ordinal != current.ordinal) {
    return capture;
  }

  auto snapshot = std::make_shared<Venue9ETitleDrawSnapshot>();
  snapshot->identity = candidate.identity;
  snapshot->program = candidate.program;
  snapshot->vertex_declaration = candidate.vertex_declaration;
  snapshot->sequence = candidate.sequence;
  snapshot->ordinal = candidate.ordinal;
  snapshot->owner = draw.owner;
  snapshot->material_shader = draw.scope.shader;
  snapshot->model = draw.scope.model;
  snapshot->geometry_index = draw.scope.geometry_index;
  snapshot->lod = draw.scope.lod;
  snapshot->alternate_pass = draw.scope.alternate_pass;
  snapshot->vertex_aggregate = draw.mesh.vertex_aggregate;
  snapshot->world = draw.world;
  snapshot->world_view_projection = draw.world_view_projection;

  if (snapshot->material_shader != 0 &&
      !TryReadBeU32(guest_base, snapshot->material_shader,
                    snapshot->material_shader_vtable)) {
    ++capture.guest_read_failures;
  }
  snapshot->vertices = CaptureVertices(guest_base, draw);
  capture.payload_copy_failures += snapshot->vertices == nullptr;
  if (snapshot->vertices != nullptr) {
    snapshot->indices =
        CaptureIndices(guest_base, draw, snapshot->vertices->vertex_count);
  }
  capture.payload_copy_failures += snapshot->indices == nullptr;
  const bool material_valid =
      CaptureMaterial(guest_base, draw, candidate.vertex_declaration,
                      snapshot->material, capture.guest_read_failures,
                      capture.texture_capture_failures);
  capture.material_validation_failures += !material_valid;

  snapshot->valid =
      capture.guest_read_failures == 0 &&
      capture.payload_copy_failures == 0 &&
      capture.texture_capture_failures == 0 &&
      capture.material_validation_failures == 0 &&
      snapshot->identity.valid() && snapshot->program.valid() &&
      snapshot->vertex_declaration.valid() &&
      snapshot->vertices != nullptr && snapshot->vertices->valid() &&
      snapshot->vertices->physical_address ==
          snapshot->identity.guest_vertex_base &&
      snapshot->vertices->byte_count ==
          snapshot->identity.guest_vertex_bytes &&
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
