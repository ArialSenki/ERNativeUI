#pragma once

// Fork: patches the settings movies (02_040_optionsetting.gfx and
// 02_042_pc_graphicsetting.gfx) in memory while Scaleform opens them, so the
// package does not replace those files and stays compatible with other mods
// that ship their own copies. The hook sits on CS::CSScaleformFileOpener's
// OpenFile override, found through RTTI on every game build.

#include <string>

namespace erui::host {

// Installs the OpenFile hook. Call as early as possible: a movie opened
// before installation is not patched. Returns false when the game's
// Scaleform types could not be validated (nothing is hooked then).
bool install_runtime_gfx(bool trace_urls = false) noexcept;

[[nodiscard]] bool runtime_gfx_installed() noexcept;

// Called once the host knows the menu language. Returns an in-game warning
// for movies that could not be patched so far and arms later warnings.
[[nodiscard]] std::wstring runtime_gfx_ready(bool spanish) noexcept;

} // namespace erui::host
