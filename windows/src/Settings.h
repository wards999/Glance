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

    // Video (needs libmpv-2.dll beside Glance.exe)
    bool includeVideos = true;    // when browsing folders; opening a video always includes its neighbours
    bool videoAutoplay = true;    // start playing when you stop on a video
    bool videoPreviews = true;    // pictures above the timeline as you hover along it
    bool videoLoopShort = true;   // loop silent clips under 30 seconds, like GIFs
    int subtitleMode = 0;         // 0 only forced (foreign dialogue), 1 always
    int subtitleLanguage = 0;     // index into Video::languages(); 0 English

    // Window position, remembered between runs (empty: centred, 80% of the screen)
    RECT windowRect{};
    bool maximized = false;

    static Settings load();
    void save() const;
};
