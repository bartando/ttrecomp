#include "native/tabletennis_camera_observer.h"

#include "generated/default/tabletennis_init.h"
#include "native/tabletennis_guest_memory.h"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

#include <rex/logging.h>

namespace tabletennis::native {
namespace {

// sub_82153070 only uploads constants when its context matches this global.
constexpr uint32_t kMainRenderContextGlobal = 0x825EBAA0;
constexpr uint32_t kCameraBlockOffset = 0x80;
constexpr size_t kCameraBlockBytes = 0x200;

thread_local uint32_t g_latest_bound_context = 0;
bool g_announced_match = false;

bool CheckedAdd(uint32_t address, uint32_t offset, uint32_t& result) {
  if (address == 0 ||
      address > std::numeric_limits<uint32_t>::max() - offset) {
    return false;
  }
  result = address + offset;
  return true;
}

bool ReadBeU32(uint8_t* base, uint32_t address, uint32_t& value) {
  std::array<std::byte, sizeof(uint32_t)> bytes;
  if (base == nullptr ||
      !GuestTryCopy(bytes.data(), REX_RAW_ADDR(address), bytes.size())) {
    return false;
  }
  uint32_t encoded;
  std::memcpy(&encoded, bytes.data(), sizeof(encoded));
  value = std::byteswap(encoded);
  return true;
}

}  // namespace

void ObserveRenderContextCamera(uint8_t* base, uint32_t render_context) {
  (void)base;
  // sub_82152E80 is the actual c0-c19 upload. Remembering the context here is
  // deliberately the only hot-path work; the fault-guarded copy and matrix
  // validation are deferred until a proven table draw asks for them.
  g_latest_bound_context = render_context;
}

bool VerifyDrawCameraAgainstRenderContext(uint8_t* base,
                                          CapturedCamera& draw_camera) {
  uint32_t main_context = 0;
  if (g_latest_bound_context == 0 ||
      !ReadBeU32(base, kMainRenderContextGlobal, main_context) ||
      g_latest_bound_context != main_context) {
    return false;
  }

  uint32_t block_address;
  if (!CheckedAdd(g_latest_bound_context, kCameraBlockOffset,
                  block_address)) {
    return false;
  }
  std::array<std::byte, kCameraBlockBytes> bytes;
  if (!GuestTryCopy(bytes.data(), REX_RAW_ADDR(block_address),
                    bytes.size())) {
    return false;
  }

  CapturedCamera context_camera;
  if (!DecodeCameraRenderContext(bytes, g_latest_bound_context,
                                 context_camera) ||
      !CameraMatchesRenderContext(draw_camera, context_camera)) {
    return false;
  }

  draw_camera.source_render_context =
      context_camera.source_render_context;
  draw_camera.direct_context_verified = true;
  if (!g_announced_match) {
    g_announced_match = true;
    REXLOG_INFO(
        "Table Tennis camera observer: draw constants cross-validated against "
        "authoritative render context {:08X}; observer-only",
        context_camera.source_render_context);
  }
  return true;
}

}  // namespace tabletennis::native
