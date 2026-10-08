#!/bin/zsh
# Wrap a signed app in a read-only disk image. Copy the app to a writable
# folder before launching: the ISO installer keeps game files beside it.
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

cat > "$STAGE/Read Me.txt" <<'EOF'
Table Tennis Recomp — Apple Silicon

1. Create a folder you control, for example ~/Games/Table Tennis Recomp.
2. Drag TableTennisRecomp.app from this disk image into that folder.
3. Eject the disk image, then open the copied app.
4. Select your own Xbox 360 ISO of Rockstar Games Presents Table Tennis.

Do not launch from this disk image or put the app in Applications: the game
installer writes extracted files into a game folder beside the app. Settings
and logs also live beside the app; saves and shader caches use your user-data
directory. Keep your existing game folder and settings when replacing the app.

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
