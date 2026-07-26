#!/usr/bin/env python3
"""Extract files from an Xbox 360 XDVDFS (XGD1/2/3) disc image.

Usage:
    xdvdfs_extract.py ISO OUTDIR [--only PATH]... [--list]

--list          print the file tree and exit, extracting nothing
--only PATH     extract just this disc path (repeatable), e.g. --only /default.xex
"""

import argparse
import os
import struct
import sys

MAGIC = b"MICROSOFT*XBOX*MEDIA"
SECTOR = 2048

# Candidate base offsets of the game partition, by disc generation.
BASES = {
    "raw": 0x00000000,
    "XGD3": 0x02080000,
    "XGD2": 0x0FD90000,
    "XGD1": 0x18300000,
}


def find_base(f, size):
    for name, base in BASES.items():
        off = base + 0x10000
        if off + SECTOR > size:
            continue
        f.seek(off)
        if f.read(20) == MAGIC:
            return name, base
    raise SystemExit("no XDVDFS volume descriptor found - not an Xbox 360 disc image?")


def read_sectors(f, base, sector, size):
    f.seek(base + sector * SECTOR)
    return f.read(size)


def walk(f, base, sector, size, path=""):
    """Yield (discpath, start_sector, size, is_dir) for one directory table.

    The table is a binary tree of variable-length entries; offsets are in
    4-byte units relative to the start of the table.
    """
    data = read_sectors(f, base, sector, size)
    entries = []

    def visit(off):
        o = off * 4
        if o + 14 > len(data):
            return
        left, right, start, fsize, attr, namelen = struct.unpack("<HHIIBB", data[o:o + 14])
        if left == 0xFFFF and right == 0xFFFF:
            return
        name = data[o + 14:o + 14 + namelen].decode("latin-1")
        if left:
            visit(left)
        entries.append((path + "/" + name, start, fsize, bool(attr & 0x10)))
        if right:
            visit(right)

    visit(0)

    for discpath, start, fsize, is_dir in entries:
        yield discpath, start, fsize, is_dir
        if is_dir and fsize > 0:
            yield from walk(f, base, start, fsize, discpath)


def extract(f, base, start, fsize, dest):
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    f.seek(base + start * SECTOR)
    remaining = fsize
    with open(dest, "wb") as out:
        while remaining > 0:
            chunk = f.read(min(1 << 20, remaining))
            if not chunk:
                raise SystemExit(f"unexpected EOF while reading {dest}")
            out.write(chunk)
            remaining -= len(chunk)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("iso")
    ap.add_argument("outdir")
    ap.add_argument("--only", action="append", default=[])
    ap.add_argument("--list", action="store_true")
    args = ap.parse_args()

    size = os.path.getsize(args.iso)
    with open(args.iso, "rb") as f:
        gen, base = find_base(f, size)
        print(f"{gen} image, game partition at 0x{base:X}", file=sys.stderr)

        f.seek(base + 0x10000)
        desc = f.read(SECTOR)
        root_sector, root_size = struct.unpack("<II", desc[20:28])

        wanted = {p.lower() for p in args.only}
        total = 0
        for discpath, start, fsize, is_dir in walk(f, base, root_sector, root_size):
            if is_dir:
                if args.list:
                    print(f"{'DIR':>12}  {discpath}")
                continue
            if args.list:
                print(f"{fsize:12}  {discpath}")
                continue
            if wanted and discpath.lower() not in wanted:
                continue
            dest = os.path.join(args.outdir, discpath.lstrip("/"))
            extract(f, base, start, fsize, dest)
            total += fsize
            print(f"extracted {discpath} ({fsize} bytes)", file=sys.stderr)

        if not args.list:
            print(f"total {total} bytes", file=sys.stderr)


if __name__ == "__main__":
    main()
