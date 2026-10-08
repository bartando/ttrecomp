#!/bin/zsh
# Wrap a signed app in a read-only disk image with an Applications shortcut.
# usage: tools/package_macos_dmg.sh [app_path] [output_dir]
set -euo pipefail

ROOT="${0:A:h:h}"
APP="${1:-$ROOT/out/package/TableTennisRecomp.app}"
APP="${APP:A}"
OUT="${2:-${APP:h}}"
OUT="${OUT:A}"

[[ -d "$APP" && "${APP:e}" == app ]] || { echo "no app bundle at $APP" >&2; exit 1; }
codesign --verify --deep --strict "$APP"
mkdir -p "$OUT"

STAGE="$(mktemp -d "${TMPDIR:-/tmp}/ttrecomp-dmg.XXXXXX")"
trap 'rm -rf "$STAGE"' EXIT
ditto --norsrc "$APP" "$STAGE/${APP:t}"
ln -s /Applications "$STAGE/Applications"

cat > "$STAGE/Read Me.txt" <<'EOF'
Table Tennis Recomp — Apple Silicon

1. Drag TableTennisRecomp.app onto the Applications shortcut.
2. Eject the disk image.
3. Open TableTennisRecomp from Applications.
4. Select your own Xbox 360 ISO of Rockstar Games Presents Table Tennis.

Game files, settings, logs, saves and shader caches are kept in
~/.local/share/tabletennis/ (or XDG_DATA_HOME/tabletennis when set), so replacing
the app does not remove them. Your ISO is only needed for installation.

The app is ad-hoc signed, not notarized. If macOS blocks it, use the approval
options in System Settings > Privacy & Security, then launch it again.

This download contains no retail game files. An early playable build;
first-use shader compilation and other occasional stutters may occur.
EOF

DMG="$OUT/TableTennisRecomp-macOS.dmg"
hdiutil create -ov -format UDZO -fs HFS+ \
  -volname "Table Tennis Recomp" -srcfolder "$STAGE" "$DMG"
hdiutil verify "$DMG"
echo "$DMG"
