# PS5 CPU and fault-handler probes

`platform.cpp` and `fault_sites.S` check CPU-side requirements without loading
the game, submitting GPU commands, writing files or changing console settings.
The payload reserves a 4.5 GiB virtual range outside RADV's GPU address window,
allocates **1 MiB** of direct memory, maps it through six owned aliases, then
tests write-watch retry and emulated-load register writeback. It releases the
views and physical allocation before normal exit.

This does not allocate 4.5 GiB of physical memory, implement the complete
ReXGlue arena, or verify an installed title's memory budget. The prototype maps
the same small backing through both executable aliases and physical aliases
only to check the kernel's aliasing behaviour; the production arena uses
separate offsets for those regions.

The fault sites have known instruction addresses and a known R12 marker.
The handler checks both public SDK context offsets (0x10 and 0x40) against
those values before accessing saved registers for writeback. An unknown
context or unexpected fault terminates this payload instead of guessing.
This probe verifies RAX/RIP writeback, not XMM register writeback.

## Rebuild

For the current local toolchain, from the repository root:

```sh
LLVM_CONFIG=/opt/homebrew/opt/llvm@21/bin/llvm-config \
PS5_PROBE_LINKER="$PWD/out/ps5-probe/linker21/lld/21.1.7/bin/ld.lld" \
out/ps5-probe/ps5-payload-sdk/bin/prospero-clang++ \
  -std=c++23 -Wall -Wextra -Werror -O2 -ffile-prefix-map="$PWD"=. \
  -I out/ps5-port/sdk-source/platform/include \
  ps5/probes/platform.cpp ps5/probes/fault_sites.S \
  -o out/ps5-probe/tt-ps5-platform.elf
```

The public payload SDK is v0.43. The additional kernel declarations come from
the GPL-3.0-or-later platform headers in
[mihawk-99/PS5_PayloadSDK](https://github.com/mihawk-99/PS5_PayloadSDK), pinned
at `b83202be73e930050e00de0f1b4d9ec46c0391bf`. The ignored local linker
wrapper setup is described in `out/ps5-probe/README.md`.

Before uploading, check that the ELF is x86-64 PIE, every executable section
and the entry point belong to an executable load segment, and load-segment
alignment is 16 KiB. These checks passed for the tested payload.

```sh
python3 ps5/probes/check_elf.py out/ps5-probe/tt-ps5-platform.elf
```

The checker rejects the known MCLA linker failure where an executable section
ends up in a non-executable load segment. It also checks segment bounds and
file/address alignment; it does not establish that imports resolve on hardware.

## Run

With the console's `elfldr` running:

```sh
python3 ps5/probes/send.py --host PS5_IP \
  --payload out/ps5-probe/tt-ps5-platform.elf \
  --log out/ps5-probe/platform-console.log
```

The sender requires the exact final zero-failures line. A successful transfer
or a clean local link is not a successful hardware test.

## Observed on the user's console

On 2026-10-08, on firmware **13.60 (user-reported)**, this payload returned:

```text
Host page size: 16384
PASS: 4.5 GiB virtual arena at 1000000000, outside GPU window
PASS: all six aliases, including E-range 4 KiB address bias
PASS: write-watch retry; machine-context offset=0x40
PASS: fault handler wrote RAX and RIP back correctly
TT PS5 platform probe: finished with 0 failures
```

The full console output is under ignored
`out/ps5-probe/platform-console.log`. Both the offset and register writeback
were measured; the offset was not assumed from another firmware.

This work follows the memory/fault findings in
[holdmysocks/mcla-recomp](https://github.com/holdmysocks/mcla-recomp), reviewed
at `b5765a912a8efc65a3fc1753237831c9c229a4ab`. The probe source is
project-authored GPL-3.0-or-later code, contains no game code or data, and is
not part of the macOS build.

## Actual ReXGlue handler and SIMD layout

`rex_faults.cpp` tests the PS5 handler adapted into an isolated clone of this
project's v0.8 SDK. The patch is preserved under `ps5/sdk-patches/`; apply it
to a clean clone at the base commit documented there, not the macOS submodule.

```sh
LLVM_CONFIG=/opt/homebrew/opt/llvm@21/bin/llvm-config \
PS5_PROBE_LINKER="$PWD/out/ps5-probe/linker21/lld/21.1.7/bin/ld.lld" \
out/ps5-probe/ps5-payload-sdk/bin/prospero-clang++ \
  -std=c++23 -Wall -Wextra -Werror -O2 -ffile-prefix-map="$PWD"=. \
  -march=znver2 -I out/ps5-port/rexglue-sdk/include \
  ps5/probes/rex_faults.cpp ps5/probes/fault_sites.S \
  out/ps5-port/rexglue-sdk/src/core/exception_handler_ps5.cpp \
  out/ps5-port/rexglue-sdk/src/core/math_gcc.cpp \
  -o out/ps5-probe/tt-ps5-rex-faults.elf

python3 ps5/probes/send.py --host PS5_IP \
  --payload out/ps5-probe/tt-ps5-rex-faults.elf \
  --log out/ps5-probe/rex-faults-console.log \
  --expect 'TT PS5 ReX fault probe: finished with 0 failures'
```

The tested console session returned (firmware discrepancy noted below):

```text
PASS: ReXGlue write-watch retry
PASS: ReXGlue RAX/RIP writeback
PASS: ReXGlue XMM0/XMM15 read and writeback
TT PS5 ReX fault probe: finished with 0 failures
```

`simd_context.cpp` diagnosed a discrepancy in the public SDK header: its
FXSAVE area is declared at machine-context +0xE0, but this console saves it
at +0x100. It searches only for a deliberately loaded register marker and
reports the matching offsets, without dumping arbitrary saved state.
Build it with the same compiler flags and `fault_sites.S`, without the SDK
handler or math source; its success line is
`TT PS5 SIMD context probe: finished with 0 failures`.
The corrected handler reads and writes the measured FXSAVE area. The marker
diagnostic also verified that the kernel restores the original XMM0/XMM15
values after a fault. The subsequent installed-title run verified the same
scalar and SIMD read/write offsets there too.

## Installed-title test

`native_main.cpp` runs all three CPU probes from a home-screen title and writes
`/app0/tt-ps5-probes.txt`. The title ID in `param.json` is **PPSA99780**.
No game files, GPU submissions or console configuration changes are involved
in the probe itself. All three probes completed with zero failures in the
installed-title process on 2026-10-08. The results are preserved in the
ignored `out/ps5-port/native-probes/native-console.log`.

The first harness then crashed **after completing the tests**: the kernel
reported signal 12 (`SIGSYS`), matching the public native runtime's documented
exit failure when a title returns from `main`. ShadowMountPlus confirmed the
crash preceded its scheduled Kstuff pause. `native_main.cpp` now overrides
the CRT's `catchReturnFromMain` hook to sleep until the shell closes the game,
on success or failure. It writes a liveness checkpoint after 25 seconds.
The updated harness passed its hardware rerun with zero failures and wrote the
25-second liveness checkpoint. ShadowMountPlus recorded a 152-second session
followed by title/sandbox cleanup, without the previous immediate crash.
It intentionally has no renderer, so close it using the PS menu after the
checks rather than waiting for gameplay to appear.

The build uses the pinned public SDK and PS5_Vulkan native tools documented in
`docs/ps5_runtime_reference.md`, including the reproducibly rebuilt runtime
shim. With those prerequisites prepared in the isolated Arch builder, stage
the project sources from the repository root:

```sh
mkdir -p out/ps5-port/probe-source
cp ps5/probes/*.cpp ps5/probes/*.S ps5/probes/check_elf.py \
  out/ps5-port/probe-source/
cp assets/icon/tabletennis_icon.png out/ps5-port/probe-source/icon0.png
cp ps5/probes/param.json out/ps5-port/probe-param.json
cp ps5/probes/build_native.sh out/ps5-port/build-native-probes.sh
docker exec ttrecomp-ps5-build bash /work/build-native-probes.sh /work
```

The output is `out/ps5-port/dist/PPSA99780/`; the ELF layout checker runs before
and after native conversion. The title was uploaded to
`/data/homebrew/PPSA99780` and registered on 2026-10-08. The console's FTP
service unwraps SELF files and clears their `PT_SCE_VERSION` metadata.
Readback verification established that ELF code/data were otherwise identical;
the icon and JSON metadata were byte-identical.

The first registration attempt failed with **TitleDir bridge unavailable**.
ShadowMountPlus's log showed its AppInstallAll hook had disappeared since
startup. Reloading the same official **1.7beta1** payload restored the hook
and registered this title with result `0x00000000`. Console settings were not
edited. The release archive SHA-256 is
`46e55f698935424cf38d03985c4f56e3e6fd871a4315b64ba5542ef9e1232199`.

Firmware caveat: the user reported 13.60, but the live ShadowMountPlus log
reports **13.42** and resolves its 13.42 ShellCore offsets. The discrepancy
has not been resolved. All measured signal-layout results refer to this
specific console session, rather than a proven firmware-wide ABI.

## Vulkan device probe

`vulkan_info.cpp` links the pinned static PS5 RADV driver directly and checks
instance/device creation, the renderer's baseline features, graphics queue,
swapchain/push-descriptor extensions, and CPU alias/fault recovery while the
Vulkan device is alive. The first hardware run passed all of these with zero
failures, reported RADV Vulkan 1.4.354, and remained open until title cleanup
after 67 seconds. Its result is saved in the ignored
`out/ps5-port/vulkan-probe/device-only-console.log`.

The next revision also compiles and dispatches `compute.comp`: four bounded
workgroups write a 256-word integer pattern into a coherent storage buffer.
`compute_probe.cpp` waits for a fence (five-second timeout), then verifies
every word on the CPU. A timed-out submission retains its resources until
system close. This revision passed on the console: pipeline compilation,
submission, fence completion and all 256 output words were correct, with zero
failures. The log is `out/ps5-port/vulkan-probe/compute-console.log`.

The next revision enables `VK_KHR_display`, presents one blue frame, and checks
the first and last image pixels using transfer readback. It hides the native
launch splash with a strong system-module import and retains the swapchain,
device and image resources until system close. This display revision built
successfully and was uploaded after the previous probe's sandbox was released.
Readback verified every code/data byte, allowing only the FTP service's known
zeroing of `PT_SCE_VERSION` metadata. The console passed presentation and edge-pixel readback, and the user
confirmed a solid blue screen on the TV. The saved result is
`out/ps5-port/vulkan-probe/display-console.log`. The probe
writes `/app0/tt-vulkan-info.txt` and waits for system close after completion.
Successful compilation is not evidence that these checks pass on hardware.

With the same prepared builder and RADV archive, stage and build:

```sh
cp ps5/probes/vulkan_info.cpp ps5/probes/compute_probe.cpp ps5/probes/compute.comp \
  ps5/probes/display_probe.cpp ps5/probes/system_service_stub.c \
  ps5/probes/vulkan-param.json \
  ps5/probes/build_native_vulkan.sh out/ps5-port/probe-source/
docker exec ttrecomp-ps5-build bash /work/probe-source/build_native_vulkan.sh /work
```

The output uses a separate title ID, **PPSA99781**, labeled **Table Tennis
Vulkan Probe**, in `out/ps5-port/dist/PPSA99781/`. Its build script reuses
PS5_Vulkan's RADV link recipe and checks executable segment layout before
and after native conversion. The title contains no game data. Display,
and actual game rendering still need separate checks.
