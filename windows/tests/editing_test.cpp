// Checks the orientation maths behind lossless rotate, which writes the result into the user's
// own file. Run: glance-tests.exe (prints FAIL lines and exits 1 on any mismatch).

#include "Editing.h"

#include <cstdio>
#include <cstring>

namespace {

int failures = 0;

void check(bool ok, const char* what, int a, int b) {
    if (!ok) {
        std::printf("FAIL: %s (%d, %d)\n", what, a, b);
        ++failures;
    }
}

/// A 3 x 2 picture whose every pixel is different, so any wrong turn or flip shows.
Decoded sample() {
    Decoded image;
    image.width = 3;
    image.height = 2;
    image.stride = image.width * 4;
    image.pixels.reset(new uint8_t[image.bytes()]);
    for (size_t i = 0; i < image.bytes(); ++i) image.pixels[i] = static_cast<uint8_t>(i);
    return image;
}

bool same(const Decoded& a, const Decoded& b) {
    return a.width == b.width && a.height == b.height && std::memcmp(a.pixels.get(), b.pixels.get(), a.bytes()) == 0;
}

D2D1_MATRIX_3X2_F matrix(float m11, float m12, float m21, float m22) {
    D2D1_MATRIX_3X2_F m{};
    m._11 = m11;
    m._12 = m12;
    m._21 = m21;
    m._22 = m22;
    return m;
}

}  // namespace

int wmain() {
    const Decoded stored = sample();

    // EXIF 6 is "turn 90 degrees clockwise to show upright": the stored top-left pixel ends up
    // top-right, and the picture becomes 2 wide and 3 tall.
    auto upright = Editing::orient(stored, Editing::exifMatrix(6));
    check(upright && upright->width == 2 && upright->height == 3, "EXIF 6 size", 0, 0);
    if (upright) check(std::memcmp(upright->pixels.get() + 4, stored.pixels.get(), 4) == 0, "EXIF 6 corner", 0, 0);

    // The five lossless turns from Viewer::losslessTransform.
    const D2D1_MATRIX_3X2_F turns[] = {matrix(0, 1, -1, 0), matrix(0, -1, 1, 0), matrix(-1, 0, 0, -1),
                                       matrix(-1, 0, 0, 1), matrix(1, 0, 0, -1)};
    for (int current = 1; current <= 8; ++current) {
        for (int t = 0; t < 5; ++t) {
            // Showing the file as it is and then turning it must look exactly like showing it with
            // the new tag that lossless rotate writes.
            auto shown = Editing::orient(stored, Editing::exifMatrix(current));
            auto expected = shown ? Editing::orient(*shown, turns[t]) : nullptr;
            const int written = Editing::composeOrientation(current, turns[t]);
            auto actual = Editing::orient(stored, Editing::exifMatrix(written));
            check(expected && actual && same(*expected, *actual), "turn then show", current, t);
        }
    }

    std::printf(failures ? "%d failed\n" : "All orientation checks passed\n", failures);
    return failures ? 1 : 0;
}
