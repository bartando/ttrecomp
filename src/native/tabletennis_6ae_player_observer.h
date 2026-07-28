#pragma once

#include "native/tabletennis_6ae_player_snapshot.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace rex::graphics {
struct NativeGuestDrawContext;
}

namespace tabletennis::native {

struct SceneCatalogDrawOccurrence;

struct Player6AEFrameSnapshot {
  uint64_t sequence = 0;
  uint32_t title_candidate_count = 0;
  uint32_t admitted_draw_count = 0;
  uint32_t dropped_draw_count = 0;
  uint32_t guest_read_failures = 0;
  uint32_t payload_copy_failures = 0;
  uint32_t texture_capture_failures = 0;
  uint32_t sampler_contract_failures = 0;
  uint32_t shared_resource_capture_failures = 0;
  uint32_t backend_tile_blocks_matched = 0;
  uint32_t backend_sequence_mismatches = 0;
  std::vector<Player6AEDrawSnapshot> draws;

  bool valid() const {
    if (title_candidate_count == 0 ||
        title_candidate_count != admitted_draw_count ||
        admitted_draw_count != draws.size() || dropped_draw_count != 0 ||
        guest_read_failures != 0 || payload_copy_failures != 0 ||
        texture_capture_failures != 0 ||
        sampler_contract_failures != 0 ||
        shared_resource_capture_failures != 0 ||
        backend_tile_blocks_matched != 3 ||
        backend_sequence_mismatches != 0) {
      return false;
    }
    for (const Player6AEDrawSnapshot& draw : draws) {
      if (!draw.valid || !draw.backend.valid) {
        return false;
      }
    }
    return true;
  }
};

struct Player6AEObserverTelemetry {
  uint64_t title_frames = 0;
  uint64_t finalized_frames = 0;
  uint64_t valid_frames = 0;
  uint64_t rejected_frames = 0;
  uint64_t expired_frames = 0;
  uint64_t title_candidates = 0;
  uint64_t admitted_draws = 0;
  uint64_t backend_events = 0;
  uint64_t backend_events_dropped = 0;
  uint64_t backend_tile_blocks_matched = 0;
  uint64_t backend_sequence_mismatches = 0;
  uint64_t sampler_contract_failures = 0;
  uint64_t shared_resource_capture_failures = 0;
  uint64_t latest_title_sequence = 0;
  uint64_t latest_published_sequence = 0;
  uint32_t pending_frames = 0;
  uint32_t queued_backend_events = 0;
};

// Exact title-side pass identity selects the 6AE family before immutable
// guest data is copied. The independent backend stream proves shader hashes,
// attachment/state identity and all three EDRAM tile replays.
void ObservePlayer6AECatalogDraw(
    uint8_t* guest_base, const SceneCatalogDrawOccurrence& draw);
void ObservePlayer6AEBackendDraw(
    const rex::graphics::NativeGuestDrawContext& context);

void Player6AEObserverFrameEnd();
bool Player6AEObserverEnabled();

std::shared_ptr<const Player6AEFrameSnapshot>
LatestPlayer6AEFrameSnapshot();
Player6AEObserverTelemetry LatestPlayer6AEObserverTelemetry();

}  // namespace tabletennis::native
