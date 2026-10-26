#pragma once

#include <windows.h>

#include <optional>
#include <string>

/// Asks for a PDF's password. Modal; empty result if cancelled.
namespace PasswordDialog {

std::optional<std::wstring> ask(HWND owner, const std::wstring& fileName, bool retry);

}  // namespace PasswordDialog
