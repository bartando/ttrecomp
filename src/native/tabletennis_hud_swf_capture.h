#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace tabletennis::native {

struct TextureSnapshot;

enum class HudSwfTextureCaptureFailure : uint8_t {
  kNone = 0,
  kResourceUnresolved = 1,
  kBindingRead = 2,
  kBindingChanged = 3,
  kNoTextureFetch = 4,
  kTextureSnapshot = 5,
};

// One ordered texture-selection event from the title's gameplay HUD SWF
// renderer. The handle is the title's GPU-binding pointer; the embedded Xenos
// fetch and immutable mip-0 payload are copied before publication.
struct HudSwfTextureBindSnapshot {
  uint64_t order = 0;
  uint32_t requested_handle = 0;
  uint32_t resolved_handle = 0;
  uint32_t resource_vtable = 0;
  uint32_t gpu_binding = 0;
  uint32_t texture_fetch_offset = 0;
  HudSwfTextureCaptureFailure failure =
      HudSwfTextureCaptureFailure::kNone;
  std::array<uint32_t, 13> gpu_binding_words{};
  std::array<uint32_t, 6> texture_fetch_words{};
  std::shared_ptr<const TextureSnapshot> texture;
};

// One completed dynamic SWF vertex batch. The 36-byte vertices are retained
// byte-for-byte in guest order, with no pointer into streaming guest memory.
struct HudSwfVertexBatchSnapshot {
  static constexpr uint32_t kVertexStride = 0x24;

  uint64_t order = 0;
  uint64_t texture_bind_order = 0;
  uint32_t swf_context = 0;
  uint32_t texture_handle = 0;
  uint32_t batch_kind = 0;
  uint32_t requested_vertex_count = 0;
  uint32_t vertex_count = 0;
  std::vector<std::byte> vertex_bytes;
};

// Immutable publication made at the title's Swap boundary. A frame is
// complete only when every accepted gameplay-HUD SWF scope closed and all
// non-empty batches were copied without a guest fault or capacity drop.
struct HudSwfFrameSnapshot {
  uint64_t sequence = 0;
  uint32_t hud = 0;
  uint32_t swf_context = 0;
  uint32_t draw_scope_count = 0;
  uint32_t completed_draw_scope_count = 0;
  uint32_t empty_batch_count = 0;
  uint32_t guest_read_failure_count = 0;
  uint32_t texture_capture_failure_count = 0;
  uint32_t required_texture_bind_count = 0;
  uint32_t required_texture_capture_failure_count = 0;
  uint32_t unreferenced_texture_bind_count = 0;
  uint32_t missing_batch_texture_bind_count = 0;
  uint32_t invalid_batch_count = 0;
  uint32_t invalid_cursor_count = 0;
  uint32_t invalid_vertex_count = 0;
  uint32_t invalid_guest_range_count = 0;
  uint32_t submitted_cursor_reset_count = 0;
  uint32_t first_invalid_initial_base = 0;
  uint32_t first_invalid_current_cursor = 0;
  uint32_t first_invalid_vertex_count = 0;
  uint32_t first_invalid_requested_count = 0;
  uint32_t dropped_batch_count = 0;
  uint32_t dropped_texture_bind_count = 0;
  uint32_t dropped_scope_count = 0;
  size_t captured_vertex_bytes = 0;
  std::vector<HudSwfTextureBindSnapshot> texture_binds;
  std::vector<HudSwfVertexBatchSnapshot> batches;

  bool valid() const {
    return draw_scope_count != 0 && !batches.empty() &&
           completed_draw_scope_count == draw_scope_count &&
           guest_read_failure_count == 0 &&
           required_texture_capture_failure_count == 0 &&
           missing_batch_texture_bind_count == 0 &&
           invalid_batch_count == 0 &&
           dropped_batch_count == 0 && dropped_texture_bind_count == 0 &&
           dropped_scope_count == 0;
  }
};

// Default-off observer. The full immutable native-frame observer also enables
// this capture, but neither route serves or suppresses a guest draw.
bool HudSwfCaptureEnabled();

// sub_823F9238 entry/exit. Only r3 equal to
// BE32(BE32(0x82606610) + 0x5C) is accepted as gameplay HUD content.
void BeginHudSwfDrawScope(uint8_t *guest_base, uint32_t swf_context);
void EndHudSwfDrawScope(uint8_t *guest_base);

// sub_822EC298 entry. A zero handle resolves through BE32(0x825EBA20).
void ObserveHudSwfTextureBind(uint8_t *guest_base, uint32_t handle_or_zero);

// grcTextureReference/grcTextureXenon::GetGpuBinding exit. The SWF setter
// stores a resource object, and the title later resolves that object to the
// binding containing the Xenos fetch. This exact join avoids interpreting the
// resource object's vtable and fields as texture-fetch words.
void ObserveHudSwfTextureResourceUnwrap(uint8_t *guest_base,
                                        uint32_t resource,
                                        uint32_t gpu_binding);

// sub_82152A78 entry/exit. Entry finalizes the preceding batch before the
// title overwrites its dynamic-buffer globals. Exit records the initial write
// cursor at BE32(0x825EBA74). The title advances that cursor by 36 bytes per
// emitted vertex and records the count at BE32(0x825EBA88). Submission clears
// the cursor to zero but deliberately retains the count, so capture accepts
// either the live end cursor or that exact post-submit state before copying
// the original byte range at the next start or SWF draw exit.
void BeginHudSwfDynamicBatch(uint8_t *guest_base, uint32_t batch_kind,
                             uint32_t requested_vertex_count);
void CompleteHudSwfDynamicBatchStart(uint8_t *guest_base);

void HudSwfCaptureFrameEnd();
std::shared_ptr<const HudSwfFrameSnapshot> LatestHudSwfFrameSnapshot();

} // namespace tabletennis::native
