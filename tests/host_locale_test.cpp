#include "host_locale.hpp"

#include "test_assertions.hpp"

#include <filesystem>
#include <fstream>

int main() {
    const std::filesystem::path assets = ERNATIVEUI_TEST_ASSETS;

    const erui::host::HostLocale french =
        erui::host::load_host_locale(assets, "french");
    ERUI_TEST_CHECK(french.loaded);
    ERUI_TEST_CHECK(french.pagination.previous_label == L"Page précédente");
    ERUI_TEST_CHECK(french.pagination.next_label == L"Page suivante");

    const erui::host::HostLocale vietnamese =
        erui::host::load_host_locale(assets, "vietnamese");
    ERUI_TEST_CHECK(vietnamese.loaded);
    ERUI_TEST_CHECK(vietnamese.pagination.previous_label == L"Trang trước");
    ERUI_TEST_CHECK(vietnamese.pagination.previous_help ==
        L"Quay lại trang cài đặt trước đó.");
    ERUI_TEST_CHECK(vietnamese.pagination.next_label == L"Trang sau");
    ERUI_TEST_CHECK(vietnamese.pagination.next_help ==
        L"Mở trang cài đặt tiếp theo.");

    const erui::host::HostLocale missing =
        erui::host::load_host_locale(assets, "community-locale");
    ERUI_TEST_CHECK(!missing.loaded);
    ERUI_TEST_CHECK(missing.pagination.previous_label == L"Previous Page");
    ERUI_TEST_CHECK(missing.pagination.next_label == L"Next Page");

    const erui::host::HostLocale unsafe =
        erui::host::load_host_locale(assets, "../french");
    ERUI_TEST_CHECK(!unsafe.loaded);

    // Built-in text: no locales folder at all.
    const std::filesystem::path empty =
        std::filesystem::temp_directory_path() / L"erui-host-locale-empty";
    std::filesystem::create_directories(empty);
    const erui::host::HostLocale builtin_spanish =
        erui::host::load_host_locale(empty, "spanish");
    ERUI_TEST_CHECK(!builtin_spanish.loaded);
    ERUI_TEST_CHECK(builtin_spanish.pagination.menu_title == L"NativeTweaks");
    ERUI_TEST_CHECK(builtin_spanish.pagination.menu_help ==
        L"Ajustes proporcionados por los mods instalados.");
    ERUI_TEST_CHECK(builtin_spanish.pagination.previous_label == L"Página anterior");
    ERUI_TEST_CHECK(builtin_spanish.pagination.next_help ==
        L"Abrir la página de ajustes siguiente.");
    const erui::host::HostLocale builtin_japanese =
        erui::host::load_host_locale(empty, "japanese");
    ERUI_TEST_CHECK(builtin_japanese.pagination.next_label == L"次のページ");
    ERUI_TEST_CHECK(builtin_japanese.pagination.menu_help ==
        L"Settings provided by installed mods.");
    const erui::host::HostLocale builtin_english =
        erui::host::load_host_locale(empty, "english");
    ERUI_TEST_CHECK(builtin_english.pagination.menu_title == L"NativeTweaks");
    ERUI_TEST_CHECK(builtin_english.pagination.previous_label == L"Previous Page");

    // Every bundled locale file matches the built-in text, key by key.
    for (const char* identifier : {"arabic", "brazilian", "french", "german",
             "italian", "japanese", "koreana", "latam", "polish", "russian",
             "schinese", "spanish", "tchinese", "thai", "vietnamese"}) {
        const erui::host::HostLocale from_file =
            erui::host::load_host_locale(assets, identifier);
        const erui::host::HostLocale from_dll =
            erui::host::load_host_locale(empty, identifier);
        ERUI_TEST_CHECK(from_file.loaded);
        ERUI_TEST_CHECK(from_file.pagination.menu_title == from_dll.pagination.menu_title);
        ERUI_TEST_CHECK(from_file.pagination.menu_help == from_dll.pagination.menu_help);
        ERUI_TEST_CHECK(from_file.pagination.previous_label == from_dll.pagination.previous_label);
        ERUI_TEST_CHECK(from_file.pagination.previous_help == from_dll.pagination.previous_help);
        ERUI_TEST_CHECK(from_file.pagination.next_label == from_dll.pagination.next_label);
        ERUI_TEST_CHECK(from_file.pagination.next_help == from_dll.pagination.next_help);
    }

    // A file still overrides single keys; the rest stays built in.
    const std::filesystem::path override_dir =
        std::filesystem::temp_directory_path() / L"erui-host-locale-override";
    std::filesystem::create_directories(override_dir / L"locales");
    {
        std::ofstream file(override_dir / L"locales" / L"spanish.ini", std::ios::binary);
        file << "[Pagination]\nNextLabel = Siguiente\n";
    }
    const erui::host::HostLocale overridden =
        erui::host::load_host_locale(override_dir, "spanish");
    ERUI_TEST_CHECK(overridden.loaded);
    ERUI_TEST_CHECK(overridden.pagination.next_label == L"Siguiente");
    ERUI_TEST_CHECK(overridden.pagination.previous_label == L"Página anterior");
    std::error_code ignored{};
    std::filesystem::remove_all(override_dir, ignored);
    std::filesystem::remove_all(empty, ignored);
    return 0;
}
