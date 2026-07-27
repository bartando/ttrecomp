#include "native/tabletennis_crowd_observer.h"

#include "generated/default/tabletennis_init.h"
#include "native/tabletennis_crowd_replacement_prewarm.h"
#include "native/tabletennis_frame_scene.h"
#include "native/tabletennis_guest_memory.h"
#include "native/tabletennis_main_coverage_ledger.h"
#include "native/tabletennis_scene_draw_catalog.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstring>
#include <limits>
#include <mutex>
#include <utility>

#include <rex/cvar.h>
#include <rex/graphics/native_guest_renderer.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(
    tabletennis_native_crowd_observer, false, "Table Tennis",
    "Capture the fxCrowdGfx-owned 36-byte vertex, 28-byte palette and layered "
    "DXT1 material contract. Observer-only; never suppresses title draws.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace tabletennis::native {
namespace {

constexpr uint32_t kCrowdVtable = 0x8206AF9C;
constexpr uint32_t kCrowdVertexStride = 36;
constexpr uint32_t kTriangleStripPrimitive = 0x06;
constexpr size_t kMaximumCrowdScopeDepth = 4;
constexpr size_t kMaximumFrameDraws = 512;
constexpr size_t kMaximumCapturedTitleFrames = 8;
constexpr size_t kMaximumPendingBackendBlocks = 16;
constexpr size_t kMaximumTileRenderPasses = 8;
constexpr size_t kMaximumUniqueRenderPassContracts = 16;
constexpr uint64_t kCrowdVertexShaderHash = 0xBD4B1DF972B828B7ull;
constexpr uint64_t kCrowdPixelShaderHash = 0xC6CEFDA3753CF2BAull;
constexpr uint64_t kFnvOffsetBasis = 1469598103934665603ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;

// One logical frame from trace 545407DF_5661. The GPU repeats it for three
// EDRAM tiles, but the title submits these 156 engine draws exactly once.
constexpr std::array<std::pair<uint32_t, uint32_t>, 22> kTraceIndexHistogram = {
    {
        {553, 6}, {561, 12}, {562, 5},  {609, 5},  {634, 7}, {649, 6},
        {652, 9}, {659, 7},  {672, 5},  {690, 8},  {693, 8}, {739, 6},
        {759, 8}, {772, 12}, {793, 8},  {823, 9},  {839, 6}, {872, 7},
        {893, 5}, {919, 6},  {1003, 5}, {1004, 6},
    }};

thread_local std::array<CrowdOwnerSnapshot, kMaximumCrowdScopeDepth>
    g_crowd_render_scope_stack;
thread_local size_t g_crowd_render_scope_depth = 0;
thread_local std::array<CrowdOwnerSnapshot, kMaximumCrowdScopeDepth>
    g_crowd_submit_scope_stack;
thread_local size_t g_crowd_submit_scope_depth = 0;

struct CrowdBackendDrawIdentity {
  uint32_t primitive_type = 0;
  uint32_t index_count = 0;
  uint32_t index_physical_address = 0;

  bool operator==(const CrowdBackendDrawIdentity &) const = default;
};

// Every field comes from an immutable, fully validated host-side snapshot.
// The backend cannot see this payload identity directly; retaining it beside
// the exact index binding proves which title object and bytes a successful
// correlation belongs to without retaining revocable guest pointers.
struct CrowdImmutablePayloadIdentity {
  uint32_t vertex_physical_address = 0;
  uint32_t vertex_size = 0;
  uint64_t vertex_fingerprint = 0;
  uint32_t index_physical_address = 0;
  uint32_t index_count = 0;
  uint64_t index_fingerprint = 0;
  uint32_t palette_physical_address = 0;
  uint32_t palette_size = 0;
  uint64_t palette_fingerprint = 0;
  uint32_t texture_physical_address = 0;
  uint32_t texture_size = 0;
  uint64_t texture_fingerprint = 0;
  uint64_t material_fingerprint = 0;

  bool valid() const {
    return vertex_physical_address != 0 && vertex_size != 0 &&
           index_physical_address != 0 && index_count != 0 &&
           palette_physical_address != 0 && palette_size != 0 &&
           texture_physical_address != 0 && texture_size != 0;
  }
};

struct CrowdTitleDrawIdentity {
  uint32_t owner = 0;
  uint32_t drawable = 0;
  uint32_t model = 0;
  CrowdBackendDrawIdentity draw{};
  CrowdImmutablePayloadIdentity payload{};
};

struct CrowdCapturedTitleFrame {
  uint64_t generation = 0;
  uint64_t sequence_fingerprint = 0;
  uint64_t payload_fingerprint = 0;
  uint32_t draw_count = 0;
  uint32_t matched_block_count = 0;
  std::array<uint32_t, kMaximumTileRenderPasses> render_pass_keys{};
  uint32_t render_pass_key_count = 0;
  std::array<CrowdTitleDrawIdentity, kMaximumFrameDraws> draws{};
  std::shared_ptr<const CrowdFrameSnapshot> snapshot;
  bool valid = false;
};

struct CrowdBackendBlockContract {
  uint32_t backend = 0;
  uint32_t render_pass_key = 0;
  uint32_t surface_pitch = 0;
  uint32_t normalized_depth_control = 0;
  uint32_t normalized_color_mask = 0;
  uint32_t color_control = 0;
  uint32_t blend_control_0 = 0;
  uint32_t rasterizer_mode_control = 0;
  uint32_t primitive_restart_index = 0;
  std::array<uint32_t,
             rex::graphics::NativeGuestDrawContext::kMaxColorAttachments>
      color_attachment_formats{};
  uint32_t color_attachment_count = 0;
  uint32_t depth_attachment_format = 0;
  uint32_t stencil_attachment_format = 0;
  uint32_t sample_count = 0;
  uint64_t sample_mask = 0;
  bool primitive_restart_enabled = false;
  bool rasterizer_mode_control_valid = false;
  bool draw_state_contract_valid = false;
  bool attachment_contract_valid = false;

  bool operator==(const CrowdBackendBlockContract &) const = default;
};

struct CrowdBackendBlock {
  uint64_t sequence = 0;
  uint64_t sequence_fingerprint = 0;
  uint32_t draw_count = 0;
  uint32_t overflow_draw_count = 0;
  CrowdBackendBlockContract contract{};
  std::array<CrowdBackendDrawIdentity, kMaximumFrameDraws> draws{};
  uint64_t candidate_title_generation = 0;
  bool candidate_prefix_matches = false;
  bool contract_uniform = true;
  bool valid = false;
};

struct CrowdUniqueRenderPassContract {
  CrowdBackendBlockContract contract{};
  bool valid = false;
};

std::mutex g_observer_mutex;
CrowdFrameSnapshot g_building_frame;
std::shared_ptr<const CrowdFrameSnapshot> g_published_frame;
uint64_t g_frame_sequence = 0;
uint64_t g_backend_block_sequence = 0;
uint64_t g_active_title_generation = 0;
CrowdBackendBlock g_active_backend_block;
CrowdBackendBlock g_last_matched_backend_block;
uint32_t g_backend_replay_run_ordinal = 0;
std::array<CrowdCapturedTitleFrame, kMaximumCapturedTitleFrames>
    g_captured_title_frames{};
std::array<CrowdBackendBlock, kMaximumPendingBackendBlocks>
    g_pending_backend_blocks{};
std::array<CrowdUniqueRenderPassContract, kMaximumUniqueRenderPassContracts>
    g_unique_render_pass_contracts{};
std::shared_ptr<const CrowdReplacementCandidate>
    g_pending_replacement_candidate;
bool g_announced_verified = false;
bool g_announced_rejection = false;
bool g_announced_backend_block_proof = false;
bool g_announced_repeated_tile_block = false;
uint64_t g_last_backend_summary_sequence = 0;

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
  uint32_t value = 0;
  std::memcpy(&value, source, sizeof(value));
  return std::byteswap(value);
}

bool TryReadBeU32(uint8_t *guest_base, uint32_t address, size_t offset,
                  uint32_t &value) {
  uint32_t guest_address = 0;
  std::array<std::byte, sizeof(uint32_t)> bytes;
  if (guest_base == nullptr ||
      !CheckedGuestOffset(address, offset, bytes.size(), guest_address) ||
      !GuestTryCopy(bytes.data(),
                    guest_base + guest_address +
                        REX_PHYS_HOST_OFFSET(guest_address),
                    bytes.size())) {
    return false;
  }
  value = LoadBeU32(bytes.data());
  return true;
}

CrowdOwnerSnapshot CaptureOwner(uint8_t *guest_base, uint32_t crowd) {
  CrowdOwnerSnapshot owner;
  owner.crowd = crowd;
  auto read = [&](size_t offset, uint32_t &value) {
    if (!TryReadBeU32(guest_base, crowd, offset, value)) {
      ++owner.guest_read_failures;
    }
  };
  read(0x00, owner.vtable);
  read(0x20, owner.crowd_resource);
  read(0x24, owner.crowd_state);
  read(0x28, owner.drawables[0]);
  read(0x2C, owner.drawables[1]);
  read(0x30, owner.models[0]);
  read(0x34, owner.models[1]);
  read(0x438, owner.visible_instance_count);
  owner.valid =
      owner.guest_read_failures == 0 && owner.vtable == kCrowdVtable &&
      owner.visible_instance_count != 0 &&
      owner.visible_instance_count <= 256 && owner.crowd_resource != 0 &&
      owner.drawables[0] != 0 && owner.models[0] != 0;
  return owner;
}

const CrowdOwnerSnapshot *CurrentCrowdRenderOwner() {
  if (g_crowd_render_scope_depth == 0 ||
      g_crowd_render_scope_depth > g_crowd_render_scope_stack.size()) {
    return nullptr;
  }
  const CrowdOwnerSnapshot &owner =
      g_crowd_render_scope_stack[g_crowd_render_scope_depth - 1];
  return owner.valid ? &owner : nullptr;
}

const CrowdOwnerSnapshot *CurrentCrowdSubmitOwner() {
  if (g_crowd_submit_scope_depth == 0 ||
      g_crowd_submit_scope_depth > g_crowd_submit_scope_stack.size()) {
    return nullptr;
  }
  const CrowdOwnerSnapshot &owner =
      g_crowd_submit_scope_stack[g_crowd_submit_scope_depth - 1];
  return owner.valid ? &owner : nullptr;
}

bool MatchesOwnedSubmitPair(const CrowdOwnerSnapshot &owner, uint32_t drawable,
                            uint32_t model) {
  for (size_t index = 0; index < owner.drawables.size(); ++index) {
    if (owner.drawables[index] == drawable && owner.models[index] == model &&
        drawable != 0 && model != 0) {
      return true;
    }
  }
  return false;
}

bool IsCrowdCandidate(const SceneCatalogDrawOccurrence &draw) {
  return draw.mesh.valid && draw.mesh.vertex_stride == kCrowdVertexStride &&
         draw.submitted_index_count != 0;
}

void AppendFingerprintWord(uint64_t &fingerprint, uint32_t word) {
  for (uint32_t shift = 0; shift < 32; shift += 8) {
    fingerprint = (fingerprint ^ ((word >> shift) & 0xFFu)) * kFnvPrime;
  }
}

template <size_t Size>
void AppendFingerprintFloats(uint64_t &fingerprint,
                             const std::array<float, Size> &values) {
  for (float value : values) {
    AppendFingerprintWord(fingerprint, std::bit_cast<uint32_t>(value));
  }
}

uint64_t MaterialFingerprint(const CrowdMaterialSnapshot &material) {
  uint64_t fingerprint = kFnvOffsetBasis;
  const VertexDeclarationProbe &declaration = material.declaration;
  AppendFingerprintWord(fingerprint, declaration.guest_address);
  AppendFingerprintWord(fingerprint, declaration.element_count);
  AppendFingerprintWord(fingerprint, declaration.max_stream);
  AppendFingerprintWord(fingerprint, declaration.cache_id);
  const size_t element_count =
      std::min<size_t>(declaration.element_count, declaration.elements.size());
  for (size_t index = 0; index < element_count; ++index) {
    const VertexDeclarationElement &element = declaration.elements[index];
    AppendFingerprintWord(
        fingerprint, static_cast<uint32_t>(element.stream) |
                         (static_cast<uint32_t>(element.byte_offset) << 16));
    AppendFingerprintWord(fingerprint, element.packed_type);
    AppendFingerprintWord(
        fingerprint, static_cast<uint32_t>(element.method) |
                         (static_cast<uint32_t>(element.usage) << 8) |
                         (static_cast<uint32_t>(element.usage_index) << 16));
  }
  for (uint32_t word : material.texture_fetch) {
    AppendFingerprintWord(fingerprint, word);
  }
  AppendFingerprintFloats(fingerprint, material.instance_transform);
  AppendFingerprintFloats(fingerprint, material.view_projection);
  AppendFingerprintFloats(fingerprint, material.lighting_spheres);
  AppendFingerprintFloats(fingerprint, material.ambient);
  AppendFingerprintFloats(fingerprint, material.decode_constants);
  return fingerprint;
}

CrowdImmutablePayloadIdentity
PayloadIdentityForSnapshot(const CrowdDrawSnapshot &snapshot) {
  CrowdImmutablePayloadIdentity identity;
  if (!snapshot.valid || snapshot.vertices == nullptr ||
      snapshot.indices == nullptr || snapshot.palette == nullptr ||
      snapshot.material.texture == nullptr) {
    return identity;
  }
  identity.vertex_physical_address = snapshot.vertices->fetch.physical_address;
  identity.vertex_size = snapshot.vertices->fetch.size;
  identity.vertex_fingerprint = snapshot.vertices->payload_fingerprint;
  identity.index_physical_address = snapshot.indices->physical_address;
  identity.index_count = snapshot.indices->submitted_index_count;
  identity.index_fingerprint = snapshot.indices->payload_fingerprint;
  identity.palette_physical_address = snapshot.palette->fetch.physical_address;
  identity.palette_size = snapshot.palette->fetch.size;
  identity.palette_fingerprint = snapshot.palette->payload_fingerprint;
  identity.texture_physical_address =
      snapshot.material.texture->physical_address;
  identity.texture_size = snapshot.material.texture->byte_size;
  identity.texture_fingerprint = snapshot.material.texture->payload_fingerprint;
  identity.material_fingerprint = MaterialFingerprint(snapshot.material);
  return identity;
}

bool IsExactCrowdBackendDraw(
    const rex::graphics::NativeGuestDrawContext &context) {
  return context.render_pass_key_valid && context.indexed &&
         context.guest_index_base_valid &&
         context.vertex_shader_hash == kCrowdVertexShaderHash &&
         context.pixel_shader_hash == kCrowdPixelShaderHash &&
         context.primitive_type == kTriangleStripPrimitive &&
         context.guest_vertex_or_index_count != 0 &&
         context.guest_index_base != 0;
}

CrowdBackendDrawIdentity
BackendIdentity(const rex::graphics::NativeGuestDrawContext &context) {
  return {
      .primitive_type = context.primitive_type,
      .index_count = context.guest_vertex_or_index_count,
      .index_physical_address = context.guest_index_base,
  };
}

template <typename Entry, size_t Size>
Entry *FindFreeSlot(std::array<Entry, Size> &entries) {
  const auto found = std::ranges::find_if(
      entries, [](const Entry &entry) { return !entry.valid; });
  return found == entries.end() ? nullptr : &*found;
}

void AppendFingerprintU64(uint64_t &fingerprint, uint64_t value) {
  AppendFingerprintWord(fingerprint, static_cast<uint32_t>(value));
  AppendFingerprintWord(fingerprint, static_cast<uint32_t>(value >> 32));
}

void AppendDrawFingerprint(uint64_t &fingerprint,
                           const CrowdBackendDrawIdentity &draw) {
  AppendFingerprintWord(fingerprint, draw.primitive_type);
  AppendFingerprintWord(fingerprint, draw.index_count);
  AppendFingerprintWord(fingerprint, draw.index_physical_address);
}

CrowdBackendBlockContract
CaptureBackendContract(const rex::graphics::NativeGuestDrawContext &context) {
  CrowdBackendBlockContract contract;
  contract.backend = static_cast<uint32_t>(context.backend);
  contract.render_pass_key = context.render_pass_key;
  contract.surface_pitch = context.surface_pitch;
  contract.normalized_depth_control = context.normalized_depth_control;
  contract.normalized_color_mask = context.normalized_color_mask;
  contract.color_control = context.color_control;
  contract.blend_control_0 = context.blend_control_0;
  contract.rasterizer_mode_control = context.rasterizer_mode_control;
  contract.primitive_restart_index = context.primitive_restart_index;
  for (size_t index = 0; index < contract.color_attachment_formats.size();
       ++index) {
    contract.color_attachment_formats[index] =
        static_cast<uint32_t>(context.color_attachment_formats[index]);
  }
  contract.color_attachment_count = context.color_attachment_count;
  contract.depth_attachment_format =
      static_cast<uint32_t>(context.depth_attachment_format);
  contract.stencil_attachment_format =
      static_cast<uint32_t>(context.stencil_attachment_format);
  contract.sample_count = context.sample_count;
  contract.sample_mask = context.sample_mask;
  contract.primitive_restart_enabled = context.primitive_restart_enabled;
  contract.rasterizer_mode_control_valid =
      context.rasterizer_mode_control_valid;
  contract.draw_state_contract_valid = context.draw_state_contract_valid;
  contract.attachment_contract_valid =
      context.borrowed_attachment_contract_valid;
  return contract;
}

CrowdBackendBlockContractTelemetry
ContractTelemetry(const CrowdBackendBlock &block) {
  CrowdBackendBlockContractTelemetry telemetry;
  telemetry.backend = block.contract.backend;
  telemetry.render_pass_key = block.contract.render_pass_key;
  telemetry.surface_pitch = block.contract.surface_pitch;
  telemetry.normalized_depth_control = block.contract.normalized_depth_control;
  telemetry.normalized_color_mask = block.contract.normalized_color_mask;
  telemetry.color_control = block.contract.color_control;
  telemetry.blend_control_0 = block.contract.blend_control_0;
  telemetry.rasterizer_mode_control =
      block.contract.rasterizer_mode_control;
  telemetry.primitive_restart_index = block.contract.primitive_restart_index;
  telemetry.color_attachment_formats = block.contract.color_attachment_formats;
  telemetry.color_attachment_count = block.contract.color_attachment_count;
  telemetry.depth_attachment_format = block.contract.depth_attachment_format;
  telemetry.stencil_attachment_format =
      block.contract.stencil_attachment_format;
  telemetry.sample_count = block.contract.sample_count;
  telemetry.sample_mask = block.contract.sample_mask;
  telemetry.primitive_restart_enabled =
      block.contract.primitive_restart_enabled;
  telemetry.rasterizer_mode_control_valid =
      block.contract.rasterizer_mode_control_valid;
  telemetry.draw_state_contract_valid =
      block.contract.draw_state_contract_valid;
  telemetry.attachment_contract_valid =
      block.contract.attachment_contract_valid;
  telemetry.block_uniform = block.contract_uniform;
  return telemetry;
}

bool SameBackendSequence(const CrowdBackendBlock &block,
                         const CrowdCapturedTitleFrame &frame) {
  if (!block.valid || !frame.valid || block.overflow_draw_count != 0 ||
      block.draw_count != frame.draw_count ||
      block.sequence_fingerprint != frame.sequence_fingerprint) {
    return false;
  }
  for (uint32_t index = 0; index < block.draw_count; ++index) {
    if (block.draws[index] != frame.draws[index].draw) {
      return false;
    }
  }
  return true;
}

bool SameBackendSequence(const CrowdBackendBlock &left,
                         const CrowdBackendBlock &right) {
  if (!left.valid || !right.valid || left.overflow_draw_count != 0 ||
      right.overflow_draw_count != 0 || left.draw_count != right.draw_count ||
      left.sequence_fingerprint != right.sequence_fingerprint) {
    return false;
  }
  return std::equal(left.draws.begin(), left.draws.begin() + left.draw_count,
                    right.draws.begin());
}

bool HasRenderPassKey(const CrowdCapturedTitleFrame &frame,
                      uint32_t render_pass_key) {
  return std::ranges::find(frame.render_pass_keys.begin(),
                           frame.render_pass_keys.begin() +
                               frame.render_pass_key_count,
                           render_pass_key) !=
         frame.render_pass_keys.begin() + frame.render_pass_key_count;
}

bool AddRenderPassKey(CrowdCapturedTitleFrame &frame,
                      uint32_t render_pass_key) {
  if (HasRenderPassKey(frame, render_pass_key)) {
    return false;
  }
  if (frame.render_pass_key_count == frame.render_pass_keys.size()) {
    return false;
  }
  frame.render_pass_keys[frame.render_pass_key_count++] = render_pass_key;
  return true;
}

CrowdCapturedTitleFrame *FindCapturedTitleFrame(uint64_t generation) {
  const auto found =
      std::ranges::find_if(g_captured_title_frames, [&](const auto &frame) {
        return frame.valid && frame.generation == generation;
      });
  return found == g_captured_title_frames.end() ? nullptr : &*found;
}

CrowdCapturedTitleFrame *
SelectTitleFrameForBlock(const CrowdBackendBlock &block,
                         uint32_t &fifo_candidates) {
  fifo_candidates = 0;
  CrowdCapturedTitleFrame *active =
      FindCapturedTitleFrame(g_active_title_generation);
  if (active != nullptr && SameBackendSequence(block, *active) &&
      !HasRenderPassKey(*active, block.contract.render_pass_key)) {
    return active;
  }

  CrowdCapturedTitleFrame *oldest = nullptr;
  for (CrowdCapturedTitleFrame &frame : g_captured_title_frames) {
    if (!frame.valid || frame.matched_block_count != 0 ||
        !SameBackendSequence(block, frame) ||
        (g_active_title_generation != 0 &&
         frame.generation <= g_active_title_generation)) {
      continue;
    }
    ++fifo_candidates;
    if (oldest == nullptr || frame.generation < oldest->generation) {
      oldest = &frame;
    }
  }
  return oldest;
}

void LogUniqueRenderPassContract(const CrowdBackendBlock &block,
                                 bool contract_variant) {
  REXLOG_INFO(
      "Table Tennis crowd observer: C6 backend block contract "
      "block={} draws={} sequence={:016X} pass={:08X} variant={} "
      "backend={} pitch={} state_valid={} depth={:08X} "
      "color_mask={:08X} color_control={:08X} blend0={:08X} "
      "restart={{enabled={},index={:08X}}} attachments_valid={} "
      "colors={{count={},f0={},f1={},f2={},f3={}}} "
      "depth_format={} stencil_format={} samples={} "
      "sample_mask={:016X} uniform={} observer_only=true",
      block.sequence, block.draw_count + block.overflow_draw_count,
      block.sequence_fingerprint, block.contract.render_pass_key,
      contract_variant, block.contract.backend, block.contract.surface_pitch,
      block.contract.draw_state_contract_valid,
      block.contract.normalized_depth_control,
      block.contract.normalized_color_mask, block.contract.color_control,
      block.contract.blend_control_0, block.contract.primitive_restart_enabled,
      block.contract.primitive_restart_index,
      block.contract.attachment_contract_valid,
      block.contract.color_attachment_count,
      block.contract.color_attachment_formats[0],
      block.contract.color_attachment_formats[1],
      block.contract.color_attachment_formats[2],
      block.contract.color_attachment_formats[3],
      block.contract.depth_attachment_format,
      block.contract.stencil_attachment_format, block.contract.sample_count,
      block.contract.sample_mask, block.contract_uniform);
}

bool RegisterRenderPassContractLocked(const CrowdBackendBlock &block) {
  for (CrowdUniqueRenderPassContract &entry : g_unique_render_pass_contracts) {
    if (!entry.valid ||
        entry.contract.render_pass_key != block.contract.render_pass_key) {
      continue;
    }
    if (entry.contract == block.contract) {
      return true;
    }
    ++g_building_frame.backend_contract_mismatch_block_count;
    LogUniqueRenderPassContract(block, true);
    return false;
  }

  CrowdUniqueRenderPassContract *entry =
      FindFreeSlot(g_unique_render_pass_contracts);
  if (entry == nullptr) {
    ++g_building_frame.backend_contract_mismatch_block_count;
    return false;
  }
  entry->contract = block.contract;
  entry->valid = true;
  ++g_building_frame.backend_unique_render_pass_count;
  LogUniqueRenderPassContract(block, false);
  return true;
}

bool RecordBackendBlockMatchLocked(const CrowdBackendBlock &block,
                                   CrowdCapturedTitleFrame &title_frame,
                                   uint32_t fifo_candidates) {
  const bool repeated_sequence =
      SameBackendSequence(block, g_last_matched_backend_block);
  if (repeated_sequence) {
    ++g_backend_replay_run_ordinal;
  } else {
    g_backend_replay_run_ordinal = 1;
  }
  g_last_matched_backend_block = block;

  if (!AddRenderPassKey(title_frame, block.contract.render_pass_key)) {
    return false;
  }
  if (fifo_candidates > 1) {
    ++g_building_frame.backend_fifo_disambiguated_block_count;
  }
  ++title_frame.matched_block_count;
  ++g_building_frame.backend_matched_block_count;
  if (repeated_sequence) {
    ++g_building_frame.backend_repeated_tile_block_count;
  }
  g_active_title_generation = title_frame.generation;
  g_building_frame.backend_last_block_draw_count = block.draw_count;
  g_building_frame.backend_last_tile_ordinal = g_backend_replay_run_ordinal;
  g_building_frame.backend_last_title_generation = title_frame.generation;
  g_building_frame.backend_last_sequence_fingerprint =
      block.sequence_fingerprint;
  g_building_frame.backend_last_contract = ContractTelemetry(block);
  g_building_frame.backend_block_proof_observed = true;

  const CrowdTitleDrawIdentity &first = title_frame.draws[0];
  if (!g_announced_backend_block_proof) {
    g_announced_backend_block_proof = true;
    REXLOG_INFO("Table Tennis crowd observer: matched ordered C6 backend block "
                "block={} title_generation={} tile={} draws={} "
                "sequence={:016X} payload={:016X} pass={:08X} "
                "owner={:08X} drawable={:08X} model={:08X} "
                "first_index={:08X}+{} observer_only=true",
                block.sequence, title_frame.generation,
                g_backend_replay_run_ordinal, block.draw_count,
                block.sequence_fingerprint, title_frame.payload_fingerprint,
                block.contract.render_pass_key, first.owner, first.drawable,
                first.model, first.draw.index_physical_address,
                first.draw.index_count);
  }
  if (repeated_sequence && !g_announced_repeated_tile_block) {
    g_announced_repeated_tile_block = true;
    REXLOG_INFO(
        "Table Tennis crowd observer: detected repeated EDRAM tile block "
        "title_generation={} tile={} block={} draws={} "
        "sequence={:016X} pass={:08X} observer_only=true",
        title_frame.generation, g_backend_replay_run_ordinal, block.sequence,
        block.draw_count, block.sequence_fingerprint,
        block.contract.render_pass_key);
  }
  return true;
}

bool TryMatchBackendBlockLocked(const CrowdBackendBlock &block) {
  uint32_t fifo_candidates = 0;
  CrowdCapturedTitleFrame *title_frame =
      SelectTitleFrameForBlock(block, fifo_candidates);
  return title_frame != nullptr &&
         RecordBackendBlockMatchLocked(block, *title_frame, fifo_candidates);
}

CrowdBackendBlock *OldestPendingBackendBlock() {
  CrowdBackendBlock *oldest = nullptr;
  for (CrowdBackendBlock &block : g_pending_backend_blocks) {
    if (block.valid &&
        (oldest == nullptr || block.sequence < oldest->sequence)) {
      oldest = &block;
    }
  }
  return oldest;
}

void DrainPendingBackendBlocksLocked() {
  while (CrowdBackendBlock *block = OldestPendingBackendBlock()) {
    if (!TryMatchBackendBlockLocked(*block)) {
      break;
    }
    *block = {};
  }
}

void QueuePendingBackendBlockLocked(CrowdBackendBlock block) {
  CrowdBackendBlock *slot = FindFreeSlot(g_pending_backend_blocks);
  if (slot == nullptr) {
    CrowdBackendBlock *oldest = OldestPendingBackendBlock();
    if (oldest != nullptr) {
      ++g_building_frame.backend_unmatched_block_count;
      ++g_building_frame.backend_dropped_block_count;
      *oldest = {};
      slot = oldest;
    }
  }
  if (slot == nullptr) {
    ++g_building_frame.backend_unmatched_block_count;
    ++g_building_frame.backend_dropped_block_count;
    return;
  }
  *slot = std::move(block);
  DrainPendingBackendBlocksLocked();
}

uint32_t PendingBackendBlockCount() {
  return static_cast<uint32_t>(std::ranges::count_if(
      g_pending_backend_blocks,
      [](const CrowdBackendBlock &block) { return block.valid; }));
}

void FinalizeActiveBackendBlockLocked() {
  if (!g_active_backend_block.valid) {
    return;
  }

  CrowdBackendBlock block = std::move(g_active_backend_block);
  g_active_backend_block = {};
  ++g_building_frame.backend_finalized_block_count;
  g_building_frame.backend_last_block_draw_count =
      block.draw_count + block.overflow_draw_count;
  g_building_frame.backend_last_sequence_fingerprint =
      block.sequence_fingerprint;
  g_building_frame.backend_last_contract = ContractTelemetry(block);

  const bool registered = RegisterRenderPassContractLocked(block);
  if (block.overflow_draw_count != 0) {
    ++g_building_frame.backend_overflow_block_count;
  }
  if (!block.contract_uniform || !block.contract.draw_state_contract_valid ||
      !block.contract.attachment_contract_valid) {
    ++g_building_frame.backend_contract_mismatch_block_count;
  }
  if (!registered || block.overflow_draw_count != 0 ||
      !block.contract_uniform || !block.contract.draw_state_contract_valid ||
      !block.contract.attachment_contract_valid) {
    ++g_building_frame.backend_unmatched_block_count;
    return;
  }
  QueuePendingBackendBlockLocked(std::move(block));
}

void AppendBackendBlockDrawLocked(
    const rex::graphics::NativeGuestDrawContext &context) {
  g_pending_replacement_candidate.reset();
  const CrowdBackendBlockContract contract = CaptureBackendContract(context);
  if (!g_active_backend_block.valid) {
    g_active_backend_block = {};
    g_active_backend_block.sequence = ++g_backend_block_sequence;
    g_active_backend_block.sequence_fingerprint = kFnvOffsetBasis;
    g_active_backend_block.contract = contract;
    g_active_backend_block.candidate_title_generation =
        g_active_title_generation;
    g_active_backend_block.candidate_prefix_matches =
        g_active_title_generation != 0;
    g_active_backend_block.contract_uniform = true;
    g_active_backend_block.valid = true;
  } else {
    g_active_backend_block.contract_uniform &=
        g_active_backend_block.contract == contract;
  }

  const CrowdBackendDrawIdentity draw = BackendIdentity(context);
  AppendDrawFingerprint(g_active_backend_block.sequence_fingerprint, draw);
  if (g_active_backend_block.draw_count ==
      g_active_backend_block.draws.size()) {
    ++g_active_backend_block.overflow_draw_count;
    return;
  }
  g_active_backend_block.draws[g_active_backend_block.draw_count++] = draw;

  CrowdCapturedTitleFrame *const title_frame =
      FindCapturedTitleFrame(g_active_backend_block.candidate_title_generation);
  const uint32_t draw_index = g_active_backend_block.draw_count - 1;
  if (title_frame == nullptr || title_frame->snapshot == nullptr ||
      title_frame->matched_block_count == 0 ||
      draw_index >= title_frame->draw_count) {
    g_active_backend_block.candidate_prefix_matches = false;
    return;
  }
  g_active_backend_block.candidate_prefix_matches &=
      title_frame->draws[draw_index].draw == draw;
  if (!g_active_backend_block.candidate_prefix_matches) {
    return;
  }

  const CrowdTitleDrawIdentity &title_draw = title_frame->draws[draw_index];
  auto candidate = std::make_shared<CrowdReplacementCandidate>();
  candidate->title_generation = title_frame->generation;
  candidate->backend_block_sequence = g_active_backend_block.sequence;
  candidate->tile_ordinal = title_frame->matched_block_count + 1;
  candidate->draw_index = draw_index;
  candidate->render_pass_key = contract.render_pass_key;
  candidate->primitive_type = title_draw.draw.primitive_type;
  candidate->submitted_index_count = title_draw.draw.index_count;
  candidate->guest_index_base = title_draw.draw.index_physical_address;
  candidate->frame = title_frame->snapshot;
  if (candidate->valid()) {
    g_pending_replacement_candidate = std::move(candidate);
  }
}

bool CaptureTitleFrameLocked(const CrowdFrameSnapshot &source) {
  if (!source.family_contract_verified || source.draws.empty() ||
      source.draws.size() > kMaximumFrameDraws) {
    return false;
  }

  CrowdCapturedTitleFrame captured;
  captured.generation = source.sequence;
  captured.sequence_fingerprint = kFnvOffsetBasis;
  captured.payload_fingerprint = kFnvOffsetBasis;
  for (const CrowdDrawSnapshot &snapshot : source.draws) {
    if (!snapshot.valid || snapshot.primitive_type != kTriangleStripPrimitive ||
        snapshot.owner.crowd == 0 || snapshot.owner.submitted_drawable == 0 ||
        snapshot.owner.submitted_model == 0 || snapshot.indices == nullptr ||
        snapshot.indices->physical_address == 0 ||
        snapshot.indices->submitted_index_count !=
            snapshot.submitted_index_count) {
      return false;
    }
    const CrowdImmutablePayloadIdentity payload =
        PayloadIdentityForSnapshot(snapshot);
    if (!payload.valid()) {
      return false;
    }
    CrowdTitleDrawIdentity &identity = captured.draws[captured.draw_count++];
    identity = {
        .owner = snapshot.owner.crowd,
        .drawable = snapshot.owner.submitted_drawable,
        .model = snapshot.owner.submitted_model,
        .draw =
            {
                .primitive_type = snapshot.primitive_type,
                .index_count = snapshot.submitted_index_count,
                .index_physical_address = snapshot.indices->physical_address,
            },
        .payload = payload,
    };
    AppendDrawFingerprint(captured.sequence_fingerprint, identity.draw);
    AppendFingerprintWord(captured.payload_fingerprint, identity.owner);
    AppendFingerprintWord(captured.payload_fingerprint, identity.drawable);
    AppendFingerprintWord(captured.payload_fingerprint, identity.model);
    AppendFingerprintU64(captured.payload_fingerprint,
                         identity.payload.vertex_fingerprint);
    AppendFingerprintU64(captured.payload_fingerprint,
                         identity.payload.index_fingerprint);
    AppendFingerprintU64(captured.payload_fingerprint,
                         identity.payload.palette_fingerprint);
    AppendFingerprintU64(captured.payload_fingerprint,
                         identity.payload.texture_fingerprint);
    AppendFingerprintU64(captured.payload_fingerprint,
                         identity.payload.material_fingerprint);
  }
  captured.valid = captured.draw_count == source.draws.size();

  CrowdCapturedTitleFrame *slot = FindFreeSlot(g_captured_title_frames);
  if (slot == nullptr) {
    slot = &g_captured_title_frames.front();
    for (CrowdCapturedTitleFrame &candidate : g_captured_title_frames) {
      if (candidate.generation < slot->generation) {
        slot = &candidate;
      }
    }
    if (slot->generation == g_active_title_generation) {
      g_active_title_generation = 0;
    }
  }
  *slot = std::move(captured);
  ++g_building_frame.backend_captured_title_frame_count;
  DrainPendingBackendBlocksLocked();
  return true;
}

uint32_t CountUniqueGeometry(const CrowdFrameSnapshot &frame) {
  std::vector<std::pair<uint32_t, uint64_t>> identities;
  identities.reserve(frame.draws.size());
  for (const CrowdDrawSnapshot &draw : frame.draws) {
    if (draw.vertices == nullptr) {
      continue;
    }
    const auto identity = std::pair{draw.vertices->source_virtual_alias,
                                    draw.vertices->payload_fingerprint};
    if (std::ranges::find(identities, identity) == identities.end()) {
      identities.push_back(identity);
    }
  }
  return static_cast<uint32_t>(identities.size());
}

bool VerifyFamilyContract(const CrowdFrameSnapshot &frame) {
  if (frame.candidate_draw_count == 0 ||
      frame.draws.size() != frame.candidate_draw_count ||
      frame.valid_draw_count != frame.candidate_draw_count ||
      frame.copy_failures != 0 || frame.dropped_draw_count != 0 ||
      frame.unique_geometry_count == 0) {
    return false;
  }

  return std::ranges::all_of(frame.draws, [](const CrowdDrawSnapshot &draw) {
    return draw.valid && draw.owner.valid &&
           std::ranges::any_of(kTraceIndexHistogram, [&](const auto &expected) {
             return expected.first == draw.submitted_index_count;
           });
  });
}

bool VerifyTraceParity(const CrowdFrameSnapshot &frame) {
  if (!VerifyFamilyContract(frame) || frame.candidate_draw_count != 156 ||
      frame.submitted_index_count != 114819 ||
      frame.unique_geometry_count != 22) {
    return false;
  }
  for (const auto &[index_count, expected_occurrences] : kTraceIndexHistogram) {
    const uint32_t actual_occurrences = static_cast<uint32_t>(
        std::ranges::count(frame.draws, index_count,
                           &CrowdDrawSnapshot::submitted_index_count));
    if (actual_occurrences != expected_occurrences) {
      return false;
    }
  }
  return true;
}

void LogRejectedFrame(const CrowdFrameSnapshot &frame) {
  if (frame.draws.empty()) {
    REXLOG_INFO(
        "Table Tennis crowd observer: fxCrowdGfx scope observed but no "
        "36-byte crowd draws were captured (render_scopes={} "
        "drawable_submits={} owner_indexed_draws={} mesh_valid={} "
        "stride36={} secondary_stream={} nonzero_indices={} "
        "first={{mesh_valid={},mesh_failures={},aggregate={:08X},"
        "primary={:08X},secondary={:08X},vb={:08X},stride={},"
        "ib={:08X},index_size={},primitive={},count={}}}) "
        "observer_only=true",
        frame.render_scope_count, frame.drawable_submit_count,
        frame.owner_indexed_draw_count, frame.mesh_valid_owner_draw_count,
        frame.stride_36_owner_draw_count,
        frame.secondary_stream_owner_draw_count,
        frame.nonzero_index_owner_draw_count, frame.first_owner_mesh_valid,
        frame.first_owner_mesh_read_failures,
        frame.first_owner_vertex_aggregate, frame.first_owner_primary_stream,
        frame.first_owner_secondary_stream, frame.first_owner_vertex_alias,
        frame.first_owner_vertex_stride, frame.first_owner_index_alias,
        frame.first_owner_index_element_size, frame.first_owner_primitive_type,
        frame.first_owner_submitted_index_count);
    return;
  }

  const CrowdDrawSnapshot &first = frame.draws.front();
  REXLOG_INFO(
      "Table Tennis crowd observer: contract pending render_scopes={} "
      "drawable_submits={} owner_indexed_draws={} candidates={} valid={} "
      "indices={} unique_geometry={} dropped={} copy_failures={} "
      "backend={{c6_draws={},blocks={},matched={},pending={},unmatched={},"
      "tile_replays={},fifo={},contract_mismatch={},overflow={},"
      "dropped={},proof={}}} "
      "first={{ordinal={},owner={:08X},drawable={:08X},model={:08X},"
      "instances={},count={},pass={:08X},"
      "program={:08X},vs={:08X},ps={:08X},"
      "vf95={{valid={},address={:08X},size={},endian={}}},"
      "vf92={{valid={},address={:08X},size={},endian={}}},"
      "texture_fetch={{{:08X},{:08X},{:08X},{:08X},{:08X},{:08X}}},"
      "vb={},ib={},palette={},texture={},decl={},decode={}}} "
      "observer_only=true",
      frame.render_scope_count, frame.drawable_submit_count,
      frame.owner_indexed_draw_count, frame.candidate_draw_count,
      frame.valid_draw_count, frame.submitted_index_count,
      frame.unique_geometry_count, frame.dropped_draw_count,
      frame.copy_failures, frame.backend_c6_draw_count,
      frame.backend_finalized_block_count, frame.backend_matched_block_count,
      frame.backend_pending_block_count, frame.backend_unmatched_block_count,
      frame.backend_repeated_tile_block_count,
      frame.backend_fifo_disambiguated_block_count,
      frame.backend_contract_mismatch_block_count,
      frame.backend_overflow_block_count, frame.backend_dropped_block_count,
      frame.backend_block_proof_observed, first.ordinal, first.owner.crowd,
      first.owner.submitted_drawable, first.owner.submitted_model,
      first.owner.visible_instance_count, first.submitted_index_count,
      first.pass_descriptor, first.program_pair, first.vertex_shader,
      first.pixel_shader, first.vertex_fetch.valid,
      first.vertex_fetch.physical_address, first.vertex_fetch.size,
      first.vertex_fetch.endian, first.palette_fetch.valid,
      first.palette_fetch.physical_address, first.palette_fetch.size,
      first.palette_fetch.endian, first.material.texture_fetch[0],
      first.material.texture_fetch[1], first.material.texture_fetch[2],
      first.material.texture_fetch[3], first.material.texture_fetch[4],
      first.material.texture_fetch[5],
      first.vertices != nullptr && first.vertices->valid(),
      first.indices != nullptr && first.indices->valid(),
      first.palette != nullptr && first.palette->valid(),
      first.material.texture != nullptr && first.material.texture->valid(),
      first.material.declaration.valid,
      first.material.decode_constants_verified);
}

void LogVerifiedFrame(const CrowdFrameSnapshot &frame) {
  const CrowdDrawSnapshot &first = frame.draws.front();
  REXLOG_INFO(
      "Table Tennis crowd observer: VERIFIED fxCrowdGfx family "
      "owner={:08X} vtable={:08X} drawable={:08X} model={:08X} "
      "instances={} drawable_submits={} owner_indexed_draws={} "
      "engine_draws={} unique_geometry={} submitted_indices={} "
      "trace_parity={} backend={{c6_draws={},blocks={},matched={},"
      "pending={},unmatched={},tile_replays={},fifo={},"
      "contract_mismatch={},overflow={},dropped={},proof={}}} "
      "trace_ps=C6CEFDA3753CF2BA trace_vs=BD4B1DF972B828B7 "
      "observer_only=true",
      first.owner.crowd, first.owner.vtable, first.owner.submitted_drawable,
      first.owner.submitted_model, first.owner.visible_instance_count,
      frame.drawable_submit_count, frame.owner_indexed_draw_count,
      frame.candidate_draw_count, frame.unique_geometry_count,
      frame.submitted_index_count, frame.trace_parity_verified,
      frame.backend_c6_draw_count, frame.backend_finalized_block_count,
      frame.backend_matched_block_count, frame.backend_pending_block_count,
      frame.backend_unmatched_block_count,
      frame.backend_repeated_tile_block_count,
      frame.backend_fifo_disambiguated_block_count,
      frame.backend_contract_mismatch_block_count,
      frame.backend_overflow_block_count, frame.backend_dropped_block_count,
      frame.backend_block_proof_observed);
  REXLOG_INFO(
      "  crowd_contract vf95={{stride=36,endian=2,float3_position,"
      "packed_instance_index,2_10_10_10_normal,float2_uv}} "
      "vf92={{stride=28,endian=2,float4_quaternion,float3_translation}} "
      "texture={{format=DXT1,size={}x{}x{},volume={},tiled={},"
      "swizzle={:03X}}} "
      "constants={{c32-c39,c136-c141,c145,c255}}",
      first.material.texture->width, first.material.texture->height,
      first.material.texture->layers, first.material.texture->volume,
      first.material.texture->tiled, first.material.texture->fetch_swizzle);
}

void LogBackendProofSummary(const CrowdFrameSnapshot &frame) {
  const auto &contract = frame.backend_last_contract;
  REXLOG_INFO(
      "Table Tennis crowd observer: backend block summary generation={} "
      "captured_frames={} c6_draws={} blocks={} matched={} pending={} "
      "unmatched={} tile_replays={} fifo={} contract_mismatch={} "
      "overflow={} dropped={} unique_passes={} proof={} "
      "last={{title_generation={},tile={},draws={},sequence={:016X},"
      "pass={:08X},backend={},pitch={},state_valid={},depth={:08X},"
      "color_mask={:08X},color_control={:08X},blend0={:08X},"
      "raster={:08X}/{},"
      "restart_enabled={},restart_index={:08X},attachments_valid={},"
      "color_count={},formats={{{},{},{},{}}},depth_format={},"
      "stencil_format={},samples={},sample_mask={:016X},uniform={}}} "
      "observer_only=true",
      frame.sequence, frame.backend_captured_title_frame_count,
      frame.backend_c6_draw_count, frame.backend_finalized_block_count,
      frame.backend_matched_block_count, frame.backend_pending_block_count,
      frame.backend_unmatched_block_count,
      frame.backend_repeated_tile_block_count,
      frame.backend_fifo_disambiguated_block_count,
      frame.backend_contract_mismatch_block_count,
      frame.backend_overflow_block_count, frame.backend_dropped_block_count,
      frame.backend_unique_render_pass_count,
      frame.backend_block_proof_observed, frame.backend_last_title_generation,
      frame.backend_last_tile_ordinal, frame.backend_last_block_draw_count,
      frame.backend_last_sequence_fingerprint, contract.render_pass_key,
      contract.backend, contract.surface_pitch,
      contract.draw_state_contract_valid, contract.normalized_depth_control,
      contract.normalized_color_mask, contract.color_control,
      contract.blend_control_0, contract.rasterizer_mode_control,
      contract.rasterizer_mode_control_valid,
      contract.primitive_restart_enabled,
      contract.primitive_restart_index, contract.attachment_contract_valid,
      contract.color_attachment_count, contract.color_attachment_formats[0],
      contract.color_attachment_formats[1],
      contract.color_attachment_formats[2],
      contract.color_attachment_formats[3], contract.depth_attachment_format,
      contract.stencil_attachment_format, contract.sample_count,
      contract.sample_mask, contract.block_uniform);
}

} // namespace

bool CrowdObserverEnabled() {
  return REXCVAR_GET(tabletennis_native_crowd_observer) ||
         CrowdReplacementPrewarmEnabled() || NativeFrameSceneCaptureEnabled();
}

void BeginCrowdRenderScope(uint8_t *guest_base, uint32_t crowd) {
  CrowdOwnerSnapshot owner;
  if (CrowdObserverEnabled()) {
    owner = CaptureOwner(guest_base, crowd);
  }
  if (g_crowd_render_scope_depth < g_crowd_render_scope_stack.size()) {
    g_crowd_render_scope_stack[g_crowd_render_scope_depth] = owner;
  }
  ++g_crowd_render_scope_depth;

  if (owner.valid) {
    std::lock_guard lock(g_observer_mutex);
    ++g_building_frame.render_scope_count;
  }
}

void EndCrowdRenderScope() {
  if (g_crowd_render_scope_depth != 0) {
    --g_crowd_render_scope_depth;
  }
}

void BeginCrowdDrawableSubmitScope(uint8_t *guest_base, uint32_t crowd,
                                   uint32_t drawable, uint32_t model) {
  (void)guest_base;
  CrowdOwnerSnapshot owner;
  if (CrowdObserverEnabled()) {
    if (const CrowdOwnerSnapshot *render_owner = CurrentCrowdRenderOwner();
        render_owner != nullptr && render_owner->crowd == crowd) {
      owner = *render_owner;
      owner.submitted_drawable = drawable;
      owner.submitted_model = model;
      owner.valid = MatchesOwnedSubmitPair(owner, drawable, model);
    }
  }
  if (g_crowd_submit_scope_depth < g_crowd_submit_scope_stack.size()) {
    g_crowd_submit_scope_stack[g_crowd_submit_scope_depth] = owner;
  }
  ++g_crowd_submit_scope_depth;

  if (owner.valid) {
    std::lock_guard lock(g_observer_mutex);
    ++g_building_frame.drawable_submit_count;
  }
}

void EndCrowdDrawableSubmitScope() {
  if (g_crowd_submit_scope_depth != 0) {
    --g_crowd_submit_scope_depth;
  }
}

void ObserveCrowdDraw(uint8_t *guest_base,
                      const SceneCatalogDrawOccurrence &draw) {
  if (!CrowdObserverEnabled()) {
    return;
  }
  const CrowdOwnerSnapshot *owner = CurrentCrowdSubmitOwner();
  if (owner == nullptr) {
    return;
  }
  {
    std::lock_guard lock(g_observer_mutex);
    CrowdFrameSnapshot &frame = g_building_frame;
    ++frame.owner_indexed_draw_count;
    frame.mesh_valid_owner_draw_count += draw.mesh.valid;
    frame.stride_36_owner_draw_count +=
        draw.mesh.vertex_stride == kCrowdVertexStride;
    frame.secondary_stream_owner_draw_count +=
        draw.mesh.secondary_vertex_stream != 0;
    frame.nonzero_index_owner_draw_count += draw.submitted_index_count != 0;
    if (frame.owner_indexed_draw_count == 1) {
      frame.first_owner_mesh_valid = draw.mesh.valid;
      frame.first_owner_mesh_read_failures = draw.mesh.guest_read_failures;
      frame.first_owner_vertex_aggregate = draw.mesh.vertex_aggregate;
      frame.first_owner_primary_stream = draw.mesh.primary_vertex_stream;
      frame.first_owner_secondary_stream = draw.mesh.secondary_vertex_stream;
      frame.first_owner_vertex_alias = draw.mesh.vertex_buffer_alias;
      frame.first_owner_vertex_stride = draw.mesh.vertex_stride;
      frame.first_owner_index_alias = draw.mesh.index_buffer_alias;
      frame.first_owner_index_element_size = draw.mesh.index_element_size;
      frame.first_owner_primitive_type = draw.primitive_type;
      frame.first_owner_submitted_index_count = draw.submitted_index_count;
    }
  }
  if (!IsCrowdCandidate(draw)) {
    return;
  }

  CrowdDrawSnapshot snapshot =
      CaptureCrowdDrawSnapshot(guest_base, draw, *owner);
  std::lock_guard lock(g_observer_mutex);
  ++g_building_frame.candidate_draw_count;
  g_building_frame.valid_draw_count += snapshot.valid;
  g_building_frame.submitted_index_count += snapshot.submitted_index_count;
  g_building_frame.copy_failures += snapshot.copy_failures;
  if (g_building_frame.draws.size() == kMaximumFrameDraws) {
    ++g_building_frame.dropped_draw_count;
    return;
  }
  g_building_frame.draws.push_back(std::move(snapshot));
}

bool ObserveCrowdBackendProofDraw(
    const rex::graphics::NativeGuestDrawContext &context) {
  if (!CrowdObserverEnabled() || !context.render_pass_key_valid) {
    return false;
  }

  std::lock_guard lock(g_observer_mutex);
  if (IsExactCrowdBackendDraw(context)) {
    ++g_building_frame.backend_c6_draw_count;
    AppendBackendBlockDrawLocked(context);
  } else {
    // Early probes are rejected above. Therefore only a real late backend
    // draw can terminate a contiguous C6 block.
    FinalizeActiveBackendBlockLocked();
    g_pending_replacement_candidate.reset();
  }
  return false;
}

std::shared_ptr<const CrowdReplacementCandidate>
ConsumeCrowdReplacementCandidate(
    const rex::graphics::NativeGuestDrawContext &context) {
  std::lock_guard lock(g_observer_mutex);
  std::shared_ptr<const CrowdReplacementCandidate> candidate =
      std::move(g_pending_replacement_candidate);
  if (candidate == nullptr || !candidate->valid() ||
      !context.render_pass_key_valid || !context.indexed ||
      !context.guest_index_base_valid ||
      context.render_pass_key != candidate->render_pass_key ||
      context.primitive_type != candidate->primitive_type ||
      context.guest_vertex_or_index_count != candidate->submitted_index_count ||
      context.guest_index_base != candidate->guest_index_base) {
    return nullptr;
  }
  return candidate;
}

void CrowdObserverFrameEnd() {
  const bool enabled = CrowdObserverEnabled();
  std::shared_ptr<const CrowdFrameSnapshot> published;
  {
    std::lock_guard lock(g_observer_mutex);
    ++g_frame_sequence;
    if (!enabled) {
      g_building_frame = {};
      g_published_frame.reset();
      g_active_backend_block = {};
      g_captured_title_frames = {};
      g_pending_backend_blocks = {};
      g_unique_render_pass_contracts = {};
      g_active_title_generation = 0;
      g_pending_replacement_candidate.reset();
      g_announced_verified = false;
      g_announced_rejection = false;
      g_announced_backend_block_proof = false;
      g_announced_repeated_tile_block = false;
      g_last_backend_summary_sequence = 0;
    } else {
      g_building_frame.sequence = g_frame_sequence;
      g_building_frame.unique_geometry_count =
          CountUniqueGeometry(g_building_frame);
      g_building_frame.family_contract_verified =
          VerifyFamilyContract(g_building_frame);
      g_building_frame.trace_parity_verified =
          VerifyTraceParity(g_building_frame);
      CaptureTitleFrameLocked(g_building_frame);
      g_building_frame.backend_pending_block_count = PendingBackendBlockCount();
      g_published_frame = std::make_shared<const CrowdFrameSnapshot>(
          std::move(g_building_frame));
      if (CrowdCapturedTitleFrame *const captured =
              FindCapturedTitleFrame(g_published_frame->sequence);
          captured != nullptr) {
        captured->snapshot = g_published_frame;
      }
      g_building_frame = {};
      published = g_published_frame;
    }
  }
  CrowdSnapshotFrameEnd();

  if (published == nullptr || published->render_scope_count == 0) {
    return;
  }
  if (published->valid() && !g_announced_verified) {
    g_announced_verified = true;
    LogVerifiedFrame(*published);
  } else if (!published->valid() && !g_announced_rejection) {
    g_announced_rejection = true;
    LogRejectedFrame(*published);
  }
  if (published->valid() &&
      (published->backend_captured_title_frame_count != 0 ||
       published->backend_c6_draw_count != 0 ||
       published->backend_finalized_block_count != 0 ||
       published->backend_pending_block_count != 0) &&
      (g_last_backend_summary_sequence == 0 ||
       published->sequence >= g_last_backend_summary_sequence + 30)) {
    LogBackendProofSummary(*published);
    g_last_backend_summary_sequence = published->sequence;
  }
  ObserveMainCoverageFamilyFrame(LatestCrowdBackendProofFrameSnapshot());
}

std::shared_ptr<const CrowdFrameSnapshot> LatestCrowdFrameSnapshot() {
  std::lock_guard lock(g_observer_mutex);
  return g_published_frame;
}

std::shared_ptr<const CrowdFrameSnapshot>
LatestCrowdBackendProofFrameSnapshot() {
  std::lock_guard lock(g_observer_mutex);
  const CrowdCapturedTitleFrame *const frame =
      FindCapturedTitleFrame(g_active_title_generation);
  return frame != nullptr ? frame->snapshot : nullptr;
}

} // namespace tabletennis::native
