#include "SettingsDialog.h"

#include <commctrl.h>
#include <commdlg.h>

#include <algorithm>
#include <initializer_list>
#include <iterator>

namespace {

constexpr wchar_t kClass[] = L"GlanceSettings";
constexpr int kSlideshowSeconds[] = {1, 2, 3, 5, 10, 30};
constexpr int kCacheMB[] = {0, 512, 1024, 2048, 4096, 8192};
constexpr int kQuality[] = {100, 95, 90, 85, 80, 70};
constexpr int kHoldZoom[] = {150, 200, 250, 300, 400};

enum Id {
    IdSort = 100,
    IdDescending,
    IdLoop,
    IdWheel,
    IdEscape,
    IdSlideshow,
    IdStartFullScreen,
    IdEnlarge,
    IdBackground,
    IdInfo,
    IdFilename,
    IdPixels,
    IdPrefetch,
    IdCache,
    IdQuality,
    IdKeepAdjustments,
    IdPdfLayout,
    IdPdfRightToLeft,
    IdIncludePdfs,
    IdHoldToZoom,
    IdHoldZoom,
    IdClose,
};

HWND g_window = nullptr;
Settings* g_settings = nullptr;
std::function<void()> g_changed;
HFONT g_font = nullptr;
UINT g_dpi = 96;

int S(int value) { return MulDiv(value, static_cast<int>(g_dpi), 96); }

HWND make(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id = 0) {
    HWND control = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), g_window,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
    return control;
}

// Each column: a group box at `x`, labels right-aligned, controls after them.
struct Column {
    int x;
    int width;
    int y = 12;

    int labelX() const { return x + 12; }
    int controlX() const { return x + 172; }
    int controlWidth() const { return width - 184; }

    void group(const wchar_t* title, int rows) {
        make(L"BUTTON", title, BS_GROUPBOX, x, y, width, rows * 31 + 34);
        y += 26;
    }
    void endGroup() { y += 18; }
    void label(const wchar_t* text) { make(L"STATIC", text, SS_RIGHT, labelX(), y + 4, 150, 20); }
    void combo(int id, const wchar_t* text, std::initializer_list<const wchar_t*> items, int selected, int w = 0) {
        label(text);
        HWND box = make(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, controlX(), y,
                        w ? w : controlWidth(), 260, id);
        for (const wchar_t* item : items) SendMessageW(box, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item));
        SendMessageW(box, CB_SETCURSEL, selected, 0);
        y += 31;
    }
    void checkbox(int id, const wchar_t* text, bool checked) {
        HWND box = make(L"BUTTON", text, BS_AUTOCHECKBOX | WS_TABSTOP, controlX(), y, controlWidth(), 22, id);
        SendMessageW(box, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
        y += 31;
    }
    void button(int id, const wchar_t* labelText, const wchar_t* text) {
        label(labelText);
        make(L"BUTTON", text, BS_PUSHBUTTON | WS_TABSTOP, controlX(), y, 150, 26, id);
        y += 31;
    }
    void note(const wchar_t* text) {
        make(L"STATIC", text, 0, controlX(), y, controlWidth(), 34);
        y += 31;
    }
};

template <size_t N>
int indexOf(const int (&values)[N], int value, int fallback) {
    for (size_t i = 0; i < N; ++i) {
        if (values[i] == value) return static_cast<int>(i);
    }
    return fallback;
}

/// Lays out every control and returns the size used.
SIZE build() {
    const Settings& s = *g_settings;
    Column left{12, 520};
    left.group(L"General", 6);
    left.combo(IdSort, L"Sort images by:", {L"Name", L"Date modified", L"Date created", L"Size", L"Random"},
               static_cast<int>(s.sortOrder));
    left.checkbox(IdDescending, L"Descending order", s.descending);
    left.checkbox(IdLoop, L"Loop back to the first image at the end", s.loop);
    left.combo(IdWheel, L"Mouse wheel:", {L"Goes to the next or previous image", L"Zooms in and out"}, s.wheelNavigates ? 0 : 1);
    left.combo(IdEscape, L"Escape key:", {L"Closes Glance", L"Leaves full screen"}, s.escapeCloses ? 0 : 1);
    left.combo(IdSlideshow, L"Slideshow interval:",
               {L"1 second", L"2 seconds", L"3 seconds", L"5 seconds", L"10 seconds", L"30 seconds"},
               indexOf(kSlideshowSeconds, s.slideshowSeconds, 2));
    left.endGroup();
    left.group(L"Display", 6);
    left.checkbox(IdStartFullScreen, L"Open in full screen", s.startFullScreen);
    left.checkbox(IdEnlarge, L"Enlarge small images to fit the window", s.enlargeSmall);
    left.button(IdBackground, L"Background:", L"Choose colour...");
    left.checkbox(IdInfo, L"Show picture info (EXIF)", s.showInfo);
    left.checkbox(IdFilename, L"Show the file name", s.showFilename);
    left.checkbox(IdPixels, L"Show individual pixels above 400% zoom", s.pixelatedZoom);
    left.endGroup();

    Column right{544, 500};
    right.group(L"Performance", 3);
    right.combo(IdPrefetch, L"Decode ahead:",
                {L"Off", L"1 image", L"2 images", L"3 images", L"4 images", L"5 images", L"6 images", L"7 images", L"8 images"},
                s.prefetchCount);
    right.combo(IdCache, L"Memory cache:", {L"Automatic", L"512 MB", L"1 GB", L"2 GB", L"4 GB", L"8 GB"},
                indexOf(kCacheMB, s.cacheMB, 0));
    right.note(L"More of both makes flipping instant, at the cost of memory.");
    right.endGroup();
    right.group(L"Editing", 2);
    right.combo(IdQuality, L"JPEG save quality:", {L"100% (largest)", L"95%", L"90%", L"85%", L"80%", L"70% (smallest)"},
                indexOf(kQuality, s.saveQuality, 2));
    right.checkbox(IdKeepAdjustments, L"Keep adjustments for the next image (F4)", s.keepAdjustments);
    right.endGroup();
    right.group(L"PDF", 5);
    right.combo(IdPdfLayout, L"Page layout:", {L"Automatic", L"Single page", L"Two pages", L"Two pages with cover"}, s.pdfLayout);
    right.checkbox(IdPdfRightToLeft, L"Right to left (manga)", s.pdfRightToLeft);
    right.checkbox(IdIncludePdfs, L"Include PDFs when browsing folders", s.includePdfs);
    right.combo(IdHoldToZoom, L"Hold to zoom:", {L"Off", L"PDFs only", L"PDFs and images"}, s.holdToZoom);
    right.combo(IdHoldZoom, L"Magnification:", {L"1.5x", L"2x", L"2.5x", L"3x", L"4x"}, indexOf(kHoldZoom, s.holdZoomPercent, 2));
    right.endGroup();

    const int bottom = std::max(left.y, right.y) + 4;
    make(L"BUTTON", L"Close", BS_DEFPUSHBUTTON | WS_TABSTOP, 944, bottom, 100, 28, IdClose);
    return SIZE{1056, bottom + 42};
}

void chooseColour() {
    static COLORREF custom[16] = {};
    CHOOSECOLORW choose{sizeof(choose)};
    choose.hwndOwner = g_window;
    choose.rgbResult = g_settings->background;
    choose.lpCustColors = custom;
    choose.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (ChooseColorW(&choose)) g_settings->background = choose.rgbResult;
}

template <size_t N>
int pick(const int (&values)[N], int selection, int fallback) {
    return selection >= 0 && size_t(selection) < N ? values[selection] : fallback;
}

LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_COMMAND: {
        const int id = LOWORD(wParam);
        const int code = HIWORD(wParam);
        HWND control = reinterpret_cast<HWND>(lParam);
        if (id == IdClose || id == IDCANCEL) {
            DestroyWindow(hwnd);
            return 0;
        }
        const bool picked = code == CBN_SELCHANGE;
        const bool clicked = code == BN_CLICKED;
        if (!picked && !clicked) break;
        const int selection = picked ? static_cast<int>(SendMessageW(control, CB_GETCURSEL, 0, 0)) : -1;
        const bool checked = clicked && SendMessageW(control, BM_GETCHECK, 0, 0) == BST_CHECKED;
        Settings& s = *g_settings;
        switch (id) {
        case IdSort: s.sortOrder = static_cast<SortOrder>(selection); break;
        case IdDescending: s.descending = checked; break;
        case IdLoop: s.loop = checked; break;
        case IdWheel: s.wheelNavigates = selection == 0; break;
        case IdEscape: s.escapeCloses = selection == 0; break;
        case IdSlideshow: s.slideshowSeconds = pick(kSlideshowSeconds, selection, 3); break;
        case IdStartFullScreen: s.startFullScreen = checked; break;
        case IdEnlarge: s.enlargeSmall = checked; break;
        case IdBackground: chooseColour(); break;
        case IdInfo: s.showInfo = checked; break;
        case IdFilename: s.showFilename = checked; break;
        case IdPixels: s.pixelatedZoom = checked; break;
        case IdPrefetch: s.prefetchCount = selection; break;
        case IdCache: s.cacheMB = pick(kCacheMB, selection, 0); break;
        case IdQuality: s.saveQuality = pick(kQuality, selection, 90); break;
        case IdKeepAdjustments: s.keepAdjustments = checked; break;
        case IdPdfLayout: s.pdfLayout = selection; break;
        case IdPdfRightToLeft: s.pdfRightToLeft = checked; break;
        case IdIncludePdfs: s.includePdfs = checked; break;
        case IdHoldToZoom: s.holdToZoom = selection; break;
        case IdHoldZoom: s.holdZoomPercent = pick(kHoldZoom, selection, 250); break;
        default: return 0;
        }
        s.save();
        if (g_changed) g_changed();
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        g_window = nullptr;
        if (g_font) DeleteObject(g_font);
        g_font = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

}  // namespace

void SettingsDialog::show(HWND owner, Settings* settings, std::function<void()> onChange) {
    if (g_window) {
        SetForegroundWindow(g_window);
        return;
    }
    g_settings = settings;
    g_changed = std::move(onChange);
    g_dpi = GetDpiForWindow(owner);

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = windowProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
        wc.lpszClassName = kClass;
        RegisterClassExW(&wc);
        registered = true;
    }

    NONCLIENTMETRICSW metrics{sizeof(metrics)};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, g_dpi);
    g_font = CreateFontIndirectW(&metrics.lfMessageFont);

    const DWORD style = WS_CAPTION | WS_SYSMENU | WS_POPUP;
    const DWORD exStyle = WS_EX_CONTROLPARENT | WS_EX_DLGMODALFRAME;
    g_window = CreateWindowExW(exStyle, kClass, L"Glance Settings", style, 0, 0, 100, 100, owner, nullptr,
                               GetModuleHandleW(nullptr), nullptr);
    const SIZE size = build();

    RECT frame{0, 0, S(size.cx), S(size.cy)};
    AdjustWindowRectExForDpi(&frame, style, FALSE, exStyle, g_dpi);
    RECT ownerRect{};
    GetWindowRect(owner, &ownerRect);
    const int w = frame.right - frame.left, h = frame.bottom - frame.top;
    const int x = ownerRect.left + (ownerRect.right - ownerRect.left - w) / 2;
    const int y = ownerRect.top + (ownerRect.bottom - ownerRect.top - h) / 2;
    SetWindowPos(g_window, nullptr, x, y, w, h, SWP_NOZORDER);
    ShowWindow(g_window, SW_SHOW);
}

bool SettingsDialog::handleMessage(MSG* message) {
    return g_window && IsDialogMessageW(g_window, message);
}
