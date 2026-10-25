#pragma once

#include "Decoder.h"

#include <memory>
#include <string>
#include <vector>

/// PDF reading through Windows' built-in PDF renderer (Windows.Data.Pdf), so it adds nothing to
/// Glance's size and nothing loads until a PDF is opened. Each screen — one page, or a two-page
/// spread — becomes one bitmap, so the image pipeline caches and decodes ahead pages like photos.
namespace Pdf {

/// Page layout packed into one number for cache keys: mode × 2 + right-to-left, where mode is
/// 0 automatic, 1 single, 2 two pages, 3 two pages with cover.
int packLayout(int mode, bool rightToLeft);

bool isPdf(const std::wstring& path);
/// False in builds without the Windows PDF renderer (the MinGW build).
bool available();

/// Renders screen `index` just big enough for `request`. Null if the file can't be read
/// (damaged, or password-protected, which isn't supported yet).
std::shared_ptr<Decoded> decode(const std::wstring& path, UINT index, const DecodeRequest& request, int layout);

/// The pages (1-based, reading order) on each screen.
std::vector<std::vector<int>> spreads(const std::wstring& path, int layout);

}  // namespace Pdf
