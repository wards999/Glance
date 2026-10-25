#pragma once

#include "Adjustments.h"

#include <windows.h>

#include <functional>

/// Sliders for brightness, contrast, colour balance and so on. Changes show immediately.
class AdjustmentsDialog {
public:
    using Changed = std::function<void(const Adjustments&)>;
    static void show(HWND owner, const Adjustments& current, Changed onChange);
    /// Reflects adjustments changed elsewhere (a new image, F5) without reporting a change.
    static void update(const Adjustments& adjustments);
    static bool handleMessage(MSG* message);
};
