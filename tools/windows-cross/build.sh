#!/usr/bin/env bash
# Cross-compiles tabletennis.exe in Docker and packages
# out/package/TableTennisRecomp-Windows.zip, the same layout as
# tools/package_windows.ps1. Needs generated/ from a prior codegen run.
#
# usage: tools/windows-cross/build.sh [Release|RelWithDebInfo]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
CONFIG="${1:-Release}"
IMAGE=ttrecomp-win-cross
BUILD=out/build/win-amd64-cross
OUT="$ROOT/out/package"
NAME=TableTennisRecomp

docker build -q -t "$IMAGE" "$ROOT/tools/windows-cross" >/dev/null

# Mounted at /src so no host path ends up in the binary.
docker run --rm -v "$ROOT":/src -w /src "$IMAGE" bash -ec "
git config --global --add safe.directory '*'
cmake -S . -B $BUILD -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE=/opt/windows-cross/toolchain.cmake \
    -DCMAKE_BUILD_TYPE=$CONFIG \
    -DCMAKE_C_FLAGS=-march=x86-64-v3 -DCMAKE_CXX_FLAGS=-march=x86-64-v3 \
    -DREXSDK_DIR=/src/third_party/rexglue-sdk \
    -DREXGLUE_ENABLE_TRACY=OFF
ninja -C $BUILD tabletennis
"

STAGE="$OUT/$NAME"
rm -rf "$STAGE"
mkdir -p "$STAGE"
cp "$ROOT/$BUILD/tabletennis.exe" "$ROOT/$BUILD"/*.dll "$STAGE/"

for file in "$STAGE"/*; do
    if grep -q -F "$HOME" "$file"; then
        echo "refusing to package: $HOME appears in $(basename "$file")" >&2
        exit 1
    fi
done

ZIP="$OUT/$NAME-Windows.zip"
rm -f "$ZIP"
(cd "$OUT" && zip -qr "$ZIP" "$NAME")
echo "$ZIP"
