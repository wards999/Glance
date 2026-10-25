#pragma once

#include "Settings.h"

#include <functional>

/// The Settings window. Every control applies and saves immediately; there's no OK or Apply.
class SettingsDialog {
public:
    static void show(HWND owner, Settings* settings, std::function<void()> onChange);
    /// For the message loop: keyboard navigation (Tab, Esc) inside the dialog.
    static bool handleMessage(MSG* message);
};
