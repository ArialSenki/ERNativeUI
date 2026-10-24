#include "mod_settings_direct_open.hpp"

#include "test_assertions.hpp"

namespace {

using erui::detail::kGameOptionsPanelCategory;
using erui::detail::kModSettingsPanelCategory;
using erui::detail::should_build_game_options_before_mod_settings;

void test_menu_opened_on_mod_settings() {
    ERUI_TEST_CHECK(should_build_game_options_before_mod_settings(
        kModSettingsPanelCategory, false, false, false));
}

void test_tab_change_keeps_native_path() {
    // A previous panel exists: the switch already copies its state.
    ERUI_TEST_CHECK(!should_build_game_options_before_mod_settings(
        kModSettingsPanelCategory, true, false, false));
    // Returning to an already built Hub or with Game Options cached.
    ERUI_TEST_CHECK(!should_build_game_options_before_mod_settings(
        kModSettingsPanelCategory, false, true, false));
    ERUI_TEST_CHECK(!should_build_game_options_before_mod_settings(
        kModSettingsPanelCategory, false, false, true));
}

void test_other_categories_untouched() {
    for (std::int32_t category = 0; category < 10; ++category) {
        if (category == kModSettingsPanelCategory) continue;
        ERUI_TEST_CHECK(!should_build_game_options_before_mod_settings(
            category, false, false, false));
    }
    ERUI_TEST_CHECK(kGameOptionsPanelCategory != kModSettingsPanelCategory);
}

} // namespace

int main() {
    test_menu_opened_on_mod_settings();
    test_tab_change_keeps_native_path();
    test_other_categories_untouched();
    return 0;
}
