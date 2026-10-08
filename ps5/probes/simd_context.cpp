// SPDX-License-Identifier: GPL-3.0-or-later
// Locate only our register marker in the fault context; do not dump its data.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <signal.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

extern "C" uint32_t tt_fault_read_xmm(void*, void*);
extern "C" const char tt_fault_xmm_site[], tt_fault_xmm_resume[];
namespace {
constexpr uint64_t marker = 0x5454505346351234ull;
void* page;
volatile sig_atomic_t count;
volatile sig_atomic_t marker_offsets[8];
volatile sig_atomic_t fp_format_valid;
void handler(int, siginfo_t* info, void* opaque) {
  auto* mc = reinterpret_cast<mcontext_t*>(static_cast<uint8_t*>(opaque) + 0x40);
  if (info->si_addr != page || uintptr_t(mc->mc_rip) != uintptr_t(tt_fault_xmm_site) ||
      uint64_t(mc->mc_r12) != marker) _exit(77);
  fp_format_valid = mc->mc_fpformat == _MC_FPFMT_XMM;
  const auto* words = static_cast<const uint64_t*>(opaque);
  for (unsigned i = 0; i < 96; ++i) {
    if (words[i] == marker && count < 8) {
      marker_offsets[count] = i * sizeof(uint64_t);
      count = count + 1;
    }
  }
  // Only the scalar layout already verified on this console is modified.
  mc->mc_rax = 0x545407df;
  mc->mc_rip = uintptr_t(tt_fault_xmm_resume);
}
}
int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::puts("TT PS5 SIMD context probe: started");
  page = mmap(nullptr, 0x4000, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (page == MAP_FAILED) return 1;
  struct sigaction action{}, old{};
  action.sa_flags = SA_SIGINFO;
  action.sa_sigaction = handler;
  sigemptyset(&action.sa_mask);
  if (sigaction(SIGSEGV, &action, &old) != 0) return 1;
  uint64_t vectors[4]{};
  const bool scalar = tt_fault_read_xmm(page, vectors) == 0x545407df;
  const bool xmm = vectors[0] == marker && vectors[1] == 0 && vectors[2] == marker && vectors[3] == 0;
  sigaction(SIGSEGV, &old, nullptr);
  std::printf("Header: mcontext=0x%zx, fpstate offset=0x%zx, expected XMM0=0x%zx\n",
              sizeof(mcontext_t), offsetof(mcontext_t, mc_fpstate), 0x40 + offsetof(mcontext_t, mc_fpstate) + 160);
  std::printf("FP format matches XMM: %s; original XMM0/15 restored: %s\n",
              fp_format_valid ? "yes" : "no", xmm ? "yes" : "no");
  for (int i = 0; i < count; ++i) std::printf("Our marker at signal-context offset 0x%x\n", unsigned(marker_offsets[i]));
  const bool unmap = munmap(page, 0x4000) == 0;
  std::printf("TT PS5 SIMD context probe: finished with %d failures\n", scalar && xmm && unmap ? 0 : 1);
  return scalar && xmm && unmap ? 0 : 1;
}
