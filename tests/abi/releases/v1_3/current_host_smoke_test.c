#include "host_loader.h"

#include <stdio.h>
#include <string.h>
#include <wchar.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "check failed at %s:%d: %s\n", \
            __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

static ERUI_StringView string_view(const char* value) {
    ERUI_StringView result = {0};
    result.data = value;
    result.length = (uint32_t)strlen(value);
    return result;
}

static ERUI_Utf16View utf16_view(const wchar_t* value) {
    ERUI_Utf16View result = {0};
    result.data = (const uint16_t*)value;
    result.length = (uint32_t)wcslen(value);
    return result;
}

int main(int argc, char** argv) {
    ERUI_LoadedTestHost host = {0};
    ERUI_Api api = {0};
    ERUI_ProviderDesc provider_desc = {0};
    ERUI_ProviderHandle provider = ERUI_INVALID_PROVIDER;
    ERUI_PageHandle root = ERUI_INVALID_PAGE;
    ERUI_TitleScreenTextDesc title_desc = {0};
    ERUI_TabDesc tab_desc = {0};
    ERUI_PageHandle tab = ERUI_INVALID_PAGE;
    ERUI_ToggleDesc toggle_desc = {0};
    ERUI_RowHandle row = ERUI_INVALID_ROW;

    _Static_assert(ERUI_API_VERSION_CURRENT == ERUI_API_VERSION_1_3,
        "test must compile against the frozen API 1.3 header");
    _Static_assert(sizeof(ERUI_Api) == 384u,
        "test must compile against the frozen API 1.3 table");
    if (argc != 2) return 2;
    CHECK(erui_load_test_host(argv[1], &host));
    CHECK(host.get_api != NULL);

    api.size = ERUI_API_V1_3_SIZE;
    CHECK(host.get_api(ERUI_API_VERSION_1_3, &api) == ERUI_OK);
    CHECK(api.size == ERUI_API_V1_3_SIZE);
    CHECK(api.api_version == ERUI_API_VERSION_1_3);
    CHECK((api.capabilities & ERUI_CAP_CUSTOM_TABS) != 0u);
    CHECK((api.capabilities & ERUI_CAP_TITLE_SCREEN_TEXT) != 0u);
    CHECK(api.add_tab != NULL);
    CHECK(api.add_title_screen_text != NULL);

    provider_desc.size = sizeof(provider_desc);
    provider_desc.api_version = ERUI_API_VERSION_1_3;
    provider_desc.owner_module = GetModuleHandleW(NULL);
    provider_desc.provider_id = string_view("tests.v1-3-c-to-current");
    provider_desc.display_name = utf16_view(L"Frozen API 1.3 C client");
    CHECK(api.register_provider(
        &provider_desc, &provider, &root) == ERUI_OK);
    CHECK(provider != ERUI_INVALID_PROVIDER);
    CHECK(root != ERUI_INVALID_PAGE);

    title_desc.size = sizeof(title_desc);
    title_desc.text = utf16_view(L"Frozen API 1.3 title text");
    title_desc.corner = ERUI_TITLE_SCREEN_CORNER_TOP_LEFT;
    CHECK(api.add_title_screen_text(provider, &title_desc) == ERUI_OK);

    tab_desc.size = sizeof(tab_desc);
    tab_desc.tab_id = string_view("settings");
    tab_desc.title = utf16_view(L"Frozen API 1.3 tab");
    tab_desc.preferred_order = 13;
    CHECK(api.add_tab(provider, &tab_desc, &tab) == ERUI_OK);
    CHECK(tab != ERUI_INVALID_PAGE);
    CHECK(tab != root);

    toggle_desc.size = sizeof(toggle_desc);
    toggle_desc.label = utf16_view(L"Frozen toggle");
    toggle_desc.help = utf16_view(L"Ordinary row on a custom tab");
    toggle_desc.initial_value = 1u;
    toggle_desc.enabled = 1u;
    CHECK(api.add_toggle(provider, tab, &toggle_desc, &row) == ERUI_OK);
    CHECK(row != ERUI_INVALID_ROW);
    CHECK(api.commit_provider(provider) == ERUI_OK);

    erui_unload_test_host(&host);
    return 0;
}
