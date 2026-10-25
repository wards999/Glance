#include "Editing.h"

#include <oleauto.h>
#include <propidl.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>

namespace Editing {

namespace {

D2D1_MATRIX_3X2_F matrix(float m11, float m12, float m21, float m22) {
    D2D1_MATRIX_3X2_F m{};
    m._11 = m11;
    m._12 = m12;
    m._21 = m21;
    m._22 = m22;
    return m;
}

ComPtr<IWICImagingFactory> factory() {
    ComPtr<IWICImagingFactory> result;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IWICImagingFactory),
                     reinterpret_cast<void**>(result.put()));
    return result;
}

bool setOrientation(IWICMetadataQueryWriter* writer, int orientation) {
    PROPVARIANT value;
    PropVariantInit(&value);
    value.vt = VT_UI2;
    value.uiVal = static_cast<USHORT>(orientation);
    return SUCCEEDED(writer->SetMetadataByName(L"System.Photo.Orientation", &value));
}

}  // namespace

D2D1_MATRIX_3X2_F exifMatrix(int orientation) {
    switch (orientation) {
    case 2: return matrix(-1, 0, 0, 1);
    case 3: return matrix(-1, 0, 0, -1);
    case 4: return matrix(1, 0, 0, -1);
    case 5: return matrix(0, 1, 1, 0);
    case 6: return matrix(0, 1, -1, 0);
    case 7: return matrix(0, -1, -1, 0);
    case 8: return matrix(0, -1, 1, 0);
    default: return matrix(1, 0, 0, 1);
    }
}

D2D1_MATRIX_3X2_F multiply(const D2D1_MATRIX_3X2_F& a, const D2D1_MATRIX_3X2_F& b) {
    D2D1_MATRIX_3X2_F m;
    m._11 = a._11 * b._11 + a._12 * b._21;
    m._12 = a._11 * b._12 + a._12 * b._22;
    m._21 = a._21 * b._11 + a._22 * b._21;
    m._22 = a._21 * b._12 + a._22 * b._22;
    m._31 = a._31 * b._11 + a._32 * b._21 + b._31;
    m._32 = a._31 * b._12 + a._32 * b._22 + b._32;
    return m;
}

std::shared_ptr<Decoded> orient(const Decoded& source, const D2D1_MATRIX_3X2_F& orientation) {
    const int a = static_cast<int>(std::lround(orientation._11)), b = static_cast<int>(std::lround(orientation._12));
    const int c = static_cast<int>(std::lround(orientation._21)), d = static_cast<int>(std::lround(orientation._22));
    const int w = static_cast<int>(source.width), h = static_cast<int>(source.height);
    const int ow = a == 0 ? h : w, oh = a == 0 ? w : h;

    auto result = std::make_shared<Decoded>();
    result->info = source.info;
    result->info.width = static_cast<UINT>(ow);
    result->info.height = static_cast<UINT>(oh);
    result->info.orientation = 1;
    result->frame = source.frame;
    result->width = static_cast<UINT>(ow);
    result->height = static_cast<UINT>(oh);
    result->stride = static_cast<UINT>(ow) * 4;
    result->pixels.reset(new (std::nothrow) uint8_t[result->bytes()]);
    if (!result->pixels) return nullptr;
    if (a == 1 && b == 0 && c == 0 && d == 1) {
        for (int y = 0; y < oh; ++y) {
            std::memcpy(result->pixels.get() + size_t(y) * result->stride, source.pixels.get() + size_t(y) * source.stride, result->stride);
        }
        return result;
    }
    for (int dy = 0; dy < oh; ++dy) {
        uint8_t* row = result->pixels.get() + size_t(dy) * result->stride;
        const int v = 2 * dy + 1 - oh;
        for (int dx = 0; dx < ow; ++dx) {
            // The inverse of a quarter-turn or flip is its transpose; coordinates are doubled
            // and centred so they stay integers.
            const int u = 2 * dx + 1 - ow;
            const int sx = (u * a + v * b + w - 1) / 2;
            const int sy = (u * c + v * d + h - 1) / 2;
            std::memcpy(row + size_t(dx) * 4, source.pixels.get() + size_t(sy) * source.stride + size_t(sx) * 4, 4);
        }
    }
    return result;
}

std::shared_ptr<Decoded> crop(const Decoded& source, RECT area) {
    area.left = std::clamp<LONG>(area.left, 0, static_cast<LONG>(source.width) - 1);
    area.top = std::clamp<LONG>(area.top, 0, static_cast<LONG>(source.height) - 1);
    area.right = std::clamp<LONG>(area.right, area.left + 1, static_cast<LONG>(source.width));
    area.bottom = std::clamp<LONG>(area.bottom, area.top + 1, static_cast<LONG>(source.height));
    auto result = std::make_shared<Decoded>();
    result->info = source.info;
    result->frame = source.frame;
    result->width = static_cast<UINT>(area.right - area.left);
    result->height = static_cast<UINT>(area.bottom - area.top);
    result->info.width = result->width;
    result->info.height = result->height;
    result->stride = result->width * 4;
    result->pixels.reset(new (std::nothrow) uint8_t[result->bytes()]);
    if (!result->pixels) return nullptr;
    for (UINT y = 0; y < result->height; ++y) {
        std::memcpy(result->pixels.get() + size_t(y) * result->stride,
                    source.pixels.get() + size_t(area.top + y) * source.stride + size_t(area.left) * 4, result->stride);
    }
    return result;
}

int composeOrientation(int current, const D2D1_MATRIX_3X2_F& transform) {
    const D2D1_MATRIX_3X2_F target = multiply(exifMatrix(current), transform);
    for (int candidate = 1; candidate <= 8; ++candidate) {
        const D2D1_MATRIX_3X2_F m = exifMatrix(candidate);
        if (std::lround(m._11) == std::lround(target._11) && std::lround(m._12) == std::lround(target._12) &&
            std::lround(m._21) == std::lround(target._21) && std::lround(m._22) == std::lround(target._22)) {
            return candidate;
        }
    }
    return current;
}

bool rewriteOrientation(const std::wstring& path, int orientation) {
    auto wic = factory();
    if (!wic) return false;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFastMetadataEncoder> encoder;
    ComPtr<IWICMetadataQueryWriter> writer;
    // The fast metadata encoder edits the tags in the file itself; the image data isn't touched.
    return SUCCEEDED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ | GENERIC_WRITE,
                                                    WICDecodeMetadataCacheOnDemand, decoder.put())) &&
           SUCCEEDED(decoder->GetFrame(0, frame.put())) &&
           SUCCEEDED(wic->CreateFastMetadataEncoderFromFrameDecode(frame.get(), encoder.put())) &&
           SUCCEEDED(encoder->GetMetadataQueryWriter(writer.put())) && setOrientation(writer.get(), orientation) &&
           SUCCEEDED(encoder->Commit());
}

bool save(const Decoded& upright, const std::wstring& path, const GUID& container, int quality,
          const std::wstring& metadataFrom) {
    auto wic = factory();
    if (!wic || !upright.pixels) return false;
    const std::wstring temporary = path + L".glance-saving";
    bool ok = false;
    {
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapEncoder> encoder;
        ComPtr<IWICBitmapFrameEncode> frame;
        ComPtr<IPropertyBag2> options;
        ComPtr<IWICBitmapDecoder> source;  // kept open until the frame is committed
        ok = SUCCEEDED(wic->CreateStream(stream.put())) &&
             SUCCEEDED(stream->InitializeFromFilename(temporary.c_str(), GENERIC_WRITE)) &&
             SUCCEEDED(wic->CreateEncoder(container, nullptr, encoder.put())) &&
             SUCCEEDED(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache)) &&
             SUCCEEDED(encoder->CreateNewFrame(frame.put(), options.put()));
        if (ok && container == GUID_ContainerFormatJpeg && options) {
            PROPBAG2 option{};
            option.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
            VARIANT value;
            VariantInit(&value);
            value.vt = VT_R4;
            value.fltVal = std::clamp(quality, 1, 100) / 100.0f;
            options->Write(1, &option, &value);
        }
        ok = ok && SUCCEEDED(frame->Initialize(options.get())) && SUCCEEDED(frame->SetSize(upright.width, upright.height));

        if (ok && !metadataFrom.empty() &&
            SUCCEEDED(wic->CreateDecoderFromFilename(metadataFrom.c_str(), nullptr, GENERIC_READ,
                                                     WICDecodeMetadataCacheOnDemand, source.put()))) {
            GUID sourceContainer{};
            ComPtr<IWICBitmapFrameDecode> sourceFrame;
            if (SUCCEEDED(source->GetContainerFormat(&sourceContainer)) && sourceContainer == container &&
                SUCCEEDED(source->GetFrame(0, sourceFrame.put()))) {
                auto reader = sourceFrame.as<IWICMetadataBlockReader>();
                auto blocks = frame.as<IWICMetadataBlockWriter>();
                if (reader && blocks) blocks->InitializeFromBlockReader(reader.get());
            }
        }
        if (ok) {
            ComPtr<IWICMetadataQueryWriter> tags;
            if (SUCCEEDED(frame->GetMetadataQueryWriter(tags.put()))) setOrientation(tags.get(), 1);
        }

        ComPtr<IWICBitmap> bitmap;
        ok = ok &&
             SUCCEEDED(wic->CreateBitmapFromMemory(upright.width, upright.height, GUID_WICPixelFormat32bppPBGRA,
                                                   upright.stride, static_cast<UINT>(upright.bytes()),
                                                   upright.pixels.get(), bitmap.put())) &&
             SUCCEEDED(frame->WriteSource(bitmap.get(), nullptr)) && SUCCEEDED(frame->Commit()) &&
             SUCCEEDED(encoder->Commit());
    }  // closes the new file and the source before swapping them
    if (!ok) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    return MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED) != 0;
}

}  // namespace Editing
