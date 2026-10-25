#pragma once

#include <windows.h>

#include <string>

enum class SortOrder { Name = 0, Modified, Created, Size, Random };

/// Preferences, edited in the Settings dialog. Stored in %APPDATA%\Glance\Glance.ini, which is
/// only an implementation detail: nobody needs to edit it by hand.
struct Settings {
    // General
    SortOrder sortOrder = SortOrder::Name;
    bool descending = false;
    bool loop = true;
    bool wheelNavigates = true;  // otherwise the wheel zooms (Ctrl+wheel always zooms)
    bool escapeCloses = true;    // otherwise Esc only leaves full screen
    int slideshowSeconds = 3;

    // Display
    bool startFullScreen = false;
    bool enlargeSmall = true;
    COLORREF background = RGB(0, 0, 0);
    bool showInfo = false;
    bool showFilename = false;
    bool pixelatedZoom = true;  // individual pixels above 400%

    // Performance
    int prefetchCount = 3;
    int cacheMB = 0;  // 0: automatic, a quarter of RAM up to 4 GB

    // Window position, remembered between runs (empty: centred, 80% of the screen)
    RECT windowRect{};
    bool maximized = false;

    static Settings load();
    void save() const;
};
