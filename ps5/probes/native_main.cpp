// SPDX-License-Identifier: GPL-3.0-or-later
// Run the CPU probes inside an installed homebrew title, without game data.
#include <cstdio>
#include <cstdint>

extern "C" int sceKernelDebugOutText(int channel, const char* text);
extern "C" int sceKernelUsleep(uint32_t microseconds);
int tt_platform_probe_main();
int tt_simd_context_probe_main();
int tt_rex_fault_probe_main();

// The public title CRT offers this hook before exit(). On this console its
// normal exit path raises SIGSYS after main has completed. Keep diagnostics
// alive for the shell's Close Game action instead, on success or failure.
extern "C" [[noreturn]] void catchReturnFromMain(int status) {
  sceKernelDebugOutText(0, status ? "[tt-probes] checks failed; waiting for system close\n"
                                : "[tt-probes] checks passed; waiting for system close\n");
  unsigned seconds = 0;
  for (;;) {
    sceKernelUsleep(1000000);
    if (++seconds == 25) {
      if (FILE* results = std::fopen("/app0/tt-ps5-probes.txt", "a")) {
        std::fputs("TT PS5 native probes: alive after 25 seconds; awaiting system close\n", results);
        std::fclose(results);
      }
      sceKernelDebugOutText(0, "[tt-probes] alive after 25 seconds\n");
    }
  }
}

int main() {
  constexpr const char* path = "/app0/tt-ps5-probes.txt";
  if (!std::freopen(path, "w", stdout)) {
    sceKernelDebugOutText(0, "[tt-probes] FAIL: cannot open title results file\n");
    return 1;
  }
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  sceKernelDebugOutText(0, "[tt-probes] installed-title CPU probes started\n");
  int failures = 0;
  if (tt_platform_probe_main() != 0) ++failures;
  if (tt_simd_context_probe_main() != 0) ++failures;
  if (tt_rex_fault_probe_main() != 0) ++failures;
  std::printf("TT PS5 native probes: finished with %d failures\n", failures);
  std::fflush(stdout);
  sceKernelDebugOutText(0, failures ? "[tt-probes] installed-title probes FAILED\n"
                                  : "[tt-probes] installed-title probes PASSED\n");
  return failures ? 1 : 0;
}
