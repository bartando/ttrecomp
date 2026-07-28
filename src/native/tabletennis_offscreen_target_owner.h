#pragma once

#include "native/tabletennis_native_scene_pass.h"

#include <array>
#include <cstdint>

namespace rex::graphics {
struct NativeGuestOutputRenderContext;
}

namespace tabletennis::native {

enum class OffscreenTargetOwnerResult : uint8_t {
  kSucceeded,
  kInvalidContext,
  kPassAlreadyOpen,
  kPassNotOpen,
  kInvalidTargets,
  kInvalidClearValues,
  kSampleCountUnsupported,
  kAttachmentCreationFailed,
};

struct OffscreenTargetDesc {
  rex::graphics::nrhi::Format color_format =
      rex::graphics::nrhi::Format::kUnknown;
  rex::graphics::nrhi::Format depth_format =
      rex::graphics::nrhi::Format::kUnknown;
  uint32_t sample_count = 0;
};

// Shared ownership/state machine for private multisample passes. It never
// writes the presenter and has no shader or draw policy.
class OffscreenTargetOwner {
public:
  OffscreenTargetOwner() = default;
  OffscreenTargetOwner(const OffscreenTargetOwner &) = delete;
  OffscreenTargetOwner &operator=(const OffscreenTargetOwner &) = delete;

  OffscreenTargetOwnerResult Prepare(
      const rex::graphics::NativeGuestOutputRenderContext &context,
      const OffscreenTargetDesc &desc, NativeScenePassTargets &targets_out);
  OffscreenTargetOwnerResult Begin(
      const rex::graphics::NativeGuestOutputRenderContext &context,
      const NativeScenePassTargets &targets,
      const std::array<float, 4> &clear_color, float clear_depth);
  OffscreenTargetOwnerResult Abort(
      const rex::graphics::NativeGuestOutputRenderContext &context);

  // Closes an open attachment scope and makes color sampleable. The caller
  // owns resolve commands and must call RestoreAfterResolve afterwards.
  OffscreenTargetOwnerResult PrepareForResolve(
      const rex::graphics::NativeGuestOutputRenderContext &context,
      const NativeScenePassTargets &targets);
  // A backend-owned replay records directly into an already-prepared target,
  // so this owner never observes an open NRHI pass. Transition that exact
  // ready target for a private shader resolve without pretending Begin ran.
  OffscreenTargetOwnerResult PrepareExternalReplayForResolve(
      const rex::graphics::NativeGuestOutputRenderContext &context,
      const NativeScenePassTargets &targets);
  void RestoreAfterResolve(
      const rex::graphics::NativeGuestOutputRenderContext &context);

  rex::graphics::nrhi::TextureView *resolve_source() const {
    return resolve_source_;
  }
  bool ExactTargets(const NativeScenePassTargets &targets) const;
  void Shutdown();

private:
  enum class State : uint8_t {
    kUnavailable,
    kReady,
    kOpen,
    kResolving,
  };

  bool EnsureContext(
      const rex::graphics::NativeGuestOutputRenderContext &context);
  void ReleaseResources();

  rex::graphics::nrhi::Device *device_ = nullptr;
  rex::graphics::nrhi::Texture *color_ = nullptr;
  rex::graphics::nrhi::Texture *depth_ = nullptr;
  rex::graphics::nrhi::TextureView *resolve_source_ = nullptr;
  OffscreenTargetDesc desc_{};
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  State state_ = State::kUnavailable;
};

const char *OffscreenTargetOwnerResultName(OffscreenTargetOwnerResult result);

} // namespace tabletennis::native
