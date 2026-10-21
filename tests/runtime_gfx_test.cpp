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

void test_backdrop_alpha(const char* name, erui::gfx::SettingsMovie movie) {
#if defined(ERNATIVEUI_TEST_VANILLA_GFX_DIR) && defined(ERNATIVEUI_TEST_EXPECTED_GFX_DIR)
    const auto vanilla = read_file(std::filesystem::path(ERNATIVEUI_TEST_VANILLA_GFX_DIR) / name);
    const auto expected = read_file(std::filesystem::path(ERNATIVEUI_TEST_EXPECTED_GFX_DIR) / name);
    if (vanilla.empty() || expected.empty()) {
        std::printf("skip %s backdrop fixtures missing\n", name);
        return;
    }
    const std::string label(name);
    const auto half = erui::gfx::set_menu_background_alpha(vanilla, 50);
    if (!half.success()) std::printf("     failure: %s\n", half.message.c_str());
    check(half.success() && !half.report.already_satisfied && half.report.bytes_added == 6,
        (label + " backdrop alpha 50% adds one color transform").c_str());
    const auto again = erui::gfx::set_menu_background_alpha(half.output, 50);
    check(again.success() && again.report.already_satisfied && again.output == half.output,
        (label + " backdrop alpha is idempotent").c_str());
    const auto restored = erui::gfx::set_menu_background_alpha(half.output, 100);
    check(restored.success() && restored.output == vanilla,
        (label + " backdrop alpha 100% restores the vanilla bytes").c_str());
    const auto untouched = erui::gfx::set_menu_background_alpha(vanilla, 100);
    check(untouched.success() && untouched.report.already_satisfied,
        (label + " backdrop alpha 100% leaves vanilla unchanged").c_str());

    erui::gfx::SettingsMovieOptions options{};
    options.menu_background_alpha_percent = 50;
    const auto combined = erui::gfx::patch_settings_movie(movie, vanilla, options);
    const auto expected_half = erui::gfx::set_menu_background_alpha(expected, 50);
    check(combined.success() && !combined.report.menu_background_skipped &&
            combined.output == expected_half.output,
        (label + " runtime patch with backdrop alpha = r121 file + alpha").c_str());
    const auto combined_again = erui::gfx::patch_settings_movie(movie, combined.output, options);
    check(combined_again.success() && combined_again.report.already_satisfied &&
            combined_again.output == combined.output,
        (label + " runtime patch with backdrop alpha is idempotent").c_str());
    const auto defaults = erui::gfx::patch_settings_movie(movie, vanilla);
    check(defaults.success() && defaults.output == expected,
        (label + " default options keep the backdrop untouched").c_str());
#else
    std::printf("skip %s backdrop (no fixture directories configured)\n", name);
#endif
}

void test_backdrop_rejects_foreign_movie() {
    std::vector<std::uint8_t> minimal = {
        'G', 'F', 'X', 0x0B, 0, 0, 0, 0, 0x00, 0x00, 0x18, 0x01, 0x00, 0x00, 0x00};
    minimal[4] = static_cast<std::uint8_t>(minimal.size());
    const auto result = erui::gfx::set_menu_background_alpha(minimal, 50);
    check(!result.success(), "backdrop alpha rejects a movie without MENU_BaseMain_01");
    check(!erui::gfx::set_menu_background_alpha({}, 50).success(),
        "backdrop alpha rejects empty input");
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
        // Exact values for the builds analysed offline: 2.7.1.0 (and its
        // 2.7.1.1 rebuild) and 2.7.2.0.
        const auto* const dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(game.base());
        const auto* const nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
            game.base() + dos->e_lfanew);
        const DWORD stamp = nt->FileHeader.TimeDateStamp;
        unsigned long long open_file = 0, memory_file = 0;
        if (stamp == 0x6A96B418 || stamp == 0x6A96BA95) {
            open_file = 0xD6D280;
            memory_file = 0x2BA7D70;
        } else if (stamp == 0x6AABF7E7) {
            open_file = 0xD6E970;
            memory_file = 0x2BAA020;
        }
        if (open_file) {
            check(rva(targets.open_file) == open_file, "OpenFile RVA matches static analysis");
            check(rva(targets.memory_file_vtable) == memory_file,
                "MemoryFile vtable RVA matches static analysis");
        } else {
            std::printf("skip exact RVAs: build 0x%lX not analysed offline\n", stamp);
        }
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
    test_backdrop_alpha("02_040_optionsetting.gfx", erui::gfx::SettingsMovie::game_options);
    test_backdrop_alpha("02_042_pc_graphicsetting.gfx",
        erui::gfx::SettingsMovie::advanced_settings);
    test_backdrop_rejects_foreign_movie();
    test_locate();
    std::printf("%s (%d failures)\n", g_failures ? "FAILED" : "PASSED", g_failures);
    return g_failures ? 1 : 0;
}
