#include "Navigator.h"

#include "Common.h"

#include <shlwapi.h>

#include <algorithm>
#include <random>

namespace {

unsigned long long ticks(const FILETIME& time) {
    return (static_cast<unsigned long long>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}

}  // namespace

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

std::vector<FileEntry> Navigator::scan(const std::wstring& folder, const std::unordered_set<std::wstring>& extensions) {
    std::vector<FileEntry> files;
    WIN32_FIND_DATAW data;
    std::wstring pattern = folder + L"\\*";
    HANDLE find = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr,
                                   FIND_FIRST_EX_LARGE_FETCH);
    if (find == INVALID_HANDLE_VALUE) return files;
    do {
        if (data.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_HIDDEN)) continue;
        std::wstring name = data.cFileName;
        if (!extensions.count(extensionOf(name))) continue;
        files.push_back(FileEntry{folder + L"\\" + name, ticks(data.ftLastWriteTime), ticks(data.ftCreationTime),
                                  (static_cast<unsigned long long>(data.nFileSizeHigh) << 32) | data.nFileSizeLow});
    } while (FindNextFileW(find, &data));
    FindClose(find);
    return files;
}

FileEntry Navigator::entryFor(const std::wstring& path) {
    FileEntry entry{path};
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
        entry.modified = ticks(data.ftLastWriteTime);
        entry.created = ticks(data.ftCreationTime);
        entry.size = (static_cast<unsigned long long>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    }
    return entry;
}

void Navigator::setFiles(std::vector<FileEntry> files, const std::wstring& current) {
    files_ = std::move(files);
    index_ = 0;
    if (current.empty()) return;
    auto it = std::find_if(files_.begin(), files_.end(), [&](const FileEntry& f) { return samePath(f.path, current); });
    if (it != files_.end()) {
        index_ = static_cast<size_t>(it - files_.begin());
    } else {
        files_.insert(files_.begin(), entryFor(current));
    }
}

void Navigator::sortEntries(std::vector<FileEntry>& files, SortOrder order, bool descending) {
    auto byName = [](const FileEntry& a, const FileEntry& b) { return StrCmpLogicalW(a.path.c_str(), b.path.c_str()) < 0; };
    switch (order) {
    case SortOrder::Name:
        // Explorer's order: "img2" before "img10".
        std::stable_sort(files.begin(), files.end(), byName);
        break;
    case SortOrder::Modified:
        std::stable_sort(files.begin(), files.end(), [](const FileEntry& a, const FileEntry& b) { return a.modified < b.modified; });
        break;
    case SortOrder::Created:
        std::stable_sort(files.begin(), files.end(), [](const FileEntry& a, const FileEntry& b) { return a.created < b.created; });
        break;
    case SortOrder::Size:
        std::stable_sort(files.begin(), files.end(), [](const FileEntry& a, const FileEntry& b) { return a.size < b.size; });
        break;
    case SortOrder::Random: {
        std::mt19937 random(std::random_device{}());
        std::shuffle(files.begin(), files.end(), random);
        break;
    }
    }
    if (descending && order != SortOrder::Random) std::reverse(files.begin(), files.end());
}

void Navigator::sort(SortOrder order, bool descending) {
    const std::wstring selected = current() ? *current() : std::wstring();
    sortEntries(files_, order, descending);
    index_ = 0;
    for (size_t i = 0; i < files_.size(); ++i) {
        if (samePath(files_[i].path, selected)) index_ = i;
    }
}

void Navigator::removeCurrent() {
    if (index_ >= files_.size()) return;
    files_.erase(files_.begin() + static_cast<long long>(index_));
    if (index_ >= files_.size() && !files_.empty()) index_ = files_.size() - 1;
}

const std::wstring* Navigator::current() const {
    return index_ < files_.size() ? &files_[index_].path : nullptr;
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
    return &files_[static_cast<size_t>(target)].path;
}
