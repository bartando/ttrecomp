#include "native/tabletennis_hud_swf_capture.h"

#include "generated/default/tabletennis_init.h"
#include "native/tabletennis_frame_scene.h"
#include "native/tabletennis_guest_memory.h"
#include "native/tabletennis_texture_snapshot.h"

#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <mutex>
#include <utility>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(
    tabletennis_native_hud_swf_capture, false, "Table Tennis",
    "Capture the gameplay HUD's real SWF texture binds and completed 36-byte "
    "dynamic vertex batches. Observer-only; never serves or suppresses.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_UINT32(
    tabletennis_native_hud_swf_log_interval, 120, "Table Tennis",
    "Published HUD/SWF frames between observer readiness reports.")
    .range(1, 100000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace tabletennis::native {
namespace {

constexpr uint32_t kHudGlobal = 0x82606610;
constexpr uint32_t kHudSwfContextOffset = 0x5C;
constexpr uint32_t kDefaultTextureGlobal = 0x825EBA20;
constexpr uint32_t kDynamicVertexBaseGlobal = 0x825EBA74;
constexpr uint32_t kDynamicVertexCountGlobal = 0x825EBA88;
constexpr uint32_t kGrcTextureXenonVtable = 0x820353CC;
constexpr size_t kGrcTextureXenonGpuBindingOffset = 0x10;
constexpr size_t kMaximumScopeDepth = 8;
constexpr size_t kMaximumBatchesPerFrame = 2048;
constexpr size_t kMaximumTextureBindsPerFrame = 2048;
constexpr uint32_t kMaximumVerticesPerBatch = 65536;
constexpr size_t kMaximumVertexBytesPerFrame = 32 * 1024 * 1024;
constexpr size_t kGpuBindingWordCount = 13;
constexpr size_t kTextureFetchWordCount = 6;
constexpr size_t kMaximumTextureResourceMappings = 1024;

struct TextureResourceMapping {
  uint32_t resource = 0;
  uint32_t resource_vtable = 0;
  uint32_t gpu_binding = 0;
};

struct PendingBatch {
  uint64_t order = 0;
  uint32_t guest_base = 0;
  uint32_t batch_kind = 0;
  uint32_t requested_vertex_count = 0;
  uint32_t texture_handle = 0;
  uint64_t texture_bind_order = 0;
  bool armed = false;
};

struct DrawScope {
  uint32_t hud = 0;
  uint32_t swf_context = 0;
  uint32_t current_texture = 0;
  uint64_t current_texture_bind_order = 0;
  PendingBatch pending{};
  bool accepted = false;
};

thread_local std::array<DrawScope, kMaximumScopeDepth> g_scope_stack;
thread_local size_t g_scope_depth = 0;
thread_local size_t g_scope_overflow_depth = 0;

std::mutex g_capture_mutex;
HudSwfFrameSnapshot g_building_frame;
std::shared_ptr<const HudSwfFrameSnapshot> g_published_frame;
uint64_t g_frame_sequence = 0;
uint64_t g_event_order = 0;
bool g_logged_first_nonempty_frame = false;
std::array<TextureResourceMapping, kMaximumTextureResourceMappings>
    g_texture_resource_mappings;
size_t g_texture_resource_mapping_count = 0;

bool CheckedGuestRange(uint32_t address, size_t size) {
  if (address == 0 || size == 0) {
    return false;
  }
  const uint64_t end = static_cast<uint64_t>(address) + size;
  return end <= (uint64_t{1} << 32);
}

const void *GuestHostAddress(uint8_t *guest_base, uint32_t address) {
  return guest_base + address + REX_PHYS_HOST_OFFSET(address);
}

bool TryReadBeU32(uint8_t *guest_base, uint32_t address, uint32_t &value) {
  if (guest_base == nullptr ||
      !CheckedGuestRange(address, sizeof(uint32_t))) {
    return false;
  }
  uint32_t guest_value = 0;
  if (!GuestTryCopy(&guest_value, GuestHostAddress(guest_base, address),
                    sizeof(guest_value))) {
    return false;
  }
  value = std::byteswap(guest_value);
  return true;
}

bool TryReadBeU32AtOffset(uint8_t *guest_base, uint32_t address,
                          size_t offset, uint32_t &value) {
  const uint64_t guest_address = static_cast<uint64_t>(address) + offset;
  if (guest_address > std::numeric_limits<uint32_t>::max()) {
    return false;
  }
  return TryReadBeU32(guest_base, static_cast<uint32_t>(guest_address),
                      value);
}

HudSwfTextureCaptureFailure CaptureTextureFetch(
    uint8_t *guest_base, uint32_t gpu_binding,
    std::array<uint32_t, kGpuBindingWordCount> &words,
    uint32_t &fetch_offset,
    std::array<uint32_t, kTextureFetchWordCount> &fetch_words) {
  std::array<uint32_t, kGpuBindingWordCount> verification{};
  for (size_t index = 0; index < words.size(); ++index) {
    if (!TryReadBeU32AtOffset(guest_base, gpu_binding,
                              index * sizeof(uint32_t), words[index])) {
      return HudSwfTextureCaptureFailure::kBindingRead;
    }
  }
  for (size_t index = 0; index < verification.size(); ++index) {
    if (!TryReadBeU32AtOffset(guest_base, gpu_binding,
                              index * sizeof(uint32_t),
                              verification[index])) {
      return HudSwfTextureCaptureFailure::kBindingRead;
    }
  }
  if (words != verification) {
    return HudSwfTextureCaptureFailure::kBindingChanged;
  }
  for (size_t index = 0;
       index + fetch_words.size() <= words.size(); ++index) {
    // Xenos fetch type 2 is a texture. Dword 1 contains its nonzero memory
    // base. This is the same structural proof used for material textures.
    if ((words[index] & 0x3u) != 2 || words[index + 1] == 0) {
      continue;
    }
    fetch_offset = static_cast<uint32_t>(index * sizeof(uint32_t));
    std::copy_n(words.begin() + index, fetch_words.size(),
                fetch_words.begin());
    return HudSwfTextureCaptureFailure::kNone;
  }
  return HudSwfTextureCaptureFailure::kNoTextureFetch;
}

void CaptureTexturePayload(uint8_t *guest_base, uint32_t owner,
                           HudSwfTextureBindSnapshot &bind) {
  if (bind.gpu_binding == 0) {
    bind.failure =
        HudSwfTextureCaptureFailure::kResourceUnresolved;
    return;
  }
  bind.failure = CaptureTextureFetch(
      guest_base, bind.gpu_binding, bind.gpu_binding_words,
      bind.texture_fetch_offset, bind.texture_fetch_words);
  if (bind.failure != HudSwfTextureCaptureFailure::kNone) {
    return;
  }
  bind.texture = CaptureTextureSnapshot(
      guest_base, owner, bind.resolved_handle,
      bind.texture_fetch_words);
  if (bind.texture == nullptr || !bind.texture->valid()) {
    bind.failure =
        HudSwfTextureCaptureFailure::kTextureSnapshot;
  }
}

bool FindTextureResourceMapping(uint32_t resource,
                                uint32_t resource_vtable,
                                uint32_t &gpu_binding) {
  std::lock_guard lock(g_capture_mutex);
  const auto begin = g_texture_resource_mappings.begin();
  const auto end = begin + g_texture_resource_mapping_count;
  const auto found = std::find_if(
      begin, end, [&](const TextureResourceMapping &mapping) {
        return mapping.resource == resource &&
               mapping.resource_vtable == resource_vtable;
      });
  if (found == end) {
    return false;
  }
  gpu_binding = found->gpu_binding;
  return gpu_binding != 0;
}

bool ResolveKnownTextureXenonBinding(uint8_t *guest_base,
                                     uint32_t resource,
                                     uint32_t resource_vtable,
                                     uint32_t &gpu_binding) {
  if (resource_vtable != kGrcTextureXenonVtable) {
    return false;
  }
  uint32_t first = 0;
  uint32_t second = 0;
  if (!TryReadBeU32AtOffset(guest_base, resource,
                            kGrcTextureXenonGpuBindingOffset, first) ||
      !TryReadBeU32AtOffset(guest_base, resource,
                            kGrcTextureXenonGpuBindingOffset, second) ||
      first == 0 || first != second) {
    return false;
  }
  gpu_binding = first;
  return true;
}

void StoreTextureResourceMappingLocked(
    const TextureResourceMapping &mapping) {
  const auto begin = g_texture_resource_mappings.begin();
  const auto end = begin + g_texture_resource_mapping_count;
  const auto found = std::find_if(
      begin, end, [&](const TextureResourceMapping &existing) {
        return existing.resource == mapping.resource;
      });
  if (found != end) {
    *found = mapping;
    return;
  }
  if (g_texture_resource_mapping_count ==
      g_texture_resource_mappings.size()) {
    return;
  }
  g_texture_resource_mappings[g_texture_resource_mapping_count++] =
      mapping;
}

void RecordGuestReadFailure() {
  std::lock_guard lock(g_capture_mutex);
  ++g_building_frame.guest_read_failure_count;
}

void ClassifyTextureBindUsage(HudSwfFrameSnapshot &frame) {
  frame.required_texture_bind_count = 0;
  frame.required_texture_capture_failure_count = 0;
  frame.unreferenced_texture_bind_count = 0;
  frame.missing_batch_texture_bind_count = 0;

  for (const HudSwfTextureBindSnapshot &bind : frame.texture_binds) {
    const bool referenced = std::ranges::any_of(
        frame.batches, [&](const HudSwfVertexBatchSnapshot &batch) {
          return batch.texture_bind_order == bind.order;
        });
    if (!referenced) {
      ++frame.unreferenced_texture_bind_count;
      continue;
    }
    ++frame.required_texture_bind_count;
    frame.required_texture_capture_failure_count +=
        bind.failure != HudSwfTextureCaptureFailure::kNone;
  }

  for (const HudSwfVertexBatchSnapshot &batch : frame.batches) {
    const bool binding_present =
        batch.texture_bind_order != 0 &&
        std::ranges::any_of(
            frame.texture_binds,
            [&](const HudSwfTextureBindSnapshot &bind) {
              return bind.order == batch.texture_bind_order;
            });
    frame.missing_batch_texture_bind_count += !binding_present;
  }
}

DrawScope *ActiveScope() {
  if (g_scope_depth == 0 || g_scope_depth > g_scope_stack.size()) {
    return nullptr;
  }
  DrawScope &scope = g_scope_stack[g_scope_depth - 1];
  return scope.accepted ? &scope : nullptr;
}

void FlushCompletedBatch(uint8_t *guest_base, DrawScope &scope) {
  PendingBatch pending = std::exchange(scope.pending, {});
  if (!pending.armed) {
    return;
  }

  uint32_t current_cursor = 0;
  uint32_t vertex_count = 0;
  if (!TryReadBeU32(guest_base, kDynamicVertexBaseGlobal, current_cursor) ||
      !TryReadBeU32(guest_base, kDynamicVertexCountGlobal, vertex_count)) {
    RecordGuestReadFailure();
    return;
  }

  if (vertex_count == 0) {
    std::lock_guard lock(g_capture_mutex);
    ++g_building_frame.empty_batch_count;
    return;
  }

  const uint64_t byte_count_64 =
      static_cast<uint64_t>(vertex_count) *
      HudSwfVertexBatchSnapshot::kVertexStride;
  const uint64_t expected_cursor_64 =
      static_cast<uint64_t>(pending.guest_base) + byte_count_64;
  const bool submitted_cursor_reset = current_cursor == 0;
  const bool cursor_valid =
      expected_cursor_64 <= std::numeric_limits<uint32_t>::max() &&
      (current_cursor == static_cast<uint32_t>(expected_cursor_64) ||
       submitted_cursor_reset);
  const bool vertex_count_valid =
      vertex_count <= pending.requested_vertex_count &&
      vertex_count <= kMaximumVerticesPerBatch &&
      byte_count_64 <= std::numeric_limits<size_t>::max();
  const bool guest_range_valid =
      byte_count_64 <= std::numeric_limits<size_t>::max() &&
      CheckedGuestRange(pending.guest_base,
                        static_cast<size_t>(byte_count_64));
  if (!cursor_valid || !vertex_count_valid || !guest_range_valid) {
    std::lock_guard lock(g_capture_mutex);
    if (g_building_frame.invalid_batch_count == 0) {
      g_building_frame.first_invalid_initial_base = pending.guest_base;
      g_building_frame.first_invalid_current_cursor = current_cursor;
      g_building_frame.first_invalid_vertex_count = vertex_count;
      g_building_frame.first_invalid_requested_count =
          pending.requested_vertex_count;
    }
    ++g_building_frame.invalid_batch_count;
    g_building_frame.invalid_cursor_count += !cursor_valid;
    g_building_frame.invalid_vertex_count += !vertex_count_valid;
    g_building_frame.invalid_guest_range_count += !guest_range_valid;
    return;
  }

  const size_t byte_count = static_cast<size_t>(byte_count_64);
  {
    std::lock_guard lock(g_capture_mutex);
    if (g_building_frame.batches.size() == kMaximumBatchesPerFrame ||
        byte_count >
            kMaximumVertexBytesPerFrame -
                std::min(g_building_frame.captured_vertex_bytes,
                         kMaximumVertexBytesPerFrame)) {
      ++g_building_frame.dropped_batch_count;
      return;
    }
  }

  HudSwfVertexBatchSnapshot batch;
  batch.order = pending.order;
  batch.texture_bind_order = pending.texture_bind_order;
  batch.swf_context = scope.swf_context;
  batch.texture_handle = pending.texture_handle;
  batch.batch_kind = pending.batch_kind;
  batch.requested_vertex_count = pending.requested_vertex_count;
  batch.vertex_count = vertex_count;
  batch.vertex_bytes.resize(byte_count);
  if (!GuestTryCopy(batch.vertex_bytes.data(),
                    GuestHostAddress(guest_base, pending.guest_base),
                    byte_count)) {
    RecordGuestReadFailure();
    return;
  }

  std::lock_guard lock(g_capture_mutex);
  if (g_building_frame.batches.size() == kMaximumBatchesPerFrame ||
      byte_count >
          kMaximumVertexBytesPerFrame -
              std::min(g_building_frame.captured_vertex_bytes,
                       kMaximumVertexBytesPerFrame)) {
    ++g_building_frame.dropped_batch_count;
    return;
  }
  g_building_frame.submitted_cursor_reset_count +=
      submitted_cursor_reset;
  g_building_frame.captured_vertex_bytes += byte_count;
  g_building_frame.batches.push_back(std::move(batch));
}

void LogFrame(const HudSwfFrameSnapshot &frame) {
  uint64_t vertex_count = 0;
  uint32_t texture_payload_count = 0;
  std::array<uint32_t, 6> texture_failure_counts{};
  const HudSwfTextureBindSnapshot *first_texture_failure = nullptr;
  for (const HudSwfVertexBatchSnapshot &batch : frame.batches) {
    vertex_count += batch.vertex_count;
  }
  for (const HudSwfTextureBindSnapshot &bind : frame.texture_binds) {
    texture_payload_count +=
        bind.texture != nullptr && bind.texture->valid();
    const size_t failure_index =
        static_cast<size_t>(bind.failure);
    if (failure_index < texture_failure_counts.size()) {
      ++texture_failure_counts[failure_index];
    }
    if (bind.failure != HudSwfTextureCaptureFailure::kNone &&
        first_texture_failure == nullptr) {
      first_texture_failure = &bind;
    }
  }
  const HudSwfTextureBindSnapshot empty_failure{};
  const HudSwfTextureBindSnapshot &failure =
      first_texture_failure != nullptr
          ? *first_texture_failure
          : empty_failure;
  REXLOG_INFO(
      "Table Tennis HUD/SWF observer: frame={} hud={:08X} context={:08X} "
      "scopes={}/{} batches={} vertices={} bytes={} texture_binds={} "
      "texture_payloads={} empty={} read_failures={} texture_failures={} "
      "texture_usage[required={} failed={} unreferenced={} missing={}] "
      "[unresolved={} read={} changed={} no_fetch={} snapshot={} "
      "first={:08X}/{:08X} vtable={:08X} binding={:08X}:{} "
      "words={:08X},{:08X},{:08X},{:08X}] "
      "submitted_cursor_resets={} invalid={}[cursor={} count={} range={} "
      "first={:08X}->{:08X} "
      "vertices={}/{}] dropped[batches={} textures={} "
      "scopes={}] complete={} observer_only=true guest_suppressed=false",
      frame.sequence, frame.hud, frame.swf_context,
      frame.completed_draw_scope_count, frame.draw_scope_count,
      frame.batches.size(), vertex_count, frame.captured_vertex_bytes,
      frame.texture_binds.size(), texture_payload_count,
      frame.empty_batch_count, frame.guest_read_failure_count,
      frame.texture_capture_failure_count,
      frame.required_texture_bind_count,
      frame.required_texture_capture_failure_count,
      frame.unreferenced_texture_bind_count,
      frame.missing_batch_texture_bind_count,
      texture_failure_counts[static_cast<size_t>(
          HudSwfTextureCaptureFailure::kResourceUnresolved)],
      texture_failure_counts[static_cast<size_t>(
          HudSwfTextureCaptureFailure::kBindingRead)],
      texture_failure_counts[static_cast<size_t>(
          HudSwfTextureCaptureFailure::kBindingChanged)],
      texture_failure_counts[static_cast<size_t>(
          HudSwfTextureCaptureFailure::kNoTextureFetch)],
      texture_failure_counts[static_cast<size_t>(
          HudSwfTextureCaptureFailure::kTextureSnapshot)],
      failure.requested_handle, failure.resolved_handle,
      failure.resource_vtable, failure.gpu_binding,
      static_cast<uint32_t>(failure.failure),
      failure.gpu_binding_words[0], failure.gpu_binding_words[1],
      failure.gpu_binding_words[2], failure.gpu_binding_words[3],
      frame.submitted_cursor_reset_count, frame.invalid_batch_count,
      frame.invalid_cursor_count, frame.invalid_vertex_count,
      frame.invalid_guest_range_count, frame.first_invalid_initial_base,
      frame.first_invalid_current_cursor,
      frame.first_invalid_vertex_count,
      frame.first_invalid_requested_count,
      frame.dropped_batch_count, frame.dropped_texture_bind_count,
      frame.dropped_scope_count, frame.valid());
}

} // namespace

bool HudSwfCaptureEnabled() {
  return REXCVAR_GET(tabletennis_native_hud_swf_capture) ||
         NativeFrameSceneCaptureEnabled();
}

void BeginHudSwfDrawScope(uint8_t *guest_base, uint32_t swf_context) {
  if (!HudSwfCaptureEnabled()) {
    return;
  }
  if (g_scope_depth == g_scope_stack.size()) {
    ++g_scope_overflow_depth;
    std::lock_guard lock(g_capture_mutex);
    ++g_building_frame.dropped_scope_count;
    return;
  }

  DrawScope &scope = g_scope_stack[g_scope_depth++];
  scope = {};

  uint32_t hud = 0;
  uint32_t live_swf_context = 0;
  if (!TryReadBeU32(guest_base, kHudGlobal, hud) || hud == 0 ||
      !TryReadBeU32AtOffset(guest_base, hud, kHudSwfContextOffset,
                            live_swf_context)) {
    RecordGuestReadFailure();
    return;
  }
  if (swf_context == 0 || swf_context != live_swf_context) {
    return;
  }

  scope.accepted = true;
  scope.hud = hud;
  scope.swf_context = swf_context;
  std::lock_guard lock(g_capture_mutex);
  ++g_building_frame.draw_scope_count;
  if (g_building_frame.hud == 0) {
    g_building_frame.hud = hud;
    g_building_frame.swf_context = swf_context;
  } else if (g_building_frame.hud != hud ||
             g_building_frame.swf_context != swf_context) {
    ++g_building_frame.invalid_batch_count;
  }
}

void EndHudSwfDrawScope(uint8_t *guest_base) {
  if (g_scope_overflow_depth != 0) {
    --g_scope_overflow_depth;
    return;
  }
  if (g_scope_depth == 0) {
    return;
  }

  DrawScope &scope = g_scope_stack[g_scope_depth - 1];
  if (scope.accepted) {
    // The title does not call another batch-start helper after its final HUD
    // primitive, so the SWF draw exit is the only exact completion boundary.
    FlushCompletedBatch(guest_base, scope);
    std::lock_guard lock(g_capture_mutex);
    ++g_building_frame.completed_draw_scope_count;
  }
  scope = {};
  --g_scope_depth;
}

void ObserveHudSwfTextureBind(uint8_t *guest_base,
                              uint32_t handle_or_zero) {
  DrawScope *scope = ActiveScope();
  if (scope == nullptr) {
    return;
  }

  uint32_t resolved_handle = handle_or_zero;
  if (resolved_handle == 0 &&
      !TryReadBeU32(guest_base, kDefaultTextureGlobal, resolved_handle)) {
    RecordGuestReadFailure();
    return;
  }

  HudSwfTextureBindSnapshot bind;
  bind.order = ++g_event_order;
  bind.requested_handle = handle_or_zero;
  bind.resolved_handle = resolved_handle;
  if (!TryReadBeU32(guest_base, resolved_handle,
                    bind.resource_vtable)) {
    bind.failure = HudSwfTextureCaptureFailure::kBindingRead;
  } else {
    if (!FindTextureResourceMapping(resolved_handle,
                                    bind.resource_vtable,
                                    bind.gpu_binding)) {
      ResolveKnownTextureXenonBinding(
          guest_base, resolved_handle, bind.resource_vtable,
          bind.gpu_binding);
    }
    CaptureTexturePayload(guest_base, scope->swf_context, bind);
  }
  scope->current_texture = resolved_handle;
  scope->current_texture_bind_order = bind.order;

  std::lock_guard lock(g_capture_mutex);
  if (bind.failure != HudSwfTextureCaptureFailure::kNone) {
    ++g_building_frame.texture_capture_failure_count;
  }
  if (g_building_frame.texture_binds.size() ==
      kMaximumTextureBindsPerFrame) {
    ++g_building_frame.dropped_texture_bind_count;
    return;
  }
  g_building_frame.texture_binds.push_back(bind);
}

void ObserveHudSwfTextureResourceUnwrap(uint8_t *guest_base,
                                        uint32_t resource,
                                        uint32_t gpu_binding) {
  if (!HudSwfCaptureEnabled() || guest_base == nullptr ||
      resource == 0 || gpu_binding == 0) {
    return;
  }

  TextureResourceMapping mapping;
  mapping.resource = resource;
  mapping.gpu_binding = gpu_binding;
  if (!TryReadBeU32(guest_base, resource,
                    mapping.resource_vtable) ||
      mapping.resource_vtable == 0) {
    return;
  }

  DrawScope *scope = ActiveScope();
  const uint32_t owner =
      scope == nullptr ? 0 : scope->swf_context;
  HudSwfTextureBindSnapshot resolved;
  resolved.resolved_handle = resource;
  resolved.resource_vtable = mapping.resource_vtable;
  resolved.gpu_binding = gpu_binding;
  if (owner != 0) {
    CaptureTexturePayload(guest_base, owner, resolved);
  }

  std::lock_guard lock(g_capture_mutex);
  StoreTextureResourceMappingLocked(mapping);
  if (owner == 0) {
    return;
  }
  for (HudSwfTextureBindSnapshot &bind :
       g_building_frame.texture_binds) {
    if (bind.resolved_handle != resource ||
        bind.failure !=
            HudSwfTextureCaptureFailure::kResourceUnresolved) {
      continue;
    }
    const uint64_t order = bind.order;
    const uint32_t requested_handle = bind.requested_handle;
    bind = resolved;
    bind.order = order;
    bind.requested_handle = requested_handle;
    if (bind.failure ==
        HudSwfTextureCaptureFailure::kNone) {
      --g_building_frame.texture_capture_failure_count;
    }
  }
}

void BeginHudSwfDynamicBatch(uint8_t *guest_base, uint32_t batch_kind,
                             uint32_t requested_vertex_count) {
  DrawScope *scope = ActiveScope();
  if (scope == nullptr) {
    return;
  }

  // At entry the global still describes the previous fully-written batch.
  FlushCompletedBatch(guest_base, *scope);
  scope->pending = {};
  scope->pending.order = ++g_event_order;
  scope->pending.batch_kind = batch_kind;
  scope->pending.requested_vertex_count = requested_vertex_count;
  scope->pending.texture_handle = scope->current_texture;
  scope->pending.texture_bind_order = scope->current_texture_bind_order;
}

void CompleteHudSwfDynamicBatchStart(uint8_t *guest_base) {
  DrawScope *scope = ActiveScope();
  if (scope == nullptr) {
    return;
  }

  uint32_t dynamic_base = 0;
  if (!TryReadBeU32(guest_base, kDynamicVertexBaseGlobal, dynamic_base)) {
    RecordGuestReadFailure();
    scope->pending = {};
    return;
  }
  if (dynamic_base == 0 ||
      scope->pending.requested_vertex_count > kMaximumVerticesPerBatch) {
    std::lock_guard lock(g_capture_mutex);
    ++g_building_frame.invalid_batch_count;
    scope->pending = {};
    return;
  }
  scope->pending.guest_base = dynamic_base;
  scope->pending.armed = true;
}

void HudSwfCaptureFrameEnd() {
  const bool enabled = HudSwfCaptureEnabled();
  std::lock_guard lock(g_capture_mutex);
  ++g_frame_sequence;
  if (!enabled) {
    g_building_frame = {};
    g_published_frame.reset();
    g_logged_first_nonempty_frame = false;
    g_texture_resource_mappings = {};
    g_texture_resource_mapping_count = 0;
    return;
  }

  if (g_scope_depth != 0 || g_scope_overflow_depth != 0) {
    g_building_frame.dropped_scope_count +=
        static_cast<uint32_t>(g_scope_depth + g_scope_overflow_depth);
    g_scope_stack = {};
    g_scope_depth = 0;
    g_scope_overflow_depth = 0;
  }

  g_building_frame.sequence = g_frame_sequence;
  ClassifyTextureBindUsage(g_building_frame);
  auto published =
      std::make_shared<HudSwfFrameSnapshot>(std::move(g_building_frame));
  g_building_frame = {};
  g_published_frame = std::move(published);

  const uint32_t interval =
      REXCVAR_GET(tabletennis_native_hud_swf_log_interval);
  const bool first_nonempty =
      g_published_frame->draw_scope_count != 0 &&
      !g_logged_first_nonempty_frame;
  if (g_published_frame->sequence % interval == 0 || first_nonempty) {
    LogFrame(*g_published_frame);
  }
  g_logged_first_nonempty_frame |=
      g_published_frame->draw_scope_count != 0;
}

std::shared_ptr<const HudSwfFrameSnapshot> LatestHudSwfFrameSnapshot() {
  std::lock_guard lock(g_capture_mutex);
  return g_published_frame;
}

} // namespace tabletennis::native
