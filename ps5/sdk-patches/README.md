# PS5 SDK changes

The PS5 runtime lives on the `ps5` branch of the SDK fork,
[bartando/rexglue-skate3](https://github.com/bartando/rexglue-skate3/tree/ps5):
the desktop branch `skate3-sdk-clean` plus one commit per PS5 change. Each
commit message explains its change. Build the PS5 title from a separate
checkout of that branch, not from the desktop submodule:

```sh
git clone -b ps5 https://github.com/bartando/rexglue-skate3.git \
  out/ps5-port/rexglue-sdk
git -C out/ps5-port/rexglue-sdk submodule update --init --recursive
git -C out/ps5-port/rexglue-sdk/thirdparty/FFmpeg apply \
  "$PWD/ps5/sdk-patches/ffmpeg-config.patch"
```

`ffmpeg-config.patch` selects the PS5 configuration inside the FFmpeg
dependency without changing desktop configurations. It is the only change
kept as a patch, because FFmpeg is a third-party submodule.

## Keeping the branch current

Fixes that matter on every platform go to `skate3-sdk-clean` first; the PS5
branch then picks them up by rebasing:

```sh
git -C out/ps5-port/rexglue-sdk fetch origin
git -C out/ps5-port/rexglue-sdk rebase origin/skate3-sdk-clean
```

Git drops PS5 commits whose change is already on the desktop branch. Rebuild
and run the title on the console before pushing the rebased branch.

The branch first went onto the desktop branch on 2026-10-09. Before that,
the changes were patch files against SDK commit `beacb3b`; the host-thread
FPSCR fix, FSR 1 without the FidelityFX SDK, the public
`REX_HAS_FIDELITYFX_SDK` define and System Link networking have since moved
to the desktop branch.

## Third-party code

The platform detection and fault handler in the first commit are adapted from
[holdmysocks/mcla-recomp](https://github.com/holdmysocks/mcla-recomp),
revision `b5765a912a8efc65a3fc1753237831c9c229a4ab`, under
GPL-3.0-or-later. Original third-party notices remain intact.

The handler's scalar context offset is 0x40. On the tested firmware 13.60,
the FXSAVE area begins at machine-context +0x100, despite the public payload
SDK's +0xE0 declaration. The adapted handler uses the measured offset.
`ps5/probes/rex_faults.cpp` passed write-watch retry, RAX/RIP writeback,
and XMM0/XMM15 read/writeback on the console. Other firmware still needs
verification.
