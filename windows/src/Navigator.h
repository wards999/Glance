#pragma once

#include "Settings.h"

#include <string>
#include <unordered_set>
#include <vector>

/// A file in the folder, with what sorting needs (all from the directory listing, no extra I/O).
struct FileEntry {
    std::wstring path;
    unsigned long long modified = 0;
    unsigned long long created = 0;
    unsigned long long size = 0;
};

/// The images in the current folder and where we are in it.
class Navigator {
public:
    /// Every supported image in `folder`. Call off the UI thread: big or network folders are slow.
    static std::vector<FileEntry> scan(const std::wstring& folder, const std::unordered_set<std::wstring>& extensions);
    static FileEntry entryFor(const std::wstring& path);
    static void sortEntries(std::vector<FileEntry>& files, SortOrder order, bool descending);

    static std::wstring folderOf(const std::wstring& path);
    static std::wstring extensionOf(const std::wstring& path);

    /// Keeps `current` selected; it's added if the listing missed it.
    void setFiles(std::vector<FileEntry> files, const std::wstring& current);
    /// Re-sorts, staying on the current file.
    void sort(SortOrder order, bool descending);
    void removeCurrent();

    const std::wstring* current() const;
    size_t index() const { return index_; }
    size_t count() const { return files_.size(); }

    bool move(int delta);
    void first() { index_ = 0; }
    void last() { index_ = files_.empty() ? 0 : files_.size() - 1; }

    /// The file `offset` places away, following the wrap setting.
    const std::wstring* at(int offset) const;

    bool wraps = true;

private:
    std::vector<FileEntry> files_;
    size_t index_ = 0;
};
