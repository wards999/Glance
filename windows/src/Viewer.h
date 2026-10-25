#pragma once

#include "Navigator.h"
#include "Pipeline.h"

#include <d2d1_1.h>
#include <d3d11.h>
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
/// once and every zoom, pan or rotation afterwards only changes a transform.
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
    std::shared_ptr<GpuImage> upload(const Decoded& image);
    std::shared_ptr<GpuImage> gpuFor(const DecodeKey& key, const std::shared_ptr<Decoded>& image);

    // Images
    void showCurrent();
    void show(const DecodeKey& key, const std::shared_ptr<Decoded>& image, bool resetView);
    void onDecoded(std::unique_ptr<DecodeResult> result);
    void prefetchNeighbors();
    void requestResolutionIfNeeded();
    DecodeRequest openingRequest() const;
    void scanFolder(const std::wstring& folder, const std::wstring& selected);
    void go(int delta);

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
    D2D1_MATRIX_3X2_F orientation() const;
    D2D1_MATRIX_3X2_F imageTransform() const;

    // Window
    void setFullScreen(bool on);
    void updateTitle();
    void showContextMenu(int x, int y);
    void command(int id);
    bool onKey(WPARAM key, bool control);

    HINSTANCE instance_;
    HWND hwnd_ = nullptr;
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
    UINT maxBitmapSize_ = 8192;

    // GPU copies of recent and decoded-ahead images, so showing them is only a draw.
    std::unordered_map<DecodeKey, std::shared_ptr<GpuImage>, DecodeKeyHash> gpuCache_;
    std::vector<DecodeKey> gpuOrder_;
    size_t gpuBytes_ = 0;

    std::shared_ptr<Decoded> current_;  // best decode of what's on screen
    std::shared_ptr<GpuImage> shown_;
    DecodeKey currentKey_;
    DecodeKey wantedKey_;
    bool waiting_ = false;
    DecodeKey levelKey_;
    UINT requestedLevel_ = 0;
    std::vector<DecodeKey> prefetchKeys_;
    int direction_ = 1;

    ZoomMode zoomMode_ = ZoomMode::Fit;
    double zoom_ = 1;      // device pixels per full-resolution pixel
    double centerX_ = 0;   // image point at the middle of the view
    double centerY_ = 0;
    int quarterTurns_ = 0; // user rotation, clockwise

    bool dragging_ = false;
    bool didDrag_ = false;
    bool zoomBox_ = false;
    POINT dragStart_{};
    POINT dragLast_{};
    int wheelRemainder_ = 0;

    bool fullScreen_ = false;
    WINDOWPLACEMENT windowedPlacement_{sizeof(WINDOWPLACEMENT)};

    // Matches your JPEGView setup; becomes Settings later.
    bool enlargeSmallImages_ = true;
    bool wheelNavigates_ = true;
    bool escapeCloses_ = true;
};
