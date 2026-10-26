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
    bool showControls = true;   // button bar when the mouse nears the bottom (JPEGView's navigation panel)
    bool hideCursor = true;     // in full screen, after the mouse stops

    // Performance
    int prefetchCount = 3;
    int cacheMB = 0;  // 0: automatic, a quarter of RAM up to 4 GB

    // Editing
    int saveQuality = 90;         // JPEG quality, percent
    bool keepAdjustments = false; // carry brightness etc. on to the next image (JPEGView's F4)

    // PDF
    int pdfLayout = 0;            // 0 automatic, 1 single page, 2 two pages, 3 two pages with cover
    bool pdfRightToLeft = false;
    bool includePdfs = false;     // when browsing folders; opening a PDF always includes its neighbours
    int holdToZoom = 1;           // 0 off, 1 PDFs only, 2 PDFs and images
    int holdZoomPercent = 250;

    // Window position, remembered between runs (empty: centred, 80% of the screen)
    RECT windowRect{};
    bool maximized = false;

    static Settings load();
    void save() const;
};
