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
struct PlayerSkinFrameSnapshot;
struct PlayerReplacementCandidate;

enum class PlayerNativeSceneRecordResult : uint8_t {
  kRecorded,
  kInvalidTarget,
  kWrongFamily,
  kInvalidFrame,
  kIncompletePhasePlan,
  kResourcesNotPrepared,
  kDrawIndexOutOfRange,
  kOrdinalMismatch,
  kPreparedDrawMissing,
  kPreparedDrawIdentityMismatch,
  kUnsupportedPrimitive,
  kRhiDrawStateRejected,
};

enum class PlayerReplacementPreflightResult : uint32_t {
  kSucceeded = 0,
  kSucceededCompatiblePreparedFrame,
  kWrongBackend,
  kMissingContext,
  kInvalidCandidate,
  kDeviceMismatch,
  kFrameMismatch,
  kMissingPipelineResources,
  kMissingDynamicBuffers,
  kPreparedDrawMissing,
  kPreparedDrawIdentityMismatch,
  kUnsupportedPrimitive,
  kRhiDrawStateRejected,
};

// The observer renderer is a comparison overlay only. Enabling it also asks
// the capture side to publish the immutable player frame it consumes.
bool PlayerObserverOverlayEnabled();

// Resource creation and guest-payload uploads happen before the output image
// becomes a render target. Rendering itself only binds prepared resources.
bool PreparePlayerObserverOverlay(
    const rex::graphics::NativeGuestOutputRenderContext &context,
    const std::shared_ptr<const PlayerSkinFrameSnapshot> &frame);
uint32_t RenderPlayerObserverOverlay(
    const rex::graphics::NativeGuestOutputRenderContext &context,
    const std::shared_ptr<const PlayerSkinFrameSnapshot> &frame);

// Shared-pass observer adapter. The exact frame must contain complete,
// original-order prepass/color pairs. Recording selects the matching phase
// pipeline for one NativeSceneDrawRef and never touches attachment state.
bool PreparePlayerNativeScene(
    const rex::graphics::NativeGuestOutputRenderContext &context,
    const NativeScenePassTargets &targets,
    const std::shared_ptr<const PlayerSkinFrameSnapshot> &frame);
PlayerNativeSceneRecordResult RecordPreparedPlayerNativeSceneDraw(
    const rex::graphics::NativeGuestOutputRenderContext &context,
    const NativeScenePassTargets &targets,
    const std::shared_ptr<const PlayerSkinFrameSnapshot> &frame,
    const NativeSceneDrawRef &draw);
const char *
PlayerNativeSceneRecordResultName(PlayerNativeSceneRecordResult result);

// Observer-only borrowed-scope warmup. Binds the candidate's exact prepared
// phase/resource tuple and resolves every fallible PSO/descriptor object, but
// never records a draw.
PlayerReplacementPreflightResult PreflightPlayerReplacementCandidate(
    const rex::graphics::NativeGuestDrawContext &context,
    const PlayerReplacementCandidate &candidate);
const char *
PlayerReplacementPreflightResultName(PlayerReplacementPreflightResult result);
bool PlayerReplacementPreflightSucceeded(
    PlayerReplacementPreflightResult result);

void ShutdownPlayerObserverRenderer();

} // namespace tabletennis::native
