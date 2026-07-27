#pragma once

#include <cstdint>
#include <memory>

namespace rex::graphics {
struct NativeGuestDrawContext;
struct NativeGuestOutputRenderContext;
} // namespace rex::graphics

namespace tabletennis::native {

struct CrowdFrameSnapshot;
struct CrowdReplacementCandidate;
struct NativeSceneDrawRef;
struct NativeScenePassTargets;

enum class CrowdNativeSceneRecordResult : uint8_t {
  kRecorded,
  kInvalidTarget,
  kWrongFamily,
  kInvalidFrame,
  kResourcesNotPrepared,
  kDrawIndexOutOfRange,
  kOrdinalMismatch,
  kPreparedDrawMissing,
  kPreparedDrawIdentityMismatch,
  kRhiDrawStateRejected,
};

enum class CrowdReplacementPreflightResult : uint32_t {
  kSucceeded = 0,
  kWrongBackend,
  kMissingContext,
  kInvalidCandidate,
  kDeviceMismatch,
  kFrameMismatch,
  kMissingPipelineResources,
  kMissingDynamicBuffers,
  kPreparedDrawMissing,
  kPreparedDrawIdentityMismatch,
  kRhiDrawStateRejected,
};

// Observer-only comparison renderer. It consumes only complete, verified
// fxCrowdGfx snapshots and never suppresses a title draw.
bool CrowdObserverOverlayEnabled();

// Resource creation and payload upload occur before the guest output is
// transitioned to a render target.
bool PrepareCrowdObserverOverlay(
    const rex::graphics::NativeGuestOutputRenderContext &context,
    const std::shared_ptr<const CrowdFrameSnapshot> &frame);

// Prewarm resources are independent of the observer geometry selector. The
// full proven title frame is prepared so every ordered backend candidate can
// resolve its exact draw tuple.
bool PrepareCrowdReplacementPrewarmResources(
    const rex::graphics::NativeGuestOutputRenderContext &context,
    const std::shared_ptr<const CrowdFrameSnapshot> &frame);

uint32_t RenderCrowdObserverOverlay(
    const rex::graphics::NativeGuestOutputRenderContext &context,
    const std::shared_ptr<const CrowdFrameSnapshot> &frame);

// Shared-pass observer adapter. Preparation owns immutable uploads and exact
// current-frame dynamic buffers. Recording consumes one original-ordinal C6
// draw without binding, clearing or transitioning the caller's attachments.
bool PrepareCrowdNativeScene(
    const rex::graphics::NativeGuestOutputRenderContext &context,
    const NativeScenePassTargets &targets,
    const std::shared_ptr<const CrowdFrameSnapshot> &frame);
CrowdNativeSceneRecordResult RecordPreparedCrowdNativeSceneDraw(
    const rex::graphics::NativeGuestOutputRenderContext &context,
    const NativeScenePassTargets &targets,
    const std::shared_ptr<const CrowdFrameSnapshot> &frame,
    const NativeSceneDrawRef &draw);
const char *
CrowdNativeSceneRecordResultName(CrowdNativeSceneRecordResult result);

// Binds only the exact borrowed RGBA8/D32S8 4x crowd PSO and the captured
// resource tuple for this ordered candidate. It resolves fallible state via
// PreflightDraw and never records a draw.
CrowdReplacementPreflightResult PreflightCrowdReplacementCandidate(
    const rex::graphics::NativeGuestDrawContext &context,
    const CrowdReplacementCandidate &candidate);
const char *
CrowdReplacementPreflightResultName(CrowdReplacementPreflightResult result);

// Records the exact draw whose state was resolved by the immediately
// preceding successful preflight. Returns true only after the checked indexed
// draw command is recorded.
bool DrawPreflightedCrowdReplacementCandidate(
    const rex::graphics::NativeGuestDrawContext &context,
    const CrowdReplacementCandidate &candidate);

void ShutdownCrowdObserverRenderer();

} // namespace tabletennis::native
