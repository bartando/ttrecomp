#include "native/tabletennis_native_capture.h"

#include "native/tabletennis_draw_constants.h"
#include "native/tabletennis_fps_telemetry.h"
#include "native/tabletennis_observer_overlay.h"
#include "test/tabletennis_frontend_launch_test.h"

#include <algorithm>
#include <cmath>
#include <mutex>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_UINT32(
    tabletennis_native_capture_log_interval, 0, "Table Tennis",
    "Guest frames between native scene-capture summaries (0 = disabled).")
    .range(0, 100000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(
    tabletennis_native_capture_log_submissions, false, "Table Tennis",
    "Include captured renderable and vtable addresses in scene summaries.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace tabletennis::native {
namespace {

std::mutex g_capture_mutex;
CapturedFrame g_building_frame;
CapturedFrame g_published_frame;
CapturedBall g_last_ball;
uint64_t g_last_match_frame = 0;
bool g_logged_character_select = false;

// pongPlayer submissions occasionally skip a frame between render passes.
// Keep takeover steady across those gaps, but release it quickly when the
// title genuinely leaves gameplay.
constexpr uint64_t kGameplayGraceFrames = 30;

void RememberPlayer(CapturedFrame& frame, uint32_t player) {
  const auto begin = frame.players.begin();
  const auto used_end = begin + frame.player_count;
  if (std::find(begin, used_end, player) != used_end ||
      frame.player_count == frame.players.size()) {
    return;
  }
  frame.players[frame.player_count++] = player;
}

}  // namespace

void CapturePlayerDraw(uint32_t player, bool alternate_pass) {
  (void)alternate_pass;
  if (player == 0) {
    return;
  }

  std::lock_guard lock(g_capture_mutex);
  ++g_building_frame.player_draws;
  RememberPlayer(g_building_frame, player);
}

void CaptureDrawBucketEntry(uint32_t bucket_manager, uint32_t renderable,
                            uint32_t vtable, uint32_t bucket_mask) {
  (void)bucket_manager;
  if (renderable == 0) {
    return;
  }

  std::lock_guard lock(g_capture_mutex);
  ++g_building_frame.draw_bucket_entries;
  if (g_building_frame.submission_count <
      g_building_frame.submissions.size()) {
    g_building_frame.submissions[g_building_frame.submission_count++] = {
        renderable, vtable, bucket_mask};
  } else {
    ++g_building_frame.dropped_submissions;
  }
}

void CaptureBall(uint32_t ball, float x, float y, float z) {
  if (ball == 0 || !std::isfinite(x) || !std::isfinite(y) ||
      !std::isfinite(z)) {
    return;
  }
  std::lock_guard lock(g_capture_mutex);
  g_last_ball = {ball, {x, y, z}, true};
}

void CaptureFrameEnd() {
  std::unique_lock lock(g_capture_mutex);

  const bool was_gameplay_active = g_published_frame.gameplay_active;
  g_building_frame.sequence = g_published_frame.sequence + 1;
  g_building_frame.ball = g_last_ball;
  g_building_frame.camera = ConsumeObservedCamera();
  if (g_building_frame.HasMatchPlayers()) {
    g_last_match_frame = g_building_frame.sequence;
  }
  g_building_frame.gameplay_active =
      g_last_match_frame != 0 &&
      g_building_frame.sequence - g_last_match_frame <= kGameplayGraceFrames;
  g_published_frame = g_building_frame;
  g_building_frame = {};
  RequestObserverOverlayForFrame(
      g_published_frame.gameplay_active,
      g_published_frame.camera.valid &&
          g_published_frame.camera.direct_context_verified);

  if (!g_logged_character_select &&
      g_published_frame.player_count == 1 &&
      g_published_frame.player_draws != 0) {
    g_logged_character_select = true;
    REXLOG_INFO(
        "Table Tennis character select detected: frame={} player={:08X}",
        g_published_frame.sequence, g_published_frame.players[0]);
  }
  if (g_published_frame.gameplay_active && !was_gameplay_active) {
    REXLOG_INFO(
        "Table Tennis gameplay detected: frame={} players={:08X},{:08X}",
        g_published_frame.sequence, g_published_frame.players[0],
        g_published_frame.players[1]);
    tabletennis::test::NotifyGameplayReached();
  } else if (!g_published_frame.gameplay_active && was_gameplay_active) {
    REXLOG_INFO("Table Tennis gameplay ended: frame={}",
                g_published_frame.sequence);
  }

  const uint32_t log_interval =
      REXCVAR_GET(tabletennis_native_capture_log_interval);
  if (log_interval != 0 &&
      g_published_frame.sequence % log_interval == 0) {
    REXLOG_INFO(
        "Table Tennis native capture: frame={} bucket_entries={} "
        "player_draws={} players={} [{:08X}, {:08X}] match={} active={}",
        g_published_frame.sequence, g_published_frame.draw_bucket_entries,
        g_published_frame.player_draws, g_published_frame.player_count,
        g_published_frame.players[0], g_published_frame.players[1],
        g_published_frame.HasMatchPlayers(),
        g_published_frame.gameplay_active);
    if (g_published_frame.ball.valid) {
      REXLOG_INFO(
          "  ball={:08X} position=({:.3f}, {:.3f}, {:.3f})",
          g_published_frame.ball.guest_address,
          g_published_frame.ball.position[0],
          g_published_frame.ball.position[1],
          g_published_frame.ball.position[2]);
    }
    if (g_published_frame.camera.valid) {
      REXLOG_INFO(
          "  camera_generation={} position=({:.3f}, {:.3f}, {:.3f}) "
          "projection=({:.4f}, {:.4f}, {:.4f}, {:.4f}) source={:08X}",
          g_published_frame.camera.constant_generation,
          g_published_frame.camera.position[0],
          g_published_frame.camera.position[1],
          g_published_frame.camera.position[2],
          g_published_frame.camera.projection[0],
          g_published_frame.camera.projection[5],
          g_published_frame.camera.projection[10],
          g_published_frame.camera.projection[14],
          g_published_frame.camera.source_model);
      REXLOG_INFO(
          "  camera source context={:08X} draw_context_verified={} "
          "verified_candidates={}",
          g_published_frame.camera.source_render_context,
          g_published_frame.camera.direct_context_verified,
          g_published_frame.camera.verified_candidates);
    }
    if (REXCVAR_GET(tabletennis_native_capture_log_submissions)) {
      for (uint32_t index = 0; index < g_published_frame.submission_count;
           ++index) {
        const CapturedSubmission& submission =
            g_published_frame.submissions[index];
        REXLOG_INFO(
            "  submission[{}] renderable={:08X} vtable={:08X} buckets={:08X}",
            index, submission.renderable, submission.vtable,
            submission.bucket_mask);
      }
      if (g_published_frame.dropped_submissions != 0) {
        REXLOG_INFO("  dropped {} submissions",
                    g_published_frame.dropped_submissions);
      }
    }
  }

  const uint64_t published_sequence = g_published_frame.sequence;
  lock.unlock();
  UpdateGuestFpsTelemetry(published_sequence);
}

CapturedFrame LatestCapturedFrame() {
  std::lock_guard lock(g_capture_mutex);
  return g_published_frame;
}

}  // namespace tabletennis::native
