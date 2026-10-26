#include "Settings.h"

#include <shlobj.h>

#include <algorithm>
#include <cwchar>
#include <map>

namespace {

constexpr wchar_t kSection[] = L"Glance";

std::wstring settingsPath(bool createFolder) {
    PWSTR appData = nullptr;
    std::wstring path;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData))) {
        path = std::wstring(appData) + L"\\Glance";
        CoTaskMemFree(appData);
        if (createFolder) CreateDirectoryW(path.c_str(), nullptr);
        path += L"\\Glance.ini";
    }
    return path;
}

}  // namespace

Settings Settings::load() {
    Settings s;
    const std::wstring path = settingsPath(false);
    if (path.empty()) return s;

    // One read of the whole section rather than a file access per key: this runs at launch.
    std::wstring buffer(8192, L'\0');
    DWORD length = GetPrivateProfileSectionW(kSection, buffer.data(), static_cast<DWORD>(buffer.size()), path.c_str());
    std::map<std::wstring, std::wstring> values;
    for (size_t start = 0; start < length;) {
        std::wstring line(buffer.c_str() + start);
        start += line.size() + 1;
        size_t equals = line.find(L'=');
        if (equals != std::wstring::npos) values[line.substr(0, equals)] = line.substr(equals + 1);
    }
    auto number = [&](const wchar_t* key, int fallback) {
        auto it = values.find(key);
        return it == values.end() ? fallback : _wtoi(it->second.c_str());
    };
    auto flag = [&](const wchar_t* key, bool fallback) { return number(key, fallback ? 1 : 0) != 0; };

    s.sortOrder = static_cast<SortOrder>(std::clamp(number(L"SortOrder", 0), 0, 4));
    s.descending = flag(L"Descending", s.descending);
    s.loop = flag(L"Loop", s.loop);
    s.wheelNavigates = flag(L"WheelNavigates", s.wheelNavigates);
    s.escapeCloses = flag(L"EscapeCloses", s.escapeCloses);
    s.slideshowSeconds = std::clamp(number(L"SlideshowSeconds", s.slideshowSeconds), 1, 300);
    s.startFullScreen = flag(L"StartFullScreen", s.startFullScreen);
    s.enlargeSmall = flag(L"EnlargeSmall", s.enlargeSmall);
    s.background = static_cast<COLORREF>(number(L"Background", static_cast<int>(s.background)));
    s.showInfo = flag(L"ShowInfo", s.showInfo);
    s.showFilename = flag(L"ShowFilename", s.showFilename);
    s.pixelatedZoom = flag(L"PixelatedZoom", s.pixelatedZoom);
    s.showControls = flag(L"ShowControls", s.showControls);
    s.hideCursor = flag(L"HideCursor", s.hideCursor);
    s.prefetchCount = std::clamp(number(L"PrefetchCount", s.prefetchCount), 0, 8);
    s.cacheMB = std::clamp(number(L"CacheMB", s.cacheMB), 0, 65536);
    s.saveQuality = std::clamp(number(L"SaveQuality", s.saveQuality), 30, 100);
    s.keepAdjustments = flag(L"KeepAdjustments", s.keepAdjustments);
    s.pdfLayout = std::clamp(number(L"PdfLayout", s.pdfLayout), 0, 3);
    s.pdfRightToLeft = flag(L"PdfRightToLeft", s.pdfRightToLeft);
    s.includePdfs = flag(L"IncludePdfs", s.includePdfs);
    s.includeVideos = flag(L"IncludeVideos", s.includeVideos);
    s.videoAutoplay = flag(L"VideoAutoplay", s.videoAutoplay);
    s.videoPreviews = flag(L"VideoPreviews", s.videoPreviews);
    s.videoLoopShort = flag(L"VideoLoopShort", s.videoLoopShort);
    s.subtitleMode = std::clamp(number(L"SubtitleMode", s.subtitleMode), 0, 1);
    s.subtitleLanguage = std::max(0, number(L"SubtitleLanguage", s.subtitleLanguage));
    s.holdToZoom = std::clamp(number(L"HoldToZoom", s.holdToZoom), 0, 2);
    s.holdZoomPercent = std::clamp(number(L"HoldZoomPercent", s.holdZoomPercent), 150, 400);
    s.windowRect = {number(L"WindowLeft", 0), number(L"WindowTop", 0), number(L"WindowRight", 0), number(L"WindowBottom", 0)};
    s.maximized = flag(L"Maximized", false);
    return s;
}

void Settings::save() const {
    const std::wstring path = settingsPath(true);
    if (path.empty()) return;
    std::wstring section;
    auto put = [&](const wchar_t* key, long long value) {
        section += key;
        section += L'=';
        section += std::to_wstring(value);
        section += L'\0';
    };
    put(L"SortOrder", static_cast<int>(sortOrder));
    put(L"Descending", descending);
    put(L"Loop", loop);
    put(L"WheelNavigates", wheelNavigates);
    put(L"EscapeCloses", escapeCloses);
    put(L"SlideshowSeconds", slideshowSeconds);
    put(L"StartFullScreen", startFullScreen);
    put(L"EnlargeSmall", enlargeSmall);
    put(L"Background", static_cast<long long>(background));
    put(L"ShowInfo", showInfo);
    put(L"ShowFilename", showFilename);
    put(L"PixelatedZoom", pixelatedZoom);
    put(L"ShowControls", showControls);
    put(L"HideCursor", hideCursor);
    put(L"PrefetchCount", prefetchCount);
    put(L"CacheMB", cacheMB);
    put(L"SaveQuality", saveQuality);
    put(L"KeepAdjustments", keepAdjustments);
    put(L"PdfLayout", pdfLayout);
    put(L"PdfRightToLeft", pdfRightToLeft);
    put(L"IncludePdfs", includePdfs);
    put(L"IncludeVideos", includeVideos);
    put(L"VideoAutoplay", videoAutoplay);
    put(L"VideoPreviews", videoPreviews);
    put(L"VideoLoopShort", videoLoopShort);
    put(L"SubtitleMode", subtitleMode);
    put(L"SubtitleLanguage", subtitleLanguage);
    put(L"HoldToZoom", holdToZoom);
    put(L"HoldZoomPercent", holdZoomPercent);
    put(L"WindowLeft", windowRect.left);
    put(L"WindowTop", windowRect.top);
    put(L"WindowRight", windowRect.right);
    put(L"WindowBottom", windowRect.bottom);
    put(L"Maximized", maximized);
    section += L'\0';
    WritePrivateProfileSectionW(kSection, section.c_str(), path.c_str());
}
