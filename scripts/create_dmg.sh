#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
VERSION=${1:-0.5.0}
APP="$ROOT/dist/ChronoForge.app"
STAGING="$ROOT/dist/dmg-staging"
DMG="$ROOT/dist/ChronoForge-${VERSION}-arm64.dmg"

if [ ! -d "$APP" ]; then
    "$ROOT/scripts/package_macos.sh" release
fi

rm -rf "$STAGING" "$DMG"
mkdir -p "$STAGING"
ditto "$APP" "$STAGING/ChronoForge.app"
ln -s /Applications "$STAGING/Applications"
hdiutil create -volname "ChronoForge" -srcfolder "$STAGING" -ov -format UDZO "$DMG" >/dev/null
rm -rf "$STAGING"
echo "$DMG"
