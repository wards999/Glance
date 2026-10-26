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
#include <utility>
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

/// Subtitle languages Glance can prefer: a name for menus, and the codes files use for it.
struct Language {
    const wchar_t* name;
    std::vector<std::string> codes;  // lowercase: "en", "eng", "english"
};
const std::vector<Language>& languages();

}  // namespace Video

class VideoPlayer {
public:
    struct Options {
        bool autoplay = true;
        bool previews = true;
        bool loopShort = true;
        bool alwaysSubtitles = false;  // otherwise only forced ones (for foreign-language dialogue)
        int subtitleLanguage = 0;      // index into Video::languages()
        int volume = 100;              // where the player starts (the level you last left it at)
        bool muted = false;            // and whether it was muted
    };

    /// Command IDs for the subtitle and audio menus start here (the CC button and right-click).
    static constexpr int kTrackMenuBase = 1000;

    /// `eventMessage` is posted to `parent` when mpv has news (handleEvents), `previewMessage`
    /// when a timeline picture is ready (takePreview).
    VideoPlayer(HWND parent, UINT eventMessage, UINT previewMessage);
    ~VideoPlayer();

    /// Starts mpv and its windows without a file, so the first video opens without the ~0.4 s
    /// it takes to bring up mpv's GPU output. False if libmpv-2.dll can't be loaded.
    bool warm();
    /// Shows `path`, playing at once if `play`; otherwise paused on the first frame until play().
    bool open(const std::wstring& path, bool play);
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

    // Subtitles and audio tracks
    void cycleSubtitles(bool forward);  // J / Shift+J, through every track and Off
    void toggleSubtitles();             // V
    /// Adds "Subtitles" and "Audio" submenus (IDs from kTrackMenuBase) when the video has them.
    void appendTrackMenus(HMENU menu);
    /// A command from those menus; false if the ID isn't one of them.
    bool trackCommand(int id);

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
    bool scrubbing() const { return scrubbing_ || volumeDragging_; }  // dragging the timeline or volume

    /// The bar also has Previous File, Next File and Full Screen, which are Glance's to do: after
    /// mouseDown, takeAction says which was pressed.
    enum class Action { None, PreviousFile, NextFile, FullScreen };
    Action takeAction() { return std::exchange(action_, Action::None); }
    void setFullScreen(bool on) { fullScreen_ = on; }  // for the full screen button's picture

    /// "1920 x 1080  -  1:32:10" for the title bar.
    std::wstring describe() const;
    /// The volume now, percent (to remember for next time).
    int volume() const;
    bool muted() const;

private:
    struct Track {
        bool subtitle = false;  // otherwise audio
        int id = 0;             // mpv's sid / aid
        std::string lang, title, codec, file;
        bool external = false, forced = false, isDefault = false, hearingImpaired = false, selected = false;
    };
    std::vector<Track> tracks() const;
    std::wstring label(const Track& track) const;
    bool matchesLanguage(const Track& track) const;
    /// After a file loads: the subtitle the settings ask for (an .srt you put there, else forced
    /// dialogue, or with "always" a full track in your language).
    void chooseSubtitles();
    void selectSubtitle(int id);  // 0: off
    void loadSubtitleFile();
    void showTrackMenu();
    void fillTrackMenus(HMENU subtitles, HMENU audio);
    /// Keeps subtitles above the controls while they show.
    void placeSubtitles();
    /// A short message over the video ("Subtitles: English"), for a moment.
    void flash(const std::wstring& text);

    struct Preview {
        double time = 0;  // of the frame shown
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
    bool firstFrame_ = false;  // for the trace log
    std::wstring path_;
    double duration_ = 0;
    double lastPosition_ = 0;  // shown while mpv has no position (during a seek)
    int videoWidth_ = 0, videoHeight_ = 0;

    // Controls
    bool controlsVisible_ = false;
    bool scrubbing_ = false;
    bool hovering_ = false;
    POINT hover_{};
    RECT barRect_{}, trackRect_{}, playRect_{}, muteRect_{}, ccRect_{}, volumeRect_{};
    RECT previousRect_{}, nextRect_{}, fullRect_{};
    Action action_ = Action::None;
    bool fullScreen_ = false;
    bool volumeDragging_ = false;
    void setVolumeAt(int x);
    std::wstring toast_;
    ULONGLONG toastUntil_ = 0;
    int lastSubtitle_ = 0;  // what V turns back on
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
    std::map<int, Preview> previews_;  // UI thread: bucket -> picture made ahead, for the open video
    Preview exact_;                    // UI thread: the exact frame for the latest hover
    double exactTime_ = -1;            // its time; < 0: none
    double exactTolerance() const;     // seconds: about a pixel of timeline
    unsigned generation_ = 0;    // UI thread copy
    void previewWorker();
};
