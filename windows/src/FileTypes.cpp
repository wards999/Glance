#include "FileTypes.h"

#include <windows.h>

#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>

#include <cwctype>
#include <iterator>

namespace FileTypes {
namespace {

constexpr wchar_t kClasses[] = L"Software\\Classes\\";
constexpr wchar_t kCapabilities[] = L"Software\\Glance\\Capabilities";
constexpr wchar_t kRegisteredApplications[] = L"Software\\RegisteredApplications";
constexpr wchar_t kProgIdPrefix[] = L"Glance.";

std::wstring exePath() {
    wchar_t exe[MAX_PATH * 4];
    const DWORD length = GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
    return std::wstring(exe, length);
}

bool setString(const std::wstring& key, const wchar_t* name, const std::wstring& value) {
    return RegSetKeyValueW(HKEY_CURRENT_USER, key.c_str(), name, REG_SZ, value.c_str(),
                           static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}

std::wstring upper(std::wstring text) {
    for (wchar_t& c : text) c = static_cast<wchar_t>(std::towupper(c));
    return text;
}

/// Glance.jpg: its description, icon and how it opens; offered in Open with for .jpg.
bool registerExtension(const std::wstring& extension, const std::wstring& description, int icon) {
    const std::wstring exe = exePath();
    const std::wstring progId = kProgIdPrefix + extension;
    const std::wstring key = kClasses + progId;
    bool ok = setString(key, nullptr, description);
    ok = ok && setString(key + L"\\DefaultIcon", nullptr, L"\"" + exe + L"\",-" + std::to_wstring(icon));
    ok = ok && setString(key + L"\\shell\\open\\command", nullptr, L"\"" + exe + L"\" \"%1\"");
    ok = ok && RegSetKeyValueW(HKEY_CURRENT_USER, (kClasses + (L"." + extension) + L"\\OpenWithProgids").c_str(),
                               progId.c_str(), REG_NONE, nullptr, 0) == ERROR_SUCCESS;
    ok = ok && setString(std::wstring(kCapabilities) + L"\\FileAssociations", (L"." + extension).c_str(), progId);
    return ok;
}

}  // namespace

bool registered() {
    wchar_t value[MAX_PATH * 4] = L"";
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, kRegisteredApplications, L"Glance", RRF_RT_REG_SZ, nullptr, value, &size) !=
        ERROR_SUCCESS) {
        return false;
    }
    size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, kCapabilities, L"ApplicationIcon", RRF_RT_REG_SZ, nullptr, value, &size) !=
        ERROR_SUCCESS) {
        return false;
    }
    return _wcsicmp(value, (L"\"" + exePath() + L"\",0").c_str()) == 0;  // registered from this copy
}

bool registerAll(const std::vector<std::wstring>& pictures, bool pdf, const std::vector<std::wstring>& videos) {
    unregisterAll();  // a fresh start: extensions no longer supported, or an old folder, go
    bool ok = setString(kCapabilities, L"ApplicationName", L"Glance");
    ok = ok && setString(kCapabilities, L"ApplicationDescription", L"Fast picture, PDF and video viewer");
    ok = ok && setString(kCapabilities, L"ApplicationIcon", L"\"" + exePath() + L"\",0");
    for (const std::wstring& extension : pictures) ok = ok && registerExtension(extension, upper(extension) + L" picture", kPictureIcon);
    if (pdf) ok = ok && registerExtension(L"pdf", L"PDF document", kPdfIcon);
    for (const std::wstring& extension : videos) ok = ok && registerExtension(extension, upper(extension) + L" video", kVideoIcon);
    ok = ok && setString(kRegisteredApplications, L"Glance", kCapabilities);
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);  // Explorer redraws its icons
    return ok;
}

void unregisterAll() {
    // Every Glance.<ext> type, and the Open with entries pointing at them.
    HKEY classes = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Classes", 0, KEY_READ, &classes) == ERROR_SUCCESS) {
        std::vector<std::wstring> progIds;
        wchar_t name[256];
        for (DWORD i = 0;; ++i) {
            DWORD length = static_cast<DWORD>(std::size(name));
            if (RegEnumKeyExW(classes, i, name, &length, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
            if (_wcsnicmp(name, kProgIdPrefix, wcslen(kProgIdPrefix)) == 0) progIds.emplace_back(name, length);
        }
        RegCloseKey(classes);
        for (const std::wstring& progId : progIds) {
            const std::wstring extension = progId.substr(wcslen(kProgIdPrefix));
            RegDeleteTreeW(HKEY_CURRENT_USER, (kClasses + progId).c_str());
            HKEY openWith = nullptr;
            if (RegOpenKeyExW(HKEY_CURRENT_USER, (kClasses + (L"." + extension) + L"\\OpenWithProgids").c_str(), 0,
                              KEY_SET_VALUE, &openWith) == ERROR_SUCCESS) {
                RegDeleteValueW(openWith, progId.c_str());
                RegCloseKey(openWith);
            }
        }
    }
    RegDeleteTreeW(HKEY_CURRENT_USER, kCapabilities);
    RegDeleteKeyValueW(HKEY_CURRENT_USER, kRegisteredApplications, L"Glance");
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}

void openDefaultApps() {
    ShellExecuteW(nullptr, L"open", L"ms-settings:defaultapps?registeredAppUser=Glance", nullptr, nullptr, SW_SHOWNORMAL);
}

}  // namespace FileTypes
