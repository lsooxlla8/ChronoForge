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

codesign --force --deep --sign - "$APP"
echo "$APP"
