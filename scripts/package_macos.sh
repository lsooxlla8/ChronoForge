#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
CONFIGURATION=${1:-release}
OUTPUT="$ROOT/dist"
APP="$OUTPUT/ChronoForge.app"

swift build --package-path "$ROOT" -c "$CONFIGURATION"
BIN_DIR=$(swift build --package-path "$ROOT" -c "$CONFIGURATION" --show-bin-path)

rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
install -m 755 "$BIN_DIR/ChronoForgeMac" "$APP/Contents/MacOS/ChronoForgeMac"
install -m 644 "$ROOT/apps/macos/Resources/Info.plist" "$APP/Contents/Info.plist"

ICONSET="$OUTPUT/ChronoForge.iconset"
ICON_BASE="$OUTPUT/ChronoForge-1024.png"
rm -rf "$ICONSET"
mkdir -p "$ICONSET"
sips -s format png "$ROOT/apps/macos/Resources/ChronoForgeIcon.svg" --out "$ICON_BASE" >/dev/null
for SIZE in 16 32 128 256 512; do
    sips -z "$SIZE" "$SIZE" "$ICON_BASE" --out "$ICONSET/icon_${SIZE}x${SIZE}.png" >/dev/null
    DOUBLE=$((SIZE * 2))
    sips -z "$DOUBLE" "$DOUBLE" "$ICON_BASE" --out "$ICONSET/icon_${SIZE}x${SIZE}@2x.png" >/dev/null
done
iconutil -c icns "$ICONSET" -o "$APP/Contents/Resources/ChronoForge.icns"
rm -rf "$ICONSET" "$ICON_BASE"

codesign --force --deep --sign - "$APP"
echo "$APP"
