#include "Decoder.h"

#include <propidl.h>

#include <algorithm>
#include <cmath>
#include <new>

namespace {

// WICBitmapInterpolationModeHighQualityCubic (Windows 10+); MinGW's header predates it.
constexpr auto kHighQualityCubic = static_cast<WICBitmapInterpolationMode>(4);

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

UINT ceilDiv(UINT value, UINT divisor) { return (value + divisor - 1) / divisor; }

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

    // Pick the output size. Powers of two use the codec's own scaled decode exactly, so no
    // resampling pass follows; beyond 1/8, WIC resamples the rest of the way.
    UINT factor = 1;
    while (factor < 8 && ceilDiv(full, factor * 2) >= needed) factor *= 2;
    UINT targetWidth = ceilDiv(info.width, factor);
    UINT targetHeight = ceilDiv(info.height, factor);
    if (ceilDiv(full, factor) > needed * 2) {
        double scale = double(needed) / full;
        targetWidth = std::max(1u, static_cast<UINT>(std::lround(info.width * scale)));
        targetHeight = std::max(1u, static_cast<UINT>(std::lround(info.height * scale)));
    }

    IWICBitmapSource* source = frame.get();
    ComPtr<IWICBitmapScaler> scaler;
    if (targetWidth != info.width || targetHeight != info.height) {
        // Connected straight to the decoder, the scaler asks codecs that can (JPEG) to decode
        // at reduced size directly rather than decoding everything and shrinking it.
        if (SUCCEEDED(factory_->CreateBitmapScaler(scaler.put())) &&
            SUCCEEDED(scaler->Initialize(frame.get(), targetWidth, targetHeight, kHighQualityCubic))) {
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
