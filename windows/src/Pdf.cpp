#include "Pdf.h"

#include "Navigator.h"

#ifdef GLANCE_PDF
#include <unknwn.h>
#include <shcore.h>
#include <winrt/Windows.Data.Pdf.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Storage.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <list>
#include <mutex>
#include <new>
#endif

namespace Pdf {

int packLayout(int mode, bool rightToLeft) { return mode * 2 + (rightToLeft ? 1 : 0); }

bool isPdf(const std::wstring& path) { return Navigator::extensionOf(path) == L"pdf"; }

#ifndef GLANCE_PDF

bool available() { return false; }
std::shared_ptr<Decoded> decode(const std::wstring&, UINT, const DecodeRequest&, int) { return nullptr; }
std::vector<std::vector<int>> spreads(const std::wstring&, int) { return {}; }

#else

namespace {

using winrt::Windows::Data::Pdf::PdfDocument;
using winrt::Windows::Data::Pdf::PdfPage;
using winrt::Windows::Data::Pdf::PdfPageRenderOptions;
using winrt::Windows::Foundation::Size;

struct OpenDocument {
    std::wstring path;
    PdfDocument document{nullptr};
    std::vector<Size> sizes;  // per page, in DIPs, after the page's own rotation
};

std::mutex g_mutex;
std::list<OpenDocument> g_open;  // most recent first

/// Loading parses the file, so the last few PDFs stay open while you turn their pages.
bool open(const std::wstring& path, PdfDocument& document, std::vector<Size>& sizes) {
    {
        std::lock_guard lock(g_mutex);
        for (auto it = g_open.begin(); it != g_open.end(); ++it) {
            if (samePath(it->path, path)) {
                g_open.splice(g_open.begin(), g_open, it);
                document = it->document;
                sizes = it->sizes;
                return true;
            }
        }
    }
    try {
        auto file = winrt::Windows::Storage::StorageFile::GetFileFromPathAsync(path).get();
        auto loaded = PdfDocument::LoadFromFileAsync(file).get();
        std::vector<Size> pageSizes;
        for (uint32_t i = 0; i < loaded.PageCount(); ++i) pageSizes.push_back(loaded.GetPage(i).Size());
        std::lock_guard lock(g_mutex);
        g_open.push_front(OpenDocument{path, loaded, pageSizes});
        if (g_open.size() > 4) g_open.pop_back();
        document = loaded;
        sizes = std::move(pageSizes);
        return true;
    } catch (...) {
        return false;  // damaged, or needs a password
    }
}

bool landscape(const Size& size) { return size.Width > size.Height; }

std::vector<std::vector<int>> group(const std::vector<Size>& sizes, int layout) {
    const int count = static_cast<int>(sizes.size());
    std::vector<std::vector<int>> screens;
    if (count == 0) return screens;
    int mode = layout / 2;
    if (mode == 0) {
        // Magazine-like (four or more mostly portrait pages): a cover, then spreads.
        const int sample = std::min(count, 20);
        int portrait = 0;
        for (int i = 0; i < sample; ++i) portrait += landscape(sizes[i]) ? 0 : 1;
        mode = count >= 4 && portrait * 5 >= sample * 4 ? 3 : 1;
    }
    if (mode == 1) {
        for (int page = 1; page <= count; ++page) screens.push_back({page});
        return screens;
    }
    int page = 1;
    if (mode == 3) {
        screens.push_back({1});
        page = 2;
    }
    while (page <= count) {
        // A landscape page is usually a spread already, so it stands alone.
        if (page == count || landscape(sizes[page - 1]) || landscape(sizes[page])) {
            screens.push_back({page});
            page += 1;
        } else {
            screens.push_back({page, page + 1});
            page += 2;
        }
    }
    return screens;
}

bool renderPage(const PdfPage& page, UINT width, UINT height, Decoded& out, UINT left, IWICImagingFactory* wic) {
    try {
        winrt::Windows::Storage::Streams::InMemoryRandomAccessStream stream;
        PdfPageRenderOptions options;
        options.DestinationWidth(width);
        options.DestinationHeight(height);
        // Uncompressed, so there's nothing to decode on the way back.
        options.BitmapEncoderId(winrt::Windows::Graphics::Imaging::BitmapEncoder::BmpEncoderId());
        page.RenderToStreamAsync(stream, options).get();
        stream.Seek(0);

        ComPtr<IStream> native;
        if (FAILED(CreateStreamOverRandomAccessStream(winrt::get_unknown(stream), __uuidof(IStream),
                                                      reinterpret_cast<void**>(native.put())))) {
            return false;
        }
        ComPtr<IWICBitmapDecoder> decoder;
        ComPtr<IWICBitmapFrameDecode> frame;
        ComPtr<IWICFormatConverter> converter;
        if (FAILED(wic->CreateDecoderFromStream(native.get(), nullptr, WICDecodeMetadataCacheOnDemand, decoder.put())) ||
            FAILED(decoder->GetFrame(0, frame.put())) || FAILED(wic->CreateFormatConverter(converter.put())) ||
            FAILED(converter->Initialize(frame.get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                         WICBitmapPaletteTypeMedianCut))) {
            return false;
        }
        UINT w = 0, h = 0;
        converter->GetSize(&w, &h);
        w = std::min(w, width);
        h = std::min(h, height);
        WICRect rect{0, 0, static_cast<INT>(w), static_cast<INT>(h)};
        uint8_t* start = out.pixels.get() + size_t(left) * 4;
        return SUCCEEDED(converter->CopyPixels(&rect, out.stride, static_cast<UINT>(out.bytes() - size_t(left) * 4), start));
    } catch (...) {
        return false;
    }
}

}  // namespace

bool available() { return true; }

std::vector<std::vector<int>> spreads(const std::wstring& path, int layout) {
    PdfDocument document{nullptr};
    std::vector<Size> sizes;
    if (!open(path, document, sizes)) return {};
    return group(sizes, layout);
}

std::shared_ptr<Decoded> decode(const std::wstring& path, UINT index, const DecodeRequest& request, int layout) {
    PdfDocument document{nullptr};
    std::vector<Size> sizes;
    if (!open(path, document, sizes)) return nullptr;
    const auto screens = group(sizes, layout);
    if (index >= screens.size()) return nullptr;
    const std::vector<int>& pages = screens[index];

    // Pages side by side, all scaled to the tallest page's height.
    float height = 0;
    for (int page : pages) height = std::max(height, sizes[page - 1].Height);
    float width = 0;
    for (int page : pages) width += sizes[page - 1].Width * height / std::max(sizes[page - 1].Height, 1.0f);
    if (width <= 0 || height <= 0) return nullptr;

    // Vector pages have no native pixel size: "full resolution" is 3 pixels per DIP (288 dpi),
    // capped so a poster stays a sane size.
    const double fullScale = std::min(3.0, 16384.0 / std::max(width, height));
    const UINT fullW = static_cast<UINT>(std::lround(width * fullScale));
    const UINT fullH = static_cast<UINT>(std::lround(height * fullScale));
    const UINT full = std::max(fullW, fullH);
    UINT needed = full;
    if (request.boxWidth && request.boxHeight) {
        const double fit = std::min(double(request.boxWidth) / fullW, double(request.boxHeight) / fullH);
        needed = static_cast<UINT>(std::ceil(full * std::min(fit, 1.0)));
    } else if (request.longSide) {
        needed = std::min(request.longSide, full);
    }
    // Unlike JPEG, rendering costs the same per pixel at any size, so render exactly what's needed.
    const double scale = double(needed) / std::max(width, height);

    std::vector<int> drawOrder = pages;
    if (layout & 1) std::reverse(drawOrder.begin(), drawOrder.end());  // right to left
    std::vector<UINT> pageWidths;
    UINT outW = 0;
    const UINT outH = std::max(1u, static_cast<UINT>(std::lround(height * scale)));
    for (int page : drawOrder) {
        const float w = sizes[page - 1].Width * height / std::max(sizes[page - 1].Height, 1.0f);
        pageWidths.push_back(std::max(1u, static_cast<UINT>(std::lround(w * scale))));
        outW += pageWidths.back();
    }

    auto result = std::make_shared<Decoded>();
    result->frame = index;
    result->width = outW;
    result->height = outH;
    result->stride = outW * 4;
    result->pixels.reset(new (std::nothrow) uint8_t[result->bytes()]);
    if (!result->pixels) return nullptr;
    std::memset(result->pixels.get(), 0xFF, result->bytes());  // paper is white

    ComPtr<IWICImagingFactory> wic;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IWICImagingFactory),
                     reinterpret_cast<void**>(wic.put()));
    if (!wic) return nullptr;
    UINT left = 0;
    for (size_t i = 0; i < drawOrder.size(); ++i) {
        // Each page is drawn at the spread's height; uneven rounding leaves at most a pixel of white.
        const float pageHeight = sizes[drawOrder[i] - 1].Height * height / std::max(sizes[drawOrder[i] - 1].Height, 1.0f);
        const UINT h = std::min(outH, std::max(1u, static_cast<UINT>(std::lround(pageHeight * scale))));
        if (!renderPage(document.GetPage(drawOrder[i] - 1), pageWidths[i], h, *result, left, wic.get())) return nullptr;
        left += pageWidths[i];
    }

    ImageInfo& info = result->info;
    info.path = path;
    info.width = fullW;
    info.height = fullH;
    info.frameCount = static_cast<UINT>(screens.size());
    info.format = L"PDF document";
    info.isPdf = true;
    info.pdfFirstPage = pages.front();
    const std::wstring total = std::to_wstring(sizes.size());
    info.pdfPages = pages.size() == 1 ? L"Page " + std::to_wstring(pages[0]) + L" of " + total
                                      : L"Pages " + std::to_wstring(pages.front()) + L"–" + std::to_wstring(pages.back()) + L" of " + total;
    WIN32_FILE_ATTRIBUTE_DATA attributes;
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes)) {
        info.fileSize = (ULONGLONG(attributes.nFileSizeHigh) << 32) | attributes.nFileSizeLow;
    }
    return result;
}

#endif

}  // namespace Pdf
