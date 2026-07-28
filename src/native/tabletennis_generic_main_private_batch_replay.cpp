#include "native/tabletennis_generic_main_private_batch_replay.h"

#include "native/tabletennis_exact_main_targets.h"
#include "native/tabletennis_phase0_rectangle_readback.h"

#include <atomic>
#include <vector>

#include <rex/logging.h>
#include <rex/graphics/native_guest_renderer.h>

namespace tabletennis::native {
namespace {

namespace nrhi = rex::graphics::nrhi;
using ReplayToken =
    rex::graphics::NativeGuestTranslatedReplayTokenContext;

std::atomic<uint32_t> g_census_logged_family_count{0};

GenericMainPrivateBatchReplayResult Failure(
    GenericMainPrivateBatchReplayResult result,
    GenericMainPrivateBatchReplayStatus status) {
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

GenericMainPrivateBatchReplayResult
ReplayCurrentFrameGenericMainPrivateBatch(
    const rex::graphics::NativeGuestOutputRenderContext &context,
    bool queue_readback) {
  GenericMainPrivateBatchReplayResult result;
  result.backend_frame_sequence = context.backend_frame_sequence;
  if (context.backend_frame_sequence == 0 || context.device == nullptr) {
    return Failure(result,
                   GenericMainPrivateBatchReplayStatus::kInvalidContext);
  }

  rex::graphics::NativeGuestGuardedMainReplayBatch guarded_batch;
  if (!rex::graphics::TryGetCurrentFrameGuardedMainReplayBatch(
          context.backend_frame_sequence, guarded_batch) ||
      !guarded_batch.valid) {
    return Failure(
        result,
        GenericMainPrivateBatchReplayStatus::kGuardedBatchQueryFailed);
  }
  result.discovered_family_count = guarded_batch.discovered_family_count;
  result.proven_family_count = guarded_batch.proven_family_count;
  result.observed_draw_count = guarded_batch.observed_token_count;
  result.covered_draw_count = guarded_batch.covered_token_count;
  if (!guarded_batch.full_output_normalization_proven ||
      !guarded_batch.scissor_only_normalization ||
      guarded_batch.normalized_output_width != 1280 ||
      guarded_batch.normalized_output_height != 720 ||
      guarded_batch.ordered_tokens.empty()) {
    return Failure(
        result,
        GenericMainPrivateBatchReplayStatus::kNormalizationContractRejected);
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
          GenericMainPrivateBatchReplayStatus::kNormalizationContractRejected);
    }
    if (!tokens.empty() &&
        !AttachmentSignatureMatches(tokens.front(), token)) {
      return Failure(result,
                     GenericMainPrivateBatchReplayStatus::kAttachmentMismatch);
    }
    previous_opaque_token = token.opaque_token;
    tokens.push_back(token);
  }

  const ReplayToken &first = tokens.front();
  if (first.color_attachment_count != 1 ||
      first.color_attachment_formats[0] != nrhi::Format::kR8G8B8A8_UNORM ||
      first.depth_attachment_format == nrhi::Format::kUnknown ||
      first.stencil_attachment_format != first.depth_attachment_format ||
      context.guest_output_width != 1280 ||
      context.guest_output_height != 720) {
    return Failure(result,
                   GenericMainPrivateBatchReplayStatus::kAttachmentMismatch);
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
        result, GenericMainPrivateBatchReplayStatus::kTargetPreparationFailed);
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

  const auto replay_result =
      rex::graphics::TryReplayNativeGuestTranslatedDrawBatch(context, tokens,
                                                            target);
  if (replay_result !=
      rex::graphics::NativeGuestTranslatedReplayResult::kSucceeded) {
    result.replay_result_name =
        rex::graphics::NativeGuestTranslatedReplayResultName(replay_result);
    return Failure(result,
                   GenericMainPrivateBatchReplayStatus::kReplayRejected);
  }
  result.recorded_draw_count = static_cast<uint32_t>(tokens.size());
  if (queue_readback &&
      !QueuePhase0RectangleReadback(
          context, private_targets, context.backend_frame_sequence,
          PrivateReplayDiagnosticKind::kGuardedVenueBatch)) {
    return Failure(result,
                   GenericMainPrivateBatchReplayStatus::kReadbackQueueFailed);
  }
  result.status = GenericMainPrivateBatchReplayStatus::kSucceeded;
  return result;
}

void LogGenericMainFamilyCensusOnChange(
    const rex::graphics::NativeGuestOutputRenderContext &context) {
  if (context.backend_frame_sequence == 0) {
    return;
  }
  rex::graphics::NativeGuestGuardedMainReplayBatch batch;
  if (!rex::graphics::TryGetCurrentFrameGuardedMainReplayBatch(
          context.backend_frame_sequence, batch) ||
      batch.families.empty() || batch.proven_family_count == 0) {
    return;
  }
  // The per-family lines are the expensive part of this diagnostic, so it
  // reports only when the discovered set actually grew.
  uint32_t logged = g_census_logged_family_count.load(std::memory_order_acquire);
  if (batch.discovered_family_count <= logged ||
      !g_census_logged_family_count.compare_exchange_strong(
          logged, batch.discovered_family_count, std::memory_order_acq_rel)) {
    return;
  }
  REXLOG_INFO(
      "Table Tennis generic MAIN family census frame={} families={} "
      "proven={} observed_draws={} covered_draws={}",
      batch.backend_frame_sequence, batch.discovered_family_count,
      batch.proven_family_count, batch.observed_token_count,
      batch.covered_token_count);
  for (const auto &family : batch.families) {
    REXLOG_INFO(
        "  family vs={:016X} ps={:016X} observed={} replayed={} phase={} "
        "reject_mask={:#x}",
        family.vertex_shader_hash, family.pixel_shader_hash,
        family.observed_token_count, family.replayed_token_count,
        family.phase, family.reject_mask);
  }
}

const char *GenericMainPrivateBatchReplayStatusName(
    GenericMainPrivateBatchReplayStatus status) {
  switch (status) {
  case GenericMainPrivateBatchReplayStatus::kSucceeded:
    return "succeeded";
  case GenericMainPrivateBatchReplayStatus::kInvalidContext:
    return "invalid_context";
  case GenericMainPrivateBatchReplayStatus::kGuardedBatchQueryFailed:
    return "guarded_batch_query_failed";
  case GenericMainPrivateBatchReplayStatus::kNormalizationContractRejected:
    return "normalization_contract_rejected";
  case GenericMainPrivateBatchReplayStatus::kAttachmentMismatch:
    return "attachment_mismatch";
  case GenericMainPrivateBatchReplayStatus::kTargetPreparationFailed:
    return "target_preparation_failed";
  case GenericMainPrivateBatchReplayStatus::kReplayRejected:
    return "replay_rejected";
  case GenericMainPrivateBatchReplayStatus::kReadbackQueueFailed:
    return "readback_queue_failed";
  }
  return "unknown";
}

} // namespace tabletennis::native
