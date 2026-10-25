#include "Pipeline.h"

#include <algorithm>

Pipeline::Pipeline(HWND notify, UINT message, size_t costLimit, unsigned threads)
    : notify_(notify), message_(message), costLimit_(costLimit) {
    threads = std::max(threads, 2u);
    // Always keep a thread free for whatever you're about to look at.
    maxBackground_ = std::min(2u, threads - 1);
    for (unsigned i = 0; i < threads; ++i) threads_.emplace_back([this] { work(); });
}

Pipeline::~Pipeline() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    for (auto& thread : threads_) thread.join();
}

std::shared_ptr<Decoded> Pipeline::cached(const DecodeKey& key) {
    std::lock_guard lock(mutex_);
    auto it = cache_.find(key);
    if (it == cache_.end()) return nullptr;
    lru_.splice(lru_.end(), lru_, it->second.position);
    return it->second.image;
}

void Pipeline::load(const DecodeKey& key) {
    {
        std::lock_guard lock(mutex_);
        if (auto it = cache_.find(key); it != cache_.end()) {
            PostMessageW(notify_, message_, 0, reinterpret_cast<LPARAM>(new DecodeResult{key, it->second.image}));
            return;
        }
        Job& job = jobs_[key];
        job.foreground = true;
        // Queued even if a prefetch of it is waiting: whichever worker gets there first does it.
        if (!job.started) foreground_.push_back(key);
    }
    wake_.notify_all();
}

void Pipeline::prefetch(const std::vector<DecodeKey>& keys) {
    {
        std::lock_guard lock(mutex_);
        wanted_ = std::unordered_set<DecodeKey, DecodeKeyHash>(keys.begin(), keys.end());
        for (const auto& key : keys) {
            if (cache_.count(key) || jobs_.count(key)) continue;
            jobs_[key] = Job{};
            background_.push_back(key);
        }
    }
    wake_.notify_all();
}

void Pipeline::drop(const DecodeKey& key) {
    std::lock_guard lock(mutex_);
    if (auto it = jobs_.find(key); it != jobs_.end() && !it->second.started) it->second.foreground = false;
}

void Pipeline::setCostLimit(size_t bytes) {
    std::lock_guard lock(mutex_);
    costLimit_ = bytes;
    evict();
}

void Pipeline::work() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    {
        Decoder decoder;
        for (;;) {
            DecodeKey key;
            bool background = false;
            {
                std::unique_lock lock(mutex_);
                wake_.wait(lock, [this] {
                    return stopping_ || !foreground_.empty() ||
                           (!background_.empty() && runningBackground_ < maxBackground_);
                });
                if (stopping_) break;
                if (!foreground_.empty()) {
                    key = std::move(foreground_.front());
                    foreground_.pop_front();
                } else {
                    key = std::move(background_.front());
                    background_.pop_front();
                    background = true;
                }
                auto it = jobs_.find(key);
                if (it == jobs_.end() || it->second.started) continue;  // done, or another worker has it
                if (!it->second.foreground && !wanted_.count(key)) {
                    jobs_.erase(it);  // stale: you've moved on
                    continue;
                }
                it->second.started = true;
                if (background) ++runningBackground_;
            }

            std::shared_ptr<Decoded> image = decoder.decode(key.path, key.frame, key.request);

            {
                std::lock_guard lock(mutex_);
                if (background) --runningBackground_;
                jobs_.erase(key);
                if (image) insert(key, image);
            }
            wake_.notify_all();
            // Every result goes to the window: what you're waiting for is shown, and decoded-ahead
            // images get uploaded to the GPU while nothing else is happening.
            PostMessageW(notify_, message_, 0, reinterpret_cast<LPARAM>(new DecodeResult{key, image}));
        }
    }
    CoUninitialize();
}

void Pipeline::insert(const DecodeKey& key, std::shared_ptr<Decoded> image) {
    if (auto it = cache_.find(key); it != cache_.end()) {
        cost_ -= it->second.image->bytes();
        lru_.erase(it->second.position);
        cache_.erase(it);
    }
    cost_ += image->bytes();
    lru_.push_back(key);
    cache_[key] = Entry{std::move(image), std::prev(lru_.end())};
    evict();
}

void Pipeline::evict() {
    // Least recently used first, but always keep the newest.
    while (cost_ > costLimit_ && lru_.size() > 1) {
        auto it = cache_.find(lru_.front());
        cost_ -= it->second.image->bytes();
        cache_.erase(it);
        lru_.pop_front();
    }
}
