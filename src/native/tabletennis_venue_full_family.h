#pragma once

#include "native/tabletennis_venue_snapshot.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace tabletennis::native {

// One exact PS328-family title submission. The source occurrence preserves
// pass, program, shaders, stride and draw-time device state; captured owns the
// immutable host-side geometry, textures and constants.
struct VenueFullFamilyDrawSnapshot {
  SceneCatalogDrawOccurrence source{};
  VenueDrawSnapshot captured{};

  bool valid() const {
    return captured.mesh != nullptr && captured.mesh->valid() &&
           captured.material.valid;
  }
};

// Observer-only ledger for every exact PS328-family submission in one title
// frame. Order and duplicates are authoritative and intentionally preserved.
struct VenueFullFamilyFrame {
  uint64_t sequence = 0;
  uint32_t submitted_index_count = 0;
  uint32_t copy_failures = 0;
  uint32_t dropped_draws = 0;
  std::vector<VenueFullFamilyDrawSnapshot> draws;

  bool valid() const {
    if (sequence == 0 || draws.empty() || copy_failures != 0 ||
        dropped_draws != 0) {
      return false;
    }
    for (const VenueFullFamilyDrawSnapshot &draw : draws) {
      if (!draw.valid() || draw.source.frame_sequence != sequence) {
        return false;
      }
    }
    return true;
  }
};

bool VenueFullFamilyObserverEnabled();
bool VenueFullFamilyOverlayEnabled();

// Receives a draw already classified and captured by the shared venue capture
// path. This API cannot create replacement candidates or suppress guest work.
void ObserveVenueFullFamilyDraw(const SceneCatalogDrawOccurrence &source,
                                const VenueDrawSnapshot &captured);
void VenueFullFamilyFrameEnd();

std::shared_ptr<const VenueFullFamilyFrame> LatestVenueFullFamilyFrame();

} // namespace tabletennis::native
