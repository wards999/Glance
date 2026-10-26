#!/bin/sh
# Builds Glance.exe and the timing tools into windows/build with MinGW-w64 g++.
# (CMakeLists.txt does the same for anyone with a working CMake.)
set -e
cd "$(dirname "$0")"
mkdir -p build

FLAGS="-std=c++20 -O2 -march=x86-64-v2 -Wall -Wno-unknown-pragmas -Isrc
       -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DNOMINMAX -D_WIN32_WINNT=0x0A00"
# Static runtime: one exe, no MinGW DLLs to ship.
LINK="-municode -static -static-libgcc -static-libstdc++"
# PDF reading needs pdfium.dll next to Glance.exe (from bblanchon/pdfium-binaries, chromium/8076);
# see .github/workflows/build-windows.yml.
CORE="src/Decoder.cpp src/Pipeline.cpp src/Navigator.cpp src/Pdf.cpp"
WIC="-lwindowscodecs -lole32 -loleaut32 -luuid -lshlwapi -luser32"

windres glance.rc -O coff -o build/glance.res
g++ $FLAGS -mwindows src/main.cpp src/Viewer.cpp src/Settings.cpp src/SettingsDialog.cpp \
    src/Adjustments.cpp src/AdjustmentsDialog.cpp src/Editing.cpp src/ParameterStore.cpp src/PasswordDialog.cpp $CORE build/glance.res \
    -o build/Glance.exe $LINK -ld2d1 -ld3d11 -ldxgi -ldxguid -ldwrite -ldwmapi -lcomdlg32 -lcomctl32 -lshell32 -luser32 -lgdi32 $WIC
g++ $FLAGS tools/bench.cpp $CORE -o build/glance-bench.exe $LINK $WIC
g++ $FLAGS tools/make_test_images.cpp -o build/make-test-images.exe $LINK -lwindowscodecs -lole32 -luuid

ls -l build/*.exe
