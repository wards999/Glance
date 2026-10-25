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

/// With GLANCE_TRACE set, appends "<ms since the process started>  <what>" to
/// %TEMP%\glance-trace.txt: where launch time goes, step by step.
inline void trace(const char* what) {
    static const bool enabled = GetEnvironmentVariableW(L"GLANCE_TRACE", nullptr, 0) > 0;
    if (!enabled) return;
    FILETIME created, exited, kernel, user, now;
    GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
    GetSystemTimePreciseAsFileTime(&now);
    auto ticks = [](const FILETIME& t) { return (ULONGLONG(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
    const double ms = double(ticks(now) - ticks(created)) / 10000.0;
    wchar_t path[MAX_PATH];
    GetTempPathW(MAX_PATH, path);
    wcscat_s(path, L"glance-trace.txt");
    HANDLE file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    char line[160];
    int length = wsprintfA(line, "%d.%d ms  %s\r\n", int(ms), int(ms * 10) % 10, what);
    DWORD written;
    WriteFile(file, line, static_cast<DWORD>(length), &written, nullptr);
    CloseHandle(file);
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
