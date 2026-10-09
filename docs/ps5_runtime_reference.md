# PS5 runtime reference: MCLA and RADV

Source review on 2026-10-08. This is a porting reference, not a successful
Table Tennis boot or a PS5 release.

## How MCLA launches the game

[holdmysocks/mcla-recomp](https://github.com/holdmysocks/mcla-recomp), inspected
at `b5765a912a8efc65a3fc1753237831c9c229a4ab`, uses the same ReXGlue family
as this project. It is a native ahead-of-time recompilation, not an emulator
installed in place of the recompilation.

Its actual launch chain is:

1. Cross-compile the generated guest C++ and patched ReXGlue runtime to PS5
   x86-64 with the public payload SDK fork.
2. Statically link the runtime and Mesa RADV PS5 driver into a native title;
   convert the executable to `eboot.bin`, with title metadata and the open
   runtime shim.
3. Upload the title to `/data/homebrew/<TITLEID>` over FTP. Upload the user's
   extracted retail game separately; MCLA uses `/data/mcla/game`.
4. ShadowMountPlus registers the title on the PS5 home screen. The user
   launches that tile. The host creates the Vulkan presenter, hides the
   shell's launch splash, constructs `rex::Runtime`, loads `default.xex`,
   prepares the guest main thread, and resumes it.

The source paths containing those steps are `ps5/game/main_ps5.cpp`,
`ps5/game/build.sh`, `ps5/title_build.sh`, and `ps5/make_ps5.sh` in that
repository. Local reference copies are under ignored
`out/ps5-port/reference/mcla-recomp/`.

The full graphics path needs an installed title: the driver uses the
console's AGC/VideoOut exports in that process. `elfldr` is useful for CPU,
memory and fault-handler milestones; sending the existing macOS executable
or a CPU-only ELF will not supply the game renderer.

## What its platform port supplies

- PS5 platform detection and static runtime/GPU linkage.
- Guest virtual-memory aliases, 16 KiB host-page handling, and direct-memory
  backing when a title's small flexible-memory budget cannot hold the arena.
- PS5 signal-context handling, POSIX threading and libc adaptations.
- Vulkan dispatch directly through RADV's `vk_icdGetInstanceProcAddr`;
  `VK_KHR_display` surfaces and swapchains instead of desktop window surfaces.
- DualSense input through `scePad*` and stereo output through `sceAudioOut*`.

These are implemented in the SDK's `ps5` branch and the PS5 host, rather than by replacing
the Xbox 360 guest code. Its published results report 102 passed / 0 failed
RADV smoke checks on a PS5 Pro, firmware 13.42. Its README reports playable
MCLA on Pro 13.42 and Slim 12.70. These are upstream evidence, not tests on
the requesting user's PS5 or proof of Table Tennis compatibility.

## RADV and our renderer

[mihawk-99/PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan), inspected at
`5b5e4fc2d80fdb67a4f61ba9e6a8a424026bb8a5`, pins PS5 Mesa
`7b59ef27c1b09b9671bc4153c41940c3155c3af2` and payload SDK fork
`b83202be73e930050e00de0f1b4d9ec46c0391bf`.

The Mesa source advertises the features checked by our Vulkan device setup:
independent blending, vertex/fragment stores and atomics, geometry shaders
and non-solid fill. It supplies swapchains, push descriptors, and the
PS5-specific VideoOut display backend. That makes it a plausible backend for
our existing Vulkan renderer; feature advertisement still needs checking on
hardware with our shaders and workload.

The ordinary desktop Vulkan-loader path in our SDK and its desktop surface
types need PS5 implementations. The reference already demonstrates both.

## Compatibility and next verification

MCLA's patch targets ReXGlue v0.10.0; our SDK is a modified v0.8 tree.
`git apply --check` rejects the complete patch. Checking its 47 file sections
individually finds 20 whose context applies and 27 needing adaptation. That
is a context check only, not a build or correctness test. The complete patch
was not applied. In particular, MCLA-specific hooks and performance tuning must not
be transplanted as general Table Tennis fixes.

The pinned platform library cross-compiled on this Mac with LLVM 21 and GNU
Make. The Table Tennis PS5 title is not built yet.
The upstream build procedure assumes Arch Linux; a Mac build requires host
toolchain adaptation. A native Mesa host-tools configure reached LLVM 21
successfully, then stopped because matching `LLVMSPIRVLib` was absent. This
dependency is required to generate the driver's shader kernels; it is the
dependency that required the Linux builder, not evidence of a console incompatibility.

An isolated x86-64 Arch Linux Docker builder was subsequently started, with
only `out/ps5-port` mounted, a four-CPU / 8 GiB limit, and no privileged mode.
Its pinned base image is
`archlinux@sha256:4e77cf2ea5f410e6f8be5abf93ccf17ce2436e87a138c167208356711a405dbd`.
The host shader tools compiled there with matching Arch LLVM/SPIR-V
dependencies, and the 793-step RADV archive build completed successfully.
The resulting static archive is `out/ps5-port/arch-radv-build/src/amd/vulkan/libvulkan_radeon.a`.
Pacman's download sandbox is disabled for
that package-install invocation because its seccomp setup fails under x86
emulation; Docker's container isolation remains enabled.

Before game execution, verify the complete guest arena and aliases in an
installed-title process, then fault-handler register writeback on the actual
console session, then Vulkan device/display/readback checks. Our earlier successful
elfldr probe verifies a virtual reservation and protection changes only;
it does not establish the installed-title memory budget or GPU coexistence.

A subsequent **CPU platform payload passed on the user's console**: a 4.5 GiB
virtual reservation outside the GPU window, six views of a small direct
allocation, write-watch retry, and RAX/RIP writeback. It measured the signal
machine-context offset as **0x40**. See `ps5/probes/README.md` for limits,
source/build paths and the observed output. The subsequent installed-title
run passed the same CPU memory/alias checks and the actual ReXGlue handler's
scalar/SIMD checks, all with zero failures. Full physical arena allocation
remains outstanding. Coexistence with an active Vulkan logical device
subsequently passed in the separate device probe described below.

A separate clone of our v0.8 SDK under `out/ps5-port/rexglue-sdk` now contains
the adapted PS5 exception handler. Its scalar recovery passed, but its first
SIMD test failed: the payload SDK's `mc_fpstate` offset is 32 bytes too early
on this console. A marker-only diagnostic found XMM0 at signal-context
+0x1E0 and XMM15 at +0x2D0, placing FXSAVE at machine-context +0x100.
After correcting that shared read/write offset, the **actual ReXGlue handler
passed write-watch retry, RAX/RIP writeback and XMM0/XMM15 read/writeback**
with zero failures. This is verified in both payload and installed-title
processes on the user's console session, not for other firmware. The tested adaptation
is the first commit of the SDK fork's `ps5` branch.

The PS5 platform library documents a dedicated GPU address window. CPU guest
memory placement must be tested alongside that window, rather than assuming
the kernel-selected address from the earlier CPU-only probe is suitable.

The macOS code, packaging scripts and release remain unchanged by this review.
PS5 references, tools and build outputs are under ignored `out/ps5-port/`;
the porting branches are local `ps5-prototype` branches. No console title,
game data or release was uploaded during the reference review. Subsequent
hardware work uploaded the CPU-only probe title PPSA99780 and registered it
after restoring ShadowMountPlus's stale registration hook; see the probe
README. All CPU checks subsequently passed in the installed title. Its first
harness crashed after test completion with SIGSYS in the native runtime's
known exit path. Kstuff's auto-pause had not occurred yet. The revised harness
uses the CRT's return hook to stay alive until closed by the shell. Its hardware
rerun completed all checks with zero failures and wrote the 25-second liveness
checkpoint. ShadowMountPlus recorded it running from 15:06:07 to 15:08:39,
then released its sandbox, without the previous immediate crash.

ShadowMountPlus's live log reports firmware **13.42**, whereas the user
reported **13.60**. The probe measurements describe the actual console session;
they do not independently identify its firmware version. The public runtime
shim was rebuilt twice and matched upstream's recorded SHA-256 exactly.

The user approved GPL-3.0-or-later for project-authored code. `LICENSE` and
the README record that choice; third-party licenses and notices remain intact.
Our vblank implementation was checked: both guest-clock and host-clock paths
compare absolute deadlines and cap catch-up at three vblanks per wake. The
MCLA vblank-underflow patch is therefore excluded from this port.

The installed **Table Tennis Vulkan Probe** (PPSA99781) was built and uploaded
with readback-verified code/data. Its first run created a RADV Vulkan instance
and logical device and completed with zero failures. The device reported
`PlayStation 5 GPU (RADV NAVI21)`, Vulkan 1.4.354; it supported independent
blend, fragment/vertex storage atomics, geometry shaders, non-solid fill,
swapchain and push-descriptor extensions. CPU virtual reservation, aliases
and fault recovery passed while this device was alive. The title stayed open
and its sandbox was released after a 67-second session. This run submitted
no GPU work; it does not prove shader execution or display output. Its saved
result is `out/ps5-port/vulkan-probe/device-only-console.log`.

The subsequent compute revision also passed with zero failures on the console.
It compiled a bounded integer shader, submitted four workgroups, waited for
GPU completion and verified all 256 output words on the CPU. Its result is
`out/ps5-port/vulkan-probe/compute-console.log`. This verifies this shader's
execution and coherent-buffer readback, rather than the game's renderer or
display path. The following display revision passed presentation, edge-pixel
readback and native launch-splash dismissal; the user confirmed a solid blue
screen on the TV. Its result is `out/ps5-port/vulkan-probe/display-console.log`.

The actual Table Tennis title now builds against our adapted v0.8 SDK and
is uploaded at `/data/homebrew/PPSA99782`, with code/data readback verified.
It uses native controller/audio libraries and the full guest recompilation.
Game startup and playability still await the first console run.
