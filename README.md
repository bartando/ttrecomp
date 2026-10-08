# Table Tennis Recomp

An in-progress native recompilation of the Xbox 360 version of
*Rockstar Games Presents Table Tennis*, built on the
[ReXGlue SDK](https://github.com/rexglue/rexglue-sdk).

**Status: it runs into a match.** Menus, character selection, loading, and the
arena render through Vulkan (MoltenVK) on Apple Silicon, with keyboard and
controller input available. Full-match compatibility is still untested.

This project contains no retail game code or assets. To build or run it you
must supply files from your own legally obtained copy of the game.

Fan-made and unofficial. Not affiliated with or endorsed by Rockstar Games or
Take-Two Interactive.

## How do I play?

The release downloads contain only the recompiled program. On first launch it asks
for your own Xbox 360 ISO of the game and extracts the files it needs next to
the app.

### Windows

1. Extract `TableTennisRecomp-Windows.zip` into a folder you control.
2. Run `tabletennis.exe`.
3. Click "Select ISO", pick your ISO, and wait for the install to finish.
4. Click "Start Game".

### macOS (Apple Silicon)

1. Open `TableTennisRecomp-macOS.dmg` and drag the app into a folder you control,
   such as `~/Games/Table Tennis Recomp`, then eject the disk image. If using
   the ZIP instead, extract it and move the app into that folder. Game files,
   settings and logs live beside the app; saves and shader caches use your
   user-data directory. Avoid Downloads and Applications.
2. Launch the copied app. If macOS still runs it from its
   quarantine location, the installer says so.
3. The first time, right-click the app and choose Open, or allow it under
   System Settings > Privacy & Security. The app is ad-hoc signed, not
   notarized.
4. Click "Select ISO", pick your ISO, and wait for the install to finish.
5. Click "Start Game".

Settings go in `tabletennis.toml` next to the app (see the one in this repo
for the common ones). Without it, the built-in defaults apply.

With `store_shaders = true` (the default), known Vulkan pipelines are compiled
before gameplay starts. This adds a short startup delay and avoids compiling
those pipelines again during play. A pipeline encountered for the first time
can still cause a stutter; it is saved for the next launch.

On macOS (Vulkan), normal play sessions automatically record stutter diagnostics in
`logs/tabletennis_NNN.log` next to the app. `PERF hitch` entries report guest
swap intervals over 25 ms, pipeline compilation, GPU fence waits, guest file
reads (including lock waits and memory invalidation), and stale protection
recoveries. `PERF summary` reports average, p95, p99 and worst frame times
every 300 intervals, including hitches whose individual entries were suppressed
to keep logging bounded. macOS also reports game CPU usage, system load averages
and peak resident memory. System load is context, not proof of a background
process causing a stall. Worker timings can overlap and are attributed when
the operation finishes; they do not prove causation either.

Press **F8** (or **Fn+F8** if macOS uses media keys) just after a noticeable
stutter to add a timestamped `PERF user marker`. Logs flush every second and
retain the five most recent launches. To turn this telemetry off, set
`frame_hitch_diagnostics = false` and `tabletennis_guest_fps_log_interval = 0`
in `tabletennis.toml`.

## Packaging a release

Build the release preset, then:

- macOS: `tools/package_macos.sh out/build/macos-arm64-release` writes
  `out/package/TableTennisRecomp.app`, `TableTennisRecomp-macOS.zip`, and
  `TableTennisRecomp-macOS.dmg`. The minimum macOS version follows
  the Vulkan loader you build against. Homebrew's needs macOS 15, and the
  LunarG SDK's (`VULKAN_SDK`) goes lower.
- Windows: `tools/package_windows.ps1 -Build out/build/win-amd64-release`
  writes `out/package/TableTennisRecomp-Windows.zip`.

Both scripts refuse to package a binary that still contains the builder's home
directory path.

To wrap an existing signed app without rebuilding it, run
`tools/package_macos_dmg.sh /path/to/TableTennisRecomp.app /path/to/output`.
The DMG contains the app and copy-before-launch instructions; it does not
include an Applications shortcut because game files currently live beside
the app.

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
trace-viewer.sh  inspect or dump a captured GPU frame
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

The app can also install the game files itself: launched without them, it
shows a setup screen that asks for your ISO, checks it's Table Tennis (title
ID `545407DF`) and extracts it to `game/` next to the executable. Setting
`TABLETENNIS_INSTALL_ISO=/path/to.iso` does the same without the UI. The file
picker is macOS-only for now.

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
first needs it. The app's baked defaults turn it back on, allowing temporary
placeholder draws while worker threads compile pipelines. Submission still
waits for compilation, so async compilation alone does not remove first-use
stutters. `store_shaders`
keeps guest shaders and pipeline descriptions, which are used to precompile
previously seen pipelines at startup. Both cache streams explicitly seek to
the beginning before loading: macOS starts `a+b` streams at EOF, which previously
made valid caches look empty and caused them to be rewritten on each launch.
The storage writer also flushes pending records before sleeping, so the latest
batch does not remain buffered while the game is otherwise idle.
Precompiling moves that work into startup; a pipeline encountered for the first
time can still require compilation during play.

**Rendering at 4x the pixels.** `resolution_scale` and
`draw_resolution_scale_x/y` all default to 2 in the SDK, supersampling the
1280x720 guest framebuffer to 2560x1440. The SDK itself warns that the path
"is experimental and may not affect all titles correctly". The app's baked
defaults set them to 1: about 47-52 fps in gameplay versus 28 at 2x on the
same machine.

Beyond that, the frame rate is limited by this being the emulated Xenos
renderer. Skate 3's ~10x uplift on Apple Silicon came from replacing that with
a native renderer, which is a from-scratch effort per game.

**Black character skin is fixed.** The Vulkan SPIR-V translator read the
texture result exponent adjustment from fetch-constant dword 4. Xenos stores
that field in dword 3, which the SDK's D3D12 path already used correctly. For
the skin material, the unrelated bits in dword 4 decoded as `-8`, multiplying
otherwise-correct skin texels by `2^-8` and making them effectively black.
The Vulkan path now reads dword 3 too.

The GPU tracer made this deterministic: shader `D47C83252CF2B765` returned a
correct face texture sample, then lost almost all brightness while applying
the result exponent. This ruled out texture upload, UVs, lighting, normal
maps, descriptor binding, and LOD before changing production code.

Also outstanding:

- The guest `cache:` device is never mounted, so every `cache:\assets\...`
  open fails. The game continues past it.
- `__imp__Refresh` is a stub.
- The log fills with "Recovered stale physical page protection" - the guest
  memory write-protection path used for GPU invalidation. Worth checking
  whether it is costing frame time.

## Rendering regression test

`repro.sh` launches the game, drives it to the character-select screen and
screenshots the character, so the fixed rendering can be regression-tested
without a human at the keyboard:

```sh
./repro.sh /tmp/base.png                          # baseline
./repro.sh /tmp/try.png --some_cvar=value         # with a change
./repro.sh /tmp/match.png --enter-gameplay        # continue through loading
./repro.sh /tmp/test.png --test-path               # game-side fast path, verified match
./run.sh --skip-menu                               # normal interactive launch into a match
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

## GPU frame tracer

Press F7 in the game, or let the repro script do it:

```sh
./repro.sh /tmp/trace.png --capture-trace
```

Captured frames are written to `out/trace/frames`. Open the interactive viewer
or produce a textual/texture/frame dump:

```sh
./trace-viewer.sh out/trace/frames/<frame>.xtr

./trace-viewer.sh out/trace/frames/<frame>.xtr \
  --trace_dump=true \
  --trace_dump_shader=D47C83252CF2B765 \
  --trace_dump_textures=/tmp/tabletennis-textures \
  --trace_dump_frame=/tmp/tabletennis-frame.ppm
```

The viewer also supports targeted Vulkan shader output/register/fetch probes.
Those are diagnostic restart-time cvars; run the executable with `--help` for
the current names and modes.

## Native renderer observer

Native-renderer work follows Skate 3's capture-first method: hook the game's
render submission, decode the guest structures it actually uses, compare the
result with the emulated frame, and only then serve one verified piece at a
time. The native path does not contain a replacement scene.

The geometry/camera proof is working, and the first real textured pass is
implemented:

- `sub_82152E80` captures the completed camera constant context.
- Indexed draw submission captures a real table/net mesh: 948 vertices and
  4,680 indices.
- The mesh and camera are copied into immutable host-side snapshots.
- An optional post-process observer draws that real mesh over the untouched
  emulated frame. The position-only proof aligned with the original net across
  moving gameplay cameras, validating the mesh decode, transforms, and matrix
  convention together.
- `grmShaderFx::DrawModelGeometry` at `0x820EFB30` ties the mesh to its real
  shader object, model, geometry index, LOD, and alternate pass.
- `rage_fx_ApplyPass` at `0x82158C48` captures the two live program pairs and
  their render/sampler command records. The observer has verified every
  pointer and callback with zero guest-read failures.
- The type-6 Fx resource setter at `0x8215A830` caches material-build bindings
  by owner shader and joins them to the later proven draw. The table/net
  material currently exposes three real resource objects through handles
  `0x000C0004`, `0x00080002`, and `0x00100006`.
- The resource vtable `+0x50` unwrappers expose each guest D3D texture. Stable
  double-reads locate Table Tennis' six-dword Xenos fetch block at binding
  `+0x10` and decode the three textures as 256x256 DXT1, 512x512 DXT5, and
  512x256 DXT5, including tiled base/mip addresses.
- The visible 4,680-index net pass uses the two DXT5 resources, UV0/UV1, and
  COLOR0 from the game's 96-byte vertex format. The observer untile-copies
  those payloads, uploads them as BC3, and ports the captured two-texture
  blend and opacity calculation.
- Vulkan shader generation asserts the NRHI descriptor contract before
  writing the embedded SPIR-V: constants at set 0/binding 0, samplers at set
  0/bindings 1-2, and textures at set 1/bindings 0-1. This check exists
  because the initial auto-mapped SPIR-V collided all three resource classes
  in set 0 and caused an Apple GPU page fault.

Run the observer overlay with:

```sh
./run.sh --skip-menu --tabletennis_native_observer_overlay=true
```

The overlay is off by default and is diagnostic only; it does not suppress the
guest renderer or improve the current frame rate.

Ghidra 12.1.2 plus XEXLoaderWV is also part of the workflow. A headless import
of `game/default.xex` lives in the gitignored `out/ghidra-projects` directory.
Static analysis has confirmed the live render chain and is now being used to
recover material and texture bindings rather than guessing guest layouts from
draw data alone. The project database contains named RTTI/vtables and partial
layouts for `grcTextureReferenceBase`, `grcTextureReference`, and
`grcTextureXenon`, including the validated fetch block at binding `+0x10`.

Detailed material telemetry is opt-in:

```sh
./run.sh --skip-menu --tabletennis_native_material_log_interval=30
```

## Next steps

1. Verify a full playable rally and then a complete match with a controller.
2. Keep working through unregistered-indirect-target aborts as they appear.
3. Reboot after the Apple GPU fault, then validate the corrected descriptor
   mapping and first verified camera on the textured net observer.
4. Widen capture from the proven net to the remaining table and arena meshes.
5. Decode player meshes, skinning, transforms, and materials.
6. Publish complete immutable scene snapshots before enabling any native
   takeover. The emulated renderer remains the authority until each field has
   passed an observer comparison.
7. Investigate the repeated stale physical-page-protection recovery warnings
   and mount a `cache:` device.
