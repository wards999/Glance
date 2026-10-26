cmake_minimum_required(VERSION 3.20)
project(Glance LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(CORE_SOURCES src/Decoder.cpp src/Pipeline.cpp src/Navigator.cpp src/Pdf.cpp)
set(SYSTEM_LIBS windowscodecs ole32 oleaut32 uuid shlwapi user32 shcore)

# The viewer: a GUI app with no runtime to install.
add_executable(Glance WIN32
    src/main.cpp src/Viewer.cpp src/Settings.cpp src/SettingsDialog.cpp
    src/Adjustments.cpp src/AdjustmentsDialog.cpp src/Editing.cpp src/ParameterStore.cpp src/PasswordDialog.cpp src/Video.cpp
    ${CORE_SOURCES} glance.rc)
target_link_libraries(Glance PRIVATE d2d1 d3d11 dxgi dxguid dwrite dwmapi comdlg32 comctl32 shell32 gdi32 ${SYSTEM_LIBS})

# Console tools: decode timing, and test images for it.
add_executable(glance-bench tools/bench.cpp ${CORE_SOURCES})
target_link_libraries(glance-bench PRIVATE ${SYSTEM_LIBS})
add_executable(make-test-images tools/make_test_images.cpp)
target_link_libraries(make-test-images PRIVATE windowscodecs ole32 oleaut32 uuid)

# Explorer thumbnails for PDFs (first page, via the same pdfium.dll). Registered per user with
# regsvr32; see thumbnails/GlanceThumbnails.cpp.
add_library(GlanceThumbnails SHARED thumbnails/GlanceThumbnails.cpp thumbnails/GlanceThumbnails.def)
target_link_libraries(GlanceThumbnails PRIVATE shlwapi shell32 ole32 gdi32 advapi32 user32 uuid)

foreach(target Glance glance-bench make-test-images GlanceThumbnails)
    target_include_directories(${target} PRIVATE src)
    target_compile_definitions(${target} PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
    if(MINGW)
        target_compile_options(${target} PRIVATE -O2 -march=x86-64-v2 -Wall -Wno-unknown-pragmas)
        # Static runtime: a single exe, no MinGW DLLs to ship.
        target_link_options(${target} PRIVATE -static -static-libgcc -static-libstdc++)
        if(NOT target STREQUAL GlanceThumbnails)
            target_link_options(${target} PRIVATE -municode)  # wWinMain / wmain
        endif()
    elseif(MSVC)
        target_compile_options(${target} PRIVATE /utf-8 /W3 /permissive- /bigobj)
        # Static runtime: no Visual C++ redistributable to install.
        set_property(TARGET ${target} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
endforeach()

# PDF reading loads pdfium.dll from next to Glance.exe at run time; nothing to link here.
if(MSVC)
    # glance.rc already embeds our manifest (per-monitor DPI); don't let the linker add another.
    target_link_options(Glance PRIVATE /MANIFEST:NO)
endif()
