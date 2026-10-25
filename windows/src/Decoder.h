#pragma once

#include "Common.h"

#include <wincodec.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

/// What we know about one frame of an image file.
struct ImageInfo {
    std::wstring path;
    UINT width = 0;   // stored pixels, before EXIF orientation
    UINT height = 0;
    int orientation = 1;  // EXIF 1–8
    UINT frameCount = 1;

    // For the info overlay. Read alongside the pixels; it costs well under a millisecond.
    unsigned long long fileSize = 0;
    std::wstring format;    // "JPEG", "PNG", …
    std::wstring camera;
    std::wstring lens;
    std::wstring taken;     // "2024-05-01 10:22"
    std::wstring exposure;  // "1/250 s   f/2.8   ISO 100   35 mm"

    UINT longSide() const { return width > height ? width : height; }
    bool swapsAxes() const { return orientation >= 5 && orientation <= 8; }
    UINT orientedWidth() const { return swapsAxes() ? height : width; }
    UINT orientedHeight() const { return swapsAxes() ? width : height; }
};

/// How big a decode to make. The image's size isn't known until its header is read, so the
/// opening request describes the view ("fit in this box") and the decoder works out the pixels.
struct DecodeRequest {
    UINT boxWidth = 0;   // fit in this box (device pixels)…
    UINT boxHeight = 0;
    UINT longSide = 0;   // …or this long side; 0 with no box means full size

    bool operator==(const DecodeRequest&) const = default;
};

/// Decoded pixels: 32-bit premultiplied BGRA, the format Direct2D uploads without converting.
/// Kept in the file's stored orientation; EXIF rotation is applied on the GPU when drawn.
struct Decoded {
    ImageInfo info;
    UINT frame = 0;
    UINT width = 0;
    UINT height = 0;
    UINT stride = 0;
    std::unique_ptr<uint8_t[]> pixels;

    size_t bytes() const { return static_cast<size_t>(stride) * height; }
    UINT longSide() const { return width > height ? width : height; }
};

/// Wraps WIC. Use one per thread.
class Decoder {
public:
    Decoder();

    /// Null if the file can't be read.
    std::shared_ptr<Decoded> decode(const std::wstring& path, UINT frame, const DecodeRequest& request);

    /// Lower-case extensions (no dot) that the installed WIC codecs can decode, including
    /// Store extensions such as HEIF, WebP, AVIF and RAW when they're installed.
    static std::vector<std::wstring> supportedExtensions();

    /// The decode's long side: the smallest of full, full/2, full/4, full/8 that still covers
    /// `needed`. JPEG decodes those sizes directly (DCT scaling), skipping most of the work.
    static UINT halvingLevel(UINT full, UINT needed);

private:
    ComPtr<IWICImagingFactory> factory_;
};
