#!/bin/zsh
# Launch, drive to the Exhibition/Tournament character-select screen, and
# screenshot the character. That screen is the reproduction case for the black
# skin bug, so this makes rendering changes testable without a human at the
# keyboard.
#
#   ./repro.sh out.png [--some_cvar=value ...]
#
# Writes out.png (the window) and out_char.png (cropped to the character).
# Verifies it actually arrived - shader warm-up makes the timing vary, so a
# fixed key sequence lands on the wrong screen often enough to matter - and
# retries before giving up. Exits non-zero if it never gets there, so a failed
# run is never mistaken for a rendering result.
#
# Needs pyobjc-framework-Quartz and Accessibility permission for the caller.
set -e
ROOT="${0:A:h}"
SHOT="${1:-/tmp/repro.png}"; shift 2>/dev/null || true
PY=${TT_PYTHON:-/opt/homebrew/opt/python@3.10/bin/python3.10}
CHAR="${SHOT%.png}_char.png"

pkill -f "macos-arm64-relwithdebinfo/tabletennis" 2>/dev/null || true
sleep 1

"$ROOT/run.sh" --log_level=info --log_file=/tmp/tt_repro.log "$@" >/tmp/tt_repro.out 2>&1 &

for i in {1..45}; do
  osascript -e 'tell application "System Events" to exists process "tabletennis"' 2>/dev/null | grep -q true && break
  sleep 1
done
sleep 22   # attract video + shader warm-up

k(){ $PY "$ROOT/tools/sendkey.py" "$1" 0.2; sleep "${2:-2}"; }
focus(){ osascript -e 'tell application "System Events" to set frontmost of process "tabletennis" to true' >/dev/null 2>&1 || true; }

capture(){
  $PY "$ROOT/tools/shotwindow.py" "$SHOT" >/dev/null 2>&1 || return 1
  $PY - "$SHOT" "$CHAR" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]); W, H = im.size
im.crop((int(W*.15), int(H*.05), int(W*.55), int(H*.75))).save(sys.argv[2])
PY
}

# The jersey is brightly lit on the target screen and black everywhere else,
# so it doubles as an "are we there yet" probe.
arrived(){
  capture || return 1
  $PY - "$CHAR" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB"); W, H = im.size
px = list(im.crop((int(W*.22), int(H*.45), int(W*.34), int(H*.60))).getdata())
sys.exit(0 if sum(sum(q) for q in px) / (3 * len(px)) > 12 else 1)
PY
}

focus; sleep 1
k return 4      # skip attract
k return 5      # title -> main menu

for attempt in 1 2 3 4 5; do
  focus
  k space 6     # accept the highlighted mode -> character select
  k return 3    # settle
  if arrived; then
    echo "reached character select (attempt $attempt) -> $CHAR"
    pkill -f "macos-arm64-relwithdebinfo/tabletennis" 2>/dev/null || true
    exit 0
  fi
  k return 3    # nudge past whatever intermediate screen we landed on
done

echo "FAILED to reach character select" >&2
pkill -f "macos-arm64-relwithdebinfo/tabletennis" 2>/dev/null || true
exit 1
