#!/bin/zsh
# Launch the recompiled build. Keeps librexruntime*.dylib resolvable, since it
# is emitted into the SDK output dir rather than next to the executable.
set -e
ROOT="${0:A:h}"
BUILD="$ROOT/out/build/macos-arm64-relwithdebinfo"
cp -f "$ROOT/tabletennis.toml" "$BUILD/" 2>/dev/null || true
export DYLD_LIBRARY_PATH="$ROOT/third_party/rexglue-sdk/out/macos-arm64${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}"
exec "$BUILD/tabletennis" "$@"
