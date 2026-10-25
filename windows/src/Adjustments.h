#pragma once

#include <d2d1_1.h>

#include <cstdint>

struct Decoded;

/// Brightness, colour balance and so on; zero is "unchanged" for every value.
/// Applied on the GPU with Direct2D effects, so changing them costs nothing per frame.
struct Adjustments {
    float brightness = 0;    // -1…1
    float contrast = 0;      // -1…1
    float saturation = 0;    // -1…1
    float gamma = 0;         // -1…1, positive lifts the midtones
    float sharpen = 0;       // 0…1
    float cyanRed = 0;       // -1…1
    float magentaGreen = 0;  // -1…1
    float yellowBlue = 0;    // -1…1
    bool autoCorrect = false;

    bool operator==(const Adjustments&) const = default;
    bool isIdentity() const { return *this == Adjustments{}; }
    bool needsColorMatrix() const;
};

/// Auto correction's measurement of a picture: where each channel's darkest and brightest
/// 0.5% lie, so they can be stretched to full range (which also removes colour casts).
struct Levels {
    float low[3] = {0, 0, 0};
    float high[3] = {1, 1, 1};
};

Levels measureLevels(const Decoded& image);

/// Auto levels, colour balance, brightness, contrast and saturation folded into one
/// colour matrix, so they cost a single GPU pass together.
D2D1_MATRIX_5X4_F colorMatrix(const Adjustments& adjustments, const Levels& levels);
