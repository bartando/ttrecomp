#!/bin/zsh
# Drive the game to a screen and screenshot it, so rendering changes can be
# checked without a human at the keyboard.
#
#   ./autotest.sh <shot.png> [extra cvar flags...]
#
# Sends: skip attract video -> title (Start) -> main menu. Timings are
# generous because pipeline warm-up dominates the first run.
#
# Requires Terminal (or whichever host runs this) to hold Accessibility
# permission, since it drives the game with synthetic key events.
set -e
ROOT="${0:A:h}"
SHOT="${1:-/tmp/tt_shot.png}"
shift 2>/dev/null || true

pkill -f "out/build/macos-arm64-relwithdebinfo/tabletennis" 2>/dev/null || true
sleep 1

"$ROOT/run.sh" --log_level=info --log_file=/tmp/tt_autotest.log "$@" >/tmp/tt_autotest.out 2>&1 &
GAME=$!

key() { osascript -e "tell application \"System Events\" to key code $1" >/dev/null 2>&1 || true; }
focus() { osascript -e 'tell application "System Events" to set frontmost of process "tabletennis" to true' >/dev/null 2>&1 || true; }

# Wait for the window to exist.
for i in {1..40}; do
  if osascript -e 'tell application "System Events" to exists process "tabletennis"' 2>/dev/null | grep -q true; then break; fi
  sleep 1
done
sleep 18          # attract video + shader warm-up

focus; sleep 1
key 36; sleep 3   # Return = Start: skip video / leave title
key 36; sleep 3   # through to the menu
key 36; sleep 4

screencapture -x -o "$SHOT"
echo "captured $SHOT"
kill $GAME 2>/dev/null || true
