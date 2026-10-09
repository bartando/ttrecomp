# PS5 SDK changes

The PS5 runtime is part of the SDK fork's main branch, `skate3-sdk-clean` on
[bartando/rexglue-skate3](https://github.com/bartando/rexglue-skate3). PS5 code
is behind `REX_PLATFORM_PS5`, the `PS5` CMake option, or cvars that default to
off on desktop. The commits whose subjects start with `ps5:` explain each
change.

Build the PS5 title from a separate checkout at the commit pinned by
`third_party/rexglue-sdk`. That keeps the FFmpeg patch and the PS5 build tree
out of the desktop submodule:

```sh
git clone https://github.com/bartando/rexglue-skate3.git out/ps5-port/rexglue-sdk
git -C out/ps5-port/rexglue-sdk checkout "$(git rev-parse HEAD:third_party/rexglue-sdk)"
git -C out/ps5-port/rexglue-sdk submodule update --init --recursive
git -C out/ps5-port/rexglue-sdk/thirdparty/FFmpeg apply \
  "$PWD/ps5/sdk-patches/ffmpeg-config.patch"
```

`ffmpeg-config.patch` selects the PS5 configuration inside the FFmpeg
dependency without changing desktop configurations. It is the only change
kept as a patch, because FFmpeg is a third-party submodule.

## Changes active on desktop

Most PS5 code is compiled out or switched off on desktop. These parts of the
landed commits also change desktop behavior:

- **Upload shadow cap** ("ps5: zero-copy shared memory"). The CPU copy of
  uploaded pages used to be a single 512 MB allocation. It is now allocated in
  64-page chunks and capped by `shared_memory_upload_shadow_max_mb`, default
  64. Past the cap, the least recently used chunk is freed. After that, an
  upload to one of its pages that was already accessed earlier in the
  submission can't be hoisted. `-1` removes the cap; `0` disables the copy.
- **Upload range widening** ("ps5: batch write-watch uploads and profile the
  GPU thread"). Uploads now widen to the same window that CPU write faults
  already used for invalidation, `shared_memory_cpu_invalidation_widen_kb`
  (default 16). Already valid pages, hot pages, and uncommitted guest memory
  are skipped. That window is 4 pages on x86-64 Windows and Linux (4 KB pages), but
  only one page on Apple silicon (16 KB pages), so macOS behavior is unchanged.
- **Lock-free scaled-resolve lookups** ("ps5: 2x resolution at a locked 60").
  `TextureCache` updates the scaled-resolve page bits atomically.
  `IsRangeScaledResolved` and `IsRangeFullyScaledResolved` read them without
  taking the global lock. A concurrent change can make a lookup stale, but
  only as stale as an answer read under the lock and used after it is
  released.
- **`VK_EXT_external_memory_host`** ("ps5: zero-copy shared memory"). The
  extension is enabled on every Vulkan device that supports it, including
  MoltenVK. Desktop only uses it when `vulkan_zero_copy_shared_memory` is on,
  and that is off by default.

Smaller changes: CPU profiling counters, which record per-owner texture and
upload bytes. `RegisterFile::IsKnownRegister` now builds a lookup table once.

Before landing, the Windows build held a locked 60 fps both before and after
these commits. The macOS build matched the old build too, apart from
GPU-bound runs that appeared with both builds.

## Keeping it current

SDK changes for any platform go to `skate3-sdk-clean`. After a change touches
shared code, build and run both a desktop build and the PS5 title before
pushing.

The PS5 changes landed on the desktop branch on 2026-10-09. Before that, they
were patch files against SDK commit `beacb3b`, then a separate `ps5` branch.

## Third-party code

The platform detection, the fault handler (`src/core/exception_handler_ps5.cpp`)
and the direct memory arena in `src/core/memory_posix.cpp` are adapted from
[holdmysocks/mcla-recomp](https://github.com/holdmysocks/mcla-recomp),
revision `b5765a912a8efc65a3fc1753237831c9c229a4ab`, under
GPL-3.0-or-later. Original third-party notices remain intact. All of it is
inside `#if REX_PLATFORM_PS5`, so desktop builds contain none of it.

The handler's scalar context offset is 0x40. On the tested firmware 13.60,
the FXSAVE area begins at machine-context +0x100, despite the public payload
SDK's +0xE0 declaration. The adapted handler uses the measured offset.
`ps5/probes/rex_faults.cpp` passed write-watch retry, RAX/RIP writeback,
and XMM0/XMM15 read/writeback on the console. Other firmware still needs
verification.
