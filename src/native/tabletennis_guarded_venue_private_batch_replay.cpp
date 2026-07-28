#include "native/tabletennis_guarded_venue_private_batch_replay.h"

#include "native/tabletennis_exact_main_targets.h"
#include "native/tabletennis_phase0_rectangle_readback.h"

#include <vector>

#include <rex/graphics/native_guest_renderer.h>

namespace tabletennis::native {
namespace {

namespace nrhi = rex::graphics::nrhi;
using ReplayToken =
    rex::graphics::NativeGuestTranslatedReplayTokenContext;

GuardedVenuePrivateBatchReplayResult Failure(
    GuardedVenuePrivateBatchReplayResult result,
    GuardedVenuePrivateBatchReplayStatus status) {
  result.status = status;
  return result;
}

bool AttachmentSignatureMatches(const ReplayToken &left,
                                const ReplayToken &right) {
  return left.color_attachment_count == right.color_attachment_count &&
         left.color_attachment_formats == right.color_attachment_formats &&
         left.depth_attachment_format == right.depth_attachment_format &&
         left.stencil_attachment_format == right.stencil_attachment_format &&
         left.sample_count == right.sample_count &&
         left.sample_mask == right.sample_mask &&
         left.dynamic_rendering == right.dynamic_rendering;
}

} // namespace

GuardedVenuePrivateBatchReplayResult
ReplayCurrentFrameGuardedVenuePrivateBatch(
    const rex::graphics::NativeGuestOutputRenderContext &context) {
  GuardedVenuePrivateBatchReplayResult result;
  result.backend_frame_sequence = context.backend_frame_sequence;
  if (context.backend_frame_sequence == 0 || context.device == nullptr) {
    return Failure(
        result, GuardedVenuePrivateBatchReplayStatus::kInvalidContext);
  }

  rex::graphics::NativeGuestGuardedReplayBatch guarded_batch;
  if (!rex::graphics::TryGetCurrentFrameGuardedVenueReplayBatch(
          context.backend_frame_sequence, guarded_batch) ||
      !guarded_batch.valid) {
    return Failure(
        result,
        GuardedVenuePrivateBatchReplayStatus::kGuardedBatchQueryFailed);
  }
  result.ps328_draw_count = guarded_batch.ps328_token_count;
  result.venue_9e_draw_count = guarded_batch.venue_9e_token_count;
  result.crowd_c6_draw_count = guarded_batch.crowd_c6_token_count;
  result.venue_14d_draw_count = guarded_batch.venue_14d_token_count;
  const size_t aggregate_draw_count =
      size_t(result.ps328_draw_count) +
      size_t(result.venue_9e_draw_count) +
      size_t(result.crowd_c6_draw_count) +
      size_t(result.venue_14d_draw_count);
  if (!guarded_batch.full_output_normalization_proven ||
      !guarded_batch.scissor_only_normalization ||
      guarded_batch.normalized_output_width != 1280 ||
      guarded_batch.normalized_output_height != 720 ||
      guarded_batch.ordered_tokens.empty() ||
      aggregate_draw_count != guarded_batch.ordered_tokens.size() ||
      aggregate_draw_count >
          rex::graphics::NativeGuestGuardedReplayBatch::kMaximumTokens) {
    return Failure(
        result,
        GuardedVenuePrivateBatchReplayStatus::
            kNormalizationContractRejected);
  }

  std::vector<ReplayToken> tokens;
  tokens.reserve(guarded_batch.ordered_tokens.size());
  uint64_t previous_opaque_token = 0;
  for (const auto &entry : guarded_batch.ordered_tokens) {
    const ReplayToken &token = entry.token;
    if (token.opaque_token <= previous_opaque_token ||
        token.scissor_offset != std::array<int32_t, 2>{0, 0} ||
        token.scissor_extent != std::array<uint32_t, 2>{1280, 720}) {
      return Failure(
          result,
          GuardedVenuePrivateBatchReplayStatus::
              kNormalizationContractRejected);
    }
    if (!tokens.empty() &&
        !AttachmentSignatureMatches(tokens.front(), token)) {
      return Failure(
          result,
          GuardedVenuePrivateBatchReplayStatus::kAttachmentMismatch);
    }
    previous_opaque_token = token.opaque_token;
    tokens.push_back(token);
  }

  const ReplayToken &first = tokens.front();
  if (first.color_attachment_count != 1 ||
      first.color_attachment_formats[0] !=
          nrhi::Format::kR8G8B8A8_UNORM ||
      first.depth_attachment_format == nrhi::Format::kUnknown ||
      first.stencil_attachment_format !=
          first.depth_attachment_format ||
      context.guest_output_width != 1280 ||
      context.guest_output_height != 720) {
    return Failure(
        result,
        GuardedVenuePrivateBatchReplayStatus::kAttachmentMismatch);
  }

  const ExactMainTargetContract contract = {
      .depth_stencil_format = first.depth_attachment_format,
      .sample_count = first.sample_count,
      .sample_mask = first.sample_mask,
  };
  ExactMainPreparedTargets private_targets;
  if (PrepareExactMainTargets(context, contract, private_targets) !=
          ExactMainTargetsResult::kSucceeded ||
      !private_targets.valid(context)) {
    return Failure(
        result,
        GuardedVenuePrivateBatchReplayStatus::kTargetPreparationFailed);
  }

  rex::graphics::NativeGuestTranslatedReplayTarget target;
  target.color = private_targets.attachments.color;
  target.depth_stencil = private_targets.attachments.depth;
  target.width = private_targets.attachments.width;
  target.height = private_targets.attachments.height;
  target.color_format = nrhi::Format::kR8G8B8A8_UNORM;
  target.depth_stencil_format = contract.depth_stencil_format;
  target.sample_count = contract.sample_count;
  target.clear_color = true;
  target.clear_depth_stencil = true;
  target.clear_color_value = {0.0f, 0.0f, 0.0f, 0.0f};
  target.clear_depth_value = 1.0f;
  target.clear_stencil_value = 0;

  if (rex::graphics::TryReplayNativeGuestTranslatedDrawBatch(
          context, tokens, target) !=
      rex::graphics::NativeGuestTranslatedReplayResult::kSucceeded) {
    return Failure(
        result, GuardedVenuePrivateBatchReplayStatus::kReplayRejected);
  }
  result.recorded_draw_count = static_cast<uint32_t>(tokens.size());
  if (!QueuePhase0RectangleReadback(
          context, private_targets, context.backend_frame_sequence,
          PrivateReplayDiagnosticKind::kGuardedVenueBatch)) {
    return Failure(
        result,
        GuardedVenuePrivateBatchReplayStatus::kReadbackQueueFailed);
  }
  result.status = GuardedVenuePrivateBatchReplayStatus::kSucceeded;
  return result;
}

const char *GuardedVenuePrivateBatchReplayStatusName(
    GuardedVenuePrivateBatchReplayStatus status) {
  switch (status) {
  case GuardedVenuePrivateBatchReplayStatus::kSucceeded:
    return "succeeded";
  case GuardedVenuePrivateBatchReplayStatus::kInvalidContext:
    return "invalid_context";
  case GuardedVenuePrivateBatchReplayStatus::kGuardedBatchQueryFailed:
    return "guarded_batch_query_failed";
  case GuardedVenuePrivateBatchReplayStatus::kNormalizationContractRejected:
    return "normalization_contract_rejected";
  case GuardedVenuePrivateBatchReplayStatus::kAttachmentMismatch:
    return "attachment_mismatch";
  case GuardedVenuePrivateBatchReplayStatus::kTargetPreparationFailed:
    return "target_preparation_failed";
  case GuardedVenuePrivateBatchReplayStatus::kReplayRejected:
    return "replay_rejected";
  case GuardedVenuePrivateBatchReplayStatus::kReadbackQueueFailed:
    return "readback_queue_failed";
  }
  return "unknown";
}

} // namespace tabletennis::native
