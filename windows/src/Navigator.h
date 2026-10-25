#pragma once

#include <string>
#include <unordered_set>
#include <vector>

/// The images in the current folder and where we are in it.
class Navigator {
public:
    /// Every supported image in `folder`, in Explorer's order ("img2" before "img10").
    /// Call off the UI thread: big or network folders are slow.
    static std::vector<std::wstring> scan(const std::wstring& folder, const std::unordered_set<std::wstring>& extensions);

    static std::wstring folderOf(const std::wstring& path);
    static std::wstring extensionOf(const std::wstring& path);

    /// Keeps `current` selected; it's added if the listing missed it.
    void setFiles(std::vector<std::wstring> files, const std::wstring& current);

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
    std::vector<std::wstring> files_;
    size_t index_ = 0;
};
