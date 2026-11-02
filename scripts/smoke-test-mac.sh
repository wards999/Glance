#!/bin/sh
# Opens build/Glance.app on a picture and checks that it shows a window and is still running.
# A build that compiles can still fail at launch (an exception while the window is set up leaves
# only the Dock icon), so CI runs this after bundle.sh.
set -e
cd "$(dirname "$0")/.."

WORK=$(mktemp -d)
# Any picture will do: the generic document icon is on every Mac.
sips -s format png /System/Library/CoreServices/CoreTypes.bundle/Contents/Resources/GenericDocumentIcon.icns \
    --out "$WORK/test.png" > /dev/null

cat > "$WORK/windows.swift" <<'EOF'
import CoreGraphics
import Foundation
// Ordinary (layer 0) on-screen windows owned by the given process.
let pid = Int(CommandLine.arguments[1])!
let list = CGWindowListCopyWindowInfo([.optionOnScreenOnly], kCGNullWindowID) as? [[String: Any]] ?? []
let mine = list.filter { ($0[kCGWindowOwnerPID as String] as? Int) == pid && ($0[kCGWindowLayer as String] as? Int) == 0 }
print(mine.count)
EOF
swiftc -O "$WORK/windows.swift" -o "$WORK/windows"

build/Glance.app/Contents/MacOS/Glance "$WORK/test.png" > "$WORK/glance.log" 2>&1 &
PID=$!
sleep 8
WINDOWS=$("$WORK/windows" "$PID")
RUNNING=yes
kill -0 "$PID" 2> /dev/null || RUNNING=no
kill "$PID" 2> /dev/null || true

echo "Running: $RUNNING, windows: $WINDOWS"
echo "--- Glance's output ---"
cat "$WORK/glance.log"
echo "---"
if [ "$RUNNING" != yes ] || [ "$WINDOWS" -lt 1 ] || grep -qi "exception" "$WORK/glance.log"; then
    echo "Glance didn't start properly"
    exit 1
fi
echo "Glance started and showed its window"
