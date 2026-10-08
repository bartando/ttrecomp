// SPDX-License-Identifier: GPL-3.0-or-later
// Test the ported ReXGlue handler itself, including SIMD register writeback.
#include <rex/exception_handler.h>

#include <cstdint>
#include <cstdio>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>

extern "C" {
void tt_fault_write(void*, uint32_t);
uint32_t tt_fault_read(void*);
uint32_t tt_fault_read_xmm(void*, void*);
extern const char tt_fault_write_site[], tt_fault_read_site[], tt_fault_read_resume[];
extern const char tt_fault_xmm_site[], tt_fault_xmm_resume[];
}

namespace {
constexpr uint64_t kMarker = 0x5454505346351234ull;
constexpr uint32_t kResult = 0x545407df;
constexpr uint64_t kVectorHigh = 0x123456789abcdef0ull;
void* page;
size_t page_bytes;
volatile sig_atomic_t mode, faults;

bool handler(rex::arch::Exception* ex, void*) {
  using Exception = rex::arch::Exception;
  const char* site = mode == 1 ? tt_fault_write_site : mode == 2 ? tt_fault_read_site : tt_fault_xmm_site;
  if (ex->code() != Exception::Code::kAccessViolation ||
      ex->fault_address() != reinterpret_cast<uintptr_t>(page) ||
      ex->pc() != reinterpret_cast<uintptr_t>(site) ||
      ex->thread_context()->r12 != kMarker || faults != 0)
    return false;
  faults = 1;
  if (mode == 1) {
    return ex->access_violation_operation() == Exception::AccessViolationOperation::kWrite &&
           mprotect(page, page_bytes, PROT_READ | PROT_WRITE) == 0;
  }
  if (ex->access_violation_operation() != Exception::AccessViolationOperation::kRead) return false;
  ex->ModifyIntRegister(0) = kResult;  // RAX, through the writeback mask.
  if (mode == 2) {
    ex->set_resume_pc(reinterpret_cast<uintptr_t>(tt_fault_read_resume));
    return true;
  }
  if (mode != 3) return false;
  const auto* ctx = ex->thread_context();
  if (ctx->xmm0.low != kMarker || ctx->xmm0.high != 0 ||
      ctx->xmm15.low != kMarker || ctx->xmm15.high != 0)
    return false;
  auto& first = ex->ModifyXmmRegister(0);
  first.low = kResult;
  first.high = kVectorHigh;
  auto& last = ex->ModifyXmmRegister(15);
  last.low = kVectorHigh;
  last.high = kResult;
  ex->set_resume_pc(reinterpret_cast<uintptr_t>(tt_fault_xmm_resume));
  return true;
}
}  // namespace

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::puts("TT PS5 ReX fault probe: started (ported 0.8 SDK handler)");
  const long size = sysconf(_SC_PAGESIZE);
  if (size != 0x4000) return 1;
  page_bytes = static_cast<size_t>(size);
  page = mmap(nullptr, page_bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (page == MAP_FAILED) return 1;
  rex::arch::ExceptionHandler::Install(handler, nullptr);
  int failures = 0;
  mode = 1;
  faults = 0;
  if (mprotect(page, page_bytes, PROT_READ) == 0) {
    std::puts("NEXT: ReXGlue write-watch retry");
    tt_fault_write(page, kResult);
    const bool passed = faults == 1 && *static_cast<volatile uint32_t*>(page) == kResult;
    std::printf("%s: ReXGlue write-watch retry\n", passed ? "PASS" : "FAIL");
    if (!passed) ++failures;
  } else ++failures;
  mode = 2;
  faults = 0;
  if (mprotect(page, page_bytes, PROT_NONE) == 0) {
    std::puts("NEXT: ReXGlue RAX/RIP writeback");
    const bool passed = tt_fault_read(page) == kResult && faults == 1;
    std::printf("%s: ReXGlue RAX/RIP writeback\n", passed ? "PASS" : "FAIL");
    if (!passed) ++failures;
    mode = 3;
    faults = 0;
    uint64_t vectors[4]{};
    std::puts("NEXT: ReXGlue XMM0/XMM15 read and writeback");
    const bool xmm = tt_fault_read_xmm(page, vectors) == kResult && faults == 1 &&
                     vectors[0] == kResult && vectors[1] == kVectorHigh &&
                     vectors[2] == kVectorHigh && vectors[3] == kResult;
    std::printf("%s: ReXGlue XMM0/XMM15 read and writeback\n", xmm ? "PASS" : "FAIL");
    if (!xmm) ++failures;
  } else ++failures;
  rex::arch::ExceptionHandler::Uninstall(handler, nullptr);
  if (munmap(page, page_bytes) != 0) ++failures;
  std::printf("TT PS5 ReX fault probe: finished with %d failures\n", failures);
  return failures ? 1 : 0;
}
