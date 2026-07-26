# Table Tennis Recomp

An in-progress native recompilation of the Xbox 360 version of
*Rockstar Games Presents Table Tennis*, built on the
[ReXGlue SDK](https://github.com/rexglue/rexglue-sdk).

**Status: bring-up. Nothing runs yet.** The SDK builds and the disc image
parses; recompilation has not produced a working executable.

This project contains no retail game code or assets. To build or run it you
must supply files from your own legally obtained copy of the game.

## Approach

This is static recompilation, not emulation. The game's PowerPC executable
(`default.xex`) is translated ahead of time into C++ source, which is then
compiled natively for the host. The SDK supplies the runtime around that
translated code: guest memory, Xbox kernel and XAM high-level emulation,
threading, filesystem, input, audio, and the graphics backends.

The structure follows [skate3recomp](https://github.com/mchughalex/skate3recomp),
which uses the same SDK.

## Disc layout

The dump is an XGD2 image; the game partition begins at `0xFD90000`.

| Path | Size | Notes |
| --- | --- | --- |
| `/default.xex` | 6.1 MB | the title executable - the recompilation input |
| `/assets/` | - | `assets.rpf` archive, audio, resources |
| `/movies/` | ~6.6 GB | Bink video, three variants per clip (NTSC/PAL/wide) |
| `/$SystemUpdate/` | 1.7 MB | console system update, not used |

Compared to Skate 3 this is a simple target: one executable, no secondary
module (Skate 3 also recompiles `EAWebkit.xex`), and no title update to patch
in. The executable imports only `xboxkrnl.exe` and `xam.xex`.

XEX details: `XEX2`, image base `0x82000000`, 16 optional headers.

## Layout

```
config/     function boundary overrides fed to codegen
game/       extracted disc files (gitignored; supply your own)
manifests/  codegen manifest templates
src/        host-side glue code
third_party/rexglue-sdk   the SDK, as a git submodule
tools/      xdvdfs_extract.py - extracts files from the disc image
```

## Extracting the disc

`tools/xdvdfs_extract.py` reads XGD1/2/3 images directly; no external tools
needed.

```sh
# see what's on the disc
python3 tools/xdvdfs_extract.py thegame.iso game --list

# pull just the executable (all that codegen needs)
python3 tools/xdvdfs_extract.py thegame.iso game --only /default.xex

# or extract everything (needs ~7 GB)
python3 tools/xdvdfs_extract.py thegame.iso game
```

## Building the SDK

macOS (Apple Silicon), with Homebrew LLVM - Apple Clang is not supported:

```sh
brew install llvm cmake ninja molten-vk

cd third_party/rexglue-sdk
LLVM=/opt/homebrew/opt/llvm@21
cmake -S . -B out/macos-rel -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_C_COMPILER=$LLVM/bin/clang \
  -DCMAKE_CXX_COMPILER=$LLVM/bin/clang++ \
  -DCMAKE_AR=$LLVM/bin/llvm-ar \
  -DCMAKE_RANLIB=$LLVM/bin/llvm-ranlib \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0 \
  -DREXGLUE_BUILD_TESTS=OFF -DREXGLUE_ENABLE_TRACY=OFF
cmake --build out/macos-rel --target rexglue --parallel
```

## SDK notes

The SDK is pinned to the `skate3-sdk-clean` branch of the Skate-specific
ReXGlue fork rather than upstream. Upstream v0.9.0 rejects macOS outright
(`"ReXGlue supports Windows and Linux only"`); the fork carries the macOS ARM
support this project needs. The tradeoff is an older (0.8.0) SDK line.

Two deviations were needed to build the fork today:

- **imgui** is pinned in the fork to a commit that no longer exists upstream -
  a patched imgui that added `ImFontConfig::RasterizerGamma`. This project uses
  stock imgui v1.92.9 and drops the two `RasterizerGamma` assignments in
  `src/ui/imgui_drawer.cpp`. The setting only tuned overlay font gamma.
- Several submodules needed their pinned commits fetched explicitly, since a
  plain `--depth 1` clone lands on branch tips instead.

## Next steps

1. Run codegen against `default.xex` and work through the unresolved calls and
   bad function boundaries it reports, recording fixes in
   `config/tabletennis_functions.toml`.
2. Get the game booting under the SDK's emulated renderer. Expect missing
   kernel/XAM imports - this is a 2006 launch-window title, and most recomp
   work so far has targeted later games.
3. Only once it runs, consider a native renderer. That is a separate
   from-scratch effort per game; none of Skate 3's shader work transfers.
