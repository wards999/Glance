#include "Decoder.h"

#include <propidl.h>

#include <algorithm>
#include <cmath>
#include <new>

namespace {

UINT ceilDiv(UINT value, UINT divisor) { return (value + divisor - 1) / divisor; }

/// Has the codec decode straight to `width`×`height` in a pixel format we can turn into BGRA
/// ourselves. For JPEG this is DCT scaling, the cheap way to get 1/2, 1/4, 1/8 size; it also
/// skips WIC's separate resize and format-conversion passes. False if the codec can't.
bool decodeNative(IWICBitmapFrameDecode* frame, UINT width, UINT height, Decoded& out) {
    ComPtr<IWICBitmapSourceTransform> transform;
    if (FAILED(frame->QueryInterface(__uuidof(IWICBitmapSourceTransform), reinterpret_cast<void**>(transform.put())))) {
        return false;
    }
    UINT w = width, h = height;
    if (FAILED(transform->GetClosestSize(&w, &h)) || w != width || h != height) return false;

    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGR;
    if (FAILED(transform->GetClosestPixelFormat(&format))) return false;
    UINT sourceBytes = 0;
    if (format == GUID_WICPixelFormat32bppBGR) sourceBytes = 4;
    else if (format == GUID_WICPixelFormat24bppBGR) sourceBytes = 3;
    else if (format == GUID_WICPixelFormat8bppGray) sourceBytes = 1;
    else return false;  // CMYK and friends: let WIC's converter handle them

    const UINT stride = width * 4;
    const size_t total = size_t(stride) * height;
    const size_t sourceStride = size_t(width) * sourceBytes;
    if (total > 0xFFFFFFFFull) return false;  // CopyPixels takes a 32-bit size
    out.pixels.reset(new (std::nothrow) uint8_t[total]);
    if (!out.pixels) return false;

    // 32-bit decodes go straight into the output; narrower ones into a scratch buffer first.
    std::unique_ptr<uint8_t[]> scratch;
    uint8_t* target = out.pixels.get();
    if (sourceBytes != 4) {
        scratch.reset(new (std::nothrow) uint8_t[sourceStride * height]);
        if (!scratch) return false;
        target = scratch.get();
    }
    if (FAILED(transform->CopyPixels(nullptr, width, height, &format, WICBitmapTransformRotate0,
                                     static_cast<UINT>(sourceStride), static_cast<UINT>(sourceStride * height), target))) {
        return false;
    }

    // To opaque BGRA (alpha 255 is valid premultiplied).
    uint8_t* dst = out.pixels.get();
    for (UINT y = 0; y < height; ++y) {
        uint8_t* row = dst + size_t(y) * stride;
        if (sourceBytes == 4) {
            for (UINT x = 0; x < width; ++x) row[x * 4 + 3] = 255;
        } else if (sourceBytes == 3) {
            const uint8_t* src = scratch.get() + size_t(y) * sourceStride;
            for (UINT x = 0; x < width; ++x) {
                row[x * 4 + 0] = src[x * 3 + 0];
                row[x * 4 + 1] = src[x * 3 + 1];
                row[x * 4 + 2] = src[x * 3 + 2];
                row[x * 4 + 3] = 255;
            }
        } else {
            const uint8_t* src = scratch.get() + size_t(y) * sourceStride;
            for (UINT x = 0; x < width; ++x) {
                row[x * 4 + 0] = row[x * 4 + 1] = row[x * 4 + 2] = src[x];
                row[x * 4 + 3] = 255;
            }
        }
    }
    out.width = width;
    out.height = height;
    out.stride = stride;
    return true;
}

int readOrientation(IWICBitmapFrameDecode* frame) {
    ComPtr<IWICMetadataQueryReader> reader;
    if (FAILED(frame->GetMetadataQueryReader(reader.put()))) return 1;
    PROPVARIANT value;
    PropVariantInit(&value);
    int orientation = 1;
    // The photo-metadata policy name works across JPEG, TIFF, HEIF and RAW.
    if (SUCCEEDED(reader->GetMetadataByName(L"System.Photo.Orientation", &value)) && value.vt == VT_UI2) {
        orientation = value.uiVal;
    }
    PropVariantClear(&value);
    return (orientation >= 1 && orientation <= 8) ? orientation : 1;
}

}  // namespace

Decoder::Decoder() {
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IWICImagingFactory),
                     reinterpret_cast<void**>(factory_.put()));
}

UINT Decoder::halvingLevel(UINT full, UINT needed) {
    UINT level = full;
    needed = std::max(needed, 1u);
    while (level / 2 >= needed) level /= 2;
    return level;
}

std::shared_ptr<Decoded> Decoder::decode(const std::wstring& path, UINT frameIndex, const DecodeRequest& request) {
    if (!factory_) return nullptr;

    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory_->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                   WICDecodeMetadataCacheOnDemand, decoder.put()))) {
        return nullptr;
    }
    auto result = std::make_shared<Decoded>();
    ImageInfo& info = result->info;
    info.path = path;
    decoder->GetFrameCount(&info.frameCount);
    if (frameIndex >= info.frameCount) return nullptr;

    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(frameIndex, frame.put()))) return nullptr;
    if (FAILED(frame->GetSize(&info.width, &info.height)) || info.width == 0 || info.height == 0) return nullptr;
    info.orientation = readOrientation(frame.get());
    result->frame = frameIndex;

    // How many pixels does the view need along the long side?
    const UINT full = info.longSide();
    UINT needed = full;
    if (request.boxWidth && request.boxHeight) {
        double fit = std::min(double(request.boxWidth) / info.orientedWidth(),
                              double(request.boxHeight) / info.orientedHeight());
        needed = static_cast<UINT>(std::ceil(full * std::min(fit, 1.0)));
    } else if (request.longSide) {
        needed = std::min(request.longSide, full);
    }

    // 1. Codecs that can (JPEG) decode straight at 1/2, 1/4 or 1/8 size: the smallest of those
    //    that still covers the view. Whatever's left over, the GPU scales when drawing.
    UINT factor = 1;
    while (factor < 8 && ceilDiv(full, factor * 2) >= needed) factor *= 2;
    if (decodeNative(frame.get(), ceilDiv(info.width, factor), ceilDiv(info.height, factor), *result)) {
        return result;
    }

    // 2. Everything else decodes in full. Shrink on the CPU only when it's far bigger than the
    //    view needs (to keep memory down); otherwise the GPU does the scaling for free.
    IWICBitmapSource* source = frame.get();
    ComPtr<IWICBitmapScaler> scaler;
    if (full > needed * 4) {
        const double scale = double(needed) * 2 / full;
        const UINT targetWidth = std::max(1u, static_cast<UINT>(std::lround(info.width * scale)));
        const UINT targetHeight = std::max(1u, static_cast<UINT>(std::lround(info.height * scale)));
        if (SUCCEEDED(factory_->CreateBitmapScaler(scaler.put())) &&
            SUCCEEDED(scaler->Initialize(frame.get(), targetWidth, targetHeight, WICBitmapInterpolationModeFant))) {
            source = scaler.get();
        }
    }

    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory_->CreateFormatConverter(converter.put())) ||
        FAILED(converter->Initialize(source, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeMedianCut))) {
        return nullptr;
    }

    UINT width = 0, height = 0;
    converter->GetSize(&width, &height);
    result->width = width;
    result->height = height;
    result->stride = width * 4;
    const size_t total = static_cast<size_t>(result->stride) * height;
    // new[] without () leaves the memory uninitialised: zeroing 50 MB just to overwrite it costs time.
    result->pixels.reset(new (std::nothrow) uint8_t[total]);
    if (!result->pixels) return nullptr;

    // One call when it fits WIC's 32-bit buffer size; otherwise in bands of rows.
    const UINT stride = result->stride;
    const UINT rowsPerBand = total <= 0x7FFFFFFF ? height : std::max(1u, 0x40000000u / stride);
    for (UINT y = 0; y < height; y += rowsPerBand) {
        UINT rows = std::min(rowsPerBand, height - y);
        WICRect rect{0, static_cast<INT>(y), static_cast<INT>(width), static_cast<INT>(rows)};
        if (FAILED(converter->CopyPixels(&rect, stride, stride * rows, result->pixels.get() + size_t(y) * stride))) {
            return nullptr;
        }
    }
    return result;
}

std::vector<std::wstring> Decoder::supportedExtensions() {
    std::vector<std::wstring> extensions;
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IWICImagingFactory),
                                reinterpret_cast<void**>(factory.put())))) {
        return extensions;
    }
    ComPtr<IEnumUnknown> components;
    if (FAILED(factory->CreateComponentEnumerator(WICDecoder, WICComponentEnumerateDefault, components.put()))) {
        return extensions;
    }
    IUnknown* unknown = nullptr;
    ULONG fetched = 0;
    while (components->Next(1, &unknown, &fetched) == S_OK) {
        ComPtr<IWICBitmapDecoderInfo> codec;
        unknown->QueryInterface(__uuidof(IWICBitmapDecoderInfo), reinterpret_cast<void**>(codec.put()));
        unknown->Release();
        if (!codec) continue;
        UINT length = 0;
        if (FAILED(codec->GetFileExtensions(0, nullptr, &length)) || length == 0) continue;
        std::wstring list(length, L'\0');
        if (FAILED(codec->GetFileExtensions(length, list.data(), &length))) continue;
        list.resize(wcslen(list.c_str()));
        // ".jpeg,.jpe,.jpg,…"
        size_t start = 0;
        while (start < list.size()) {
            size_t end = list.find(L',', start);
            if (end == std::wstring::npos) end = list.size();
            std::wstring ext = list.substr(start, end - start);
            if (!ext.empty() && ext[0] == L'.') ext.erase(0, 1);
            if (!ext.empty()) extensions.push_back(lowercase(ext));
            start = end + 1;
        }
    }
    return extensions;
}
