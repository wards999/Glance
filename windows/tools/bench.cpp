// Times Glance's decoder on every image in a folder: the window-sized decode it opens with, and
// a full-size decode for comparison (which is roughly what a viewer that always decodes in full pays).
//
//   glance-bench <folder> [window-width window-height] [runs]

#include "Decoder.h"
#include "Navigator.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

double median(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    return values.empty() ? 0 : values[values.size() / 2];
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::fwprintf(stderr, L"usage: glance-bench <folder> [window-width window-height] [runs]\n");
        return 1;
    }
    const std::wstring folder = argv[1];
    const UINT boxWidth = argc > 3 ? static_cast<UINT>(_wtoi(argv[2])) : 1920;
    const UINT boxHeight = argc > 3 ? static_cast<UINT>(_wtoi(argv[3])) : 1080;
    const int runs = argc > 4 ? std::max(1, _wtoi(argv[4])) : 3;

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    auto list = Decoder::supportedExtensions();
    std::unordered_set<std::wstring> extensions(list.begin(), list.end());
    auto files = Navigator::scan(folder, extensions);
    std::wprintf(L"%zu images, window %ux%u, median of %d runs\n\n", files.size(), boxWidth, boxHeight, runs);
    std::wprintf(L"%-28ls %12ls %14ls %12ls\n", L"file", L"size", L"fit decode", L"full decode");

    Decoder decoder;
    std::vector<double> fitTimes, fullTimes;
    for (const auto& path : files) {
        std::vector<double> fit, full;
        UINT outW = 0, outH = 0, w = 0, h = 0;
        for (int run = 0; run < runs; ++run) {
            double start = nowMs();
            auto image = decoder.decode(path, 0, DecodeRequest{boxWidth, boxHeight, 0});
            fit.push_back(nowMs() - start);
            if (image) {
                outW = image->width;
                outH = image->height;
                w = image->info.width;
                h = image->info.height;
            }
            start = nowMs();
            decoder.decode(path, 0, DecodeRequest{});
            full.push_back(nowMs() - start);
        }
        fitTimes.push_back(median(fit));
        fullTimes.push_back(median(full));
        std::wstring name = path.substr(path.find_last_of(L'\\') + 1);
        wchar_t size[32], decoded[32];
        swprintf(size, 32, L"%ux%u", w, h);
        swprintf(decoded, 32, L"%ux%u", outW, outH);
        std::wprintf(L"%-28ls %12ls %7.1f ms %-9ls %7.1f ms\n", name.c_str(), size, median(fit), decoded, median(full));
    }
    std::wprintf(L"\nmedian per image: fit %.1f ms, full %.1f ms\n", median(fitTimes), median(fullTimes));
    CoUninitialize();
    return 0;
}
