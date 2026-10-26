#!/bin/sh
# Downloads the libraries Mac video playback uses (libmpv 0.41.0, FFmpeg and friends, universal)
# into build/deps/lib. It's Glance's own pinned copy of IINA's prebuilt set, and the SHA-256 must
# match, so a changed download fails the build. bundle.sh copies them into Glance.app.
set -e
cd "$(dirname "$0")/.."

URL="https://github.com/wards999/Glance/releases/download/mac-deps-20261026/iina-dylibs-universal-mpv0.41.0-20261026.tar.gz"
SHA256="07ba5b95e191c530f87282562b360e6070502182b8905e1303eb63868b5cc9e6"

mkdir -p build/deps
ARCHIVE=build/deps/mac-deps.tar.gz
if [ ! -f "$ARCHIVE" ] || [ "$(shasum -a 256 "$ARCHIVE" | cut -d' ' -f1)" != "$SHA256" ]; then
    curl -sSfL "$URL" -o "$ARCHIVE"
fi
ACTUAL="$(shasum -a 256 "$ARCHIVE" | cut -d' ' -f1)"
if [ "$ACTUAL" != "$SHA256" ]; then
    echo "mac-deps.tar.gz is $ACTUAL, expected the pinned $SHA256" >&2
    exit 1
fi
rm -rf build/deps/lib
tar -xzf "$ARCHIVE" -C build/deps
(cd build/deps/lib && shasum -a 256 -c ../SHA256SUMS.txt --quiet)
echo "Mac video libraries ready in build/deps/lib"
