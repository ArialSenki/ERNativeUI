#include "host_loader.h"
#include "connection_control.h"

#include <ernativeui/ERNativeUI.hpp>

#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>

namespace {

#define CHECK(condition) do { \
    if (!(condition)) { \
        std::fprintf(stderr, "check failed at %s:%d: %s\n", \
            __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (false)

void button_pressed() noexcept {}

template <typename Function>
Function resolve(HMODULE module, const char* name) noexcept {
    const FARPROC symbol = GetProcAddress(module, name);
    Function function{};
    if (!symbol || sizeof(symbol) != sizeof(function)) return function;
    std::memcpy(&function, &symbol, sizeof(function));
    return function;
}

std::wstring observed_text(
    const ERUI_TestTitleScreenTextObservation& observation) {
    std::wstring result;
    result.reserve(observation.text_length);
    for (std::uint32_t index = 0; index < observation.text_length; ++index) {
        result.push_back(static_cast<wchar_t>(observation.text[index]));
    }
    return result;
}

} // namespace

int main(int argc, char** argv) {
    static_assert(ERUI_API_VERSION_CURRENT == ERUI_API_VERSION_1_3,
        "not a frozen API 1.3 wrapper client");
    static_assert(ERUI_API_V1_3_SIZE == 384u,
        "frozen API 1.3 prefix size drift");
    static_assert(sizeof(ERUI_Api) == 384u, "frozen table drift");
    static_assert(offsetof(ERUI_Api, add_tab) == 368u,
        "API 1.3 custom-tab entry moved");
    static_assert(offsetof(ERUI_Api, add_title_screen_text) == 376u,
        "API 1.3 title-screen entry moved");

    if (argc != 2) return 2;

    ERUI_LoadedTestHost host{};
    CHECK(erui_load_test_host(argv[1], &host));

    auto connected = erui::connect();
    CHECK(connected.success());
    CHECK(connected.value().api_version() == ERUI_API_VERSION_1_3);
    CHECK(connected.value().supports(erui::Capability::custom_tabs));
    CHECK(connected.value().supports(erui::Capability::title_screen_text));
    const auto get_title_observation =
        resolve<ERUI_TestGetTitleScreenTextObservationFn>(
            host.module, "ERUI_TestGetTitleScreenTextObservation");
    const bool contract_host = GetProcAddress(
        host.module, "ERUI_TestGetConnectionSnapshot") != nullptr;
    CHECK(!contract_host || get_title_observation != nullptr);

    std::array<std::uint8_t, ERUI_TAB_ICON_RGBA8_BYTE_COUNT> pixels{};
    for (std::size_t offset = 3; offset < pixels.size(); offset += 4) {
        pixels[offset] = 255u;
    }
    erui::ProviderOptions provider{};
    provider.provider_id = "tests.frozen-v1-3-custom-tabs-cpp";
    provider.display_name = L"Frozen custom-tabs C++ wrapper";
    provider.owner_module = GetModuleHandleW(nullptr);
    erui::RowHandle row{ERUI_INVALID_ROW};
    const auto result = connected.value().register_menu(
        provider,
        [&](erui::Menu& menu) noexcept {
            menu.add_title_screen_text(
                erui::TitleScreenCorner::top_left,
                {L"Frozen wrapper test"});
            menu.add_title_screen_text(
                erui::TitleScreenCorner::bottom_right,
                {
                    L"Colored wrapper test",
                    erui::Color{216u, 167u, 103u},
                });
            erui::TabOptions tab{};
            tab.tab_id = "settings";
            tab.title = L"Settings";
            tab.preferred_order = 25;
            tab.icon = {pixels.data(), pixels.size()};
            erui::Page page = menu.add_tab(tab);
            row = page.add_button<&button_pressed>(
                L"Apply", L"Ordinary rows work on custom-tab pages");

            pixels[3] = 0u;
            pixels[7] = 127u;
            pixels[11] = 255u;
            erui::TabOptions alpha{};
            alpha.tab_id = "transparent";
            alpha.title = L"Transparent";
            alpha.icon = {
                pixels.data(),
                pixels.size()};
            (void)menu.add_tab(alpha);

            erui::TabOptions fallback{};
            fallback.tab_id = "advanced";
            fallback.title = L"Advanced";
            (void)menu.add_tab(fallback);
        });
    CHECK(result.success());
    CHECK(result.value().api_version() == ERUI_API_VERSION_1_3);
    CHECK(result.value().supports(erui::Capability::custom_tabs));
    CHECK(result.value().supports(erui::Capability::title_screen_text));
    CHECK(row != ERUI_INVALID_ROW);

    // The frozen contract fixture exposes a test-only descriptor capture
    // seam. The current-host compatibility run deliberately lacks that seam
    // and continues with the public validation and budget assertions below.
    if (get_title_observation) {
        ERUI_TestTitleScreenTextObservation observed{};
        observed.size = sizeof(observed);
        CHECK(get_title_observation(0, &observed) == ERUI_OK);
        CHECK(observed.descriptor_size == sizeof(ERUI_TitleScreenTextDesc));
        CHECK(observed.flags == 0);
        CHECK(observed.color.red == 0);
        CHECK(observed.color.green == 0);
        CHECK(observed.color.blue == 0);
        CHECK(observed.color.reserved == 0);
        CHECK(observed.corner == ERUI_TITLE_SCREEN_CORNER_TOP_LEFT);
        CHECK(observed_text(observed) == L"Frozen wrapper test");

        observed = {};
        observed.size = sizeof(observed);
        CHECK(get_title_observation(1, &observed) == ERUI_OK);
        CHECK(observed.descriptor_size == sizeof(ERUI_TitleScreenTextDesc));
        CHECK(observed.flags == ERUI_TITLE_SCREEN_TEXT_USE_CUSTOM_COLOR);
        CHECK(observed.color.red == 216u);
        CHECK(observed.color.green == 167u);
        CHECK(observed.color.blue == 103u);
        CHECK(observed.color.reserved == 0);
        CHECK(observed.corner == ERUI_TITLE_SCREEN_CORNER_BOTTOM_RIGHT);
        CHECK(observed_text(observed) == L"Colored wrapper test");

        observed = {};
        observed.size = sizeof(observed);
        CHECK(get_title_observation(2, &observed) == ERUI_NOT_FOUND);
    }

    // Two committed lines leave fourteen process-wide slots. A poisoned
    // draft may stage all fourteen, but aborting it must release the draft so
    // a replacement provider can commit the same fourteen lines.
    erui::ProviderOptions abandoned_title_provider{};
    abandoned_title_provider.provider_id =
        "tests.current-v1-3-abandoned-title-budget-cpp";
    abandoned_title_provider.display_name = L"Abandoned title budget";
    abandoned_title_provider.owner_module = GetModuleHandleW(nullptr);
    const auto abandoned_title = connected.value().register_menu(
        abandoned_title_provider,
        [&](erui::Menu& menu) noexcept {
            for (std::uint32_t index = 0;
                 index < ERUI_TITLE_SCREEN_TEXT_MAX_LINES - 2u;
                 ++index) {
                menu.add_title_screen_text(
                    erui::TitleScreenCorner::bottom_left,
                    {L"Abandoned title line"});
            }
            menu.add_title_screen_text(
                erui::TitleScreenCorner::bottom_left,
                {L"Invalid\ntitle line"});
        });
    CHECK(!abandoned_title.success());
    CHECK(abandoned_title.error().native_result() == ERUI_INVALID_ARGUMENT);

    erui::ProviderOptions replacement_title_provider{};
    replacement_title_provider.provider_id =
        "tests.current-v1-3-replacement-title-budget-cpp";
    replacement_title_provider.display_name = L"Replacement title budget";
    replacement_title_provider.owner_module = GetModuleHandleW(nullptr);
    const auto replacement_title = connected.value().register_menu(
        replacement_title_provider,
        [&](erui::Menu& menu) noexcept {
            for (std::uint32_t index = 0;
                 index < ERUI_TITLE_SCREEN_TEXT_MAX_LINES - 2u;
                 ++index) {
                menu.add_title_screen_text(
                    erui::TitleScreenCorner::bottom_left,
                    {L"Replacement title line"});
            }
        });
    CHECK(replacement_title.success());

    erui::ProviderOptions exhausted_title_provider{};
    exhausted_title_provider.provider_id =
        "tests.current-v1-3-exhausted-title-budget-cpp";
    exhausted_title_provider.display_name = L"Exhausted title budget";
    exhausted_title_provider.owner_module = GetModuleHandleW(nullptr);
    const auto exhausted_title = connected.value().register_menu(
        exhausted_title_provider,
        [&](erui::Menu& menu) noexcept {
            menu.add_title_screen_text(
                erui::TitleScreenCorner::top_right,
                {L"No committed capacity remains"});
        });
    CHECK(!exhausted_title.success());
    CHECK(exhausted_title.error().native_result() ==
        ERUI_TITLE_SCREEN_TEXT_LIMIT_REACHED);

    erui::ProviderOptions invalid_provider{};
    invalid_provider.provider_id = "tests.current-v1-3-invalid-tab-cpp";
    invalid_provider.display_name = L"Invalid custom tab";
    invalid_provider.owner_module = GetModuleHandleW(nullptr);
    pixels[3] = 0u;
    const auto invalid = connected.value().register_menu(
        invalid_provider,
        [&](erui::Menu& menu) noexcept {
            erui::TabOptions tab{};
            tab.tab_id = "invalid";
            tab.title = L"Invalid";
            tab.icon = {
                pixels.data(),
                pixels.size(),
                static_cast<erui::TabIconFormat>(2u)};
            (void)menu.add_tab(tab);
        });
    CHECK(!invalid.success());
    CHECK(invalid.error().native_result() == ERUI_INVALID_ARGUMENT);

    erui::ProviderOptions oversized_provider{};
    oversized_provider.provider_id =
        "tests.current-v1-3-oversized-tab-title-cpp";
    oversized_provider.display_name = L"Oversized custom tab title";
    oversized_provider.owner_module = GetModuleHandleW(nullptr);
    const std::wstring oversized_title(
        ERUI_CUSTOM_TAB_TITLE_MAX_CODE_UNITS + 1u, L'X');
    const auto oversized = connected.value().register_menu(
        oversized_provider,
        [&](erui::Menu& menu) noexcept {
            erui::TabOptions tab{};
            tab.tab_id = "oversized";
            tab.title = oversized_title;
            (void)menu.add_tab(tab);
        });
    CHECK(!oversized.success());
    CHECK(oversized.error().native_result() == ERUI_INVALID_ARGUMENT);

    erui::ProviderOptions invalid_title_provider{};
    invalid_title_provider.provider_id =
        "tests.current-v1-3-invalid-title-screen-text-cpp";
    invalid_title_provider.display_name = L"Invalid title-screen text";
    invalid_title_provider.owner_module = GetModuleHandleW(nullptr);
    const auto invalid_title = connected.value().register_menu(
        invalid_title_provider,
        [&](erui::Menu& menu) noexcept {
            menu.add_title_screen_text(
                erui::TitleScreenCorner::top_right,
                {L"First line\nSecond line"});
        });
    CHECK(!invalid_title.success());
    CHECK(invalid_title.error().native_result() == ERUI_INVALID_ARGUMENT);

    erui::ProviderOptions invalid_corner_provider{};
    invalid_corner_provider.provider_id =
        "tests.current-v1-3-invalid-title-screen-corner-cpp";
    invalid_corner_provider.display_name = L"Invalid title-screen corner";
    invalid_corner_provider.owner_module = GetModuleHandleW(nullptr);
    const auto invalid_corner = connected.value().register_menu(
        invalid_corner_provider,
        [&](erui::Menu& menu) noexcept {
            menu.add_title_screen_text(
                static_cast<erui::TitleScreenCorner>(0u),
                {L"Invalid corner"});
        });
    CHECK(!invalid_corner.success());
    CHECK(invalid_corner.error().native_result() == ERUI_INVALID_ARGUMENT);

    erui_unload_test_host(&host);
    return 0;
}
