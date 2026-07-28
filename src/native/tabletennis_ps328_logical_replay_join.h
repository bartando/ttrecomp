#pragma once

#include "native/tabletennis_logical_draw_replay_packet.h"

#include <cstdint>
#include <memory>

namespace rex::graphics {
struct NativeGuestTranslatedReplayTokenContext;
}

namespace tabletennis::native {

enum class Ps328LogicalReplayJoinResult : uint8_t {
  kReady,
  kMissingTileProof,
  kTileProofUnsafe,
  kMissingToken,
  kMissingTitleFrame,
  kTitleDrawMismatch,
  kInvalidPacket,
  kShaderArtifactsPending,
  kBackendResourcesUnstable,
};

struct Ps328LogicalReplayJoin {
  Ps328LogicalReplayJoinResult result =
      Ps328LogicalReplayJoinResult::kMissingTileProof;
  uint64_t backend_frame_sequence = 0;
  uint32_t selected_ordinal = 0;
  // False while tiled instances differ mechanically or semantically. This
  // blocks deduplication/takeover, but not an exact one-token private replay.
  bool one_copy_takeover_ready = false;
  std::shared_ptr<
      const rex::graphics::NativeGuestTranslatedReplayTokenContext>
      token;
  LogicalDrawReplayPacket packet;

  bool observer_valid() const;
  bool replay_ready() const {
    return observer_valid() && result == Ps328LogicalReplayJoinResult::kReady &&
           packet.submission_ready();
  }
};

// Joins the dynamically selected PS328 tile proof to the exact immutable title
// draw. This only prepares evidence and requests missing translated artifacts;
// it never records, suppresses, resolves or presents rendering.
Ps328LogicalReplayJoin PrepareLatestPs328LogicalReplayJoin();
const char *Ps328LogicalReplayJoinResultName(
    Ps328LogicalReplayJoinResult result);

} // namespace tabletennis::native
