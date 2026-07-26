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
repro.sh    drive to the character-select screen and screenshot it
trace.sh    capture shaders + GPU trace for rendering bugs
src/        host-side glue code
third_party/rexglue-sdk   the SDK, as a git submodule
tools/      xdvdfs_extract.py - extracts files from the disc image
            sendkey.py - posts held key presses (see below)
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

The game is playable into a match, but with three problems.

**Everything runs too fast.** Fixed by pacing the guest vblank. With
`vsync = false` and no present limiter the vblank free-runs, so the game
advances its own clock faster than the console did - hence intro videos and
menus playing at well above normal speed. `vsync = true` plus
`vblank_host_clock_pacing = true` ties it to the display.

**Stutter when entering a new screen.** The SDK disables async shader
compilation on macOS (`async_shader_compilation` defaults to
`!REX_PLATFORM_MAC`), so every new pipeline is compiled on the frame that
first needs it. `tabletennis.toml` turns it back on, which trades the stall
for brief pop-in while pipelines warm. If MoltenVK proves unstable with it,
set it back to false - the default is presumably deliberate. `store_shaders`
keeps compiled pipelines so later runs skip the warm-up.

**Rendering at 4x the pixels.** `resolution_scale` and
`draw_resolution_scale_x/y` all default to 2 in the SDK, supersampling the
1280x720 guest framebuffer to 2560x1440. The SDK itself warns that the path
"is experimental and may not affect all titles correctly". `tabletennis.toml`
sets them to 1. This is a large fragment-cost saving and is also a candidate
explanation for the black skin - unverified, see above.

Beyond that, the frame rate is limited by this being the emulated Xenos
renderer. Skate 3's ~10x uplift on Apple Silicon came from replacing that with
a native renderer, which is a from-scratch effort per game.

**Character skin renders pure black.** Still unresolved, and now the main
open bug. Clothing, hair, shoes and the 2D portrait thumbnails all render
correctly; eyes remain faintly visible. So the skin material's colour output
is going to zero while the rest of the character is fine.

Ruled out by experiment:

- *Draw resolution scaling.* Was the leading theory - the SDK supersamples 2x
  by default and warns the path is experimental. Rendering at native
  resolution changes nothing; skin is still black.
- *Invalid texture fetch constants.* The Vulkan texture cache binds a pure
  black fallback (`kInvalidTextureFetchFallbackColor`) for these, and
  `gpu_allow_invalid_fetch_constants` defaults to true, so it happens
  silently - a very good fit for the symptom. But with the cvar off exactly
  one warning fires, on the title screen, and none on character select.
- *Shader translation failures.* None are logged.
- *Exotic shader instructions.* The character shaders use only `tfetch2D`
  and a little `tfetchCube`; nothing unusual to mistranslate.
- *DXN / CTX1*, the 360 normal-map formats: both have load shaders in the
  Vulkan texture cache.
- *The unmounted `cache:` device.* Deliberate - `runtime.cpp` explicitly
  declines to register it, because games handle "device not found" cleanly
  but not device errors.

Also ruled out, each measured on the repro screen (see below):

| Change | Face brightness |
| --- | --- |
| baseline | 3.6 |
| `native_2x_msaa=false` | 3.6 |
| `vulkan_dynamic_rendering=false` | 3.7 |
| `readback_resolve=full`, `vulkan_readback_resolve=true` | 0.0 (worse) |
| native draw resolution | 3.6 |

Correct skin would read as a mid-tone; the shirt reference reads ~23 on the
same frames, so the measurement is sound.

What the per-frame GPU summary says (run with
`--vulkan_debug_log_frame_summaries_remaining=100000`): the character-select
frame issues 1765 draws, 1573 of them textured, with `placeholder=0` and
`no_effect=0`. So every pipeline is compiled and the skin draws really are
executing and sampling textures - they simply shade to black. That rules out
a missing or still-compiling pipeline, and points at either the skin
material's shader math or the contents of one of its textures.

Where to look next: the character materials are the seven heavy fragment
shaders in the dump (18-21 texture fetches, ~200 instructions, constants up
to c255) - consistent with this game's subsurface-scattering skin shading.
`shader_D47C83252CF2B765` is a representative one. The open question is which
input to its final colour arrives as zero.

The SDK has a trace viewer (`src/graphics/trace_viewer.cpp`) that can step
through a captured frame and show each draw's bound textures and constants,
which would answer this directly - but it is only compiled into the library,
with no executable target. Building one is probably the shortest path.

`./trace.sh` dumps the translated shaders on the affected screen. Note that
`--with-stream` renders a black screen, so shader dumping is the default.

Also outstanding:

- The guest `cache:` device is never mounted, so every `cache:\assets\...`
  open fails. The game continues past it.
- `__imp__Refresh` is a stub.
- The log fills with "Recovered stale physical page protection" - the guest
  memory write-protection path used for GPU invalidation. Worth checking
  whether it is costing frame time.

## Reproducing the black skin

`repro.sh` launches the game, drives it to the character-select screen and
screenshots the character, so rendering changes can be A/B tested without a
human at the keyboard:

```sh
./repro.sh /tmp/base.png                          # baseline
./repro.sh /tmp/try.png --some_cvar=value         # with a change
python3 tools/skinmeter.py /tmp/*_char.png        # compare numerically
```

`skinmeter.py` reports the face brightness plus a shirt reference. The shirt
is the sanity check: if it is also near zero the run never reached the screen
and the face number is meaningless. `repro.sh` verifies arrival itself and
exits non-zero on failure, so a mis-navigated run is never mistaken for a
rendering result.

This needs `pyobjc-framework-Quartz`, and an interpreter that can see it -
Homebrew's python3.10 here, not Xcode's python3. Set `TT_PYTHON` to override.
The caller also needs Accessibility permission, since it posts synthetic key
events. Note that AppleScript's `key code` sends a down/up pair back-to-back
which the game's per-frame input polling misses entirely; `tools/sendkey.py`
holds each key instead.

## Next steps

1. Diagnose the black skin material from a `./trace.sh` capture.
2. Keep working through unregistered-indirect-target aborts as they appear.
3. Mount a `cache:` device so the game's cache probing succeeds.
4. Only once it plays properly, consider a native renderer - that is where a
   large frame-rate win would come from, and it is a from-scratch effort per
   game; none of Skate 3's shader work transfers.
