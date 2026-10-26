#!/bin/sh
# Builds an optimized Glance.app into ./build.
set -e
cd "$(dirname "$0")/.."

swift build -c release
BIN="$(swift build -c release --show-bin-path)/Glance"

APP=build/Glance.app
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "$BIN" "$APP/Contents/MacOS/Glance"
cp Resources/Info.plist "$APP/Contents/Info.plist"
cp Resources/Glance.icns "$APP/Contents/Resources/Glance.icns"

# Video playback: libmpv and its libraries (scripts/fetch-mac-deps.sh), loaded only when a video
# is opened. Without them Glance still builds and runs; it just doesn't list videos.
if [ -d build/deps/lib ]; then
    mkdir -p "$APP/Contents/Frameworks"
    cp build/deps/lib/*.dylib "$APP/Contents/Frameworks/"
    codesign --force --sign - "$APP/Contents/Frameworks/"*.dylib
fi

# Ad-hoc signature so Gatekeeper lets a locally built app run.
codesign --force --sign - "$APP"
echo "Built $APP"
