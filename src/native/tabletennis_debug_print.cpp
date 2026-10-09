#include "generated/default/tabletennis_init.h"

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(tabletennis_debug_print, false, "Table Tennis",
                    "Log the game's own debug messages (compiled out of the retail build)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// sub_8240E6D0 is RAGE's debug print, an empty function in the retail build
// with ~2400 callers. The format string is in r3 (printf) or r4 (sprintf
// into a buffer in r3), always in .rdata. Each format is logged a few times.
namespace {
constexpr uint32_t kRdataBegin = 0x82000600;
constexpr uint32_t kRdataEnd = 0x8209AEE0;
constexpr int kLogsPerFormat = 200;

std::mutex formats_mutex;
std::unordered_map<uint32_t, int> formats_seen;

bool InRdata(uint32_t address) {
  return address >= kRdataBegin && address < kRdataEnd;
}

std::string ReadString(uint8_t* base, uint32_t address, size_t limit = 200) {
  std::string text;
  if (address < 0x40000000) {
    return "(null)";
  }
  for (size_t i = 0; i < limit; ++i) {
    const char c = char(REX_LOAD_U8(address + i));
    if (!c) break;
    text.push_back(c);
  }
  return text;
}

// printf with integer and string arguments from consecutive GPRs.
std::string Format(uint8_t* base, PPCContext& ctx, uint32_t format, int first_arg) {
  const std::string pattern = ReadString(base, format, 400);
  const uint64_t gprs[] = {ctx.r3.u64, ctx.r4.u64, ctx.r5.u64, ctx.r6.u64,
                           ctx.r7.u64, ctx.r8.u64, ctx.r9.u64, ctx.r10.u64};
  const double fprs[] = {ctx.f1.f64, ctx.f2.f64, ctx.f3.f64, ctx.f4.f64};
  int arg = first_arg;
  int float_arg = 0;
  std::string out;
  for (size_t i = 0; i < pattern.size(); ++i) {
    if (pattern[i] != '%' || i + 1 >= pattern.size()) {
      out.push_back(pattern[i]);
      continue;
    }
    size_t j = i + 1;
    while (j < pattern.size() && std::string("-+ #0123456789.lhz").find(pattern[j]) !=
                                     std::string::npos) {
      ++j;
    }
    if (j >= pattern.size()) break;
    const char conversion = pattern[j];
    i = j;
    if (conversion == '%') {
      out.push_back('%');
      continue;
    }
    if (arg >= 8) {
      out += "?";
      continue;
    }
    const uint64_t value = gprs[arg++];
    char buffer[64];
    switch (conversion) {
      case 's':
        out += ReadString(base, uint32_t(value));
        continue;
      case 'd':
      case 'i':
        std::snprintf(buffer, sizeof(buffer), "%d", int32_t(value));
        break;
      case 'u':
        std::snprintf(buffer, sizeof(buffer), "%u", uint32_t(value));
        break;
      case 'x':
      case 'X':
      case 'p':
        std::snprintf(buffer, sizeof(buffer), "%x", uint32_t(value));
        break;
      case 'c':
        std::snprintf(buffer, sizeof(buffer), "%c", char(value));
        break;
      case 'f':
      case 'g':
      case 'e':
        // Floating-point varargs travel in FPRs and also take a GPR slot.
        std::snprintf(buffer, sizeof(buffer), "%g", float_arg < 4 ? fprs[float_arg++] : 0.0);
        break;
      default:
        std::snprintf(buffer, sizeof(buffer), "<%%%c>", conversion);
        break;
    }
    out += buffer;
  }
  return out;
}
}  // namespace

extern "C" REX_FUNC(sub_8240E6D0) {
  if (!REXCVAR_GET(tabletennis_debug_print)) {
    return;
  }
  uint32_t format = 0;
  int first_arg = 0;
  if (InRdata(ctx.r3.u32)) {
    format = ctx.r3.u32;
    first_arg = 1;
  } else if (InRdata(ctx.r4.u32)) {
    format = ctx.r4.u32;
    first_arg = 2;
  } else {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(formats_mutex);
    if (++formats_seen[format] > kLogsPerFormat) {
      return;
    }
  }
  REXLOG_INFO("game: {}", Format(base, ctx, format, first_arg));
}
