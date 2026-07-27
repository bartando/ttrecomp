#!/usr/bin/env python3
"""Screenshot just the game window, so crops are stable between runs.

    shotwindow.py out.png [window-title-substring] [owner-pid]
"""

import subprocess
import sys

from Quartz import (
    CGWindowListCopyWindowInfo,
    kCGNullWindowID,
    kCGWindowListOptionOnScreenOnly,
)


def find(sub, owner_pid=None):
    matches = []
    for w in CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly, kCGNullWindowID):
        name = w.get("kCGWindowName") or ""
        owner = w.get("kCGWindowOwnerName") or ""
        pid = w.get("kCGWindowOwnerPID")
        if sub.lower() in name.lower() or sub.lower() in owner.lower():
            b = w["kCGWindowBounds"]
            # Skip tiny helper windows.
            if (owner_pid is None or pid == owner_pid) and b["Width"] > 200 and b["Height"] > 200:
                matches.append(w)
    if not matches:
        return None
    # A stale uninterruptible app process may keep an old window registered.
    # With no explicit PID, prefer the newest matching application instance.
    return max(matches, key=lambda w: w.get("kCGWindowOwnerPID") or 0)["kCGWindowNumber"]


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "/tmp/window.png"
    sub = sys.argv[2] if len(sys.argv) > 2 else "tabletennis"
    owner_pid = int(sys.argv[3]) if len(sys.argv) > 3 else None
    wid = find(sub, owner_pid)
    if wid is None:
        pid_note = f" owned by PID {owner_pid}" if owner_pid is not None else ""
        raise SystemExit(f"no window matching {sub!r}{pid_note}")
    subprocess.run(["screencapture", "-x", "-o", "-l", str(wid), out], check=True)
    print(out)


if __name__ == "__main__":
    main()
