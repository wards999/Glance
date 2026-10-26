#include "Video.h"

#include "Navigator.h"

#include "../third_party/mpv/client.h"
#include "../third_party/mpv/render.h"

#include <commdlg.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <type_traits>

namespace {

// The libmpv functions Glance uses, looked up at run time so there's nothing to link against and
// the DLL only loads when a video is shown.
struct Api {
    decltype(&mpv_create) create;
    decltype(&mpv_initialize) initialize;
    decltype(&mpv_terminate_destroy) terminateDestroy;
    decltype(&mpv_set_option_string) setOptionString;
    decltype(&mpv_set_property_string) setPropertyString;
    decltype(&mpv_get_property) getProperty;
    decltype(&mpv_get_property_string) getPropertyString;
    decltype(&mpv_free) free;
    decltype(&mpv_command) command;
    decltype(&mpv_wait_event) waitEvent;
    decltype(&mpv_set_wakeup_callback) setWakeupCallback;
    decltype(&mpv_render_context_create) renderCreate;
    decltype(&mpv_render_context_update) renderUpdate;
    decltype(&mpv_render_context_render) renderRender;
    decltype(&mpv_render_context_free) renderFree;
};

std::wstring dllPath() {
    wchar_t exe[MAX_PATH * 4];
    const DWORD length = GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
    return Navigator::folderOf(std::wstring(exe, length)) + L"\\libmpv-2.dll";
}

const Api* api() {
    static Api loaded{};
    static const Api* result = []() -> const Api* {
        HMODULE module = LoadLibraryExW(dllPath().c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        bool ok = module != nullptr;
        auto find = [&](auto& function, const char* name) {
            if (!ok) return;
            function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(
                reinterpret_cast<void*>(GetProcAddress(module, name)));
            ok = function != nullptr;
        };
        find(loaded.create, "mpv_create");
        find(loaded.initialize, "mpv_initialize");
        find(loaded.terminateDestroy, "mpv_terminate_destroy");
        find(loaded.setOptionString, "mpv_set_option_string");
        find(loaded.setPropertyString, "mpv_set_property_string");
        find(loaded.getProperty, "mpv_get_property");
        find(loaded.getPropertyString, "mpv_get_property_string");
        find(loaded.free, "mpv_free");
        find(loaded.command, "mpv_command");
        find(loaded.waitEvent, "mpv_wait_event");
        find(loaded.setWakeupCallback, "mpv_set_wakeup_callback");
        find(loaded.renderCreate, "mpv_render_context_create");
        find(loaded.renderUpdate, "mpv_render_context_update");
        find(loaded.renderRender, "mpv_render_context_render");
        find(loaded.renderFree, "mpv_render_context_free");
        if (!ok) {
            trace("libmpv-2.dll missing or unusable");
            return nullptr;
        }
        trace("libmpv-2.dll loaded");
        return &loaded;
    }();
    return result;
}

std::string utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr);
    return result;
}

void applyOptions(const Api* mpv, mpv_handle* handle, std::initializer_list<std::pair<const char*, const char*>> options) {
    for (const auto& [name, value] : options) {
        if (mpv->setOptionString(handle, name, value) < 0) {
            char line[160];
            snprintf(line, sizeof(line), "mpv option %s=%s not accepted", name, value);
            trace(line);
        }
    }
}

void run(const Api* mpv, mpv_handle* handle, std::initializer_list<const char*> arguments) {
    const char* list[8] = {};
    size_t i = 0;
    for (const char* argument : arguments) {
        if (i + 1 < std::size(list)) list[i++] = argument;
    }
    mpv->command(handle, list);
}

/// Waits up to `timeout` seconds for `wanted`; false on timeout or if the file failed to load.
bool waitFor(const Api* mpv, mpv_handle* handle, mpv_event_id wanted, double timeout) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(timeout);
    while (true) {
        const double left = std::chrono::duration<double>(end - std::chrono::steady_clock::now()).count();
        if (left <= 0) return false;
        const mpv_event* event = mpv->waitEvent(handle, left);
        if (event->event_id == wanted) return true;
        if (event->event_id == MPV_EVENT_END_FILE || event->event_id == MPV_EVENT_SHUTDOWN) return false;
    }
}

std::wstring clock(double seconds) {
    if (!(seconds >= 0)) seconds = 0;
    const long long total = static_cast<long long>(seconds);
    wchar_t text[32];
    if (total >= 3600) {
        swprintf(text, std::size(text), L"%lld:%02lld:%02lld", total / 3600, total / 60 % 60, total % 60);
    } else {
        swprintf(text, std::size(text), L"%lld:%02lld", total / 60, total % 60);
    }
    return text;
}

struct PreviewResult {
    unsigned generation;
    double time;
    UINT width, height;
    std::vector<uint8_t> pixels;
};

constexpr wchar_t kSurfaceClass[] = L"GlanceVideoSurface";
constexpr wchar_t kOverlayClass[] = L"GlanceVideoOverlay";
constexpr int kOverlayHeight = 300;   // DIPs: the bar plus room for a picture above it
constexpr int kPreviewWidth = 220;    // DIPs
constexpr int kPreviewBuckets = 300;  // pictures per video, at most

LRESULT CALLBACK passThroughProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_NCHITTEST:
        return HTTRANSPARENT;  // the mouse belongs to Glance's window underneath
    case WM_ERASEBKGND: {
        RECT client;
        GetClientRect(hwnd, &client);
        FillRect(reinterpret_cast<HDC>(wParam), &client, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        return 1;
    }
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

void registerClasses() {
    static bool done = false;
    if (done) return;
    done = true;
    for (const wchar_t* name : {kSurfaceClass, kOverlayClass}) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = passThroughProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = name;
        RegisterClassExW(&wc);
    }
}

}  // namespace

namespace Video {

bool available() {
    static const bool present = GetFileAttributesW(dllPath().c_str()) != INVALID_FILE_ATTRIBUTES;
    return present;
}

const std::vector<std::wstring>& extensions() {
    static const std::vector<std::wstring> list = {L"mp4", L"m4v", L"mov", L"mkv", L"webm", L"avi", L"wmv", L"flv",
                                                   L"mpg", L"mpeg", L"m2ts", L"mts", L"ts", L"3gp", L"ogv", L"vob",
                                                   L"asf", L"f4v", L"rm", L"rmvb", L"divx"};
    return list;
}

const std::vector<Language>& languages() {
    static const std::vector<Language> list = {
        {L"English", {"en", "eng", "english"}},       {L"French", {"fr", "fre", "fra", "french"}},
        {L"German", {"de", "ger", "deu", "german"}},  {L"Spanish", {"es", "spa", "spanish"}},
        {L"Italian", {"it", "ita", "italian"}},       {L"Dutch", {"nl", "dut", "nld", "dutch"}},
        {L"Portuguese", {"pt", "por", "portuguese"}}, {L"Swedish", {"sv", "swe", "swedish"}},
        {L"Danish", {"da", "dan", "danish"}},         {L"Norwegian", {"no", "nor", "nob", "norwegian"}},
        {L"Finnish", {"fi", "fin", "finnish"}},       {L"Polish", {"pl", "pol", "polish"}},
        {L"Greek", {"el", "gre", "ell", "greek"}},    {L"Russian", {"ru", "rus", "russian"}},
        {L"Japanese", {"ja", "jpn", "japanese"}},     {L"Korean", {"ko", "kor", "korean"}},
        {L"Chinese", {"zh", "chi", "zho", "chinese"}}, {L"Arabic", {"ar", "ara", "arabic"}},
    };
    return list;
}

bool isVideo(const std::wstring& path) {
    const std::wstring extension = Navigator::extensionOf(path);
    for (const auto& known : extensions()) {
        if (samePath(extension, known)) return true;
    }
    return false;
}

}  // namespace Video

VideoPlayer::VideoPlayer(HWND parent, UINT eventMessage, UINT previewMessage)
    : parent_(parent), eventMessage_(eventMessage), previewMessage_(previewMessage) {}

VideoPlayer::~VideoPlayer() {
    if (previewThread_.joinable()) {
        {
            std::lock_guard lock(previewLock_);
            previewQuit_ = true;
        }
        previewWake_.notify_all();
        previewThread_.join();
    }
    if (mpv_) api()->terminateDestroy(static_cast<mpv_handle*>(mpv_));
    if (overlay_) DestroyWindow(overlay_);
    if (surface_) DestroyWindow(surface_);
    if (dc_) {
        SelectObject(dc_, oldBitmap_);
        DeleteDC(dc_);
    }
    if (dib_) DeleteObject(dib_);
}

void VideoPlayer::createWindows() {
    registerClasses();
    RECT client{};
    GetClientRect(parent_, &client);
    surface_ = CreateWindowExW(0, kSurfaceClass, L"", WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN, 0, 0,
                               client.right, client.bottom, parent_, nullptr, GetModuleHandleW(nullptr), nullptr);
    // Layered and transparent to the mouse: drawn with per-pixel alpha over the video.
    overlay_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE, kOverlayClass, L"",
                               WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 1, 1, parent_, nullptr, GetModuleHandleW(nullptr), nullptr);
}

bool VideoPlayer::start() {
    if (mpv_) return true;
    const Api* mpv = api();
    if (!mpv) return false;
    if (!surface_) createWindows();
    mpv_handle* handle = mpv->create();
    if (!handle) return false;
    const std::string wid = std::to_string(reinterpret_cast<intptr_t>(surface_));
    // mpv prepares its GPU shaders on first use; kept on disk, the next start skips that
    // (about 0.15 s off every first video).
    std::string shaderCache;
    if (wchar_t local[MAX_PATH]; GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH)) {
        const std::wstring folder = std::wstring(local) + L"\\Glance\\mpv-cache";
        CreateDirectoryW((std::wstring(local) + L"\\Glance").c_str(), nullptr);
        CreateDirectoryW(folder.c_str(), nullptr);
        shaderCache = utf8(folder);
    }
    applyOptions(mpv, handle,
               {{"wid", wid.c_str()},
                {"vo", "gpu-next"},
                {"hwdec", "auto-safe"},           // the GPU decodes when it can
                {"force-window", "yes"},          // bring up the GPU output now, not at the first file
                {"idle", "yes"},
                {"keep-open", "yes"},             // stay on the last frame at the end
                {"pause", "yes"},
                {"input-default-bindings", "no"}, // Glance handles every key and click
                {"input-vo-keyboard", "no"},
                {"input-cursor-passthrough", "yes"},
                {"cursor-autohide", "no"},
                {"osc", "no"},
                {"osd-level", "0"},
                {"load-scripts", "no"},
                {"ytdl", "no"},
                {"terminal", "no"},
                {"config", "no"},
                {"audio-display", "no"},
                // Subtitle files beside the video load even when named "Movie.en.srt" and the
                // like, or kept in a Subs folder. Which one shows is Glance's choice (chooseSubtitles).
                {"sub-auto", "fuzzy"},
                {"sub-file-paths", "subs;Subs;subtitles;Subtitles"}});
    if (!shaderCache.empty()) applyOptions(mpv, handle, {{"gpu-shader-cache-dir", shaderCache.c_str()}});
    if (mpv->initialize(handle) < 0) {
        mpv->terminateDestroy(handle);
        return false;
    }
    mpv_ = handle;
    mpv->setWakeupCallback(
        handle,
        [](void* context) {
            auto* self = static_cast<VideoPlayer*>(context);
            if (!self->wakePending_.exchange(true)) PostMessageW(self->parent_, self->eventMessage_, 0, 0);
        },
        this);
    previewThread_ = std::thread([this] { previewWorker(); });
    trace("video player started");
    return true;
}

bool VideoPlayer::warm() { return start(); }

bool VideoPlayer::open(const std::wstring& path, bool play) {
    if (!start()) return false;
    const Api* mpv = api();
    auto* handle = static_cast<mpv_handle*>(mpv_);
    open_ = true;
    ended_ = false;
    path_ = path;
    duration_ = 0;
    videoWidth_ = videoHeight_ = 0;
    previews_.clear();
    ++generation_;
    mpv->setPropertyString(handle, "pause", play ? "no" : "yes");
    mpv->setPropertyString(handle, "loop-file", "no");
    const std::string file = utf8(path);
    run(mpv, handle, {"loadfile", file.c_str(), "replace"});
    {
        std::lock_guard lock(previewLock_);
        previewPath_ = options_.previews ? path : std::wstring();
        previewTime_ = -1;
        previewGeneration_ = generation_;
    }
    previewWake_.notify_all();
    layout();
    ShowWindow(surface_, SW_SHOWNA);
    SetWindowPos(overlay_, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    drawControls();
    return true;
}

void VideoPlayer::play() {
    if (open_) api()->setPropertyString(static_cast<mpv_handle*>(mpv_), "pause", "no");
}

void VideoPlayer::close() {
    if (!open_) return;
    open_ = false;
    scrubbing_ = false;
    hovering_ = false;
    run(api(), static_cast<mpv_handle*>(mpv_), {"stop"});
    ShowWindow(surface_, SW_HIDE);
    ShowWindow(overlay_, SW_HIDE);
    controlsVisible_ = false;
    previews_.clear();
    ++generation_;
    {
        std::lock_guard lock(previewLock_);
        previewPath_.clear();  // the preview mpv lets go of the file too
        previewTime_ = -1;
        previewGeneration_ = generation_;
    }
    previewWake_.notify_all();
}

double VideoPlayer::property(const char* name, double fallback) const {
    if (!mpv_) return fallback;
    double value = fallback;
    if (api()->getProperty(static_cast<mpv_handle*>(mpv_), name, MPV_FORMAT_DOUBLE, &value) < 0) return fallback;
    return value;
}

std::string VideoPlayer::stringProperty(const char* name) const {
    if (!mpv_) return {};
    char* value = api()->getPropertyString(static_cast<mpv_handle*>(mpv_), name);
    if (!value) return {};
    std::string result(value);
    api()->free(value);
    return result;
}

void VideoPlayer::command(std::initializer_list<const char*> arguments) {
    if (mpv_) run(api(), static_cast<mpv_handle*>(mpv_), arguments);
}

bool VideoPlayer::handleEvents() {
    wakePending_ = false;
    if (!mpv_) return false;
    const Api* mpv = api();
    bool titleChanged = false;
    while (true) {
        const mpv_event* event = mpv->waitEvent(static_cast<mpv_handle*>(mpv_), 0);
        if (event->event_id == MPV_EVENT_NONE) break;
        if (!open_) continue;
        switch (event->event_id) {
        case MPV_EVENT_FILE_LOADED: {
            duration_ = property("duration");
            videoWidth_ = static_cast<int>(property("width"));
            videoHeight_ = static_cast<int>(property("height"));
            // Short silent clips (phone "live" videos and the like) loop, like GIFs.
            const bool silent = stringProperty("current-tracks/audio/id").empty();
            if (options_.loopShort && silent && duration_ > 0 && duration_ < 30) {
                mpv->setPropertyString(static_cast<mpv_handle*>(mpv_), "loop-file", "inf");
            }
            // Warm the picture decoder, so the first hover over the timeline is quick too.
            if (options_.previews && duration_ > 0) requestPreview(duration_ / 2);
            chooseSubtitles();
            placeSubtitles();
            titleChanged = true;
            drawControls();
            break;
        }
        case MPV_EVENT_PLAYBACK_RESTART:
            drawControls();
            break;
        default:
            break;
        }
    }
    return titleChanged;
}

void VideoPlayer::tick() {
    if (!open_) return;
    int eof = 0;
    if (api()->getProperty(static_cast<mpv_handle*>(mpv_), "eof-reached", MPV_FORMAT_FLAG, &eof) >= 0) ended_ = eof != 0;
    if (!toast_.empty() && GetTickCount64() >= toastUntil_) {
        toast_.clear();
        drawControls();
    } else if (controlsVisible_) {
        drawControls();
    }
}

bool VideoPlayer::paused() const {
    if (!mpv_) return true;
    int value = 1;
    api()->getProperty(static_cast<mpv_handle*>(mpv_), "pause", MPV_FORMAT_FLAG, &value);
    return value != 0;
}

void VideoPlayer::togglePause() {
    if (!open_) return;
    if (ended_ || (duration_ > 0 && property("time-pos") >= duration_ - 0.05)) {
        command({"seek", "0", "absolute"});  // at the end: play again from the start
        ended_ = false;
        play();
    } else {
        command({"cycle", "pause"});
    }
    drawControls();
}

void VideoPlayer::seekBy(double seconds) {
    if (!open_) return;
    char amount[32];
    snprintf(amount, sizeof(amount), "%.3f", seconds);
    command({"seek", amount, "relative"});
    drawControls();
}

void VideoPlayer::frameStep(bool forward) {
    if (!open_) return;
    command({forward ? "frame-step" : "frame-back-step"});
    drawControls();
}

void VideoPlayer::changeVolume(int delta) {
    if (!open_) return;
    char amount[16];
    snprintf(amount, sizeof(amount), "%d", delta);
    command({"add", "volume", amount});
    command({"set", "mute", "no"});
    drawControls();
}

void VideoPlayer::toggleMute() {
    if (!open_) return;
    command({"cycle", "mute"});
    drawControls();
}

std::wstring VideoPlayer::describe() const {
    if (!open_ || videoWidth_ <= 0) return {};
    return std::to_wstring(videoWidth_) + L" x " + std::to_wstring(videoHeight_) + L"  -  " + clock(duration_);
}

// MARK: - Controls

void VideoPlayer::layout() {
    if (!surface_) return;
    RECT client{};
    GetClientRect(parent_, &client);
    MoveWindow(surface_, 0, 0, client.right, client.bottom, TRUE);
    const int height = std::min<int>(client.bottom, MulDiv(kOverlayHeight, GetDpiForWindow(parent_), 96));
    SetWindowPos(overlay_, HWND_TOP, 0, client.bottom - height, client.right, height, SWP_NOACTIVATE);
    if (controlsVisible_) drawControls();
}

void VideoPlayer::setControlsVisible(bool visible) {
    if (!open_) visible = false;
    if (visible == controlsVisible_) return;
    controlsVisible_ = visible;
    if (!visible) hovering_ = false;
    placeSubtitles();
    drawControls();
}

bool VideoPlayer::overControls(POINT point) const {
    if (!controlsVisible_) return false;
    RECT area = barRect_;
    InflateRect(&area, 0, MulDiv(8, GetDpiForWindow(parent_), 96));
    return PtInRect(&area, point) != FALSE;
}

double VideoPlayer::timeAt(int x) const {
    const int width = trackRect_.right - trackRect_.left;
    if (width <= 0 || duration_ <= 0) return 0;
    return std::clamp(double(x - trackRect_.left) / width, 0.0, 1.0) * duration_;
}

void VideoPlayer::mouseMove(POINT point) {
    if (!open_) return;
    RECT hit = trackRect_;
    hit.top = barRect_.top;
    hit.bottom = barRect_.bottom;
    const bool over = controlsVisible_ && PtInRect(&hit, point);
    if (scrubbing_) {
        char at[32];
        snprintf(at, sizeof(at), "%.3f", timeAt(point.x));
        command({"seek", at, "absolute+keyframes"});  // keyframes while dragging: instant
    }
    if (over || scrubbing_) requestPreview(timeAt(point.x));
    if (over != hovering_ || over || scrubbing_) {
        hovering_ = over;
        hover_ = point;
        drawControls();
    }
}

bool VideoPlayer::mouseDown(POINT point) {
    if (!open_ || !controlsVisible_) return false;
    RECT hit = trackRect_;
    hit.top = barRect_.top;
    hit.bottom = barRect_.bottom;
    if (PtInRect(&playRect_, point)) {
        togglePause();
    } else if (PtInRect(&muteRect_, point)) {
        toggleMute();
    } else if (PtInRect(&ccRect_, point)) {
        showTrackMenu();
    } else if (PtInRect(&hit, point)) {
        scrubbing_ = true;
        hover_ = point;
        mouseMove(point);
    } else if (!PtInRect(&barRect_, point)) {
        return false;
    }
    return true;
}

void VideoPlayer::mouseUp(POINT point) {
    if (!scrubbing_) return;
    scrubbing_ = false;
    char at[32];
    snprintf(at, sizeof(at), "%.3f", timeAt(point.x));
    command({"seek", at, "absolute+exact"});  // land on the exact frame when you let go
    ended_ = false;
    drawControls();
}

void VideoPlayer::mouseLeave() {
    if (!hovering_) return;
    hovering_ = false;
    drawControls();
}

int VideoPlayer::bucketFor(double time) const {
    const double length = std::max(1.0, duration_ / kPreviewBuckets);
    return static_cast<int>(time / length);
}

void VideoPlayer::requestPreview(double time) {
    if (!options_.previews || duration_ <= 0) return;
    const int bucket = bucketFor(time);
    if (previews_.count(bucket)) return;
    const double length = std::max(1.0, duration_ / kPreviewBuckets);
    {
        std::lock_guard lock(previewLock_);
        previewPath_ = path_;
        previewTime_ = std::min(duration_, (bucket + 0.5) * length);
        previewGeneration_ = generation_;
        previewWidth_ = MulDiv(kPreviewWidth, GetDpiForWindow(parent_), 96);
    }
    previewWake_.notify_all();
}

void VideoPlayer::takePreview(LPARAM lParam) {
    std::unique_ptr<PreviewResult> result(reinterpret_cast<PreviewResult*>(lParam));
    if (!open_ || result->generation != generation_) return;  // a video you've left
    Preview& preview = previews_[bucketFor(result->time)];
    preview.width = result->width;
    preview.height = result->height;
    preview.pixels = std::move(result->pixels);
    if (hovering_ || scrubbing_) drawControls();
}

/// The second, silent mpv: no audio, software decoding of the nearest keyframe only, drawn small
/// by mpv's software renderer. It runs below normal priority so playback never waits for it.
void VideoPlayer::previewWorker() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    const Api* mpv = api();
    if (!mpv) return;
    mpv_handle* handle = mpv->create();
    if (!handle) return;
    applyOptions(mpv, handle,
               {{"vo", "libmpv"}, {"aid", "no"}, {"sid", "no"}, {"hr-seek", "no"}, {"pause", "yes"},
                {"hwdec", "no"}, {"idle", "yes"}, {"keep-open", "yes"}, {"vd-lavc-skiploopfilter", "all"},
                {"vd-lavc-fast", "yes"}, {"cache", "no"}, {"demuxer-max-bytes", "8MiB"}, {"load-scripts", "no"},
                {"ytdl", "no"}, {"terminal", "no"}, {"config", "no"}});
    mpv_render_context* context = nullptr;
    if (mpv->initialize(handle) < 0) {
        mpv->terminateDestroy(handle);
        return;
    }
    mpv_render_param create[] = {{MPV_RENDER_PARAM_API_TYPE, const_cast<char*>(MPV_RENDER_API_TYPE_SW)},
                                 {MPV_RENDER_PARAM_INVALID, nullptr}};
    if (mpv->renderCreate(&context, handle, create) < 0) {
        mpv->terminateDestroy(handle);
        return;
    }
    std::vector<uint8_t> scratch(4 * 16 * 16);
    auto renderInto = [&](std::vector<uint8_t>& pixels, int width, int height) {
        int size[2] = {width, height};
        size_t stride = size_t(width) * 4;
        pixels.resize(stride * height);
        mpv_render_param params[] = {{MPV_RENDER_PARAM_SW_SIZE, size},
                                     {MPV_RENDER_PARAM_SW_FORMAT, const_cast<char*>("bgr0")},
                                     {MPV_RENDER_PARAM_SW_STRIDE, &stride},
                                     {MPV_RENDER_PARAM_SW_POINTER, pixels.data()},
                                     {MPV_RENDER_PARAM_INVALID, nullptr}};
        mpv->renderRender(context, params);
    };

    std::wstring loaded;
    while (true) {
        std::wstring path;
        double time;
        unsigned generation;
        UINT width;
        {
            std::unique_lock lock(previewLock_);
            previewWake_.wait(lock, [&] { return previewQuit_ || previewTime_ >= 0 || previewPath_ != loaded; });
            if (previewQuit_) break;
            path = previewPath_;
            time = previewTime_;
            generation = previewGeneration_;
            width = std::max<UINT>(previewWidth_, 32);
            previewTime_ = -1;
        }
        if (path.empty()) {
            if (!loaded.empty()) run(mpv, handle, {"stop"});
            loaded.clear();
            continue;
        }
        if (path != loaded) {
            const std::string file = utf8(path);
            run(mpv, handle, {"loadfile", file.c_str(), "replace"});
            loaded = path;
            if (!waitFor(mpv, handle, MPV_EVENT_FILE_LOADED, 10)) {
                loaded.clear();  // can't decode it; don't keep trying
                std::lock_guard lock(previewLock_);
                if (previewPath_ == path) previewPath_.clear();
                continue;
            }
        }
        if (time < 0) continue;
        // A frame mpv drew before this seek mustn't be taken for the new one.
        if (mpv->renderUpdate(context) & MPV_RENDER_UPDATE_FRAME) renderInto(scratch, 16, 16);
        char at[32];
        snprintf(at, sizeof(at), "%.3f", time);
        run(mpv, handle, {"seek", at, "absolute+keyframes"});
        waitFor(mpv, handle, MPV_EVENT_PLAYBACK_RESTART, 3);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        while (!(mpv->renderUpdate(context) & MPV_RENDER_UPDATE_FRAME) && std::chrono::steady_clock::now() < deadline) {
            Sleep(2);
        }
        double w = 16, h = 9;
        mpv->getProperty(handle, "dwidth", MPV_FORMAT_DOUBLE, &w);
        mpv->getProperty(handle, "dheight", MPV_FORMAT_DOUBLE, &h);
        const int height = std::max(2, static_cast<int>(std::lround(width * (h > 0 && w > 0 ? h / w : 9.0 / 16))));
        auto result = std::make_unique<PreviewResult>();
        result->generation = generation;
        result->time = time;
        result->width = width;
        result->height = static_cast<UINT>(height);
        renderInto(result->pixels, static_cast<int>(width), height);
        for (size_t i = 3; i < result->pixels.size(); i += 4) result->pixels[i] = 255;  // bgr0 -> opaque BGRA
        PostMessageW(parent_, previewMessage_, 0, reinterpret_cast<LPARAM>(result.release()));
    }
    mpv->renderFree(context);
    mpv->terminateDestroy(handle);
}

void VideoPlayer::drawControls() {
    if (!overlay_) return;
    if ((!controlsVisible_ && toast_.empty()) || !open_) {
        ShowWindow(overlay_, SW_HIDE);
        return;
    }
    RECT client{};
    GetClientRect(parent_, &client);
    const UINT dpi = GetDpiForWindow(parent_);
    const float s = dpi / 96.0f;
    const int width = client.right;
    const int height = std::min<int>(client.bottom, MulDiv(kOverlayHeight, dpi, 96));
    const int top = client.bottom - height;  // overlay's top in client coordinates
    if (width <= 0 || height <= 0) return;

    if (!d2d_) {
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), nullptr,
                          reinterpret_cast<void**>(d2d_.put()));
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dwrite_.put()));
        if (!d2d_ || !dwrite_) return;
        const D2D1_RENDER_TARGET_PROPERTIES properties = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        d2d_->CreateDCRenderTarget(&properties, target_.put());
        if (!target_) return;
    }
    if (formatDpi_ != dpi) {
        textFormat_.reset();
        iconFormat_.reset();
        dwrite_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                  DWRITE_FONT_STRETCH_NORMAL, 13 * s, L"", textFormat_.put());
        ComPtr<IDWriteFontCollection> fonts;
        dwrite_->GetSystemFontCollection(fonts.put(), FALSE);
        UINT32 index = 0;
        BOOL exists = FALSE;
        if (fonts) fonts->FindFamilyName(L"Segoe Fluent Icons", &index, &exists);
        dwrite_->CreateTextFormat(exists ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                  DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 16 * s, L"", iconFormat_.put());
        for (IDWriteTextFormat* format : {textFormat_.get(), iconFormat_.get()}) {
            if (!format) return;
            format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        formatDpi_ = dpi;
    }
    if (width != dibWidth_ || height != dibHeight_) {
        if (dc_) {
            SelectObject(dc_, oldBitmap_);
            DeleteDC(dc_);
        }
        if (dib_) DeleteObject(dib_);
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        dc_ = CreateCompatibleDC(nullptr);
        dib_ = CreateDIBSection(dc_, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        oldBitmap_ = SelectObject(dc_, dib_);
        dibWidth_ = width;
        dibHeight_ = height;
    }

    // Layout, in client coordinates.
    const int margin = static_cast<int>(16 * s), barHeight = static_cast<int>(44 * s), button = static_cast<int>(36 * s);
    const int timeWidth = static_cast<int>((duration_ >= 3600 ? 64 : 48) * s);
    barRect_ = RECT{margin, client.bottom - margin - barHeight, width - margin, client.bottom - margin};
    const int middle = (barRect_.top + barRect_.bottom) / 2;
    playRect_ = RECT{barRect_.left + static_cast<int>(4 * s), middle - button / 2, barRect_.left + static_cast<int>(4 * s) + button,
                     middle + button / 2};
    muteRect_ = RECT{barRect_.right - static_cast<int>(4 * s) - button, middle - button / 2, barRect_.right - static_cast<int>(4 * s),
                     middle + button / 2};
    const RECT nowRect{playRect_.right, barRect_.top, playRect_.right + timeWidth, barRect_.bottom};
    ccRect_ = RECT{muteRect_.left - button, muteRect_.top, muteRect_.left, muteRect_.bottom};
    const RECT totalRect{ccRect_.left - timeWidth, barRect_.top, ccRect_.left, barRect_.bottom};
    trackRect_ = RECT{nowRect.right + static_cast<int>(8 * s), middle - static_cast<int>(2 * s), totalRect.left - static_cast<int>(8 * s),
                      middle + static_cast<int>(2 * s)};

    const double position = std::clamp(property("time-pos"), 0.0, std::max(duration_, 0.0));
    const double shownTime = scrubbing_ ? timeAt(hover_.x) : position;
    int muted = 0;
    api()->getProperty(static_cast<mpv_handle*>(mpv_), "mute", MPV_FORMAT_FLAG, &muted);
    const bool isPaused = paused();

    auto local = [&](const RECT& r) {
        return D2D1::RectF(float(r.left), float(r.top - top), float(r.right), float(r.bottom - top));
    };
    const RECT whole{0, 0, width, height};
    target_->BindDC(dc_, &whole);
    target_->BeginDraw();
    target_->Clear(D2D1::ColorF(0, 0, 0, 0));
    ComPtr<ID2D1SolidColorBrush> panel, white, dim, accent;
    target_->CreateSolidColorBrush(D2D1::ColorF(0.07f, 0.07f, 0.08f, 0.78f), panel.put());
    target_->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), white.put());
    target_->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.3f), dim.put());
    target_->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.12f), accent.put());

    if (controlsVisible_) {
    target_->FillRoundedRectangle(D2D1::RoundedRect(local(barRect_), 10 * s, 10 * s), panel.get());
    // The file's name, just above the bar on the left (the title bar is gone in full screen).
    if (!(hovering_ || scrubbing_)) {
        std::wstring name = path_.substr(path_.find_last_of(L"\\/") + 1);
        const size_t dot = name.find_last_of(L'.');
        if (dot != std::wstring::npos && dot > 0) name.resize(dot);
        const float maxWidth = float(barRect_.right - barRect_.left) * 0.6f;
        ComPtr<IDWriteTextLayout> layout;
        dwrite_->CreateTextLayout(name.c_str(), static_cast<UINT32>(name.size()), textFormat_.get(), maxWidth, 30 * s,
                                  layout.put());
        if (layout) {
            layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            ComPtr<IDWriteInlineObject> ellipsis;
            dwrite_->CreateEllipsisTrimmingSign(textFormat_.get(), ellipsis.put());
            layout->SetTrimming(&trimming, ellipsis.get());
            DWRITE_TEXT_METRICS metrics{};
            layout->GetMetrics(&metrics);
            const float textWidth = std::min(metrics.width, maxWidth);
            layout->SetMaxWidth(textWidth + 1);
            const float padX = 12 * s, boxHeight = 30 * s;
            const float left = float(barRect_.left), bottom = float(barRect_.top - top) - 8 * s;
            const D2D1_RECT_F box{left, bottom - boxHeight, left + textWidth + 2 * padX, bottom};
            target_->FillRoundedRectangle(D2D1::RoundedRect(box, 8 * s, 8 * s), panel.get());
            target_->DrawTextLayout(D2D1::Point2F(left + padX, box.top), layout.get(), white.get());
        }
    }
    if (hovering_ && PtInRect(&playRect_, hover_)) target_->FillRoundedRectangle(D2D1::RoundedRect(local(playRect_), 6 * s, 6 * s), accent.get());
    const wchar_t* playGlyph = isPaused || ended_ ? L"\xE768" : L"\xE769";
    target_->DrawText(playGlyph, 1, iconFormat_.get(), local(playRect_), white.get());
    const wchar_t* muteGlyph = muted ? L"\xE74F" : L"\xE767";
    target_->DrawText(muteGlyph, 1, iconFormat_.get(), local(muteRect_), white.get());
    for (const RECT* button : {&muteRect_, &ccRect_}) {
        if (hovering_ && PtInRect(button, hover_)) target_->FillRoundedRectangle(D2D1::RoundedRect(local(*button), 6 * s, 6 * s), accent.get());
    }
    target_->DrawText(L"\xE7F0", 1, iconFormat_.get(), local(ccRect_), white.get());  // subtitles and audio
    const std::wstring now = clock(shownTime), total = clock(duration_);
    target_->DrawText(now.c_str(), static_cast<UINT32>(now.size()), textFormat_.get(), local(nowRect), white.get());
    ComPtr<ID2D1SolidColorBrush> soft;
    target_->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.75f), soft.put());
    target_->DrawText(total.c_str(), static_cast<UINT32>(total.size()), textFormat_.get(), local(totalRect), soft.get());

    // Timeline: thicker under the pointer, played part solid, a knob at the playhead.
    D2D1_RECT_F track = local(trackRect_);
    if (hovering_ || scrubbing_) {
        track.top -= 1.5f * s;
        track.bottom += 1.5f * s;
    }
    const float radius = (track.bottom - track.top) / 2;
    target_->FillRoundedRectangle(D2D1::RoundedRect(track, radius, radius), dim.get());
    if (duration_ > 0) {
        D2D1_RECT_F played = track;
        played.right = track.left + float((track.right - track.left) * (shownTime / duration_));
        target_->FillRoundedRectangle(D2D1::RoundedRect(played, radius, radius), white.get());
        target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(played.right, (track.top + track.bottom) / 2), 6 * s, 6 * s), white.get());
    }

    // The picture above the timeline: the one for this moment, or the nearest made so far.
    if ((hovering_ || scrubbing_) && duration_ > 0) {
        const double at = timeAt(hover_.x);
        const Preview* picture = nullptr;
        if (options_.previews && !previews_.empty()) {
            const int bucket = bucketFor(at);
            auto it = previews_.lower_bound(bucket);
            if (it == previews_.end() || (it != previews_.begin() && std::abs(std::prev(it)->first - bucket) < std::abs(it->first - bucket))) {
                --it;
            }
            if (std::abs(it->first - bucket) <= 3) picture = &it->second;  // don't show a far-away one
        }
        const float boxWidth = picture ? float(picture->width) : 70 * s;
        const float boxHeight = picture ? float(picture->height) : 0;
        const float labelHeight = 22 * s;
        const float x = std::clamp(float(hover_.x) - boxWidth / 2, float(barRect_.left), float(barRect_.right) - boxWidth);
        const float bottom = float(barRect_.top - top) - 10 * s;
        const D2D1_RECT_F frame{x - 3 * s, bottom - boxHeight - labelHeight - 3 * s, x + boxWidth + 3 * s, bottom};
        target_->FillRoundedRectangle(D2D1::RoundedRect(frame, 6 * s, 6 * s), panel.get());
        if (picture) {
            ComPtr<ID2D1Bitmap> bitmap;
            const D2D1_BITMAP_PROPERTIES properties =
                D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            target_->CreateBitmap(D2D1::SizeU(picture->width, picture->height), picture->pixels.data(), picture->width * 4,
                                  &properties, bitmap.put());
            if (bitmap) {
                const D2D1_RECT_F place{x, frame.top + 3 * s, x + boxWidth, frame.top + 3 * s + boxHeight};
                target_->DrawBitmap(bitmap.get(), place);
            }
        }
        const std::wstring label = clock(at);
        const D2D1_RECT_F labelRect{frame.left, frame.bottom - labelHeight, frame.right, frame.bottom};
        target_->DrawText(label.c_str(), static_cast<UINT32>(label.size()), textFormat_.get(), labelRect, white.get());
    }
    }  // controls

    // A moment's message ("Subtitles: English") above the bar.
    if (!toast_.empty()) {
        const float boxWidth = std::min(float(width) - 2 * margin, 420 * s), boxHeight = 34 * s;
        const float x = (width - boxWidth) / 2, bottomY = float(barRect_.top - top) - 18 * s - (hovering_ ? 170 * s : 0);
        const D2D1_RECT_F box{x, bottomY - boxHeight, x + boxWidth, bottomY};
        target_->FillRoundedRectangle(D2D1::RoundedRect(box, 8 * s, 8 * s), panel.get());
        target_->DrawText(toast_.c_str(), static_cast<UINT32>(toast_.size()), textFormat_.get(), box, white.get());
    }
    target_->EndDraw();

    POINT source{0, 0};
    SIZE size{width, height};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(overlay_, nullptr, nullptr, &size, dc_, &source, 0, &blend, ULW_ALPHA);
    if (!IsWindowVisible(overlay_)) {
        SetWindowPos(overlay_, HWND_TOP, 0, top, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
}

// MARK: - Subtitles and audio tracks

namespace {

std::wstring wide(const std::string& text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size);
    return result;
}

std::string lower(std::string text) {
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

bool saysHearingImpaired(const std::string& title) {
    const std::string text = lower(title);
    return text.find("sdh") != std::string::npos || text.find("hearing") != std::string::npos ||
           text.find("(cc)") != std::string::npos || text == "cc";
}

constexpr int kMenuOff = 0;
constexpr int kMenuFirstSubtitle = 1;
constexpr int kMenuLoadFile = 250;
constexpr int kMenuFirstAudio = 300;
constexpr int kMenuEnd = 500;

}  // namespace

std::vector<VideoPlayer::Track> VideoPlayer::tracks() const {
    std::vector<Track> list;
    if (!open_) return list;
    const int count = std::atoi(stringProperty("track-list/count").c_str());
    for (int i = 0; i < count; ++i) {
        auto field = [&](const char* name) {
            return stringProperty(("track-list/" + std::to_string(i) + "/" + name).c_str());
        };
        const std::string type = field("type");
        if (type != "sub" && type != "audio") continue;
        Track track;
        track.subtitle = type == "sub";
        track.id = std::atoi(field("id").c_str());
        track.lang = lower(field("lang"));
        track.title = field("title");
        track.codec = field("codec");
        track.file = field("external-filename");
        track.external = field("external") == "yes";
        track.forced = field("forced") == "yes";
        track.isDefault = field("default") == "yes";
        track.hearingImpaired = field("hearing-impaired") == "yes" || saysHearingImpaired(track.title);
        track.selected = field("selected") == "yes";
        list.push_back(std::move(track));
    }
    return list;
}

bool VideoPlayer::matchesLanguage(const Track& track) const {
    if (track.lang.empty()) return false;
    const auto& list = Video::languages();
    const auto& codes = list[std::clamp<size_t>(options_.subtitleLanguage, 0, list.size() - 1)].codes;
    return std::find(codes.begin(), codes.end(), track.lang) != codes.end();
}

/// "English - British | SDH", "French (forced)", or the file name of an .srt.
std::wstring VideoPlayer::label(const Track& track) const {
    std::wstring name;
    for (const auto& language : Video::languages()) {
        if (std::find(language.codes.begin(), language.codes.end(), track.lang) != language.codes.end()) name = language.name;
    }
    if (name.empty()) name = wide(track.lang);
    std::wstring detail = wide(track.title);
    if (track.external && !track.file.empty()) {
        detail = wide(track.file);
        const size_t slash = detail.find_last_of(L"\\/");
        if (slash != std::wstring::npos) detail = detail.substr(slash + 1);
    }
    std::wstring text = name;
    if (!detail.empty() && lowercase(detail) != lowercase(name)) text += (text.empty() ? L"" : L" - ") + detail;
    if (text.empty()) text = (track.subtitle ? L"Subtitles " : L"Audio ") + std::to_wstring(track.id);
    if (track.forced && lower(track.title).find("forced") == std::string::npos) text += L" (forced)";
    if (track.hearingImpaired && !saysHearingImpaired(track.title)) text += L" (SDH)";
    return text;
}

void VideoPlayer::chooseSubtitles() {
    std::vector<Track> subtitles;
    for (auto& track : tracks()) {
        if (track.subtitle) subtitles.push_back(track);
    }
    auto first = [&](auto wanted) -> const Track* {
        for (const auto& track : subtitles) {
            if (wanted(track)) return &track;
        }
        return nullptr;
    };
    auto mine = [&](const Track& track) { return matchesLanguage(track); };
    // A subtitle file put beside the film is there to be read, unless it is in another language.
    const Track* pick = first([&](const Track& t) { return t.external && (t.lang.empty() || mine(t)); });
    if (!pick && options_.alwaysSubtitles) {
        pick = first([&](const Track& t) { return mine(t) && !t.forced && !t.hearingImpaired; });
        if (!pick) pick = first([&](const Track& t) { return mine(t) && !t.forced; });
    }
    // Forced subtitles translate dialogue in other languages; they belong on either way.
    if (!pick) pick = first([&](const Track& t) { return t.forced && (mine(t) || t.lang.empty()); });
    selectSubtitle(pick ? pick->id : 0);
    lastSubtitle_ = pick ? pick->id : 0;
}

void VideoPlayer::selectSubtitle(int id) {
    if (!mpv_) return;
    auto* handle = static_cast<mpv_handle*>(mpv_);
    api()->setPropertyString(handle, "sid", id ? std::to_string(id).c_str() : "no");
    api()->setPropertyString(handle, "sub-visibility", "yes");
}

void VideoPlayer::cycleSubtitles(bool forward) {
    if (!open_) return;
    std::vector<Track> subtitles;
    for (auto& track : tracks()) {
        if (track.subtitle) subtitles.push_back(track);
    }
    if (subtitles.empty()) {
        flash(L"No subtitles for this video");
        return;
    }
    // Off, then each track in turn.
    const int current = std::atoi(stringProperty("sid").c_str());
    const int count = static_cast<int>(subtitles.size()) + 1;
    int index = 0;
    for (int i = 0; i < static_cast<int>(subtitles.size()); ++i) {
        if (subtitles[i].id == current) index = i + 1;
    }
    index = (index + (forward ? 1 : count - 1)) % count;
    if (index == 0) {
        selectSubtitle(0);
        flash(L"Subtitles off");
    } else {
        selectSubtitle(subtitles[index - 1].id);
        lastSubtitle_ = subtitles[index - 1].id;
        flash(L"Subtitles: " + label(subtitles[index - 1]));
    }
}

void VideoPlayer::toggleSubtitles() {
    if (!open_) return;
    const int current = std::atoi(stringProperty("sid").c_str());
    if (current) {
        lastSubtitle_ = current;
        selectSubtitle(0);
        flash(L"Subtitles off");
        return;
    }
    const auto list = tracks();
    const Track* pick = nullptr;
    for (const auto& track : list) {
        if (track.subtitle && track.id == lastSubtitle_) pick = &track;
    }
    // Nothing chosen before: your language, plain before SDH, else whatever there is.
    for (int pass = 0; pass < 3 && !pick; ++pass) {
        for (const auto& track : list) {
            if (!track.subtitle) continue;
            const bool ok = pass == 0   ? matchesLanguage(track) && !track.forced && !track.hearingImpaired
                            : pass == 1 ? matchesLanguage(track)
                                        : true;
            if (ok) {
                pick = &track;
                break;
            }
        }
    }
    if (!pick) {
        flash(L"No subtitles for this video");
        return;
    }
    selectSubtitle(pick->id);
    lastSubtitle_ = pick->id;
    flash(L"Subtitles: " + label(*pick));
}

void VideoPlayer::fillTrackMenus(HMENU subtitles, HMENU audio) {
    const auto list = tracks();
    const int current = std::atoi(stringProperty("sid").c_str());
    AppendMenuW(subtitles, MF_STRING | (current == 0 ? MF_CHECKED : 0), kTrackMenuBase + kMenuOff, L"Off\tV");
    int index = 0;
    for (const auto& track : list) {
        if (!track.subtitle || index >= kMenuLoadFile - kMenuFirstSubtitle) continue;
        AppendMenuW(subtitles, MF_STRING | (track.id == current ? MF_CHECKED : 0), kTrackMenuBase + kMenuFirstSubtitle + index,
                    label(track).c_str());
        ++index;
    }
    AppendMenuW(subtitles, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(subtitles, MF_STRING, kTrackMenuBase + kMenuLoadFile, L"Load subtitle file...");
    if (!audio) return;
    index = 0;
    for (const auto& track : list) {
        if (track.subtitle || index >= kMenuEnd - kMenuFirstAudio) continue;
        AppendMenuW(audio, MF_STRING | (track.selected ? MF_CHECKED : 0), kTrackMenuBase + kMenuFirstAudio + index, label(track).c_str());
        ++index;
    }
}

void VideoPlayer::appendTrackMenus(HMENU menu) {
    if (!open_) return;
    int audioTracks = 0;
    for (const auto& track : tracks()) audioTracks += track.subtitle ? 0 : 1;
    HMENU subtitles = CreatePopupMenu();
    HMENU audio = audioTracks > 1 ? CreatePopupMenu() : nullptr;
    fillTrackMenus(subtitles, audio);
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(subtitles), L"Subtitles");
    if (audio) AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(audio), L"Audio track");
}

void VideoPlayer::showTrackMenu() {
    int audioTracks = 0;
    for (const auto& track : tracks()) audioTracks += track.subtitle ? 0 : 1;
    HMENU menu = CreatePopupMenu();
    HMENU audio = audioTracks > 1 ? CreatePopupMenu() : nullptr;
    fillTrackMenus(menu, audio);  // subtitles straight in the menu, audio tracks below
    if (audio) {
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(audio), L"Audio track");
    }
    POINT corner{ccRect_.left, ccRect_.top};
    ClientToScreen(parent_, &corner);
    const int chosen = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_BOTTOMALIGN | TPM_LEFTALIGN | TPM_RIGHTBUTTON, corner.x,
                                      corner.y, 0, parent_, nullptr);
    DestroyMenu(menu);
    if (chosen) trackCommand(chosen);
}

bool VideoPlayer::trackCommand(int id) {
    if (id < kTrackMenuBase || id >= kTrackMenuBase + kMenuEnd) return false;
    if (!open_) return true;
    const int item = id - kTrackMenuBase;
    if (item == kMenuOff) {
        const int current = std::atoi(stringProperty("sid").c_str());
        if (current) lastSubtitle_ = current;
        selectSubtitle(0);
        flash(L"Subtitles off");
    } else if (item == kMenuLoadFile) {
        loadSubtitleFile();
    } else {
        const bool subtitle = item < kMenuLoadFile;
        int index = item - (subtitle ? kMenuFirstSubtitle : kMenuFirstAudio);
        for (const auto& track : tracks()) {
            if (track.subtitle != subtitle || index-- != 0) continue;
            if (subtitle) {
                selectSubtitle(track.id);
                lastSubtitle_ = track.id;
                flash(L"Subtitles: " + label(track));
            } else {
                api()->setPropertyString(static_cast<mpv_handle*>(mpv_), "aid", std::to_string(track.id).c_str());
                flash(L"Audio: " + label(track));
            }
            break;
        }
    }
    drawControls();
    return true;
}

void VideoPlayer::loadSubtitleFile() {
    wchar_t file[MAX_PATH * 4] = L"";
    const std::wstring folder = Navigator::folderOf(path_);
    OPENFILENAMEW dialog{sizeof(dialog)};
    dialog.hwndOwner = parent_;
    dialog.lpstrFilter = L"Subtitles\0*.srt;*.ass;*.ssa;*.vtt;*.sub;*.idx;*.sup\0All files\0*.*\0";
    dialog.lpstrFile = file;
    dialog.nMaxFile = static_cast<DWORD>(std::size(file));
    dialog.lpstrInitialDir = folder.c_str();
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&dialog)) return;
    const std::string chosen = utf8(file);
    run(api(), static_cast<mpv_handle*>(mpv_), {"sub-add", chosen.c_str(), "select"});
    lastSubtitle_ = std::atoi(stringProperty("sid").c_str());
    const std::wstring name(file);
    flash(L"Subtitles: " + name.substr(name.find_last_of(L"\\/") + 1));
}

void VideoPlayer::placeSubtitles() {
    if (!mpv_ || !open_) return;
    RECT client{};
    GetClientRect(parent_, &client);
    // mpv measures the margin in pixels of a 720-pixel-high window; 22 is its own default.
    int margin = 22;
    if (controlsVisible_ && client.bottom > 0) {
        const double covered = (16 + 44 + 14) * GetDpiForWindow(parent_) / 96.0;
        margin = std::max(22, static_cast<int>(covered / client.bottom * 720 + 0.5));
    }
    api()->setPropertyString(static_cast<mpv_handle*>(mpv_), "sub-margin-y", std::to_string(margin).c_str());
}

void VideoPlayer::flash(const std::wstring& text) {
    toast_ = text;
    toastUntil_ = GetTickCount64() + 1800;
    drawControls();
}
