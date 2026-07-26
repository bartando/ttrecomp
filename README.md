# Table Tennis Recomp

An in-progress native recompilation of the Xbox 360 version of
*Rockstar Games Presents Table Tennis*, built on the
[ReXGlue SDK](https://github.com/rexglue/rexglue-sdk).

**Status: it boots.** The game reaches its title screen and renders through
Vulkan (MoltenVK) on Apple Silicon, with keyboard input working. It has not
been played past the title screen, so most of the game is untested.

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
| `/movies/` | 434 MB | Bink video, three variants per clip (NTSC/PAL/wide) |
| `/$SystemUpdate/` | 1.7 MB | console system update, not used |

Compared to Skate 3 this is a simple target: one executable, no secondary
module (Skate 3 also recompiles `EAWebkit.xex`), and no title update to patch
in. The executable imports only `xboxkrnl.exe` and `xam.xex`.

XEX details: `XEX2`, image base `0x82000000`, 16 optional headers.

## Layout

```
config/     function boundary overrides fed to codegen
game/       extracted disc files (gitignored; supply your own)
run.sh      launcher (sets DYLD_LIBRARY_PATH, deploys the config)
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

# or extract everything (3.5 GB - the 7.3 GB image is mostly XGD2 padding)
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

## Running

`run.sh` sets `DYLD_LIBRARY_PATH` (the runtime dylib is emitted into the SDK
output dir, not next to the executable) and copies `tabletennis.toml` into the
build directory:

```sh
./run.sh
```

Settings live in `tabletennis.toml`, which the app loads from the directory
holding the executable. Edit the copy in the repo root; `run.sh` deploys it.

## Controls

Keyboard emulation is enabled via `mnk_mode = true`. It is off in the SDK by
default, which is why the title screen ignores the keyboard until it is set.

| Input | Key |
| --- | --- |
| Left stick | W / A / S / D (press: Shift) |
| Right stick | mouse (press: middle mouse) |
| A / B / X / Y | Space / C / E / F |
| LT / RT | right mouse / left mouse |
| LB / RB | Q / R |
| D-pad | arrow keys |
| Back / Start | Tab / Return |

Controllers work through the SDL backend without extra setup.

## Bring-up notes

Codegen was unusually clean for a first pass: 15,679 functions discovered with
a single unresolved call. The overrides in
`config/tabletennis_functions.toml` all address one root cause - code that is
only ever reached *indirectly* (through a vtable slot or a tail-branch from a
C++ adjustor thunk) is never registered as a function, so the first indirect
call through it aborts with "call to invalid or unregistered function".

Two aborts of this kind were hit and fixed during bring-up, at 0x8211CC58 and
0x82464E40. The current entries were found by scanning for vtable-shaped runs
of consecutive code pointers and keeping targets that start after a terminator,
decode as valid PowerPC, and are never a static branch target.

A blanket sweep of *all* 1,351 such candidates was tried and rejected - it
broke analysis with 324 unsealed functions, because the heuristic splits real
functions at internal labels. The committed set is restricted to small
branchless leaf stubs, which validates cleanly. Expect more of these to surface
as the game is played further; the loop is: run, read the aborting address from
the log, confirm the boundary by disassembling, add an entry, regenerate.

## Known gaps

- The guest `cache:` device is never mounted, so every `cache:\assets\...`
  open fails. The game continues past it, but this is unresolved.
- `__imp__Refresh` is a stub.
- Nothing beyond the title screen has been exercised.

## Next steps

1. Play further and work through the unregistered-indirect-target aborts as
   they appear.
2. Mount a `cache:` device so the game's cache probing succeeds.
3. Only once it plays properly, consider a native renderer. That is a separate
   from-scratch effort per game; none of Skate 3's shader work transfers.
