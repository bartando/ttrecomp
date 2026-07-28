#include "native/tabletennis_scene_draw_catalog.h"

#include "generated/default/tabletennis_init.h"
#include "native/tabletennis_6ae_player_observer.h"
#include "native/tabletennis_crowd_observer.h"
#include "native/tabletennis_d47_player_observer.h"
#include "native/tabletennis_frame_scene.h"
#include "native/tabletennis_guest_memory.h"
#include "native/tabletennis_net_bb903_observer.h"
#include "native/tabletennis_player_palette_write_observer.h"
#include "native/tabletennis_player_a406_observer.h"
#include "native/tabletennis_player_bbb5_observer.h"
#include "native/tabletennis_player_2ac_observer.h"
#include "native/tabletennis_player_skin_observer.h"
#include "native/tabletennis_scene_owner_observer.h"
#include "native/tabletennis_venue_14d_observer.h"
#include "native/tabletennis_venue_526a_observer.h"
#include "native/tabletennis_venue_9e_observer.h"
#include "native/tabletennis_venue_e33_observer.h"
#include "native/tabletennis_venue_full_family.h"
#include "native/tabletennis_venue_snapshot.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

#ifndef XXH_INLINE_ALL
#define XXH_INLINE_ALL
#endif
#include "third_party/rexglue-sdk/thirdparty/xxHash/xxhash.h"

REXCVAR_DEFINE_UINT32(
    tabletennis_native_scene_catalog_log_interval, 0, "Table Tennis",
    "Guest frames between generic observer-only scene draw catalog reports "
    "(0 disables capture).")
    .range(0, 100000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace tabletennis::native {
namespace {

constexpr size_t kMaxScopeDepth = 16;
constexpr size_t kMaxMeshSelectionDepth = 16;
constexpr size_t kMaxPlayerScopeDepth = 4;
constexpr uint32_t kTextureFetchBankOffset = 0x480;
constexpr uint32_t kVertexConstantBankOffset = 1920;
constexpr uint32_t kPixelConstantBankOffset = 0x1780;
constexpr uint32_t kVertexDeclarationOffset = 0x2C90;
constexpr size_t kConstantRowBytes = sizeof(uint32_t) * 4;
constexpr size_t kWorldFirstRow = 0;
constexpr size_t kWorldViewProjectionFirstRow = 12;
constexpr size_t kMatrixRowCount = 4;
constexpr size_t kMatrixBytes = kMatrixRowCount * kConstantRowBytes;
constexpr uint32_t kActiveVertexStreamSelector = 0x8260634C;
constexpr size_t kGeometryRecordBytes = 0x28;
constexpr uint32_t kMaxVertexStreamSelector = 3;
constexpr uint32_t kMaxLoggedDraws = 48;
constexpr uint32_t kMaxLoggedPlayerDraws = 64;
constexpr uint32_t kMaxLoggedStride40Draws = 96;
constexpr uint32_t kMaximumShaderUcodeBytes = 1024 * 1024;
constexpr uint64_t kFnvOffsetBasis = 14695981039346656037ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;

struct SceneCatalogScope {
  bool active = false;
  SceneCatalogScopeSignature signature{};
  SceneCatalogPassIdentity current_pass{};
};

struct SceneCatalogMeshSelection {
  bool active = false;
  SceneCatalogMeshIdentity identity{};
};

thread_local std::array<SceneCatalogScope, kMaxScopeDepth> g_scope_stack;
thread_local size_t g_scope_depth = 0;
thread_local std::array<SceneCatalogMeshSelection, kMaxMeshSelectionDepth>
    g_mesh_selection_stack;
thread_local size_t g_mesh_selection_depth = 0;
thread_local std::array<uint32_t, kMaxPlayerScopeDepth> g_player_scope_stack;
thread_local size_t g_player_scope_depth = 0;
thread_local SceneCatalogPassIdentity g_current_pass;
thread_local SceneCatalogBoundShaderState g_bound_shaders;

std::mutex g_catalog_mutex;
SceneDrawCatalogFrame g_building_frame;
std::shared_ptr<const SceneDrawCatalogFrame> g_published_frame;
uint64_t g_frame_sequence = 0;
bool g_was_enabled = false;
bool g_building_frame_dirty = false;

bool CatalogEnabled() {
  return REXCVAR_GET(tabletennis_native_scene_catalog_log_interval) != 0;
}

bool CaptureEnabled() {
  return CatalogEnabled() || NativeFrameSceneCaptureEnabled() ||
         VenueFamilyCaptureEnabled() ||
         PlayerPaletteWriteObserverEnabled() ||
         Player2ACObserverEnabled() ||
         PlayerSkinObserverEnabled() || Player6AEObserverEnabled() ||
         D47PlayerObserverEnabled() || PlayerA406ObserverEnabled() ||
         PlayerBBB5ObserverEnabled() ||
         CrowdObserverEnabled() ||
         VenueFullFamilyObserverEnabled() || Venue14DObserverEnabled() ||
         Venue526AObserverEnabled() || Venue9EObserverEnabled() ||
         SceneOwnerObserverEnabled();
}

SceneCatalogScope *CurrentScope() {
  if (g_scope_depth == 0 || g_scope_depth > g_scope_stack.size()) {
    return nullptr;
  }
  return &g_scope_stack[g_scope_depth - 1];
}

const SceneCatalogMeshSelection *CurrentMeshSelection() {
  if (g_mesh_selection_depth == 0 ||
      g_mesh_selection_depth > g_mesh_selection_stack.size()) {
    return nullptr;
  }
  return &g_mesh_selection_stack[g_mesh_selection_depth - 1];
}

uint32_t CurrentPlayer() {
  if (g_player_scope_depth == 0 ||
      g_player_scope_depth > g_player_scope_stack.size()) {
    return 0;
  }
  return g_player_scope_stack[g_player_scope_depth - 1];
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

uint32_t LoadBeU32(const std::byte *source) {
  uint32_t value;
  std::memcpy(&value, source, sizeof(value));
  return std::byteswap(value);
}

uint16_t LoadBeU16(const std::byte *source) {
  uint16_t value;
  std::memcpy(&value, source, sizeof(value));
  return std::byteswap(value);
}

bool TryReadBeU32(uint8_t *guest_base, uint32_t address, size_t offset,
                  uint32_t &value) {
  uint32_t guest_address;
  if (guest_base == nullptr ||
      !CheckedGuestOffset(address, offset, sizeof(uint32_t), guest_address)) {
    return false;
  }
  std::array<std::byte, sizeof(uint32_t)> bytes;
  const void *host_address =
      guest_base + guest_address + REX_PHYS_HOST_OFFSET(guest_address);
  if (!GuestTryCopy(bytes.data(), host_address, bytes.size())) {
    return false;
  }
  value = LoadBeU32(bytes.data());
  return true;
}

bool TryReadBeU16(uint8_t *guest_base, uint32_t address, size_t offset,
                  uint16_t &value) {
  uint32_t guest_address;
  if (guest_base == nullptr ||
      !CheckedGuestOffset(address, offset, sizeof(uint16_t), guest_address)) {
    return false;
  }
  std::array<std::byte, sizeof(uint16_t)> bytes;
  const void *host_address =
      guest_base + guest_address + REX_PHYS_HOST_OFFSET(guest_address);
  if (!GuestTryCopy(bytes.data(), host_address, bytes.size())) {
    return false;
  }
  value = LoadBeU16(bytes.data());
  return true;
}

bool CaptureShaderFingerprint(uint8_t *guest_base, uint32_t shader,
                              size_t ucode_pointer_offset,
                              size_t ucode_size_offset,
                              uint32_t &ucode_bytes, uint64_t &hash) {
  uint32_t ucode = 0;
  if (!TryReadBeU32(guest_base, shader, ucode_pointer_offset, ucode) ||
      !TryReadBeU32(guest_base, shader, ucode_size_offset, ucode_bytes) ||
      ucode == 0 || ucode_bytes == 0 ||
      ucode_bytes > kMaximumShaderUcodeBytes ||
      ucode_bytes % sizeof(uint32_t) != 0) {
    return false;
  }

  uint32_t checked_ucode = 0;
  if (!CheckedGuestOffset(ucode, 0, ucode_bytes, checked_ucode)) {
    return false;
  }
  std::vector<std::byte> payload(ucode_bytes);
  const void *host_address =
      guest_base + checked_ucode + REX_PHYS_HOST_OFFSET(checked_ucode);
  if (!GuestTryCopy(payload.data(), host_address, payload.size())) {
    return false;
  }
  hash = XXH3_64bits(payload.data(), payload.size());
  return hash != 0;
}

template <size_t Size>
bool CaptureBeWords(uint8_t *guest_base, uint32_t address, size_t offset,
                    std::array<uint32_t, Size> &words) {
  uint32_t guest_address;
  constexpr size_t kBytes = Size * sizeof(uint32_t);
  if (guest_base == nullptr ||
      !CheckedGuestOffset(address, offset, kBytes, guest_address)) {
    return false;
  }
  std::array<std::byte, kBytes> bytes;
  const void *host_address =
      guest_base + guest_address + REX_PHYS_HOST_OFFSET(guest_address);
  if (!GuestTryCopy(bytes.data(), host_address, bytes.size())) {
    return false;
  }
  for (size_t index = 0; index < Size; ++index) {
    words[index] =
        LoadBeU32(bytes.data() + index * sizeof(uint32_t));
  }
  return true;
}

template <size_t Size>
bool CaptureBeFloats(uint8_t *guest_base, uint32_t address, size_t offset,
                     std::array<float, Size> &values) {
  std::array<uint32_t, Size> words;
  if (!CaptureBeWords(guest_base, address, offset, words)) {
    return false;
  }
  for (size_t index = 0; index < Size; ++index) {
    values[index] = std::bit_cast<float>(words[index]);
  }
  return true;
}

SceneCatalogDrawState CaptureDrawState(uint8_t *guest_base,
                                       uint32_t device) {
  SceneCatalogDrawState state;
  auto record = [&](bool succeeded) {
    state.guest_read_failures += !succeeded;
  };

  record(TryReadBeU32(guest_base, device, kVertexDeclarationOffset,
                      state.vertex_declaration));
  std::array<uint32_t, 12> texture_fetch_words;
  if (CaptureBeWords(guest_base, device, kTextureFetchBankOffset,
                     texture_fetch_words)) {
    std::copy_n(texture_fetch_words.begin(), 6,
                state.texture_fetches[0].begin());
    std::copy_n(texture_fetch_words.begin() + 6, 6,
                state.texture_fetches[1].begin());
  } else {
    ++state.guest_read_failures;
  }
  record(CaptureBeFloats(guest_base, device,
                         kVertexConstantBankOffset,
                         state.vertex_constants_0_6));
  record(CaptureBeFloats(
      guest_base, device,
      kVertexConstantBankOffset + 12 * kConstantRowBytes,
      state.vertex_constants_12_15));
  record(CaptureBeFloats(
      guest_base, device,
      kPixelConstantBankOffset + 20 * kConstantRowBytes,
      state.pixel_constant_20));
  record(CaptureBeFloats(
      guest_base, device,
      kPixelConstantBankOffset + 46 * kConstantRowBytes,
      state.pixel_constant_46));
  record(CaptureBeFloats(
      guest_base, device,
      kPixelConstantBankOffset + 254 * kConstantRowBytes,
      state.pixel_constant_254));
  record(CaptureBeFloats(
      guest_base, device,
      kPixelConstantBankOffset + 255 * kConstantRowBytes,
      state.pixel_constant_255));
  state.valid =
      state.guest_read_failures == 0 && state.vertex_declaration != 0;
  return state;
}

bool SameScope(const SceneCatalogScopeSignature &left,
               const SceneCatalogScopeSignature &right) {
  return left.shader == right.shader && left.model == right.model &&
         left.geometry_index == right.geometry_index && left.lod == right.lod &&
         left.alternate_pass == right.alternate_pass;
}

bool SamePass(const SceneCatalogPassIdentity &left,
              const SceneCatalogPassIdentity &right) {
  return left.pass_descriptor == right.pass_descriptor &&
         left.program_pair == right.program_pair &&
         left.vertex_shader == right.vertex_shader &&
         left.pixel_shader == right.pixel_shader;
}

bool SameMesh(const SceneCatalogMeshIdentity &left,
              const SceneCatalogMeshIdentity &right) {
  return left.vertex_aggregate == right.vertex_aggregate &&
         left.vertex_declaration == right.vertex_declaration &&
         left.stream_selector == right.stream_selector &&
         left.vertex_buffer_resource == right.vertex_buffer_resource &&
         left.vertex_buffer_alias == right.vertex_buffer_alias &&
         left.vertex_stride == right.vertex_stride &&
         left.index_buffer_resource == right.index_buffer_resource &&
         left.index_buffer_alias == right.index_buffer_alias &&
         left.index_element_size == right.index_element_size &&
         left.index_is_32_bit == right.index_is_32_bit;
}

bool SameDraw(const SceneCatalogDrawSignature &signature,
              const SceneCatalogDrawOccurrence &draw) {
  return signature.owner.kind == draw.owner.kind &&
         signature.owner.role == draw.owner.role &&
         signature.owner.owner == draw.owner.owner &&
         signature.owner.renderable == draw.owner.renderable &&
         signature.player == draw.player &&
         signature.scope_valid == draw.scope_valid &&
         (!draw.scope_valid || SameScope(signature.scope, draw.scope)) &&
         SamePass(signature.pass, draw.pass) &&
         SameMesh(signature.mesh, draw.mesh) &&
         signature.primitive_type == draw.primitive_type &&
         signature.submitted_index_count == draw.submitted_index_count &&
         signature.world_hash == draw.world_hash &&
         signature.world_view_projection_hash ==
             draw.world_view_projection_hash;
}

void StoreScopeLocked(const SceneCatalogScopeSignature &scope) {
  const auto begin = g_building_frame.unique_scopes.begin();
  const auto end = begin + g_building_frame.unique_scope_count;
  const auto existing = std::find_if(begin, end, [&](const auto &candidate) {
    return SameScope(candidate, scope);
  });
  if (existing != end) {
    ++existing->occurrence_count;
    return;
  }
  if (g_building_frame.unique_scope_count ==
      g_building_frame.unique_scopes.size()) {
    ++g_building_frame.dropped_unique_scopes;
    return;
  }
  SceneCatalogScopeSignature stored = scope;
  stored.occurrence_count = 1;
  g_building_frame.unique_scopes[g_building_frame.unique_scope_count++] =
      stored;
}

SceneCatalogPassIdentity ReadPassIdentity(uint8_t *guest_base,
                                          uint32_t runtime_state,
                                          uint32_t pass_descriptor) {
  SceneCatalogPassIdentity pass;
  pass.runtime_state = runtime_state;
  pass.pass_descriptor = pass_descriptor;
  auto read = [&](uint32_t address, size_t offset, uint32_t &value) {
    if (!TryReadBeU32(guest_base, address, offset, value)) {
      ++pass.guest_read_failures;
      return false;
    }
    return true;
  };

  read(pass_descriptor, 0x08, pass.program_pair);
  if (pass.program_pair != 0) {
    read(pass.program_pair, 0x48, pass.vertex_shader_reference);
    read(pass.program_pair, 0x4C, pass.pixel_shader_reference);
  }
  if (pass.vertex_shader_reference != 0) {
    read(pass.vertex_shader_reference, 0, pass.vertex_shader);
  }
  if (pass.pixel_shader_reference != 0) {
    read(pass.pixel_shader_reference, 0, pass.pixel_shader);
  }
  const bool vertex_fingerprint_valid =
      pass.vertex_shader != 0 &&
      CaptureShaderFingerprint(
          guest_base, pass.vertex_shader,
          0x28, 0x258, pass.vertex_shader_ucode_bytes,
          pass.vertex_shader_hash);
  const bool pixel_fingerprint_valid =
      pass.pixel_shader != 0 &&
      CaptureShaderFingerprint(
          guest_base, pass.pixel_shader,
          0x0C, 0x3C, pass.pixel_shader_ucode_bytes,
          pass.pixel_shader_hash);
  pass.shader_fingerprints_valid =
      vertex_fingerprint_valid && pixel_fingerprint_valid;
  pass.guest_read_failures += !vertex_fingerprint_valid;
  pass.guest_read_failures += !pixel_fingerprint_valid;
  pass.valid = pass.guest_read_failures == 0 && pass.pass_descriptor != 0 &&
               pass.program_pair != 0 && pass.vertex_shader != 0 &&
               pass.pixel_shader != 0 && pass.shader_fingerprints_valid;
  return pass;
}

const SceneCatalogPassIdentity *FindPassLocked(uint32_t pass_descriptor) {
  const auto begin = g_building_frame.unique_passes.begin();
  const auto end = begin + g_building_frame.unique_pass_count;
  const auto existing = std::find_if(begin, end, [&](const auto &candidate) {
    return candidate.pass_descriptor == pass_descriptor;
  });
  return existing == end ? nullptr : &*existing;
}

void StorePassLocked(const SceneCatalogPassIdentity &pass) {
  if (FindPassLocked(pass.pass_descriptor) != nullptr) {
    return;
  }
  if (g_building_frame.unique_pass_count ==
      g_building_frame.unique_passes.size()) {
    ++g_building_frame.dropped_unique_passes;
    return;
  }
  g_building_frame.unique_passes[g_building_frame.unique_pass_count++] = pass;
}

SceneCatalogMeshIdentity CaptureMeshIdentity(uint8_t *guest_base,
                                             uint32_t vertex_aggregate,
                                             uint32_t stream_selector,
                                             uint32_t secondary_vertex_stream,
                                             bool alternate_primary_stream,
                                             bool global_stream_selector) {
  SceneCatalogMeshIdentity mesh;
  mesh.vertex_aggregate = vertex_aggregate;
  mesh.stream_selector = stream_selector;
  mesh.secondary_vertex_stream = secondary_vertex_stream;
  mesh.alternate_primary_stream = alternate_primary_stream;
  mesh.global_stream_selector = global_stream_selector;

  auto read32 = [&](uint32_t address, size_t offset, uint32_t &value) {
    if (!TryReadBeU32(guest_base, address, offset, value)) {
      ++mesh.guest_read_failures;
      return false;
    }
    return true;
  };
  auto read16 = [&](uint32_t address, size_t offset, uint16_t &value) {
    if (!TryReadBeU16(guest_base, address, offset, value)) {
      ++mesh.guest_read_failures;
      return false;
    }
    return true;
  };

  const SceneCatalogScope *scope = CurrentScope();
  if (scope != nullptr && scope->active && scope->signature.model != 0) {
    uint32_t geometry_records = 0;
    if (read32(scope->signature.model, 0x04, geometry_records)) {
      const size_t record_offset =
          static_cast<size_t>(scope->signature.geometry_index) *
          kGeometryRecordBytes;
      read32(geometry_records, record_offset + 0x04, mesh.vertex_declaration);
    }
  }

  read16(vertex_aggregate, 0x36, mesh.aggregate_index_count);
  read16(vertex_aggregate, 0x40, mesh.aggregate_primitive_type);
  if (stream_selector > kMaxVertexStreamSelector) {
    ++mesh.guest_read_failures;
    return mesh;
  }

  const size_t stream_offset =
      alternate_primary_stream
          ? 0x30
          : static_cast<size_t>(stream_selector) * sizeof(uint32_t);
  read32(vertex_aggregate, stream_offset, mesh.primary_vertex_stream);
  const size_t index_wrapper_offset =
      static_cast<size_t>(stream_selector + 4) * sizeof(uint32_t);
  read32(vertex_aggregate, index_wrapper_offset, mesh.index_buffer_wrapper);

  if (mesh.primary_vertex_stream != 0) {
    read32(mesh.primary_vertex_stream, 0x10, mesh.vertex_buffer_resource);
    read32(mesh.primary_vertex_stream, 0xD4, mesh.vertex_stride);
  }
  if (mesh.vertex_buffer_resource != 0) {
    std::array<uint32_t, 5> words{};
    for (size_t index = 0; index < words.size(); ++index) {
      read32(mesh.vertex_buffer_resource, index * sizeof(uint32_t),
             words[index]);
    }
    mesh.vertex_fetch_type = static_cast<uint8_t>(words[3] & 0x3u);
    mesh.vertex_buffer_alias = words[3] & ~0x3u;
    mesh.vertex_endian = static_cast<uint8_t>(words[4] & 0x3u);
    mesh.vertex_buffer_bytes = words[4] & 0x03FFFFFCu;
  }

  if (mesh.index_buffer_wrapper != 0) {
    read32(mesh.index_buffer_wrapper, 0x00, mesh.index_element_count);
    read32(mesh.index_buffer_wrapper, 0x04, mesh.index_element_size);
    read32(mesh.index_buffer_wrapper, 0x0C, mesh.index_buffer_resource);
    read32(mesh.index_buffer_wrapper, 0x10, mesh.index_data);
  }
  if (mesh.index_buffer_resource != 0) {
    std::array<uint32_t, 5> words{};
    for (size_t index = 0; index < words.size(); ++index) {
      read32(mesh.index_buffer_resource, index * sizeof(uint32_t),
             words[index]);
    }
    mesh.index_is_32_bit = (words[0] & 0x80000000u) != 0;
    mesh.index_buffer_alias = words[3];
    mesh.index_buffer_bytes = words[4];
  }

  mesh.valid =
      mesh.guest_read_failures == 0 && mesh.vertex_aggregate != 0 &&
      mesh.primary_vertex_stream != 0 && mesh.vertex_buffer_resource != 0 &&
      mesh.vertex_buffer_alias != 0 && mesh.vertex_stride != 0 &&
      mesh.index_buffer_wrapper != 0 && mesh.index_buffer_resource != 0 &&
      mesh.index_buffer_alias != 0 && mesh.index_element_size != 0;
  return mesh;
}

bool CaptureMatrixRows(uint8_t *guest_base, uint32_t device, size_t first_row,
                       std::array<float, 16> &matrix, uint64_t &hash) {
  uint32_t bank_address;
  const size_t byte_offset =
      kVertexConstantBankOffset + first_row * kConstantRowBytes;
  if (guest_base == nullptr ||
      !CheckedGuestOffset(device, byte_offset, kMatrixBytes, bank_address)) {
    return false;
  }

  std::array<std::byte, kMatrixBytes> bytes;
  const void *host_address =
      guest_base + bank_address + REX_PHYS_HOST_OFFSET(bank_address);
  if (!GuestTryCopy(bytes.data(), host_address, bytes.size())) {
    return false;
  }

  hash = kFnvOffsetBasis;
  for (size_t index = 0; index < matrix.size(); ++index) {
    const uint32_t bits = LoadBeU32(bytes.data() + index * sizeof(uint32_t));
    matrix[index] = std::bit_cast<float>(bits);
    hash ^= bits;
    hash *= kFnvPrime;
  }
  return true;
}

void StoreDrawSummaryLocked(const SceneCatalogDrawOccurrence &draw) {
  const auto begin = g_building_frame.unique_draws.begin();
  const auto end = begin + g_building_frame.unique_draw_count;
  const auto existing = std::find_if(begin, end, [&](const auto &candidate) {
    return SameDraw(candidate, draw);
  });
  if (existing != end) {
    ++existing->occurrence_count;
    return;
  }
  if (g_building_frame.unique_draw_count ==
      g_building_frame.unique_draws.size()) {
    ++g_building_frame.dropped_unique_draws;
    return;
  }

  SceneCatalogDrawSignature signature;
  signature.owner = draw.owner;
  signature.player = draw.player;
  signature.scope_valid = draw.scope_valid;
  signature.scope = draw.scope;
  signature.pass = draw.pass;
  signature.mesh = draw.mesh;
  signature.device = draw.device;
  signature.primitive_type = draw.primitive_type;
  signature.submitted_index_count = draw.submitted_index_count;
  signature.first_ordinal = draw.ordinal;
  signature.occurrence_count = 1;
  signature.world_hash = draw.world_hash;
  signature.world_view_projection_hash = draw.world_view_projection_hash;
  g_building_frame.unique_draws[g_building_frame.unique_draw_count++] =
      signature;
}

void LogFrame(const SceneDrawCatalogFrame &frame) {
  REXLOG_INFO("Table Tennis scene catalog: frame={} scopes={} unique_scopes={} "
              "dropped_scopes={} pass_applies={} unique_passes={} "
              "dropped_passes={} draw_hooks={} scoped={} unscoped={} "
              "player_draws={} "
              "ordered={} dropped_ordered={} unique_draws={} dropped_draws={} "
              "read_failures={} observer_only=true",
              frame.sequence, frame.model_geometry_scope_count,
              frame.unique_scope_count, frame.dropped_unique_scopes,
              frame.pass_apply_count, frame.unique_pass_count,
              frame.dropped_unique_passes, frame.total_indexed_hook_count,
              frame.scoped_indexed_draw_count,
              frame.unscoped_indexed_draw_count,
              frame.player_indexed_draw_count, frame.ordered_draw_count,
              frame.dropped_ordered_draws, frame.unique_draw_count,
              frame.dropped_unique_draws, frame.guest_read_failures);

  const uint32_t logged_draw_count =
      std::min(frame.unique_draw_count, kMaxLoggedDraws);
  for (uint32_t index = 0; index < logged_draw_count; ++index) {
    const SceneCatalogDrawSignature &draw = frame.unique_draws[index];
    REXLOG_INFO("  scene_draw[{}] first={} x{} owner_kind={} "
                "owner={:08X} owner_renderable={:08X} player={:08X} scoped={} "
                "shader={:08X} "
                "model={:08X} geom={} lod={} alt={} pass={:08X} "
                "program={:08X} vs={:08X} ps={:08X} primitive={} indices={} "
                "aggregate={:08X} declaration={:08X} vb={:08X}/{:08X} "
                "stride={} ib={:08X}/{:08X} index_size={} index32={} "
                "world={:016X} wvp={:016X}",
                index, draw.first_ordinal, draw.occurrence_count,
                static_cast<uint32_t>(draw.owner.kind), draw.owner.owner,
                draw.owner.renderable, draw.player,
                draw.scope_valid, draw.scope.shader, draw.scope.model,
                draw.scope.geometry_index, draw.scope.lod,
                draw.scope.alternate_pass, draw.pass.pass_descriptor,
                draw.pass.program_pair, draw.pass.vertex_shader,
                draw.pass.pixel_shader, draw.primitive_type,
                draw.submitted_index_count, draw.mesh.vertex_aggregate,
                draw.mesh.vertex_declaration, draw.mesh.vertex_buffer_resource,
                draw.mesh.vertex_buffer_alias, draw.mesh.vertex_stride,
                draw.mesh.index_buffer_resource, draw.mesh.index_buffer_alias,
                draw.mesh.index_element_size, draw.mesh.index_is_32_bit,
                draw.world_hash, draw.world_view_projection_hash);
  }
  if (logged_draw_count != frame.unique_draw_count) {
    REXLOG_INFO("  omitted {} additional unique draw signatures",
                frame.unique_draw_count - logged_draw_count);
  }

  uint32_t logged_player_draws = 0;
  uint32_t logged_stride40_draws = 0;
  for (uint32_t index = 0; index < frame.ordered_draw_count; ++index) {
    const SceneCatalogDrawOccurrence &draw = frame.ordered_draws[index];
    if (draw.player != 0 &&
        logged_player_draws++ < kMaxLoggedPlayerDraws) {
      REXLOG_INFO(
          "  scene_player_draw ordinal={} player={:08X} shader={:08X} "
          "model={:08X} geom={} pass={:08X} program={:08X} vs={:08X} "
          "ps={:08X} indices={} aggregate={:08X} declaration={:08X} "
          "vb={:08X}/{:08X} stride={} ib={:08X}/{:08X} world={:016X} "
          "wvp={:016X}",
          draw.ordinal, draw.player, draw.scope.shader, draw.scope.model,
          draw.scope.geometry_index, draw.pass.pass_descriptor,
          draw.pass.program_pair, draw.pass.vertex_shader,
          draw.pass.pixel_shader, draw.submitted_index_count,
          draw.mesh.vertex_aggregate, draw.mesh.vertex_declaration,
          draw.mesh.vertex_buffer_resource, draw.mesh.vertex_buffer_alias,
          draw.mesh.vertex_stride, draw.mesh.index_buffer_resource,
          draw.mesh.index_buffer_alias, draw.world_hash,
          draw.world_view_projection_hash);
    }
    if (draw.player == 0 && draw.mesh.vertex_stride == 40 &&
        logged_stride40_draws++ < kMaxLoggedStride40Draws) {
      REXLOG_INFO(
          "  scene_stride40_draw ordinal={} scoped={} shader={:08X} "
          "model={:08X} geom={} pass={:08X} program={:08X} vs={:08X} "
          "ps={:08X} indices={} aggregate={:08X} declaration={:08X} "
          "vb={:08X}/{:08X} ib={:08X}/{:08X} world={:016X} wvp={:016X}",
          draw.ordinal, draw.scope_valid, draw.scope.shader, draw.scope.model,
          draw.scope.geometry_index, draw.pass.pass_descriptor,
          draw.pass.program_pair, draw.pass.vertex_shader,
          draw.pass.pixel_shader, draw.submitted_index_count,
          draw.mesh.vertex_aggregate, draw.mesh.vertex_declaration,
          draw.mesh.vertex_buffer_resource, draw.mesh.vertex_buffer_alias,
          draw.mesh.index_buffer_resource, draw.mesh.index_buffer_alias,
          draw.world_hash, draw.world_view_projection_hash);
    }
  }
}

} // namespace

void BeginSceneDrawCatalogScope(uint32_t shader, uint32_t model,
                                uint32_t geometry_index, uint32_t lod,
                                bool alternate_pass) {
  SceneCatalogScope scope;
  scope.active = CaptureEnabled();
  scope.signature.shader = shader;
  scope.signature.model = model;
  scope.signature.geometry_index = geometry_index;
  scope.signature.lod = lod;
  scope.signature.alternate_pass = alternate_pass;

  if (g_scope_depth < g_scope_stack.size()) {
    g_scope_stack[g_scope_depth] = scope;
  }
  ++g_scope_depth;

  if (!scope.active) {
    return;
  }
  std::lock_guard lock(g_catalog_mutex);
  g_building_frame_dirty = true;
  ++g_building_frame.model_geometry_scope_count;
  StoreScopeLocked(scope.signature);
}

void EndSceneDrawCatalogScope() {
  if (g_scope_depth != 0) {
    --g_scope_depth;
  }
}

void BeginSceneDrawCatalogPlayerScope(uint32_t player) {
  if (g_player_scope_depth < g_player_scope_stack.size()) {
    if (CaptureEnabled() && player == 0) {
      player = CurrentPlayer();
    }
    g_player_scope_stack[g_player_scope_depth] =
        CaptureEnabled() ? player : 0;
  }
  ++g_player_scope_depth;
}

void EndSceneDrawCatalogPlayerScope() {
  if (g_player_scope_depth != 0) {
    --g_player_scope_depth;
  }
}

void ObserveSceneDrawCatalogPass(uint8_t *guest_base, uint32_t runtime_state,
                                 uint32_t pass_descriptor) {
  if (!CaptureEnabled()) {
    return;
  }
  SceneCatalogScope *scope = CurrentScope();
  const bool scope_active = scope != nullptr && scope->active;

  {
    std::lock_guard lock(g_catalog_mutex);
    g_building_frame_dirty = true;
    ++g_building_frame.pass_apply_count;
    if (const SceneCatalogPassIdentity *existing =
            FindPassLocked(pass_descriptor)) {
      g_current_pass = *existing;
      if (scope_active) {
        scope->current_pass = *existing;
      }
      return;
    }
  }

  const SceneCatalogPassIdentity pass =
      ReadPassIdentity(guest_base, runtime_state, pass_descriptor);
  {
    std::lock_guard lock(g_catalog_mutex);
    g_building_frame_dirty = true;
    g_building_frame.guest_read_failures += pass.guest_read_failures;
    if (const SceneCatalogPassIdentity *existing =
            FindPassLocked(pass_descriptor)) {
      g_current_pass = *existing;
      if (scope_active) {
        scope->current_pass = *existing;
      }
    } else {
      StorePassLocked(pass);
      g_current_pass = pass;
      if (scope_active) {
        scope->current_pass = pass;
      }
    }
  }
}

void ObserveSceneDrawCatalogBoundVertexShader(uint8_t *guest_base,
                                              uint32_t device,
                                              uint32_t shader) {
  if (!CaptureEnabled()) {
    return;
  }
  g_bound_shaders.device = device;
  g_bound_shaders.vertex_shader = shader;
  g_bound_shaders.vertex_shader_ucode_bytes = 0;
  g_bound_shaders.vertex_shader_hash = 0;
  g_bound_shaders.vertex_shader_valid =
      shader != 0 &&
      CaptureShaderFingerprint(
          guest_base, shader, 0x28, 0x258,
          g_bound_shaders.vertex_shader_ucode_bytes,
          g_bound_shaders.vertex_shader_hash);
}

void ObserveSceneDrawCatalogBoundPixelShader(uint8_t *guest_base,
                                             uint32_t device,
                                             uint32_t shader) {
  if (!CaptureEnabled()) {
    return;
  }
  g_bound_shaders.device = device;
  g_bound_shaders.pixel_shader = shader;
  g_bound_shaders.pixel_shader_ucode_bytes = 0;
  g_bound_shaders.pixel_shader_hash = 0;
  g_bound_shaders.pixel_shader_valid =
      shader != 0 &&
      CaptureShaderFingerprint(
          guest_base, shader, 0x0C, 0x3C,
          g_bound_shaders.pixel_shader_ucode_bytes,
          g_bound_shaders.pixel_shader_hash);
}

void BeginSceneDrawCatalogMeshSelection(uint8_t *guest_base,
                                        uint32_t vertex_aggregate,
                                        uint32_t stream_selector,
                                        uint32_t secondary_vertex_stream,
                                        bool alternate_primary_stream) {
  SceneCatalogMeshSelection selection;
  selection.active = CaptureEnabled();
  if (selection.active) {
    selection.identity = CaptureMeshIdentity(
        guest_base, vertex_aggregate, stream_selector, secondary_vertex_stream,
        alternate_primary_stream, false);
  }
  if (g_mesh_selection_depth < g_mesh_selection_stack.size()) {
    g_mesh_selection_stack[g_mesh_selection_depth] = selection;
  }
  ++g_mesh_selection_depth;

  if (selection.active && selection.identity.guest_read_failures != 0) {
    std::lock_guard lock(g_catalog_mutex);
    g_building_frame_dirty = true;
    g_building_frame.guest_read_failures +=
        selection.identity.guest_read_failures;
  }
}

void BeginSceneDrawCatalogGlobalMeshSelection(uint8_t *guest_base,
                                              uint32_t vertex_aggregate) {
  SceneCatalogMeshSelection selection;
  selection.active = CaptureEnabled();
  if (selection.active) {
    uint32_t stream_selector = 0;
    if (TryReadBeU32(guest_base, kActiveVertexStreamSelector, 0,
                     stream_selector)) {
      selection.identity = CaptureMeshIdentity(guest_base, vertex_aggregate,
                                               stream_selector, 0, true, true);
    } else {
      selection.identity.vertex_aggregate = vertex_aggregate;
      selection.identity.alternate_primary_stream = true;
      selection.identity.global_stream_selector = true;
      ++selection.identity.guest_read_failures;
    }
  }
  if (g_mesh_selection_depth < g_mesh_selection_stack.size()) {
    g_mesh_selection_stack[g_mesh_selection_depth] = selection;
  }
  ++g_mesh_selection_depth;

  if (selection.active && selection.identity.guest_read_failures != 0) {
    std::lock_guard lock(g_catalog_mutex);
    g_building_frame_dirty = true;
    g_building_frame.guest_read_failures +=
        selection.identity.guest_read_failures;
  }
}

void EndSceneDrawCatalogMeshSelection() {
  if (g_mesh_selection_depth != 0) {
    --g_mesh_selection_depth;
  }
}

void ObserveSceneDrawCatalogIndexedDraw(uint8_t *guest_base, uint32_t device,
                                        uint32_t primitive_type,
                                        uint32_t submitted_index_count) {
  if (!CaptureEnabled()) {
    return;
  }

  const SceneCatalogScope *scope = CurrentScope();
  const bool scope_valid = scope != nullptr && scope->active;
  const SceneCatalogMeshSelection *mesh = CurrentMeshSelection();

  SceneCatalogDrawOccurrence draw;
  {
    std::lock_guard lock(g_catalog_mutex);
    g_building_frame_dirty = true;
    SceneDrawCatalogFrame &frame = g_building_frame;
    const uint32_t ordinal = ++frame.total_indexed_hook_count;
    if (scope_valid) {
      ++frame.scoped_indexed_draw_count;
    } else {
      ++frame.unscoped_indexed_draw_count;
    }
    const uint32_t player = CurrentPlayer();
    frame.player_indexed_draw_count += player != 0;
    if (frame.ordered_draw_count == frame.ordered_draws.size()) {
      ++frame.dropped_ordered_draws;
      return;
    }

    draw.frame_sequence = g_frame_sequence + 1;
    draw.ordinal = ordinal;
    draw.owner = CurrentSceneOwnerToken();
    draw.player = player;
    draw.scope_valid = scope_valid;
    if (scope_valid) {
      draw.scope = scope->signature;
      draw.pass = scope->current_pass;
    } else {
      draw.pass = g_current_pass;
    }
    if (mesh != nullptr && mesh->active) {
      draw.mesh = mesh->identity;
    }
    draw.device = device;
    if (g_bound_shaders.device == device) {
      draw.bound_shaders = g_bound_shaders;
    }
    draw.primitive_type = primitive_type;
    draw.submitted_index_count = submitted_index_count;
    draw.state = CaptureDrawState(guest_base, device);
    if (draw.state.vertex_declaration != 0) {
      draw.mesh.vertex_declaration =
          draw.state.vertex_declaration;
    }
    draw.world_valid = CaptureMatrixRows(guest_base, device, kWorldFirstRow,
                                         draw.world, draw.world_hash);
    draw.world_view_projection_valid = CaptureMatrixRows(
        guest_base, device, kWorldViewProjectionFirstRow,
        draw.world_view_projection, draw.world_view_projection_hash);
    frame.guest_read_failures += !draw.world_valid;
    frame.guest_read_failures += !draw.world_view_projection_valid;
    frame.guest_read_failures += draw.state.guest_read_failures;
    frame.ordered_draws[frame.ordered_draw_count++] = draw;
    StoreDrawSummaryLocked(draw);
  }

  // Guest buffers are copied outside the catalog mutex. The venue observer
  // has its own cache and never blocks unrelated catalog readers while a
  // streaming range is being fault-guarded.
  ObservePlayerPaletteWriteCatalogDraw(guest_base, draw);
  ObservePlayer2ACTitleDraw(guest_base, draw);
  ObserveSceneOwnerCatalogDraw(draw.owner);
  ObserveNetBB903TitleDraw(guest_base, draw);
  ObservePlayer6AECatalogDraw(guest_base, draw);
  ObserveD47PlayerCatalogDraw(guest_base, draw);
  ObservePlayerA406TitleDraw(guest_base, draw);
  ObservePlayerBBB5TitleDraw(guest_base, draw);
  ObserveVenue14DTitleDraw(guest_base, draw);
  ObserveVenue526ATitleDraw(guest_base, draw);
  ObserveVenue9ETitleDraw(guest_base, draw);
  ObserveVenueE33TitleDraw(guest_base, draw);
  ObserveVenueFamilyDraw(guest_base, draw);
  ObservePlayerSkinDraw(guest_base, draw);
  ObserveCrowdDraw(guest_base, draw);
}

void SceneDrawCatalogFrameEnd() {
  const uint32_t log_interval =
      REXCVAR_GET(tabletennis_native_scene_catalog_log_interval);
  const bool log_enabled = log_interval != 0;
  const bool publish_enabled =
      log_enabled || NativeFrameSceneCaptureEnabled();

  std::lock_guard lock(g_catalog_mutex);
  ++g_frame_sequence;
  if (!publish_enabled) {
    if (g_building_frame_dirty) {
      g_building_frame = {};
      g_building_frame_dirty = false;
    }
    if (g_was_enabled) {
      g_published_frame.reset();
    }
    g_was_enabled = false;
    return;
  }

  g_building_frame.sequence = g_frame_sequence;
  g_published_frame = std::make_shared<const SceneDrawCatalogFrame>(
      std::move(g_building_frame));
  g_building_frame = {};
  g_building_frame_dirty = false;
  g_was_enabled = true;
  if (log_enabled && g_published_frame->sequence % log_interval == 0) {
    LogFrame(*g_published_frame);
  }
}

SceneDrawCatalogFrame LatestSceneDrawCatalogFrame() {
  std::lock_guard lock(g_catalog_mutex);
  return g_published_frame == nullptr ? SceneDrawCatalogFrame{}
                                      : *g_published_frame;
}

std::shared_ptr<const SceneDrawCatalogFrame>
LatestSceneDrawCatalogFrameSnapshot() {
  std::lock_guard lock(g_catalog_mutex);
  return g_published_frame;
}

} // namespace tabletennis::native
