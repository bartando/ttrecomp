#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Reject PS5 ELF layouts with code outside executable load segments."""
import argparse
import struct
from pathlib import Path


def check(data):
    if len(data) < 64:
        raise ValueError("truncated ELF header")
    header = struct.unpack_from("<16sHHIQQQIHHHHHH", data)
    if header[0][:7] != b"\x7fELF\x02\x01\x01" or header[2] != 62:
        raise ValueError("expected little-endian x86-64 ELF")
    if header[1] not in (3, 0xFE10):
        raise ValueError("expected PIE or native PS5 dynamic executable")
    if header[9] != 56 or not header[10]:
        raise ValueError("invalid program-header table")
    if header[5] + header[9] * header[10] > len(data):
        raise ValueError("truncated program-header table")
    segments = [struct.unpack_from("<IIQQQQQQ", data, header[5] + i * header[9])
                for i in range(header[10])]
    loads = [segment for segment in segments if segment[0] == 1]
    executable = [segment for segment in loads if segment[1] & 1]

    def executable_range(address, size):
        return any(s[3] <= address and address + size <= s[3] + s[6]
                   for s in executable)

    if not executable_range(header[4], 1):
        raise ValueError("entry point is outside executable LOAD segments")
    for segment in loads:
        if segment[7] != 0x4000 or segment[2] % 0x4000 != segment[3] % 0x4000:
            raise ValueError("LOAD segment does not meet PS5 16 KiB alignment")
        if segment[5] > segment[6] or segment[2] + segment[5] > len(data):
            raise ValueError("invalid or truncated LOAD segment")
    if header[12]:
        if header[11] != 64 or header[6] + header[11] * header[12] > len(data):
            raise ValueError("invalid or truncated section-header table")
        for i in range(header[12]):
            section = struct.unpack_from("<IIQQQQIIQQ", data, header[6] + i * header[11])
            if section[2] & 4 and section[5] and not executable_range(section[3], section[5]):
                raise ValueError(f"executable section {i} is outside executable LOAD segments")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("elf", type=Path)
    args = parser.parse_args()
    try:
        check(args.elf.read_bytes())
    except (OSError, ValueError, struct.error) as error:
        parser.exit(1, f"FAIL: {error}\n")
    print("PASS: entry, executable sections and 16 KiB LOAD alignment")


if __name__ == "__main__":
    main()
