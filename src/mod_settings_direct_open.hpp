#pragma once

#include <cstdint>

namespace erui::detail {

// Category codes in the Configuration composite's ten-slot panel cache.
inline constexpr std::int32_t kGameOptionsPanelCategory = 0;
inline constexpr std::int32_t kModSettingsPanelCategory = 4;

// The Hub page only draws its row widgets correctly when the native panel
// switch builds it with a previous panel to copy state from, as a tab change
// does. A menu reopened on Mod Settings switches straight into a fresh
// composite, so Game Options is built first there: the Hub then goes through
// exactly the tab-change path and Game Options stays hidden behind it.
[[nodiscard]] constexpr bool should_build_game_options_before_mod_settings(
    std::int32_t category,
    bool has_current_panel,
    bool mod_settings_cached,
    bool game_options_cached) noexcept {
    return category == kModSettingsPanelCategory && !has_current_panel &&
        !mod_settings_cached && !game_options_cached;
}

} // namespace erui::detail
