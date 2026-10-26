#include "PasswordDialog.h"

namespace PasswordDialog {

namespace {

constexpr wchar_t kClass[] = L"GlancePassword";
constexpr int kEdit = 100;

HWND g_edit = nullptr;
bool g_done = false;
bool g_accepted = false;
std::wstring g_password;

LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK) {
            const int length = GetWindowTextLengthW(g_edit);
            g_password.assign(static_cast<size_t>(length) + 1, L'\0');
            GetWindowTextW(g_edit, g_password.data(), length + 1);
            g_password.resize(static_cast<size_t>(length));
            g_accepted = true;
            g_done = true;
            return 0;
        }
        if (LOWORD(wParam) == IDCANCEL) {
            g_done = true;
            return 0;
        }
        break;
    case WM_CLOSE:
        g_done = true;
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

}  // namespace

std::optional<std::wstring> ask(HWND owner, const std::wstring& fileName, bool retry) {
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
    const UINT dpi = GetDpiForWindow(owner);
    auto S = [dpi](int v) { return MulDiv(v, static_cast<int>(dpi), 96); };
    NONCLIENTMETRICSW metrics{sizeof(metrics)};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi);
    HFONT font = CreateFontIndirectW(&metrics.lfMessageFont);

    const DWORD style = WS_CAPTION | WS_SYSMENU | WS_POPUP;
    const DWORD exStyle = WS_EX_CONTROLPARENT | WS_EX_DLGMODALFRAME;
    HWND window = CreateWindowExW(exStyle, kClass, L"Password required", style, 0, 0, 100, 100, owner, nullptr,
                                  GetModuleHandleW(nullptr), nullptr);
    auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD controlStyle, int x, int y, int w, int h, int id) {
        HWND control = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | controlStyle, S(x), S(y), S(w), S(h), window,
                                       reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        return control;
    };
    const std::wstring prompt = (retry ? L"That password didn't work. " : L"") + std::wstring(L"“") + fileName +
                                L"” is password protected. Enter its password:";
    make(L"STATIC", prompt.c_str(), 0, 16, 14, 368, 40, 0);
    g_edit = make(L"EDIT", L"", ES_PASSWORD | ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 16, 60, 368, 26, kEdit);
    make(L"BUTTON", L"Open", BS_DEFPUSHBUTTON | WS_TABSTOP, 184, 100, 96, 28, IDOK);
    make(L"BUTTON", L"Cancel", BS_PUSHBUTTON | WS_TABSTOP, 288, 100, 96, 28, IDCANCEL);

    RECT frame{0, 0, S(400), S(142)};
    AdjustWindowRectExForDpi(&frame, style, FALSE, exStyle, dpi);
    RECT ownerRect{};
    GetWindowRect(owner, &ownerRect);
    const int w = frame.right - frame.left, h = frame.bottom - frame.top;
    SetWindowPos(window, nullptr, ownerRect.left + (ownerRect.right - ownerRect.left - w) / 2,
                 ownerRect.top + (ownerRect.bottom - ownerRect.top - h) / 2, w, h, SWP_NOZORDER);

    // Modal: the viewer is disabled until this is answered.
    EnableWindow(owner, FALSE);
    ShowWindow(window, SW_SHOW);
    SetFocus(g_edit);
    g_done = g_accepted = false;
    g_password.clear();
    MSG message;
    while (!g_done) {
        const BOOL got = GetMessageW(&message, nullptr, 0, 0);
        if (got <= 0) {
            if (got == 0) PostQuitMessage(static_cast<int>(message.wParam));  // let the main loop see it
            break;
        }
        if (IsDialogMessageW(window, &message)) continue;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    EnableWindow(owner, TRUE);
    DestroyWindow(window);
    DeleteObject(font);
    SetForegroundWindow(owner);
    if (!g_accepted) return std::nullopt;
    std::wstring result = std::move(g_password);
    SecureZeroMemory(g_password.data(), g_password.size() * sizeof(wchar_t));
    return result;
}

}  // namespace PasswordDialog
