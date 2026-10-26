#include "ParameterStore.h"

#include "Common.h"

#include <shlobj.h>

#include <cstdio>
#include <cwchar>

namespace ParameterStore {

namespace {

constexpr wchar_t kSection[] = L"Parameters";

std::wstring file(bool createFolder) {
    PWSTR appData = nullptr;
    std::wstring path;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData))) {
        path = std::wstring(appData) + L"\\Glance";
        CoTaskMemFree(appData);
        if (createFolder) CreateDirectoryW(path.c_str(), nullptr);
        path += L"\\Parameters.ini";
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
