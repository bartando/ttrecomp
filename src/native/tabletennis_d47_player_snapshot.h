#pragma once

#include "native/tabletennis_player_palette_write_observer.h"
#include "native/tabletennis_texture_snapshot.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace tabletennis::native {

struct SceneCatalogDrawOccurrence;

struct D47PlayerDrawIdentity {
  uint32_t primitive_type = 0;
  uint32_t submitted_index_count = 0;
  uint32_t guest_index_base = 0;

  bool valid() const {
    return primitive_type != 0 && submitted_index_count != 0 &&
           guest_index_base != 0;
  }
  bool operator==(const D47PlayerDrawIdentity&) const = default;
};

struct D47PlayerVertexFetch {
  uint32_t slot = 0;
  uint32_t physical_address = 0;
  uint32_t size = 0;
  uint32_t endian = 0;
  std::array<uint32_t, 2> words{};
  bool valid = false;
};

struct D47PlayerVertexPayload {
  D47PlayerVertexFetch fetch{};
  uint32_t source_virtual_alias = 0;
  uint32_t stride = 0;
  uint32_t vertex_count = 0;
  uint64_t payload_fingerprint = 0;
  std::vector<uint8_t> raw_bytes;

  bool valid() const {
    return fetch.valid && source_virtual_alias != 0 &&
           (stride == 36 || stride == 44) && vertex_count != 0 &&
           raw_bytes.size() == static_cast<size_t>(vertex_count) * stride;
  }
};

struct D47PlayerIndexPayload {
  uint32_t source_virtual_alias = 0;
  uint32_t physical_address = 0;
  uint32_t submitted_index_count = 0;
  uint64_t payload_fingerprint = 0;
  std::vector<uint8_t> raw_bytes;
  std::vector<uint16_t> indices;

  bool valid() const {
    return source_virtual_alias != 0 && physical_address != 0 &&
           submitted_index_count != 0 &&
           raw_bytes.size() ==
               static_cast<size_t>(submitted_index_count) * sizeof(uint16_t) &&
           indices.size() == submitted_index_count;
  }
};

struct D47PlayerPaletteRecord {
  std::array<float, 4> quaternion{};
  std::array<float, 3> translation{};
};

struct D47PlayerPalettePayload {
  D47PlayerVertexFetch fetch{};
  D47PaletteWritePairProof write_proof{};
  uint32_t record_count = 0;
  uint64_t payload_fingerprint = 0;
  std::vector<uint8_t> raw_bytes;
  std::vector<D47PlayerPaletteRecord> records;

  bool valid() const {
    return fetch.valid && write_proof.valid &&
           write_proof.record_count_per_half >= 1 &&
           write_proof.record_count_per_half <= 256 &&
           record_count == write_proof.record_count_per_half * 2 &&
           raw_bytes.size() == static_cast<size_t>(record_count) * 28 &&
           records.size() == record_count;
  }
};

struct D47PlayerMaterialSnapshot {
  static constexpr size_t kTextureBindingCount = 7;
  static constexpr size_t kMaterialTextureCount = 3;

  std::array<std::array<uint32_t, 6>, kTextureBindingCount> texture_fetches{};
  std::array<uint32_t, kTextureBindingCount> texture_view_swizzles{};
  // Slots 0-2 are the changing per-material maps. Slots 3-6 are retained as
  // exact fetch descriptors because they are borrowed screen/global inputs.
  std::array<std::shared_ptr<const TextureSnapshot>, kMaterialTextureCount>
      material_textures{};

  std::array<float, 16> vertex_constants_12_15{};
  std::array<float, 4> vertex_constant_19{};
  std::array<float, 32> vertex_constants_29_36{};
  std::array<float, 12> vertex_constants_46_48{};
  std::array<float, 4> vertex_constant_255{};

  std::array<float, 4> pixel_constant_19{};
  std::array<float, 28> pixel_constants_21_27{};
  std::array<float, 128> pixel_constants_46_77{};
  std::array<float, 16> pixel_constants_252_255{};
  bool valid = false;
};

struct D47PlayerBackendContract {
  uint64_t vertex_shader_hash = 0;
  uint64_t pixel_shader_hash = 0;
  uint32_t surface_pitch = 0;
  uint32_t render_pass_key = 0;
  uint32_t normalized_depth_control = 0;
  uint32_t normalized_color_mask = 0;
  uint32_t color_control = 0;
  uint32_t blend_control_0 = 0;
  uint32_t primitive_restart_index = 0;
  std::array<uint32_t, 4> color_attachment_formats{};
  uint32_t color_attachment_count = 0;
  uint32_t depth_attachment_format = 0;
  uint32_t stencil_attachment_format = 0;
  uint32_t sample_count = 0;
  uint64_t sample_mask = 0;
  bool primitive_restart_enabled = false;
  bool valid = false;
};

struct D47PlayerDrawSnapshot {
  std::shared_ptr<const D47PlayerVertexPayload> vertices;
  std::shared_ptr<const D47PlayerIndexPayload> indices;
  std::shared_ptr<const D47PlayerPalettePayload> palette;
  D47PlayerVertexFetch palette_fetch{};
  D47PlayerMaterialSnapshot material{};
  D47PlayerBackendContract backend{};
  D47PlayerDrawIdentity identity{};
  uint32_t ordinal = 0;
  uint32_t player = 0;
  uint32_t shader = 0;
  uint32_t model = 0;
  uint32_t geometry_index = 0;
  uint32_t pass_descriptor = 0;
  uint32_t program_pair = 0;
  uint32_t title_vertex_shader = 0;
  uint32_t title_pixel_shader = 0;
  uint32_t vertex_stride = 0;
  bool alternate_pass = false;
  bool valid = false;
};

struct D47PlayerTitleCaptureDiagnostic {
  static constexpr uint32_t kCacheProofFailure = 1u << 0;
  static constexpr uint32_t kVertexPayloadFailure = 1u << 1;
  static constexpr uint32_t kIndexPayloadFailure = 1u << 2;
  static constexpr uint32_t kPalettePayloadFailure = 1u << 3;
  static constexpr uint32_t kMaterialFailure = 1u << 4;
  static constexpr uint32_t kTextureFailure = 1u << 5;
  static constexpr uint32_t kGuestReadFailure = 1u << 6;

  uint32_t failure_mask = 0;
  uint32_t guest_read_failures = 0;
  uint32_t payload_copy_failures = 0;
  uint32_t texture_capture_failures = 0;

  bool failed(uint32_t failure) const {
    return (failure_mask & failure) != 0;
  }
};

struct D47PlayerTitleCapture {
  D47PlayerDrawIdentity identity{};
  std::shared_ptr<const D47PlayerDrawSnapshot> snapshot;
  D47PlayerTitleCaptureDiagnostic diagnostic{};
  bool family_candidate = false;
};

// Cheap family-agnostic skinned-player candidate. The backend hash/order
// proof, not a title shader hash, selects D47 from this superset.
bool IsStructuralD47PlayerTitleDraw(
    const SceneCatalogDrawOccurrence& draw);

// Captures one immutable title-side candidate. The value-only token is joined
// with the independent D47 backend hash stream before publication.
D47PlayerTitleCapture CaptureD47PlayerTitleDraw(
    uint8_t* guest_base, const SceneCatalogDrawOccurrence& draw);

// Attaches the exact late cache proof to a payload captured earlier in the
// same title generation. This performs no guest reads and returns null unless
// the immutable palette bytes and every other captured component validate.
std::shared_ptr<const D47PlayerDrawSnapshot>
FinalizeD47PlayerDrawSnapshot(
    uint8_t* guest_base, const D47PlayerDrawSnapshot& captured,
    const D47PaletteWritePairProof& write_proof);

// Dynamic palettes are frame-owned; static mesh and texture caches remain
// bounded and reusable.
void D47PlayerSnapshotFrameEnd();

}  // namespace tabletennis::native
