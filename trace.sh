#!/bin/zsh
# Capture GPU diagnostics for a rendering bug (black character skin, etc).
#
# Run this, play to the screen that shows the problem, then quit. Output lands
# in out/trace/ for offline inspection.
#
#   ./trace.sh                # dump translated shaders (safe, use this)
#   ./trace.sh --with-stream  # also stream the GPU command trace
#
# Note: --with-stream renders a black screen here. Shader dumping alone is
# enough to identify a bad material, so it is not the default.
set -e
ROOT="${0:A:h}"
OUT="$ROOT/out/trace"
mkdir -p "$OUT/shaders"

extra=()
if [[ "$1" == "--with-stream" ]]; then
  shift
  extra=(--trace_gpu_stream=true --trace_gpu_prefix="$OUT/gpu-")
fi

echo "dumping shaders to $OUT/shaders"
exec "$ROOT/run.sh" \
  --dump_shaders="$OUT/shaders" \
  --log_level=info \
  --log_file="$OUT/run.log" \
  "${extra[@]}" "$@"
