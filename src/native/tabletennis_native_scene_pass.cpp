#include "native/tabletennis_native_scene_pass.h"

#include <rex/graphics/native_guest_renderer.h>

namespace tabletennis::native {

NativeScenePassTargetValidation ValidateNativeScenePassTargets(
    const rex::graphics::NativeGuestOutputRenderContext &context,
    const NativeScenePassTargets &targets) {
  return ValidateNativeScenePassTargetsExact(
      context, targets, rex::graphics::nrhi::Format::kR8G8B8A8_UNORM,
      rex::graphics::nrhi::Format::kD32_FLOAT, 4);
}

NativeScenePassTargetValidation ValidateNativeScenePassTargetsExact(
    const rex::graphics::NativeGuestOutputRenderContext &context,
    const NativeScenePassTargets &targets,
    rex::graphics::nrhi::Format expected_color_format,
    rex::graphics::nrhi::Format expected_depth_format,
    uint32_t expected_sample_count) {
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
  if (expected_color_format == rex::graphics::nrhi::Format::kUnknown ||
      targets.color->format() != expected_color_format) {
    return NativeScenePassTargetValidation::kUnsupportedColorFormat;
  }
  if (expected_depth_format == rex::graphics::nrhi::Format::kUnknown ||
      targets.depth->format() != expected_depth_format) {
    return NativeScenePassTargetValidation::kUnsupportedDepthFormat;
  }
  if (expected_sample_count == 0 ||
      targets.sample_count != expected_sample_count ||
      targets.color->sample_count() != expected_sample_count ||
      targets.depth->sample_count() != expected_sample_count) {
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
