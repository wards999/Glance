#include "AdjustmentsDialog.h"
#include "SettingsDialog.h"
#include "Viewer.h"

#include <shellapi.h>

#include <iterator>
#include <string>
#include <vector>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    trace("---- start");
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    std::vector<std::wstring> paths;
    int argc = 0;
    if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        for (int i = 1; i < argc; ++i) {
            if (argv[i][0] == L'-') continue;
            wchar_t full[MAX_PATH * 8];
            DWORD length = GetFullPathNameW(argv[i], static_cast<DWORD>(std::size(full)), full, nullptr);
            paths.push_back(length ? std::wstring(full, length) : std::wstring(argv[i]));
        }
        LocalFree(argv);
    }

    int exitCode = 0;
    {
        Viewer viewer(instance);
        if (!viewer.create()) return 1;
        // Order matters for speed: decoding starts first, then the window appears, then the GPU
        // is set up while the decode is still running.
        viewer.open(paths);
        viewer.show(showCommand);
        // Started without a file: ask for one, and if you'd rather not, there's nothing to show.
        if (paths.empty() && !viewer.openDialog()) viewer.close();

        MSG message;
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (SettingsDialog::handleMessage(&message) || AdjustmentsDialog::handleMessage(&message)) continue;
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        exitCode = static_cast<int>(message.wParam);
    }
    CoUninitialize();
    return exitCode;
}
