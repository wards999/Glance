#pragma once

#include "Adjustments.h"

#include <optional>
#include <string>

/// Adjustments remembered per file (JPEGView's "Save parameters to DB"), reapplied whenever
/// that file is opened. Stored in %APPDATA%\Glance\Parameters.ini.
namespace ParameterStore {

std::optional<Adjustments> load(const std::wstring& path);
void save(const std::wstring& path, const Adjustments& adjustments);
void clear(const std::wstring& path);

}  // namespace ParameterStore

/// Where you were in each PDF, so it opens there next time. Stored in %APPDATA%\Glance\Pages.ini.
namespace PdfPlaces {

struct Place {
    int page = 1;    // the first page on screen (1-based)
    int screen = 0;  // which screen (page or spread) that was, in `layout`
    int layout = 0;  // the page layout then: if it's unchanged, `screen` is used directly
};

std::optional<Place> load(const std::wstring& path);
/// Page 1 forgets the file (it opens there anyway).
void save(const std::wstring& path, const Place& place);

}  // namespace PdfPlaces
