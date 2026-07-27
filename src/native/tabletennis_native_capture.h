#pragma once

#include "native/tabletennis_camera.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace tabletennis::native {

struct CapturedSubmission {
  uint32_t renderable = 0;
  uint32_t vtable = 0;
  uint32_t bucket_mask = 0;
};

struct CapturedBall {
  uint32_t guest_address = 0;
  std::array<float, 3> position{};
  bool valid = false;
};

struct CapturedFrame {
  static constexpr size_t kMaxSubmissions = 128;

  uint64_t sequence = 0;
  uint32_t draw_bucket_entries = 0;
  uint32_t player_draws = 0;
  std::array<uint32_t, 2> players{};
  uint32_t player_count = 0;
  std::array<CapturedSubmission, kMaxSubmissions> submissions{};
  uint32_t submission_count = 0;
  uint32_t dropped_submissions = 0;
  bool gameplay_active = false;
  CapturedBall ball;
  CapturedCamera camera;

  bool HasMatchPlayers() const {
    return player_count == players.size() && player_draws >= players.size();
  }
};

// Guest-thread capture points. These mirror the title's high-level scene
// submission flow rather than trying to reconstruct meaning from GPU draws.
void CapturePlayerDraw(uint32_t player, bool alternate_pass);
void CaptureDrawBucketEntry(uint32_t bucket_manager, uint32_t renderable,
                            uint32_t vtable, uint32_t bucket_mask);
void CaptureBall(uint32_t ball, float x, float y, float z);
void CaptureFrameEnd();

// Render-thread snapshot published at the title-side D3D swap.
CapturedFrame LatestCapturedFrame();

}  // namespace tabletennis::native
