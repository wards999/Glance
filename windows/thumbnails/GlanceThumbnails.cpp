// Explorer thumbnails for PDFs: the first page, drawn by PDFium (the pdfium.dll beside this DLL).
//
// Install for the current user only (no administrator rights):  regsvr32 GlanceThumbnails.dll
// Remove:                                                         regsvr32 /u GlanceThumbnails.dll
//
// Explorer runs thumbnail handlers in a separate process (dllhost), so a damaged PDF can't take
// Explorer down, and it caches the pictures, so each file is rendered once.

#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <thumbcache.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <type_traits>

namespace {

// {B6E3C3A1-5F0D-4E8B-9C2A-7D41E6F0A9B3}
constexpr CLSID kClsid = {0xb6e3c3a1, 0x5f0d, 0x4e8b, {0x9c, 0x2a, 0x7d, 0x41, 0xe6, 0xf0, 0xa9, 0xb3}};
constexpr wchar_t kClsidString[] = L"{B6E3C3A1-5F0D-4E8B-9C2A-7D41E6F0A9B3}";
// The ShellEx key Explorer looks under for a file type's thumbnail handler (IThumbnailProvider).
constexpr wchar_t kThumbnailHandlerKey[] = L"Software\\Classes\\.pdf\\ShellEx\\{e357fccd-a995-4576-b01f-234630154e96}";
constexpr wchar_t kClsidKey[] = L"Software\\Classes\\CLSID\\{B6E3C3A1-5F0D-4E8B-9C2A-7D41E6F0A9B3}";

HMODULE g_module = nullptr;
std::atomic<long> g_references{0};

// The PDFium functions used here, looked up at run time.
using Document = void*;
using Page = void*;
using Bitmap = void*;

struct FileAccess {
    unsigned long length;
    int (*getBlock)(void* param, unsigned long position, unsigned char* buffer, unsigned long size);
    void* param;
};

struct Api {
    void (*initLibrary)();
    Document (*loadCustomDocument)(FileAccess* file, const char* password);
    void (*closeDocument)(Document);
    Page (*loadPage)(Document, int index);
    void (*closePage)(Page);
    float (*pageWidth)(Page);
    float (*pageHeight)(Page);
    Bitmap (*createBitmap)(int width, int height, int format, void* firstScan, int stride);
    void (*destroyBitmap)(Bitmap);
    void (*renderPage)(Bitmap, Page, int x, int y, int width, int height, int rotate, int flags);
};

constexpr int kBitmapBGRA = 4;
constexpr int kRenderAnnotations = 0x01;
constexpr UINT kMaxSide = 2560;

// PDFium isn't safe to call from several threads, and Explorer may ask for several at once.
std::mutex g_pdfium;

const Api* api() {
    static Api loaded{};
    static const Api* result = [] () -> const Api* {
        wchar_t path[MAX_PATH * 4];
        const DWORD length = GetModuleFileNameW(g_module, path, static_cast<DWORD>(std::size(path)));
        std::wstring dll(path, length);
        dll = dll.substr(0, dll.find_last_of(L'\\') + 1) + L"pdfium.dll";
        HMODULE module = LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        bool ok = module != nullptr;
        auto find = [&](auto& function, const char* name) {
            if (!ok) return;
            function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(
                reinterpret_cast<void*>(GetProcAddress(module, name)));
            ok = function != nullptr;
        };
        find(loaded.initLibrary, "FPDF_InitLibrary");
        find(loaded.loadCustomDocument, "FPDF_LoadCustomDocument");
        find(loaded.closeDocument, "FPDF_CloseDocument");
        find(loaded.loadPage, "FPDF_LoadPage");
        find(loaded.closePage, "FPDF_ClosePage");
        find(loaded.pageWidth, "FPDF_GetPageWidthF");
        find(loaded.pageHeight, "FPDF_GetPageHeightF");
        find(loaded.createBitmap, "FPDFBitmap_CreateEx");
        find(loaded.destroyBitmap, "FPDFBitmap_Destroy");
        find(loaded.renderPage, "FPDF_RenderPageBitmap");
        if (!ok) return nullptr;
        loaded.initLibrary();
        return &loaded;
    }();
    return result;
}

/// PDFium reads the parts of the file it needs, through the stream Explorer gives us.
int readBlock(void* param, unsigned long position, unsigned char* buffer, unsigned long size) {
    auto* stream = static_cast<IStream*>(param);
    LARGE_INTEGER at;
    at.QuadPart = position;
    if (FAILED(stream->Seek(at, STREAM_SEEK_SET, nullptr))) return 0;
    while (size > 0) {
        ULONG read = 0;
        if (FAILED(stream->Read(buffer, size, &read)) || read == 0) return 0;
        buffer += read;
        size -= read;
    }
    return 1;
}

class ThumbnailProvider : public IInitializeWithStream, public IThumbnailProvider {
public:
    ThumbnailProvider() { ++g_references; }

    // IUnknown
    IFACEMETHODIMP QueryInterface(REFIID riid, void** object) override {
        static const QITAB table[] = {
            QITABENT(ThumbnailProvider, IInitializeWithStream),
            QITABENT(ThumbnailProvider, IThumbnailProvider),
            {},
        };
        return QISearch(this, table, riid, object);
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return ++count_; }
    IFACEMETHODIMP_(ULONG) Release() override {
        const ULONG count = --count_;
        if (count == 0) delete this;
        return count;
    }

    // IInitializeWithStream
    IFACEMETHODIMP Initialize(IStream* stream, DWORD) override {
        if (stream_) return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
        stream_ = stream;
        stream_->AddRef();
        return S_OK;
    }

    // IThumbnailProvider: the first page, `side` pixels on its longer edge, on white.
    IFACEMETHODIMP GetThumbnail(UINT side, HBITMAP* result, WTS_ALPHATYPE* alpha) override {
        if (!stream_ || !result || !alpha) return E_INVALIDARG;
        *result = nullptr;
        STATSTG stat{};
        if (FAILED(stream_->Stat(&stat, STATFLAG_NONAME)) || stat.cbSize.QuadPart > 0xFFFFFFFFull) return E_FAIL;

        std::lock_guard lock(g_pdfium);
        const Api* pdf = api();
        if (!pdf) return E_FAIL;
        FileAccess access{static_cast<unsigned long>(stat.cbSize.QuadPart), readBlock, stream_};
        Document document = pdf->loadCustomDocument(&access, nullptr);
        if (!document) return E_FAIL;  // damaged, or locked with a password
        HRESULT hr = E_FAIL;
        if (Page page = pdf->loadPage(document, 0)) {
            const double width = pdf->pageWidth(page), height = pdf->pageHeight(page);
            side = std::min(side, kMaxSide);
            if (width > 0 && height > 0 && side > 0) {
                const double scale = side / std::max(width, height);
                const int w = std::max(1, static_cast<int>(width * scale + 0.5));
                const int h = std::max(1, static_cast<int>(height * scale + 0.5));
                BITMAPINFO info{};
                info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                info.bmiHeader.biWidth = w;
                info.bmiHeader.biHeight = -h;  // top-down, as PDFium draws
                info.bmiHeader.biPlanes = 1;
                info.bmiHeader.biBitCount = 32;
                info.bmiHeader.biCompression = BI_RGB;
                void* bits = nullptr;
                HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
                if (bitmap && bits) {
                    // PDFium draws straight into the bitmap Explorer gets: white paper, then the page.
                    std::memset(bits, 0xFF, size_t(w) * h * 4);
                    if (Bitmap target = pdf->createBitmap(w, h, kBitmapBGRA, bits, w * 4)) {
                        pdf->renderPage(target, page, 0, 0, w, h, 0, kRenderAnnotations);
                        pdf->destroyBitmap(target);
                        *result = bitmap;
                        *alpha = WTSAT_RGB;
                        hr = S_OK;
                    }
                }
                if (FAILED(hr) && bitmap) DeleteObject(bitmap);
            }
            pdf->closePage(page);
        }
        pdf->closeDocument(document);
        return hr;
    }

private:
    ~ThumbnailProvider() {
        if (stream_) stream_->Release();
        --g_references;
    }

    std::atomic<ULONG> count_{1};
    IStream* stream_ = nullptr;
};

class ClassFactory : public IClassFactory {
public:
    IFACEMETHODIMP QueryInterface(REFIID riid, void** object) override {
        static const QITAB table[] = {QITABENT(ClassFactory, IClassFactory), {}};
        return QISearch(this, table, riid, object);
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return 2; }  // one static instance
    IFACEMETHODIMP_(ULONG) Release() override { return 1; }
    IFACEMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** object) override {
        *object = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;
        auto* provider = new (std::nothrow) ThumbnailProvider();
        if (!provider) return E_OUTOFMEMORY;
        const HRESULT hr = provider->QueryInterface(riid, object);
        provider->Release();
        return hr;
    }
    IFACEMETHODIMP LockServer(BOOL lock) override {
        if (lock) ++g_references; else --g_references;
        return S_OK;
    }
};

ClassFactory g_factory;

LSTATUS setString(HKEY root, const wchar_t* key, const wchar_t* name, const std::wstring& value) {
    return RegSetKeyValueW(root, key, name, REG_SZ, value.c_str(), static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void*) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = instance;
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** object) {
    if (!object) return E_POINTER;
    *object = nullptr;
    if (clsid != kClsid) return CLASS_E_CLASSNOTAVAILABLE;
    return g_factory.QueryInterface(riid, object);
}

STDAPI DllCanUnloadNow() { return g_references == 0 ? S_OK : S_FALSE; }

/// Registers for the current user only (HKEY_CURRENT_USER), so no administrator rights are needed.
STDAPI DllRegisterServer() {
    wchar_t path[MAX_PATH * 4];
    const DWORD length = GetModuleFileNameW(g_module, path, static_cast<DWORD>(std::size(path)));
    const std::wstring dll(path, length);
    const std::wstring server = std::wstring(kClsidKey) + L"\\InprocServer32";
    if (setString(HKEY_CURRENT_USER, kClsidKey, nullptr, L"Glance PDF thumbnails") ||
        setString(HKEY_CURRENT_USER, server.c_str(), nullptr, dll) ||
        setString(HKEY_CURRENT_USER, server.c_str(), L"ThreadingModel", L"Apartment") ||
        setString(HKEY_CURRENT_USER, kThumbnailHandlerKey, nullptr, kClsidString)) {
        return SELFREG_E_CLASS;
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);  // Explorer picks it up now
    return S_OK;
}

STDAPI DllUnregisterServer() {
    // Only remove the PDF handler entry if it's still ours.
    wchar_t current[64] = L"";
    DWORD size = sizeof(current);
    if (RegGetValueW(HKEY_CURRENT_USER, kThumbnailHandlerKey, nullptr, RRF_RT_REG_SZ, nullptr, current, &size) == ERROR_SUCCESS &&
        _wcsicmp(current, kClsidString) == 0) {
        RegDeleteKeyW(HKEY_CURRENT_USER, kThumbnailHandlerKey);
    }
    RegDeleteTreeW(HKEY_CURRENT_USER, kClsidKey);
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return S_OK;
}
