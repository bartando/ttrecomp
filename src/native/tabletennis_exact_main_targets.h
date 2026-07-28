#pragma once

#include "native/tabletennis_native_scene_pass.h"

#include <cstdint>

namespace rex::graphics {
struct NativeGuestOutputRenderContext;
}

namespace tabletennis::native {

// Exact attachment contract copied from one complete MAIN backend draw. The
// sample mask is retained for future pipeline preparation even though target
// allocation itself only consumes the sample count.
struct ExactMainTargetContract {
  rex::graphics::nrhi::Format depth_stencil_format =
      rex::graphics::nrhi::Format::kUnknown;
  uint32_t sample_count = 0;
  uint64_t sample_mask = 0;

  bool valid() const;
};

struct ExactMainPreparedTargets {
  NativeScenePassTargets attachments;
  ExactMainTargetContract contract;

  bool valid(
      const rex::graphics::NativeGuestOutputRenderContext &context) const;
};

enum class ExactMainTargetsResult : uint8_t {
  kSucceeded,
  kInvalidContext,
  kInvalidContract,
  kPassAlreadyOpen,
  kSampleCountUnsupported,
  kAttachmentCreationFailed,
  kInvalidTargets,
};

// Default-off preparation only. This allocates/reuses a private output-sized
// RGBA8 + exact packed depth/stencil pair and records no commands. It neither
// opens a pass nor resolves, presents, replaces, or suppresses guest work.
ExactMainTargetsResult PrepareExactMainTargets(
    const rex::graphics::NativeGuestOutputRenderContext &context,
    const ExactMainTargetContract &contract,
    ExactMainPreparedTargets &targets_out);

ExactMainTargetsResult PrepareExactMainTargetsForPrivateResolve(
    const rex::graphics::NativeGuestOutputRenderContext &context,
    const ExactMainPreparedTargets &targets,
    rex::graphics::nrhi::TextureView *&source_out);
void RestoreExactMainTargetsAfterPrivateResolve(
    const rex::graphics::NativeGuestOutputRenderContext &context);

const char *ExactMainTargetsResultName(ExactMainTargetsResult result);
void ShutdownExactMainTargets();

} // namespace tabletennis::native
