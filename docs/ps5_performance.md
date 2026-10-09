# PS5 performance notes

Measured on the jailbroken PS5 (firmware 13.60, RADV) with the full Table
Tennis title, October 2026. Menus run at 60 fps. Matches run at 23–31 fps.

## Why matches drop to ~30 fps

The swapchain only offers FIFO, so any frame over 16.7 ms waits for the next
vblank and the counter snaps to 30 (or 20). Game speed is tied to frame rate,
so a 30 fps match plays in half-speed slow motion.

The limit is the GPU command thread, not the GPU:

- `gpu_wait_ms` stays at 0.
- Per-thread CPU (`PERF threads`) shows the command thread at 100% from the
  moment a match starts, against 11–30% in menus.
- The process as a whole uses ~3.7 cores.
- Hooks are not the cause: a `TABLETENNIS_PS5_NATIVE_HOOKS=OFF` build gave
  identical fps.

The thread spends a ~38 ms match frame like this (`gpu_cpu_profile`):

| Cost | Per frame |
|---|---|
| Shared-memory requests | ~21 ms |
| `mprotect` alone | ~14 ms, ~410 calls |
| Spinning on the global lock | ~9.5 ms, ~330 contended acquisitions |

## Root cause: write-watch churn with expensive `mprotect`

`mprotect` costs **~33 µs per call** on the console, whatever the mapping:
anonymous memory, the direct-memory guest arena, 1 page or 64 pages
(`ps5/probes/mprotect_bench.c`). macOS is ~1 µs. So the cost is per call, not
per page.

Matches rewrite dynamic vertex and index data every frame. Every rewritten
16 KB host page goes through a loop:

1. The guest writes the page, which faults.
2. The fault handler takes the global lock and unprotects the page.
3. The GPU thread uploads the page and write-protects it again.

Measured per frame (`CPU upload shape`):

- ~450 guest-fault unprotects and ~410 GPU re-protects, ~860 `mprotect` calls
  in total.
- ~260 distinct pages uploaded ~450 times: ~190 uploads repeat a page within
  the same frame. The guest keeps filling a page after the GPU has re-armed
  its watch.
- Pages form ~135 runs, averaging 2 pages (longest 15).

Both sides hold the global lock across their `mprotect`, so each side spins
on the other.

Ruled out:

- Write-combined upload memory being read back by the upload shadow. Staging
  through cached memory changed nothing.
- The direct-memory aliases. Anonymous memory costs the same.

## Fix so far: fewer, wider protections

Two changes cut the `mprotect` calls:

1. Widen CPU write-fault invalidation, so one fault unprotects a whole window.
2. Make GPU uploads cover the whole invalid run inside that window, so one
   call re-protects it (`WidenUploadRanges`, SDK commit "ps5: batch
   write-watch uploads and profile the GPU thread").

The window size trades `mprotect` calls against extra copying. Each row below
is one unattended 35–40 s match:

| Window | GPU-thread frame | Match fps | `mprotect`/frame | Pages uploaded/frame |
|---|---|---|---|---|
| 16 KB, before batching | ~36 ms | 26–30 | ~860 | ~450 |
| 64 KB | ~24 ms | — | ~420 | ~760 |
| 128 KB | ~21.5 ms | — | ~315 | ~1030 |
| **256 KB** (PS5 default) | **~20.6 ms** | **~48** | ~245 | ~1500 |
| 512 KB | 20–24 ms | 45–51 | ~200–240 | ~1900–2500 |
| 1 MB | ~23 ms | ~43 | ~170 | ~3900 |

At 256 KB, what remains of the ~20.6 ms frame:

- **Texture requests, ~6 ms.** About 140 texture reloads (~53 MB) per frame,
  with ~39 resolves per frame.
- **Index buffers, ~3.4 ms.**
- **Global-lock wait, ~2.2 ms.**
- **Repeat uploads.** About 560 pages per frame are uploaded again within the
  same frame. The game keeps writing pages after the GPU has re-armed their
  watch.

## Zero copy: 60 fps

The console's CPU and GPU share one memory, and its RADV supports
`VK_EXT_external_memory_host`. RADV's notes measure that memory as cached and
coherent both ways.

With `vulkan_zero_copy_shared_memory` (the PS5 default since 2026-10-08), the
shared memory buffer is the guest's physical memory itself. Nothing is
uploaded, and vertex and index data need no write-watching.

| | Copying (256 KB window) | Zero copy |
|---|---|---|
| GPU-thread frame | ~20.3 ms | ~16.7 ms (waits ~2.3 ms for the guest) |
| Index buffers (`prim`) | 3.4 ms | 0.17 ms |
| Vertex buffers (`vb`) | 1.4 ms | 0.27 ms |
| Uploads | ~1500 pages | none |
| `mprotect` per frame | ~245 | ~200 |
| Texture reloads per frame | ~120 | ~31 |
| Match | ~49 fps | **60 fps, no hitches** |

Watches keep 256 KB fault widening off in this mode. With it on, every write
fault near a texture fired that texture's watch: ~133 reloads per frame and a
9.5 ms texture stage.

**Known gap.** Guest fences (`EVENT_WRITE_*`) are signalled when the command
processor processes them, not when the PS5 GPU has executed the draws before
them. A title that overwrites buffer memory as soon as a fence passes could
show stale or garbled geometry. A full match of Table Tennis looked correct to
the eye. The fix would move fence writes onto the GPU timeline.

## Image quality: FSR 1 to 4K

Defaults since 2026-10-08:

- 720p guest output, upscaled with FSR 1 (`present_effect = "fsr"`) to a
  native 3840x2160 swapchain.
- Matches hold 60 fps.
- The 4K swapchain and FSR cost nothing measurable on the GPU thread.

2x internal resolution (`resolution_scale = 2`, 2560x1440) looks noticeably
sharper. It runs ~58 fps with the player standing at the serve, but only
50–56 fps in real rallies (`tabletennis_test_rally`, now the run_match.py
default). The 1x default holds 59.2–60 in rallies. The GPU thread then waits ~2.2 ms a frame in
`vkQueueSubmit`, against ~0.13 ms at 1x, so the PS5 GPU is the limit there.
Its timestamp queries produce no GPU profile on this driver.

At 2x, the fork's `draw_resolution_scaled_half_pixel_offset` (half a host
pixel) left the right and bottom edge of every resolve uncovered:

- a ~10 px garbage strip on the right edge of the 4K frame;
- a cyan fringe where the game blurs it (pause menu).

The PS5 host turns it off, falling back to the standard half-guest-pixel
offset with edge fill. Edge discontinuity in screenshots dropped from 128-190
to 0.4-1.9.

Update: 2x now holds 60.00 fps in rallies (SDK commit "ps5: 2x resolution
at a locked 60").

- **The GPU was never the limit.** `vulkan_gpu_frame_timer` measures 2.75 ms
  of GPU work per frame at 1x and 8.1 ms at 2x.
- **The CPU thread was the limit.** At 2x it lost time to:
  - texture-watch ping-pong: ~116 faults and re-protections per frame, ~4 ms;
  - global-lock spinning in `IsRangeScaledResolved`, ~3 ms.
- **After the fix:** `mprotect` is about 0, the texture stage dropped from
  7.5 ms to 1.6 ms, and the GPU thread waits ~3.5 ms per frame for the game.
- **Submit cost.** `vkQueueSubmit` still takes ~2.8 ms per frame at 2x, inside
  the driver around `sceAgcDriverSubmitDcb`. RADV's own submit statistics go
  to stderr, which a retail title loses.

2x is the PS5 default since 2026-10-08 (`ps5/game/main.cpp`). The stretched
geometry first seen there was fixed by SDK commit "ps5: write
EVENT_WRITE_SHD fences on the GPU timeline".

Earlier note: getting 2x to a locked 60 needs GPU-side numbers. Timestamp queries
return valid values (10 ns ticks), but the fork's per-frame GPU profiler
records only the first two frames of a run, for reasons not yet found.

PS5 screenshots are JPEG XR files under
`/user/av_contents/photo/NPXS40087/PPSA99782/`. Decode them with Python's
`imagecodecs.jpegxr_decode`.

## Tooling

- `ps5/run_match.py --host <ip> --seconds 40 --cvar gpu_cpu_profile=true
  --log <file>` runs a match unattended:
  - It writes `ttrecomp/ps5.toml` with `tabletennis_test_path = true`, which
    makes the title drive itself into an Exhibition match (~50 s).
  - It adds `tabletennis_test_rally = true`, which keeps tapping A in the
    match so the measurement covers rallies. `--no-rally` turns this off.
    Rallies cost noticeably more than standing still.
  - It launches and kills through elfldr payloads (`ps5/tools/title_ctl.c`).
  - It downloads the log.
- The launch payload first calls ShadowMountPlus
  `POST 127.0.0.1:10101/api/v1/games/mount`. Its ShellCore launch hook can
  stop working, typically after a reboot. The launch then fails with
  `0x80940033`, and the TV shows CE-105773-3.
- Cvars in `ttrecomp/ps5.toml` load at startup (flat `name = value`).
- Log lines:
  - `PERF threads`: per-thread CPU, where 100% = one core.
  - `CPU profile`: per-stage GPU-thread time.
  - `CPU profile detail`: lock wait, `mprotect`, uploads.
  - `CPU upload shape`: distinct pages, runs, repeats, and `mprotect` calls by
    site.
  - `CPU texture detail`: texture requests, reloads and their cost.
- `gpu_cpu_sample_hz=997` and `gpu_cpu_sample_file=/app0/tt-samples.bin`
  sample the GPU thread's instruction pointer. Download the file and run
  `ps5/tools/sample_report.py <samples> out/ps5-port/game-title/llvm-pie.elf
  40 30000` to rank functions over the last ~30 s.

Never busy-spin threads in an elfldr payload. Payloads outrank the system
shell, FTP and input, and 12 spinning threads froze the console until a hard
power-off.
