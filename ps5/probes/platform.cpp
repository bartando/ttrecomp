// SPDX-License-Identifier: GPL-3.0-or-later
// CPU platform probe only. No game code, files, GPU submissions or settings.
// API/layout reference: mihawk-99/PS5_PayloadSDK and holdmysocks/mcla-recomp.
#include <ps5platform/kernel.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <signal.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

extern "C" {
void tt_fault_write(void* address, uint32_t value);
uint32_t tt_fault_read(void* address);
extern const char tt_fault_write_site[], tt_fault_read_site[], tt_fault_read_resume[];
}

namespace {
constexpr size_t kArenaBytes = 0x120000000ull;
constexpr size_t kBackingBytes = 0x100000;
constexpr uint64_t kRegisterMarker = 0x5454505346351234ull;
constexpr uint32_t kEmulatedResult = 0x545407df;
constexpr uintptr_t kGpuWindowStart = 0x200000000ull;
constexpr uintptr_t kGpuWindowEnd = 0x300000000ull;
volatile sig_atomic_t fault_mode, fault_count, context_offset;
void* fault_page;

void signal_failure() {
  constexpr char message[] = "FAIL: unrecognized fault context or protection failure\n";
  (void)write(STDOUT_FILENO, message, sizeof(message) - 1);
  _exit(77);
}

void fault_handler(int, siginfo_t* info, void* opaque) {
  if (!info || info->si_addr != fault_page) signal_failure();
  const uintptr_t expected_rip = reinterpret_cast<uintptr_t>(
      fault_mode == 1 ? tt_fault_write_site : tt_fault_read_site);
  mcontext_t* found = nullptr;
  // Compare both public SDK layouts against known instruction/register values.
  // Do not write registers until the actual layout has been established.
  constexpr size_t candidates[] = {0x10, 0x40};
  for (size_t offset : candidates) {
    auto* candidate = reinterpret_cast<mcontext_t*>(static_cast<uint8_t*>(opaque) + offset);
    if (uintptr_t(candidate->mc_rip) == expected_rip &&
        uint64_t(candidate->mc_r12) == kRegisterMarker) {
      if (found) signal_failure();
      found = candidate;
      context_offset = static_cast<sig_atomic_t>(offset);
    }
  }
  if (!found || fault_count != 0) signal_failure();
  fault_count = 1;
  if (fault_mode == 1) {
    if (!(uint64_t(found->mc_err) & 2) ||
        mprotect(fault_page, PS5_KERNEL_PAGE_SIZE, PROT_READ | PROT_WRITE) != 0)
      signal_failure();
    // Return to exactly the same instruction; it must now complete.
  } else if (fault_mode == 2) {
    if (uint64_t(found->mc_err) & 2) signal_failure();
    found->mc_rax = kEmulatedResult;
    found->mc_rip = reinterpret_cast<uintptr_t>(tt_fault_read_resume);
  } else {
    signal_failure();
  }
}

bool faults(void* page) {
  struct sigaction action{}, old_segv{}, old_bus{};
  action.sa_sigaction = fault_handler;
  action.sa_flags = SA_SIGINFO;
  sigemptyset(&action.sa_mask);
  if (sigaction(SIGSEGV, &action, &old_segv) != 0) return false;
  if (sigaction(SIGBUS, &action, &old_bus) != 0) {
    sigaction(SIGSEGV, &old_segv, nullptr);
    return false;
  }
  bool passed = false;
  fault_page = page;
  fault_mode = 1;
  fault_count = 0;
  if (mprotect(page, PS5_KERNEL_PAGE_SIZE, PROT_READ) == 0) {
    std::puts("NEXT: write-watch fault and retry");
    tt_fault_write(page, kEmulatedResult);
    const bool retry = fault_count == 1 && *static_cast<volatile uint32_t*>(page) == kEmulatedResult;
    std::printf("%s: write-watch retry; machine-context offset=0x%x\n",
                retry ? "PASS" : "FAIL", unsigned(context_offset));
    fault_mode = 2;
    fault_count = 0;
    if (retry && mprotect(page, PS5_KERNEL_PAGE_SIZE, PROT_NONE) == 0) {
      std::puts("NEXT: read fault, emulate result and advance instruction pointer");
      const uint32_t value = tt_fault_read(page);
      passed = fault_count == 1 && value == kEmulatedResult;
      std::printf("%s: fault handler wrote RAX and RIP back correctly\n", passed ? "PASS" : "FAIL");
    }
  }
  const bool restore_bus = sigaction(SIGBUS, &old_bus, nullptr) == 0;
  const bool restore_segv = sigaction(SIGSEGV, &old_segv, nullptr) == 0;
  const bool restore_page = mprotect(page, PS5_KERNEL_PAGE_SIZE, PROT_READ | PROT_WRITE) == 0;
  return passed && restore_bus && restore_segv && restore_page;
}
}  // namespace

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::puts("TT PS5 platform probe: started (CPU memory and faults only)");
  static_assert(sizeof(void*) == 8);
  int failures = 0;
  const long host_page = sysconf(_SC_PAGESIZE);
  std::printf("Host page size: %ld\n", host_page);
  if (host_page != static_cast<long>(PS5_KERNEL_PAGE_SIZE)) return 1;

  // Reserve outside RADV's GPU window. Commit only 1 MiB of physical memory.
  void* arena = reinterpret_cast<void*>(uintptr_t{0x1000000000ull});
  std::puts("NEXT: reserve 4.5 GiB outside the GPU window");
  int32_t status = sceKernelReserveVirtualRange(&arena, kArenaBytes, 0, PS5_KERNEL_DIRECT_ALIGNMENT);
  if (status != 0) {
    std::printf("FAIL: arena reservation: 0x%08x\n", uint32_t(status));
    return 1;
  }
  const uintptr_t begin = reinterpret_cast<uintptr_t>(arena);
  if (!begin || begin > UINTPTR_MAX - kArenaBytes ||
      (begin < kGpuWindowEnd && begin + kArenaBytes > kGpuWindowStart)) {
    std::puts("FAIL: arena overlaps the GPU window or has invalid bounds");
    sceKernelMunmap(arena, kArenaBytes);
    return 1;
  }
  std::printf("PASS: 4.5 GiB virtual arena at %p, outside GPU window\n", arena);
  int64_t physical = -1;
  std::puts("NEXT: allocate 1 MiB of direct memory");
  status = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), kBackingBytes,
                                        PS5_KERNEL_DIRECT_ALIGNMENT, PS5_KERNEL_DIRECT_TYPE_CPU, &physical);
  if (status != 0) {
    std::printf("FAIL: direct allocation: 0x%08x\n", uint32_t(status));
    sceKernelMunmap(arena, kArenaBytes);
    return 1;
  }

  // The same backing at the guest executable aliases and physical aliases.
  constexpr uintptr_t offsets[] = {0x80000000, 0x90000000, 0xa0000000,
                                   0xc0000000, 0xe0000000, 0x100000000};
  size_t mapped = 0;
  for (uintptr_t offset : offsets) {
    void* requested = reinterpret_cast<void*>(begin + offset);
    void* view = requested;
    std::printf("NEXT: map owned alias +0x%llx\n", static_cast<unsigned long long>(offset));
    status = sceKernelMapDirectMemory(&view, kBackingBytes, PROT_READ | PROT_WRITE,
                                     PS5_KERNEL_MAP_FIXED, physical, PS5_KERNEL_DIRECT_ALIGNMENT);
    if (status != 0 || view != requested) {
      std::printf("FAIL: alias mapping: 0x%08x\n", uint32_t(status));
      ++failures;
      break;
    }
    ++mapped;
  }
  if (mapped == sizeof(offsets) / sizeof(offsets[0])) {
    auto* original = reinterpret_cast<uint8_t*>(begin + offsets[0]);
    std::memset(original, 0, kBackingBytes);
    constexpr size_t locations[] = {0, 0x1000, 0x4000, kBackingBytes - sizeof(uint32_t)};
    bool aliases = true;
    for (size_t location : locations) {
      *reinterpret_cast<volatile uint32_t*>(original + location) = kEmulatedResult;
      for (uintptr_t offset : offsets)
        aliases &= *reinterpret_cast<volatile uint32_t*>(begin + offset + location) == kEmulatedResult;
    }
    // Guest E+0x3000, with the required 4 KiB host bias, reaches physical +0x4000.
    aliases &= *reinterpret_cast<volatile uint32_t*>(begin + 0xe0000000 + 0x3000 + 0x1000) ==
               *reinterpret_cast<volatile uint32_t*>(begin + 0x100000000 + 0x4000);
    std::printf("%s: all six aliases, including E-range 4 KiB address bias\n", aliases ? "PASS" : "FAIL");
    if (!aliases) ++failures;
    if (!faults(original + 0x4000)) ++failures;
  }
  std::puts("NEXT: release all owned views and backing");
  if (sceKernelMunmap(arena, kArenaBytes) != 0) ++failures;
  if (sceKernelReleaseDirectMemory(physical, kBackingBytes) != 0) ++failures;
  std::printf("TT PS5 platform probe: finished with %d failures\n", failures);
  return failures ? 1 : 0;
}
