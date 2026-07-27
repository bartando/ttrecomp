#!/usr/bin/env python3
"""List XTR memory commands overlapping one physical guest range."""

from __future__ import annotations

import argparse
import ctypes
import struct
from pathlib import Path


HEADER_SIZE = 48
COMMAND_SIZES = {
    0: 12,  # primary buffer start
    1: 4,   # primary buffer end
    2: 12,  # indirect buffer start
    3: 4,   # indirect buffer end
    5: 4,   # packet end
    9: 8,   # event
}


def decode_snappy(payload: bytes, decoded_length: int) -> bytes:
    library = ctypes.CDLL("/opt/homebrew/lib/libsnappy.dylib")
    library.snappy_uncompress.argtypes = [
        ctypes.c_void_p,
        ctypes.c_size_t,
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.c_size_t),
    ]
    library.snappy_uncompress.restype = ctypes.c_int
    source = ctypes.create_string_buffer(payload)
    destination = ctypes.create_string_buffer(decoded_length)
    destination_length = ctypes.c_size_t(decoded_length)
    result = library.snappy_uncompress(
        source,
        len(payload),
        destination,
        ctypes.byref(destination_length),
    )
    if result != 0 or destination_length.value != decoded_length:
        raise RuntimeError(
            f"Snappy decode failed: result={result}, "
            f"decoded={destination_length.value}/{decoded_length}"
        )
    return destination.raw[:decoded_length]


def fnv1a64(payload: bytes) -> int:
    value = 1469598103934665603
    for byte in payload:
        value = ((value ^ byte) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return value


def parse_address(value: str) -> int:
    return int(value, 0)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("trace", type=Path)
    parser.add_argument("address", type=parse_address)
    parser.add_argument("size", type=parse_address)
    arguments = parser.parse_args()

    trace = arguments.trace.read_bytes()
    offset = HEADER_SIZE
    command = 0
    packet = 0
    draws_seen = 0
    target_start = arguments.address
    target_end = target_start + arguments.size

    while offset < len(trace):
        command_type = struct.unpack_from("<I", trace, offset)[0]
        if command_type in COMMAND_SIZES:
            offset += COMMAND_SIZES[command_type]
        elif command_type == 4:  # packet start
            _, _, count = struct.unpack_from("<III", trace, offset)
            if count:
                packet_word = struct.unpack_from(">I", trace, offset + 12)[0]
                packet_type = packet_word >> 30
                opcode = (packet_word >> 8) & 0x7F
                if packet_type == 3 and opcode in (0x22, 0x36):
                    draws_seen += 1
            offset += 12 + count * 4
            packet += 1
        elif command_type in (6, 7):  # memory read/write
            _, base, encoding, encoded_length, decoded_length = (
                struct.unpack_from("<IIIII", trace, offset)
            )
            payload_start = offset + 20
            payload_end = payload_start + encoded_length
            range_end = base + decoded_length
            overlap_start = max(base, target_start)
            overlap_end = min(range_end, target_end)
            if overlap_start < overlap_end:
                encoded = trace[payload_start:payload_end]
                if encoding == 0:
                    decoded = encoded
                elif encoding == 1:
                    decoded = decode_snappy(encoded, decoded_length)
                else:
                    raise RuntimeError(f"Unknown memory encoding {encoding}")
                overlap = decoded[
                    overlap_start - base:overlap_end - base
                ]
                print(
                    f"command={command} packet={packet} "
                    f"draws_seen={draws_seen} "
                    f"type={'read' if command_type == 6 else 'write'} "
                    f"base=0x{base:08X} size={decoded_length} "
                    f"overlap=0x{overlap_start:08X}+{len(overlap)} "
                    f"zero={overlap.count(0)}/{len(overlap)} "
                    f"fnv=0x{fnv1a64(overlap):016X} "
                    f"head={overlap[:28].hex().upper()}"
                )
            offset = payload_end
        elif command_type == 8:  # EDRAM snapshot
            _, _, encoded_length = struct.unpack_from("<III", trace, offset)
            offset += 12 + encoded_length
        elif command_type == 10:  # register range
            encoded_length = struct.unpack_from("<I", trace, offset + 20)[0]
            offset += 24 + encoded_length
        elif command_type == 11:  # gamma ramp
            encoded_length = struct.unpack_from("<I", trace, offset + 12)[0]
            offset += 16 + encoded_length
        else:
            raise RuntimeError(
                f"Unknown command type {command_type} at 0x{offset:X}"
            )
        command += 1


if __name__ == "__main__":
    main()
