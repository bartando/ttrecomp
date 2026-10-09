#include "generated/default/tabletennis_init.h"

#include <atomic>
#include <chrono>
#include <string>

#include <fmt/format.h>

#include <rex/logging.h>

// Debugging only: which rage netSocket the system link broadcast uses.
namespace {
std::atomic<int> init_logs{0}, broadcast_logs{0}, send_logs{0};
}

// rage::netSocket init: r3 = socket object.
extern "C" REX_FUNC(sub_82416B48) {
  const uint32_t self = ctx.r3.u32;
  __imp__sub_82416B48(ctx, base);
  if (init_logs.fetch_add(1) < 8) {
    REXLOG_INFO("diag socket init {:08X}: result {} handle {:08X} flags {:02X} port {}", self,
                ctx.r3.u32 & 0xFF, REX_LOAD_U32(self + 24), REX_LOAD_U8(self + 44),
                REX_LOAD_U16(self + 20));
  }
}

// Connection manager broadcast: r3 = manager, socket at +4, port at +608.
extern "C" REX_FUNC(sub_82413F30) {
  const uint32_t manager = ctx.r3.u32;
  const uint32_t socket = REX_LOAD_U32(manager + 4);
  if (broadcast_logs.fetch_add(1) < 6) {
    REXLOG_INFO("diag broadcast manager {:08X} socket {:08X} handle {:08X} flags {:02X} port {}",
                manager, socket, socket ? REX_LOAD_U32(socket + 24) : 0,
                socket ? REX_LOAD_U8(socket + 44) : 0, REX_LOAD_U16(manager + 608));
  }
  __imp__sub_82413F30(ctx, base);
  if (broadcast_logs.load() <= 6) {
    REXLOG_INFO("diag broadcast result {}", ctx.r3.u32 & 0xFF);
  }
}

// netSocket::Send: r3 = socket, r4 = destination (ip, port).
extern "C" REX_FUNC(sub_82416A38) {
  const uint32_t self = ctx.r3.u32;
  const uint32_t destination = ctx.r4.u32;
  if (send_logs.fetch_add(1) < 6) {
    REXLOG_INFO("diag send socket {:08X} handle {:08X} to {:08X}:{}", self,
                REX_LOAD_U32(self + 24), REX_LOAD_U32(destination), REX_LOAD_U16(destination + 4));
  }
  __imp__sub_82416A38(ctx, base);
}

// Connection manager init steps and who shuts it down.
extern "C" REX_FUNC(sub_82418F40) {
  __imp__sub_82418F40(ctx, base);
  REXLOG_INFO("diag socket layer start -> {}", ctx.r3.u32 & 0xFF);
}

extern "C" REX_FUNC(sub_82419128) {
  __imp__sub_82419128(ctx, base);
  REXLOG_INFO("diag create socket -> {:08X}", ctx.r3.u32);
}

extern "C" REX_FUNC(sub_82413680) {
  REXLOG_INFO("diag connection manager shutdown {:08X} from {:08X}", ctx.r3.u32,
              uint32_t(ctx.lr));
  __imp__sub_82413680(ctx, base);
}

extern "C" REX_FUNC(sub_82413538) {
  REXLOG_INFO("diag connection manager init {:08X} port {}", ctx.r3.u32, ctx.r4.u32 & 0xFFFF);
  __imp__sub_82413538(ctx, base);
  REXLOG_INFO("diag connection manager init -> {}", ctx.r3.u32 & 0xFF);
}

// What the socket open's imports return to the game.
#define DIAG_THUNK(addr, name)                                                       \
  extern "C" REX_FUNC(sub_##addr) {                                                  \
    static std::atomic<int> logs{0};                                                 \
    __imp__sub_##addr(ctx, base);                                                    \
    if (logs.fetch_add(1) < 4) REXLOG_INFO("diag " name " -> {:08X}", ctx.r3.u32);   \
  }
DIAG_THUNK(82481B38, "ioctlsocket")
DIAG_THUNK(82481B50, "setsockopt")
DIAG_THUNK(82481B88, "bind")
extern "C" REX_FUNC(sub_82481D40) {
  static std::atomic<int> logs{0};
  const uint64_t r27 = ctx.r27.u64, r28 = ctx.r28.u64, r29 = ctx.r29.u64, r31 = ctx.r31.u64;
  __imp__sub_82481D40(ctx, base);
  if (logs.fetch_add(1) < 4) {
    REXLOG_INFO("diag XNetGetTitleXnAddr -> {:08X}; r27 {:X}->{:X} r28 {:X}->{:X} r29 {:X}->{:X} r31 {:X}->{:X}",
                ctx.r3.u32, r27, ctx.r27.u64, r28, ctx.r28.u64, r29, ctx.r29.u64, r31, ctx.r31.u64);
  }
}

// Lobby starting-state update: what it waits for.
extern "C" REX_FUNC(sub_82302D18) {
  static std::atomic<int64_t> last_ms{0};
  const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now().time_since_epoch())
                          .count();
  if (now - last_ms.load() > 2000) {
    last_ms.store(now);
    const uint32_t net = REX_LOAD_U32(0x825EAB28);
    const uint32_t holder = REX_LOAD_U32(0x8271A328);
    const uint32_t inner = holder ? REX_LOAD_U32(holder + 8) : 0;
    const uint32_t object = inner ? REX_LOAD_U32(inner + 52) : 0;
    const uint32_t vtable = object ? REX_LOAD_U32(object) : 0;
    REXLOG_INFO("diag lobby: net state {} object {:08X} vfunc48 {:08X}",
                net ? int32_t(REX_LOAD_U32(net + 8)) : -1, object,
                vtable ? REX_LOAD_U32(vtable + 48) : 0);
  }
  __imp__sub_82302D18(ctx, base);
}

// Host lobby start sender: every input it decides on.
extern "C" REX_FUNC(sub_823028A8) {
  static std::atomic<int64_t> last_ms{0};
  const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now().time_since_epoch())
                          .count();
  if (now - last_ms.load() > 2000) {
    last_ms.store(now);
    const uint32_t holder = REX_LOAD_U32(0x8271A328);
    const uint32_t x = holder ? REX_LOAD_U32(holder + 8) : 0;
    const uint32_t session = x ? REX_LOAD_U32(x + 52) : 0;
    const uint32_t live = REX_LOAD_U32(0x8271A7B0);
    const uint32_t gamer = live ? REX_LOAD_U32(live + 0x1F5E8) : 0;
    const uint32_t other = session ? REX_LOAD_U32(session + 36) : 0;
    REXLOG_INFO(
        "diag start: x+56={} x+28={} gamer+32={} session+36+32={} slot0={} slot1={} "
        "params={:X},{:X} live+56={:08X} live+60={}",
        x ? int32_t(REX_LOAD_U32(x + 56)) : -9, x ? int32_t(REX_LOAD_U32(x + 28)) : -9,
        gamer ? int32_t(REX_LOAD_U32(gamer + 32)) : -9,
        other ? int32_t(REX_LOAD_U32(other + 32)) : -9,
        session ? int32_t(REX_LOAD_U32(session + 68)) : -9,
        session ? int32_t(REX_LOAD_U32(session + 76)) : -9, REX_LOAD_U32(0x825D0768 + 4),
        REX_LOAD_U32(0x825D077C + 4), live ? REX_LOAD_U32(live + 56) : 0,
        live ? int32_t(REX_LOAD_U32(live + 60)) : -9);
  }
  __imp__sub_823028A8(ctx, base);
}

// "All gamers reached lobby phase n?" (session vfunc 56).
extern "C" REX_FUNC(sub_8239E7D0) {
  static std::atomic<int64_t> last_ms{0};
  const uint32_t phase = ctx.r4.u32;
  __imp__sub_8239E7D0(ctx, base);
  const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now().time_since_epoch())
                          .count();
  // Log transitions only: (phase, answer) changes.
  static std::atomic<uint32_t> last_state{0xFFFFFFFF};
  (void)now;
  const uint32_t state = (phase << 8) | (ctx.r3.u32 & 0xFF);
  if (last_state.exchange(state) != state) {
    const uint32_t live = REX_LOAD_U32(0x8271A7B0);
    std::string gamers;
    for (int i = 0; i < 4 && live; ++i) {
      const uint32_t gamer = REX_LOAD_U32(live + 0x1DBC0 + 4 * i);
      gamers += fmt::format(" [{}] {:08X} flag449={}", i, gamer,
                            gamer ? REX_LOAD_U8(gamer + 449) : 0);
    }
    REXLOG_INFO("diag all-at-phase({}) -> {}; live+1DBBC={}{}", phase, ctx.r3.u32 & 0xFF,
                live ? int32_t(REX_LOAD_U32(live + 0x1DBBC)) : -9, gamers);
    // Connections: live+64 is the connection manager (count +496, array +492).
    const uint32_t manager = live + 64;
    const uint32_t count = REX_LOAD_U16(manager + 496);
    const uint32_t array = REX_LOAD_U32(manager + 492);
    for (uint32_t c = 0; c < count && c < 8 && array; ++c) {
      const uint32_t conn = array + c * 248;
      std::string head;
      for (int w = 0; w < 12; ++w) head += fmt::format(" {:08X}", REX_LOAD_U32(conn + 4 * w));
      REXLOG_INFO("diag conn[{}] state={}{}", c, int32_t(REX_LOAD_U32(conn + 232)), head);
    }
    for (int i = 0; i < 2 && live; ++i) {
      const uint32_t gamer = REX_LOAD_U32(live + 0x1DBC0 + 4 * i);
      if (!gamer) continue;
      std::string head;
      for (int w = 0; w < 10; ++w) head += fmt::format(" {:08X}", REX_LOAD_U32(gamer + 40 + 4 * w));
      REXLOG_INFO("diag gamer[{}]+40:{}", i, head);
    }
  }
}

// Per-gamer: connection index from the gamer's XNADDR, and whether it is up.
extern "C" REX_FUNC(sub_823B4928) {
  static std::atomic<int> logs{0};
  const uint32_t gamer = ctx.r3.u32;
  __imp__sub_823B4928(ctx, base);
  if (logs.fetch_add(1) < 40) {
    REXLOG_INFO("diag gamer {:08X} ({:08X} mac {:04X}{:08X}) -> connection {}", gamer,
                REX_LOAD_U32(gamer + 40), REX_LOAD_U16(gamer + 50), REX_LOAD_U32(gamer + 52),
                int32_t(ctx.r3.u32));
  }
}

// Ready handler: the two states it wants at 2, and the gamer it looks up.
extern "C" REX_FUNC(sub_823020A0) {
  static std::atomic<uint64_t> last{~0ull};
  const uint32_t live = REX_LOAD_U32(0x8271A7B0);
  if (live) {
    const uint32_t a = REX_LOAD_U32(live + 0x1DC48), b = REX_LOAD_U32(live + 0x1DC44);
    const uint32_t which = REX_LOAD_U32(live + 0xF4B4);
    const uint64_t state = (uint64_t(a) << 40) | (uint64_t(b) << 20) | which;
    if (last.exchange(state) != state) {
      REXLOG_INFO("diag ready: live+1DC48={} live+1DC44={} live+F4B4={}", int32_t(a),
                  int32_t(b), int32_t(which));
    }
  }
  __imp__sub_823020A0(ctx, base);
}


// Event 14 follow-up on the host: the message check and the step it gates.
extern "C" REX_FUNC(sub_823ACD80) {
  static std::atomic<int> logs{0};
  const uint32_t message = ctx.r4.u32;
  __imp__sub_823ACD80(ctx, base);
  if (logs.fetch_add(1) < 20) {
    const uint32_t field = (uint32_t(REX_LOAD_U8(message + 479)) << 16) |
                           (uint32_t(REX_LOAD_U16(message + 480)) << 8) | REX_LOAD_U8(message + 482);
    REXLOG_INFO("diag 823ACD80 -> {} field479={:06X} (lr {:08X})", ctx.r3.u32 & 0xFF, field,
                uint32_t(ctx.lr));
  }
}

extern "C" REX_FUNC(sub_823AE2A8) {
  REXLOG_INFO("diag 823AE2A8 called (lr {:08X})", uint32_t(ctx.lr));
  __imp__sub_823AE2A8(ctx, base);
}
