#include "AdjustmentsDialog.h"

#include <commctrl.h>

#include <iterator>

namespace {

constexpr wchar_t kClass[] = L"GlanceAdjustments";

struct Slider {
    const wchar_t* label;
    float Adjustments::*value;
    int minimum;  // percent
};

const Slider kSliders[] = {
    {L"Brightness", &Adjustments::brightness, -100},
    {L"Contrast", &Adjustments::contrast, -100},
    {L"Saturation", &Adjustments::saturation, -100},
    {L"Gamma", &Adjustments::gamma, -100},
    {L"Sharpen", &Adjustments::sharpen, 0},
    {L"Cyan / Red", &Adjustments::cyanRed, -100},
    {L"Magenta / Green", &Adjustments::magentaGreen, -100},
    {L"Yellow / Blue", &Adjustments::yellowBlue, -100},
};

constexpr int kFirstSlider = 100;
constexpr int kAuto = 200;
constexpr int kReset = 201;
constexpr int kClose = 202;

HWND g_window = nullptr;
HWND g_sliders[std::size(kSliders)] = {};
HWND g_auto = nullptr;
Adjustments g_adjustments;
AdjustmentsDialog::Changed g_changed;
HFONT g_font = nullptr;
UINT g_dpi = 96;
bool g_updating = false;

int S(int value) { return MulDiv(value, static_cast<int>(g_dpi), 96); }

HWND make(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id = 0) {
    HWND control = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), g_window,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
    return control;
}

void refresh() {
    g_updating = true;
    for (size_t i = 0; i < std::size(kSliders); ++i) {
        const int position = static_cast<int>(g_adjustments.*kSliders[i].value * 100);
        SendMessageW(g_sliders[i], TBM_SETPOS, TRUE, position);
    }
    SendMessageW(g_auto, BM_SETCHECK, g_adjustments.autoCorrect ? BST_CHECKED : BST_UNCHECKED, 0);
    g_updating = false;
}

void changed() {
    if (!g_updating && g_changed) g_changed(g_adjustments);
}

LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_HSCROLL: {
        HWND slider = reinterpret_cast<HWND>(lParam);
        for (size_t i = 0; i < std::size(kSliders); ++i) {
            if (g_sliders[i] != slider) continue;
            int position = static_cast<int>(SendMessageW(slider, TBM_GETPOS, 0, 0));
            if (position > -3 && position < 3) position = 0;  // easy to land back on "unchanged"
            g_adjustments.*kSliders[i].value = position / 100.0f;
            changed();
        }
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case kAuto:
            g_adjustments.autoCorrect = SendMessageW(g_auto, BM_GETCHECK, 0, 0) == BST_CHECKED;
            changed();
            return 0;
        case kReset:
            g_adjustments = Adjustments{};
            refresh();
            changed();
            return 0;
        case kClose:
        case IDCANCEL:
            DestroyWindow(hwnd);
            return 0;
        }
        break;
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

void AdjustmentsDialog::show(HWND owner, const Adjustments& current, Changed onChange) {
    g_adjustments = current;
    g_changed = std::move(onChange);
    if (g_window) {
        refresh();
        SetForegroundWindow(g_window);
        return;
    }
    static bool registered = false;
    if (!registered) {
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_BAR_CLASSES};
        InitCommonControlsEx(&controls);
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = windowProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
        wc.lpszClassName = kClass;
        RegisterClassExW(&wc);
        registered = true;
    }
    g_dpi = GetDpiForWindow(owner);
    NONCLIENTMETRICSW metrics{sizeof(metrics)};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, g_dpi);
    g_font = CreateFontIndirectW(&metrics.lfMessageFont);

    const DWORD style = WS_CAPTION | WS_SYSMENU | WS_POPUP;
    const DWORD exStyle = WS_EX_CONTROLPARENT | WS_EX_TOOLWINDOW;
    g_window = CreateWindowExW(exStyle, kClass, L"Adjustments", style, 0, 0, 100, 100, owner, nullptr,
                               GetModuleHandleW(nullptr), nullptr);
    int y = 14;
    for (size_t i = 0; i < std::size(kSliders); ++i) {
        make(L"STATIC", kSliders[i].label, SS_RIGHT, 10, y + 6, 110, 20);
        g_sliders[i] = make(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, 126, y, 260, 30, kFirstSlider + int(i));
        SendMessageW(g_sliders[i], TBM_SETRANGE, TRUE, MAKELPARAM(kSliders[i].minimum, 100));
        y += 36;
    }
    g_auto = make(L"BUTTON", L"Auto correction (F5)", BS_AUTOCHECKBOX | WS_TABSTOP, 126, y + 4, 200, 22, kAuto);
    y += 38;
    make(L"BUTTON", L"Reset", BS_PUSHBUTTON | WS_TABSTOP, 126, y, 90, 28, kReset);
    make(L"BUTTON", L"Close", BS_DEFPUSHBUTTON | WS_TABSTOP, 296, y, 90, 28, kClose);
    y += 44;
    refresh();

    RECT frame{0, 0, S(400), S(y)};
    AdjustWindowRectExForDpi(&frame, style, FALSE, exStyle, g_dpi);
    RECT ownerRect{};
    GetWindowRect(owner, &ownerRect);
    // Top right of the viewer, out of the way of the picture's middle.
    SetWindowPos(g_window, nullptr, ownerRect.right - (frame.right - frame.left) - S(24), ownerRect.top + S(60),
                 frame.right - frame.left, frame.bottom - frame.top, SWP_NOZORDER);
    ShowWindow(g_window, SW_SHOWNOACTIVATE);
}

void AdjustmentsDialog::update(const Adjustments& adjustments) {
    g_adjustments = adjustments;
    if (g_window) refresh();
}

bool AdjustmentsDialog::handleMessage(MSG* message) {
    return g_window && IsDialogMessageW(g_window, message);
}
