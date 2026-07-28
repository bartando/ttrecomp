#!/bin/zsh
# Launch, drive to the Tournament character-select screen, and
# screenshot the character. That screen is the reproduction case for the black
# skin bug, so this makes rendering changes testable without a human at the
# keyboard.
#
#   ./repro.sh out.png [--capture-trace] [--capture-gameplay-trace]
#                    [--enter-gameplay] [--play-rally] [--test-path]
#                    [--native-takeover]
#                    [--some_cvar=value ...]
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
LOG=${TT_LOG:-/tmp/tt_repro.log}
STDOUT_LOG=${TT_STDOUT_LOG:-/tmp/tt_repro.out}
TRACE_DIR="$ROOT/out/trace/frames"
CAPTURE_TRACE=false
CAPTURE_GAMEPLAY_TRACE=false
ENTER_GAMEPLAY=false
PLAY_RALLY=false
NATIVE_TAKEOVER=false
TEST_PATH=false
extra=()
# Keep automated renderer runs silent without replacing the title's audio
# clock or XMA services, both of which may participate in game timing.
extra+=(--audio_mute=true)
for arg in "$@"; do
  if [[ "$arg" == "--capture-trace" ]]; then
    CAPTURE_TRACE=true
  elif [[ "$arg" == "--capture-gameplay-trace" ]]; then
    CAPTURE_GAMEPLAY_TRACE=true
    ENTER_GAMEPLAY=true
    PLAY_RALLY=true
  elif [[ "$arg" == "--enter-gameplay" ]]; then
    ENTER_GAMEPLAY=true
  elif [[ "$arg" == "--play-rally" ]]; then
    ENTER_GAMEPLAY=true
    PLAY_RALLY=true
  elif [[ "$arg" == "--native-takeover" ]]; then
    ENTER_GAMEPLAY=true
    NATIVE_TAKEOVER=true
  elif [[ "$arg" == "--test-path" || "$arg" == "--skip-menu" ]]; then
    ENTER_GAMEPLAY=true
    TEST_PATH=true
  else
    extra+=("$arg")
  fi
done
if $CAPTURE_TRACE || $CAPTURE_GAMEPLAY_TRACE; then
  mkdir -p "$TRACE_DIR"
  extra+=(--trace_gpu_prefix="$TRACE_DIR")
fi
if $NATIVE_TAKEOVER; then
  # Enable the renderer from launch; its title-level gameplay detector yields
  # every frontend/loading frame and starts serving only once both match
  # players are submitted. This is more reliable than synthesizing F5 after a
  # long, variable shader-loading sequence.
  extra+=(--tabletennis_native_render=true)
fi
if $TEST_PATH; then
  # The title-side test path owns its state-aware Start/Continue traversal.
  # The host harness sends no menu input and waits until render capture proves
  # that both match players are live.
  # MoltenVK can leave an async placeholder pipeline resident indefinitely,
  # presenting a solid-magenta frame while the guest keeps running. Test runs
  # need deterministic real frames more than background compilation, so build
  # missing pipelines synchronously on this path.
  extra+=(--tabletennis_test_path=true
          --async_shader_compilation=false)
  if $CAPTURE_TRACE || $CAPTURE_GAMEPLAY_TRACE; then
    extra+=(--tabletennis_test_capture_gameplay_trace=true)
  fi
fi

TRACE_COUNT_BEFORE_LAUNCH=0
if $CAPTURE_TRACE || $CAPTURE_GAMEPLAY_TRACE; then
  traces_before_launch=("$TRACE_DIR"/*.xtr(N))
  TRACE_COUNT_BEFORE_LAUNCH=${#traces_before_launch}
fi

marker_count(){
  local count
  count=$(rg -c "$1" "$LOG" 2>/dev/null || true)
  echo "${count:-0}"
}
CHARACTER_MARKERS_BEFORE=$(marker_count "Table Tennis character select detected")
GAMEPLAY_MARKERS_BEFORE=$(marker_count "Table Tennis gameplay detected")
QUIESCE_MARKERS_BEFORE=$(marker_count \
  "Table Tennis PS328 title capture retired")
CAPTURE_POST_QUIESCE_SAMPLE=${TT_CAPTURE_POST_QUIESCE_SAMPLE:-true}
POST_QUIESCE_SAMPLE_DELAY=${TT_POST_QUIESCE_SAMPLE_DELAY:-1}
POST_QUIESCE_SAMPLE_SECONDS=${TT_POST_QUIESCE_SAMPLE_SECONDS:-5}
POST_QUIESCE_SAMPLE_FILE=${TT_POST_QUIESCE_SAMPLE_FILE:-${LOG%.log}_sample.txt}
POST_QUIESCE_PROGRESS_FILE=${TT_POST_QUIESCE_PROGRESS_FILE:-${LOG%.log}_progress.txt}
STALL_MONITOR_PID=0

pkill -f "macos-arm64-relwithdebinfo/tabletennis" 2>/dev/null || true
sleep 1

"$ROOT/run.sh" --log_level=info --log_file="$LOG" "${extra[@]}" >"$STDOUT_LOG" 2>&1 &
GAME_PID=$!

record_stall_progress(){
  local phase="$1"
  {
    echo "phase=$phase time=$(date -u +%Y-%m-%dT%H:%M:%SZ) pid=$GAME_PID"
    ps -p "$GAME_PID" -o pid=,ppid=,state=,%cpu=,%mem=,etime=,command= \
      2>/dev/null || true
    rg "Table Tennis guest performance:" "$LOG" 2>/dev/null | tail -n 1 || true
    rg "post-quiesce guest swap|post-quiesce native output|private batch replay|private translated replay readback" \
      "$LOG" 2>/dev/null | tail -n 12 || true
  } >>"$POST_QUIESCE_PROGRESS_FILE"
}

monitor_post_quiesce_stall(){
  while kill -0 "$GAME_PID" 2>/dev/null; do
    if (( $(marker_count \
          "Table Tennis PS328 title capture retired") >
          QUIESCE_MARKERS_BEFORE )); then
      sleep "$POST_QUIESCE_SAMPLE_DELAY"
      kill -0 "$GAME_PID" 2>/dev/null || return
      : >"$POST_QUIESCE_PROGRESS_FILE"
      record_stall_progress before_sample
      /usr/bin/sample "$GAME_PID" "$POST_QUIESCE_SAMPLE_SECONDS" 1 \
        -file "$POST_QUIESCE_SAMPLE_FILE" \
        >>"$POST_QUIESCE_PROGRESS_FILE" 2>&1 || true
      record_stall_progress after_sample
      echo "post-quiesce process sample -> $POST_QUIESCE_SAMPLE_FILE"
      echo "post-quiesce progress -> $POST_QUIESCE_PROGRESS_FILE"
      return
    fi
    sleep 0.2
  done
}

if $CAPTURE_POST_QUIESCE_SAMPLE; then
  monitor_post_quiesce_stall &
  STALL_MONITOR_PID=$!
fi

for i in {1..45}; do
  osascript -e "tell application \"System Events\" to exists first process whose unix id is $GAME_PID" 2>/dev/null | grep -q true && break
  sleep 1
done

k(){ $PY "$ROOT/tools/sendkey.py" "$1" 0.2; sleep "${2:-2}"; }
focus(){
  osascript -e "tell application \"System Events\" to set frontmost of first process whose unix id is $GAME_PID to true" >/dev/null 2>&1 || true
}

stop_game(){
  if ! kill -0 "$GAME_PID" 2>/dev/null; then
    return
  fi
  # Let RexGlue flush shader and pipeline storage. Killing the process here can
  # leave a partial trailing record, making the next run rebuild a cold cache.
  focus
  osascript -e 'tell application "System Events" to keystroke "q" using command down' >/dev/null 2>&1 || true
  for _ in {1..100}; do
    if ! kill -0 "$GAME_PID" 2>/dev/null; then
      wait "$GAME_PID" 2>/dev/null || true
      return
    fi
    sleep 0.1
  done
  kill -TERM "$GAME_PID" 2>/dev/null || true
  for _ in {1..20}; do
    if ! kill -0 "$GAME_PID" 2>/dev/null; then
      wait "$GAME_PID" 2>/dev/null || true
      return
    fi
    sleep 0.1
  done
  kill -KILL "$GAME_PID" 2>/dev/null || true
  wait "$GAME_PID" 2>/dev/null || true
}

wait_for_post_quiesce_sample(){
  if (( STALL_MONITOR_PID == 0 )) ||
      ! kill -0 "$STALL_MONITOR_PID" 2>/dev/null; then
    return
  fi
  if (( $(marker_count \
        "Table Tennis PS328 title capture retired") <=
        QUIESCE_MARKERS_BEFORE )); then
    return
  fi
  local tenths=$(( (POST_QUIESCE_SAMPLE_DELAY +
                    POST_QUIESCE_SAMPLE_SECONDS + 2) * 10 ))
  local sample_wait_index
  for (( sample_wait_index = 0;
         sample_wait_index < tenths;
         ++sample_wait_index )); do
    if ! kill -0 "$STALL_MONITOR_PID" 2>/dev/null; then
      wait "$STALL_MONITOR_PID" 2>/dev/null || true
      return
    fi
    sleep 0.1
  done
}

capture(){
  $PY "$ROOT/tools/shotwindow.py" "$SHOT" tabletennis "$GAME_PID" >/dev/null 2>&1 || return 1
  $PY - "$SHOT" "$CHAR" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]); W, H = im.size
im.crop((int(W*.15), int(H*.05), int(W*.55), int(H*.75))).save(sys.argv[2])
PY
}

capture_trace(){
  local label="$1"
  local before=("$TRACE_DIR"/*.xtr(N))
  local before_count=${#before}
  k f7 5
  local traces=("$TRACE_DIR"/*.xtr(N.om))
  if (( ${#traces} <= before_count )); then
    echo "FAILED to capture $label GPU frame trace" >&2
    return 1
  fi
  echo "captured $label GPU frame -> $traces[1]"
}

wait_for_title_trace(){
  local label="$1"
  local waited=0
  while (( waited < 30 )); do
    local traces=("$TRACE_DIR"/*.xtr(N.om))
    if (( ${#traces} > TRACE_COUNT_BEFORE_LAUNCH )); then
      sleep 3
      echo "captured $label GPU frame -> $traces[1]"
      return 0
    fi
    sleep 1
    (( waited += 1 ))
  done
  echo "FAILED to capture $label GPU frame from title marker" >&2
  return 1
}

if $TEST_PATH; then
  focus
  reached_gameplay=false
  timeout=${TT_TEST_PATH_TIMEOUT:-180}
  elapsed=0
  echo "waiting up to ${timeout}s for test-path gameplay verification"
  while (( elapsed < timeout )); do
    if (( $(marker_count "Table Tennis gameplay detected") >
          GAMEPLAY_MARKERS_BEFORE )); then
      reached_gameplay=true
      break
    fi
    if ! kill -0 "$GAME_PID" 2>/dev/null; then
      break
    fi
    sleep 1
    (( elapsed += 1 ))
  done
  if ! $reached_gameplay; then
    echo "FAILED: test path did not reach verified gameplay" >&2
    tail -n 30 "$LOG" >&2 2>/dev/null || true
    tail -n 30 "$STDOUT_LOG" >&2 2>/dev/null || true
    stop_game
    exit 1
  fi

  post_gameplay_wait=${TT_TEST_PATH_POST_WAIT:-3}
  if (( post_gameplay_wait > 0 )); then
    echo "waiting ${post_gameplay_wait}s for a presented gameplay frame"
    sleep "$post_gameplay_wait"
  fi
  # CGWindow snapshots of a long-backgrounded Metal window can return an old
  # backing-store frame. Bring the title forward immediately before evidence
  # capture so the screenshot reflects the frame the user would actually see.
  focus
  sleep 1
  capture || true
  echo "reached verified gameplay through test path after ${elapsed}s -> $SHOT"
  if $CAPTURE_TRACE || $CAPTURE_GAMEPLAY_TRACE; then
    wait_for_title_trace "gameplay" || {
      stop_game
      exit 1
    }
  fi
  if $PLAY_RALLY; then
    # Test-path traversal stops before this point, so these are gameplay
    # actions only, never blind frontend accepts.
    $PY "$ROOT/tools/sendkey.py" space 0.15 wait 0.8 space 0.55
    for rally_input in {1..40}; do
      k space 0.5
    done
  fi
  checkpoint="${SHOT%.png}_gameplay_final.png"
  focus
  sleep 1
  $PY "$ROOT/tools/shotwindow.py" "$checkpoint" tabletennis "$GAME_PID" >/dev/null 2>&1 || true
  echo "gameplay final -> $checkpoint"
  wait_for_post_quiesce_sample
  stop_game
  exit 0
fi

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

focus
reached_character_select=false
# Startup timing varies wildly with a cold shader cache. Alternate Start/A
# and wait for the title hook's first one-player scene instead of sleeping
# through a worst-case attract movie or guessing from pixels.
for attempt in {1..40}; do
  key=return
  (( attempt % 2 == 0 )) && key=space
  k "$key" 0.35
  if (( $(marker_count "Table Tennis character select detected") >
        CHARACTER_MARKERS_BEFORE )); then
    reached_character_select=true
    capture
    echo "reached character select (input $attempt) -> $CHAR"
    break
  fi
done

if $reached_character_select; then
    if $CAPTURE_TRACE; then
      k f7 3
      traces=("$TRACE_DIR"/*.xtr(N.om))
      if (( ${#traces} == 0 )); then
        echo "FAILED to capture GPU frame trace" >&2
        stop_game
        exit 1
      fi
      echo "captured GPU frame -> $traces[1]"
    fi
    if $ENTER_GAMEPLAY; then
      # All setup screens accept their highlighted default with A. Pulse A
      # only when the title has had a frame to consume it, and stop as soon as
      # the two-player capture hook reports the actual match. This replaces
      # six fixed waits plus the old 45-second arena guess.
      reached_gameplay=false
      for pulse in {1..120}; do
        if (( $(marker_count "Table Tennis gameplay detected") >
              GAMEPLAY_MARKERS_BEFORE )); then
          reached_gameplay=true
          break
        fi
        k space 0.35
        if [[ "${TT_GAMEPLAY_CHECKPOINTS:-0}" == "1" ]] &&
           (( pulse % 10 == 0 )); then
          checkpoint="${SHOT%.png}_gameplay_step$((pulse / 10)).png"
          $PY "$ROOT/tools/shotwindow.py" "$checkpoint" tabletennis "$GAME_PID" >/dev/null 2>&1 || true
          echo "gameplay checkpoint $((pulse / 10)) -> $checkpoint"
        fi
      done
      if ! $reached_gameplay; then
        echo "FAILED to reach gameplay" >&2
        stop_game
        exit 1
      fi
      echo "reached gameplay after $pulse A pulses"
      sleep 3
      if $NATIVE_TAKEOVER; then
        sleep 3
      fi
      if $PLAY_RALLY; then
        # Enter the serve stance, serve, then keep requesting safe topspin
        # returns. Inputs that arrive before the opponent's hit are ignored,
        # so repeat across multiple points instead of relying on one timing.
        $PY "$ROOT/tools/sendkey.py" space 0.15 wait 0.8 space 0.55
        if $CAPTURE_GAMEPLAY_TRACE; then
          trace_before=("$TRACE_DIR"/*.xtr(N))
          trace_before_count=${#trace_before}
          k f7 5
          traces=("$TRACE_DIR"/*.xtr(N.om))
          if (( ${#traces} <= trace_before_count )); then
            echo "FAILED to capture gameplay GPU frame trace" >&2
            stop_game
            exit 1
          fi
          echo "captured gameplay GPU frame -> $traces[1]"
        fi
        for rally_input in {1..40}; do
          if ! kill -0 "$GAME_PID" 2>/dev/null; then
            echo "FAILED: game process exited during gameplay rally" >&2
            wait "$GAME_PID" 2>/dev/null || true
            exit 1
          fi
          k space 0.5
        done
      fi
      if ! kill -0 "$GAME_PID" 2>/dev/null; then
        echo "FAILED: game process exited before gameplay evidence capture" >&2
        wait "$GAME_PID" 2>/dev/null || true
        exit 1
      fi
      checkpoint="${SHOT%.png}_gameplay_final.png"
      if ! $PY "$ROOT/tools/shotwindow.py" "$checkpoint" tabletennis "$GAME_PID" >/dev/null 2>&1; then
        echo "FAILED: could not capture live gameplay window" >&2
        stop_game
        exit 1
      fi
      echo "gameplay final -> $checkpoint"
    fi
    stop_game
    exit 0
fi

echo "FAILED to reach character select" >&2
stop_game
exit 1
