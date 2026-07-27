#include "native/tabletennis_native_observer.h"

#include "generated/default/tabletennis_init.h"
#include "native/tabletennis_guest_memory.h"
#include "native/tabletennis_mesh_payload_probe.h"
#include "native/tabletennis_mesh_snapshot.h"
#include "native/tabletennis_net_bb903_observer.h"
#include "native/tabletennis_observer_overlay.h"
#include "native/tabletennis_vertex_declaration.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <string_view>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_UINT32(
    tabletennis_native_observer_log_interval, 0, "Table Tennis",
    "Frames between observer-only lvlTable guest-layout reports (0 disables).")
    .range(0, 100000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace tabletennis::native {
namespace {

constexpr uint32_t kLvlTableRenderableVtable = 0x8204F6D4;
constexpr uint32_t kLvlTableRenderableOffset = 0x60;
constexpr uint32_t kEnvironmentClothVtable = 0x82071C34;
constexpr uint32_t kRmcDrawableVtable = 0x8204FFB4;
constexpr uint32_t kGrmModelGeomVtable = 0x8202F3CC;
constexpr uint32_t kGrmShaderGroupVtable = 0x8202F528;
// sub_820EE910: lis r11,-32160 (0x82600000), lwz r4,25420(r11).
constexpr uint32_t kActiveVertexStreamSelector = 0x8260634C;
constexpr size_t kLvlTableProbeBytes = 0xEC;
constexpr size_t kRmcDrawableProbeBytes = 0x44;
constexpr size_t kModelGeomProbeBytes = 0x18;
constexpr size_t kShaderGroupProbeBytes = 0x10;
constexpr size_t kGeometryRecordBytes = 0x28;
// D3D buffer resources expose their flags at +0, GPU address at +0x0C and
// byte size at +0x10, so include all five leading words.
constexpr size_t kRawResourceWordCount = 5;
constexpr size_t kRawVertexDeclarationWordCount = 10;
constexpr size_t kMaxModelsPerFrame = 8;
constexpr size_t kMaxGeometriesPerModel = 24;
constexpr uint16_t kMaxPlausibleGeometryCount = 2048;
constexpr uint16_t kMaxPlausibleShaderCount = 4096;
constexpr uint16_t kMaxModelListScan = 64;
// The aggregate contains four selector-indexed vertex streams at slots 0..3;
// slots 4..7 are the corresponding index wrappers.
constexpr uint32_t kMaxVertexStreamSelector = 3;

struct ChildProbe {
  uint32_t wrapper = 0;
  uint32_t wrapper_vtable = 0;
  uint32_t drawable = 0;
  uint32_t drawable_vtable = 0;
  uint32_t shader_group = 0;
  uint32_t shader_group_vtable = 0;
  uint32_t model_list = 0;
  uint32_t model_array = 0;
  uint16_t model_count = 0;
  bool drawable_layout_valid = false;
  bool model_list_layout_valid = false;
};

struct TableProbe {
  uint32_t renderable = 0;
  uint32_t owner = 0;
  uint32_t owner_vtable = 0;
  std::array<float, 16> transform{};
  bool transform_xyz_finite = false;
  uint32_t model_116 = 0;
  uint32_t model_124 = 0;
  uint32_t model_124_vtable = 0;
  std::array<ChildProbe, 3> children{};
  bool valid = false;
};

enum class ModelListMatch : uint8_t {
  kUnreadable,
  kFound,
  kNotFound,
  kTruncated,
};

struct GeometryProbe {
  uint16_t index = 0;
  uint16_t material_index = 0;
  uint32_t record = 0;
  uint32_t shader = 0;
  uint32_t shader_vtable = 0;
  uint32_t vertex_aggregate = 0;
  uint32_t secondary_vertex_stream = 0;
  uint32_t vertex_declaration = 0;
  VertexDeclarationProbe decoded_vertex_declaration{};
  std::array<uint32_t, kRawVertexDeclarationWordCount>
      vertex_declaration_words{};
  uint32_t stream_selector = 0;
  uint32_t primary_vertex_stream = 0;
  uint32_t vertex_buffer_resource = 0;
  std::array<uint32_t, kRawResourceWordCount> vertex_resource_words{};
  uint32_t vertex_buffer_alias = 0;
  uint32_t vertex_buffer_bytes = 0;
  uint8_t vertex_fetch_type = 0;
  uint8_t vertex_endian = 0;
  uint32_t vertex_stride = 0;
  uint32_t index_buffer_wrapper = 0;
  uint32_t index_buffer_resource = 0;
  uint32_t index_element_count = 0;
  uint32_t index_element_size = 0;
  uint32_t index_data = 0;
  std::array<uint32_t, kRawResourceWordCount> index_resource_words{};
  uint32_t index_buffer_alias = 0;
  uint32_t index_buffer_bytes = 0;
  bool index_is_32_bit = false;
  uint32_t secondary_vertex_buffer_resource = 0;
  uint32_t secondary_vertex_stride = 0;
  uint16_t submitted_index_count = 0;
  uint16_t primitive_type = 0;
  uint8_t use_global_stream_selector = 0;
  uint8_t use_alternate_stream = 0;
  uint32_t copy_failures = 0;
  bool valid = false;
};

struct ModelGeometryProbe {
  uint32_t model = 0;
  uint32_t model_vtable = 0;
  uint32_t shader_group = 0;
  uint32_t shader_group_vtable = 0;
  uint32_t shader_array = 0;
  uint32_t geometry_records = 0;
  uint32_t material_indices = 0;
  uint32_t table_drawable = 0;
  uint32_t render_category = 0;
  uint32_t lod = 0;
  uint16_t shader_count = 0;
  uint16_t geometry_count = 0;
  uint16_t captured_geometry_count = 0;
  uint16_t dropped_geometry_count = 0;
  uint32_t copy_failures = 0;
  uint8_t associated_child = 0;
  ModelListMatch model_list_match = ModelListMatch::kUnreadable;
  std::array<GeometryProbe, kMaxGeometriesPerModel> geometries{};
  bool expected_vtables = false;
  bool valid = false;
};

struct ModelGeometryFrame {
  uint32_t associated_submissions = 0;
  uint32_t model_count = 0;
  uint32_t dropped_models = 0;
  std::array<ModelGeometryProbe, kMaxModelsPerFrame> models{};
};

std::mutex g_observer_mutex;
std::mutex g_payload_probe_mutex;
TableProbe g_current_table;
ModelGeometryFrame g_building_geometry;
ModelGeometryFrame g_published_geometry;
uint64_t g_observer_frame = 0;
uint64_t g_table_candidates = 0;
uint64_t g_table_copy_failures = 0;
uint64_t g_model_submissions = 0;
uint64_t g_shader_group_associations = 0;
uint64_t g_model_probe_failures = 0;
uint64_t g_model_probe_drops = 0;
std::array<MeshPayloadProbe, 4> g_mesh_payload_probes{};
std::array<bool, 4> g_mesh_payload_probe_captured{};

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

bool TryCopyGuest(uint8_t* base, uint32_t address, size_t offset,
                  void* destination, size_t size) {
  uint32_t guest_address;
  return base != nullptr &&
         CheckedGuestOffset(address, offset, size, guest_address) &&
         GuestTryCopy(destination, REX_RAW_ADDR(guest_address), size);
}

uint32_t LoadBeU32(const std::byte* data, size_t offset) {
  uint32_t value;
  std::memcpy(&value, data + offset, sizeof(value));
  return std::byteswap(value);
}

uint16_t LoadBeU16(const std::byte* data, size_t offset) {
  uint16_t value;
  std::memcpy(&value, data + offset, sizeof(value));
  return std::byteswap(value);
}

float LoadBeF32(const std::byte* data, size_t offset) {
  return std::bit_cast<float>(LoadBeU32(data, offset));
}

bool TryReadBeU32(uint8_t* base, uint32_t address, size_t offset,
                  uint32_t& value) {
  std::array<std::byte, sizeof(uint32_t)> bytes;
  if (!TryCopyGuest(base, address, offset, bytes.data(), bytes.size())) {
    return false;
  }
  value = LoadBeU32(bytes.data(), 0);
  return true;
}

bool TryReadBeU16(uint8_t* base, uint32_t address, size_t offset,
                  uint16_t& value) {
  std::array<std::byte, sizeof(uint16_t)> bytes;
  if (!TryCopyGuest(base, address, offset, bytes.data(), bytes.size())) {
    return false;
  }
  value = LoadBeU16(bytes.data(), 0);
  return true;
}

template <size_t WordCount>
bool TryReadRawWords(uint8_t* base, uint32_t address, size_t offset,
                     std::array<uint32_t, WordCount>& words) {
  std::array<std::byte, sizeof(uint32_t) * WordCount> bytes;
  if (!TryCopyGuest(base, address, offset, bytes.data(), bytes.size())) {
    return false;
  }
  for (size_t index = 0; index < words.size(); ++index) {
    words[index] = LoadBeU32(bytes.data(), index * sizeof(uint32_t));
  }
  return true;
}

uint32_t ProbeVtable(uint8_t* base, uint32_t address) {
  uint32_t vtable = 0;
  TryReadBeU32(base, address, 0, vtable);
  return vtable;
}

ChildProbe ProbeChild(uint8_t* base, uint32_t wrapper) {
  ChildProbe child;
  child.wrapper = wrapper;
  if (wrapper == 0) {
    return child;
  }

  std::array<std::byte, 8> wrapper_bytes;
  if (!TryCopyGuest(base, wrapper, 0, wrapper_bytes.data(),
                    wrapper_bytes.size())) {
    return child;
  }
  child.wrapper_vtable = LoadBeU32(wrapper_bytes.data(), 0);
  child.drawable = LoadBeU32(wrapper_bytes.data(), 4);
  child.drawable_vtable = ProbeVtable(base, child.drawable);

  if (child.drawable_vtable != kRmcDrawableVtable) {
    return child;
  }

  std::array<std::byte, kRmcDrawableProbeBytes> drawable_bytes;
  if (!TryCopyGuest(base, child.drawable, 0, drawable_bytes.data(),
                    drawable_bytes.size())) {
    return child;
  }
  child.shader_group = LoadBeU32(drawable_bytes.data(), 0x04);
  child.shader_group_vtable = ProbeVtable(base, child.shader_group);
  child.model_list = LoadBeU32(drawable_bytes.data(), 0x40);
  child.drawable_layout_valid = true;

  std::array<std::byte, 6> model_list_bytes;
  if (!TryCopyGuest(base, child.model_list, 0, model_list_bytes.data(),
                    model_list_bytes.size())) {
    return child;
  }
  child.model_array = LoadBeU32(model_list_bytes.data(), 0);
  child.model_count = LoadBeU16(model_list_bytes.data(), 4);
  child.model_list_layout_valid = true;
  return child;
}

std::string_view ModelListMatchName(ModelListMatch match) {
  switch (match) {
    case ModelListMatch::kUnreadable:
      return "unreadable";
    case ModelListMatch::kFound:
      return "found";
    case ModelListMatch::kNotFound:
      return "not-found";
    case ModelListMatch::kTruncated:
      return "not-found-truncated";
  }
  return "unknown";
}

ModelListMatch MatchModelInDrawableList(uint8_t* base,
                                        const ChildProbe& child,
                                        uint32_t model,
                                        uint32_t& copy_failures) {
  if (!child.model_list_layout_valid || child.model_array == 0) {
    return ModelListMatch::kUnreadable;
  }

  const uint16_t scan_count =
      std::min(child.model_count, kMaxModelListScan);
  for (uint16_t index = 0; index < scan_count; ++index) {
    uint32_t list_model = 0;
    if (!TryReadBeU32(base, child.model_array,
                      static_cast<size_t>(index) * sizeof(uint32_t),
                      list_model)) {
      ++copy_failures;
      return ModelListMatch::kUnreadable;
    }
    if (list_model == model) {
      return ModelListMatch::kFound;
    }
  }
  return child.model_count > scan_count ? ModelListMatch::kTruncated
                                        : ModelListMatch::kNotFound;
}

void ProbeVertexStream(uint8_t* base, uint32_t stream,
                       uint32_t& buffer_handle, uint32_t& stride,
                       uint32_t& copy_failures) {
  if (stream == 0) {
    return;
  }
  if (!TryReadBeU32(base, stream, 0x10, buffer_handle)) {
    ++copy_failures;
  }
  if (!TryReadBeU32(base, stream, 0xD4, stride)) {
    ++copy_failures;
  }
}

GeometryProbe ProbeGeometry(uint8_t* base, const ModelGeometryProbe& model,
                            uint16_t geometry_index) {
  GeometryProbe geometry;
  geometry.index = geometry_index;

  if (!CheckedGuestOffset(
          model.geometry_records,
          static_cast<size_t>(geometry_index) * kGeometryRecordBytes,
          kGeometryRecordBytes, geometry.record)) {
    ++geometry.copy_failures;
    return geometry;
  }

  std::array<std::byte, kGeometryRecordBytes> record_bytes;
  if (!TryCopyGuest(base, geometry.record, 0, record_bytes.data(),
                    record_bytes.size())) {
    ++geometry.copy_failures;
    return geometry;
  }
  geometry.vertex_aggregate = LoadBeU32(record_bytes.data(), 0x00);
  // sub_820EE910 passes record+4 to sub_82357400. That routine retains it as
  // the device's vertex-declaration object; it is not an index-buffer address.
  geometry.vertex_declaration = LoadBeU32(record_bytes.data(), 0x04);
  geometry.secondary_vertex_stream = LoadBeU32(record_bytes.data(), 0x20);
  geometry.use_global_stream_selector =
      std::to_integer<uint8_t>(record_bytes[0x25]);
  geometry.use_alternate_stream =
      std::to_integer<uint8_t>(record_bytes[0x27]);
  geometry.valid = true;

  if (!TryReadBeU16(base, model.material_indices,
                    static_cast<size_t>(geometry_index) * sizeof(uint16_t),
                    geometry.material_index)) {
    ++geometry.copy_failures;
  } else if (geometry.material_index < model.shader_count &&
             model.shader_count <= kMaxPlausibleShaderCount &&
             !TryReadBeU32(base, model.shader_array,
                           static_cast<size_t>(geometry.material_index) *
                               sizeof(uint32_t),
                           geometry.shader)) {
    ++geometry.copy_failures;
  }
  geometry.shader_vtable = ProbeVtable(base, geometry.shader);

  if (geometry.vertex_declaration != 0 &&
      !TryReadRawWords(base, geometry.vertex_declaration, 0,
                       geometry.vertex_declaration_words)) {
    ++geometry.copy_failures;
  }
  geometry.decoded_vertex_declaration =
      ProbeVertexDeclaration(base, geometry.vertex_declaration);
  geometry.copy_failures +=
      geometry.decoded_vertex_declaration.copy_failures;

  // sub_820EE6E8 passes these directly to sub_82357B30. The live value 4 is
  // D3DPT_TRIANGLELIST; +0x36 is the submitted index count.
  if (!TryReadBeU16(base, geometry.vertex_aggregate, 0x36,
                    geometry.submitted_index_count)) {
    ++geometry.copy_failures;
  }
  if (!TryReadBeU16(base, geometry.vertex_aggregate, 0x40,
                    geometry.primitive_type)) {
    ++geometry.copy_failures;
  }

  if (geometry.use_global_stream_selector != 0 &&
      !TryReadBeU32(base, kActiveVertexStreamSelector, 0,
                    geometry.stream_selector)) {
    ++geometry.copy_failures;
  }
  if (geometry.stream_selector <= kMaxVertexStreamSelector) {
    const size_t stream_offset =
        geometry.use_alternate_stream != 0
            ? 0x30
            : static_cast<size_t>(geometry.stream_selector) *
                  sizeof(uint32_t);
    if (!TryReadBeU32(base, geometry.vertex_aggregate, stream_offset,
                      geometry.primary_vertex_stream)) {
      ++geometry.copy_failures;
    }

    const size_t index_wrapper_offset =
        static_cast<size_t>(geometry.stream_selector + 4) * sizeof(uint32_t);
    if (!TryReadBeU32(base, geometry.vertex_aggregate, index_wrapper_offset,
                      geometry.index_buffer_wrapper)) {
      ++geometry.copy_failures;
    }
  } else {
    ++geometry.copy_failures;
  }

  // sub_820EE6E8 dereferences the selected wrapper at +0x0C and passes that
  // resource to sub_82356210. sub_821589B8 proves the wrapper fields at
  // +0/+4/+0x0C/+0x10 are element count, element size, GPU resource and
  // mapped/staging data respectively.
  if (geometry.index_buffer_wrapper != 0) {
    if (!TryReadBeU32(base, geometry.index_buffer_wrapper, 0x00,
                      geometry.index_element_count)) {
      ++geometry.copy_failures;
    }
    if (!TryReadBeU32(base, geometry.index_buffer_wrapper, 0x04,
                      geometry.index_element_size)) {
      ++geometry.copy_failures;
    }
    if (!TryReadBeU32(base, geometry.index_buffer_wrapper, 0x0C,
                      geometry.index_buffer_resource)) {
      ++geometry.copy_failures;
    }
    if (!TryReadBeU32(base, geometry.index_buffer_wrapper, 0x10,
                      geometry.index_data)) {
      ++geometry.copy_failures;
    }
  }
  if (geometry.index_buffer_resource != 0 &&
      !TryReadRawWords(base, geometry.index_buffer_resource, 0,
                       geometry.index_resource_words)) {
    ++geometry.copy_failures;
  } else if (geometry.index_buffer_resource != 0) {
    geometry.index_is_32_bit =
        (geometry.index_resource_words[0] & 0x80000000u) != 0;
    geometry.index_buffer_alias = geometry.index_resource_words[3];
    geometry.index_buffer_bytes = geometry.index_resource_words[4];
  }

  ProbeVertexStream(base, geometry.primary_vertex_stream,
                    geometry.vertex_buffer_resource, geometry.vertex_stride,
                    geometry.copy_failures);
  if (geometry.vertex_buffer_resource != 0 &&
      !TryReadRawWords(base, geometry.vertex_buffer_resource, 0,
                       geometry.vertex_resource_words)) {
    ++geometry.copy_failures;
  } else if (geometry.vertex_buffer_resource != 0) {
    // Older Xenos D3D vertex buffers embed the two fetch-constant words.
    // Dword 0 is the aligned guest alias plus the 2-bit fetch type. Dword 1
    // contains the byte size in bits 2..25 and the endian mode in bits 0..1.
    geometry.vertex_fetch_type =
        static_cast<uint8_t>(geometry.vertex_resource_words[3] & 0x3u);
    geometry.vertex_buffer_alias =
        geometry.vertex_resource_words[3] & ~0x3u;
    geometry.vertex_endian =
        static_cast<uint8_t>(geometry.vertex_resource_words[4] & 0x3u);
    geometry.vertex_buffer_bytes =
        geometry.vertex_resource_words[4] & 0x03FFFFFCu;
  }
  ProbeVertexStream(base, geometry.secondary_vertex_stream,
                    geometry.secondary_vertex_buffer_resource,
                    geometry.secondary_vertex_stride,
                    geometry.copy_failures);
  return geometry;
}

void MaybeProbeMeshPayload(uint8_t* base, const GeometryProbe& geometry) {
  if (!geometry.valid || geometry.copy_failures != 0 ||
      geometry.stream_selector >= g_mesh_payload_probes.size()) {
    return;
  }

  MeshPayloadDescriptor descriptor;
  descriptor.stream_selector = geometry.stream_selector;
  descriptor.vertex_alias = geometry.vertex_buffer_alias;
  descriptor.vertex_bytes = geometry.vertex_buffer_bytes;
  descriptor.vertex_stride = geometry.vertex_stride;
  descriptor.vertex_endian = geometry.vertex_endian;
  descriptor.index_alias = geometry.index_buffer_alias;
  descriptor.index_bytes = geometry.index_buffer_bytes;
  descriptor.submitted_index_count = geometry.submitted_index_count;
  descriptor.index_element_size = geometry.index_element_size;
  descriptor.index_is_32_bit = geometry.index_is_32_bit;
  descriptor.declaration = geometry.decoded_vertex_declaration;

  TryCaptureTableMeshSnapshot(base, descriptor);

  std::lock_guard lock(g_payload_probe_mutex);
  const size_t selector = geometry.stream_selector;
  if (g_mesh_payload_probe_captured[selector]) {
    return;
  }

  MeshPayloadProbe probe = ProbeMeshPayload(base, descriptor);
  g_mesh_payload_probes[selector] = probe;
  // A streaming fault is transient, so retry on a later submission. A
  // structurally invalid but readable payload is stable telemetry and should
  // be reported once rather than copied on every draw.
  g_mesh_payload_probe_captured[selector] =
      probe.valid || probe.copy_failures == 0;
}

ModelGeometryProbe ProbeModelGeometry(uint8_t* base, uint32_t model,
                                      uint32_t shader_group,
                                      uint32_t render_category, uint32_t lod,
                                      const ChildProbe& child,
                                      uint8_t associated_child) {
  ModelGeometryProbe probe;
  probe.model = model;
  probe.shader_group = shader_group;
  probe.table_drawable = child.drawable;
  probe.render_category = render_category;
  probe.lod = lod;
  probe.associated_child = associated_child;
  probe.model_list_match =
      MatchModelInDrawableList(base, child, model, probe.copy_failures);

  std::array<std::byte, kModelGeomProbeBytes> model_bytes;
  if (!TryCopyGuest(base, model, 0, model_bytes.data(), model_bytes.size())) {
    ++probe.copy_failures;
    return probe;
  }
  probe.model_vtable = LoadBeU32(model_bytes.data(), 0x00);
  probe.geometry_records = LoadBeU32(model_bytes.data(), 0x04);
  probe.material_indices = LoadBeU32(model_bytes.data(), 0x0C);
  probe.geometry_count = LoadBeU16(model_bytes.data(), 0x16);

  std::array<std::byte, kShaderGroupProbeBytes> shader_group_bytes;
  if (!TryCopyGuest(base, shader_group, 0, shader_group_bytes.data(),
                    shader_group_bytes.size())) {
    ++probe.copy_failures;
    return probe;
  }
  probe.shader_group_vtable = LoadBeU32(shader_group_bytes.data(), 0x00);
  probe.shader_array = LoadBeU32(shader_group_bytes.data(), 0x08);
  probe.shader_count = LoadBeU16(shader_group_bytes.data(), 0x0C);
  probe.expected_vtables =
      probe.model_vtable == kGrmModelGeomVtable &&
      probe.shader_group_vtable == kGrmShaderGroupVtable;
  probe.valid = true;

  if (probe.geometry_count > kMaxPlausibleGeometryCount ||
      probe.shader_count > kMaxPlausibleShaderCount ||
      probe.geometry_records == 0 || probe.material_indices == 0 ||
      probe.shader_array == 0) {
    ++probe.copy_failures;
    return probe;
  }

  probe.captured_geometry_count = static_cast<uint16_t>(
      std::min<size_t>(probe.geometry_count, probe.geometries.size()));
  probe.dropped_geometry_count =
      probe.geometry_count - probe.captured_geometry_count;
  for (uint16_t index = 0; index < probe.captured_geometry_count; ++index) {
    probe.geometries[index] = ProbeGeometry(base, probe, index);
    MaybeProbeMeshPayload(base, probe.geometries[index]);
    probe.copy_failures += probe.geometries[index].copy_failures;
  }
  return probe;
}

void StoreModelGeometryProbe(const ModelGeometryProbe& probe) {
  std::lock_guard lock(g_observer_mutex);
  ++g_shader_group_associations;
  g_model_probe_failures += probe.copy_failures;
  ++g_building_geometry.associated_submissions;

  for (uint32_t index = 0; index < g_building_geometry.model_count; ++index) {
    ModelGeometryProbe& existing = g_building_geometry.models[index];
    if (existing.model == probe.model &&
        existing.shader_group == probe.shader_group) {
      existing = probe;
      return;
    }
  }
  if (g_building_geometry.model_count < g_building_geometry.models.size()) {
    g_building_geometry.models[g_building_geometry.model_count++] = probe;
  } else {
    ++g_building_geometry.dropped_models;
    ++g_model_probe_drops;
  }
}

}  // namespace

void ObserveDrawBucketEntry(uint8_t* base, uint32_t renderable,
                            uint32_t vtable) {
  if (base == nullptr || vtable != kLvlTableRenderableVtable ||
      renderable < kLvlTableRenderableOffset) {
    return;
  }

  TableProbe probe;
  probe.renderable = renderable;
  probe.owner = renderable - kLvlTableRenderableOffset;

  std::array<std::byte, kLvlTableProbeBytes> bytes;
  if (!TryCopyGuest(base, probe.owner, 0, bytes.data(), bytes.size())) {
    std::lock_guard lock(g_observer_mutex);
    ++g_table_candidates;
    ++g_table_copy_failures;
    return;
  }

  probe.owner_vtable = LoadBeU32(bytes.data(), 0);
  probe.transform_xyz_finite = true;
  for (size_t index = 0; index < probe.transform.size(); ++index) {
    probe.transform[index] = LoadBeF32(bytes.data(), 0x10 + index * 4);
  }
  // RAGE matrices use the W lane of each basis vector as padding; those
  // values are NaN in the live lvlTable. Validate only basis XYZ and
  // translation XYZ.
  constexpr std::array<size_t, 12> kMeaningfulTransformComponents = {
      0, 1, 2, 4, 5, 6, 8, 9, 10, 12, 13, 14};
  for (const size_t index : kMeaningfulTransformComponents) {
    probe.transform_xyz_finite &=
        std::isfinite(probe.transform[index]) &&
        std::abs(probe.transform[index]) < 1000000.0f;
  }

  probe.model_116 = LoadBeU32(bytes.data(), 0x74);
  probe.model_124 = LoadBeU32(bytes.data(), 0x7C);
  probe.model_124_vtable = ProbeVtable(base, probe.model_124);
  constexpr std::array<size_t, 3> kChildOffsets = {0xDC, 0xE0, 0xE4};
  for (size_t index = 0; index < kChildOffsets.size(); ++index) {
    probe.children[index] =
        ProbeChild(base, LoadBeU32(bytes.data(), kChildOffsets[index]));
  }
  probe.valid = true;

  std::lock_guard lock(g_observer_mutex);
  ++g_table_candidates;
  g_current_table = probe;
}

bool ObserveModelGeometrySubmission(uint8_t* base, uint32_t model,
                                    uint32_t shader_group,
                                    uint32_t render_category, uint32_t lod) {
  if (base == nullptr || model == 0 || shader_group == 0) {
    return false;
  }

  TableProbe table;
  {
    std::lock_guard lock(g_observer_mutex);
    ++g_model_submissions;
    table = g_current_table;
  }
  if (!table.valid) {
    return false;
  }

  for (size_t index = 0; index < table.children.size(); ++index) {
    const ChildProbe& child = table.children[index];
    if (!child.drawable_layout_valid ||
        child.drawable_vtable != kRmcDrawableVtable ||
        child.shader_group != shader_group) {
      continue;
    }
    const bool telemetry_enabled =
        REXCVAR_GET(tabletennis_native_observer_log_interval) != 0;
    if (telemetry_enabled ||
        ((ObserverOverlayEnabled() || NetBB903ObserverEnabled()) &&
         !HasTableMeshSnapshot())) {
      ModelGeometryProbe probe = ProbeModelGeometry(
          base, model, shader_group, render_category, lod, child,
          static_cast<uint8_t>(index));
      if (telemetry_enabled) {
        StoreModelGeometryProbe(probe);
      }
    }
    return true;
  }
  return false;
}

void ObserverFrameEnd() {
  const uint32_t interval =
      REXCVAR_GET(tabletennis_native_observer_log_interval);
  if (interval == 0) {
    return;
  }

  std::lock_guard lock(g_observer_mutex);
  ++g_observer_frame;
  g_published_geometry = g_building_geometry;
  g_building_geometry = {};
  if (g_observer_frame % interval != 0) {
    return;
  }

  if (!g_current_table.valid) {
    REXLOG_INFO(
        "Table Tennis observer: frame={} waiting for lvlTable "
        "(candidates={} copy_failures={})",
        g_observer_frame, g_table_candidates, g_table_copy_failures);
    return;
  }

  const TableProbe& table = g_current_table;
  REXLOG_INFO(
      "Table Tennis observer: frame={} lvlTable renderable={:08X} "
      "owner={:08X} owner_vt={:08X} transform_xyz_finite={} "
      "model116={:08X} model124={:08X} model124_vt={:08X} "
      "candidates={} copy_failures={}",
      g_observer_frame, table.renderable, table.owner, table.owner_vtable,
      table.transform_xyz_finite, table.model_116, table.model_124,
      table.model_124_vtable, g_table_candidates, g_table_copy_failures);
  REXLOG_INFO(
      "  world rows: [{:.3f} {:.3f} {:.3f} {:.3f}] "
      "[{:.3f} {:.3f} {:.3f} {:.3f}] "
      "[{:.3f} {:.3f} {:.3f} {:.3f}] "
      "[{:.3f} {:.3f} {:.3f} {:.3f}]",
      table.transform[0], table.transform[1], table.transform[2],
      table.transform[3], table.transform[4], table.transform[5],
      table.transform[6], table.transform[7], table.transform[8],
      table.transform[9], table.transform[10], table.transform[11],
      table.transform[12], table.transform[13], table.transform[14],
      table.transform[15]);
  for (size_t index = 0; index < table.children.size(); ++index) {
    const ChildProbe& child = table.children[index];
    REXLOG_INFO(
        "  child[{}] wrapper={:08X} wrapper_vt={:08X} "
        "drawable={:08X} drawable_vt={:08X} shader_group={:08X} "
        "shader_group_vt={:08X} lod0_list={:08X} model_array={:08X} "
        "model_count={} expected_chain={}",
        index, child.wrapper, child.wrapper_vtable, child.drawable,
        child.drawable_vtable, child.shader_group,
        child.shader_group_vtable, child.model_list, child.model_array,
        child.model_count,
        child.wrapper_vtable == kEnvironmentClothVtable &&
            child.drawable_vtable == kRmcDrawableVtable &&
            child.shader_group_vtable == kGrmShaderGroupVtable);
  }

  const ModelGeometryFrame& geometry_frame = g_published_geometry;
  REXLOG_INFO(
      "  geometry observer: frame_associated={} models={} dropped_models={} "
      "all_model_submissions={} shader_group_associations={} "
      "read_failures={} model_drops={} "
      "association=shader-group-pointer-observer-only",
      geometry_frame.associated_submissions, geometry_frame.model_count,
      geometry_frame.dropped_models, g_model_submissions,
      g_shader_group_associations, g_model_probe_failures,
      g_model_probe_drops);
  for (uint32_t model_index = 0;
       model_index < geometry_frame.model_count; ++model_index) {
    const ModelGeometryProbe& model = geometry_frame.models[model_index];
    REXLOG_INFO(
        "    model[{}]={:08X} vt={:08X} shader_group={:08X} "
        "shader_group_vt={:08X} shaders={:08X}/{} category={} lod={} "
        "child={} drawable={:08X} list_match={} expected_vtables={} "
        "records={:08X} materials={:08X} geometry={}/{} dropped={} "
        "read_failures={}",
        model_index, model.model, model.model_vtable, model.shader_group,
        model.shader_group_vtable, model.shader_array, model.shader_count,
        model.render_category, model.lod, model.associated_child,
        model.table_drawable, ModelListMatchName(model.model_list_match),
        model.expected_vtables, model.geometry_records,
        model.material_indices, model.captured_geometry_count,
        model.geometry_count, model.dropped_geometry_count,
        model.copy_failures);
    for (uint16_t geometry_index = 0;
         geometry_index < model.captured_geometry_count; ++geometry_index) {
      const GeometryProbe& geometry = model.geometries[geometry_index];
      REXLOG_INFO(
          "      geom[{}] record={:08X} material={} shader={:08X} "
          "shader_vt={:08X} aggregate={:08X} selector={} global={} alt={} "
          "stream={:08X} vb_resource={:08X} stride={} "
          "vb_alias={:08X} vb_bytes={} fetch_type={} endian={} "
          "secondary={:08X} secondary_vb_resource={:08X} "
          "secondary_stride={} primitive_type={} submitted_indices={} "
          "vb_resource_words=[{:08X} {:08X} {:08X} {:08X} {:08X}] "
          "valid={} failures={}",
          geometry.index, geometry.record, geometry.material_index,
          geometry.shader, geometry.shader_vtable,
          geometry.vertex_aggregate, geometry.stream_selector,
          geometry.use_global_stream_selector,
          geometry.use_alternate_stream, geometry.primary_vertex_stream,
          geometry.vertex_buffer_resource, geometry.vertex_stride,
          geometry.vertex_buffer_alias, geometry.vertex_buffer_bytes,
          geometry.vertex_fetch_type, geometry.vertex_endian,
          geometry.secondary_vertex_stream,
          geometry.secondary_vertex_buffer_resource,
          geometry.secondary_vertex_stride, geometry.primitive_type,
          geometry.submitted_index_count,
          geometry.vertex_resource_words[0],
          geometry.vertex_resource_words[1],
          geometry.vertex_resource_words[2],
          geometry.vertex_resource_words[3],
          geometry.vertex_resource_words[4],
          geometry.valid, geometry.copy_failures);
      REXLOG_INFO(
          "        index_wrapper={:08X} elements={} element_size={} "
          "resource={:08X} data={:08X} alias={:08X} bytes={} index32={} "
          "resource_words=[{:08X} {:08X} {:08X} {:08X} {:08X}]",
          geometry.index_buffer_wrapper, geometry.index_element_count,
          geometry.index_element_size, geometry.index_buffer_resource,
          geometry.index_data, geometry.index_buffer_alias,
          geometry.index_buffer_bytes, geometry.index_is_32_bit,
          geometry.index_resource_words[0],
          geometry.index_resource_words[1],
          geometry.index_resource_words[2],
          geometry.index_resource_words[3],
          geometry.index_resource_words[4]);
      REXLOG_INFO(
          "        vertex_decl={:08X} words=[{:08X} {:08X} {:08X} "
          "{:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}]",
          geometry.vertex_declaration,
          geometry.vertex_declaration_words[0],
          geometry.vertex_declaration_words[1],
          geometry.vertex_declaration_words[2],
          geometry.vertex_declaration_words[3],
          geometry.vertex_declaration_words[4],
          geometry.vertex_declaration_words[5],
          geometry.vertex_declaration_words[6],
          geometry.vertex_declaration_words[7],
          geometry.vertex_declaration_words[8],
          geometry.vertex_declaration_words[9]);
      const VertexDeclarationProbe& declaration =
          geometry.decoded_vertex_declaration;
      REXLOG_INFO(
          "        declaration count={} max_stream={} masks={:016X}/"
          "{:016X} cache_id={:08X} valid={} failures={}",
          declaration.element_count, declaration.max_stream,
          declaration.stream_mask_lo, declaration.stream_mask_hi,
          declaration.cache_id, declaration.valid,
          declaration.copy_failures);
      if (declaration.valid) {
        for (uint32_t element_index = 0;
             element_index < declaration.element_count; ++element_index) {
          const VertexDeclarationElement& element =
              declaration.elements[element_index];
          REXLOG_INFO(
              "          element[{}] stream={} offset={} type={:08X} "
              "format={} signed={} normalized={} method={} usage={} "
              "usage_index={} unused_padding={}",
              element_index, element.stream, element.byte_offset,
              element.packed_type, element.format(), element.is_signed(),
              element.normalized(), element.method, element.usage,
              element.usage_index, element.unused_padding);
        }
      }
    }
  }

  std::array<MeshPayloadProbe, 4> payload_probes;
  std::array<bool, 4> payload_captured;
  {
    std::lock_guard payload_lock(g_payload_probe_mutex);
    payload_probes = g_mesh_payload_probes;
    payload_captured = g_mesh_payload_probe_captured;
  }
  bool all_payloads_valid = true;
  bool vertex_payloads_identical = true;
  bool index_topology_identical = true;
  uint64_t reference_vertex_fingerprint = 0;
  uint64_t reference_index_fingerprint = 0;
  for (size_t selector = 0; selector < payload_probes.size(); ++selector) {
    const MeshPayloadProbe& payload = payload_probes[selector];
    all_payloads_valid &=
        payload_captured[selector] && payload.valid;
    if (selector == 0) {
      reference_vertex_fingerprint = payload.vertex_fingerprint;
      reference_index_fingerprint = payload.index_fingerprint;
    } else {
      vertex_payloads_identical &=
          payload.vertex_fingerprint == reference_vertex_fingerprint;
      index_topology_identical &=
          payload.index_fingerprint == reference_index_fingerprint;
    }
    if (!payload_captured[selector]) {
      REXLOG_INFO("  payload[{}] waiting for draw-time capture", selector);
      continue;
    }
    REXLOG_INFO(
        "  payload[{}] valid={} vb={:08X}/{} stride={} vertices={} "
        "ib={:08X}/{} indices={} range={}..{} out_of_range={} "
        "degenerate={} finite_positions={} non_finite_positions={} "
        "non_finite_w={} "
        "bounds=({:.3f},{:.3f},{:.3f})..({:.3f},{:.3f},{:.3f}) "
        "first=({:.3f},{:.3f},{:.3f},{:.3f}) "
        "fingerprints={:016X}/{:016X} copy_failures={}",
        selector, payload.valid, payload.vertex_alias,
        payload.vertex_bytes, payload.vertex_stride, payload.vertex_count,
        payload.index_alias, payload.index_bytes, payload.index_count,
        payload.minimum_index, payload.maximum_index,
        payload.out_of_range_indices, payload.degenerate_triangles,
        payload.finite_positions, payload.non_finite_positions,
        payload.non_finite_position_w,
        payload.bounds_min[0], payload.bounds_min[1], payload.bounds_min[2],
        payload.bounds_max[0], payload.bounds_max[1], payload.bounds_max[2],
        payload.first_position[0], payload.first_position[1],
        payload.first_position[2], payload.first_position[3],
        payload.vertex_fingerprint, payload.index_fingerprint,
        payload.copy_failures);
  }
  REXLOG_INFO(
      "  payload coherence: all_four_valid={} index_topology_identical={} "
      "vertex_payloads_identical={} dynamic_vertex_variation={} "
      "observer_only=true",
      all_payloads_valid,
      all_payloads_valid && index_topology_identical,
      all_payloads_valid && vertex_payloads_identical,
      all_payloads_valid && !vertex_payloads_identical);
}

}  // namespace tabletennis::native
