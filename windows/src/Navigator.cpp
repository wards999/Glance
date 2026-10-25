#include "Navigator.h"

#include "Common.h"

#include <shlwapi.h>

#include <algorithm>

std::wstring Navigator::folderOf(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : path.substr(0, slash);
}

std::wstring Navigator::extensionOf(const std::wstring& path) {
    size_t dot = path.find_last_of(L'.');
    size_t slash = path.find_last_of(L"\\/");
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash)) return L"";
    return lowercase(path.substr(dot + 1));
}

std::vector<std::wstring> Navigator::scan(const std::wstring& folder, const std::unordered_set<std::wstring>& extensions) {
    std::vector<std::wstring> files;
    WIN32_FIND_DATAW data;
    std::wstring pattern = folder + L"\\*";
    HANDLE find = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr,
                                   FIND_FIRST_EX_LARGE_FETCH);
    if (find == INVALID_HANDLE_VALUE) return files;
    do {
        if (data.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_HIDDEN)) continue;
        std::wstring name = data.cFileName;
        if (extensions.count(extensionOf(name))) files.push_back(folder + L"\\" + name);
    } while (FindNextFileW(find, &data));
    FindClose(find);

    std::sort(files.begin(), files.end(), [](const std::wstring& a, const std::wstring& b) {
        return StrCmpLogicalW(a.c_str(), b.c_str()) < 0;
    });
    return files;
}

void Navigator::setFiles(std::vector<std::wstring> files, const std::wstring& current) {
    files_ = std::move(files);
    index_ = 0;
    if (current.empty()) return;
    auto it = std::find_if(files_.begin(), files_.end(), [&](const std::wstring& f) { return samePath(f, current); });
    if (it != files_.end()) {
        index_ = static_cast<size_t>(it - files_.begin());
    } else {
        files_.insert(files_.begin(), current);
    }
}

const std::wstring* Navigator::current() const {
    return index_ < files_.size() ? &files_[index_] : nullptr;
}

bool Navigator::move(int delta) {
    if (files_.size() < 2) return false;
    long long target = static_cast<long long>(index_) + delta;
    long long count = static_cast<long long>(files_.size());
    if (target < 0 || target >= count) {
        if (!wraps) return false;
        target = ((target % count) + count) % count;
    }
    index_ = static_cast<size_t>(target);
    return true;
}

const std::wstring* Navigator::at(int offset) const {
    if (files_.empty()) return nullptr;
    long long target = static_cast<long long>(index_) + offset;
    long long count = static_cast<long long>(files_.size());
    if (target < 0 || target >= count) {
        if (!wraps) return nullptr;
        target = ((target % count) + count) % count;
    }
    return &files_[static_cast<size_t>(target)];
}
