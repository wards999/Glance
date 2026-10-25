#pragma once

#include <windows.h>

#include <string>
#include <utility>

/// Minimal owning COM pointer (avoids depending on WRL).
template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ComPtr(T* p) : p_(p) { if (p_) p_->AddRef(); }
    ComPtr(const ComPtr& other) : p_(other.p_) { if (p_) p_->AddRef(); }
    ComPtr(ComPtr&& other) noexcept : p_(std::exchange(other.p_, nullptr)) {}
    ~ComPtr() { reset(); }

    ComPtr& operator=(ComPtr other) noexcept {
        std::swap(p_, other.p_);
        return *this;
    }

    T* get() const { return p_; }
    T* operator->() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }

    /// For out-parameters: releases the current object first.
    T** put() {
        reset();
        return &p_;
    }

    void reset() {
        if (p_) {
            p_->Release();
            p_ = nullptr;
        }
    }

    template <typename U>
    ComPtr<U> as() const {
        ComPtr<U> result;
        if (p_) p_->QueryInterface(__uuidof(U), reinterpret_cast<void**>(result.put()));
        return result;
    }

private:
    T* p_ = nullptr;
};

inline std::wstring lowercase(std::wstring text) {
    if (!text.empty()) CharLowerBuffW(text.data(), static_cast<DWORD>(text.size()));
    return text;
}

inline bool samePath(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

/// Monotonic milliseconds, for timing.
inline double nowMs() {
    static const double frequency = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart);
    }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<double>(t.QuadPart) * 1000.0 / frequency;
}
