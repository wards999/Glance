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
