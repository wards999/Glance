// Writes photo-like test images (smooth gradients plus sensor-style noise, so JPEGs come out at
// realistic sizes) for timing Glance against other viewers.
//
//   make-test-images <folder>

#include <windows.h>
#include <oleauto.h>
#include <propidl.h>
#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

namespace {

bool writeImage(IWICImagingFactory* factory, const std::wstring& path, const GUID& container, UINT width, UINT height,
                const std::vector<uint8_t>& bgr, int orientation) {
    IWICStream* stream = nullptr;
    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    IPropertyBag2* options = nullptr;
    bool ok = SUCCEEDED(factory->CreateStream(&stream)) &&
              SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
              SUCCEEDED(factory->CreateEncoder(container, nullptr, &encoder)) &&
              SUCCEEDED(encoder->Initialize(stream, WICBitmapEncoderNoCache)) &&
              SUCCEEDED(encoder->CreateNewFrame(&frame, &options));
    if (ok && container == GUID_ContainerFormatJpeg) {
        PROPBAG2 option{};
        option.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
        VARIANT value;
        VariantInit(&value);
        value.vt = VT_R4;
        value.fltVal = 0.9f;
        options->Write(1, &option, &value);
    }
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    ok = ok && SUCCEEDED(frame->Initialize(options)) && SUCCEEDED(frame->SetSize(width, height)) &&
         SUCCEEDED(frame->SetPixelFormat(&format));
    if (ok && orientation != 1) {
        IWICMetadataQueryWriter* writer = nullptr;
        if (SUCCEEDED(frame->GetMetadataQueryWriter(&writer))) {
            PROPVARIANT value;
            PropVariantInit(&value);
            value.vt = VT_UI2;
            value.uiVal = static_cast<USHORT>(orientation);
            writer->SetMetadataByName(L"System.Photo.Orientation", &value);
            writer->Release();
        }
    }
    const UINT stride = width * 3;
    ok = ok && SUCCEEDED(frame->WritePixels(height, stride, stride * height, const_cast<BYTE*>(bgr.data()))) &&
         SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
    if (options) options->Release();
    if (frame) frame->Release();
    if (encoder) encoder->Release();
    if (stream) stream->Release();
    return ok;
}

std::vector<uint8_t> makePixels(UINT width, UINT height, unsigned seed, int noise) {
    std::vector<uint8_t> bgr(size_t(width) * height * 3);
    std::mt19937 random(seed);
    std::uniform_real_distribution<double> phase(0, 6.283);
    std::normal_distribution<double> grain(0, noise);
    const double p1 = phase(random), p2 = phase(random), p3 = phase(random);
    const double f1 = 2 + seed % 5, f2 = 3 + seed % 7;
    for (UINT y = 0; y < height; ++y) {
        const double v = double(y) / height;
        for (UINT x = 0; x < width; ++x) {
            const double u = double(x) / width;
            // A few overlapping waves look like soft shapes and skies; the grain is what makes
            // real photos expensive to compress and decode.
            const double base = 0.5 + 0.25 * std::sin(f1 * u * 6.283 + p1) * std::cos(f2 * v * 6.283 + p2) +
                                0.2 * std::sin((u + v) * 9 + p3);
            uint8_t* px = &bgr[(size_t(y) * width + x) * 3];
            for (int c = 0; c < 3; ++c) {
                double value = 255 * (base * (0.8 + 0.1 * c)) + grain(random);
                px[c] = static_cast<uint8_t>(std::clamp(value, 0.0, 255.0));
            }
        }
    }
    return bgr;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::fwprintf(stderr, L"usage: make-test-images <folder>\n");
        return 1;
    }
    std::wstring folder = argv[1];
    CreateDirectoryW(folder.c_str(), nullptr);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory* factory = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IWICImagingFactory),
                                reinterpret_cast<void**>(&factory)))) {
        return 1;
    }

    struct Spec {
        const wchar_t* prefix;
        const GUID* container;
        const wchar_t* extension;
        UINT width, height;
        int count, noise, orientation;
    };
    const Spec specs[] = {
        {L"photo12mp", &GUID_ContainerFormatJpeg, L"jpg", 4000, 3000, 12, 6, 1},
        {L"portrait12mp", &GUID_ContainerFormatJpeg, L"jpg", 4000, 3000, 4, 6, 6},  // stored sideways, EXIF says rotate
        {L"photo24mp", &GUID_ContainerFormatJpeg, L"jpg", 6000, 4000, 4, 6, 1},
        {L"png12mp", &GUID_ContainerFormatPng, L"png", 4000, 3000, 4, 2, 1},
    };
    unsigned seed = 1;
    for (const Spec& spec : specs) {
        for (int i = 1; i <= spec.count; ++i, ++seed) {
            wchar_t name[MAX_PATH];
            swprintf(name, MAX_PATH, L"%ls\\%ls_%02d.%ls", folder.c_str(), spec.prefix, i, spec.extension);
            auto pixels = makePixels(spec.width, spec.height, seed, spec.noise);
            bool ok = writeImage(factory, name, *spec.container, spec.width, spec.height, pixels, spec.orientation);
            std::wprintf(L"%ls %ls\n", ok ? L"wrote" : L"FAILED", name);
        }
    }
    factory->Release();
    CoUninitialize();
    return 0;
}
