#pragma once

#include <filesystem>
#include <string>

namespace erui::host {

// Fork: verifies that the settings movies the game will load are the patched
// ones shipped with the host (another mod may replace them). Only loose-file
// installs are checked: the host DLL must live inside the game folder, where
// the game resolves menu\win\*.gfx. Returns a user-facing message listing the
// missing features, or an empty string when everything matches or the layout
// cannot be checked. Details are written to the log.
[[nodiscard]] std::wstring check_settings_gfx(
    const std::filesystem::path& host_directory,
    bool spanish) noexcept;

} // namespace erui::host
