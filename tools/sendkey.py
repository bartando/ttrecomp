#!/usr/bin/env python3
"""Post held key presses to the focused window.

AppleScript's `key code` sends a down/up pair back-to-back, which the game's
per-frame input polling can miss entirely. This holds each key for a real
duration instead.

    sendkey.py return 0.15              # hold Return for 150ms
    sendkey.py space 0.2 wait 0.5 space # timed sequence
"""

import sys
import time

from Quartz import (
    CGEventCreateKeyboardEvent,
    CGEventPost,
    CGEventSetFlags,
    kCGHIDEventTap,
    kCGEventFlagMaskSecondaryFn,
)

# macOS virtual key codes.
KEYS = {
    "return": 36, "enter": 36, "escape": 53, "space": 49, "tab": 48,
    "f2": 120, "f5": 96, "f7": 98,
    "up": 126, "down": 125, "left": 123, "right": 124,
    "a": 0, "s": 1, "d": 2, "w": 13, "c": 8, "e": 14, "f": 3, "q": 12, "r": 15,
}


def press(code, hold, function_key=False):
    for down in (True, False):
        event = CGEventCreateKeyboardEvent(None, code, down)
        if function_key:
            CGEventSetFlags(event, kCGEventFlagMaskSecondaryFn)
        CGEventPost(kCGHIDEventTap, event)
        if down:
            time.sleep(hold)
    time.sleep(0.05)


def main():
    args = sys.argv[1:]
    if not args:
        raise SystemExit(__doc__)
    while args:
        name = args.pop(0).lower()
        if name == "wait":
            if not args or not _isfloat(args[0]):
                raise SystemExit("wait requires a duration in seconds")
            time.sleep(float(args.pop(0)))
            continue
        hold = float(args.pop(0)) if args and _isfloat(args[0]) else 0.12
        if args and _isfloat(args[0]):
            args.pop(0)
        if name not in KEYS:
            raise SystemExit(f"unknown key: {name}")
        press(KEYS[name], hold, name.startswith("f") and name[1:].isdigit())


def _isfloat(s):
    try:
        float(s)
        return True
    except ValueError:
        return False


if __name__ == "__main__":
    main()
