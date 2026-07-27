#pragma once

#include <cstdint>
#include <memory>

namespace rex::graphics {
struct NativeGuestOutputRenderContext;
}

namespace tabletennis::native {

struct NativeSceneDrawRef;
struct NativeScenePassTargets;
struct Venue14DFrameSnapshot;

enum class Venue14DNativeSceneRecordResult : uint8_t {
  kRecorded,
  kInvalidTarget,
  kWrongFamily,
  kInvalidFrame,
  kUnsupportedBackendContract,
  kResourcesNotPrepared,
  kDrawIndexOutOfRange,
  kOrdinalMismatch,
  kPreparedDrawMissing,
  kPreparedDrawIdentityMismatch,
  kUnsupportedPrimitive,
  kRhiDrawStateRejected,
};

// This renderer is deliberately an observer. It draws immutable, independently
// backend-verified 14D title snapshots over the untouched guest output and
// never registers a replacement route or suppresses a guest draw.
bool Venue14DRendererEnabled();

bool PrepareVenue14DObserver(
    const rex::graphics::NativeGuestOutputRenderContext &context,
    const std::shared_ptr<const Venue14DFrameSnapshot> &frame);

uint32_t RenderVenue14DObserver(
    const rex::graphics::NativeGuestOutputRenderContext &context,
    const std::shared_ptr<const Venue14DFrameSnapshot> &frame);

// Shared-pass observer adapter. Preparation uploads every immutable resource;
// recording emits exactly one original-ordinal draw and never mutates target
// state.
bool PrepareVenue14DNativeScene(
    const rex::graphics::NativeGuestOutputRenderContext &context,
    const NativeScenePassTargets &targets,
    const std::shared_ptr<const Venue14DFrameSnapshot> &frame);
Venue14DNativeSceneRecordResult RecordPreparedVenue14DNativeSceneDraw(
    const rex::graphics::NativeGuestOutputRenderContext &context,
    const NativeScenePassTargets &targets,
    const std::shared_ptr<const Venue14DFrameSnapshot> &frame,
    const NativeSceneDrawRef &draw);
const char *
Venue14DNativeSceneRecordResultName(Venue14DNativeSceneRecordResult result);

void ShutdownVenue14DRenderer();

} // namespace tabletennis::native
