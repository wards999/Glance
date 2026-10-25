#include "SettingsDialog.h"

#include <commctrl.h>
#include <commdlg.h>

#include <initializer_list>
#include <iterator>

namespace {

constexpr wchar_t kClass[] = L"GlanceSettings";
constexpr int kSlideshowSeconds[] = {1, 2, 3, 5, 10, 30};
constexpr int kCacheMB[] = {0, 512, 1024, 2048, 4096, 8192};

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

void label(const wchar_t* text, int y) { make(L"STATIC", text, SS_RIGHT, 24, y + 4, 170, 20); }

void combo(int id, int y, int width, std::initializer_list<const wchar_t*> items, int selected) {
    HWND box = make(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 204, y, width, 240, id);
    for (const wchar_t* item : items) SendMessageW(box, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item));
    SendMessageW(box, CB_SETCURSEL, selected, 0);
}

void checkbox(int id, const wchar_t* text, int x, int y, int width, bool checked) {
    HWND box = make(L"BUTTON", text, BS_AUTOCHECKBOX | WS_TABSTOP, x, y, width, 22, id);
    SendMessageW(box, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
}

template <size_t N>
int indexOf(const int (&values)[N], int value, int fallback) {
    for (size_t i = 0; i < N; ++i) {
        if (values[i] == value) return static_cast<int>(i);
    }
    return fallback;
}

/// Lays out every control and returns the height used.
int build() {
    const Settings& s = *g_settings;
    int y = 12;
    make(L"BUTTON", L"General", BS_GROUPBOX, 12, y, 536, 196);
    y += 26;
    label(L"Sort images by:", y);
    combo(IdSort, y, 170, {L"Name", L"Date modified", L"Date created", L"Size", L"Random"}, static_cast<int>(s.sortOrder));
    checkbox(IdDescending, L"Descending", 390, y + 2, 140, s.descending);
    y += 32;
    checkbox(IdLoop, L"Loop back to the first image at the end of a folder", 204, y, 330, s.loop);
    y += 30;
    label(L"Mouse wheel:", y);
    combo(IdWheel, y, 330, {L"Goes to the next or previous image", L"Zooms in and out"}, s.wheelNavigates ? 0 : 1);
    y += 32;
    label(L"Escape key:", y);
    combo(IdEscape, y, 330, {L"Closes Glance", L"Leaves full screen"}, s.escapeCloses ? 0 : 1);
    y += 32;
    label(L"Slideshow interval:", y);
    combo(IdSlideshow, y, 170, {L"1 second", L"2 seconds", L"3 seconds", L"5 seconds", L"10 seconds", L"30 seconds"},
          indexOf(kSlideshowSeconds, s.slideshowSeconds, 2));
    y += 50;

    make(L"BUTTON", L"Display", BS_GROUPBOX, 12, y, 536, 196);
    y += 26;
    checkbox(IdStartFullScreen, L"Open in full screen", 204, y, 330, s.startFullScreen);
    y += 28;
    checkbox(IdEnlarge, L"Enlarge small images to fit the window", 204, y, 330, s.enlargeSmall);
    y += 30;
    label(L"Background:", y);
    make(L"BUTTON", L"Choose colour...", BS_PUSHBUTTON | WS_TABSTOP, 204, y, 140, 26, IdBackground);
    y += 34;
    checkbox(IdInfo, L"Show picture info (EXIF)", 204, y, 330, s.showInfo);
    y += 28;
    checkbox(IdFilename, L"Show the file name", 204, y, 330, s.showFilename);
    y += 28;
    checkbox(IdPixels, L"Show individual pixels above 400% zoom", 204, y, 330, s.pixelatedZoom);
    y += 44;

    make(L"BUTTON", L"Performance", BS_GROUPBOX, 12, y, 536, 112);
    y += 26;
    label(L"Decode ahead:", y);
    combo(IdPrefetch, y, 170, {L"Off", L"1 image", L"2 images", L"3 images", L"4 images", L"5 images", L"6 images", L"7 images", L"8 images"},
          s.prefetchCount);
    y += 32;
    label(L"Memory cache:", y);
    combo(IdCache, y, 170, {L"Automatic", L"512 MB", L"1 GB", L"2 GB", L"4 GB", L"8 GB"}, indexOf(kCacheMB, s.cacheMB, 0));
    y += 30;
    make(L"STATIC", L"More of both makes flipping instant, at the cost of memory.", 0, 204, y, 330, 20);
    y += 40;

    make(L"BUTTON", L"Close", BS_DEFPUSHBUTTON | WS_TABSTOP, 448, y, 100, 28, IdClose);
    return y + 40;
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
        case IdSlideshow: s.slideshowSeconds = kSlideshowSeconds[std::size(kSlideshowSeconds) > size_t(selection) ? selection : 2]; break;
        case IdStartFullScreen: s.startFullScreen = checked; break;
        case IdEnlarge: s.enlargeSmall = checked; break;
        case IdBackground: chooseColour(); break;
        case IdInfo: s.showInfo = checked; break;
        case IdFilename: s.showFilename = checked; break;
        case IdPixels: s.pixelatedZoom = checked; break;
        case IdPrefetch: s.prefetchCount = selection; break;
        case IdCache: s.cacheMB = kCacheMB[std::size(kCacheMB) > size_t(selection) ? selection : 0]; break;
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
    g_window = CreateWindowExW(WS_EX_CONTROLPARENT | WS_EX_DLGMODALFRAME, kClass, L"Glance Settings", style, 0, 0, 100,
                               100, owner, nullptr, GetModuleHandleW(nullptr), nullptr);
    const int height = build();

    RECT frame{0, 0, S(560), S(height)};
    AdjustWindowRectExForDpi(&frame, style, FALSE, WS_EX_CONTROLPARENT | WS_EX_DLGMODALFRAME, g_dpi);
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
