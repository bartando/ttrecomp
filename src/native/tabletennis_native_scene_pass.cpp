#include "native/tabletennis_native_scene_pass.h"

#include <rex/graphics/native_guest_renderer.h>

namespace tabletennis::native {

NativeScenePassTargetValidation ValidateNativeScenePassTargets(
    const rex::graphics::NativeGuestOutputRenderContext &context,
    const NativeScenePassTargets &targets) {
  if (context.device == nullptr || context.cmd == nullptr ||
      context.guest_output == nullptr) {
    return NativeScenePassTargetValidation::kMissingContext;
  }
  if (targets.color == nullptr || targets.color == context.guest_output) {
    return NativeScenePassTargetValidation::kColorIsPresenterOutput;
  }
  if (targets.depth == nullptr) {
    return NativeScenePassTargetValidation::kMissingDepth;
  }
  if (targets.width == 0 || targets.height == 0 ||
      targets.width != context.guest_output_width ||
      targets.height != context.guest_output_height ||
      targets.color->width() != targets.width ||
      targets.color->height() != targets.height ||
      targets.depth->width() != targets.width ||
      targets.depth->height() != targets.height) {
    return NativeScenePassTargetValidation::kExtentMismatch;
  }
  if (targets.color->format() != rex::graphics::nrhi::Format::kR8G8B8A8_UNORM) {
    return NativeScenePassTargetValidation::kUnsupportedColorFormat;
  }
  if (targets.depth->format() != rex::graphics::nrhi::Format::kD32_FLOAT) {
    return NativeScenePassTargetValidation::kUnsupportedDepthFormat;
  }
  if (targets.sample_count != 4 || targets.color->sample_count() != 4 ||
      targets.depth->sample_count() != 4) {
    return NativeScenePassTargetValidation::kUnsupportedSampleCount;
  }
  if (context.guest_output->sample_count() != 1) {
    return NativeScenePassTargetValidation::kPresenterIsMultisampled;
  }
  return NativeScenePassTargetValidation::kValid;
}

const char *NativeScenePassTargetValidationName(
    NativeScenePassTargetValidation validation) {
  switch (validation) {
  case NativeScenePassTargetValidation::kValid:
    return "valid";
  case NativeScenePassTargetValidation::kMissingContext:
    return "missing_context";
  case NativeScenePassTargetValidation::kColorIsPresenterOutput:
    return "color_is_presenter_output";
  case NativeScenePassTargetValidation::kMissingDepth:
    return "missing_depth";
  case NativeScenePassTargetValidation::kExtentMismatch:
    return "extent_mismatch";
  case NativeScenePassTargetValidation::kUnsupportedColorFormat:
    return "unsupported_color_format";
  case NativeScenePassTargetValidation::kUnsupportedDepthFormat:
    return "unsupported_depth_format";
  case NativeScenePassTargetValidation::kUnsupportedSampleCount:
    return "unsupported_sample_count";
  case NativeScenePassTargetValidation::kPresenterIsMultisampled:
    return "presenter_is_multisampled";
  }
  return "unknown";
}

} // namespace tabletennis::native
