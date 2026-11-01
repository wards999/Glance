#pragma once

#include "Adjustments.h"

#include <optional>
#include <string>

/// Adjustments remembered per file ("Save parameters to DB"), reapplied whenever
/// that file is opened. Stored in Parameters.ini, beside Glance.ini (see dataFile in Settings.h).
namespace ParameterStore {

std::optional<Adjustments> load(const std::wstring& path);
void save(const std::wstring& path, const Adjustments& adjustments);
void clear(const std::wstring& path);

}  // namespace ParameterStore

/// Where you were in each PDF, so it opens there next time. Stored in Pages.ini, beside Glance.ini.
namespace PdfPlaces {

struct Place {
    int page = 1;    // the first page on screen (1-based)
    int screen = 0;  // which screen (page or spread) that was, in `layout`
    int layout = 0;  // the page layout then: if it's unchanged, `screen` is used directly
    // The zoom you left it at ("Remember each PDF's zoom"): a multiple of fitting the window,
    // 0 for fitted, and the point in the middle of the view as a fraction of the screen's size.
    double zoom = 0;
    double centerX = 0.5;
    double centerY = 0.5;
};

std::optional<Place> load(const std::wstring& path);
/// Page 1, fitted, forgets the file (it opens like that anyway).
void save(const std::wstring& path, const Place& place);

}  // namespace PdfPlaces
