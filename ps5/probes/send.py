#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Send a diagnostic payload to elfldr; require its explicit success message."""
import argparse
from pathlib import Path
import socket
import sys


def send(payload, host, port, log_path, expected):
    data = payload.read_bytes()
    if (len(data) < 64 or data[:7] != b'\x7fELF\x02\x01\x01'
            or int.from_bytes(data[18:20], 'little') != 62):
        raise ValueError('Expected a little-endian ELF64 x86-64 payload')
    output = bytearray()
    with socket.create_connection((host, port), timeout=5) as connection:
        connection.settimeout(30)
        connection.sendall(data)
        connection.shutdown(socket.SHUT_WR)
        while True:
            try:
                chunk = connection.recv(4096)
            except socket.timeout:
                break
            if not chunk:
                break
            output.extend(chunk)
            if len(output) > 1024 * 1024:
                raise RuntimeError('Loader response exceeded 1 MiB')
    text = output.decode('utf-8', errors='replace')
    log_path.write_text(text)
    print(text, end='' if text.endswith('\n') else '\n')
    if expected not in text.splitlines():
        raise RuntimeError('Probe success was not confirmed; inspect the captured log')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', required=True)
    parser.add_argument('--port', type=int, default=9021)
    parser.add_argument('--payload', type=Path, required=True)
    parser.add_argument('--log', type=Path, required=True)
    parser.add_argument('--expect', default='TT PS5 platform probe: finished with 0 failures')
    args = parser.parse_args()
    if not 1 <= args.port <= 65535 or not args.expect.strip():
        parser.error('Provide a valid port and a nonempty expected success line')
    try:
        send(args.payload, args.host, args.port, args.log, args.expect)
    except (OSError, ValueError, RuntimeError) as error:
        print(f'Probe failed: {error}', file=sys.stderr)
        return 1
    print('Confirmed: diagnostic payload returned its success message')
    return 0


if __name__ == '__main__':
    sys.exit(main())
