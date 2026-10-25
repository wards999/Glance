#pragma once

#include "Decoder.h"

#include <condition_variable>
#include <deque>
#include <list>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct DecodeKey {
    std::wstring path;
    UINT frame = 0;
    DecodeRequest request;
    int layout = -1;  // PDFs: Pdf::packLayout, since `frame` counts screens under it

    bool operator==(const DecodeKey&) const = default;
};

struct DecodeKeyHash {
    size_t operator()(const DecodeKey& key) const {
        size_t h = std::hash<std::wstring>{}(key.path);
        auto mix = [&h](size_t v) { h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2); };
        mix(key.frame);
        mix(key.request.boxWidth);
        mix(key.request.boxHeight);
        mix(key.request.longSide);
        mix(static_cast<size_t>(key.layout + 1));
        return h;
    }
};

/// Delivered to the window with the pipeline's message; the receiver owns it.
struct DecodeResult {
    DecodeKey key;
    std::shared_ptr<Decoded> image;  // null if the file couldn't be decoded
};

/// Decodes on worker threads, keeps recent results in memory and decodes ahead.
/// Requests for the same key are coalesced: landing on an image that's being decoded ahead
/// waits for that decode rather than starting another.
class Pipeline {
public:
    Pipeline(HWND notify, UINT message, size_t costLimit, unsigned threads);
    ~Pipeline();

    std::shared_ptr<Decoded> cached(const DecodeKey& key);

    /// High priority. The result is posted to the window, as is every finished decode.
    void load(const DecodeKey& key);

    /// Replaces the set of images to decode ahead; queued ones no longer wanted are skipped.
    void prefetch(const std::vector<DecodeKey>& keys);

    /// The caller has moved on: a queued decode that hasn't started is skipped.
    void drop(const DecodeKey& key);

    void setCostLimit(size_t bytes);

    /// Forget everything decoded from `path`, e.g. after it changed on disk.
    void invalidate(const std::wstring& path);

private:
    struct Job {
        bool foreground = false;
        bool started = false;
    };

    void work();
    void insert(const DecodeKey& key, std::shared_ptr<Decoded> image);  // mutex held
    void evict();                                                        // mutex held

    HWND notify_;
    UINT message_;

    std::mutex mutex_;
    std::condition_variable wake_;
    bool stopping_ = false;
    std::unordered_map<DecodeKey, Job, DecodeKeyHash> jobs_;
    std::deque<DecodeKey> foreground_;
    std::deque<DecodeKey> background_;
    std::unordered_set<DecodeKey, DecodeKeyHash> wanted_;
    unsigned runningBackground_ = 0;
    unsigned maxBackground_ = 1;

    using Lru = std::list<DecodeKey>;
    struct Entry {
        std::shared_ptr<Decoded> image;
        Lru::iterator position;
    };
    std::unordered_map<DecodeKey, Entry, DecodeKeyHash> cache_;
    Lru lru_;
    size_t cost_ = 0;
    size_t costLimit_;

    std::vector<std::thread> threads_;
};
