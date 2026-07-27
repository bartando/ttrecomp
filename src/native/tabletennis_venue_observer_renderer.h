#pragma once

#include <cstdint>
#include <memory>

namespace rex::graphics {
struct NativeGuestDrawContext;
struct NativeGuestOutputRenderContext;
} // namespace rex::graphics

namespace tabletennis::native {

struct NativeSceneDrawRef;
struct NativeScenePassTargets;
struct VenueFrameSnapshot;
struct VenueFullFamilyFrame;

enum class VenueNativeSceneRecordResult : uint8_t {
  kRecorded,
  kInvalidTarget,
  kWrongFamily,
  kInvalidFrame,
  kResourcesNotPrepared,
  kDrawIndexOutOfRange,
  kOrdinalMismatch,
  kUnsupportedPrimitive,
  kMissingTexture,
  kRhiDrawStateRejected,
};

// Draws the trace-verified venue slice with its captured geometry, textures,
// constants and shader equation. The guest frame remains authoritative.
uint32_t RenderVenueObserver(
    const rex::graphics::NativeGuestOutputRenderContext &context,
    const std::shared_ptr<const VenueFrameSnapshot> &frame);

// Full-family resources are prepared in the end-of-frame output callback,
// never while borrowing one of the title's guest render passes.
bool PrepareVenueFullFamilyOverlay(
    const rex::graphics::NativeGuestOutputRenderContext &context,
    const std::shared_ptr<const VenueFullFamilyFrame> &frame);
uint32_t RenderVenueFullFamilyOverlay(
    const rex::graphics::NativeGuestOutputRenderContext &context,
    const std::shared_ptr<const VenueFullFamilyFrame> &frame);

// Shared-pass phase one. Preparation may allocate and upload, so it must run
// before the caller binds NativeScenePassTargets. Recording consumes exactly
// one original-ordinal PS328 draw and never binds, clears or transitions the
// caller-owned color/depth attachments.
bool PrepareVenueFullFamilyNativeScene(
    const rex::graphics::NativeGuestOutputRenderContext &context,
    const NativeScenePassTargets &targets,
    const std::shared_ptr<const VenueFullFamilyFrame> &frame);
VenueNativeSceneRecordResult RecordPreparedVenueNativeSceneDraw(
    const rex::graphics::NativeGuestOutputRenderContext &context,
    const NativeScenePassTargets &targets,
    const std::shared_ptr<const VenueFullFamilyFrame> &frame,
    const NativeSceneDrawRef &draw);
const char *
VenueNativeSceneRecordResultName(VenueNativeSceneRecordResult result);

// Uploads immutable venue geometry/textures and creates the exact-state
// pipeline outside the guest render pass. Replacement callbacks must never
// allocate, copy or transition resources while borrowing the game pass.
bool PrepareVenueReplacement(
    const rex::graphics::NativeGuestOutputRenderContext &context,
    const std::shared_ptr<const VenueFrameSnapshot> &frame);

bool MatchVenueReplacement(const rex::graphics::NativeGuestDrawContext &context,
                           void *user_data);
bool RenderVenueReplacement(
    const rex::graphics::NativeGuestDrawContext &context, void *user_data);

void ShutdownVenueObserverRenderer();

} // namespace tabletennis::native
