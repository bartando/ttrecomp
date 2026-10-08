# Table Tennis PS5 title

The native game host is in `game/`. It uses the existing generated guest code
and game hooks with our adapted ReXGlue v0.8 runtime, the statically linked
PS5 RADV driver, DualSense input and native stereo audio. Desktop sources
and the macOS SDK submodule are not patched by this experimental build.

The installed title is **Table Tennis Recompiled**, **PPSA99782**. Its game
files are the user's own extracted copy at
`/data/homebrew/PPSA99782/ttrecomp/game`, which the title sees as
`/app0/ttrecomp/game`; user data and caches go next to it. A title launched
from the home screen runs sandboxed without `/data`, so `/data/ttrecomp` is
only a fallback. The game files are not bundled into the title package or
repository.

Startup logs are `/app0/tt-game.log` and `/app0/tt-game-crash.log`. Through
FTP those appear in `/data/homebrew/PPSA99782/`. A host failure leaves the
title waiting for system close; close it through the PS menu.

## Local build

The ignored `out/ps5-port` workspace contains the prepared public toolchain,
RADV archive, an isolated SDK clone, staged guest/game sources and build
outputs. Public dependency revisions and the Linux builder are documented in
`docs/ps5_runtime_reference.md`. SDK adaptations are preserved in
`sdk-patches/`; do not apply them to the desktop SDK while this port is
experimental.

`game-source` contains copies of `src`, `generated` and `ps5/game`. Its
`third_party/rexglue-sdk` symlink points to the isolated SDK. Only the staged
`generated/default/tabletennis_init.h` enables `REX_PLATFORM_PS5` in the
`REX_PHYS_HOST_OFFSET` condition, matching the patched codegen template and
the console's 16 KiB pages.

The PS5 SDK CMake build includes the game with
`-DREXGLUE_PS5_GAME_SOURCE=/work/game-source`. Build the
`tabletennis-ps5` static target, then link and package in the prepared builder:

```sh
cp ps5/build_title.sh out/ps5-port/build-game-title.sh
docker exec ttrecomp-ps5-build bash /work/build-game-title.sh /work
```

For the current accelerated local build, the runtime and game archives are
cross-compiled with the installed native LLVM 21 compiler into `mac-libs`;
the final link and native conversion still use the proven Arch LLVM 23 tools:

```sh
docker exec ttrecomp-ps5-build bash /work/build-game-title.sh /work /work/mac-libs
```

The script checks executable sections and 16 KiB alignment before and after
native conversion. In particular, large-model `.ltext` belongs in the
executable segment. It then signs the title and extracts the signed ELF for
upload verification; signing updates an ELF note, so the pre-sign ELF is
not the correct readback reference.

```sh
python3 ps5/upload_title.py out/ps5-port/dist/PPSA99782 --host PS5_IP \
  --eboot-elf out/ps5-port/game-title/eboot-extracted.elf \
  --libc-elf out/ps5-port/game-title/libc-extracted.elf
```

Close an existing game instance and wait for its sandbox to be released
before replacing the title. The uploader checks all code/data bytes after
FTP's SELF unwrapping, allowing only the service's known clearing of
`PT_SCE_VERSION`. Metadata is installed last. For development,
`upload_game.py` uploads/resumes an extracted game folder and checks the XEX on
readback.

## Settings menu

The touchpad click opens the title's own menu (`game/settings_menu.h`), drawn
with ImGui over the game. It sets the upscaler (FSR 1 or bilinear, live), the
render resolution (720p or 1440p, next launch), an FPS counter and controller
rumble. Choices go to `/app0/ttrecomp/settings.toml`, loaded after the host's
defaults and before `ps5.toml`. While the menu is open the game sees an idle
pad. The touchpad no longer sends Xbox Back; nothing in the game was found to
use it.

For unattended checks, `ps5_pad_test_script` presses buttons on a timeline,
and `ps5_capture_final` makes the capture cvars grab the presented frame,
overlays included. `run_match.py` turns rumble off unless `--rumble` is given.

## Installing for players

`install_ps5.py` is the player-facing installer: it takes the user's Xbox 360
disc image (or extracted folder), streams the game files straight from it to
the console with resume, installs the title and, with ffmpeg, makes the
home-screen background (`pic0.png` 3840x2160, `pic1.png` 1920x1080) from the
game's menu movie. It finds the PS5 by scanning the local /24 for ftpsrv.
`package_release.py` lays out the release folder and zip with the title, the
installer and `release.json`, the expected readback hashes of the executables.

```sh
python3 ps5/package_release.py
python3 out/ps5-release/TableTennisRecompiled-PS5/install_ps5.py "Table Tennis.iso"
```

## Current verification

CPU faults/aliases, compute shaders and the blue display frame passed on the
console. The complete game and runtime build, native conversion and title
upload also passed. The first game launches showed a black screen: the
host stopped at its first check because `/data` does not exist inside the
title sandbox. With the game files under the title directory, the next run
reached guest execution, Vulkan draws and audio output, then died after about
three seconds with SIGFPE in the guest audio callback (see
`sdk-patches/0004-host-thread-fpscr.patch`). With that fixed, the intro played
at 60 fps with audio, then the title called a null guest function about 20
seconds in. The voice-chat singleton's init (VDP socket on port 1001, voice
engine) failed on PS5; the game's own teardown leaves the "voice enabled" bit
set, and the next update dereferences the freed engine.
`src/native/tabletennis_voice_session_guard.cpp` clears that bit after
teardown, and `sdk-patches/0005-socket-bind.patch` lets the bind succeed.
That build also links the game library with `--whole-archive`: before, every
`extern "C" REX_FUNC(sub_...)` override in `src/native` was silently dropped
from the PS5 link, while desktop has always had them. Next run pending.
ShadowMountPlus's "crashed before KStuff pause" notice
means the title crashed on its own; it does not point at Kstuff.
Game rendering, controls, audio and frame rate are not yet verified.

The host/platform adaptations reuse GPL-3.0-or-later work from
[holdmysocks/mcla-recomp](https://github.com/holdmysocks/mcla-recomp), revision
`b5765a912a8efc65a3fc1753237831c9c229a4ab`, and native tooling from
[mihawk-99/PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan). Original
third-party notices remain intact.
