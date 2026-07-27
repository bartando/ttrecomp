#pragma once

#include "native/tabletennis_native_scene_pass.h"

#include <array>
#include <cstdint>

namespace rex::graphics {
struct NativeGuestOutputRenderContext;
}

namespace tabletennis::native {

// The compositor supplies explicit clears when it opens the offscreen scene.
// The target owner does not invent title content or infer a background.
struct NativeScenePassClearValues {
  std::array<float, 4> color{};
  float depth = 1.0f;
};

enum class NativeSceneRenderTargetsResult : uint8_t {
  kSucceeded,
  kInvalidContext,
  kPassAlreadyOpen,
  kPassNotOpen,
  kInvalidTargets,
  kInvalidClearValues,
  kFourSampleUnsupported,
  kAttachmentCreationFailed,
  kResolveResourcesFailed,
  kResolvePreflightFailed,
};

// Allocates or reuses one output-sized RGBA8+D32 4x pair and the exact
// four-sample box-resolve resources. No commands are recorded and the guest
// output remains untouched.
NativeSceneRenderTargetsResult PrepareNativeSceneRenderTargets(
    const rex::graphics::NativeGuestOutputRenderContext &context,
    NativeScenePassTargets &targets_out);

// Opens the caller-owned offscreen pass. Families record between Begin and
// Resolve; they never own attachment transitions, binds or clears.
NativeSceneRenderTargetsResult BeginNativeSceneRenderPass(
    const rex::graphics::NativeGuestOutputRenderContext &context,
    const NativeScenePassTargets &targets,
    const NativeScenePassClearValues &clear_values);

// Discards an open offscreen pass after any family-record failure. This
// closes backend pass state and restores both owned attachments to their
// steady states without transitioning or writing the presenter.
NativeSceneRenderTargetsResult AbortNativeSceneRenderPass(
    const rex::graphics::NativeGuestOutputRenderContext &context);

// Averages all four RGBA8 samples into the one-sample presenter with a
// fullscreen shader, then restores both images to their steady states.
NativeSceneRenderTargetsResult ResolveNativeSceneRenderPass(
    const rex::graphics::NativeGuestOutputRenderContext &context,
    const NativeScenePassTargets &targets);

const char *
NativeSceneRenderTargetsResultName(NativeSceneRenderTargetsResult result);

void ShutdownNativeSceneRenderTargets();

} // namespace tabletennis::native
