#include "host_loader.h"

#include <ernativeui/ERNativeUI.hpp>

#include <chrono>
#include <cstdio>

#define CHECK(condition) do { \
    if (!(condition)) { \
        std::fprintf(stderr, "check failed at %s:%d: %s\n", \
            __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (false)

int main(int argc, char** argv) {
    using namespace std::chrono_literals;
    static_assert(ERUI_API_VERSION_CURRENT == ERUI_API_VERSION_1_3);
    static_assert(sizeof(ERUI_Api) == 384u);
    if (argc != 2) return 2;

    ERUI_LoadedTestHost host{};
    CHECK(erui_load_test_host(argv[1], &host));
    const erui::ConnectionResult connected = erui::connect(0ms);
    CHECK(connected.success());
    CHECK(connected.value().api_version() == ERUI_API_VERSION_1_3);
    CHECK(connected.value().supports(erui::Capability::custom_tabs));
    CHECK(connected.value().supports(erui::Capability::title_screen_text));

    erui::RowHandle row{ERUI_INVALID_ROW};
    erui::ProviderOptions options{};
    options.provider_id = "tests.v1-3-wrapper-to-current";
    options.display_name = L"Frozen API 1.3 wrapper client";
    options.owner_module = GetModuleHandleW(nullptr);
    const erui::RegistrationResult registered =
        connected.value().register_menu(
            options,
            [&](erui::Menu& menu) noexcept {
                menu.add_title_screen_text(
                    erui::TitleScreenCorner::top_left,
                    {L"Frozen API 1.3 title text"});

                erui::TabOptions tab_options{};
                tab_options.tab_id = "settings";
                tab_options.title = L"Frozen API 1.3 tab";
                tab_options.preferred_order = 13;
                erui::Page tab = menu.add_tab(tab_options);
                row = tab.add_toggle(
                    L"Frozen toggle",
                    L"Ordinary row on a custom tab",
                    1u);
            });
    CHECK(registered.success());
    CHECK(registered.value().supports(erui::Capability::custom_tabs));
    CHECK(registered.value().supports(erui::Capability::title_screen_text));
    CHECK(row != ERUI_INVALID_ROW);

    erui_unload_test_host(&host);
    return 0;
}
