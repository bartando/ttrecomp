#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Install Table Tennis Recompiled on a jailbroken PS5 from your own copy of the game.

    python3 install_ps5.py "Table Tennis.iso"

The source can be the Xbox 360 disc image or an extracted game folder (the one
holding default.xex). The PS5 needs ftpsrv (FTP on port 2121) and
ShadowMountPlus, which adds the title to the home screen. The console is found
on the local network unless --host is given. With ffmpeg installed, frames of
the game's own menu movie become the home-screen background.

An interrupted install resumes where it stopped when run again.
"""
import argparse
import hashlib
import json
import os
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor
from ftplib import FTP, error_perm
from io import BytesIO
from pathlib import Path

TITLE_ID = "PPSA99782"
XBOX_TITLE_ID = 0x545407DF
REMOTE_TITLE = "/data/homebrew/" + TITLE_ID
# Inside the title folder: a title launched from the home screen cannot see /data.
REMOTE_DATA = REMOTE_TITLE + "/ttrecomp"
FTP_PORT = 2121
BLOCK = 1024 * 1024
# Home-screen background and start-up picture: a frame of the main menu movie
# each, at the sizes PS5 titles ship them.
MENU_MOVIE = "movies/mainmenu_movie_0_w.bik"
BACKGROUNDS = (("sce_sys/pic0.png", 22.0, 3840, 2160), ("sce_sys/pic1.png", 4.0, 1920, 1080))


def fail(message):
    sys.exit("error: " + message)


# --- Game files -------------------------------------------------------------

class GameFile:
    def __init__(self, path, size, opener):
        self.path = path
        self.size = size
        self.open = opener


class IsoSlice:
    """One file inside the disc image, read like an open file."""

    def __init__(self, image, offset, size):
        self.image, self.offset, self.remaining = image, offset, size

    def read(self, size=-1):
        if size < 0 or size > self.remaining:
            size = self.remaining
        data = self.image.read_at(self.offset, size)
        self.offset += size
        self.remaining -= size
        return data

    def close(self):
        pass

    def __enter__(self):
        return self

    def __exit__(self, *_):
        pass


class XboxIso:
    """Files of an Xbox 360 disc image (XDVDFS game partition)."""

    SECTOR = 2048
    # Plain XDVDFS dumps, then XGD3, XGD2 and XGD1 discs.
    GAME_OFFSETS = (0x0, 0xFB20, 0x20600, 0x2080000, 0xFD90000, 0x18300000)

    def __init__(self, path):
        self.file = open(path, "rb")
        self.size = os.fstat(self.file.fileno()).st_size
        for offset in self.GAME_OFFSETS:
            if self.read_at(offset + 32 * self.SECTOR, 20, check=False) == b"MICROSOFT*XBOX*MEDIA":
                break
        else:
            fail(f"{path} is not an Xbox 360 game image")
        root_sector, root_size = struct.unpack(
            "<II", self.read_at(offset + 32 * self.SECTOR + 20, 8))
        if not 13 <= root_size <= 32 * 1024 * 1024:
            fail("the image's root directory is invalid")
        self.files = self._walk(offset, offset + root_sector * self.SECTOR)

    def read_at(self, offset, size, check=True):
        if offset + size > self.size:
            if check:
                fail("the image is truncated")
            return b""
        self.file.seek(offset)
        return self.file.read(size)

    def _walk(self, game_offset, root_offset):
        # Directories are binary trees of entries addressed in 4-byte units.
        files, pending = [], [(root_offset, 0, "")]
        visited = 0
        while pending:
            directory, node, prefix = pending.pop()
            visited += 1
            if visited > 500000:
                fail("the image's directory tree is unexpectedly large")
            entry = directory + node
            left, right, sector, length, attributes, name_length = struct.unpack(
                "<HHIIBB", self.read_at(entry, 14))
            # An empty directory's only "entry" is 0xFF padding.
            if left == 0xFFFF and right == 0xFFFF:
                continue
            name = self.read_at(entry + 14, name_length).decode("latin-1")
            if not name or name in (".", "..") or "/" in name or "\\" in name:
                fail("the image contains an unsafe file name")
            if left:
                pending.append((directory, left * 4, prefix))
            if right:
                pending.append((directory, right * 4, prefix))
            path = prefix + name
            start = game_offset + sector * self.SECTOR
            if attributes & 0x10:
                if length:
                    pending.append((start, 0, path + "/"))
            elif not path.lower().startswith("$systemupdate/"):
                files.append(GameFile(path, length,
                                      lambda o=start, n=length: IsoSlice(self, o, n)))
        return sorted(files, key=lambda f: f.path.lower())


def folder_files(root):
    files = []
    for path in sorted(root.rglob("*")):
        relative = path.relative_to(root).as_posix()
        if path.is_symlink():
            fail(f"refusing a symlink in the game folder: {relative}")
        # Console system update files are never used by the game.
        if (not path.is_file() or path.name == ".DS_Store"
                or relative.lower().startswith("$systemupdate/")):
            continue
        files.append(GameFile(relative, path.stat().st_size, lambda p=path: open(p, "rb")))
    return files


def find_file(files, path):
    return next((f for f in files if f.path.lower() == path.lower()), None)


def read_all(game_file):
    with game_file.open() as stream:
        return stream.read()


def xex_title_id(xex):
    """Title ID from the XEX2 execution info optional header."""
    if xex[:4] != b"XEX2" or len(xex) < 0x18:
        return None
    count = struct.unpack_from(">I", xex, 0x14)[0]
    for index in range(count):
        key, value = struct.unpack_from(">II", xex, 0x18 + 8 * index)
        if key == 0x00040006 and value + 16 <= len(xex):
            return struct.unpack_from(">I", xex, value + 12)[0]
    return None


def load_game(source):
    files = XboxIso(source).files if source.is_file() else folder_files(source)
    xex = find_file(files, "default.xex")
    if not xex:
        fail("no default.xex: pass the disc image or the folder that holds default.xex")
    data = read_all(xex)
    if xex_title_id(data) != XBOX_TITLE_ID:
        fail("this is not Rockstar Presents Table Tennis (title ID 545407DF)")
    return files, hashlib.sha256(data).hexdigest()


# --- Artwork ----------------------------------------------------------------

def make_backgrounds(files):
    """Home-screen pictures from the game's menu movie, if ffmpeg can decode it."""
    ffmpeg = shutil.which("ffmpeg")
    movie = find_file(files, MENU_MOVIE)
    if not ffmpeg or not movie:
        print("No ffmpeg found: installing without a home-screen background "
              "(install ffmpeg and run again to add one).")
        return {}
    pictures = {}
    with tempfile.TemporaryDirectory() as work:
        bink = Path(work) / "menu.bik"
        with movie.open() as source, bink.open("wb") as target:
            shutil.copyfileobj(source, target, BLOCK)
        for name, seconds, width, height in BACKGROUNDS:
            output = Path(work) / Path(name).name
            subprocess.run([ffmpeg, "-v", "error", "-y", "-ss", str(seconds), "-i", str(bink),
                            "-frames:v", "1", "-vf", f"scale={width}:{height}:flags=lanczos",
                            "-pix_fmt", "rgb24", str(output)], check=False)
            if output.is_file() and output.stat().st_size:
                pictures[name] = output.read_bytes()
    if len(pictures) != len(BACKGROUNDS):
        print("ffmpeg could not decode the menu movie: installing without a background.")
        return {}
    print("Made the home-screen background from the game's menu movie.")
    return pictures


# --- Console ----------------------------------------------------------------

def is_ps5(host):
    try:
        ftp = FTP()
        ftp.connect(host, FTP_PORT, timeout=3)
        try:
            ftp.login()
            ftp.cwd("/system_ex")  # Only PlayStation consoles have it.
            ftp.cwd("/mnt/sandbox")
            return True
        finally:
            ftp.close()
    except Exception:
        return False


def find_console():
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
            probe.connect(("10.255.255.255", 1))  # No packet is sent.
            local = probe.getsockname()[0]
    except OSError:
        fail("could not tell which network this computer is on; pass --host PS5_IP")
    prefix = local.rsplit(".", 1)[0]
    print(f"Looking for a PS5 with FTP on {prefix}.0/24 ...")

    def ftp_open(host):
        try:
            with socket.create_connection((host, FTP_PORT), timeout=0.5):
                return host
        except OSError:
            return None

    with ThreadPoolExecutor(64) as pool:
        hosts = [h for h in pool.map(ftp_open, (f"{prefix}.{i}" for i in range(1, 255))) if h]
    consoles = [host for host in hosts if is_ps5(host)]
    if not consoles:
        fail("no PS5 found: start ftpsrv on the console (port 2121) or pass --host PS5_IP")
    if len(consoles) > 1:
        fail("several PS5s found (" + ", ".join(consoles) + "); pick one with --host")
    return consoles[0]


class Console:
    def __init__(self, host):
        self.ftp = FTP()
        self.ftp.connect(host, FTP_PORT, timeout=60)
        self.ftp.login()
        self.ftp.voidcmd("TYPE I")

    def close(self):
        try:
            self.ftp.quit()
        except Exception:
            self.ftp.close()

    def read(self, path):
        out = BytesIO()
        self.ftp.retrbinary("RETR " + path, out.write, blocksize=BLOCK)
        return out.getvalue()

    def read_optional(self, path):
        try:
            return self.read(path)
        except error_perm as error:
            if not str(error).startswith("550"):
                raise
            return None

    def write(self, path, stream, callback=None):
        if isinstance(stream, bytes):
            stream = BytesIO(stream)
        self.ftp.storbinary("STOR " + path, stream, blocksize=BLOCK, callback=callback)

    def size(self, path):
        try:
            size = self.ftp.size(path)
        except error_perm as error:
            if not str(error).startswith("550"):
                raise
            return None
        # ftpsrv v0.21.1 reports a missing file's size as unsigned -1.
        return None if size == 0xFFFFFFFFFFFFFFFF else size

    def exists(self, directory):
        try:
            self.ftp.cwd(directory)
            return True
        except error_perm:
            return False

    def make_dirs(self, path):
        current = ""
        for part in path.strip("/").split("/"):
            current += "/" + part
            if not self.exists(current):
                self.ftp.mkd(current)

    def title_running(self):
        lines = []
        self.ftp.retrlines("LIST /mnt/sandbox", lines.append)
        return any(TITLE_ID in line for line in lines)


class Progress:
    """Bytes done, counting files already on the console; the rate is of bytes sent."""

    def __init__(self, total):
        self.total, self.done, self.sent = total, 0, 0
        self.started = self.shown = time.monotonic()

    def add(self, count, sent=True):
        self.done += count
        self.sent += count if sent else 0
        now = time.monotonic()
        if now - self.shown >= 1 or self.done >= self.total:
            self.shown = now
            rate = self.sent / max(now - self.started, 1e-6) / 1e6
            sys.stdout.write(f"\r  {self.done / 2**30:5.2f} / {self.total / 2**30:.2f} GiB"
                             f"  sending {rate:5.1f} MB/s ")
            sys.stdout.flush()


def sha256_of(game_file):
    digest = hashlib.sha256()
    with game_file.open() as stream:
        while block := stream.read(BLOCK):
            digest.update(block)
    return digest.hexdigest()


def install_game_data(console, files, xex_sha256):
    identity = {"project": "ttrecomp", "xex_sha256": xex_sha256}
    if console.exists(REMOTE_DATA):
        owner = console.read_optional(REMOTE_DATA + "/owner.json")
        if owner is None or json.loads(owner) != identity:
            fail(f"{REMOTE_DATA} holds files from another copy or project; "
                 "delete it on the console to reinstall")
    else:
        console.make_dirs(REMOTE_DATA)
        console.write(REMOTE_DATA + "/owner.json", (json.dumps(identity) + "\n").encode())
    manifest_path = REMOTE_DATA + "/game-manifest.json"
    manifest = json.loads(console.read_optional(manifest_path) or b"{}")

    total = sum(f.size for f in files)
    print(f"Copying {len(files)} game files ({total / 2**30:.2f} GiB) to the PS5:")
    progress = Progress(total)
    directories = set()
    for game_file in files:
        target = REMOTE_DATA + "/game/" + game_file.path
        entry = {"size": game_file.size, "sha256": sha256_of(game_file)}
        size = console.size(target)
        if manifest.get(game_file.path) == entry and size == game_file.size:
            progress.add(game_file.size, sent=False)
            continue
        if size is not None and game_file.path not in manifest:
            fail(f"an unrecorded file is in the way on the console: {target}")
        parent = target.rsplit("/", 1)[0]
        if parent not in directories:
            console.make_dirs(parent)
            directories.add(parent)
        temporary = target + ".tt-upload"
        with game_file.open() as stream:
            console.write(temporary, stream, callback=lambda block: progress.add(len(block)))
        if console.size(temporary) != game_file.size:
            fail(f"upload size mismatch: {game_file.path}; run again to retry")
        # Recorded before the rename, so an interrupted rename resumes safely.
        manifest[game_file.path] = entry
        console.write(manifest_path, (json.dumps(manifest) + "\n").encode())
        console.ftp.rename(temporary, target)
    print()
    readback = hashlib.sha256(console.read(REMOTE_DATA + "/game/default.xex")).hexdigest()
    if readback != xex_sha256:
        fail("default.xex read back from the console differs; run again")


# --- Title ------------------------------------------------------------------

def elf_digest(data):
    """sha256 of an ELF with PT_SCE_VERSION zeroed, which ftpsrv clears when it
    unwraps the signed executable on readback."""
    if data[:4] != b"\x7fELF":
        return None
    phoff, = struct.unpack_from("<Q", data, 0x20)
    phentsize, phnum = struct.unpack_from("<HH", data, 0x36)
    normalized = bytearray(data)
    for index in range(phnum):
        kind, _, offset, _, _, size = struct.unpack_from("<IIQQQQ", data, phoff + index * phentsize)
        if kind == 0x6FFFFF01:
            normalized[offset:offset + size] = bytes(size)
    return hashlib.sha256(normalized).hexdigest()


def install_title(console, title, pictures):
    parameters = json.loads((title / "sce_sys/param.json").read_bytes())
    if parameters.get("titleId") != TITLE_ID:
        fail(f"{title} is not the Table Tennis Recompiled title")
    # Expected executable readbacks, written by the release packaging.
    release_file = title.parent / "release.json"
    executables = json.loads(release_file.read_bytes()) if release_file.is_file() else {}
    previous = console.read_optional(REMOTE_TITLE + "/sce_sys/param.json")
    if previous and json.loads(previous).get("contentId") != parameters["contentId"]:
        fail(f"{TITLE_ID} on the console belongs to another project; not overwriting it")

    files = {name: (title / name).read_bytes()
             for name in ("eboot.bin", "sce_module/libc.prx", "sce_sys/icon0.png")}
    files.update(pictures)
    # Metadata last, so a half-copied title never registers.
    files["sce_sys/param.json"] = (title / "sce_sys/param.json").read_bytes()
    for directory in ("sce_module", "sce_sys"):
        console.make_dirs(REMOTE_TITLE + "/" + directory)
    for name, data in files.items():
        target = REMOTE_TITLE + "/" + name
        console.write(target + ".tt-upload", data)
        readback = console.read(target + ".tt-upload")
        if name in ("eboot.bin", "sce_module/libc.prx"):
            # FTP hands back the unwrapped executable.
            expected = executables.get(name)
            good = readback == data or (elf_digest(readback) is not None
                                        and expected in (None, elf_digest(readback)))
        else:
            good = readback == data
        if not good:
            fail(f"{name} read back from the console differs; run again")
        console.ftp.rename(target + ".tt-upload", target)
    print("Installed the title files.")

    # The home screen reads copies made at registration, never the title's own.
    meta = "/user/appmeta/" + TITLE_ID
    if console.exists(meta):
        artwork = {"icon0.png": files["sce_sys/icon0.png"]}
        artwork.update({Path(name).name: data for name, data in pictures.items()})
        for name, data in artwork.items():
            for directory in (meta, "/user/app/" + TITLE_ID + "/sce_sys"):
                if console.exists(directory):
                    console.write(directory + "/" + name, data)
        console.write("/user/app/" + TITLE_ID + "/icon0.png", artwork["icon0.png"])
        print("Refreshed the home-screen artwork of the registered title.")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("game", type=Path, help="Table Tennis disc image (.iso) or extracted folder")
    parser.add_argument("--host", help="PS5 IP address (default: search the local network)")
    parser.add_argument("--title", type=Path, default=Path(__file__).resolve().parent / TITLE_ID,
                        help=f"the {TITLE_ID} title folder from the release (default: next to "
                             "this script)")
    args = parser.parse_args()
    if not args.game.exists():
        fail(f"{args.game} does not exist")
    if not (args.title / "eboot.bin").is_file():
        fail(f"no title at {args.title}; keep the {TITLE_ID} folder next to this script")

    files, xex_sha256 = load_game(args.game.resolve())
    print(f"Found Table Tennis: {len(files)} files.")
    pictures = make_backgrounds(files)
    host = args.host or find_console()
    console = Console(host)
    try:
        print(f"Connected to the PS5 at {host}.")
        if console.title_running():
            fail("Table Tennis Recompiled is running on the PS5; close it first")
        install_game_data(console, files, xex_sha256)
        install_title(console, args.title.resolve(), pictures)
    finally:
        console.close()
    print("Done. ShadowMountPlus adds Table Tennis Recompiled to the home screen; "
          "if it does not show up, check that ShadowMountPlus is running.")


if __name__ == "__main__":
    main()
