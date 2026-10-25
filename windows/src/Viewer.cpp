#include "Viewer.h"

#include <commdlg.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <thread>
#include <unordered_set>

namespace {

constexpr UINT WM_APP_DECODED = WM_APP + 1;
constexpr UINT WM_APP_FOLDER = WM_APP + 2;
constexpr wchar_t kClassName[] = L"GlanceViewer";
constexpr size_t kGpuBudget = size_t(1) << 30;  // 1 GB of decoded-ahead textures

enum Command {
    CmdOpen = 1,
    CmdNext,
    CmdPrevious,
    CmdFirst,
    CmdLast,
    CmdZoomIn,
    CmdZoomOut,
    CmdFit,
    CmdActual,
    CmdRotateRight,
    CmdRotateLeft,
    CmdFullScreen,
    CmdLoop,
    CmdExit,
};

struct FolderResult {
    std::wstring selected;  // empty: a folder was opened, show its first image
    std::vector<std::wstring> files;
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

/// `a` then `b` (Direct2D's row-vector convention).
D2D1_MATRIX_3X2_F multiply(const D2D1_MATRIX_3X2_F& a, const D2D1_MATRIX_3X2_F& b) {
    return matrix(a._11 * b._11 + a._12 * b._21, a._11 * b._12 + a._12 * b._22,
                  a._21 * b._11 + a._22 * b._21, a._21 * b._12 + a._22 * b._22,
                  a._31 * b._11 + a._32 * b._21 + b._31, a._31 * b._12 + a._32 * b._22 + b._32);
}

D2D1_MATRIX_3X2_F identity() { return matrix(1, 0, 0, 1); }

/// EXIF orientation as the transform that shows stored pixels upright (y points down).
D2D1_MATRIX_3X2_F exifMatrix(int orientation) {
    switch (orientation) {
    case 2: return matrix(-1, 0, 0, 1);   // mirrored
    case 3: return matrix(-1, 0, 0, -1);  // 180°
    case 4: return matrix(1, 0, 0, -1);   // flipped vertically
    case 5: return matrix(0, 1, 1, 0);    // transposed
    case 6: return matrix(0, 1, -1, 0);   // 90° clockwise
    case 7: return matrix(0, -1, -1, 0);  // transversed
    case 8: return matrix(0, -1, 1, 0);   // 90° counter-clockwise
    default: return identity();
    }
}

const std::unordered_set<std::wstring>& supportedExtensions() {
    // Asks WIC once; the caller's thread must have COM initialised.
    static const std::unordered_set<std::wstring> extensions = [] {
        auto list = Decoder::supportedExtensions();
        return std::unordered_set<std::wstring>(list.begin(), list.end());
    }();
    return extensions;
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

std::wstring fileName(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

}  // namespace

Viewer::Viewer(HINSTANCE instance) : instance_(instance) {}

Viewer::~Viewer() {
    pipeline_.reset();
}

// MARK: - Window

bool Viewer::create() {
    pendingDevice_ = std::async(std::launch::async, createDevice);
    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = windowProc;
    wc.hInstance = instance_;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    // Black from the very first frame, before Direct2D is ready.
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

    MEMORYSTATUSEX memory{sizeof(memory)};
    GlobalMemoryStatusEx(&memory);
    size_t costLimit = static_cast<size_t>(std::min<ULONGLONG>(memory.ullTotalPhys / 4, ULONGLONG(4) << 30));
    unsigned threads = std::clamp(std::thread::hardware_concurrency() / 4, 2u, 4u);
    pipeline_ = std::make_unique<Pipeline>(hwnd_, WM_APP_DECODED, costLimit, threads);
    trace("window and decoder threads created");
    return true;
}

void Viewer::show(int showCommand) {
    ShowWindow(hwnd_, showCommand);
    UpdateWindow(hwnd_);
    trace("window shown");
    // Finish setting up the GPU while the first image decodes on a worker thread.
    if (initGraphics() && current_) {
        shown_ = gpuFor(currentKey_, current_);
        applyZoomMode();
        render();
    }
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
            showCurrent();
        } else if (const std::wstring* now = navigator_.current(); now && samePath(*now, result->selected)) {
            // Only if you haven't opened something else meanwhile.
            navigator_.setFiles(std::move(result->files), result->selected);
            updateTitle();
            prefetchNeighbors();
        }
        return 0;
    }

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
        BeginPaint(hwnd_, &paint);
        EndPaint(hwnd_, &paint);
        render();
        return 0;
    }

    case WM_ERASEBKGND:
        if (context_) return 1;  // Direct2D paints everything; avoid a flash of the brush
        break;

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
        if (onKey(wParam, GetKeyState(VK_CONTROL) < 0)) return 0;
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
        if (wheelNavigates_ && !control) {
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
        // Drag pans a zoomed-in image; when it all fits, drag draws a rectangle to zoom into.
        zoomBox_ = !(canPanX() || canPanY());
        return 0;

    case WM_MOUSEMOVE:
        if (dragging_) {
            POINT p{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (std::abs(p.x - dragStart_.x) + std::abs(p.y - dragStart_.y) > 3) didDrag_ = true;
            if (!zoomBox_) scrollBy(dragLast_.x - p.x, dragLast_.y - p.y);
            dragLast_ = p;
            render();
        }
        return 0;

    case WM_LBUTTONUP:
        if (dragging_) {
            ReleaseCapture();
            dragging_ = false;
            if (zoomBox_ && didDrag_ && std::abs(dragLast_.x - dragStart_.x) >= 8 && std::abs(dragLast_.y - dragStart_.y) >= 8) {
                zoomToRect(dragStart_.x, dragStart_.y, dragLast_.x, dragLast_.y);
            }
            zoomBox_ = false;
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

    case WM_DESTROY:
        pipeline_.reset();  // stop the workers before the window goes away
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd_, message, wParam, lParam);
}

bool Viewer::onKey(WPARAM key, bool control) {
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
    case 'R': command(CmdRotateRight); break;
    case 'L': command(CmdRotateLeft); break;
    case 'O':
        if (!control) return false;
        command(CmdOpen);
        break;
    case VK_ESCAPE:
        if (dragging_) {
            ReleaseCapture();
            dragging_ = zoomBox_ = false;
        } else if (escapeCloses_) {
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
        title += L"  -  " + std::to_wstring(current_->info.orientedWidth()) + L" x " +
                 std::to_wstring(current_->info.orientedHeight()) + L"  -  " +
                 std::to_wstring(static_cast<int>(std::lround(zoom_ * 100))) + L"%";
    } else if (!waiting_) {
        title += L"  -  can't open this file";
    }
    title += L"  -  Glance";
    SetWindowTextW(hwnd_, title.c_str());
}

void Viewer::showContextMenu(int x, int y) {
    HMENU menu = CreatePopupMenu();
    auto add = [&](int id, const wchar_t* text, bool enabled = true, bool checked = false) {
        AppendMenuW(menu, MF_STRING | (enabled ? 0 : MF_GRAYED) | (checked ? MF_CHECKED : 0), id, text);
    };
    const bool hasImage = current_ != nullptr;
    const bool several = navigator_.count() > 1;
    add(CmdOpen, L"Open image...\tCtrl+O");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    add(CmdNext, L"Next image\tSpace", several);
    add(CmdPrevious, L"Previous image\tBackspace", several);
    add(CmdFirst, L"First image\tHome", several);
    add(CmdLast, L"Last image\tEnd", several);
    add(CmdLoop, L"Loop through folder", true, navigator_.wraps);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    add(CmdFit, L"Fit to window\t0", hasImage);
    add(CmdActual, L"Actual size (100%)\t1", hasImage);
    add(CmdZoomIn, L"Zoom in\t+", hasImage);
    add(CmdZoomOut, L"Zoom out\t-", hasImage);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    add(CmdRotateRight, L"Rotate right\tR", hasImage);
    add(CmdRotateLeft, L"Rotate left\tL", hasImage);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    add(CmdFullScreen, L"Full screen\tF11", true, fullScreen_);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    add(CmdExit, escapeCloses_ ? L"Exit\tEsc" : L"Exit");
    int chosen = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, x, y, 0, hwnd_, nullptr);
    DestroyMenu(menu);
    if (chosen) {
        command(chosen);
        render();
    }
}

void Viewer::command(int id) {
    switch (id) {
    case CmdOpen: openDialog(); break;
    case CmdNext: go(1); break;
    case CmdPrevious: go(-1); break;
    case CmdFirst:
        direction_ = 1;
        navigator_.first();
        showCurrent();
        break;
    case CmdLast:
        direction_ = -1;
        navigator_.last();
        showCurrent();
        break;
    case CmdLoop:
        navigator_.wraps = !navigator_.wraps;
        prefetchNeighbors();
        break;
    case CmdZoomIn: zoomBy(1.25); break;
    case CmdZoomOut: zoomBy(0.8); break;
    case CmdFit:
        zoomMode_ = ZoomMode::Fit;
        applyZoomMode();
        updateTitle();
        break;
    case CmdActual: {
        double width, height;
        viewSize(width, height);
        setZoom(1, width / 2, height / 2);
        break;
    }
    case CmdRotateRight:
    case CmdRotateLeft:
        if (!current_) break;
        quarterTurns_ = (quarterTurns_ + (id == CmdRotateRight ? 1 : 3)) % 4;
        centerX_ = orientedWidth() / 2;
        centerY_ = orientedHeight() / 2;
        if (zoomMode_ != ZoomMode::Custom) applyZoomMode(); else clampCenter();
        break;
    case CmdFullScreen: setFullScreen(!fullScreen_); break;
    case CmdExit: DestroyWindow(hwnd_); break;
    }
}

void Viewer::openDialog() {
    wchar_t buffer[32768] = L"";
    OPENFILENAMEW ofn{sizeof(ofn)};
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFilter = L"Images\0*.jpg;*.jpeg;*.jpe;*.png;*.gif;*.bmp;*.tif;*.tiff;*.webp;*.heic;*.heif;*.avif;*.jxr;*.ico;*.dng;*.cr2;*.cr3;*.nef;*.arw;*.orf;*.rw2\0All files\0*.*\0";
    ofn.lpstrFile = buffer;
    ofn.nMaxFile = static_cast<DWORD>(std::size(buffer));
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (GetOpenFileNameW(&ofn)) open({buffer});
}

// MARK: - Opening and navigation

void Viewer::open(const std::vector<std::wstring>& paths) {
    if (paths.empty()) return;
    DWORD attributes = GetFileAttributesW(paths[0].c_str());
    if (paths.size() == 1 && attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        scanFolder(paths[0], L"");
    } else if (paths.size() == 1) {
        // Show the file first; list its folder in the background.
        navigator_.setFiles({paths[0]}, paths[0]);
        showCurrent();
        scanFolder(Navigator::folderOf(paths[0]), paths[0]);
    } else {
        navigator_.setFiles(paths, paths[0]);
        showCurrent();
    }
}

void Viewer::scanFolder(const std::wstring& folder, const std::wstring& selected) {
    HWND hwnd = hwnd_;
    std::thread([hwnd, folder, selected] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        auto files = Navigator::scan(folder, supportedExtensions());
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
    if (!navigator_.move(delta)) {
        MessageBeep(MB_OK);
        return;
    }
    showCurrent();
}

void Viewer::showCurrent() {
    const std::wstring* path = navigator_.current();
    if (waiting_) pipeline_->drop(wantedKey_);
    if (!path) {
        waiting_ = false;
        current_.reset();
        shown_.reset();
        updateTitle();
        render();
        return;
    }
    DecodeKey key{*path, 0, openingRequest()};
    wantedKey_ = key;
    levelKey_ = {};
    if (auto hit = pipeline_->cached(key)) {
        // Decoded ahead: on screen in the same message as the key press.
        waiting_ = false;
        show(key, hit, true);
        prefetchNeighbors();
        return;
    }
    // Keep the previous picture up until the new one is ready.
    waiting_ = true;
    pipeline_->load(key);
    updateTitle();
}

void Viewer::show(const DecodeKey& key, const std::shared_ptr<Decoded>& image, bool resetView) {
    current_ = image;
    currentKey_ = key;
    shown_ = context_ ? gpuFor(key, image) : nullptr;
    if (resetView) {
        quarterTurns_ = 0;
        zoomMode_ = ZoomMode::Fit;
        requestedLevel_ = 0;
        applyZoomMode();
    } else {
        clampCenter();
    }
    updateTitle();
    render();
    requestResolutionIfNeeded();
}

void Viewer::onDecoded(std::unique_ptr<DecodeResult> result) {
    if (waiting_ && result->key == wantedKey_) {
        trace("decode delivered");
        waiting_ = false;
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
    if (current_ && result->key == levelKey_ && result->image->longSide() > current_->longSide()) {
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
    DecodeRequest request = openingRequest();
    // Mostly in the direction of travel, plus one behind.
    for (int offset : {direction_, 2 * direction_, -direction_, 3 * direction_}) {
        const std::wstring* path = navigator_.at(offset);
        const std::wstring* now = navigator_.current();
        if (!path || (now && samePath(*path, *now))) continue;
        DecodeKey key{*path, 0, request};
        if (std::find(keys.begin(), keys.end(), key) == keys.end()) keys.push_back(key);
    }
    prefetchKeys_ = keys;
    pipeline_->prefetch(keys);
}

void Viewer::requestResolutionIfNeeded() {
    if (!current_) return;
    const UINT full = current_->info.longSide();
    const UINT have = current_->longSide();
    if (have >= full) return;
    const double needed = full * std::min(zoom_, 1.0);
    if (needed <= have * 1.02) return;
    UINT level = Decoder::halvingLevel(full, static_cast<UINT>(std::ceil(needed)));
    if (level <= requestedLevel_ || level <= have) return;
    requestedLevel_ = level;
    levelKey_ = DecodeKey{current_->info.path, current_->frame, DecodeRequest{0, 0, level}};
    pipeline_->load(levelKey_);
}

// MARK: - Geometry

void Viewer::viewSize(double& width, double& height) const {
    RECT client{};
    GetClientRect(hwnd_, &client);
    width = std::max<LONG>(client.right - client.left, 1);
    height = std::max<LONG>(client.bottom - client.top, 1);
}

D2D1_MATRIX_3X2_F Viewer::orientation() const {
    D2D1_MATRIX_3X2_F m = exifMatrix(current_ ? current_->info.orientation : 1);
    for (int i = 0; i < quarterTurns_; ++i) m = multiply(m, matrix(0, 1, -1, 0));
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
    return enlargeSmallImages_ ? fit : std::min(fit, 1.0);
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
    auto toImageX = [&](double x) { return centerX_ + (x - width / 2) / zoom_; };
    auto toImageY = [&](double y) { return centerY_ + (y - height / 2) / zoom_; };
    const double left = toImageX(std::min(x0, x1)), right = toImageX(std::max(x0, x1));
    const double top = toImageY(std::min(y0, y1)), bottom = toImageY(std::max(y0, y1));
    zoom_ = std::clamp(std::min(width / (right - left), height / (bottom - top)), 0.01, 32.0);
    zoomMode_ = ZoomMode::Custom;
    centerX_ = (left + right) / 2;
    centerY_ = (top + bottom) / 2;
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
    m = multiply(m, matrix(float(scale), 0, 0, float(scale)));
    m = multiply(m, orientation());
    return multiply(m, matrix(1, 0, 0, 1, float(cx), float(cy)));
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
    context_->CreateSolidColorBrush(&white, nullptr, brush_.put());
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

void Viewer::render() {
    if (!context_ || !target_) return;
    context_->BeginDraw();
    const D2D1_MATRIX_3X2_F plain = identity();
    const D2D1_COLOR_F black{0, 0, 0, 1};
    context_->SetTransform(&plain);
    context_->Clear(&black);

    if (current_ && !shown_) shown_ = gpuFor(currentKey_, current_);
    if (current_ && shown_) {
        const D2D1_MATRIX_3X2_F transform = imageTransform();
        context_->SetTransform(&transform);
        // Screen pixels per bitmap pixel decides the filter: smooth when shrinking, sharp
        // pixels when blown right up.
        const double scale = std::sqrt(transform._11 * transform._11 + transform._12 * transform._12);
        const D2D1_INTERPOLATION_MODE mode = scale >= 4 ? D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR
                                             : scale < 0.99 ? D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC
                                                            : D2D1_INTERPOLATION_MODE_LINEAR;
        for (const auto& tile : shown_->tiles) {
            context_->DrawBitmap(tile.bitmap.get(), &tile.rect, 1.0f, mode, nullptr, nullptr);
        }
        context_->SetTransform(&plain);
    }
    if (dragging_ && zoomBox_ && didDrag_ && brush_) {
        D2D1_RECT_F box{float(std::min(dragStart_.x, dragLast_.x)) + 0.5f, float(std::min(dragStart_.y, dragLast_.y)) + 0.5f,
                        float(std::max(dragStart_.x, dragLast_.x)) + 0.5f, float(std::max(dragStart_.y, dragLast_.y)) + 0.5f};
        context_->DrawRectangle(&box, brush_.get(), 1.0f, nullptr);
    }

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
