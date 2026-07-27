#include "test/tabletennis_frontend_launch_test.h"

#include "generated/default/tabletennis_init.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>

#include <rex/cvar.h>
#include <rex/input/input.h>
#include <rex/kernel/xam/input_injection.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(
    tabletennis_test_path, false, "Table Tennis",
    "Test-only: ask the settled one-player frontend to launch its selected "
    "venue as an offline Exhibition match.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(
    tabletennis_test_capture_gameplay_trace, false, "Table Tennis",
    "Test-only: capture one GPU frame when the two-player render marker "
    "proves gameplay is live.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace tabletennis::test {
namespace {

constexpr uint32_t kPongShellSingleton = 0x825EAB30;
constexpr uint32_t kGameConfigSingleton = 0x825EAB28;
constexpr uint32_t kFrontendState = 6;
constexpr uint32_t kNoPendingState = UINT32_MAX;

constexpr uint32_t kControllerHud = 0x28;
constexpr uint32_t kControllerLaunchPending = 0x15C;
// pongFrontendController::Update derives this from the HUD state:
// 0 = not launchable, 1 = one-player path, 2 = two-player path.
constexpr uint32_t kControllerFrontendMode = 0x160;

constexpr uint32_t kHudGameTypeToken = 0xA8;
constexpr uint32_t kHudCurrentState = 0x2574;
constexpr uint32_t kHudPreviousState = 0x2578;
constexpr uint32_t kGameConfigGameType = 0x8;

constexpr uint32_t kGlobalStartGame = 0x82062DB0;
constexpr uint32_t kGameTypeExhibitionOne = 0x8205FDD8;
constexpr uint32_t kFlashIntegerType = 3;

constexpr uint32_t kNestedCallStackBytes = 0x200;
constexpr uint32_t kScratchOffset = 0x100;
constexpr auto kPlayerBindingRetryDelay = std::chrono::seconds(6);

enum class WaitReason : uint8_t {
  kDisabled,
  kNotInstalled,
  kNoShell,
  kNotFrontend,
  kTransitionPending,
  kAlreadyTriggered,
  kLaunchPending,
  kNoHud,
  kFrontendChanging,
  kNoLaunchMode,
  kNoGameConfig,
  kStackUnavailable,
  kFlashBusy,
  kNoFlashRoot,
  kNoStartVariable,
  kNoExhibitionToken,
  kNoWritableStartVariable,
  kReady,
};

std::atomic<bool> g_installed = false;
std::atomic<bool> g_triggered = false;
std::atomic<bool> g_bootstrap_active = false;
std::atomic<bool> g_continue_active = false;
std::atomic<WaitReason> g_wait_reason = WaitReason::kNotInstalled;
std::atomic<uint32_t> g_no_player_state = UINT32_MAX;
std::atomic<int64_t> g_no_player_state_since_ns = 0;
std::atomic<bool> g_no_player_retry_uses_a = false;
std::mutex g_trace_requester_mutex;
std::function<void()> g_trace_requester;

const char *WaitReasonName(WaitReason reason) {
  switch (reason) {
  case WaitReason::kDisabled:
    return "disabled";
  case WaitReason::kNotInstalled:
    return "not installed";
  case WaitReason::kNoShell:
    return "waiting for pongShell";
  case WaitReason::kNotFrontend:
    return "waiting for Frontend state";
  case WaitReason::kTransitionPending:
    return "waiting for the shell transition to settle";
  case WaitReason::kAlreadyTriggered:
    return "launch already triggered";
  case WaitReason::kLaunchPending:
    return "frontend launch already pending";
  case WaitReason::kNoHud:
    return "waiting for the frontend HUD";
  case WaitReason::kFrontendChanging:
    return "waiting for a settled frontend screen";
  case WaitReason::kNoLaunchMode:
    return "waiting for a launchable frontend mode";
  case WaitReason::kNoGameConfig:
    return "waiting for pongGameConfig";
  case WaitReason::kStackUnavailable:
    return "guest stack has no safe scratch space";
  case WaitReason::kFlashBusy:
    return "waiting for the frontend Flash lock";
  case WaitReason::kNoFlashRoot:
    return "waiting for the frontend Flash root";
  case WaitReason::kNoStartVariable:
    return "waiting for GLOBAL_START_GAME";
  case WaitReason::kNoExhibitionToken:
    return "waiting for GAME_TYPE_EXHIBITION_ONE";
  case WaitReason::kNoWritableStartVariable:
    return "GLOBAL_START_GAME is not writable";
  case WaitReason::kReady:
    return "ready";
  }
  return "unknown";
}

void SetWaitReason(WaitReason reason) {
  const WaitReason previous =
      g_wait_reason.exchange(reason, std::memory_order_relaxed);
  if (previous != reason && REXCVAR_GET(tabletennis_test_path)) {
    REXLOG_INFO("Table Tennis auto Exhibition: {}", WaitReasonName(reason));
  }
}

void StopStartBootstrap() {
  if (!g_bootstrap_active.exchange(false, std::memory_order_acq_rel)) {
    return;
  }
  rex::kernel::xam::ClearSyntheticInput();
  REXLOG_INFO(
      "Table Tennis test path: player bound; Start bootstrap disabled");
}

int64_t MonotonicNowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void ResetPlayerBindingRetry() {
  g_no_player_state.store(UINT32_MAX, std::memory_order_relaxed);
  g_no_player_state_since_ns.store(0, std::memory_order_relaxed);
  g_no_player_retry_uses_a.store(false, std::memory_order_relaxed);
}

void NotePlayerBindingProgress() {
  const bool was_using_a =
      g_no_player_retry_uses_a.exchange(false, std::memory_order_relaxed);
  g_no_player_state.store(UINT32_MAX, std::memory_order_relaxed);
  g_no_player_state_since_ns.store(0, std::memory_order_relaxed);
  if (was_using_a) {
    rex::kernel::xam::SetSyntheticAutoTap(
        rex::input::X_INPUT_GAMEPAD_START, true);
    REXLOG_INFO(
        "Table Tennis test path: frontend advanced; restored Start bootstrap");
  }
}

void RetryPlayerBindingIfStalled(uint32_t hud_state) {
  const int64_t now_ns = MonotonicNowNs();
  const uint32_t previous_state =
      g_no_player_state.exchange(hud_state, std::memory_order_relaxed);
  if (previous_state != hud_state) {
    g_no_player_state_since_ns.store(now_ns, std::memory_order_relaxed);
    g_no_player_retry_uses_a.store(false, std::memory_order_relaxed);
    return;
  }

  const int64_t state_since_ns =
      g_no_player_state_since_ns.load(std::memory_order_relaxed);
  if (state_since_ns == 0 ||
      now_ns - state_since_ns <
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              kPlayerBindingRetryDelay)
              .count()) {
    return;
  }

  // START is the normal press-start path, but some cold boots settle on a
  // frontend confirmation screen that only accepts A. Alternate the retry
  // button only after the exact same settled, unbound HUD state has made no
  // progress for six seconds. This remains title-state-driven and cannot fire
  // during loading or gameplay.
  const bool use_a =
      !g_no_player_retry_uses_a.load(std::memory_order_relaxed);
  rex::kernel::xam::SetSyntheticAutoTap(
      use_a ? rex::input::X_INPUT_GAMEPAD_A
            : rex::input::X_INPUT_GAMEPAD_START,
      true);
  g_no_player_retry_uses_a.store(use_a, std::memory_order_relaxed);
  g_no_player_state_since_ns.store(now_ns, std::memory_order_relaxed);
  REXLOG_INFO(
      "Table Tennis test path: settled frontend state {} remained unbound; "
      "retrying with {}",
      hud_state, use_a ? "A" : "Start");
}

uint32_t CallGuest(PPCContext &call_ctx, uint8_t *base, PPCFunc *function,
                   uint32_t r3, uint32_t r4 = 0, uint32_t r5 = 0) {
  call_ctx.r3.u32 = r3;
  call_ctx.r4.u32 = r4;
  call_ctx.r5.u32 = r5;
  function(call_ctx, base);
  return call_ctx.r3.u32;
}

bool TryLaunchExhibition(PPCContext &incoming_ctx, uint8_t *base,
                         uint32_t controller) {
  if (!g_installed.load(std::memory_order_relaxed)) {
    SetWaitReason(WaitReason::kNotInstalled);
    return false;
  }
  if (!REXCVAR_GET(tabletennis_test_path)) {
    g_triggered.store(false, std::memory_order_relaxed);
    SetWaitReason(WaitReason::kDisabled);
    return false;
  }
  if (base == nullptr || controller == 0) {
    SetWaitReason(WaitReason::kNoShell);
    return false;
  }

  const uint32_t shell = REX_LOAD_U32(kPongShellSingleton);
  if (shell == 0) {
    SetWaitReason(WaitReason::kNoShell);
    return false;
  }
  if (REX_LOAD_U32(shell + 0xC) != kFrontendState) {
    SetWaitReason(WaitReason::kNotFrontend);
    return false;
  }
  if (REX_LOAD_U32(shell + 0x10) != kNoPendingState) {
    SetWaitReason(WaitReason::kTransitionPending);
    return false;
  }
  if (g_triggered.load(std::memory_order_relaxed)) {
    SetWaitReason(WaitReason::kAlreadyTriggered);
    return false;
  }
  if (REX_LOAD_U8(controller + kControllerLaunchPending) != 0) {
    g_triggered.store(true, std::memory_order_relaxed);
    SetWaitReason(WaitReason::kLaunchPending);
    return false;
  }

  const uint32_t hud = REX_LOAD_U32(controller + kControllerHud);
  if (hud == 0) {
    SetWaitReason(WaitReason::kNoHud);
    return false;
  }
  const uint32_t frontend_mode =
      REX_LOAD_U32(controller + kControllerFrontendMode);
  if (frontend_mode != 0) {
    StopStartBootstrap();
  }
  const uint32_t hud_state = REX_LOAD_U32(hud + kHudCurrentState);
  const uint32_t hud_previous_state =
      REX_LOAD_U32(hud + kHudPreviousState);
  if (hud_state != hud_previous_state) {
    NotePlayerBindingProgress();
    SetWaitReason(WaitReason::kFrontendChanging);
    return false;
  }
  if (frontend_mode == 0) {
    RetryPlayerBindingIfStalled(hud_state);
    SetWaitReason(WaitReason::kNoLaunchMode);
    return false;
  }
  ResetPlayerBindingRetry();

  const uint32_t game_config = REX_LOAD_U32(kGameConfigSingleton);
  if (game_config == 0) {
    SetWaitReason(WaitReason::kNoGameConfig);
    return false;
  }
  if (incoming_ctx.r1.u32 < kNestedCallStackBytes) {
    SetWaitReason(WaitReason::kStackUnavailable);
    return false;
  }

  // Use a private copy of the incoming context so the title's original
  // frontend Update receives exactly the registers its caller supplied.
  // The lowered guest stack gives the nested title calls a caller frame and a
  // scratch word that none of their downward-growing frames can overlap.
  PPCContext call_ctx = incoming_ctx;
  call_ctx.r1.u32 -= kNestedCallStackBytes;
  const uint32_t scratch = call_ctx.r1.u32 + kScratchOffset;
  REX_STORE_U32(scratch, 0);

  const bool locked = CallGuest(call_ctx, base, &sub_822EB2A8, hud) != 0;
  if (!locked) {
    SetWaitReason(WaitReason::kFlashBusy);
    return false;
  }

  const auto unlock = [&] { CallGuest(call_ctx, base, &sub_822EB320, hud); };

  const uint32_t root = CallGuest(call_ctx, base, &sub_822EB1E8, hud);
  if (root == 0) {
    unlock();
    SetWaitReason(WaitReason::kNoFlashRoot);
    return false;
  }

  const uint32_t start_value =
      CallGuest(call_ctx, base, &sub_823F9280, root, kGlobalStartGame);
  if (start_value == 0) {
    unlock();
    SetWaitReason(WaitReason::kNoStartVariable);
    return false;
  }

  if (CallGuest(call_ctx, base, &sub_823FA5C8, start_value) > 0) {
    unlock();
    g_triggered.store(true, std::memory_order_relaxed);
    SetWaitReason(WaitReason::kAlreadyTriggered);
    return false;
  }

  REX_STORE_U32(scratch, 0);
  const bool found_exhibition = CallGuest(call_ctx, base, &sub_823F9808, root,
                                          kGameTypeExhibitionOne, scratch) != 0;
  const uint32_t exhibition_token = REX_LOAD_U32(scratch);
  if (!found_exhibition || exhibition_token == 0) {
    unlock();
    SetWaitReason(WaitReason::kNoExhibitionToken);
    return false;
  }

  const uint32_t writable_start =
      CallGuest(call_ctx, base, &sub_823F9318, root, kGlobalStartGame);
  if (writable_start == 0) {
    unlock();
    SetWaitReason(WaitReason::kNoWritableStartVariable);
    return false;
  }

  // Commit only after every lookup and readiness check succeeds. Setting the
  // start value last lets the stock Update own all match setup and the
  // Frontend -> Loading -> Game state transitions.
  REX_STORE_U32(game_config + kGameConfigGameType, 1);
  REX_STORE_U32(hud + kHudGameTypeToken, exhibition_token);
  REX_STORE_U32(writable_start, 1);
  REX_STORE_U32(writable_start + 4, kFlashIntegerType);
  unlock();

  // The stock loading flow ends on an explicit "Load Complete / Continue A"
  // screen. Start A auto-tap only after the match request is committed, and
  // stop it from the two-player render marker before normal gameplay input.
  rex::kernel::xam::SetSyntheticAutoTap(rex::input::X_INPUT_GAMEPAD_A, true);
  g_continue_active.store(true, std::memory_order_release);
  g_triggered.store(true, std::memory_order_relaxed);
  SetWaitReason(WaitReason::kReady);
  REXLOG_INFO(
      "Table Tennis auto Exhibition triggered through GLOBAL_START_GAME "
      "(controller={:08X}, hud={:08X}, frontend_mode={}, token={:08X})",
      controller, hud, frontend_mode, exhibition_token);
  REXLOG_INFO(
      "Table Tennis test path: loading-screen Continue enabled until gameplay");
  return true;
}

} // namespace

void InstallFrontendLaunchTest() {
  g_triggered.store(false, std::memory_order_relaxed);
  g_continue_active.store(false, std::memory_order_relaxed);
  ResetPlayerBindingRetry();
  g_wait_reason.store(WaitReason::kDisabled, std::memory_order_relaxed);
  g_installed.store(true, std::memory_order_release);
  if (REXCVAR_GET(tabletennis_test_path)) {
    // Start binds the signed-in XAM user and gets the title into its real
    // frontend. Auto-tap repeats only until the first frontend Update hook;
    // no confirm/A presses are synthesized.
    rex::kernel::xam::SetSyntheticAutoTap(rex::input::X_INPUT_GAMEPAD_START,
                                          true);
    g_bootstrap_active.store(true, std::memory_order_release);
    REXLOG_INFO(
        "Table Tennis test path: Start bootstrap enabled until Frontend");
  }
  REXLOG_INFO("Table Tennis guarded auto-Exhibition test installed "
              "(enable tabletennis_test_path to use it)");
}

void ShutdownFrontendLaunchTest() {
  g_installed.store(false, std::memory_order_release);
  const bool bootstrap_active =
      g_bootstrap_active.exchange(false, std::memory_order_acq_rel);
  const bool continue_active =
      g_continue_active.exchange(false, std::memory_order_acq_rel);
  if (bootstrap_active || continue_active) {
    rex::kernel::xam::ClearSyntheticInput();
  }
  g_triggered.store(false, std::memory_order_relaxed);
  ResetPlayerBindingRetry();
  g_wait_reason.store(WaitReason::kNotInstalled, std::memory_order_relaxed);
}

void SetGameplayTraceRequester(std::function<void()> requester) {
  std::lock_guard lock(g_trace_requester_mutex);
  g_trace_requester = std::move(requester);
}

void NotifyGameplayReached() {
  if (!g_continue_active.exchange(false, std::memory_order_acq_rel)) {
    return;
  }
  rex::kernel::xam::ClearSyntheticInput();
  REXLOG_INFO(
      "Table Tennis test path: gameplay reached; synthetic input disabled");
  if (!REXCVAR_GET(tabletennis_test_capture_gameplay_trace)) {
    return;
  }

  std::function<void()> requester;
  {
    std::lock_guard lock(g_trace_requester_mutex);
    requester = g_trace_requester;
  }
  if (requester) {
    requester();
  } else {
    REXLOG_ERROR(
        "Table Tennis gameplay trace requested without a host requester");
  }
}

}  // namespace tabletennis::test

// pongFrontendController::Update. This hook does not request a shell state
// directly; it can only raise the title's own GLOBAL_START_GAME variable.
extern "C" REX_FUNC(sub_8230DB30) {
  const uint32_t controller = ctx.r3.u32;
  tabletennis::test::TryLaunchExhibition(ctx, base, controller);
  __imp__sub_8230DB30(ctx, base);
}
