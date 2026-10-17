#pragma once

// Fork: runtime selection of the settings-movie transformations. The host
// applies these to whatever 02_040/02_042 movie the game is about to load
// (vanilla, another mod's replacement, or an already patched copy), so the
// package no longer has to ship and override those files.

#include "gfx_patch.hpp"

#include <cstdint>
#include <span>
#include <string_view>

namespace erui::gfx {

enum class SettingsMovie : std::uint8_t {
    none,
    game_options,       // 02_040_optionsetting.gfx
    advanced_settings,  // 02_042_pc_graphicsetting.gfx
};

struct SettingsMovieOptions {
    // 02_040: dedicated top-level tab before Exit.
    bool mod_settings_top_level_tab{true};
    // Both movies: wide slider value field.
    bool wide_slider_values{true};
    // 02_040: 0 keeps the movie's own row count.
    std::uint16_t game_options_rows{};
    std::uint16_t camera_options_rows{};
    TextInputPresentation game_options_text_input{TextInputPresentation::native};
    ColorPickerPresentation game_options_color_picker{ColorPickerPresentation::native};
    TextInputPresentation advanced_text_input{TextInputPresentation::character_name};
    ColorPickerPresentation advanced_color_picker{
        ColorPickerPresentation::character_creation};
};

// Classifies a Scaleform URL or file path by its file name, ignoring case
// and directories ("menu:/Win/02_040_optionsetting.gfx" -> game_options).
[[nodiscard]] SettingsMovie classify_settings_movie(std::string_view url) noexcept;

[[nodiscard]] const char* settings_movie_name(SettingsMovie movie) noexcept;

// Applies the configured transformations. The result is byte-idempotent: an
// already patched movie comes back unchanged with report.already_satisfied.
[[nodiscard]] PatchResult patch_settings_movie(
    SettingsMovie movie,
    std::span<const std::uint8_t> input,
    const SettingsMovieOptions& options = {}) noexcept;

} // namespace erui::gfx
