#include "native/tabletennis_exact_main_targets.h"

#include "native/tabletennis_offscreen_target_owner.h"

#include <rex/graphics/native_guest_renderer.h>

namespace tabletennis::native {
namespace {

namespace nrhi = rex::graphics::nrhi;

OffscreenTargetOwner g_exact_main_owner;

bool IsPackedDepthStencilFormat(nrhi::Format format) {
  return format == nrhi::Format::kD24_UNORM_S8_UINT ||
         format == nrhi::Format::kD32_FLOAT_S8_UINT;
}

} // namespace

bool ExactMainTargetContract::valid() const {
  return IsPackedDepthStencilFormat(depth_stencil_format) &&
         sample_count == 4 && sample_mask != 0;
}

bool ExactMainPreparedTargets::valid(
    const rex::graphics::NativeGuestOutputRenderContext &context) const {
  return contract.valid() &&
         ValidateNativeScenePassTargetsExact(
             context, attachments, nrhi::Format::kR8G8B8A8_UNORM,
             contract.depth_stencil_format, contract.sample_count) ==
             NativeScenePassTargetValidation::kValid;
}

ExactMainTargetsResult PrepareExactMainTargets(
    const rex::graphics::NativeGuestOutputRenderContext &context,
    const ExactMainTargetContract &contract,
    ExactMainPreparedTargets &targets_out) {
  targets_out = {};
  if (context.device == nullptr || context.cmd == nullptr ||
      context.guest_output == nullptr) {
    return ExactMainTargetsResult::kInvalidContext;
  }
  if (!contract.valid()) {
    return ExactMainTargetsResult::kInvalidContract;
  }

  NativeScenePassTargets attachments;
  const OffscreenTargetOwnerResult owner_result = g_exact_main_owner.Prepare(
      context,
      {
          .color_format = nrhi::Format::kR8G8B8A8_UNORM,
          .depth_format = contract.depth_stencil_format,
          .sample_count = contract.sample_count,
      },
      attachments);
  switch (owner_result) {
  case OffscreenTargetOwnerResult::kSucceeded:
    break;
  case OffscreenTargetOwnerResult::kInvalidContext:
    return ExactMainTargetsResult::kInvalidContext;
  case OffscreenTargetOwnerResult::kPassAlreadyOpen:
    return ExactMainTargetsResult::kPassAlreadyOpen;
  case OffscreenTargetOwnerResult::kSampleCountUnsupported:
    return ExactMainTargetsResult::kSampleCountUnsupported;
  case OffscreenTargetOwnerResult::kAttachmentCreationFailed:
    return ExactMainTargetsResult::kAttachmentCreationFailed;
  default:
    return ExactMainTargetsResult::kInvalidTargets;
  }

  targets_out = {
      .attachments = attachments,
      .contract = contract,
  };
  if (!targets_out.valid(context)) {
    targets_out = {};
    return ExactMainTargetsResult::kInvalidTargets;
  }
  return ExactMainTargetsResult::kSucceeded;
}

ExactMainTargetsResult PrepareExactMainTargetsForPrivateResolve(
    const rex::graphics::NativeGuestOutputRenderContext &context,
    const ExactMainPreparedTargets &targets,
    rex::graphics::nrhi::TextureView *&source_out) {
  source_out = nullptr;
  if (!targets.valid(context)) {
    return ExactMainTargetsResult::kInvalidTargets;
  }
  const OffscreenTargetOwnerResult result =
      g_exact_main_owner.PrepareExternalReplayForResolve(context,
                                                         targets.attachments);
  if (result == OffscreenTargetOwnerResult::kInvalidContext) {
    return ExactMainTargetsResult::kInvalidContext;
  }
  if (result == OffscreenTargetOwnerResult::kPassAlreadyOpen) {
    return ExactMainTargetsResult::kPassAlreadyOpen;
  }
  if (result != OffscreenTargetOwnerResult::kSucceeded ||
      g_exact_main_owner.resolve_source() == nullptr) {
    return ExactMainTargetsResult::kInvalidTargets;
  }
  source_out = g_exact_main_owner.resolve_source();
  return ExactMainTargetsResult::kSucceeded;
}

void RestoreExactMainTargetsAfterPrivateResolve(
    const rex::graphics::NativeGuestOutputRenderContext &context) {
  g_exact_main_owner.RestoreAfterResolve(context);
}

const char *ExactMainTargetsResultName(ExactMainTargetsResult result) {
  switch (result) {
  case ExactMainTargetsResult::kSucceeded:
    return "succeeded";
  case ExactMainTargetsResult::kInvalidContext:
    return "invalid_context";
  case ExactMainTargetsResult::kInvalidContract:
    return "invalid_contract";
  case ExactMainTargetsResult::kPassAlreadyOpen:
    return "pass_already_open";
  case ExactMainTargetsResult::kSampleCountUnsupported:
    return "sample_count_unsupported";
  case ExactMainTargetsResult::kAttachmentCreationFailed:
    return "attachment_creation_failed";
  case ExactMainTargetsResult::kInvalidTargets:
    return "invalid_targets";
  }
  return "unknown";
}

void ShutdownExactMainTargets() { g_exact_main_owner.Shutdown(); }

} // namespace tabletennis::native
