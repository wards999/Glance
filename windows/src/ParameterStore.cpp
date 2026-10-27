#include "ParameterStore.h"

#include "Common.h"

#include <shlobj.h>

#include <cstdio>
#include <cwchar>

namespace ParameterStore {

namespace {

constexpr wchar_t kSection[] = L"Parameters";

std::wstring file(bool createFolder, const wchar_t* name = L"Parameters.ini") {
    PWSTR appData = nullptr;
    std::wstring path;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData))) {
        path = std::wstring(appData) + L"\\Glance";
        CoTaskMemFree(appData);
        if (createFolder) CreateDirectoryW(path.c_str(), nullptr);
        path += L"\\";
        path += name;
    }
    return path;
}

/// INI keys can't contain '=' (or start with ';' or '['), which file names can.
std::wstring keyFor(const std::wstring& path) {
    std::wstring key;
    for (wchar_t c : lowercase(path)) {
        if (c == L'=') key += L"%3D";
        else if (c == L'%') key += L"%25";
        else key += c;
    }
    return key;
}

}  // namespace

std::wstring placesFile(bool createFolder) { return file(createFolder, L"Pages.ini"); }
std::wstring placesKey(const std::wstring& path) { return keyFor(path); }

std::optional<Adjustments> load(const std::wstring& path) {
    const std::wstring ini = file(false);
    if (ini.empty()) return std::nullopt;
    wchar_t value[256] = L"";
    if (!GetPrivateProfileStringW(kSection, keyFor(path).c_str(), L"", value, 256, ini.c_str())) return std::nullopt;
    Adjustments a;
    int autoCorrect = 0;
    if (swscanf_s(value, L"%f %f %f %f %f %f %f %f %d", &a.brightness, &a.contrast, &a.saturation, &a.gamma, &a.sharpen,
                  &a.cyanRed, &a.magentaGreen, &a.yellowBlue, &autoCorrect) != 9) {
        return std::nullopt;
    }
    a.autoCorrect = autoCorrect != 0;
    return a;
}

void save(const std::wstring& path, const Adjustments& a) {
    const std::wstring ini = file(true);
    if (ini.empty()) return;
    wchar_t value[256];
    swprintf(value, 256, L"%.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %d", a.brightness, a.contrast, a.saturation, a.gamma,
             a.sharpen, a.cyanRed, a.magentaGreen, a.yellowBlue, a.autoCorrect ? 1 : 0);
    WritePrivateProfileStringW(kSection, keyFor(path).c_str(), value, ini.c_str());
}

void clear(const std::wstring& path) {
    const std::wstring ini = file(false);
    if (!ini.empty()) WritePrivateProfileStringW(kSection, keyFor(path).c_str(), nullptr, ini.c_str());
}

}  // namespace ParameterStore

namespace PdfPlaces {

constexpr wchar_t kSection[] = L"Pages";

std::optional<Place> load(const std::wstring& path) {
    const std::wstring ini = ParameterStore::placesFile(false);
    if (ini.empty()) return std::nullopt;
    wchar_t value[64] = L"";
    if (!GetPrivateProfileStringW(kSection, ParameterStore::placesKey(path).c_str(), L"", value, 64, ini.c_str())) {
        return std::nullopt;
    }
    Place place;
    if (swscanf_s(value, L"%d %d %d", &place.page, &place.screen, &place.layout) != 3 || place.page < 1 || place.screen < 0) {
        return std::nullopt;
    }
    return place;
}

void save(const std::wstring& path, const Place& place) {
    const std::wstring ini = ParameterStore::placesFile(place.page > 1);
    if (ini.empty()) return;
    wchar_t value[64];
    swprintf(value, 64, L"%d %d %d", place.page, place.screen, place.layout);
    WritePrivateProfileStringW(kSection, ParameterStore::placesKey(path).c_str(), place.page > 1 ? value : nullptr, ini.c_str());
}

}  // namespace PdfPlaces
