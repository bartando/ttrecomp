#pragma once

#include <cstdint>

#include <rex/graphics/native_rhi.h>

namespace rex::graphics {
struct NativeGuestOutputRenderContext;
}

namespace tabletennis::native {

// Caller-owned attachments for one native-scene command pass. Family
// recorders may bind pipelines and draw resources, but must never bind,
// transition or clear these targets.
struct NativeScenePassTargets {
  rex::graphics::nrhi::Texture *color = nullptr;
  rex::graphics::nrhi::Texture *depth = nullptr;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t sample_count = 0;
};

enum class NativeScenePassTargetValidation : uint8_t {
  kValid,
  kMissingContext,
  kColorIsPresenterOutput,
  kMissingDepth,
  kExtentMismatch,
  kUnsupportedColorFormat,
  kUnsupportedDepthFormat,
  kUnsupportedSampleCount,
  kPresenterIsMultisampled,
};

// The shared scene reproduces the title's common MAIN attachment shape:
// caller-owned 4x RGBA8 plus matching 4x D32. The presenter stays separate
// until the caller explicitly resolves this pass.
NativeScenePassTargetValidation ValidateNativeScenePassTargets(
    const rex::graphics::NativeGuestOutputRenderContext &context,
    const NativeScenePassTargets &targets);

const char *
NativeScenePassTargetValidationName(NativeScenePassTargetValidation validation);

} // namespace tabletennis::native
