#!/bin/zsh
# Profile which guest functions an unattended match runs.
#
#   tools/profile_match.sh [seconds of gameplay, default 240]
#
# Needs a build with the profiler from tools/gen_function_profile.py. Boots
# through the test path into an Exhibition, rallies, then ranks the functions
# that ran during gameplay only (menus and loading left out) into
# out/profile/hot.tsv.
set -e
ROOT="${0:A:h:h}"
PLAY="${1:-240}"
RUN="$ROOT/out/profile/run"
LOG="$RUN/game.log"
rm -rf "$RUN"; mkdir -p "$RUN"
data=()
if [[ ! -f "$ROOT/game/default.xex" ]]; then
  data=(--game_data_root="${XDG_DATA_HOME:-$HOME/.local/share}/tabletennis/game")
fi

TABLETENNIS_PROFILE_DIR="$RUN" "$ROOT/run.sh" "${data[@]}" --tabletennis_test_path=true \
  --tabletennis_test_rally=true --log_level=info --log_file="$LOG" >"$RUN/stdout.log" 2>&1 &
GAME=$!
START=$SECONDS
trap 'kill $GAME 2>/dev/null || true' EXIT

until grep -q "gameplay reached" "$LOG" 2>/dev/null; do
  kill -0 $GAME 2>/dev/null || { echo "game exited before gameplay; see $RUN" >&2; exit 1; }
  (( SECONDS - START < 300 )) || { echo "no gameplay after 300s; see $RUN" >&2; exit 1; }
  sleep 1
done
GAMEPLAY=$(( SECONDS - START ))
echo "gameplay at ${GAMEPLAY}s, profiling ${PLAY}s of it"
sleep $(( PLAY + 12 ))
kill $GAME; wait $GAME 2>/dev/null || true

# Skip the first snapshot inside gameplay so the match is fully loaded.
FROM=$(( (GAMEPLAY / 10 + 2) * 10 ))
FIRST=$(printf "$RUN/t%05d.tsv" $FROM)
LAST=$(ls "$RUN"/t*.tsv | tail -1)
[[ -f "$FIRST" && "$FIRST" != "$LAST" ]] || { echo "not enough snapshots in $RUN" >&2; exit 1; }
python3 "$ROOT/tools/rank_hot_functions.py" "$FIRST" "$LAST" > "$ROOT/out/profile/hot.tsv"
echo "ranked ${FIRST:t}..${LAST:t} into out/profile/hot.tsv"
