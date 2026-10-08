#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Install this project's game title and verify FTP's unwrapped SELF readback."""
import argparse
import json
import struct
from ftplib import FTP, error_perm
from io import BytesIO
from pathlib import Path


def verify_elf(expected, actual):
    if len(expected) != len(actual):
        raise ValueError("ELF readback length differs")
    header = struct.unpack_from("<16sHHIQQQIHHHHHH", expected)
    if header[0][:7] != b"\x7fELF\x02\x01\x01" or header[9] != 56:
        raise ValueError("expected x86-64 ELF readback reference")
    # ftpsrv removes only PT_SCE_VERSION metadata while unwrapping SELF.
    allowed = []
    for index in range(header[10]):
        segment = struct.unpack_from("<IIQQQQQQ", expected, header[5] + index * 56)
        if segment[0] == 0x6FFFFF01:
            allowed.append((segment[2], segment[2] + segment[5]))
    for index, (before, after) in enumerate(zip(expected, actual)):
        if before != after and not (after == 0 and any(a <= index < b for a, b in allowed)):
            raise ValueError(f"ELF code/data readback differs at 0x{index:x}")


def download(ftp, path):
    output = BytesIO()
    ftp.retrbinary("RETR " + path, output.write, blocksize=256 * 1024)
    return output.getvalue()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("title", type=Path)
    parser.add_argument("--host", required=True)
    parser.add_argument("--port", type=int, default=2121)
    parser.add_argument("--eboot-elf", type=Path, required=True)
    parser.add_argument("--libc-elf", type=Path, required=True)
    args = parser.parse_args()
    title = args.title.resolve(strict=True)
    parameter = json.loads((title / "sce_sys/param.json").read_bytes())
    if parameter.get("titleId") != "PPSA99782":
        parser.error("only this project's PPSA99782 game title may be installed")
    files = ["eboot.bin", "sce_module/libc.prx", "sce_sys/icon0.png", "sce_sys/param.json"]
    references = {"eboot.bin": args.eboot_elf.read_bytes(),
                  "sce_module/libc.prx": args.libc_elf.read_bytes()}
    for relative in files:
        if (title / relative).is_symlink() or not (title / relative).is_file():
            parser.error(f"missing or symlinked title file: {relative}")
    remote = "/data/homebrew/PPSA99782"
    with FTP() as ftp:
        ftp.connect(args.host, args.port, timeout=60)
        ftp.login()
        ftp.voidcmd("TYPE I")
        try:
            previous = json.loads(download(ftp, remote + "/sce_sys/param.json"))
        except error_perm as error:
            if not str(error).startswith("550"):
                raise
            previous = None
        if previous and previous.get("contentId") != parameter["contentId"]:
            raise RuntimeError("title ID belongs to another project; refusing overwrite")
        for directory in [remote, remote + "/sce_module", remote + "/sce_sys"]:
            try:
                ftp.mkd(directory)
            except error_perm:
                ftp.cwd(directory)
        # Metadata is installed last, so a new title cannot register half-built.
        for relative in files:
            target = remote + "/" + relative
            temporary = target + ".tt-upload"
            data = (title / relative).read_bytes()
            try:
                readback = download(ftp, temporary)
                if relative in references:
                    verify_elf(references[relative], readback)
                elif readback != data:
                    raise ValueError("unfinished temporary upload")
            except (ValueError, error_perm):
                ftp.storbinary("STOR " + temporary, BytesIO(data), blocksize=256 * 1024)
                readback = download(ftp, temporary)
            if relative in references:
                verify_elf(references[relative], readback)
            elif readback != data:
                raise RuntimeError(f"readback differs: {relative}")
            ftp.rename(temporary, target)
            final = download(ftp, target)
            if relative in references:
                verify_elf(references[relative], final)
            elif final != data:
                raise RuntimeError(f"final readback differs: {relative}")
            print(f"Uploaded and verified {relative}", flush=True)
        # Registration copies the icon; the home screen reads these copies,
        # never the installed one.
        icon = (title / "sce_sys/icon0.png").read_bytes()
        for target in ["/user/app/PPSA99782/icon0.png",
                       "/user/app/PPSA99782/sce_sys/icon0.png",
                       "/user/appmeta/PPSA99782/icon0.png"]:
            try:
                if download(ftp, target) == icon:
                    continue
            except error_perm:
                continue  # not registered yet
            ftp.storbinary("STOR " + target, BytesIO(icon))
            if download(ftp, target) != icon:
                raise RuntimeError(f"icon readback differs: {target}")
            print(f"Refreshed registered icon {target}", flush=True)
    print(f"Installed Table Tennis Recompiled at {remote}", flush=True)


if __name__ == "__main__":
    main()
