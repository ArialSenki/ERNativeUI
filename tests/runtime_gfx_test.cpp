// Fork: runtime GFX patching. Fixture paths are optional local files (game
// assets and the game executable are never committed); missing fixtures are
// reported as skipped.

#include "runtime_gfx_locate.hpp"
#include "settings_movie.hpp"

#include <Windows.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool condition, const char* what) {
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition) ++g_failures;
}

std::vector<std::uint8_t> read_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

void test_classification() {
    using erui::gfx::SettingsMovie;
    using erui::gfx::classify_settings_movie;
    check(classify_settings_movie("menu:/Win/02_040_optionsetting.gfx") ==
            SettingsMovie::game_options, "classify Scaleform Win URL");
    check(classify_settings_movie("menu:/02_042_pc_graphicsetting.gfx") ==
            SettingsMovie::advanced_settings, "classify fallback URL");
    check(classify_settings_movie("C:\\Mods\\menu\\win\\02_040_OPTIONSETTING.GFX") ==
            SettingsMovie::game_options, "classify path ignoring case");
    check(classify_settings_movie("menu:/Win/02_043_pc_keyconfiguration.gfx") ==
            SettingsMovie::none, "ignore other movies");
    check(classify_settings_movie("02_040_optionsetting.gfx.bak") == SettingsMovie::none,
        "ignore similar names");
}

void test_movie(const char* name, erui::gfx::SettingsMovie movie) {
#if defined(ERNATIVEUI_TEST_VANILLA_GFX_DIR) && defined(ERNATIVEUI_TEST_EXPECTED_GFX_DIR)
    const auto vanilla = read_file(std::filesystem::path(ERNATIVEUI_TEST_VANILLA_GFX_DIR) / name);
    const auto expected = read_file(std::filesystem::path(ERNATIVEUI_TEST_EXPECTED_GFX_DIR) / name);
    if (vanilla.empty() || expected.empty()) {
        std::printf("skip %s fixtures missing\n", name);
        return;
    }
    const auto patched = erui::gfx::patch_settings_movie(movie, vanilla);
    check(patched.success(), (std::string(name) + " patches from vanilla").c_str());
    check(patched.output == expected,
        (std::string(name) + " equals the shipped r121 file byte for byte").c_str());
    const auto again = erui::gfx::patch_settings_movie(movie, expected);
    check(again.success() && again.report.already_satisfied && again.output == expected,
        (std::string(name) + " is idempotent on an already patched file").c_str());
    std::vector<std::uint8_t> truncated(vanilla.begin(), vanilla.begin() + vanilla.size() / 2);
    const auto broken = erui::gfx::patch_settings_movie(movie, truncated);
    check(!broken.success(), (std::string(name) + " rejects a truncated movie").c_str());
#else
    std::printf("skip %s (no fixture directories configured)\n", name);
#endif
}

void test_locate() {
#ifdef ERNATIVEUI_TEST_GAME_EXE
    HMODULE image = LoadLibraryExW(
        L"" ERNATIVEUI_TEST_GAME_EXE, nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!image) {
        std::printf("skip locate: game executable not loadable (%lu)\n", GetLastError());
        return;
    }
    erui::native::ModuleView game{};
    check(game.initialize(image), "map game image");
    erui::host::RuntimeGfxTargets targets{};
    const bool found = erui::host::locate_runtime_gfx_targets(game, targets);
    if (!found) std::printf("     failure: %s\n", targets.failure ? targets.failure : "?");
    check(found, "locate CSScaleformFileOpener and MemoryFile");
    if (found) {
        const auto rva = [&](const void* p) {
            return static_cast<unsigned long long>(
                static_cast<const std::uint8_t*>(p) - game.base());
        };
        std::printf("     opener vtable 0x%llX OpenFile 0x%llX MemoryFile vtable 0x%llX\n",
            rva(targets.file_opener_vtable), rva(targets.open_file),
            rva(targets.memory_file_vtable));
        // Exact values for Elden Ring 2.7.1.0 (the build analysed offline).
        check(rva(targets.open_file) == 0xD6D280, "OpenFile RVA matches static analysis");
        check(rva(targets.memory_file_vtable) == 0x2BA7D70,
            "MemoryFile vtable RVA matches static analysis");
    }
    FreeLibrary(image);
#else
    std::printf("skip locate (no game executable configured)\n");
#endif
}

} // namespace

int main() {
    test_classification();
    test_movie("02_040_optionsetting.gfx", erui::gfx::SettingsMovie::game_options);
    test_movie("02_042_pc_graphicsetting.gfx", erui::gfx::SettingsMovie::advanced_settings);
    test_locate();
    std::printf("%s (%d failures)\n", g_failures ? "FAILED" : "PASSED", g_failures);
    return g_failures ? 1 : 0;
}
