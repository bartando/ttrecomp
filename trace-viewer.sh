#!/bin/zsh
# Open a captured .xtr GPU frame. With no argument, open the newest capture.
set -e
ROOT="${0:A:h}"
BUILD="$ROOT/out/build/macos-arm64-relwithdebinfo"
TRACE="$1"
if (( $# > 0 )); then
  shift
fi

if [[ -z "$TRACE" ]]; then
  traces=("$ROOT"/out/trace/frames/*.xtr(N.om))
  if (( ${#traces} == 0 )); then
    echo "no frame traces found; run: ./repro.sh /tmp/tt.png --capture-trace" >&2
    exit 1
  fi
  TRACE="$traces[1]"
fi

export DYLD_LIBRARY_PATH="$ROOT/third_party/rexglue-sdk/out/macos-arm64${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}"
exec "$BUILD/tabletennis_trace_viewer" --target_trace_file="$TRACE" "$@"
