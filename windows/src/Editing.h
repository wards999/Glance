#pragma once

#include "Decoder.h"

#include <d2d1_1.h>

#include <memory>
#include <string>

namespace Editing {

/// EXIF orientation as the transform that shows stored pixels upright (y points down).
D2D1_MATRIX_3X2_F exifMatrix(int orientation);
/// `a` then `b`, in Direct2D's row-vector convention.
D2D1_MATRIX_3X2_F multiply(const D2D1_MATRIX_3X2_F& a, const D2D1_MATRIX_3X2_F& b);

/// The pixels turned by `orientation` (quarter turns and flips), so they're upright.
std::shared_ptr<Decoded> orient(const Decoded& source, const D2D1_MATRIX_3X2_F& orientation);

/// A copy of `area` (clamped to the picture).
std::shared_ptr<Decoded> crop(const Decoded& source, RECT area);

/// The EXIF orientation that shows the stored pixels as `current` and then `transform`.
int composeOrientation(int current, const D2D1_MATRIX_3X2_F& transform);

/// Rotates or flips a file without re-encoding it, by rewriting only its orientation tag in
/// place. False if the file has no orientation tag to rewrite (no room for one).
bool rewriteOrientation(const std::wstring& path, int orientation);

/// Encodes upright pixels. When `metadataFrom` is the same format, its EXIF and other metadata
/// come along, with the orientation reset to "up" since the pixels already are.
/// Writes to a temporary file and swaps it in, so a failed save never leaves a broken file.
bool save(const Decoded& upright, const std::wstring& path, const GUID& container, int quality,
          const std::wstring& metadataFrom);

}  // namespace Editing
