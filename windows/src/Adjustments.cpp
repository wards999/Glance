#include "Adjustments.h"

#include "Decoder.h"

#include <algorithm>

bool Adjustments::needsColorMatrix() const {
    return autoCorrect || brightness != 0 || contrast != 0 || saturation != 0 || cyanRed != 0 || magentaGreen != 0 ||
           yellowBlue != 0;
}

Levels measureLevels(const Decoded& image) {
    Levels levels;
    if (!image.pixels || image.width == 0 || image.height == 0) return levels;
    unsigned histogram[3][256] = {};
    // Every pixel of a screen-sized decode is a few million; a sparse sample is plenty.
    const UINT step = std::max(1u, static_cast<UINT>(static_cast<unsigned long long>(image.width) * image.height / 1'000'000));
    unsigned long long counted = 0;
    for (UINT y = 0; y < image.height; y += step) {
        const uint8_t* row = image.pixels.get() + static_cast<size_t>(y) * image.stride;
        for (UINT x = 0; x < image.width; x += step) {
            const uint8_t* px = row + static_cast<size_t>(x) * 4;  // B, G, R, A
            ++histogram[0][px[2]];
            ++histogram[1][px[1]];
            ++histogram[2][px[0]];
            ++counted;
        }
    }
    const unsigned long long clip = counted / 200;  // 0.5% at each end
    for (int c = 0; c < 3; ++c) {
        unsigned long long sum = 0;
        int low = 0, high = 255;
        for (; low < 255; ++low) {
            sum += histogram[c][low];
            if (sum > clip) break;
        }
        sum = 0;
        for (; high > 0; --high) {
            sum += histogram[c][high];
            if (sum > clip) break;
        }
        if (high - low < 16) {  // nearly flat channel: leave it alone
            low = 0;
            high = 255;
        }
        levels.low[c] = low / 255.0f;
        levels.high[c] = high / 255.0f;
    }
    return levels;
}

namespace {

/// out = m · in + offset, on RGB.
struct Affine {
    float m[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    float offset[3] = {0, 0, 0};

    /// Applies `next` after this one.
    void then(const Affine& next) {
        Affine result;
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                result.m[r][c] = next.m[r][0] * m[0][c] + next.m[r][1] * m[1][c] + next.m[r][2] * m[2][c];
            }
            result.offset[r] = next.m[r][0] * offset[0] + next.m[r][1] * offset[1] + next.m[r][2] * offset[2] + next.offset[r];
        }
        *this = result;
    }
};

}  // namespace

D2D1_MATRIX_5X4_F colorMatrix(const Adjustments& a, const Levels& levels) {
    Affine total;
    if (a.autoCorrect) {
        Affine stretch;
        for (int c = 0; c < 3; ++c) {
            const float scale = 1.0f / std::max(levels.high[c] - levels.low[c], 0.05f);
            stretch.m[c][c] = scale;
            stretch.offset[c] = -levels.low[c] * scale;
        }
        total.then(stretch);
    }
    if (a.cyanRed != 0 || a.magentaGreen != 0 || a.yellowBlue != 0) {
        Affine balance;
        balance.m[0][0] = 1 + a.cyanRed * 0.3f;
        balance.m[1][1] = 1 + a.magentaGreen * 0.3f;
        balance.m[2][2] = 1 + a.yellowBlue * 0.3f;
        total.then(balance);
    }
    if (a.brightness != 0) {
        Affine brighter;
        for (int c = 0; c < 3; ++c) brighter.offset[c] = a.brightness * 0.25f;
        total.then(brighter);
    }
    if (a.contrast != 0) {
        // Around mid-grey.
        Affine contrast;
        const float k = 1 + a.contrast * 0.5f;
        for (int c = 0; c < 3; ++c) {
            contrast.m[c][c] = k;
            contrast.offset[c] = 0.5f * (1 - k);
        }
        total.then(contrast);
    }
    if (a.saturation != 0) {
        // Mix each channel with the luminance.
        Affine saturation;
        const float s = 1 + a.saturation;
        const float luma[3] = {0.2126f, 0.7152f, 0.0722f};
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) saturation.m[r][c] = luma[c] * (1 - s) + (r == c ? s : 0);
        }
        total.then(saturation);
    }

    // Direct2D multiplies a row vector [r g b a 1] by a 5×4 matrix: M[input][output].
    D2D1_MATRIX_5X4_F matrix{};
    for (int out = 0; out < 3; ++out) {
        matrix.m[0][out] = total.m[out][0];
        matrix.m[1][out] = total.m[out][1];
        matrix.m[2][out] = total.m[out][2];
        matrix.m[4][out] = total.offset[out];
    }
    matrix.m[3][3] = 1;  // alpha unchanged
    return matrix;
}
