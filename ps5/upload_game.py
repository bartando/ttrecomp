#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Copy the user's extracted Table Tennis files into its private PS5 data folder."""
import argparse
import hashlib
import json
from ftplib import FTP, error_perm
from io import BytesIO
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("--host", required=True)
    parser.add_argument("--port", type=int, default=2121)
    args = parser.parse_args()
    source = args.source.resolve(strict=True)
    xex = source / "default.xex"
    if not xex.is_file():
        parser.error("source must be an extracted game directory containing default.xex")
    identity = {"project": "ttrecomp", "xex_sha256": hashlib.sha256(xex.read_bytes()).hexdigest()}
    # Inside the title directory: the launched title is sandboxed without /data.
    remote = "/data/homebrew/PPSA99782/ttrecomp"
    with FTP() as ftp:
        ftp.connect(args.host, args.port, timeout=30)
        ftp.login()
        ftp.voidcmd("TYPE I")
        try:
            ftp.cwd(remote)
        except error_perm as error:
            if not str(error).startswith("550"):
                raise
            ftp.mkd(remote)
            ftp.storbinary("STOR " + remote + "/owner.json",
                           BytesIO((json.dumps(identity) + "\n").encode()))
        else:
            owner = BytesIO()
            ftp.retrbinary("RETR " + remote + "/owner.json", owner.write)
            if json.loads(owner.getvalue()) != identity:
                raise RuntimeError("Destination is not owned by this project/game; refusing to overwrite")
        files = []
        for file in sorted(source.rglob("*")):
            if file.is_symlink():
                raise RuntimeError(f"Refusing symlink in game files: {file.relative_to(source)}")
            if file.is_file() and file.name != ".DS_Store":
                files.append(file)
        total = sum(file.stat().st_size for file in files)
        print(f"Uploading {len(files)} files ({total / 1024**2:.1f} MiB) to {remote}/game", flush=True)
        directories = {remote}
        manifest = {}
        try:
            previous = BytesIO()
            ftp.retrbinary("RETR " + remote + "/game-manifest.json", previous.write)
            manifest = json.loads(previous.getvalue())
        except error_perm as error:
            if not str(error).startswith("550"):
                raise
        completed = 0
        for number, file in enumerate(files, 1):
            relative = file.relative_to(source).as_posix()
            target = remote + "/game/" + relative
            parent = target.rsplit("/", 1)[0]
            current = remote
            for part in parent[len(remote) + 1:].split("/"):
                current += "/" + part
                if current not in directories:
                    try:
                        ftp.mkd(current)
                    except error_perm:
                        # Verify the directory exists rather than swallowing an upload error.
                        ftp.cwd(current)
                    directories.add(current)
            with file.open("rb") as stream:
                digest = hashlib.file_digest(stream, "sha256").hexdigest()
            entry = {"size": file.stat().st_size, "sha256": digest}
            try:
                size = ftp.size(target)
                # ftpsrv v0.21.1 encodes a missing-file stat error as unsigned -1.
                if size == 0xFFFFFFFFFFFFFFFF:
                    size = None
            except error_perm as error:
                if not str(error).startswith("550"):
                    raise
                size = None
            if manifest.get(relative) != entry or size != entry["size"]:
                if size is not None and relative not in manifest:
                    raise RuntimeError(f"Unrecorded destination file exists; refusing overwrite: {relative}")
                temporary = target + ".tt-upload"
                with file.open("rb") as stream:
                    ftp.storbinary("STOR " + temporary, stream, blocksize=256 * 1024)
                if ftp.size(temporary) != entry["size"]:
                    raise RuntimeError(f"Upload size mismatch: {relative}")
                # Record before rename: an interrupted rename can be resumed safely.
                manifest[relative] = entry
                ftp.storbinary("STOR " + remote + "/game-manifest.json",
                               BytesIO((json.dumps(manifest) + "\n").encode()))
                ftp.rename(temporary, target)
            completed += entry["size"]
            if number % 20 == 0 or number == len(files):
                print(f"{number}/{len(files)} files; {completed / 1024**2:.1f}/{total / 1024**2:.1f} MiB", flush=True)
        readback = hashlib.sha256()
        ftp.retrbinary("RETR " + remote + "/game/default.xex", readback.update)
        if readback.hexdigest() != identity["xex_sha256"]:
            raise RuntimeError("default.xex readback differs from the source")
        print("Game data uploaded; file sizes checked and default.xex readback verified", flush=True)


if __name__ == "__main__":
    main()
