#include "native/tabletennis_draw_constants.h"

#include "generated/default/tabletennis_init.h"
#include "native/tabletennis_camera_observer.h"
#include "native/tabletennis_guest_memory.h"
#include "native/tabletennis_material_observer.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_UINT32(
    tabletennis_native_constant_dump_rows, 0, "Table Tennis",
    "Dump this many rows from the final vertex-constant bank for the first "
    "observed table draw (0 disables, maximum 256).")
    .range(0, 256)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace tabletennis::native {
namespace {

// sub_82363C00 flushes dirty bank 0x4000 from device+1920 immediately before
// every draw. Each bank row is one float4 and the complete bank is 256 rows.
constexpr uint32_t kVertexConstantBankOffset = 1920;
constexpr size_t kConstantRowBytes = sizeof(uint32_t) * 4;
constexpr size_t kConstantRowCount = 256;
constexpr size_t kConstantBankBytes =
    kConstantRowBytes * kConstantRowCount;
constexpr size_t kMaxScopeDepth = 8;
constexpr uint32_t kTriangleListPrimitive = 4;
constexpr uint32_t kNetIndexCount = 4680;

struct ModelDrawScope {
  bool observed_table_model = false;
  uint32_t model = 0;
  uint32_t shader_group = 0;
};

thread_local std::array<ModelDrawScope, kMaxScopeDepth> g_scope_stack;
thread_local size_t g_scope_depth = 0;
std::atomic<bool> g_dump_claimed = false;
std::atomic<uint64_t> g_camera_generation = 0;
std::mutex g_camera_mutex;
CapturedCamera g_frame_camera;
uint32_t g_frame_verified_candidates = 0;

uint32_t LoadBeU32(const std::byte* source) {
  uint32_t value;
  std::memcpy(&value, source, sizeof(value));
  return std::byteswap(value);
}

float LoadBeF32(const std::byte* source) {
  return std::bit_cast<float>(LoadBeU32(source));
}

const ModelDrawScope* CurrentScope() {
  if (g_scope_depth == 0 || g_scope_depth > g_scope_stack.size()) {
    return nullptr;
  }
  return &g_scope_stack[g_scope_depth - 1];
}

}  // namespace

void BeginModelDrawScope(bool observed_table_model, uint32_t model,
                         uint32_t shader_group) {
  if (g_scope_depth < g_scope_stack.size()) {
    g_scope_stack[g_scope_depth] = {
        observed_table_model, model, shader_group};
  }
  ++g_scope_depth;
}

void EndModelDrawScope() {
  if (g_scope_depth != 0) {
    --g_scope_depth;
  }
}

bool CurrentObservedTableModelDraw(uint32_t model, uint32_t& shader_group) {
  const ModelDrawScope* scope = CurrentScope();
  if (scope == nullptr || !scope->observed_table_model ||
      scope->model != model) {
    return false;
  }
  shader_group = scope->shader_group;
  return true;
}

void ObserveIndexedDrawConstants(uint8_t* guest_base, uint32_t device,
                                 uint32_t primitive_type,
                                 uint32_t submitted_index_count) {
  const uint32_t requested_rows =
      REXCVAR_GET(tabletennis_native_constant_dump_rows);
  const ModelDrawScope* scope = CurrentScope();
  if (scope == nullptr || !scope->observed_table_model ||
      guest_base == nullptr || device == 0 ||
      !CurrentTableVisibleMaterialPass() ||
      primitive_type != kTriangleListPrimitive ||
      submitted_index_count != kNetIndexCount ||
      device > std::numeric_limits<uint32_t>::max() -
                   kVertexConstantBankOffset) {
    return;
  }

  const bool should_dump =
      requested_rows != 0 && !g_dump_claimed.exchange(true);
  std::array<std::byte, kConstantBankBytes> bank;
  const uint32_t bank_address = device + kVertexConstantBankOffset;
  const void* bank_host_address =
      guest_base + bank_address + REX_PHYS_HOST_OFFSET(bank_address);
  const size_t row_count =
      std::min<size_t>(requested_rows, kConstantRowCount);
  const size_t camera_bytes = 20 * kConstantRowBytes;
  const size_t read_bytes =
      std::max(camera_bytes, row_count * kConstantRowBytes);
  if (!GuestTryCopy(bank.data(), bank_host_address, read_bytes)) {
    if (should_dump) {
      g_dump_claimed.store(false);
    }
    REXLOG_WARN(
        "Table Tennis constant observer: draw-time bank read failed "
        "device={:08X} bank={:08X}; will retry",
        device, bank_address);
    return;
  }

  CapturedCamera camera;
  if (DecodeCameraConstantBank(
          std::span<const std::byte>(bank.data(), read_bytes), scope->model,
          scope->shader_group, submitted_index_count, camera) &&
      VerifyDrawCameraAgainstRenderContext(guest_base, camera)) {
    camera.constant_generation = g_camera_generation.fetch_add(1) + 1;
    std::lock_guard lock(g_camera_mutex);
    camera.verified_candidates = ++g_frame_verified_candidates;
    // The visible net pass is submitted twice per swap. Live traces prove
    // that the first submission owns the aligned scene camera, while the
    // later submission uses a different constant-bank state that projects
    // the net into a screen-filling sheet. Preserve the first verified
    // camera, but keep its candidate count current for telemetry.
    if (!g_frame_camera.valid) {
      g_frame_camera = camera;
    } else {
      g_frame_camera.verified_candidates = g_frame_verified_candidates;
    }
  }

  if (should_dump) {
    REXLOG_INFO(
        "Table Tennis constant observer: model={:08X} shader_group={:08X} "
        "device={:08X} bank={:08X} primitive={} submitted_indices={} "
        "rows={} observer_only=true",
        scope->model, scope->shader_group, device, bank_address, primitive_type,
        submitted_index_count, row_count);
    for (size_t row = 0; row < row_count; ++row) {
      const std::byte* source = bank.data() + row * kConstantRowBytes;
      REXLOG_INFO(
          "  vc[{:03}] = [{: .7g} {: .7g} {: .7g} {: .7g}] "
          "bits=[{:08X} {:08X} {:08X} {:08X}]",
          row, LoadBeF32(source + 0), LoadBeF32(source + 4),
          LoadBeF32(source + 8), LoadBeF32(source + 12),
          LoadBeU32(source + 0), LoadBeU32(source + 4),
          LoadBeU32(source + 8), LoadBeU32(source + 12));
    }
  }
}

CapturedCamera ConsumeObservedCamera() {
  CapturedCamera camera;
  {
    std::lock_guard lock(g_camera_mutex);
    camera = g_frame_camera;
    g_frame_camera = {};
    g_frame_verified_candidates = 0;
  }
  return camera;
}

}  // namespace tabletennis::native
