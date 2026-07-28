#pragma once

#include <cstdint>
#include <memory>

#include <rex/graphics/native_guest_renderer.h>

namespace tabletennis::native {

// Immutable value-only proof captured after the translated backend has
// prepared the real pipeline and attachments. The embedded draw context is
// sanitized: device and command pointers are always null.
struct MainToCompTransitionStateSnapshot {
  uint64_t sequence = 0;
  uint32_t candidate_count = 0;
  rex::graphics::NativeGuestDrawStateContext state{};

  bool valid() const;
};

struct MainToCompTransitionStateTelemetry {
  uint64_t callbacks = 0;
  uint64_t shader_pair_matches = 0;
  uint64_t candidates = 0;
  uint64_t published_frames = 0;
  uint64_t rejected_frames = 0;
  uint64_t dropped_candidates = 0;
  uint64_t latest_backend_sequence = 0;
  uint64_t latest_published_sequence = 0;
  uint32_t retained_frames = 0;
};

bool MainToCompTransitionStateObserverEnabled();
void ObserveMainToCompTransitionState(
    const rex::graphics::NativeGuestDrawStateContext &context);

std::shared_ptr<const MainToCompTransitionStateSnapshot>
LatestMainToCompTransitionStateSnapshot();
MainToCompTransitionStateTelemetry
LatestMainToCompTransitionStateTelemetry();

} // namespace tabletennis::native
