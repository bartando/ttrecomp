#include "native/tabletennis_offscreen_target_owner.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <ranges>

#include <rex/graphics/native_guest_renderer.h>

namespace tabletennis::native {
namespace {

namespace nrhi = rex::graphics::nrhi;

void SetFullOutputArea(nrhi::Cmd *cmd, uint32_t width, uint32_t height) {
  nrhi::Viewport viewport;
  viewport.width = static_cast<float>(width);
  viewport.height = static_cast<float>(height);
  cmd->SetViewport(viewport);
  nrhi::Rect scissor;
  scissor.right = static_cast<int32_t>(width);
  scissor.bottom = static_cast<int32_t>(height);
  cmd->SetScissor(scissor);
}

bool ValidClearValues(const std::array<float, 4> &color, float depth) {
  return std::ranges::all_of(
             color, [](float value) { return std::isfinite(value); }) &&
         std::isfinite(depth) && depth >= 0.0f && depth <= 1.0f;
}

} // namespace

bool OffscreenTargetOwner::EnsureContext(
    const rex::graphics::NativeGuestOutputRenderContext &context) {
  if (context.device == nullptr || context.cmd == nullptr ||
      context.guest_output == nullptr || context.guest_output_width == 0 ||
      context.guest_output_height == 0 ||
      context.guest_output_width >
          static_cast<uint32_t>(std::numeric_limits<int32_t>::max()) ||
      context.guest_output_height >
          static_cast<uint32_t>(std::numeric_limits<int32_t>::max()) ||
      context.guest_output->sample_count() != 1) {
    return false;
  }
  if (device_ != nullptr && device_ != context.device) {
    // The prior command processor already owns destruction of its resources.
    device_ = nullptr;
    color_ = nullptr;
    depth_ = nullptr;
    resolve_source_ = nullptr;
    desc_ = {};
    width_ = 0;
    height_ = 0;
    state_ = State::kUnavailable;
  }
  device_ = context.device;
  return true;
}

void OffscreenTargetOwner::ReleaseResources() {
  if (device_ != nullptr) {
    device_->DestroyDeferred(resolve_source_);
    device_->DestroyDeferred(color_);
    device_->DestroyDeferred(depth_);
  }
  color_ = nullptr;
  depth_ = nullptr;
  resolve_source_ = nullptr;
  desc_ = {};
  width_ = 0;
  height_ = 0;
  state_ = State::kUnavailable;
}

OffscreenTargetOwnerResult OffscreenTargetOwner::Prepare(
    const rex::graphics::NativeGuestOutputRenderContext &context,
    const OffscreenTargetDesc &desc, NativeScenePassTargets &targets_out) {
  targets_out = {};
  if (!EnsureContext(context) ||
      desc.color_format == nrhi::Format::kUnknown ||
      desc.depth_format == nrhi::Format::kUnknown ||
      desc.sample_count == 0) {
    return OffscreenTargetOwnerResult::kInvalidContext;
  }
  if (state_ == State::kOpen || state_ == State::kResolving) {
    return OffscreenTargetOwnerResult::kPassAlreadyOpen;
  }
  if (context.device->GetSupportedSampleCount(desc.color_format,
                                               desc.sample_count) !=
          desc.sample_count ||
      context.device->GetSupportedSampleCount(desc.depth_format,
                                               desc.sample_count) !=
          desc.sample_count) {
    return OffscreenTargetOwnerResult::kSampleCountUnsupported;
  }

  const bool attachments_match =
      color_ != nullptr && depth_ != nullptr && resolve_source_ != nullptr &&
      desc_.color_format == desc.color_format &&
      desc_.depth_format == desc.depth_format &&
      desc_.sample_count == desc.sample_count &&
      width_ == context.guest_output_width &&
      height_ == context.guest_output_height;
  if (!attachments_match) {
    nrhi::TextureDesc color;
    color.width = context.guest_output_width;
    color.height = context.guest_output_height;
    color.sample_count = desc.sample_count;
    color.format = desc.color_format;
    color.usage = nrhi::kTextureUsageRenderTarget;
    color.initial_state = nrhi::ResourceState::kRenderTarget;
    nrhi::Texture *const new_color = context.device->CreateTexture(color);

    nrhi::TextureDesc depth;
    depth.width = context.guest_output_width;
    depth.height = context.guest_output_height;
    depth.sample_count = desc.sample_count;
    depth.format = desc.depth_format;
    depth.usage = nrhi::kTextureUsageDepthStencil;
    depth.initial_state = nrhi::ResourceState::kDepthWrite;
    depth.clear_depth = 1.0f;
    nrhi::Texture *const new_depth = context.device->CreateTexture(depth);
    if (new_color == nullptr || new_depth == nullptr) {
      context.device->DestroyDeferred(new_color);
      context.device->DestroyDeferred(new_depth);
      return OffscreenTargetOwnerResult::kAttachmentCreationFailed;
    }

    nrhi::TextureViewDesc resolve_view;
    resolve_view.dimension = nrhi::ViewDimension::k2DMS;
    resolve_view.mip_levels = 1;
    nrhi::TextureView *const new_resolve_source =
        context.device->CreateTextureView(new_color, resolve_view);
    if (new_resolve_source == nullptr) {
      context.device->DestroyDeferred(new_color);
      context.device->DestroyDeferred(new_depth);
      return OffscreenTargetOwnerResult::kAttachmentCreationFailed;
    }

    context.device->DestroyDeferred(resolve_source_);
    context.device->DestroyDeferred(color_);
    context.device->DestroyDeferred(depth_);
    color_ = new_color;
    depth_ = new_depth;
    resolve_source_ = new_resolve_source;
    desc_ = desc;
    width_ = context.guest_output_width;
    height_ = context.guest_output_height;
  }

  targets_out = {
      .color = color_,
      .depth = depth_,
      .width = width_,
      .height = height_,
      .sample_count = desc_.sample_count,
  };
  if (ValidateNativeScenePassTargetsExact(
          context, targets_out, desc_.color_format, desc_.depth_format,
          desc_.sample_count) != NativeScenePassTargetValidation::kValid) {
    targets_out = {};
    return OffscreenTargetOwnerResult::kInvalidTargets;
  }
  state_ = State::kReady;
  return OffscreenTargetOwnerResult::kSucceeded;
}

bool OffscreenTargetOwner::ExactTargets(
    const NativeScenePassTargets &targets) const {
  return targets.color == color_ && targets.depth == depth_ &&
         targets.width == width_ && targets.height == height_ &&
         targets.sample_count == desc_.sample_count;
}

OffscreenTargetOwnerResult OffscreenTargetOwner::Begin(
    const rex::graphics::NativeGuestOutputRenderContext &context,
    const NativeScenePassTargets &targets,
    const std::array<float, 4> &clear_color, float clear_depth) {
  if (!EnsureContext(context)) {
    return OffscreenTargetOwnerResult::kInvalidContext;
  }
  if (state_ == State::kOpen || state_ == State::kResolving) {
    return OffscreenTargetOwnerResult::kPassAlreadyOpen;
  }
  if (state_ != State::kReady || !ExactTargets(targets) ||
      ValidateNativeScenePassTargetsExact(
          context, targets, desc_.color_format, desc_.depth_format,
          desc_.sample_count) != NativeScenePassTargetValidation::kValid) {
    return OffscreenTargetOwnerResult::kInvalidTargets;
  }
  if (!ValidClearValues(clear_color, clear_depth)) {
    return OffscreenTargetOwnerResult::kInvalidClearValues;
  }
  SetFullOutputArea(context.cmd, width_, height_);
  context.cmd->SetRenderTargets(color_, depth_);
  context.cmd->ClearRenderTarget(color_, clear_color.data());
  context.cmd->ClearDepth(depth_, clear_depth);
  state_ = State::kOpen;
  return OffscreenTargetOwnerResult::kSucceeded;
}

OffscreenTargetOwnerResult OffscreenTargetOwner::Abort(
    const rex::graphics::NativeGuestOutputRenderContext &context) {
  if (!EnsureContext(context)) {
    return OffscreenTargetOwnerResult::kInvalidContext;
  }
  if (state_ != State::kOpen) {
    return OffscreenTargetOwnerResult::kPassNotOpen;
  }
  context.cmd->Barrier(color_, nrhi::ResourceState::kRenderTarget,
                       nrhi::ResourceState::kPixelShaderResource);
  context.cmd->Barrier(depth_, nrhi::ResourceState::kDepthWrite,
                       nrhi::ResourceState::kPixelShaderResource);
  context.cmd->FlushBarriers();
  context.cmd->Barrier(color_, nrhi::ResourceState::kPixelShaderResource,
                       nrhi::ResourceState::kRenderTarget);
  context.cmd->Barrier(depth_, nrhi::ResourceState::kPixelShaderResource,
                       nrhi::ResourceState::kDepthWrite);
  context.cmd->FlushBarriers();
  state_ = State::kReady;
  return OffscreenTargetOwnerResult::kSucceeded;
}

OffscreenTargetOwnerResult OffscreenTargetOwner::PrepareForResolve(
    const rex::graphics::NativeGuestOutputRenderContext &context,
    const NativeScenePassTargets &targets) {
  if (!EnsureContext(context)) {
    return OffscreenTargetOwnerResult::kInvalidContext;
  }
  if (state_ != State::kOpen) {
    return OffscreenTargetOwnerResult::kPassNotOpen;
  }
  if (!ExactTargets(targets) || resolve_source_ == nullptr) {
    return OffscreenTargetOwnerResult::kInvalidTargets;
  }
  context.cmd->Barrier(color_, nrhi::ResourceState::kRenderTarget,
                       nrhi::ResourceState::kPixelShaderResource);
  context.cmd->FlushBarriers();
  state_ = State::kResolving;
  return OffscreenTargetOwnerResult::kSucceeded;
}

OffscreenTargetOwnerResult
OffscreenTargetOwner::PrepareExternalReplayForResolve(
    const rex::graphics::NativeGuestOutputRenderContext &context,
    const NativeScenePassTargets &targets) {
  if (!EnsureContext(context)) {
    return OffscreenTargetOwnerResult::kInvalidContext;
  }
  if (state_ != State::kReady) {
    return state_ == State::kOpen || state_ == State::kResolving
               ? OffscreenTargetOwnerResult::kPassAlreadyOpen
               : OffscreenTargetOwnerResult::kInvalidTargets;
  }
  if (!ExactTargets(targets) || resolve_source_ == nullptr) {
    return OffscreenTargetOwnerResult::kInvalidTargets;
  }
  context.cmd->Barrier(color_, nrhi::ResourceState::kRenderTarget,
                       nrhi::ResourceState::kPixelShaderResource);
  context.cmd->FlushBarriers();
  state_ = State::kResolving;
  return OffscreenTargetOwnerResult::kSucceeded;
}

void OffscreenTargetOwner::RestoreAfterResolve(
    const rex::graphics::NativeGuestOutputRenderContext &context) {
  if (state_ != State::kResolving || context.cmd == nullptr) {
    return;
  }
  context.cmd->Barrier(color_, nrhi::ResourceState::kPixelShaderResource,
                       nrhi::ResourceState::kRenderTarget);
  context.cmd->FlushBarriers();
  state_ = State::kReady;
}

void OffscreenTargetOwner::Shutdown() {
  ReleaseResources();
  device_ = nullptr;
}

const char *OffscreenTargetOwnerResultName(OffscreenTargetOwnerResult result) {
  switch (result) {
  case OffscreenTargetOwnerResult::kSucceeded:
    return "succeeded";
  case OffscreenTargetOwnerResult::kInvalidContext:
    return "invalid_context";
  case OffscreenTargetOwnerResult::kPassAlreadyOpen:
    return "pass_already_open";
  case OffscreenTargetOwnerResult::kPassNotOpen:
    return "pass_not_open";
  case OffscreenTargetOwnerResult::kInvalidTargets:
    return "invalid_targets";
  case OffscreenTargetOwnerResult::kInvalidClearValues:
    return "invalid_clear_values";
  case OffscreenTargetOwnerResult::kSampleCountUnsupported:
    return "sample_count_unsupported";
  case OffscreenTargetOwnerResult::kAttachmentCreationFailed:
    return "attachment_creation_failed";
  }
  return "unknown";
}

} // namespace tabletennis::native
