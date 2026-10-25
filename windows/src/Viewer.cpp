#include "Viewer.h"

#include "AdjustmentsDialog.h"
#include "Editing.h"
#include "Pdf.h"
#include "SettingsDialog.h"

#include <commdlg.h>
#include <d2d1effects.h>
#if __has_include(<d2d1effects_2.h>)
#include <d2d1effects_2.h>
#define GLANCE_HAS_SHARPEN 1
#endif
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <thread>
#include <unordered_set>

namespace {

constexpr UINT WM_APP_DECODED = WM_APP + 1;
constexpr UINT WM_APP_FOLDER = WM_APP + 2;
constexpr UINT WM_APP_GPU_READY = WM_APP + 3;
constexpr UINT_PTR kSlideshowTimer = 1;
constexpr UINT_PTR kHoldTimer = 2;
constexpr UINT_PTR kBusyTimer = 3;
constexpr UINT kHoldDelayMs = 200;
constexpr UINT kBusyDelayMs = 150;
// PDF pages can take seconds each in Windows' engine, so read further ahead than for photos.
constexpr int kPdfPrefetch = 6;
constexpr wchar_t kClassName[] = L"GlanceViewer";
constexpr size_t kGpuBudget = size_t(1) << 30;  // 1 GB of decoded-ahead textures
constexpr int kSlideshowChoices[] = {1, 2, 3, 5, 10, 30};
constexpr UINT kEditedMarker = 0xFFFFFFFF;  // DecodeRequest::longSide of a cropped picture's cache key

enum Command {
    CmdOpen = 1,
    CmdSaveAs,
    CmdReload,
    CmdReveal,
    CmdWallpaper,
    CmdCopy,
    CmdCopyPath,
    CmdInfo,
    CmdFilename,
    CmdNext,
    CmdPrevious,
    CmdFirst,
    CmdLast,
    CmdLoop,
    CmdDescending,
    CmdSlideshow,
    CmdRotateRight,
    CmdRotateLeft,
    CmdCrop,
    CmdRevert,
    CmdAutoCorrect,
    CmdAdjustments,
    CmdKeepAdjustments,
    CmdZoomIn,
    CmdZoomOut,
    CmdFit,
    CmdActual,
    CmdEnlarge,
    CmdFullScreen,
    CmdRightToLeft,
    CmdRecycle,
    CmdSettings,
    CmdAbout,
    CmdExit,
    CmdSortBase = 100,      // + SortOrder
    CmdIntervalBase = 200,  // + index into kSlideshowChoices
    CmdLosslessBase = 300,  // + 0 right, 1 left, 2 180°, 3 flip horizontal, 4 flip vertical
    CmdPdfLayoutBase = 400, // + Settings::pdfLayout
};

struct FolderResult {
    std::wstring selected;  // empty: a folder was opened, show its first image
    std::vector<FileEntry> files;
};

D2D1_MATRIX_3X2_F matrix(float m11, float m12, float m21, float m22, float dx = 0, float dy = 0) {
    D2D1_MATRIX_3X2_F m;
    m._11 = m11;
    m._12 = m12;
    m._21 = m21;
    m._22 = m22;
    m._31 = dx;
    m._32 = dy;
    return m;
}

D2D1_MATRIX_3X2_F identity() { return matrix(1, 0, 0, 1); }

D2D1_COLOR_F colour(COLORREF c, float alpha = 1) {
    return D2D1_COLOR_F{GetRValue(c) / 255.0f, GetGValue(c) / 255.0f, GetBValue(c) / 255.0f, alpha};
}

/// Everything WIC can decode, plus PDF when asked for and available. Asks WIC once; the
/// caller's thread must have COM initialised.
std::unordered_set<std::wstring> supportedExtensions(bool withPdf) {
    static const std::unordered_set<std::wstring> images = [] {
        auto list = Decoder::supportedExtensions();
        return std::unordered_set<std::wstring>(list.begin(), list.end());
    }();
    auto extensions = images;
    if (withPdf && Pdf::available()) extensions.insert(L"pdf");
    return extensions;
}

std::wstring fileName(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

ComPtr<ID3D11Device> createDevice() {
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                        D3D_FEATURE_LEVEL_10_0};
    ComPtr<ID3D11Device> device;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                   levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, device.put(), nullptr, nullptr);
    if (FAILED(hr)) {
        // No usable GPU (remote desktop, broken driver): software rendering still works.
        D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                          ARRAYSIZE(levels), D3D11_SDK_VERSION, device.put(), nullptr, nullptr);
    }
    trace("GPU device created");
    return device;
}

}  // namespace

Viewer::Viewer(HINSTANCE instance) : instance_(instance), settings_(Settings::load()) {
    appliedSort_ = settings_.sortOrder;
    appliedDescending_ = settings_.descending;
    appliedPdfLayout_ = pdfLayout();
    navigator_.wraps = settings_.loop;
}

Viewer::~Viewer() {
    pipeline_.reset();
}

// MARK: - Window

bool Viewer::create() {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_DBLCLKS | CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = windowProc;
    wc.hInstance = instance_;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    // Black from the very first frame, before anything is drawn.
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);

    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int width = (work.right - work.left) * 8 / 10;
    int height = (work.bottom - work.top) * 8 / 10;
    int x = work.left + (work.right - work.left - width) / 2;
    int y = work.top + (work.bottom - work.top - height) / 2;
    hwnd_ = CreateWindowExW(WS_EX_ACCEPTFILES, kClassName, L"Glance", WS_OVERLAPPEDWINDOW, x, y, width, height,
                            nullptr, nullptr, instance_, this);
    if (!hwnd_) return false;

    // Where the window was last time, if that's still on a screen.
    RECT saved = settings_.windowRect;
    if (saved.right - saved.left >= 320 && saved.bottom - saved.top >= 240 &&
        MonitorFromRect(&saved, MONITOR_DEFAULTTONULL)) {
        WINDOWPLACEMENT placement{sizeof(placement)};
        GetWindowPlacement(hwnd_, &placement);
        placement.rcNormalPosition = saved;
        placement.showCmd = SW_HIDE;
        SetWindowPlacement(hwnd_, &placement);
    }

    // Loading the GPU driver is the slowest part of starting up, so it runs on its own thread
    // and the window hears when it's done; nothing waits for it.
    pendingDevice_ = std::async(std::launch::async, [hwnd = hwnd_] {
        auto device = createDevice();
        PostMessageW(hwnd, WM_APP_GPU_READY, 0, 0);
        return device;
    });

    unsigned threads = std::clamp(std::thread::hardware_concurrency() / 4, 2u, 4u);
    pipeline_ = std::make_unique<Pipeline>(hwnd_, WM_APP_DECODED, cacheLimit(), threads);
    trace("window and decoder threads created");
    return true;
}

size_t Viewer::cacheLimit() const {
    if (settings_.cacheMB > 0) return size_t(settings_.cacheMB) << 20;
    MEMORYSTATUSEX memory{sizeof(memory)};
    GlobalMemoryStatusEx(&memory);
    return static_cast<size_t>(std::min<ULONGLONG>(memory.ullTotalPhys / 4, ULONGLONG(4) << 30));
}

void Viewer::show(int showCommand) {
    if (settings_.startFullScreen) {
        setFullScreen(true);
    } else {
        ShowWindow(hwnd_, settings_.maximized ? SW_MAXIMIZE : showCommand);
    }
    UpdateWindow(hwnd_);
    trace("window shown");
}

void Viewer::paintWithGdi(HDC dc) {
    RECT client{};
    GetClientRect(hwnd_, &client);
    HBRUSH background = CreateSolidBrush(settings_.background);
    FillRect(dc, &client, background);
    DeleteObject(background);
    // GDI can't rotate cheaply; a rotated photo simply waits the extra moment for the GPU.
    if (!current_ || current_->info.orientation != 1 || quarterTurns_ != 0) return;

    double width, height;
    viewSize(width, height);
    const double left = width / 2 - centerX_ * zoom_;
    const double top = height / 2 - centerY_ * zoom_;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = static_cast<LONG>(current_->width);
    info.bmiHeader.biHeight = -static_cast<LONG>(current_->height);  // top-down rows
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    SetStretchBltMode(dc, HALFTONE);
    SetBrushOrgEx(dc, 0, 0, nullptr);
    StretchDIBits(dc, static_cast<int>(std::lround(left)), static_cast<int>(std::lround(top)),
                  static_cast<int>(std::lround(orientedWidth() * zoom_)), static_cast<int>(std::lround(orientedHeight() * zoom_)),
                  0, 0, static_cast<int>(current_->width), static_cast<int>(current_->height), current_->pixels.get(), &info,
                  DIB_RGB_COLORS, SRCCOPY);
    trace("first image painted with GDI");
}

LRESULT CALLBACK Viewer::windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        reinterpret_cast<Viewer*>(create->lpCreateParams)->hwnd_ = hwnd;
    }
    auto* viewer = reinterpret_cast<Viewer*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return viewer ? viewer->handle(message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT Viewer::handle(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_APP_DECODED:
        onDecoded(std::unique_ptr<DecodeResult>(reinterpret_cast<DecodeResult*>(lParam)));
        return 0;

    case WM_APP_FOLDER: {
        std::unique_ptr<FolderResult> result(reinterpret_cast<FolderResult*>(lParam));
        if (result->selected.empty()) {
            navigator_.setFiles(std::move(result->files), L"");
            frame_ = 0;
            showCurrent();
        } else if (const std::wstring* now = navigator_.current(); now && samePath(*now, result->selected)) {
            // Only if you haven't opened something else meanwhile.
            navigator_.setFiles(std::move(result->files), result->selected);
            updateTitle();
            prefetchNeighbors();
            render();
        }
        return 0;
    }

    case WM_APP_GPU_READY:
        // Swap to GPU drawing; the picture doesn't move, only the drawing path changes.
        if (!context_ && initGraphics()) {
            if (current_) shown_ = gpuFor(currentKey_, current_);
            render();
        }
        return 0;

    case WM_TIMER:
        if (wParam == kSlideshowTimer) {
            direction_ = 1;
            if (showingPdf() && frame_ + 1 < pdfScreens_) {
                ++frame_;
                showCurrent();
            } else if (navigator_.move(1)) {
                frame_ = 0;
                showCurrent();
            } else {
                setSlideshow(false);
            }
        } else if (wParam == kHoldTimer) {
            KillTimer(hwnd_, kHoldTimer);
            holdPending_ = false;
            if (dragging_ && !didDrag_) beginMagnify();
        } else if (wParam == kBusyTimer) {
            KillTimer(hwnd_, kBusyTimer);
            if (waiting_) {
                busy_ = true;
                render();
            }
        }
        return 0;

    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) {
            resize();
            if (zoomMode_ != ZoomMode::Custom) applyZoomMode(); else clampCenter();
            render();
            requestResolutionIfNeeded();
        }
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT paint;
        HDC dc = BeginPaint(hwnd_, &paint);
        if (!context_) paintWithGdi(dc);
        EndPaint(hwnd_, &paint);
        render();
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;  // everything is painted in WM_PAINT; avoid a flash of the class brush

    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMinTrackSize = {320, 240};
        return 0;
    }

    case WM_DPICHANGED: {
        auto* suggested = reinterpret_cast<RECT*>(lParam);
        SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                     suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }

    case WM_KEYDOWN:
        if (onKey(wParam, GetKeyState(VK_CONTROL) < 0, GetKeyState(VK_SHIFT) < 0)) return 0;
        break;

    case WM_SYSKEYDOWN:
        if (wParam == VK_RETURN) {  // Alt+Enter
            setFullScreen(!fullScreen_);
            return 0;
        }
        break;

    case WM_MOUSEWHEEL: {
        POINT p{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(hwnd_, &p);
        int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        bool control = GET_KEYSTATE_WPARAM(wParam) & MK_CONTROL;
        if (settings_.wheelNavigates && !control) {
            // High-resolution wheels send small steps; act on whole notches.
            wheelRemainder_ += delta;
            while (std::abs(wheelRemainder_) >= WHEEL_DELTA) {
                go(wheelRemainder_ > 0 ? -1 : 1);
                wheelRemainder_ -= wheelRemainder_ > 0 ? WHEEL_DELTA : -WHEEL_DELTA;
            }
        } else if (current_) {
            setZoom(zoom_ * std::pow(1.2, double(delta) / WHEEL_DELTA), p.x, p.y);
        }
        return 0;
    }

    case WM_LBUTTONDOWN:
        if (!current_) return 0;
        SetCapture(hwnd_);
        dragging_ = true;
        didDrag_ = false;
        dragStart_ = dragLast_ = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        selecting_ = (wParam & MK_CONTROL) != 0;  // Ctrl+drag selects an area to crop, as in JPEGView
        // Drag pans a zoomed-in image; when it all fits, drag draws a rectangle to zoom into.
        zoomBox_ = !selecting_ && !(canPanX() || canPanY());
        if (!selecting_ && holdToZoomEnabled()) {
            holdPending_ = true;
            SetTimer(hwnd_, kHoldTimer, kHoldDelayMs, nullptr);
        }
        return 0;

    case WM_MOUSEMOVE:
        if (dragging_) {
            POINT p{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (magnifying_) {
                followMagnifier(p);
                render();
                return 0;
            }
            const bool moved = std::abs(p.x - dragStart_.x) + std::abs(p.y - dragStart_.y) > 4;
            if (holdPending_) {
                if (!moved) return 0;  // still might be a hold
                KillTimer(hwnd_, kHoldTimer);
                holdPending_ = false;
            }
            if (moved) didDrag_ = true;
            if (selecting_) {
                const D2D1_POINT_2F a = toImage(dragStart_.x, dragStart_.y), b = toImage(p.x, p.y);
                const float w = static_cast<float>(orientedWidth()), h = static_cast<float>(orientedHeight());
                selection_ = D2D1_RECT_F{std::clamp(std::min(a.x, b.x), 0.0f, w), std::clamp(std::min(a.y, b.y), 0.0f, h),
                                         std::clamp(std::max(a.x, b.x), 0.0f, w), std::clamp(std::max(a.y, b.y), 0.0f, h)};
                hasSelection_ = didDrag_;
            } else if (!zoomBox_) {
                scrollBy(dragLast_.x - p.x, dragLast_.y - p.y);
            }
            dragLast_ = p;
            render();
        }
        return 0;

    case WM_LBUTTONUP:
        if (dragging_) {
            ReleaseCapture();
            KillTimer(hwnd_, kHoldTimer);
            holdPending_ = false;
            dragging_ = false;
            if (magnifying_) {
                endMagnify();
            } else if (zoomBox_ && didDrag_ && std::abs(dragLast_.x - dragStart_.x) >= 8 && std::abs(dragLast_.y - dragStart_.y) >= 8) {
                zoomToRect(dragStart_.x, dragStart_.y, dragLast_.x, dragLast_.y);
            } else if (selecting_ && (selection_.right - selection_.left < 4 || selection_.bottom - selection_.top < 4)) {
                hasSelection_ = false;
            } else if (!selecting_ && !didDrag_) {
                hasSelection_ = false;  // a plain click clears the selection
            }
            zoomBox_ = false;
            selecting_ = false;
            render();
        }
        return 0;

    case WM_LBUTTONDBLCLK: {
        if (!current_) return 0;
        // Toggle between fitting the window and 100% at the clicked point.
        double fit = fitZoom();
        if (zoomMode_ == ZoomMode::Fit || std::abs(zoom_ - fit) < 0.001) {
            setZoom(std::abs(fit - 1) < 0.01 ? 2.0 : 1.0, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        } else {
            zoomMode_ = ZoomMode::Fit;
            applyZoomMode();
            render();
        }
        return 0;
    }

    case WM_XBUTTONUP:
        go(GET_XBUTTON_WPARAM(wParam) == XBUTTON1 ? -1 : 1);
        return TRUE;

    case WM_CONTEXTMENU: {
        int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);
        if (x == -1 && y == -1) {  // keyboard menu key
            POINT p{0, 0};
            ClientToScreen(hwnd_, &p);
            x = p.x;
            y = p.y;
        }
        showContextMenu(x, y);
        return 0;
    }

    case WM_DROPFILES: {
        HDROP drop = reinterpret_cast<HDROP>(wParam);
        std::vector<std::wstring> paths;
        UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < count; ++i) {
            UINT length = DragQueryFileW(drop, i, nullptr, 0);
            std::wstring path(length, L'\0');
            DragQueryFileW(drop, i, path.data(), length + 1);
            paths.push_back(path);
        }
        DragFinish(drop);
        open(paths);
        return 0;
    }

    case WM_DESTROY: {
        // Remember the window for next time (the windowed position, even if closed full screen).
        WINDOWPLACEMENT placement{sizeof(placement)};
        if (fullScreen_) placement = windowedPlacement_;
        else GetWindowPlacement(hwnd_, &placement);
        settings_.windowRect = placement.rcNormalPosition;
        settings_.maximized = placement.showCmd == SW_SHOWMAXIMIZED;
        settings_.save();
        pipeline_.reset();  // stop the workers before the window goes away
        PostQuitMessage(0);
        return 0;
    }
    }
    return DefWindowProcW(hwnd_, message, wParam, lParam);
}

bool Viewer::onKey(WPARAM key, bool control, bool shift) {
    double width, height;
    viewSize(width, height);
    const double step = std::max(width, height) * 0.15;
    switch (key) {
    // Arrows pan a zoomed-in image and change image otherwise, as in JPEGView.
    case VK_RIGHT: if (canPanX()) scrollBy(step, 0); else go(1); break;
    case VK_LEFT: if (canPanX()) scrollBy(-step, 0); else go(-1); break;
    case VK_DOWN: if (canPanY()) scrollBy(0, step); else go(1); break;
    case VK_UP: if (canPanY()) scrollBy(0, -step); else go(-1); break;
    case VK_SPACE: case VK_NEXT: go(1); break;
    case VK_BACK: case VK_PRIOR: go(-1); break;
    case VK_HOME: command(CmdFirst); break;
    case VK_END: command(CmdLast); break;
    case VK_ADD: case VK_OEM_PLUS: command(CmdZoomIn); break;
    case VK_SUBTRACT: case VK_OEM_MINUS: command(CmdZoomOut); break;
    case '0': case VK_NUMPAD0: command(CmdFit); break;
    case '1': case VK_NUMPAD1: command(CmdActual); break;
    case 'F': case VK_F11: command(CmdFullScreen); break;
    case 'R': command(control ? CmdReload : CmdRotateRight); break;
    case 'L': command(CmdRotateLeft); break;
    case 'E': command(CmdAdjustments); break;
    case 'W': command(CmdReveal); break;
    case VK_F2: command(control ? CmdFilename : CmdInfo); break;
    case VK_F4: command(CmdKeepAdjustments); break;
    case VK_F5: command(CmdAutoCorrect); break;
    case VK_DELETE: command(CmdRecycle); break;
    case VK_RETURN:
        if (!hasSelection_) return false;
        command(CmdCrop);
        break;
    case 'S':
        command(control ? CmdSaveAs : CmdSlideshow);
        break;
    case 'C':
        if (!control) return false;
        command(shift ? CmdCopyPath : CmdCopy);
        break;
    case 'O':
        if (!control) return false;
        command(CmdOpen);
        break;
    case VK_ESCAPE:
        if (dragging_) {
            ReleaseCapture();
            dragging_ = zoomBox_ = selecting_ = false;
            if (magnifying_) endMagnify();
        } else if (hasSelection_) {
            hasSelection_ = false;
        } else if (slideshow_) {
            setSlideshow(false);
        } else if (settings_.escapeCloses) {
            DestroyWindow(hwnd_);
            return true;
        } else if (fullScreen_) {
            setFullScreen(false);
        }
        break;
    default:
        return false;
    }
    render();
    return true;
}

void Viewer::setFullScreen(bool on) {
    if (on == fullScreen_) return;
    fullScreen_ = on;
    if (on) {
        GetWindowPlacement(hwnd_, &windowedPlacement_);
        MONITORINFO monitor{sizeof(monitor)};
        GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &monitor);
        const RECT& r = monitor.rcMonitor;
        // Borderless and covering the whole monitor, taskbar included: no animation, no delay.
        SetWindowLongPtrW(hwnd_, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(hwnd_, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top,
                     SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    } else {
        SetWindowLongPtrW(hwnd_, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        if (windowedPlacement_.showCmd == SW_HIDE) windowedPlacement_.showCmd = SW_SHOWNORMAL;
        SetWindowPlacement(hwnd_, &windowedPlacement_);
        SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                     SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER);
    }
}

void Viewer::updateTitle() {
    const std::wstring* path = navigator_.current();
    if (!path) {
        SetWindowTextW(hwnd_, L"Glance");
        return;
    }
    std::wstring title = fileName(*path) + L"  -  " + std::to_wstring(navigator_.index() + 1) + L" / " +
                         std::to_wstring(navigator_.count());
    if (current_ && samePath(current_->info.path, *path)) {
        if (current_->info.isPdf) {
            title += L"  -  " + current_->info.pdfPages;
        } else {
            title += L"  -  " + std::to_wstring(current_->info.orientedWidth()) + L" x " +
                     std::to_wstring(current_->info.orientedHeight());
        }
        title += L"  -  " + std::to_wstring(static_cast<int>(std::lround(zoom_ * 100))) + L"%";
        if (edited_ || !adjustments_.isIdentity()) title += L"  -  edited";
    } else if (!waiting_ && showingPdf()) {
        // One page failed; the rest of the document is still there.
        title += L"  -  screen " + std::to_wstring(frame_ + 1) + L" of " + std::to_wstring(pdfScreens_) +
                 L" couldn't be shown (Space for the next)";
    } else if (!waiting_) {
        title += L"  -  can't open this file";
    }
    title += L"  -  Glance";
    SetWindowTextW(hwnd_, title.c_str());
}

void Viewer::showContextMenu(int x, int y) {
    const bool hasImage = current_ != nullptr;
    const bool hasFile = navigator_.current() != nullptr;
    const bool several = navigator_.count() > 1 || (showingPdf() && pdfScreens_ > 1);
    const bool pdf = showingPdf();
    auto add = [](HMENU menu, int id, const wchar_t* text, bool enabled = true, bool checked = false) {
        AppendMenuW(menu, MF_STRING | (enabled ? 0 : MF_GRAYED) | (checked ? MF_CHECKED : 0), id, text);
    };
    auto separator = [](HMENU menu) { AppendMenuW(menu, MF_SEPARATOR, 0, nullptr); };
    auto submenu = [](HMENU menu, HMENU child, const wchar_t* text) {
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(child), text);
    };

    // Laid out like JPEGView's menu.
    HMENU menu = CreatePopupMenu();
    add(menu, CmdOpen, L"Open image...\tCtrl+O");
    add(menu, CmdSaveAs, L"Save processed image...\tCtrl+S", hasImage);
    add(menu, CmdReload, L"Reload image\tCtrl+R", hasFile);
    add(menu, CmdReveal, L"Open containing folder in Explorer\tW", hasFile);
    add(menu, CmdWallpaper, L"Set as desktop wallpaper", hasFile && !pdf);
    separator(menu);
    add(menu, CmdCopy, L"Copy to clipboard\tCtrl+C", hasImage);
    add(menu, CmdCopyPath, L"Copy file path\tCtrl+Shift+C", hasFile);
    separator(menu);
    add(menu, CmdInfo, L"Show picture info (EXIF)\tF2", true, settings_.showInfo);
    add(menu, CmdFilename, L"Show filename\tCtrl+F2", true, settings_.showFilename);
    separator(menu);
    add(menu, CmdNext, L"Next image\tSpace", several);
    add(menu, CmdPrevious, L"Previous image\tBackspace", several);

    HMENU navigation = CreatePopupMenu();
    add(navigation, CmdFirst, L"First image\tHome", navigator_.count() > 1);
    add(navigation, CmdLast, L"Last image\tEnd", navigator_.count() > 1);
    separator(navigation);
    add(navigation, CmdLoop, L"Loop through folder", true, settings_.loop);
    submenu(menu, navigation, L"Navigation");

    HMENU order = CreatePopupMenu();
    const wchar_t* orders[] = {L"File name", L"Date modified", L"Date created", L"File size", L"Random"};
    for (int i = 0; i < 5; ++i) {
        AppendMenuW(order, MF_STRING | (static_cast<int>(settings_.sortOrder) == i ? MF_CHECKED : 0), CmdSortBase + i, orders[i]);
    }
    separator(order);
    add(order, CmdDescending, L"Descending", true, settings_.descending);
    submenu(menu, order, L"Display order");

    HMENU slideshow = CreatePopupMenu();
    add(slideshow, CmdSlideshow, slideshow_ ? L"Stop slideshow\tS" : L"Start slideshow\tS", several);
    separator(slideshow);
    for (int i = 0; i < static_cast<int>(std::size(kSlideshowChoices)); ++i) {
        std::wstring text = std::to_wstring(kSlideshowChoices[i]) + (kSlideshowChoices[i] == 1 ? L" second" : L" seconds");
        add(slideshow, CmdIntervalBase + i, text.c_str(), true, settings_.slideshowSeconds == kSlideshowChoices[i]);
    }
    submenu(menu, slideshow, L"Play folder as slideshow");
    separator(menu);

    HMENU transform = CreatePopupMenu();
    add(transform, CmdRotateRight, L"Rotate right\tR", hasImage);
    add(transform, CmdRotateLeft, L"Rotate left\tL", hasImage);
    separator(transform);
    add(transform, CmdCrop, L"Crop to selection\tEnter", hasSelection_);
    add(transform, CmdRevert, L"Revert all edits", edited_ || !adjustments_.isIdentity() || quarterTurns_ != 0);
    submenu(menu, transform, L"Transform image");

    HMENU lossless = CreatePopupMenu();
    const bool canLossless = hasImage && !pdf && !edited_;
    add(lossless, CmdLosslessBase + 0, L"Rotate 90 degrees clockwise", canLossless);
    add(lossless, CmdLosslessBase + 1, L"Rotate 90 degrees counter-clockwise", canLossless);
    add(lossless, CmdLosslessBase + 2, L"Rotate 180 degrees", canLossless);
    add(lossless, CmdLosslessBase + 3, L"Mirror horizontally", canLossless);
    add(lossless, CmdLosslessBase + 4, L"Mirror vertically", canLossless);
    submenu(menu, lossless, L"Lossless JPEG transformations");
    add(menu, CmdAutoCorrect, L"Auto correction\tF5", hasImage, adjustments_.autoCorrect);
    add(menu, CmdAdjustments, L"Adjustments...\tE", hasImage);
    add(menu, CmdKeepAdjustments, L"Keep parameters\tF4", true, settings_.keepAdjustments);
    separator(menu);

    HMENU zoom = CreatePopupMenu();
    add(zoom, CmdZoomIn, L"Zoom in\t+", hasImage);
    add(zoom, CmdZoomOut, L"Zoom out\t-", hasImage);
    add(zoom, CmdFit, L"Fit to window\t0", hasImage);
    add(zoom, CmdActual, L"Actual size (100%)\t1", hasImage);
    submenu(menu, zoom, L"Zoom");
    HMENU autoZoom = CreatePopupMenu();
    add(autoZoom, CmdEnlarge, L"Enlarge small images to fit", true, settings_.enlargeSmall);
    submenu(menu, autoZoom, L"Auto zoom mode");
    if (Pdf::available()) {
        HMENU pages = CreatePopupMenu();
        const wchar_t* layouts[] = {L"Automatic", L"Single page", L"Two pages", L"Two pages with cover"};
        for (int i = 0; i < 4; ++i) add(pages, CmdPdfLayoutBase + i, layouts[i], true, settings_.pdfLayout == i);
        separator(pages);
        add(pages, CmdRightToLeft, L"Right to left", true, settings_.pdfRightToLeft);
        submenu(menu, pages, L"PDF pages");
    }
    add(menu, CmdFullScreen, L"Full screen\tF11", true, fullScreen_);
    separator(menu);
    add(menu, CmdRecycle, L"Move to Recycle Bin\tDel", hasFile);
    separator(menu);
    add(menu, CmdSettings, L"Settings...");
    add(menu, CmdAbout, L"About Glance...");
    separator(menu);
    add(menu, CmdExit, settings_.escapeCloses ? L"Exit\tEsc" : L"Exit");

    int chosen = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, x, y, 0, hwnd_, nullptr);
    DestroyMenu(menu);  // destroys the submenus too
    if (chosen) {
        command(chosen);
        render();
    }
}

void Viewer::command(int id) {
    if (id >= CmdSortBase && id < CmdSortBase + 5) {
        settings_.sortOrder = static_cast<SortOrder>(id - CmdSortBase);
        settings_.save();
        applySettings();
        return;
    }
    if (id >= CmdIntervalBase && id < CmdIntervalBase + static_cast<int>(std::size(kSlideshowChoices))) {
        settings_.slideshowSeconds = kSlideshowChoices[id - CmdIntervalBase];
        settings_.save();
        applySettings();
        return;
    }
    if (id >= CmdLosslessBase && id < CmdLosslessBase + 5) {
        losslessTransform(id - CmdLosslessBase);
        return;
    }
    if (id >= CmdPdfLayoutBase && id < CmdPdfLayoutBase + 4) {
        settings_.pdfLayout = id - CmdPdfLayoutBase;
        settings_.save();
        applySettings();
        return;
    }
    switch (id) {
    case CmdOpen: openDialog(); break;
    case CmdSaveAs: saveAs(); break;
    case CmdReload: reload(); break;
    case CmdReveal: revealInExplorer(); break;
    case CmdWallpaper: setAsWallpaper(); break;
    case CmdCopy: copyImage(); break;
    case CmdCopyPath: copyPath(); break;
    case CmdInfo:
        settings_.showInfo = !settings_.showInfo;
        settings_.save();
        break;
    case CmdFilename:
        settings_.showFilename = !settings_.showFilename;
        settings_.save();
        break;
    case CmdNext: go(1); break;
    case CmdPrevious: go(-1); break;
    case CmdFirst:
        direction_ = 1;
        navigator_.first();
        frame_ = 0;
        showCurrent();
        break;
    case CmdLast:
        direction_ = -1;
        navigator_.last();
        frame_ = 0;
        showCurrent();
        break;
    case CmdLoop:
        settings_.loop = !settings_.loop;
        settings_.save();
        applySettings();
        break;
    case CmdDescending:
        settings_.descending = !settings_.descending;
        settings_.save();
        applySettings();
        break;
    case CmdSlideshow: setSlideshow(!slideshow_); break;
    case CmdZoomIn: zoomBy(1.25); break;
    case CmdZoomOut: zoomBy(0.8); break;
    case CmdFit:
        zoomMode_ = ZoomMode::Fit;
        applyZoomMode();
        break;
    case CmdActual: {
        double width, height;
        viewSize(width, height);
        setZoom(1, width / 2, height / 2);
        break;
    }
    case CmdEnlarge:
        settings_.enlargeSmall = !settings_.enlargeSmall;
        settings_.save();
        applySettings();
        break;
    case CmdRotateRight:
    case CmdRotateLeft:
        if (!current_) break;
        quarterTurns_ = (quarterTurns_ + (id == CmdRotateRight ? 1 : 3)) % 4;
        hasSelection_ = false;
        centerX_ = orientedWidth() / 2;
        centerY_ = orientedHeight() / 2;
        if (zoomMode_ != ZoomMode::Custom) applyZoomMode(); else clampCenter();
        updateTitle();
        break;
    case CmdCrop: cropToSelection(); break;
    case CmdRevert: revertEdits(); break;
    case CmdAutoCorrect: {
        Adjustments changed = adjustments_;
        changed.autoCorrect = !changed.autoCorrect;
        setAdjustments(changed);
        AdjustmentsDialog::update(adjustments_);
        break;
    }
    case CmdAdjustments:
        if (current_) AdjustmentsDialog::show(hwnd_, adjustments_, [this](const Adjustments& a) { setAdjustments(a); });
        break;
    case CmdKeepAdjustments:
        settings_.keepAdjustments = !settings_.keepAdjustments;
        settings_.save();
        break;
    case CmdRightToLeft:
        settings_.pdfRightToLeft = !settings_.pdfRightToLeft;
        settings_.save();
        applySettings();
        break;
    case CmdFullScreen: setFullScreen(!fullScreen_); break;
    case CmdRecycle: moveToRecycleBin(); break;
    case CmdSettings:
        SettingsDialog::show(hwnd_, &settings_, [this] { applySettings(); });
        break;
    case CmdAbout:
        MessageBoxW(hwnd_, L"Glance 0.1\nA fast image viewer.\n\nhttps://github.com/wards999/Glance", L"About Glance",
                    MB_OK | MB_ICONINFORMATION);
        break;
    case CmdExit: DestroyWindow(hwnd_); break;
    }
}

/// After the Settings dialog or a menu toggle changed something.
void Viewer::applySettings() {
    navigator_.wraps = settings_.loop;
    if (settings_.sortOrder != appliedSort_ || settings_.descending != appliedDescending_) {
        appliedSort_ = settings_.sortOrder;
        appliedDescending_ = settings_.descending;
        navigator_.sort(settings_.sortOrder, settings_.descending);
    }
    if (pdfLayout() != appliedPdfLayout_) {
        appliedPdfLayout_ = pdfLayout();
        // Screens count spreads, so find the one that holds the page you were on.
        if (showingPdf() && current_) {
            const int page = current_->info.pdfFirstPage;
            const auto spreads = Pdf::spreads(current_->info.path, pdfLayout());
            frame_ = 0;
            for (size_t i = 0; i < spreads.size(); ++i) {
                if (std::find(spreads[i].begin(), spreads[i].end(), page) != spreads[i].end()) frame_ = static_cast<UINT>(i);
            }
            showCurrent();
        }
    }
    if (slideshow_) SetTimer(hwnd_, kSlideshowTimer, UINT(settings_.slideshowSeconds) * 1000, nullptr);
    if (pipeline_) pipeline_->setCostLimit(cacheLimit());
    if (zoomMode_ == ZoomMode::Fit) applyZoomMode();
    updateTitle();
    if (current_) prefetchNeighbors();
    render();
}

void Viewer::setSlideshow(bool on) {
    if (on && navigator_.count() < 2 && !showingPdf()) return;
    slideshow_ = on;
    if (on) SetTimer(hwnd_, kSlideshowTimer, UINT(settings_.slideshowSeconds) * 1000, nullptr);
    else KillTimer(hwnd_, kSlideshowTimer);
}

void Viewer::openDialog() {
    wchar_t buffer[32768] = L"";
    OPENFILENAMEW ofn{sizeof(ofn)};
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFilter = Pdf::available()
        ? L"Images and PDFs\0*.jpg;*.jpeg;*.jpe;*.png;*.gif;*.bmp;*.tif;*.tiff;*.webp;*.heic;*.heif;*.avif;*.jxr;*.ico;*.dng;*.cr2;*.cr3;*.nef;*.arw;*.orf;*.rw2;*.pdf\0All files\0*.*\0"
        : L"Images\0*.jpg;*.jpeg;*.jpe;*.png;*.gif;*.bmp;*.tif;*.tiff;*.webp;*.heic;*.heif;*.avif;*.jxr;*.ico;*.dng;*.cr2;*.cr3;*.nef;*.arw;*.orf;*.rw2\0All files\0*.*\0";
    ofn.lpstrFile = buffer;
    ofn.nMaxFile = static_cast<DWORD>(std::size(buffer));
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (GetOpenFileNameW(&ofn)) open({buffer});
}

// MARK: - File commands

void Viewer::reload() {
    const std::wstring* path = navigator_.current();
    if (!path) return;
    const std::wstring file = *path;
    pipeline_->invalidate(file);
    forgetGpu(file);
    showCurrent();
}

void Viewer::revealInExplorer() {
    const std::wstring* path = navigator_.current();
    if (!path) return;
    if (PIDLIST_ABSOLUTE item = ILCreateFromPathW(path->c_str())) {
        SHOpenFolderAndSelectItems(item, 0, nullptr, 0);
        ILFree(item);
    }
}

void Viewer::setAsWallpaper() {
    const std::wstring* path = navigator_.current();
    if (!path) return;
    std::wstring file = *path;
    SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, file.data(), SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);
}

void Viewer::copyPath() {
    const std::wstring* path = navigator_.current();
    if (!path || !OpenClipboard(hwnd_)) return;
    EmptyClipboard();
    const size_t bytes = (path->size() + 1) * sizeof(wchar_t);
    if (HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        std::memcpy(GlobalLock(memory), path->c_str(), bytes);
        GlobalUnlock(memory);
        SetClipboardData(CF_UNICODETEXT, memory);
    }
    CloseClipboard();
}

void Viewer::copyImage() {
    if (!current_) return;
    // Upright, as on screen, with adjustments applied.
    auto upright = Editing::orient(*current_, orientation());
    if (!upright) return;
    if (!adjustments_.isIdentity()) {
        if (auto adjusted = applyAdjustmentsForSaving(*upright)) upright = adjusted;
    }
    if (!OpenClipboard(hwnd_)) return;
    EmptyClipboard();
    const size_t rowBytes = size_t(upright->width) * 4;
    if (HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPINFOHEADER) + rowBytes * upright->height)) {
        auto* header = static_cast<BITMAPINFOHEADER*>(GlobalLock(memory));
        *header = BITMAPINFOHEADER{};
        header->biSize = sizeof(BITMAPINFOHEADER);
        header->biWidth = static_cast<LONG>(upright->width);
        header->biHeight = static_cast<LONG>(upright->height);  // bottom-up, which every program handles
        header->biPlanes = 1;
        header->biBitCount = 32;
        header->biCompression = BI_RGB;
        auto* bits = reinterpret_cast<uint8_t*>(header + 1);
        for (UINT y = 0; y < upright->height; ++y) {
            std::memcpy(bits + size_t(upright->height - 1 - y) * rowBytes, upright->pixels.get() + size_t(y) * upright->stride, rowBytes);
        }
        GlobalUnlock(memory);
        SetClipboardData(CF_DIB, memory);
    }
    CloseClipboard();
}

void Viewer::moveToRecycleBin() {
    const std::wstring* path = navigator_.current();
    if (!path) return;
    const std::wstring file = *path;
    // Recycle Bin, so it's recoverable; the file list must end with two nulls.
    std::wstring from = file;
    from.push_back(L'\0');
    SHFILEOPSTRUCTW operation{};
    operation.hwnd = hwnd_;
    operation.wFunc = FO_DELETE;
    operation.pFrom = from.c_str();
    operation.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT;
    if (SHFileOperationW(&operation) != 0 || operation.fAnyOperationsAborted) return;
    pipeline_->invalidate(file);
    forgetGpu(file);
    navigator_.removeCurrent();
    frame_ = 0;
    showCurrent();
}

// MARK: - Editing

void Viewer::setAdjustments(const Adjustments& adjustments) {
    adjustments_ = adjustments;
    updateTitle();
    render();
}

std::shared_ptr<Decoded> Viewer::fullResolutionUpright() {
    // A cropped picture is already upright; only a rotation since then still needs applying.
    if (edited_) return quarterTurns_ ? Editing::orient(*edited_, orientation()) : edited_;
    if (!current_) return nullptr;
    Decoder decoder;
    auto full = decoder.decode(current_->info.path, current_->frame, DecodeRequest{}, currentKey_.layout);
    return full ? Editing::orient(*full, orientation()) : nullptr;
}

std::shared_ptr<Decoded> Viewer::applyAdjustmentsForSaving(const Decoded& upright) {
    if (!context_ || std::max(upright.width, upright.height) > maxBitmapSize_) return nullptr;
    D2D1_BITMAP_PROPERTIES1 sourceProperties{};
    sourceProperties.pixelFormat = {DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED};
    sourceProperties.dpiX = sourceProperties.dpiY = 96;
    D2D1_BITMAP_PROPERTIES1 targetProperties = sourceProperties;
    targetProperties.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET;
    D2D1_BITMAP_PROPERTIES1 readProperties = sourceProperties;
    readProperties.bitmapOptions = static_cast<D2D1_BITMAP_OPTIONS>(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW);
    const D2D1_SIZE_U size{upright.width, upright.height};
    ComPtr<ID2D1Bitmap1> source, target, readback;
    if (FAILED(context_->CreateBitmap(size, upright.pixels.get(), upright.stride, &sourceProperties, source.put())) ||
        FAILED(context_->CreateBitmap(size, nullptr, 0, &targetProperties, target.put())) ||
        FAILED(context_->CreateBitmap(size, nullptr, 0, &readProperties, readback.put()))) {
        return nullptr;
    }
    // Draw through the same effects as the screen, into an off-screen bitmap of full size.
    ComPtr<ID2D1Image> output;
    if (!applyEffects(source.get(), output)) return nullptr;
    ComPtr<ID2D1Image> screen;
    context_->GetTarget(screen.put());
    context_->SetTarget(target.get());
    context_->BeginDraw();
    const D2D1_MATRIX_3X2_F plain = identity();
    context_->SetTransform(&plain);
    context_->DrawImage(output.get(), nullptr, nullptr, D2D1_INTERPOLATION_MODE_LINEAR, D2D1_COMPOSITE_MODE_SOURCE_COPY);
    HRESULT hr = context_->EndDraw();
    context_->SetTarget(screen.get());
    if (FAILED(hr) || FAILED(readback->CopyFromBitmap(nullptr, target.get(), nullptr))) return nullptr;

    D2D1_MAPPED_RECT mapped{};
    if (FAILED(readback->Map(D2D1_MAP_OPTIONS_READ, &mapped))) return nullptr;
    auto result = std::make_shared<Decoded>();
    result->info = upright.info;
    result->frame = upright.frame;
    result->width = upright.width;
    result->height = upright.height;
    result->stride = upright.width * 4;
    result->pixels.reset(new (std::nothrow) uint8_t[result->bytes()]);
    if (result->pixels) {
        for (UINT y = 0; y < upright.height; ++y) {
            std::memcpy(result->pixels.get() + size_t(y) * result->stride, mapped.bits + size_t(y) * mapped.pitch, result->stride);
        }
    }
    readback->Unmap();
    return result->pixels ? result : nullptr;
}

void Viewer::cropToSelection() {
    if (!hasSelection_ || !current_) return;
    SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    // Crops work on full-resolution pixels, whatever is on screen.
    auto upright = fullResolutionUpright();
    if (!upright) return;
    const double scale = double(upright->width) / orientedWidth();
    RECT area{static_cast<LONG>(std::floor(selection_.left * scale)), static_cast<LONG>(std::floor(selection_.top * scale)),
              static_cast<LONG>(std::ceil(selection_.right * scale)), static_cast<LONG>(std::ceil(selection_.bottom * scale))};
    auto cropped = Editing::crop(*upright, area);
    if (!cropped) return;
    edited_ = cropped;
    hasSelection_ = false;
    DecodeKey key = keyFor(current_->info.path, current_->frame, DecodeRequest{0, 0, kEditedMarker});
    forgetGpu(current_->info.path);
    show(key, edited_, true);
}

void Viewer::revertEdits() {
    edited_.reset();
    hasSelection_ = false;
    quarterTurns_ = 0;
    setAdjustments(Adjustments{});
    AdjustmentsDialog::update(adjustments_);
    showCurrent();
}

void Viewer::saveAs() {
    const std::wstring* path = navigator_.current();
    if (!path || !current_) return;
    const std::wstring source = *path;
    const bool pdf = current_->info.isPdf;

    std::wstring name = fileName(source);
    const size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos) name.resize(dot);
    if (pdf) name += L" - page " + std::to_wstring(current_->info.pdfFirstPage);
    wchar_t file[MAX_PATH * 4] = L"";
    wcsncpy_s(file, name.c_str(), _TRUNCATE);

    const std::wstring extension = Navigator::extensionOf(source);
    const DWORD filter = extension == L"png" ? 2 : (extension == L"tif" || extension == L"tiff") ? 3 : extension == L"bmp" ? 4 : 1;
    const wchar_t* extensions[] = {L"jpg", L"png", L"tif", L"bmp"};
    const GUID containers[] = {GUID_ContainerFormatJpeg, GUID_ContainerFormatPng, GUID_ContainerFormatTiff, GUID_ContainerFormatBmp};
    const std::wstring folder = Navigator::folderOf(source);

    OPENFILENAMEW ofn{sizeof(ofn)};
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFilter = L"JPEG\0*.jpg;*.jpeg\0PNG\0*.png\0TIFF\0*.tif;*.tiff\0BMP\0*.bmp\0";
    ofn.nFilterIndex = filter;
    ofn.lpstrFile = file;
    ofn.nMaxFile = static_cast<DWORD>(std::size(file));
    ofn.lpstrInitialDir = folder.c_str();
    ofn.lpstrDefExt = extensions[filter - 1];
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (!GetSaveFileNameW(&ofn)) return;
    const std::wstring target = file;
    const int format = static_cast<int>(std::clamp<DWORD>(ofn.nFilterIndex, 1, 4)) - 1;

    SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    auto upright = fullResolutionUpright();
    if (upright && !adjustments_.isIdentity()) {
        auto adjusted = applyAdjustmentsForSaving(*upright);
        if (!adjusted) {
            MessageBoxW(hwnd_, L"This image is too large to save with adjustments applied.", L"Glance", MB_OK | MB_ICONWARNING);
            return;
        }
        upright = adjusted;
    }
    if (!upright || !Editing::save(*upright, target, containers[format], settings_.saveQuality, pdf ? std::wstring() : source)) {
        MessageBoxW(hwnd_, L"The image couldn't be saved there.", L"Glance", MB_OK | MB_ICONWARNING);
        return;
    }
    if (samePath(target, source)) {
        // The edits are in the file now; show it fresh.
        edited_.reset();
        setAdjustments(Adjustments{});
        AdjustmentsDialog::update(adjustments_);
        reload();
    } else if (samePath(Navigator::folderOf(target), folder)) {
        scanFolder(folder, source);  // so the new file is in the folder listing
    }
}

void Viewer::losslessTransform(int which) {
    if (!current_ || current_->info.isPdf || edited_) return;
    const D2D1_MATRIX_3X2_F transforms[] = {
        matrix(0, 1, -1, 0),   // 90° clockwise
        matrix(0, -1, 1, 0),   // 90° counter-clockwise
        matrix(-1, 0, 0, -1),  // 180°
        matrix(-1, 0, 0, 1),   // mirror horizontally
        matrix(1, 0, 0, -1),   // mirror vertically
    };
    const std::wstring file = current_->info.path;
    const int orientation = Editing::composeOrientation(current_->info.orientation, transforms[which]);
    if (!Editing::rewriteOrientation(file, orientation)) {
        MessageBoxW(hwnd_,
                    L"This file has no orientation tag that can be changed in place, so it can't be turned "
                    L"losslessly.\n\nUse Rotate (R / L) and then Save processed image instead.",
                    L"Glance", MB_OK | MB_ICONINFORMATION);
        return;
    }
    quarterTurns_ = 0;
    reload();
}

// MARK: - Opening and navigation

int Viewer::pdfLayout() const {
    return Pdf::packLayout(settings_.pdfLayout, settings_.pdfRightToLeft);
}

DecodeKey Viewer::keyFor(const std::wstring& path, UINT frame, const DecodeRequest& request) const {
    DecodeKey key{path, frame, request};
    if (Pdf::isPdf(path)) key.layout = pdfLayout();
    return key;
}

bool Viewer::showingPdf() const {
    const std::wstring* path = navigator_.current();
    return pdfScreens_ > 0 && path && samePath(pdfPath_, *path);
}

void Viewer::open(const std::vector<std::wstring>& paths) {
    if (paths.empty()) return;
    frame_ = 0;
    DWORD attributes = GetFileAttributesW(paths[0].c_str());
    if (paths.size() == 1 && attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        scanFolder(paths[0], L"");
    } else if (paths.size() == 1) {
        // Show the file first; list its folder in the background.
        navigator_.setFiles({FileEntry{paths[0]}}, paths[0]);
        showCurrent();
        scanFolder(Navigator::folderOf(paths[0]), paths[0]);
    } else {
        std::vector<FileEntry> files;
        for (const auto& path : paths) files.push_back(Navigator::entryFor(path));
        Navigator::sortEntries(files, settings_.sortOrder, settings_.descending);
        navigator_.setFiles(std::move(files), paths[0]);
        showCurrent();
    }
}

void Viewer::scanFolder(const std::wstring& folder, const std::wstring& selected) {
    HWND hwnd = hwnd_;
    const SortOrder order = settings_.sortOrder;
    const bool descending = settings_.descending;
    // Opening a PDF directly means you're reading PDFs, so its neighbours count too.
    const bool withPdf = settings_.includePdfs || Pdf::isPdf(selected);
    std::thread([hwnd, folder, selected, order, descending, withPdf] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        auto files = Navigator::scan(folder, supportedExtensions(withPdf));
        Navigator::sortEntries(files, order, descending);
        CoUninitialize();
        PostMessageW(hwnd, WM_APP_FOLDER, 0, reinterpret_cast<LPARAM>(new FolderResult{selected, std::move(files)}));
    }).detach();
}

DecodeRequest Viewer::openingRequest() const {
    // Enough pixels to fit the window, rounded up so small resizes still hit decoded-ahead images.
    double width, height;
    viewSize(width, height);
    auto bucket = [](double pixels) { return std::max(1u, static_cast<UINT>(std::ceil(pixels / 256.0))) * 256u; };
    return DecodeRequest{bucket(width), bucket(height), 0};
}

void Viewer::go(int delta) {
    direction_ = delta < 0 ? -1 : 1;
    // In a PDF, pages turn before moving on to the next file.
    if (showingPdf()) {
        const long long target = static_cast<long long>(frame_) + delta;
        if (target >= 0 && target < static_cast<long long>(pdfScreens_)) {
            carryZoomForPageTurn(delta > 0);
            frame_ = static_cast<UINT>(target);
            showCurrent();
            return;
        }
    }
    goToFile(delta);
}

void Viewer::goToFile(int delta) {
    if (!navigator_.move(delta)) {
        MessageBeep(MB_OK);
        return;
    }
    frame_ = 0;
    showCurrent();
}

void Viewer::showCurrent() {
    const std::wstring* path = navigator_.current();
    if (waiting_) pipeline_->drop(wantedKey_);
    edited_.reset();
    hasSelection_ = false;
    if (!settings_.keepAdjustments && !adjustments_.isIdentity()) {
        adjustments_ = Adjustments{};
        AdjustmentsDialog::update(adjustments_);
    }
    if (!path) {
        waiting_ = false;
        pdfScreens_ = 0;
        current_.reset();
        shown_.reset();
        updateTitle();
        render();
        return;
    }
    if (pdfScreens_ && !samePath(*path, pdfPath_)) pdfScreens_ = 0;  // moved on to another file
    DecodeKey key = keyFor(*path, frame_, openingRequest());
    wantedKey_ = key;
    levelKey_ = {};
    if (auto hit = pipeline_->cached(key)) {
        // Decoded ahead: on screen in the same message as the key press.
        waiting_ = false;
        busy_ = false;
        KillTimer(hwnd_, kBusyTimer);
        show(key, hit, true);
        prefetchNeighbors();
        return;
    }
    // Keep the previous picture up until the new one is ready.
    waiting_ = true;
    pipeline_->load(key);
    SetTimer(hwnd_, kBusyTimer, kBusyDelayMs, nullptr);
    updateTitle();
}

void Viewer::show(const DecodeKey& key, const std::shared_ptr<Decoded>& image, bool resetView) {
    current_ = image;
    currentKey_ = key;
    if (image->info.isPdf) {
        pdfPath_ = image->info.path;
        pdfScreens_ = image->info.frameCount;
    }
    shown_ = context_ ? gpuFor(key, image) : nullptr;
    if (resetView) {
        quarterTurns_ = 0;
        zoomMode_ = ZoomMode::Fit;
        requestedLevel_ = 0;
        magnifying_ = false;
        applyZoomMode();
        if (carryZoom_) {
            // Turning a page while zoomed in: same zoom, opened at the corner where reading continues.
            carryZoom_ = false;
            zoom_ = carriedZoom_;
            zoomMode_ = ZoomMode::Custom;
            centerX_ = carryRight_ ? orientedWidth() : 0;
            centerY_ = carryBottom_ ? orientedHeight() : 0;
            clampCenter();
        }
    } else {
        clampCenter();
    }
    updateTitle();
    if (context_) {
        render();
    } else {
        // GPU not ready yet: paint with GDI right now rather than waiting for it.
        InvalidateRect(hwnd_, nullptr, FALSE);
        UpdateWindow(hwnd_);
    }
    requestResolutionIfNeeded();
}

void Viewer::onDecoded(std::unique_ptr<DecodeResult> result) {
    if (waiting_ && result->key == wantedKey_) {
        trace("decode delivered");
        waiting_ = false;
        busy_ = false;
        KillTimer(hwnd_, kBusyTimer);
        if (result->image) {
            show(result->key, result->image, true);
        } else {
            current_.reset();
            shown_.reset();
            updateTitle();
            render();
        }
        prefetchNeighbors();
        return;
    }
    if (!result->image) return;
    if (current_ && !edited_ && result->key == levelKey_ && result->image->longSide() > current_->longSide()) {
        show(result->key, result->image, false);  // sharper pixels for the zoomed view
        return;
    }
    // Decoded ahead: upload it now, while idle, so showing it later is only a draw.
    if (context_ && std::find(prefetchKeys_.begin(), prefetchKeys_.end(), result->key) != prefetchKeys_.end()) {
        gpuFor(result->key, result->image);
    }
}

void Viewer::prefetchNeighbors() {
    std::vector<DecodeKey> keys;
    const DecodeRequest request = openingRequest();
    // Mostly in the direction of travel, plus one behind.
    std::vector<int> offsets;
    const int ahead = showingPdf() && settings_.prefetchCount > 0 ? std::max(settings_.prefetchCount, kPdfPrefetch)
                                                                    : settings_.prefetchCount;
    for (int i = 1; i <= ahead; ++i) offsets.push_back(i * direction_);
    if (!offsets.empty()) offsets.insert(offsets.begin() + std::min<size_t>(2, offsets.size()), -direction_);
    const std::wstring* now = navigator_.current();
    const bool pdf = showingPdf();
    for (int offset : offsets) {
        DecodeKey key;
        const long long page = static_cast<long long>(frame_) + offset;
        if (pdf && page >= 0 && page < static_cast<long long>(pdfScreens_)) {
            key = keyFor(*now, static_cast<UINT>(page), request);  // the PDF's other pages
        } else {
            const std::wstring* path = navigator_.at(offset > 0 ? (pdf ? 1 : offset) : (pdf ? -1 : offset));
            if (!path || (now && samePath(*path, *now))) continue;
            key = keyFor(*path, 0, request);
        }
        if (std::find(keys.begin(), keys.end(), key) == keys.end()) keys.push_back(key);
    }
    prefetchKeys_ = keys;
    pipeline_->prefetch(keys);
}

void Viewer::requestResolutionIfNeeded() {
    if (!current_ || edited_) return;
    const UINT full = current_->info.longSide();
    const UINT have = current_->longSide();
    if (have >= full) return;
    const double needed = full * std::min(zoom_, 1.0);
    if (needed <= have * 1.02) return;
    UINT level = Decoder::halvingLevel(full, static_cast<UINT>(std::ceil(needed)));
    if (level <= requestedLevel_ || level <= have) return;
    requestedLevel_ = level;
    levelKey_ = keyFor(current_->info.path, current_->frame, DecodeRequest{0, 0, level});
    pipeline_->load(levelKey_);
}

// MARK: - Hold to zoom

bool Viewer::holdToZoomEnabled() const {
    return current_ && (settings_.holdToZoom == 2 || (settings_.holdToZoom == 1 && current_->info.isPdf));
}

void Viewer::beginMagnify() {
    if (!current_ || magnifying_) return;
    magnifying_ = true;
    zoomBox_ = false;
    savedZoom_ = zoom_;
    savedCenterX_ = centerX_;
    savedCenterY_ = centerY_;
    savedMode_ = zoomMode_;
    zoom_ = std::min(zoom_ * settings_.holdZoomPercent / 100.0, 32.0);
    zoomMode_ = ZoomMode::Custom;
    POINT mouse;
    GetCursorPos(&mouse);
    ScreenToClient(hwnd_, &mouse);
    followMagnifier(mouse);
    updateTitle();
    render();
    // Sharp pixels for the magnified view replace the stretched ones when they're ready.
    requestResolutionIfNeeded();
}

void Viewer::followMagnifier(POINT point) {
    // Moving across the window moves across the whole page: the point that was under the
    // mouse before magnifying stays under it.
    double width, height;
    viewSize(width, height);
    const double imageX = savedCenterX_ + (point.x - width / 2) / savedZoom_;
    const double imageY = savedCenterY_ + (point.y - height / 2) / savedZoom_;
    centerX_ = imageX - (point.x - width / 2) / zoom_;
    centerY_ = imageY - (point.y - height / 2) / zoom_;
    clampCenter();
}

void Viewer::endMagnify() {
    magnifying_ = false;
    zoom_ = savedZoom_;
    centerX_ = savedCenterX_;
    centerY_ = savedCenterY_;
    zoomMode_ = savedMode_;
    if (zoomMode_ != ZoomMode::Custom) applyZoomMode(); else clampCenter();
    updateTitle();
}

void Viewer::carryZoomForPageTurn(bool forward) {
    if (zoomMode_ == ZoomMode::Fit || !(canPanX() || canPanY())) return;
    carryZoom_ = true;
    carriedZoom_ = zoom_;
    // Forward opens at the top-left (top-right for right to left); back opens where that page ends.
    carryRight_ = forward == settings_.pdfRightToLeft;
    carryBottom_ = !forward;
}

// MARK: - Geometry

void Viewer::viewSize(double& width, double& height) const {
    RECT client{};
    GetClientRect(hwnd_, &client);
    width = std::max<LONG>(client.right - client.left, 1);
    height = std::max<LONG>(client.bottom - client.top, 1);
}

D2D1_MATRIX_3X2_F Viewer::orientation() const {
    D2D1_MATRIX_3X2_F m = Editing::exifMatrix(current_ ? current_->info.orientation : 1);
    for (int i = 0; i < quarterTurns_; ++i) m = Editing::multiply(m, matrix(0, 1, -1, 0));
    return m;
}

double Viewer::orientedWidth() const {
    if (!current_) return 1;
    return orientation()._11 == 0 ? current_->info.height : current_->info.width;
}

double Viewer::orientedHeight() const {
    if (!current_) return 1;
    return orientation()._11 == 0 ? current_->info.width : current_->info.height;
}

double Viewer::fitZoom() const {
    double width, height;
    viewSize(width, height);
    double fit = std::min(width / orientedWidth(), height / orientedHeight());
    return settings_.enlargeSmall ? fit : std::min(fit, 1.0);
}

void Viewer::applyZoomMode() {
    if (!current_) return;
    if (zoomMode_ == ZoomMode::Fit) zoom_ = fitZoom();
    if (zoomMode_ == ZoomMode::Actual) zoom_ = 1;
    if (zoomMode_ != ZoomMode::Custom) {
        centerX_ = orientedWidth() / 2;
        centerY_ = orientedHeight() / 2;
    }
    clampCenter();
    updateTitle();
}

void Viewer::clampCenter() {
    double width, height;
    viewSize(width, height);
    const double halfW = width / 2 / zoom_, halfH = height / 2 / zoom_;
    const double ow = orientedWidth(), oh = orientedHeight();
    centerX_ = ow <= halfW * 2 ? ow / 2 : std::clamp(centerX_, halfW, ow - halfW);
    centerY_ = oh <= halfH * 2 ? oh / 2 : std::clamp(centerY_, halfH, oh - halfH);
}

bool Viewer::canPanX() const {
    double width, height;
    viewSize(width, height);
    return current_ && orientedWidth() * zoom_ > width + 0.5;
}

bool Viewer::canPanY() const {
    double width, height;
    viewSize(width, height);
    return current_ && orientedHeight() * zoom_ > height + 0.5;
}

D2D1_POINT_2F Viewer::toImage(double x, double y) const {
    double width, height;
    viewSize(width, height);
    return D2D1_POINT_2F{static_cast<float>(centerX_ + (x - width / 2) / zoom_), static_cast<float>(centerY_ + (y - height / 2) / zoom_)};
}

D2D1_POINT_2F Viewer::toView(double x, double y) const {
    double width, height;
    viewSize(width, height);
    return D2D1_POINT_2F{static_cast<float>(width / 2 + (x - centerX_) * zoom_), static_cast<float>(height / 2 + (y - centerY_) * zoom_)};
}

void Viewer::setZoom(double zoom, double anchorX, double anchorY) {
    if (!current_) return;
    double width, height;
    viewSize(width, height);
    zoom = std::clamp(zoom, std::min(fitZoom(), 1.0) / 8, 32.0);
    // Keep the image point under the anchor where it is.
    const double imageX = centerX_ + (anchorX - width / 2) / zoom_;
    const double imageY = centerY_ + (anchorY - height / 2) / zoom_;
    zoom_ = zoom;
    zoomMode_ = ZoomMode::Custom;
    centerX_ = imageX - (anchorX - width / 2) / zoom_;
    centerY_ = imageY - (anchorY - height / 2) / zoom_;
    clampCenter();
    updateTitle();
    render();
    requestResolutionIfNeeded();
}

void Viewer::zoomBy(double factor) {
    if (!current_) return;
    double target = zoom_ * factor;
    if ((zoom_ < 1 && target > 1) || (zoom_ > 1 && target < 1)) target = 1;  // stop at 100% on the way
    const double fit = fitZoom();
    if (factor < 1 && zoom_ > fit + 1e-4 && target < fit) {
        zoomMode_ = ZoomMode::Fit;
        applyZoomMode();
        return;
    }
    double width, height;
    viewSize(width, height);
    setZoom(target, width / 2, height / 2);
}

void Viewer::zoomToRect(double x0, double y0, double x1, double y1) {
    double width, height;
    viewSize(width, height);
    const D2D1_POINT_2F a = toImage(std::min(x0, x1), std::min(y0, y1));
    const D2D1_POINT_2F b = toImage(std::max(x0, x1), std::max(y0, y1));
    zoom_ = std::clamp(std::min(width / (b.x - a.x), height / (b.y - a.y)), 0.01, 32.0);
    zoomMode_ = ZoomMode::Custom;
    centerX_ = (a.x + b.x) / 2;
    centerY_ = (a.y + b.y) / 2;
    clampCenter();
    updateTitle();
    requestResolutionIfNeeded();
}

void Viewer::scrollBy(double dx, double dy) {
    if (!current_) return;
    centerX_ += dx / zoom_;
    centerY_ += dy / zoom_;
    clampCenter();
}

D2D1_MATRIX_3X2_F Viewer::imageTransform() const {
    double width, height;
    viewSize(width, height);
    const double bitmapW = shown_->width, bitmapH = shown_->height;
    // The bitmap may be a reduced decode: scale it up to full-resolution units, then by the zoom.
    const double scale = zoom_ * double(current_->info.longSide()) / std::max(bitmapW, bitmapH);
    const double ow = orientedWidth(), oh = orientedHeight();
    double cx = width / 2 + (ow / 2 - centerX_) * zoom_;
    double cy = height / 2 + (oh / 2 - centerY_) * zoom_;
    // Snap the image's corner to whole pixels so 100% is pixel-exact.
    const double left = cx - ow * zoom_ / 2, top = cy - oh * zoom_ / 2;
    cx += std::round(left) - left;
    cy += std::round(top) - top;

    D2D1_MATRIX_3X2_F m = matrix(1, 0, 0, 1, float(-bitmapW / 2), float(-bitmapH / 2));
    m = Editing::multiply(m, matrix(float(scale), 0, 0, float(scale)));
    m = Editing::multiply(m, orientation());
    return Editing::multiply(m, matrix(1, 0, 0, 1, float(cx), float(cy)));
}

// MARK: - Graphics

bool Viewer::initGraphics() {
    // The device started at launch; after a GPU reset, make a new one here.
    d3d_ = pendingDevice_.valid() ? pendingDevice_.get() : createDevice();
    if (!d3d_) return false;
    if (!d2dFactory_ && FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), nullptr,
                                                 reinterpret_cast<void**>(d2dFactory_.put())))) {
        return false;
    }
    auto dxgiDevice = d3d_.as<IDXGIDevice>();
    if (!dxgiDevice || FAILED(d2dFactory_->CreateDevice(dxgiDevice.get(), d2dDevice_.put())) ||
        FAILED(d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, context_.put()))) {
        return false;
    }

    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    dxgiDevice->GetAdapter(adapter.put());
    if (!adapter || FAILED(adapter->GetParent(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(factory.put())))) {
        return false;
    }
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_NONE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    if (FAILED(factory->CreateSwapChainForHwnd(d3d_.get(), hwnd_, &desc, nullptr, nullptr, swapChain_.put()))) {
        return false;
    }
    factory->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);  // we do our own full screen
    createTarget();
    const D2D1_COLOR_F white{1, 1, 1, 0.9f};
    const D2D1_COLOR_F shade{0, 0, 0, 0.6f};
    const D2D1_COLOR_F dark{0, 0, 0, 0.9f};
    context_->CreateSolidColorBrush(&white, nullptr, brush_.put());
    context_->CreateSolidColorBrush(&shade, nullptr, overlayBrush_.put());
    context_->CreateSolidColorBrush(&dark, nullptr, darkBrush_.put());
    maxBitmapSize_ = context_->GetMaximumBitmapSize();
    trace("Direct2D ready");
    return true;
}

void Viewer::releaseGraphics() {
    gpuCache_.clear();
    gpuOrder_.clear();
    gpuBytes_ = 0;
    shown_.reset();
    brush_.reset();
    overlayBrush_.reset();
    darkBrush_.reset();
    colorEffect_.reset();
    gammaEffect_.reset();
    sharpenEffect_.reset();
    target_.reset();
    if (context_) context_->SetTarget(nullptr);
    swapChain_.reset();
    context_.reset();
    d2dDevice_.reset();
    d3d_.reset();
}

void Viewer::createTarget() {
    ComPtr<IDXGISurface> surface;
    if (FAILED(swapChain_->GetBuffer(0, __uuidof(IDXGISurface), reinterpret_cast<void**>(surface.put())))) return;
    D2D1_BITMAP_PROPERTIES1 properties{};
    properties.pixelFormat = {DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE};
    properties.dpiX = properties.dpiY = 96;  // work in device pixels
    properties.bitmapOptions = static_cast<D2D1_BITMAP_OPTIONS>(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW);
    context_->CreateBitmapFromDxgiSurface(surface.get(), &properties, target_.put());
    context_->SetTarget(target_.get());
    context_->SetDpi(96, 96);
}

void Viewer::resize() {
    if (!swapChain_) return;
    context_->SetTarget(nullptr);
    target_.reset();
    swapChain_->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, 0);
    createTarget();
}

std::shared_ptr<GpuImage> Viewer::upload(const Decoded& image) {
    auto gpu = std::make_shared<GpuImage>();
    gpu->width = image.width;
    gpu->height = image.height;
    gpu->bytes = image.bytes();
    const UINT longSide = image.longSide();
    const UINT tile = longSide <= maxBitmapSize_ ? longSide : std::min(maxBitmapSize_, 8192u);
    D2D1_BITMAP_PROPERTIES1 properties{};
    properties.pixelFormat = {DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED};
    properties.dpiX = properties.dpiY = 96;
    for (UINT y = 0; y < image.height; y += tile) {
        for (UINT x = 0; x < image.width; x += tile) {
            const UINT w = std::min(tile, image.width - x), h = std::min(tile, image.height - y);
            GpuImage::Tile t;
            const uint8_t* start = image.pixels.get() + size_t(y) * image.stride + size_t(x) * 4;
            if (FAILED(context_->CreateBitmap(D2D1_SIZE_U{w, h}, start, image.stride, &properties, t.bitmap.put()))) {
                return nullptr;
            }
            t.rect = D2D1_RECT_F{float(x), float(y), float(x + w), float(y + h)};
            gpu->tiles.push_back(std::move(t));
        }
    }
    return gpu;
}

std::shared_ptr<GpuImage> Viewer::gpuFor(const DecodeKey& key, const std::shared_ptr<Decoded>& image) {
    if (auto it = gpuCache_.find(key); it != gpuCache_.end()) return it->second;
    auto gpu = upload(*image);
    if (!gpu) return nullptr;
    gpuCache_[key] = gpu;
    gpuOrder_.push_back(key);
    gpuBytes_ += gpu->bytes;
    // Oldest first, never what's on screen.
    while (gpuBytes_ > kGpuBudget && gpuOrder_.size() > 1) {
        auto victim = std::find_if(gpuOrder_.begin(), gpuOrder_.end(),
                                   [&](const DecodeKey& k) { return !(k == currentKey_) && !(k == key); });
        if (victim == gpuOrder_.end()) break;
        gpuBytes_ -= gpuCache_[*victim]->bytes;
        gpuCache_.erase(*victim);
        gpuOrder_.erase(victim);
    }
    return gpu;
}

void Viewer::forgetGpu(const std::wstring& path) {
    for (auto it = gpuOrder_.begin(); it != gpuOrder_.end();) {
        if (samePath(it->path, path)) {
            gpuBytes_ -= gpuCache_[*it]->bytes;
            gpuCache_.erase(*it);
            it = gpuOrder_.erase(it);
        } else {
            ++it;
        }
    }
}

bool Viewer::applyEffects(ID2D1Image* input, ComPtr<ID2D1Image>& output) {
    if (adjustments_.isIdentity() || !context_) return false;
    ComPtr<ID2D1Image> image(input);
    if (adjustments_.needsColorMatrix()) {
        if (!colorEffect_) context_->CreateEffect(CLSID_D2D1ColorMatrix, colorEffect_.put());
        if (colorEffect_) {
            // Auto correction measures each picture once.
            const std::wstring identity = current_ ? current_->info.path + L"#" + std::to_wstring(current_->frame) : L"";
            if (adjustments_.autoCorrect && current_ && levelsFor_ != identity) {
                levels_ = measureLevels(*current_);
                levelsFor_ = identity;
            }
            colorEffect_->SetInput(0, image.get());
            colorEffect_->SetValue(D2D1_COLORMATRIX_PROP_COLOR_MATRIX, colorMatrix(adjustments_, levels_));
            colorEffect_->SetValue(D2D1_COLORMATRIX_PROP_CLAMP_OUTPUT, TRUE);
            colorEffect_->GetOutput(image.put());
        }
    }
    if (adjustments_.gamma != 0) {
        if (!gammaEffect_) context_->CreateEffect(CLSID_D2D1GammaTransfer, gammaEffect_.put());
        if (gammaEffect_) {
            const float exponent = std::pow(2.0f, -adjustments_.gamma);
            gammaEffect_->SetInput(0, image.get());
            gammaEffect_->SetValue(D2D1_GAMMATRANSFER_PROP_RED_EXPONENT, exponent);
            gammaEffect_->SetValue(D2D1_GAMMATRANSFER_PROP_GREEN_EXPONENT, exponent);
            gammaEffect_->SetValue(D2D1_GAMMATRANSFER_PROP_BLUE_EXPONENT, exponent);
            gammaEffect_->SetValue(D2D1_GAMMATRANSFER_PROP_ALPHA_DISABLE, TRUE);
            gammaEffect_->GetOutput(image.put());
        }
    }
#ifdef GLANCE_HAS_SHARPEN
    if (adjustments_.sharpen > 0) {
        if (!sharpenEffect_) context_->CreateEffect(CLSID_D2D1Sharpen, sharpenEffect_.put());
        if (sharpenEffect_) {
            sharpenEffect_->SetInput(0, image.get());
            sharpenEffect_->SetValue(D2D1_SHARPEN_PROP_SHARPNESS, adjustments_.sharpen * 5.0f);
            sharpenEffect_->SetValue(D2D1_SHARPEN_PROP_THRESHOLD, 0.0f);
            sharpenEffect_->GetOutput(image.put());
        }
    }
#endif
    output = image;
    return true;
}

void Viewer::drawImage(D2D1_INTERPOLATION_MODE mode) {
    for (const auto& tile : shown_->tiles) {
        ComPtr<ID2D1Image> adjusted;
        if (applyEffects(tile.bitmap.get(), adjusted)) {
            const D2D1_POINT_2F offset{tile.rect.left, tile.rect.top};
            context_->DrawImage(adjusted.get(), &offset, nullptr, mode, D2D1_COMPOSITE_MODE_SOURCE_OVER);
            // The effects are reused for the next tile, so this one must be drawn first.
            if (shown_->tiles.size() > 1) context_->Flush();
        } else {
            context_->DrawBitmap(tile.bitmap.get(), &tile.rect, 1.0f, mode, nullptr, nullptr);
        }
    }
}

std::wstring Viewer::overlayText() const {
    const std::wstring* path = navigator_.current();
    if (!path) return {};
    const std::wstring position = std::to_wstring(navigator_.index() + 1) + L" / " + std::to_wstring(navigator_.count());
    const bool ready = current_ && samePath(current_->info.path, *path);
    if (!settings_.showInfo) {
        return fileName(*path) + L"    " + position + (ready && current_->info.isPdf ? L"    " + current_->info.pdfPages : L"");
    }

    std::wstring text = fileName(*path) + L"\n" + position;
    if (!ready) return text;
    const ImageInfo& info = current_->info;
    wchar_t line[200];
    wchar_t size[32] = L"";
    StrFormatByteSizeW(static_cast<LONGLONG>(info.fileSize), size, static_cast<UINT>(std::size(size)));
    if (info.isPdf) {
        text += L"\n" + info.pdfPages + L"\n" + std::wstring(size) + L"  ·  PDF document";
    } else {
        swprintf(line, 200, L"\n%u × %u px  (%.1f MP)", info.orientedWidth(), info.orientedHeight(),
                 double(info.width) * info.height / 1e6);
        text += line;
        text += L"\n" + std::wstring(size) + (info.format.empty() ? L"" : L"  ·  " + info.format);
        if (!info.taken.empty()) text += L"\nTaken   " + info.taken;
        if (!info.camera.empty()) text += L"\nCamera  " + info.camera;
        if (!info.lens.empty()) text += L"\nLens    " + info.lens;
        if (!info.exposure.empty()) text += L"\n" + info.exposure;
    }
    swprintf(line, 200, L"\nZoom    %d%%", static_cast<int>(std::lround(zoom_ * 100)));
    text += line;
    if (edited_ || !adjustments_.isIdentity()) text += L"  ·  edited";
    return text;
}

bool Viewer::ensureText() {
    if (!dwrite_ && FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                               reinterpret_cast<IUnknown**>(dwrite_.put())))) {
        return false;
    }
    const UINT dpi = GetDpiForWindow(hwnd_);
    if (!textFormat_ || textFormatDpi_ != dpi) {
        textFormat_.reset();
        // Monospaced so the info panel's labels line up.
        dwrite_->CreateTextFormat(L"Consolas", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                  DWRITE_FONT_STRETCH_NORMAL, 13.0f * dpi / 96.0f, L"", textFormat_.put());
        textFormatDpi_ = dpi;
    }
    return static_cast<bool>(textFormat_);
}

void Viewer::drawBusyNote() {
    if (!ensureText()) return;
    const std::wstring text = showingPdf()
        ? L"Rendering screen " + std::to_wstring(frame_ + 1) + L" of " + std::to_wstring(pdfScreens_) + L"…   Space skips ahead"
        : std::wstring(L"Loading…");
    const float scale = GetDpiForWindow(hwnd_) / 96.0f;
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dwrite_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), textFormat_.get(), 800.0f * scale,
                                         100.0f * scale, layout.put()))) {
        return;
    }
    DWRITE_TEXT_METRICS metrics{};
    layout->GetMetrics(&metrics);
    double width, height;
    viewSize(width, height);
    const float padX = 14 * scale, padY = 8 * scale, left = 20 * scale;
    const float bottom = static_cast<float>(height) - 20 * scale;
    const float top = bottom - metrics.height - padY * 2;
    const D2D1_ROUNDED_RECT box{{left, top, left + metrics.width + padX * 2, bottom}, 8 * scale, 8 * scale};
    context_->FillRoundedRectangle(&box, overlayBrush_.get());
    context_->DrawTextLayout(D2D1_POINT_2F{left + padX, top + padY}, layout.get(), brush_.get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
}

void Viewer::drawOverlay() {
    const std::wstring text = overlayText();
    if (text.empty() || !ensureText()) return;
    const float scale = GetDpiForWindow(hwnd_) / 96.0f;
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dwrite_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), textFormat_.get(), 640.0f * scale,
                                         2000.0f * scale, layout.put()))) {
        return;
    }
    DWRITE_TEXT_METRICS metrics{};
    layout->GetMetrics(&metrics);
    const float left = 20 * scale, top = 16 * scale, padX = 14 * scale, padY = 10 * scale;
    const D2D1_ROUNDED_RECT box{{left, top, left + metrics.width + padX * 2, top + metrics.height + padY * 2}, 8 * scale, 8 * scale};
    context_->FillRoundedRectangle(&box, overlayBrush_.get());
    context_->DrawTextLayout(D2D1_POINT_2F{left + padX, top + padY}, layout.get(), brush_.get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
}

void Viewer::render() {
    if (!context_ || !target_) return;
    context_->BeginDraw();
    const D2D1_MATRIX_3X2_F plain = identity();
    const D2D1_COLOR_F background = colour(settings_.background);
    context_->SetTransform(&plain);
    context_->Clear(&background);

    if (current_ && !shown_) shown_ = gpuFor(currentKey_, current_);
    if (current_ && shown_) {
        const D2D1_MATRIX_3X2_F transform = imageTransform();
        context_->SetTransform(&transform);
        // Screen pixels per bitmap pixel decides the filter: smooth when shrinking, real pixels
        // when blown right up (if Settings asks for that).
        const double scale = std::sqrt(transform._11 * transform._11 + transform._12 * transform._12);
        const D2D1_INTERPOLATION_MODE mode = (scale >= 4 && settings_.pixelatedZoom) ? D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR
                                             : scale < 0.99 ? D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC
                                                            : D2D1_INTERPOLATION_MODE_LINEAR;
        drawImage(mode);
        context_->SetTransform(&plain);
    }
    if (dragging_ && zoomBox_ && didDrag_ && brush_) {
        D2D1_RECT_F box{float(std::min(dragStart_.x, dragLast_.x)) + 0.5f, float(std::min(dragStart_.y, dragLast_.y)) + 0.5f,
                        float(std::max(dragStart_.x, dragLast_.x)) + 0.5f, float(std::max(dragStart_.y, dragLast_.y)) + 0.5f};
        context_->DrawRectangle(&box, brush_.get(), 1.0f, nullptr);
    }
    if (hasSelection_ && brush_ && darkBrush_) {
        // Crop selection: a light line over a dark one, visible on any picture.
        const D2D1_POINT_2F a = toView(selection_.left, selection_.top), b = toView(selection_.right, selection_.bottom);
        const D2D1_RECT_F box{std::round(a.x) + 0.5f, std::round(a.y) + 0.5f, std::round(b.x) - 0.5f, std::round(b.y) - 0.5f};
        context_->DrawRectangle(&box, darkBrush_.get(), 3.0f, nullptr);
        context_->DrawRectangle(&box, brush_.get(), 1.0f, nullptr);
    }
    if (settings_.showInfo || settings_.showFilename) drawOverlay();
    if (busy_ && waiting_) drawBusyNote();

    HRESULT hr = context_->EndDraw();
    if (SUCCEEDED(hr)) hr = swapChain_->Present(1, 0);
    static bool firstImagePresented = false;
    if (!firstImagePresented && shown_) {
        firstImagePresented = true;
        trace("first image presented");
    }
    if (hr == D2DERR_RECREATE_TARGET || hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        // GPU reset or driver update: rebuild everything and draw again.
        releaseGraphics();
        if (initGraphics()) InvalidateRect(hwnd_, nullptr, FALSE);
    }
}
