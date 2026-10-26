#pragma once

#include "Common.h"

#include <d2d1.h>
#include <dwrite.h>

#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

/// Video playback through libmpv (mpv's player engine, built on FFmpeg), in libmpv-2.dll beside
/// Glance.exe. Like PDFium, the DLL is loaded the first time a video is shown, so it costs nothing
/// at launch or when viewing pictures, and without it Glance simply doesn't list videos.
///
/// mpv draws the video into a child window with its own GPU swap chain and hardware decoding.
/// Glance keeps all the input: mpv's window lets the mouse through, and the controls (play/pause,
/// time, timeline, volume) are drawn by Glance in a click-through layered window on top. Pictures
/// above the timeline come from a second, silent mpv that decodes only the nearest keyframe.
namespace Video {

/// Whether libmpv-2.dll is there to load (checked without loading it).
bool available();
bool isVideo(const std::wstring& path);
const std::vector<std::wstring>& extensions();

}  // namespace Video

class VideoPlayer {
public:
    struct Options {
        bool autoplay = true;
        bool previews = true;
        bool loopShort = true;
    };

    /// `eventMessage` is posted to `parent` when mpv has news (handleEvents), `previewMessage`
    /// when a timeline picture is ready (takePreview).
    VideoPlayer(HWND parent, UINT eventMessage, UINT previewMessage);
    ~VideoPlayer();

    /// Starts mpv and its windows without a file, so the first video opens without the ~0.4 s
    /// it takes to bring up mpv's GPU output. False if libmpv-2.dll can't be loaded.
    bool warm();
    /// Shows `path`: the first frame straight away, paused. play() starts it.
    bool open(const std::wstring& path);
    void play();
    /// Stops playback, closes the file (so it can be deleted or renamed) and hides the video.
    void close();
    bool isOpen() const { return open_; }

    void setOptions(const Options& options) { options_ = options; }
    void layout();                  // after the parent resized
    /// Drains mpv's events (on eventMessage). True when the title should be refreshed.
    bool handleEvents();
    void takePreview(LPARAM result);  // on previewMessage
    void tick();                    // ~4 times a second while the controls show: the time moves on

    // Playback
    void togglePause();
    bool paused() const;
    bool playing() const { return open_ && !paused() && !ended_; }
    void seekBy(double seconds);
    void frameStep(bool forward);
    void changeVolume(int delta);
    void toggleMute();

    // Controls, in the parent's client coordinates
    void setControlsVisible(bool visible);
    bool controlsVisible() const { return controlsVisible_; }
    bool overControls(POINT point) const;
    /// Pointer moved (hover pictures, dragging the playhead).
    void mouseMove(POINT point);
    /// A press on the controls: true if it was theirs (seek, play/pause, mute).
    bool mouseDown(POINT point);
    void mouseUp(POINT point);
    void mouseLeave();
    bool scrubbing() const { return scrubbing_; }

    /// "1920 x 1080  -  1:32:10" for the title bar.
    std::wstring describe() const;

private:
    struct Preview {
        UINT width = 0, height = 0;
        std::vector<uint8_t> pixels;  // BGRA, opaque
    };

    bool start();
    void createWindows();
    double property(const char* name, double fallback = 0) const;
    std::string stringProperty(const char* name) const;
    void command(std::initializer_list<const char*> arguments);
    void drawControls();
    // Geometry of the controls (client coordinates, recomputed by drawControls)
    double timeAt(int x) const;
    void requestPreview(double time);
    int bucketFor(double time) const;

    HWND parent_;
    UINT eventMessage_;
    UINT previewMessage_;
    Options options_;
    void* mpv_ = nullptr;  // mpv_handle
    HWND surface_ = nullptr;
    HWND overlay_ = nullptr;
    std::atomic<bool> wakePending_{false};
    bool open_ = false;
    bool ended_ = false;
    std::wstring path_;
    double duration_ = 0;
    int videoWidth_ = 0, videoHeight_ = 0;

    // Controls
    bool controlsVisible_ = false;
    bool scrubbing_ = false;
    bool hovering_ = false;
    POINT hover_{};
    RECT barRect_{}, trackRect_{}, playRect_{}, muteRect_{};
    ComPtr<ID2D1Factory> d2d_;
    ComPtr<IDWriteFactory> dwrite_;
    ComPtr<ID2D1DCRenderTarget> target_;
    ComPtr<IDWriteTextFormat> textFormat_, iconFormat_;
    UINT formatDpi_ = 0;
    HDC dc_ = nullptr;          // the overlay's pixels, handed to UpdateLayeredWindow
    HBITMAP dib_ = nullptr;
    HGDIOBJ oldBitmap_ = nullptr;
    int dibWidth_ = 0, dibHeight_ = 0;

    // Timeline pictures: a second mpv on its own thread, newest request wins.
    std::thread previewThread_;
    std::mutex previewLock_;
    std::condition_variable previewWake_;
    bool previewQuit_ = false;
    std::wstring previewPath_;   // guarded by previewLock_
    double previewTime_ = -1;    // guarded by previewLock_; < 0: nothing wanted
    unsigned previewGeneration_ = 0;  // guarded by previewLock_
    UINT previewWidth_ = 0;      // guarded by previewLock_
    std::map<int, Preview> previews_;  // UI thread: bucket -> picture, for the open video
    unsigned generation_ = 0;    // UI thread copy
    void previewWorker();
};
