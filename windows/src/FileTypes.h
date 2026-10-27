#pragma once

#include <string>
#include <vector>

/// Glance's file types, for Explorer: one per extension ("JPG picture", "PDF document", "MKV
/// video"), each with the icon for its kind (picture, PDF or video) and opening in Glance. Also
/// lists Glance under Settings > Default apps, where you choose which of them Glance opens.
///
/// Everything is under HKEY_CURRENT_USER, for this user only: no administrator prompt. Windows
/// doesn't let apps make themselves the default, so the icons appear for the extensions you then
/// choose Glance for.
namespace FileTypes {

/// Icon resources in Glance.exe (glance.rc).
constexpr int kPictureIcon = 2;
constexpr int kPdfIcon = 3;
constexpr int kVideoIcon = 4;

/// Whether this copy of Glance has registered its file types, as this version registers them.
bool registered();
/// Registers the given extensions (lower case, no dot). False if the registry refused.
bool registerAll(const std::vector<std::wstring>& pictures, bool pdf, const std::vector<std::wstring>& videos);
/// Removes everything registerAll added.
void unregisterAll();
/// Opens Settings > Default apps at Glance's page.
void openDefaultApps();

}  // namespace FileTypes
