#include "generated/default/tabletennis_init.h"

#include <rex/logging.h>

// Voice/session singleton (vtable 0x82066F84, global 0x8271A828). Its init,
// sub_8236A4D0, stores the "voice enabled" request in bit 0x80 of byte +84
// before it opens the VDP socket on port 1001 and creates the voice engine at
// +72. Any failure there runs this teardown, which frees and zeroes +72 but
// leaves bit 0x80 set. The per-frame update sub_8236A7D8 then dereferences the
// null engine. Clearing the bit selects the update's own voice-off path, which
// already re-checks it before every use of +72.
extern "C" REX_FUNC(sub_8236A720) {
  const uint32_t session = ctx.r3.u32;
  __imp__sub_8236A720(ctx, base);
  const uint8_t flags = REX_LOAD_U8(session + 84);
  if (flags & 0x80) {
    REXLOG_WARN("Voice session torn down; continuing without voice chat");
    REX_STORE_U8(session + 84, flags & ~0x80);
  }
}
