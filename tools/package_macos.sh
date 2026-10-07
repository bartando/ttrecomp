#!/bin/zsh
# Packages a built tabletennis into TableTennisRecomp.app and a release zip.
# The bundle carries no game files: the app asks for the user's own ISO on
# first launch and extracts it next to the .app.
#
# usage: tools/package_macos.sh [build_dir] [output_dir]
set -euo pipefail

ROOT="${0:A:h:h}"
BUILD="${1:-$ROOT/out/build/macos-arm64-release}"
BUILD="${BUILD:A}"
OUT="${2:-$ROOT/out/package}"
OUT="${OUT:A}"
NAME="TableTennisRecomp"
APP="$OUT/$NAME.app"
VERSION="$(sed -nE 's/^project\(tabletennis VERSION ([0-9.]+).*/\1/p' "$ROOT/CMakeLists.txt")"

[[ -x "$BUILD/tabletennis" ]] || { echo "no tabletennis in $BUILD" >&2; exit 1; }

# The SDK writes its runtime dylibs into its own output tree, not the build dir.
LIB_DIRS=("$BUILD" $ROOT/third_party/rexglue-sdk/out/*(N/))

rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
MACOS="$APP/Contents/MacOS"
cp "$BUILD/tabletennis" "$MACOS/"

find_lib() {
  local lib
  for dir in "${LIB_DIRS[@]}"; do
    lib="$dir/$1"
    [[ -f "$lib" ]] && { echo "$lib"; return 0; }
  done
  return 1
}

# Copy every @rpath dependency, transitively. The executable's rpath is
# @loader_path, so they all sit next to it.
queue=("$MACOS/tabletennis")
while (( ${#queue} )); do
  binary="${queue[1]}"
  queue=("${(@)queue[2,-1]}")
  for dep in $(otool -L "$binary" | awk 'NR > 1 && $1 ~ /^@rpath\// { sub("@rpath/", "", $1); print $1 }'); do
    [[ -f "$MACOS/$dep" ]] && continue
    src="$(find_lib "$dep")" || { echo "missing dependency $dep" >&2; exit 1; }
    cp "$src" "$MACOS/"
    queue+=("$MACOS/$dep")
  done
done

# Vulkan is loaded at runtime from the executable's folder. The ICD manifest
# can't sit there (codesign only accepts code in Contents/MacOS), so it goes
# into Resources and points back at the dylib.
for lib in libvulkan.1.dylib libMoltenVK.dylib; do
  cp "$BUILD/$lib" "$MACOS/"
done
chmod -R u+w "$MACOS"
ICD_DIR="$APP/Contents/Resources/vulkan/icd.d"
mkdir -p "$ICD_DIR"
sed 's|"library_path": *"[^"]*"|"library_path": "../../../MacOS/libMoltenVK.dylib"|' \
  "$BUILD/MoltenVK_icd.json" > "$ICD_DIR/MoltenVK_icd.json"

ICONSET="$OUT/$NAME.iconset"
rm -rf "$ICONSET" && mkdir -p "$ICONSET"
for size in 16 32 128 256 512; do
  sips -z $size $size "$ROOT/assets/icon/tabletennis_icon.png" \
    --out "$ICONSET/icon_${size}x${size}.png" >/dev/null
  sips -z $((size * 2)) $((size * 2)) "$ROOT/assets/icon/tabletennis_icon.png" \
    --out "$ICONSET/icon_${size}x${size}@2x.png" >/dev/null
done
iconutil -c icns "$ICONSET" -o "$APP/Contents/Resources/$NAME.icns"
rm -rf "$ICONSET"

MIN_MACOS="$(otool -l "$MACOS/libvulkan.1.dylib" "$MACOS/tabletennis" |
  awk '$1 == "minos" { print $2 }' | sort -V | tail -1)"

cat > "$APP/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key><string>$NAME</string>
  <key>CFBundleDisplayName</key><string>Table Tennis Recomp</string>
  <key>CFBundleIdentifier</key><string>io.github.tabletennisrecomp</string>
  <key>CFBundleExecutable</key><string>tabletennis</string>
  <key>CFBundleIconFile</key><string>$NAME</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>$VERSION</string>
  <key>CFBundleVersion</key><string>$VERSION</string>
  <key>LSMinimumSystemVersion</key><string>$MIN_MACOS</string>
  <key>LSApplicationCategoryType</key><string>public.app-category.sports-games</string>
  <key>NSHighResolutionCapable</key><true/>
</dict>
</plist>
EOF

# Absolute rpaths point into the build tree; inside the bundle everything
# resolves through @loader_path. Stripping also drops local symbols and the
# debug map, which records absolute object paths.
for binary in "$MACOS"/tabletennis "$MACOS"/*.dylib; do
  for rpath in $(otool -l "$binary" | awk '$1 == "path" && $2 ~ /^\// { print $2 }'); do
    install_name_tool -delete_rpath "$rpath" "$binary" 2>/dev/null
  done
  strip -S -x "$binary" 2>/dev/null || true
done

# Fail rather than ship a binary that names the machine it was built on.
if grep -rlaF "$HOME" "$APP" >/dev/null; then
  echo "refusing to package: $HOME appears in:" >&2
  grep -rlaF "$HOME" "$APP" >&2
  exit 1
fi

# Ad-hoc signature only: Apple Silicon refuses unsigned code, and a Developer
# ID would put a real name on the release.
for binary in "$MACOS"/*.dylib; do
  codesign --force --sign - "$binary"
done
codesign --force --sign - "$APP"

ZIP="$OUT/$NAME-macOS.zip"
rm -f "$ZIP"
ditto -c -k --keepParent --norsrc "$APP" "$ZIP"
echo "$ZIP (macOS $MIN_MACOS+)"
