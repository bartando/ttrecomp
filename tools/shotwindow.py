#!/usr/bin/env python3
"""Screenshot just the game window, so crops are stable between runs.

    shotwindow.py out.png [window-title-substring]
"""

import subprocess
import sys

from Quartz import (
    CGWindowListCopyWindowInfo,
    kCGNullWindowID,
    kCGWindowListOptionOnScreenOnly,
)


def find(sub):
    for w in CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly, kCGNullWindowID):
        name = w.get("kCGWindowName") or ""
        owner = w.get("kCGWindowOwnerName") or ""
        if sub.lower() in name.lower() or sub.lower() in owner.lower():
            b = w["kCGWindowBounds"]
            # Skip tiny helper windows.
            if b["Width"] > 200 and b["Height"] > 200:
                return w["kCGWindowNumber"]
    return None


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "/tmp/window.png"
    sub = sys.argv[2] if len(sys.argv) > 2 else "tabletennis"
    wid = find(sub)
    if wid is None:
        raise SystemExit(f"no window matching {sub!r}")
    subprocess.run(["screencapture", "-x", "-o", "-l", str(wid), out], check=True)
    print(out)


if __name__ == "__main__":
    main()
