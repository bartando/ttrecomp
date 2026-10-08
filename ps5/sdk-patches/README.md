# ReXGlue v0.8 PS5 adaptations

These patches target this project's SDK commit
`beacb3b`, not upstream ReXGlue v0.10. Apply them to a separate
SDK checkout while the PS5 port is experimental:

```sh
git -C out/ps5-port/rexglue-sdk apply --check \
  "$PWD/ps5/sdk-patches/0001-platform-and-faults.patch"
git -C out/ps5-port/rexglue-sdk apply \
  "$PWD/ps5/sdk-patches/0001-platform-and-faults.patch"
```

The first patch supplies PS5 platform detection and a signal handler adapted
from [holdmysocks/mcla-recomp](https://github.com/holdmysocks/mcla-recomp),
revision `b5765a912a8efc65a3fc1753237831c9c229a4ab`, under
GPL-3.0-or-later. Original third-party notices remain intact.
It also uses the compiler's `ffs` builtin for register-mask iteration, since
the native-title SDK has no libc `ffs` export.

The handler's scalar context offset is 0x40. On the tested firmware 13.60,
the FXSAVE area begins at machine-context +0x100, despite the public payload
SDK's +0xE0 declaration. The adapted handler uses the measured offset.
`ps5/probes/rex_faults.cpp` passed write-watch retry, RAX/RIP writeback,
and XMM0/XMM15 read/writeback on the console. Other firmware and native-title
execution still need verification.

This patch alone is not a complete PS5 SDK: it does not add CMake selection,
memory backing, threading, input, audio, or Vulkan presentation. It has not
been applied to the macOS SDK submodule.

`0002-native-title-runtime.patch` adds the native-title runtime: static SDK
linkage, a direct-memory arena outside the GPU address window, direct-memory
thread stacks, POSIX/libc adaptations, SDL's offscreen event context and
RADV display presentation. It preserves this project's v0.8 frame pacing and
performance changes. Native titles use an explicit file-descriptor log sink;
early SDK logging does not assume stdout is available.

Apply the first two patches in order to a separate SDK clone. Then apply
`0003-ffmpeg-config.patch` **inside its `thirdparty/FFmpeg` directory**. That
dependency patch selects the PS5 configuration without changing desktop
configurations. The main patches were checked against a fresh archive of
`beacb3b`; the FFmpeg patch was checked against the existing dependency.

`0004-host-thread-fpscr.patch` initializes the guest FPSCR on host threads.
The audio worker runs the game's callback on a host thread whose context never
called `InitHost`, so the first `enableFlushModeUnconditional` loaded MXCSR 0
and unmasked every x86 FP exception; the console title died with SIGFPE in
guest audio code about three seconds after launch. ARM64 hosts are unaffected
because FP traps there are opt-in.

`0005-socket-bind.patch` builds a real BSD `sockaddr_in` for `bind` (the SDK's
native struct has a 16-bit family where BSD has `sin_len` and `sin_family`),
retries on an ephemeral port when FreeBSD refuses a reserved port (titles are
not root; Table Tennis binds VDP port 1001), and logs errno for failing
`socket`, `bind` and `ioctlsocket` calls. `FIONBIO` goes through `fcntl`
because the title sandbox refuses socket `ioctl` with `EACCES`.

`0006-thread-cpu-diagnostics.patch` adds a `PERF threads` line after every
`PERF summary`: whole-process CPU and the busiest registered threads, where
100% is one core. Threads register in the POSIX start routine; the PS5 host
registers its own main thread. libkernel's `pthread_getcpuclockid` returns a
clock that reads the calling thread, and libc's `clock_getcpuclockid2` is not
linkable, so the patch builds FreeBSD's thread CPU clock id
(`0x80000000 | tid`) itself.

`0007-write-watch-batching-and-gpu-profile.patch` makes write-watching
affordable when `mprotect` costs ~33 us per call, as it does on the console.

- **Batched GPU uploads.** An upload grows over neighbouring invalid pages
  inside the CPU invalidation widening window (`WidenUploadRanges`). A window
  opened by one guest write fault is therefore re-uploaded and re-protected
  with one call instead of one per page. Only committed guest memory is added.
  With the desktop default (one 16 KB page) nothing changes.
- **Diagnostics.** With `gpu_cpu_profile`, the patch adds `CPU profile detail`,
  `CPU upload shape` and `CPU texture detail` lines. They report:
  - global-lock wait;
  - `mprotect` time and calls by site;
  - upload contiguity and repeats;
  - texture reloads.

The PS5 host sets `shared_memory_cpu_invalidation_widen_kb=256`, which took
match frames on the GPU thread from ~36 ms to ~21 ms
(see `docs/ps5_performance.md`).

`0008-zero-copy-shared-memory.patch` lets the GPU use guest memory directly
(`vulkan_zero_copy_shared_memory`).

- **Import.** The patch enables `VK_EXT_external_memory_host` and imports the
  512 MB guest physical view as the shared memory buffer.
  `memory::Ps5GrantGpuAccess` gives that view GPU access and keeps it through
  later protection changes, where a plain `mprotect` would drop it.
- **Requests.** Requests become no-ops. Watches protect their own pages, and
  GPU writes only fire watches. CPU write-fault widening drops to one page.
- **Supporting changes:**
  - The upload shadow is allocated in capped 64-page chunks
    (`shared_memory_upload_shadow_max_mb`, 0 disables it).
  - Opt-in hot pages (`shared_memory_hot_pages`), measured slower here.
  - A table for the per-write known-register check.
  - A SIGPROF sampler for the GPU thread (`gpu_cpu_sample_hz`,
    `gpu_cpu_sample_file`; report with `ps5/tools/sample_report.py`).

The PS5 host enables zero copy, and matches run at 60 fps. Fences still
signal when the command processor reaches them rather than when the PS5 GPU
finishes, so a title that reuses buffer memory right after a fence could draw
data it already overwrote. Table Tennis showed no such artefacts.

`0009-fsr1-and-4k-display.patch` enables the presenter's FSR 1 and CAS
(`present_effect`) on the PS5. They use precompiled shaders and reimplemented
constants, so the FidelityFX SDK is only needed for the temporal FSR 2/3
runtime. The patch also sizes the console's display surface from the window
instead of a fixed 1920x1080, so the host's `ps5_display_width`/`_height`
pick the VideoOut mode (3840x2160, 2560x1440 or 1920x1080 on the tested TV).

`0010-1440p-at-60.patch` takes 2x resolution scale to a locked 60 fps in
rallies.

- **Lock-free scaled-resolve checks.** `IsRangeScaledResolved` runs for every
  texture lookup. It now reads its bitmaps without taking the global lock;
  writers update them atomically under the lock as before.
- **Hot texture pages in zero copy.** Pages the CPU write-faults on in 4
  consecutive frames turn hot. Watches on them stop write-protecting them and
  fire once per frame instead, in `OnFrameEnd`. That removed ~116 faults and
  re-protections per frame.
- **`vulkan_gpu_frame_timer`.** It logs:
  - GPU work per frame, from per-submission timestamps;
  - the first-to-last span;
  - the end-to-end GPU period.

  The fork's bucket profiler records only a run's first two frames on the
  console.
- **Deeper sampler stacks.** The sampler keeps 8 likely return addresses.

GPU work per match frame is 2.75 ms at 1x and 8.1 ms at 2x, so the GPU was
never the limit.

`0011-gpu-timeline-fences.patch` fixes stretched geometry at 2x. The guest
reuses a buffer once its `EVENT_WRITE_SHD` fence lands. With zero copy the GPU
reads guest memory directly, and the command processor wrote that fence when
it *processed* the packet, often before the GPU had read the buffer. The
fence is now a `vkCmdFillBuffer` into the imported guest memory on the GPU
timeline, between barriers. Without zero copy it falls back to the old CPU
store.

`0012-relative-source-paths.patch` keeps the builder's home directory out of
the title. `__FILE__` in asserts and log source locations had baked ~200
absolute paths into `eboot.bin`. PS5 builds now map the SDK and game source
roots to `rexglue-sdk/` and `game/`. `ps5/package_release.py` refuses to
package any file that still contains the local home path or user name.

The resulting runtime and full Table Tennis recompilation built successfully
for PS5. The host writes startup/crash logs in `/app0/tt-game.log` and
`/app0/tt-game-crash.log`; hardware startup debugging is still in progress.
These patches remain separate from the working macOS SDK submodule.
