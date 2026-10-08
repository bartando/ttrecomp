#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Lay out the PS5 release: the built title, its installer and readback hashes.

    python3 ps5/package_release.py

Writes out/ps5-release/TableTennisRecompiled-PS5/ and a zip of it. No game
files go in; the installer takes them from the user's own disc image.
"""
import json
import shutil
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from install_ps5 import TITLE_ID, elf_digest  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
PORT = ROOT / "out/ps5-port"
NAME = "TableTennisRecompiled-PS5"

INSTRUCTIONS = f"""Table Tennis Recompiled for PS5

You need a jailbroken PS5 running ftpsrv (FTP on port 2121) and
ShadowMountPlus, Python 3.8 or newer on your computer, and your own copy of
Rockstar Presents Table Tennis for Xbox 360 (disc image or extracted folder).
No game files are included.

    python3 install_ps5.py "path/to/Table Tennis.iso"

The installer finds the PS5 on your network (or pass --host PS5_IP), copies the
game files and the {TITLE_ID} title, and resumes if interrupted. With ffmpeg
installed it also makes the home-screen background from the game's menu movie.
"""


def local_identity_leaks(folder):
    """Files naming this machine's user, e.g. absolute __FILE__ paths."""
    needles = {str(Path.home()).encode(), Path.home().name.lower().encode()}
    return [path for path in sorted(folder.rglob("*")) if path.is_file()
            and any(needle in path.read_bytes().lower() for needle in needles)]


def main():
    title = PORT / "dist" / TITLE_ID
    executables = {"eboot.bin": PORT / "game-title/eboot-extracted.elf",
                   "sce_module/libc.prx": PORT / "game-title/libc-extracted.elf"}
    for path in [title / "eboot.bin", *executables.values()]:
        if not path.is_file():
            sys.exit(f"missing {path}: build the title first (ps5/README.md)")
    out = ROOT / "out/ps5-release" / NAME
    if out.exists():
        shutil.rmtree(out)
    out.with_suffix(".zip").unlink(missing_ok=True)
    shutil.copytree(title, out / TITLE_ID)
    shutil.copy2(ROOT / "ps5/install_ps5.py", out / "install_ps5.py")
    digests = {name: elf_digest(path.read_bytes()) for name, path in executables.items()}
    (out / "release.json").write_text(json.dumps(digests, indent=2) + "\n")
    (out / "README.txt").write_text(INSTRUCTIONS)
    leaks = local_identity_leaks(out)
    if leaks:
        shutil.rmtree(out)
        sys.exit("refusing to package, local user paths in: " +
                 ", ".join(str(path.relative_to(out)) for path in leaks))
    archive = shutil.make_archive(str(out), "zip", out.parent, NAME)
    print(f"{out}\n{archive}")


if __name__ == "__main__":
    main()
