#pragma once

#include "native/tabletennis_d47_player_snapshot.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace rex::graphics {
struct NativeGuestDrawContext;
}

namespace tabletennis::native {

struct SceneCatalogDrawOccurrence;

struct D47PlayerFrameSnapshot {
  uint64_t sequence = 0;
  uint32_t title_candidate_count = 0;
  uint32_t admitted_draw_count = 0;
  uint32_t dropped_draw_count = 0;
  uint32_t guest_read_failures = 0;
  uint32_t payload_copy_failures = 0;
  uint32_t texture_capture_failures = 0;
  uint32_t backend_tile_blocks_matched = 0;
  uint32_t backend_sequence_mismatches = 0;
  std::vector<D47PlayerDrawSnapshot> draws;

  bool valid() const {
    if (title_candidate_count == 0 ||
        title_candidate_count != admitted_draw_count ||
        admitted_draw_count != draws.size() || dropped_draw_count != 0 ||
        guest_read_failures != 0 || payload_copy_failures != 0 ||
        texture_capture_failures != 0 ||
        backend_tile_blocks_matched != 3) {
      return false;
    }
    for (const D47PlayerDrawSnapshot& draw : draws) {
      if (!draw.valid || !draw.backend.valid) {
        return false;
      }
    }
    return true;
  }
};

struct D47PlayerObserverTelemetry {
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
  uint64_t latest_title_sequence = 0;
  uint64_t latest_published_sequence = 0;
  uint32_t pending_frames = 0;
  uint32_t queued_backend_events = 0;
};

// Title-side observer fed by the generic scene catalog after the real draw.
// It copies all guest-owned data and never mutates, suppresses, or serves.
void ObserveD47PlayerCatalogDraw(
    uint8_t* guest_base, const SceneCatalogDrawOccurrence& draw);

// Independent translated-backend hash/state proof. The observer verifies the
// complete ordered block and all three EDRAM tile replays.
void ObserveD47PlayerBackendDraw(
    const rex::graphics::NativeGuestDrawContext& context);

// Called after the alternate palette binder has published its exact cache
// postcondition. It completes immutable palette payloads for already-captured
// title draws of that generation before asynchronous backend publication.
void FinalizeD47PendingPaletteSnapshots(uint8_t* guest_base);

void D47PlayerObserverFrameEnd();
bool D47PlayerObserverEnabled();

std::shared_ptr<const D47PlayerFrameSnapshot>
LatestD47PlayerFrameSnapshot();
D47PlayerObserverTelemetry LatestD47PlayerObserverTelemetry();

}  // namespace tabletennis::native
