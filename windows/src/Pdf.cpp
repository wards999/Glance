#include "Pdf.h"

#include "Navigator.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <list>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <type_traits>

namespace Pdf {

namespace {

// The handful of PDFium functions Glance uses, looked up at run time so there's nothing to link
// against and the DLL only loads when it's needed.
using Document = void*;
using Page = void*;
using Bitmap = void*;
struct SizeF {
    float width;
    float height;
};

/// FPDF_FILEACCESS: PDFium asks for the bytes it needs. Reading through a Win32 handle works with
/// any file name (PDFium's own file opening can't do non-English paths on Windows) and reads only
/// the parts of the file it uses.
struct FileAccess {
    unsigned long length;
    int (*getBlock)(void* param, unsigned long position, unsigned char* buffer, unsigned long size);
    void* param;
};

int readBlock(void* param, unsigned long position, unsigned char* buffer, unsigned long size) {
    OVERLAPPED at{};
    at.Offset = position;
    DWORD read = 0;
    return ReadFile(static_cast<HANDLE>(param), buffer, size, &read, &at) && read == size ? 1 : 0;
}

struct Api {
    void (*initLibrary)();
    Document (*loadCustomDocument)(FileAccess* file, const char* password);
    void (*closeDocument)(Document);
    int (*pageCount)(Document);
    int (*pageSizeByIndex)(Document, int index, SizeF* size);
    Page (*loadPage)(Document, int index);
    void (*closePage)(Page);
    Bitmap (*createBitmap)(int width, int height, int format, void* firstScan, int stride);
    void (*destroyBitmap)(Bitmap);
    void (*renderPage)(Bitmap, Page, int x, int y, int width, int height, int rotate, int flags);
};

constexpr int kBitmapBGRA = 4;     // FPDFBitmap_BGRA: the byte order Glance's pixels already use
constexpr int kRenderAnnotations = 0x01;  // FPDF_ANNOT: draw form fields and stamps too
constexpr size_t kOpenDocuments = 4;
constexpr size_t kOpenPages = 6;   // open pages keep their decoded images: re-rendering is fast

/// PDFium isn't safe to call from several threads, so every call goes through this.
std::mutex g_pdfium;
const Api* g_api = nullptr;
bool g_loadFailed = false;

std::wstring dllPath() {
    wchar_t exe[MAX_PATH * 4];
    DWORD length = GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
    return Navigator::folderOf(std::wstring(exe, length)) + L"\\pdfium.dll";
}

/// Loads pdfium.dll on first use. Call with g_pdfium held.
const Api* api() {
    if (g_api || g_loadFailed) return g_api;
    const double start = nowMs();
    HMODULE module = LoadLibraryExW(dllPath().c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    static Api loaded{};
    bool ok = module != nullptr;
    auto find = [&](auto& function, const char* name) {
        if (!ok) return;
        function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(reinterpret_cast<void*>(GetProcAddress(module, name)));
        ok = function != nullptr;
    };
    find(loaded.initLibrary, "FPDF_InitLibrary");
    find(loaded.loadCustomDocument, "FPDF_LoadCustomDocument");
    find(loaded.closeDocument, "FPDF_CloseDocument");
    find(loaded.pageCount, "FPDF_GetPageCount");
    find(loaded.pageSizeByIndex, "FPDF_GetPageSizeByIndexF");
    find(loaded.loadPage, "FPDF_LoadPage");
    find(loaded.closePage, "FPDF_ClosePage");
    find(loaded.createBitmap, "FPDFBitmap_CreateEx");
    find(loaded.destroyBitmap, "FPDFBitmap_Destroy");
    find(loaded.renderPage, "FPDF_RenderPageBitmap");
    if (!ok) {
        g_loadFailed = true;
        trace("pdfium.dll missing or unusable");
        return nullptr;
    }
    loaded.initLibrary();
    g_api = &loaded;
    char line[64];
    snprintf(line, sizeof(line), "pdfium.dll loaded in %.0f ms", nowMs() - start);
    trace(line);
    return g_api;
}

struct OpenDocument {
    std::wstring path;
    Document document = nullptr;
    std::vector<SizeF> sizes;  // per page, in points, after the page's own rotation
    HANDLE file = INVALID_HANDLE_VALUE;
    std::unique_ptr<FileAccess> access;  // PDFium keeps a pointer to it, so it never moves
};

void closeDocument(OpenDocument& opened);

struct OpenPage {
    Document document;
    int index;
    Page page;
};

std::list<OpenDocument> g_documents;  // most recent first
std::list<OpenPage> g_pages;          // most recent first

void closePagesOf(Document document) {
    for (auto it = g_pages.begin(); it != g_pages.end();) {
        if (it->document == document) {
            g_api->closePage(it->page);
            it = g_pages.erase(it);
        } else {
            ++it;
        }
    }
}

void closeDocument(OpenDocument& opened) {
    closePagesOf(opened.document);
    g_api->closeDocument(opened.document);
    CloseHandle(opened.file);
}

/// Keeps the last few PDFs open while you turn their pages. Call with g_pdfium held.
OpenDocument* open(const std::wstring& path) {
    for (auto it = g_documents.begin(); it != g_documents.end(); ++it) {
        if (samePath(it->path, path)) {
            g_documents.splice(g_documents.begin(), g_documents, it);
            return &g_documents.front();
        }
    }
    const Api* pdfium = api();
    if (!pdfium) return nullptr;
    const double start = nowMs();
    OpenDocument opened;
    opened.path = path;
    opened.file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                              FILE_FLAG_RANDOM_ACCESS, nullptr);
    LARGE_INTEGER size{};
    if (opened.file == INVALID_HANDLE_VALUE || !GetFileSizeEx(opened.file, &size) || size.QuadPart > 0xFFFFFFFFLL) {
        if (opened.file != INVALID_HANDLE_VALUE) CloseHandle(opened.file);
        trace("PDF couldn't be opened");
        return nullptr;
    }
    opened.access = std::make_unique<FileAccess>(FileAccess{static_cast<unsigned long>(size.QuadPart), readBlock, opened.file});
    opened.document = pdfium->loadCustomDocument(opened.access.get(), nullptr);
    if (!opened.document) {
        CloseHandle(opened.file);
        trace("PDF load failed (damaged, or needs a password)");
        return nullptr;
    }
    const int count = pdfium->pageCount(opened.document);
    for (int i = 0; i < count; ++i) {
        SizeF pageSize{612, 792};
        pdfium->pageSizeByIndex(opened.document, i, &pageSize);
        opened.sizes.push_back(pageSize);
    }
    g_documents.push_front(std::move(opened));
    if (g_documents.size() > kOpenDocuments) {
        closeDocument(g_documents.back());
        g_documents.pop_back();
    }
    char line[80];
    snprintf(line, sizeof(line), "PDF loaded: %d pages in %.0f ms", count, nowMs() - start);
    trace(line);
    return &g_documents.front();
}

/// An open page, kept so a second render (zoom, hold to zoom) doesn't decode its images again.
Page pageOf(Document document, int index) {
    for (auto it = g_pages.begin(); it != g_pages.end(); ++it) {
        if (it->document == document && it->index == index) {
            g_pages.splice(g_pages.begin(), g_pages, it);
            return it->page;
        }
    }
    Page page = g_api->loadPage(document, index);
    if (!page) return nullptr;
    g_pages.push_front(OpenPage{document, index, page});
    if (g_pages.size() > kOpenPages) {
        g_api->closePage(g_pages.back().page);
        g_pages.pop_back();
    }
    return page;
}

bool landscape(const SizeF& size) { return size.width > size.height; }

std::vector<std::vector<int>> group(const std::vector<SizeF>& sizes, int layout) {
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

}  // namespace

int packLayout(int mode, bool rightToLeft) { return mode * 2 + (rightToLeft ? 1 : 0); }

bool isPdf(const std::wstring& path) { return Navigator::extensionOf(path) == L"pdf"; }

bool available() {
    static const bool present = GetFileAttributesW(dllPath().c_str()) != INVALID_FILE_ATTRIBUTES;
    return present;
}

std::vector<std::vector<int>> spreads(const std::wstring& path, int layout) {
    std::lock_guard lock(g_pdfium);
    OpenDocument* opened = open(path);
    return opened ? group(opened->sizes, layout) : std::vector<std::vector<int>>{};
}

std::shared_ptr<Decoded> decode(const std::wstring& path, UINT index, const DecodeRequest& request, int layout) {
    std::lock_guard lock(g_pdfium);
    OpenDocument* opened = open(path);
    if (!opened) return nullptr;
    const std::vector<SizeF>& sizes = opened->sizes;
    const auto screens = group(sizes, layout);
    if (index >= screens.size()) return nullptr;
    const std::vector<int>& pages = screens[index];

    // Pages side by side, all scaled to the tallest page's height.
    float height = 0;
    for (int page : pages) height = std::max(height, sizes[page - 1].height);
    float width = 0;
    for (int page : pages) width += sizes[page - 1].width * height / std::max(sizes[page - 1].height, 1.0f);
    if (width <= 0 || height <= 0) return nullptr;

    // Vector pages have no native pixel size: "full resolution" is 4 pixels per point (288 dpi),
    // capped so a poster stays a sane size.
    const double fullScale = std::min(4.0, 16384.0 / std::max(width, height));
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
        const float w = sizes[page - 1].width * height / std::max(sizes[page - 1].height, 1.0f);
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
    std::memset(result->pixels.get(), 0xFF, result->bytes());  // white, opaque paper

    const double start = nowMs();
    UINT left = 0;
    for (size_t i = 0; i < drawOrder.size(); ++i) {
        Page page = pageOf(opened->document, drawOrder[i] - 1);
        // Drawn straight into Glance's own pixels: no copy, no encode and decode in between.
        Bitmap bitmap = page ? g_api->createBitmap(static_cast<int>(pageWidths[i]), static_cast<int>(outH), kBitmapBGRA,
                                                   result->pixels.get() + size_t(left) * 4, static_cast<int>(result->stride))
                             : nullptr;
        if (!bitmap) {
            char line[64];
            snprintf(line, sizeof(line), "PDF page %d FAILED to render", drawOrder[i]);
            trace(line);
            return nullptr;
        }
        g_api->renderPage(bitmap, page, 0, 0, static_cast<int>(pageWidths[i]), static_cast<int>(outH), 0, kRenderAnnotations);
        g_api->destroyBitmap(bitmap);
        left += pageWidths[i];
    }
    char line[96];
    snprintf(line, sizeof(line), "PDF pages %d-%d rendered at %ux%u in %.0f ms", pages.front(), pages.back(), outW, outH,
             nowMs() - start);
    trace(line);

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

}  // namespace Pdf
