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
// menu_background_alpha_percent < 100 makes the full-screen backdrop of both
// movies translucent ([Gfx] MenuBackgroundAlpha).
bool install_runtime_gfx(
    bool trace_urls = false, unsigned menu_background_alpha_percent = 100) noexcept;

[[nodiscard]] bool runtime_gfx_installed() noexcept;

// HUD layout applied to the HUD movies (01_000_fe, 01_001_fe_soul,
// 01_002_fe_saveicon) when Scaleform opens them: screen_edges moves the
// edge-anchored blocks out of the 16:9 frame on wider screens, scale_percent
// (50-150) resizes them around their screen anchor. Takes effect the next
// time the game loads the HUD. Callable at any time, also before install.
// menu_edges does the same for the menus with a layout table (pause menu,
// equipment, inventory, status, options...), which are never scaled.
void set_hud_layout(bool screen_edges, unsigned scale_percent, bool menu_edges) noexcept;

// Called once the host knows the menu language. Returns an in-game warning
// for movies that could not be patched so far and arms later warnings.
[[nodiscard]] std::wstring runtime_gfx_ready(bool spanish) noexcept;

} // namespace erui::host
