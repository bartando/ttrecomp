#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Unattended console run: launch the title, let the test path reach a match,
keep it running for a while, close it and download the log.

    python3 ps5/run_match.py --host 192.168.0.151 --seconds 60 \\
        --cvar gpu_cpu_profile=true --log out/ps5-port/ab/run.log
"""
import argparse
from ftplib import FTP, error_perm
from io import BytesIO
from pathlib import Path
import os
import socket
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parent.parent
TITLE = "PPSA99782"
TITLE_DIR = f"/data/homebrew/{TITLE}"
GAME_LOG = f"{TITLE_DIR}/tt-game.log"
CONFIG = f"{TITLE_DIR}/ttrecomp/ps5.toml"
TOOLS = ROOT / "out/ps5-port/tools"
GAMEPLAY_MARKER = "Table Tennis gameplay detected"


def payload(action):
    elf = TOOLS / f"title-{action}.elf"
    source = ROOT / "ps5/tools/title_ctl.c"
    if elf.exists() and elf.stat().st_mtime >= source.stat().st_mtime:
        return elf
    TOOLS.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ,
               LLVM_CONFIG="/opt/homebrew/opt/llvm@21/bin/llvm-config",
               PS5_PROBE_LINKER=str(ROOT / "out/ps5-probe/linker21/lld/21.1.7/bin/ld.lld"))
    subprocess.run([str(ROOT / "out/ps5-probe/ps5-payload-sdk/bin/prospero-clang"),
                    "-Wall", "-Wextra", "-Werror", "-O2", f"-DTITLE_CTL_{action.upper()}",
                    str(source), "-lSceSystemService", "-lSceUserService", "-o", str(elf)],
                   check=True, env=env)
    return elf


def send(host, port, action):
    data = payload(action).read_bytes()
    output = bytearray()
    with socket.create_connection((host, port), timeout=5) as connection:
        connection.settimeout(15)
        connection.sendall(data)
        connection.shutdown(socket.SHUT_WR)
        try:
            while chunk := connection.recv(4096):
                output.extend(chunk)
        except socket.timeout:
            pass
    lines = [line for line in output.decode(errors="replace").splitlines()
             if line.startswith("title_ctl:")]
    print("\n".join(lines) or f"title_ctl {action}: no reply", flush=True)


class Console:
    def __init__(self, host):
        self.host = host

    def __enter__(self):
        self.ftp = FTP()
        self.ftp.connect(self.host, 2121, timeout=10)
        self.ftp.login()
        return self

    def __exit__(self, *_):
        try:
            self.ftp.quit()
        except Exception:
            self.ftp.close()

    def running(self):
        lines = []
        self.ftp.retrlines("LIST /mnt/sandbox", lines.append)
        return any(TITLE in line for line in lines)

    def read(self, path):
        out = BytesIO()
        self.ftp.retrbinary("RETR " + path, out.write)
        return out.getvalue()

    def write(self, path, data):
        self.ftp.storbinary("STOR " + path, BytesIO(data))


def wait_until(predicate, timeout, interval=2.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return True
        time.sleep(interval)
    return False


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", required=True)
    parser.add_argument("--loader-port", type=int, default=9021)
    parser.add_argument("--seconds", type=int, default=60,
                        help="how long to keep the match running")
    parser.add_argument("--reach-timeout", type=int, default=240)
    parser.add_argument("--cvar", action="append", default=[],
                        help="extra name=value for ps5.toml (repeatable)")
    parser.add_argument("--log", type=Path, required=True)
    parser.add_argument("--no-test-path", action="store_true",
                        help="stay in the menus (for scripted pad input) instead of "
                             "launching a match; the run then lasts --seconds from launch")
    parser.add_argument("--rumble", action="store_true",
                        help="keep controller rumble on (off by default: nobody holds the pad)")
    parser.add_argument("--no-rally", action="store_true",
                        help="stand at the table instead of tapping A in the match")
    args = parser.parse_args()

    config = [] if args.no_test_path else ["tabletennis_test_path = true"]
    if not args.no_rally and not args.no_test_path:
        # Serve and swing so measurements cover real rallies, not a player
        # standing at the table.
        config.append("tabletennis_test_rally = true")
    if not args.rumble:
        config.append("ps5_pad_rumble = false")
    for item in args.cvar:
        name, _, value = item.partition("=")
        if not value:
            parser.error(f"--cvar needs name=value: {item}")
        if value not in ("true", "false") and not value.lstrip("-").replace(".", "", 1).isdigit():
            value = '"' + value + '"'
        config.append(f"{name.strip()} = {value}")

    with Console(args.host) as console:
        if console.running():
            send(args.host, args.loader_port, "kill")
            if not wait_until(lambda: not console.running(), 30):
                sys.exit("previous run did not close")
        console.write(CONFIG, ("\n".join(config) + "\n").encode())
        # A stale log would satisfy the gameplay check before the title
        # recreates it. The server answers DELE with 226, which ftplib's
        # delete() rejects.
        try:
            console.ftp.sendcmd("DELE " + GAME_LOG)
        except error_perm:
            pass

    send(args.host, args.loader_port, "launch")
    started = time.monotonic()
    reached = False
    with Console(args.host) as console:
        def in_match():
            try:
                return GAMEPLAY_MARKER.encode() in console.read(GAME_LOG)
            except error_perm:
                return False
        if not wait_until(console.running, 30):
            sys.exit("title did not start")
        if args.no_test_path:
            time.sleep(args.seconds)
            reached = True
        elif reached := wait_until(in_match, args.reach_timeout, interval=5.0):
            print(f"in match after {time.monotonic() - started:.0f}s; "
                  f"running {args.seconds}s", flush=True)
            time.sleep(args.seconds)
        else:
            print("did not reach a match; collecting the log anyway", flush=True)

    send(args.host, args.loader_port, "kill")
    with Console(args.host) as console:
        if not wait_until(lambda: not console.running(), 30):
            send(args.host, args.loader_port, "kill")
            if not wait_until(lambda: not console.running(), 30):
                print("warning: title still running after two kills", flush=True)
        args.log.parent.mkdir(parents=True, exist_ok=True)
        args.log.write_bytes(console.read(GAME_LOG))
    print(f"log -> {args.log}")
    sys.exit(0 if reached else 1)


if __name__ == "__main__":
    main()
