#include "generated/default/tabletennis_init.h"

#include <atomic>
#include <bit>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(tabletennis_system_link, false, "Table Tennis",
                    "Play the Xbox Live mode over the local network (System Link): the "
                    "game's own -forcesyslink debug parameter.");

// RAGE parameter "forcesyslink" (name 0x8206D7A8); its value pointer at +4 is
// non-null when the command line sets it. pongLiveManager reads it before
// finding sessions (sub_823AD528) and hosting game or spectator sessions
// (sub_823AD608, sub_823AD730), and then broadcasts on the LAN instead of
// using Live matchmaking. Retail builds get no command line, so set it here.
namespace {
constexpr uint32_t kForceSyslinkParam = 0x825D0FDC;
constexpr uint32_t kForceSyslinkName = 0x8206D7A8;

void ApplyForceSyslink(uint8_t* base, const char* caller) {
  if (REXCVAR_GET(tabletennis_system_link)) {
    REX_STORE_U32(kForceSyslinkParam + 4, kForceSyslinkName);
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1) < 6) {
      REXLOG_INFO("System link: forcesyslink set before {}", caller);
    }
  }
}
}  // namespace

extern "C" REX_FUNC(sub_823AD528) {
  ApplyForceSyslink(base, "FindSessions");
  __imp__sub_823AD528(ctx, base);
}

extern "C" REX_FUNC(sub_823AD608) {
  ApplyForceSyslink(base, "host game session");
  __imp__sub_823AD608(ctx, base);
}

extern "C" REX_FUNC(sub_823AD730) {
  ApplyForceSyslink(base, "host spectator session");
  __imp__sub_823AD730(ctx, base);
}

// pongLiveManager's session notification handler. The lobby's Ready step
// (sub_823020A0) waits for the game session to report arbitration
// registration (event 12) as well as its start (event 14). Live sessions
// register while starting; system link sessions cannot be arbitrated and only
// ever start, so the retail build stalls at "connecting". With system link on,
// a start is also reported as a registration, as an arbitrated session would.
namespace {
constexpr uint32_t kSessionStartSucceeded = 14;
constexpr uint32_t kArbitrationRegistered = 12;
}  // namespace

extern "C" REX_FUNC(sub_823AE760) {
  const bool started =
      REXCVAR_GET(tabletennis_system_link) && ctx.r5.u32 == kSessionStartSucceeded;
  const PPCContext notification = ctx;
  __imp__sub_823AE760(ctx, base);
  if (started) {
    PPCContext registered = notification;
    registered.r5.u64 = kArbitrationRegistered;
    REXLOG_INFO("System link: session started, reporting arbitration registration");
    __imp__sub_823AE760(registered, base);
  }
}

// Online lag compensation (object at match clock + 96; sub_823CEBF8 updates
// it once per frame with the frame time in f1). While waiting to serve, the
// side whose "capacity" (+16: 1 / (frame time - its own stall sleeps)) beats
// the peer's (+20) by more than a tunable runs up a debt of frames, which
// sub_823CEDE0 pays by sleeping a whole frame period. On the 360 a stalled
// frame still ends on a vsync, so capacity stays near the real frame rate.
// Here a stalled frame is just work plus the sleep: 5.7 ms of work reads as
// 175 fps, the debt never stops growing and every frame stalls (44 fps).
// Capacity above the real frame rate cannot help the peer, so cap it there.
extern "C" REX_FUNC(sub_823CEBF8) {
  const uint32_t compensator = ctx.r3.u32;
  const double frame_time = ctx.f1.f64;
  if (frame_time > 0.0) {
    const float frame_rate = float(1.0 / frame_time);
    const float capacity = std::bit_cast<float>(uint32_t(REX_LOAD_U32(compensator + 16)));
    if (capacity > frame_rate) {
      REX_STORE_U32(compensator + 16, std::bit_cast<uint32_t>(frame_rate));
    }
  }
  __imp__sub_823CEBF8(ctx, base);
}
