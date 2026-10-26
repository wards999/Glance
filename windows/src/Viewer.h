#pragma once

#include "Adjustments.h"
#include "Navigator.h"
#include "Pipeline.h"
#include "Settings.h"
#include "Video.h"

#include <d2d1_1.h>
#include <d3d11.h>
#include <dwrite.h>
#include <dxgi1_2.h>

#include <future>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

/// A decoded image uploaded to the GPU, split into tiles if it's bigger than a texture can be.
struct GpuImage {
    struct Tile {
        ComPtr<ID2D1Bitmap1> bitmap;
        D2D1_RECT_F rect;  // in bitmap pixels
    };
    UINT width = 0;
    UINT height = 0;
    size_t bytes = 0;
    std::vector<Tile> tiles;
};

enum class ZoomMode { Fit, Actual, Custom };

/// The viewer window. Drawing is Direct2D on a flip-model swap chain: an image is uploaded
/// once and every zoom, pan, rotation or adjustment afterwards only changes what the GPU does
/// with it.
class Viewer {
public:
    explicit Viewer(HINSTANCE instance);
    ~Viewer();

    bool create();
    /// Starts decoding straight away, before the window is shown or the GPU is set up.
    void open(const std::vector<std::wstring>& paths);
    void show(int showCommand);
    void openDialog();

private:
    static LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT handle(UINT message, WPARAM wParam, LPARAM lParam);

    // Graphics
    bool initGraphics();
    /// Until the GPU is ready (about 200 ms after launch, mostly loading the driver), the first
    /// image is painted with GDI so it appears as soon as it's decoded.
    void paintWithGdi(HDC dc);
    void releaseGraphics();
    void createTarget();
    void resize();
    void render();
    void drawImage(D2D1_INTERPOLATION_MODE mode);
    /// Runs `input` through the active adjustments. False when there are none.
    bool applyEffects(ID2D1Image* input, ComPtr<ID2D1Image>& output);
    void drawOverlay();
    std::wstring overlayText() const;
    bool ensureText();
    /// Bottom-left note for a page that's taking a while, so it never looks frozen.
    void drawBusyNote();
    std::shared_ptr<GpuImage> upload(const Decoded& image);
    std::shared_ptr<GpuImage> gpuFor(const DecodeKey& key, const std::shared_ptr<Decoded>& image);
    void forgetGpu(const std::wstring& path);

    // Images
    DecodeKey keyFor(const std::wstring& path, UINT frame, const DecodeRequest& request) const;
    int pdfLayout() const;
    bool showingPdf() const;
    void showCurrent();
    void show(const DecodeKey& key, const std::shared_ptr<Decoded>& image, bool resetView);
    void onDecoded(std::unique_ptr<DecodeResult> result);
    void prefetchNeighbors();
    void requestResolutionIfNeeded();
    DecodeRequest openingRequest() const;
    void scanFolder(const std::wstring& folder, const std::wstring& selected);
    void go(int delta);
    void goToFile(int delta);

    // View geometry (all in device pixels; "image" coordinates are oriented full-resolution pixels)
    void viewSize(double& width, double& height) const;
    double orientedWidth() const;
    double orientedHeight() const;
    double fitZoom() const;
    void applyZoomMode();
    void clampCenter();
    void setZoom(double zoom, double anchorX, double anchorY);
    void zoomBy(double factor);
    void zoomToRect(double x0, double y0, double x1, double y1);
    void scrollBy(double dx, double dy);
    bool canPanX() const;
    bool canPanY() const;
    D2D1_POINT_2F toImage(double x, double y) const;
    D2D1_POINT_2F toView(double x, double y) const;
    D2D1_MATRIX_3X2_F orientation() const;
    D2D1_MATRIX_3X2_F imageTransform() const;

    // Hold to zoom, and keeping the zoom when turning PDF pages
    bool holdToZoomEnabled() const;
    void beginMagnify();
    void followMagnifier(POINT point);
    void endMagnify();
    void carryZoomForPageTurn(bool forward);

    // Editing
    void setAdjustments(const Adjustments& adjustments);
    std::shared_ptr<Decoded> fullResolutionUpright();
    std::shared_ptr<Decoded> applyAdjustmentsForSaving(const Decoded& upright);
    void cropToSelection();
    void saveAs();
    void losslessTransform(int which);
    void revertEdits();

    // Animated GIFs: frames decode on a worker thread, then play on a timer
    void startAnimation();
    void stopAnimation();
    void showAnimationFrame();

    // Multi-page TIFFs (PDF pages turn with go())
    void changePage(int delta);
    bool hasPages() const;

    // The button bar at the bottom, and hiding the pointer in full screen
    void drawControlBar();
    int controlBarHit(POINT point) const;
    void mouseActivity(POINT point);
    bool ensureIconFont();

    // Videos (libmpv, loaded only when the first one is shown)
    bool isVideoFile(const std::wstring& path) const;
    void showVideo(const std::wstring& path);
    void leaveVideo();
    bool videoKey(WPARAM key, bool control, bool shift);
    /// The video's controls show while the mouse moves, and stay while paused or in use.
    void syncVideoControls();
    VideoPlayer::Options videoOptions() const;

    // More file commands
    void printImage();
    void pasteFromClipboard();
    void setModificationDate(bool toDateTaken);
    void saveParameters();
    void clearParameters();

    // Window, menu and commands
    void setFullScreen(bool on);
    void updateTitle();
    void showContextMenu(int x, int y);
    void command(int id);
    bool onKey(WPARAM key, bool control, bool shift);
    void applySettings();
    void setSlideshow(bool on);
    /// `originalSize`: the full-resolution picture, rather than what's on screen.
    void copyImage(bool originalSize);
    void copyPath();
    void revealInExplorer();
    void setAsWallpaper();
    void moveToRecycleBin();
    void reload();
    size_t cacheLimit() const;

    HINSTANCE instance_;
    HWND hwnd_ = nullptr;
    Settings settings_;
    SortOrder appliedSort_ = SortOrder::Name;
    bool appliedDescending_ = false;
    int appliedPdfLayout_ = 0;
    /// Creating the Direct3D device loads the GPU driver, which is slow; it starts on another
    /// thread the moment the app does, in parallel with the window and the first decode.
    std::future<ComPtr<ID3D11Device>> pendingDevice_;
    std::unique_ptr<Pipeline> pipeline_;
    Navigator navigator_;

    ComPtr<ID3D11Device> d3d_;
    ComPtr<ID2D1Factory1> d2dFactory_;
    ComPtr<ID2D1Device> d2dDevice_;
    ComPtr<ID2D1DeviceContext> context_;
    ComPtr<IDXGISwapChain1> swapChain_;
    ComPtr<ID2D1Bitmap1> target_;
    ComPtr<ID2D1SolidColorBrush> brush_;
    ComPtr<ID2D1SolidColorBrush> overlayBrush_;
    ComPtr<ID2D1SolidColorBrush> darkBrush_;
    ComPtr<ID2D1Effect> colorEffect_;
    ComPtr<ID2D1Effect> gammaEffect_;
    ComPtr<ID2D1Effect> sharpenEffect_;
    UINT maxBitmapSize_ = 8192;

    // Text for the info overlay; created the first time it's shown, so launch doesn't pay for it.
    ComPtr<IDWriteFactory> dwrite_;
    ComPtr<IDWriteTextFormat> textFormat_;
    UINT textFormatDpi_ = 0;

    // GPU copies of recent and decoded-ahead images, so showing them is only a draw.
    std::unordered_map<DecodeKey, std::shared_ptr<GpuImage>, DecodeKeyHash> gpuCache_;
    std::vector<DecodeKey> gpuOrder_;
    size_t gpuBytes_ = 0;

    std::shared_ptr<Decoded> current_;  // best decode of what's on screen
    std::shared_ptr<GpuImage> shown_;
    DecodeKey currentKey_;
    DecodeKey wantedKey_;
    bool waiting_ = false;
    bool busy_ = false;  // waited long enough to say so
    DecodeKey levelKey_;
    UINT requestedLevel_ = 0;
    std::vector<DecodeKey> prefetchKeys_;
    int direction_ = 1;
    UINT frame_ = 0;  // which page or spread of a PDF
    // The PDF being read, kept apart from the picture so a page that fails to render doesn't
    // lose your place: you can still turn to the next one.
    std::wstring pdfPath_;
    UINT pdfScreens_ = 0;

    ZoomMode zoomMode_ = ZoomMode::Fit;
    double zoom_ = 1;      // device pixels per full-resolution pixel
    double centerX_ = 0;   // image point at the middle of the view
    double centerY_ = 0;
    int quarterTurns_ = 0; // user rotation, clockwise

    bool dragging_ = false;
    bool didDrag_ = false;
    bool zoomBox_ = false;
    bool selecting_ = false;  // Ctrl+drag: crop selection, as in JPEGView
    POINT dragStart_{};
    POINT dragLast_{};
    int wheelRemainder_ = 0;

    bool hasSelection_ = false;
    D2D1_RECT_F selection_{};  // image coordinates

    bool holdPending_ = false;
    bool magnifying_ = false;
    double savedZoom_ = 1;
    double savedCenterX_ = 0;
    double savedCenterY_ = 0;
    ZoomMode savedMode_ = ZoomMode::Fit;

    bool carryZoom_ = false;
    double carriedZoom_ = 1;
    bool carryRight_ = false;
    bool carryBottom_ = false;

    Adjustments adjustments_;
    Levels levels_;
    std::wstring levelsFor_;
    std::shared_ptr<Decoded> edited_;  // after a crop: upright, full-resolution pixels

    std::shared_ptr<Animation> animation_;
    std::shared_ptr<GpuImage> animationGpu_;
    size_t animationFrame_ = 0;
    unsigned animationGeneration_ = 0;  // a newer picture makes older animation decodes irrelevant

    struct BarButton {
        int command;
        const wchar_t* glyph;  // Segoe icon font character, or text
        bool text;
        D2D1_RECT_F rect;
    };
    std::vector<BarButton> barButtons_;
    bool barVisible_ = false;
    int barHover_ = -1;
    bool trackingMouse_ = false;
    bool cursorHidden_ = false;
    POINT hiddenAt_{};  // screen position when the pointer was hidden
    POINT lastMouse_{-1, -1};
    ComPtr<IDWriteTextFormat> iconFormat_;
    UINT iconFormatDpi_ = 0;

    std::unique_ptr<VideoPlayer> video_;
    bool showingVideo_ = false;
    std::wstring videoPath_;
    bool videoPressed_ = false;  // a click on the picture (not the controls) is under way

    bool fullScreen_ = false;
    bool slideshow_ = false;
    WINDOWPLACEMENT windowedPlacement_{sizeof(WINDOWPLACEMENT)};
};
