#!/bin/zsh
# Capture GPU diagnostics for a rendering bug (black character skin, etc).
#
# Run this, play to the screen that shows the problem, then quit. Everything
# lands in out/trace/ for offline inspection.
#
#   ./trace.sh
#
# Produces:
#   out/trace/shaders/   every translated shader the game used
#   out/trace/gpu-*.rextrace  GPU command stream (large - only while needed)
#   out/trace/run.log    full debug log
set -e
ROOT="${0:A:h}"
OUT="$ROOT/out/trace"
mkdir -p "$OUT/shaders"

exec "$ROOT/run.sh" \
  --dump_shaders="$OUT/shaders" \
  --trace_gpu_stream=true \
  --trace_gpu_prefix="$OUT/gpu-" \
  --gpu_debug_markers=true \
  --log_level=debug \
  --log_file="$OUT/run.log" \
  "$@"
