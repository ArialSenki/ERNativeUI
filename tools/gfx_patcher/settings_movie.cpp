#include "settings_movie.hpp"

#include <cctype>

namespace erui::gfx {
namespace {

bool equals_ignore_case(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t index = 0; index < a.size(); ++index) {
        if (std::tolower(static_cast<unsigned char>(a[index])) !=
            std::tolower(static_cast<unsigned char>(b[index]))) {
            return false;
        }
    }
    return true;
}

} // namespace

SettingsMovie classify_settings_movie(std::string_view url) noexcept {
    const std::size_t slash = url.find_last_of("/\\:");
    const std::string_view name =
        slash == std::string_view::npos ? url : url.substr(slash + 1);
    if (equals_ignore_case(name, "02_040_optionsetting.gfx")) {
        return SettingsMovie::game_options;
    }
    if (equals_ignore_case(name, "02_042_pc_graphicsetting.gfx")) {
        return SettingsMovie::advanced_settings;
    }
    return SettingsMovie::none;
}

const char* settings_movie_name(SettingsMovie movie) noexcept {
    switch (movie) {
    case SettingsMovie::game_options: return "02_040_optionsetting.gfx";
    case SettingsMovie::advanced_settings: return "02_042_pc_graphicsetting.gfx";
    default: return "(not a settings movie)";
    }
}

PatchResult patch_settings_movie(
    SettingsMovie movie,
    std::span<const std::uint8_t> input,
    const SettingsMovieOptions& options) noexcept {
    if (movie == SettingsMovie::game_options) {
        const Inspection inspection = inspect_game_options_panel(input);
        if (!inspection.success()) {
            PatchResult failed{};
            failed.error = inspection.error;
            failed.message = inspection.message;
            failed.report.before = inspection;
            return failed;
        }
        PatchOptions patch_options{};
        patch_options.game_options_rows = options.game_options_rows
            ? options.game_options_rows
            : inspection.game_options_rows;
        patch_options.camera_options_rows = options.camera_options_rows
            ? options.camera_options_rows
            : inspection.camera_options_rows;
        patch_options.text_input_presentation = options.game_options_text_input;
        patch_options.color_picker_presentation = options.game_options_color_picker;
        patch_options.mod_settings_top_level_tab = options.mod_settings_top_level_tab;
        patch_options.wide_slider_values = options.wide_slider_values;
        return patch_game_options_panel(input, patch_options);
    }
    if (movie == SettingsMovie::advanced_settings) {
        return patch_widget_presentations(
            input,
            options.advanced_text_input,
            options.advanced_color_picker,
            options.wide_slider_values);
    }
    PatchResult invalid{};
    invalid.error = ErrorCode::invalid_argument;
    invalid.message = "not a supported settings movie";
    return invalid;
}

} // namespace erui::gfx
