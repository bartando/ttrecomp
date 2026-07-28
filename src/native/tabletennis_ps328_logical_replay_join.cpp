#include "native/tabletennis_ps328_logical_replay_join.h"

#include "native/tabletennis_ps328_tile_invariance.h"
#include "native/tabletennis_venue_full_family.h"

#include <mutex>
#include <ranges>

#include <rex/graphics/native_guest_renderer.h>
#include <rex/logging.h>

namespace tabletennis::native {
namespace {

std::mutex g_mismatch_log_mutex;
uint64_t g_last_mismatch_log_sequence = 0;

bool MatchesToken(const VenueFullFamilyDrawSnapshot &draw,
                  const rex::graphics::NativeGuestTranslatedReplayTokenContext
                      &token,
                  uint32_t selected_ordinal) {
  return draw.valid() && draw.source.ordinal == selected_ordinal &&
         draw.source.primitive_type == token.guest_primitive_type &&
         draw.source.submitted_index_count ==
             token.guest_vertex_or_index_count &&
         token.guest_index_base_valid &&
         draw.captured.mesh->source_index_physical_address ==
             token.guest_index_base;
}

void LogMismatchOnce(
    const VenueFullFamilyFrame &frame,
    const rex::graphics::NativeGuestTranslatedReplayTokenContext &token,
    uint32_t selected_ordinal) {
  {
    std::lock_guard lock(g_mismatch_log_mutex);
    if (g_last_mismatch_log_sequence == frame.sequence) {
      return;
    }
    g_last_mismatch_log_sequence = frame.sequence;
  }
  REXLOG_INFO(
      "Table Tennis PS328 join mismatch: requested_sequence={} "
      "resolved_sequence={} ordinal={} primitive={} count={} "
      "physical_ib={:08X} venue_draws={}",
      token.backend_frame_sequence, frame.sequence, selected_ordinal,
      token.guest_primitive_type, token.guest_vertex_or_index_count,
      token.guest_index_base, frame.draws.size());
  for (const VenueFullFamilyDrawSnapshot &candidate : frame.draws) {
    REXLOG_INFO(
        "  PS328 venue candidate: ordinal={} primitive={} count={} "
        "virtual_ib={:08X} physical_ib={:08X}",
        candidate.source.ordinal, candidate.source.primitive_type,
        candidate.source.submitted_index_count,
        candidate.source.mesh.index_buffer_alias,
        candidate.captured.mesh != nullptr
            ? candidate.captured.mesh->source_index_physical_address
            : 0);
  }
}

} // namespace

bool Ps328LogicalReplayJoin::observer_valid() const {
  return backend_frame_sequence != 0 && selected_ordinal != 0 &&
         token != nullptr && token->valid &&
         token->backend_frame_sequence == backend_frame_sequence &&
         packet.valid() && packet.ordinal == selected_ordinal;
}

Ps328LogicalReplayJoin PrepareLatestPs328LogicalReplayJoin() {
  Ps328LogicalReplayJoin joined;
  const auto proof = LatestPs328TileInvarianceSnapshot();
  if (proof == nullptr || !proof->valid()) {
    return joined;
  }
  joined.backend_frame_sequence = proof->sequence;
  joined.selected_ordinal = proof->selected_ordinal;
  joined.one_copy_takeover_ready = proof->one_copy_replay_ready;
  joined.token = proof->selected_token;
  if (joined.token == nullptr || !joined.token->valid ||
      joined.token->backend_frame_sequence != joined.backend_frame_sequence) {
    joined.result = Ps328LogicalReplayJoinResult::kMissingToken;
    return joined;
  }

  const auto frame =
      VenueFullFamilyFrameForSequence(proof->sequence);
  if (frame == nullptr || !frame->valid()) {
    joined.result = Ps328LogicalReplayJoinResult::kMissingTitleFrame;
    return joined;
  }
  const auto draw = std::ranges::find_if(
      frame->draws, [&](const VenueFullFamilyDrawSnapshot &candidate) {
        return MatchesToken(candidate, *joined.token,
                            joined.selected_ordinal);
      });
  if (draw == frame->draws.end()) {
    LogMismatchOnce(*frame, *joined.token, joined.selected_ordinal);
    joined.result = Ps328LogicalReplayJoinResult::kTitleDrawMismatch;
    return joined;
  }

  joined.packet = PrepareLogicalReplayPacket(*draw, *joined.token);
  if (!joined.packet.valid()) {
    joined.result = Ps328LogicalReplayJoinResult::kInvalidPacket;
    return joined;
  }
  const uint32_t missing = static_cast<uint32_t>(joined.packet.missing);
  const uint32_t shader_missing =
      static_cast<uint32_t>(LogicalReplayMissing::kTranslatedVertexProgram) |
      static_cast<uint32_t>(LogicalReplayMissing::kTranslatedPixelProgram);
  if (missing & shader_missing) {
    joined.result = Ps328LogicalReplayJoinResult::kShaderArtifactsPending;
    return joined;
  }
  if (!joined.token->resources_stable_for_deferred_replay ||
      (missing & static_cast<uint32_t>(
                     LogicalReplayMissing::kTranslatedReplayState))) {
    joined.result = Ps328LogicalReplayJoinResult::kBackendResourcesUnstable;
    return joined;
  }
  joined.result = Ps328LogicalReplayJoinResult::kReady;
  return joined;
}

const char *Ps328LogicalReplayJoinResultName(
    Ps328LogicalReplayJoinResult result) {
  switch (result) {
  case Ps328LogicalReplayJoinResult::kReady:
    return "ready";
  case Ps328LogicalReplayJoinResult::kMissingTileProof:
    return "missing_tile_proof";
  case Ps328LogicalReplayJoinResult::kTileProofUnsafe:
    return "tile_proof_unsafe";
  case Ps328LogicalReplayJoinResult::kMissingToken:
    return "missing_token";
  case Ps328LogicalReplayJoinResult::kMissingTitleFrame:
    return "missing_title_frame";
  case Ps328LogicalReplayJoinResult::kTitleDrawMismatch:
    return "title_draw_mismatch";
  case Ps328LogicalReplayJoinResult::kInvalidPacket:
    return "invalid_packet";
  case Ps328LogicalReplayJoinResult::kShaderArtifactsPending:
    return "shader_artifacts_pending";
  case Ps328LogicalReplayJoinResult::kBackendResourcesUnstable:
    return "backend_resources_unstable";
  }
  return "unknown";
}

} // namespace tabletennis::native
