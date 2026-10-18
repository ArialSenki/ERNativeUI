// Fork: runtime GFX cache (src/movie_cache.hpp). The cache has no entry limit:
// with the HUD/menu layout tables the game opens more movies than the old
// 32-entry cap, and a full cache served 02_040/02_042 unpatched (the Mod
// Settings tab vanished). Every movie must come back patched, buffers must
// stay put, and the settings + edge chain must still work at the end.

#include "hud_movie.hpp"
#include "movie_cache.hpp"
#include "settings_movie.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool condition, const std::string& what) {
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", what.c_str());
    if (!condition) ++g_failures;
}

std::vector<std::uint8_t> read_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

std::uint64_t fnv1a(const std::vector<std::uint8_t>& bytes) {
    std::uint64_t hash = 0xCBF29CE484222325ull;
    for (std::uint8_t byte : bytes) {
        hash ^= byte;
        hash *= 0x100000001B3ull;
    }
    return hash;
}

void test_unbounded() {
    erui::host::MovieCache cache;
    std::vector<const std::vector<std::uint8_t>*> served;
    constexpr int kEntries = 300;  // well above the old cap of 32
    for (int index = 0; index < kEntries; ++index) {
        std::vector<std::uint8_t> bytes(64 + index, static_cast<std::uint8_t>(index));
        served.push_back(cache.insert(static_cast<std::uint64_t>(index), bytes.size(), 0, bytes, true));
    }
    bool all_served = true;
    bool all_stable = true;
    for (int index = 0; index < kEntries; ++index) {
        const auto lookup = cache.find(static_cast<std::uint64_t>(index), 64u + index, 0);
        all_served = all_served && served[index] && lookup.found && lookup.output == served[index];
        all_stable = all_stable && served[index]->size() == 64u + index &&
            (*served[index])[0] == static_cast<std::uint8_t>(index);
    }
    check(cache.size() == kEntries, "cache keeps every entry (no limit)");
    check(all_served, "every movie is served patched after 300 inserts");
    check(all_stable, "served buffers stay valid while the cache grows");

    const auto* first = served[7];
    const auto* again = cache.insert(7, 71, 0, std::vector<std::uint8_t>(71, 0xEE), true);
    check(again == first && cache.size() == kEntries, "a key keeps its first buffer");
    const auto* variant = cache.insert(7, 71, 1ull << 63, std::vector<std::uint8_t>(71, 0xAA), true);
    check(variant && variant != first, "layout variants are separate entries");

    const auto* unchanged = cache.insert(999999, 10, 0, {}, false);
    const auto lookup = cache.find(999999, 10, 0);
    check(!unchanged && lookup.found && !lookup.output, "an unpatched result is remembered and served unchanged");

    const auto* kept = cache.keep_alive(std::vector<std::uint8_t>(5, 1));
    check(kept && kept->size() == 5 && cache.size() == kEntries + 2, "keep_alive does not use lookup entries");
}

// Mirrors the runtime with the real movies: every layout movie and the
// settings movies go through the cache in startup order, settings last.
void test_real_movies(const std::filesystem::path& folder) {
    using namespace erui::gfx;
    erui::host::MovieCache cache;
    const HudLayout layout{320.0f, 1.0f};
    std::size_t movies = 0;
    std::size_t patched = 0;
    for (const LayoutMovie& entry : layout_movies()) {
        std::filesystem::path path = folder / "win" / entry.file;
        if (!std::filesystem::exists(path)) path = folder / entry.file;
        if (!std::filesystem::exists(path)) continue;
        const auto input = read_file(path);
        const auto result = patch_hud_movie(&entry, input, layout);
        const bool changed = result.success() && !result.report.already_satisfied;
        const auto* served = cache.insert(fnv1a(input), input.size(), 1ull << 63, result.output, changed);
        ++movies;
        if (served && *served == result.output) ++patched;
    }
    // Filler entries so the settings movies arrive with a well-filled cache.
    for (int index = 0; index < 100; ++index) {
        cache.insert(0xF000 + index, 32, 0, std::vector<std::uint8_t>(32, 1), true);
    }
    check(movies > 32 && patched == movies,  // more than the old cap
        "all " + std::to_string(movies) + " layout movies served patched (" + std::to_string(patched) + ")");

    for (const char* file : {"02_040_optionsetting.gfx", "02_042_pc_graphicsetting.gfx"}) {
        const std::filesystem::path path = folder / "win" / file;
        if (!std::filesystem::exists(path)) {
            std::printf("skip %s (missing)\n", file);
            continue;
        }
        const auto input = read_file(path);
        const auto settings = patch_settings_movie(classify_settings_movie(file), input);
        const auto* settings_served =
            cache.insert(fnv1a(input), input.size(), 0, settings.output, settings.success());
        check(settings_served && *settings_served == settings.output,
            std::string("settings patch served with ") + std::to_string(cache.size()) + " cached entries: " + file);
        if (!settings_served) continue;
        const HudMovie movie = classify_hud_movie(file);
        const auto edges = patch_hud_movie(movie, *settings_served, layout);
        const auto* chained = cache.insert(fnv1a(*settings_served), settings_served->size(), 1ull << 63,
            edges.output, edges.success());
        check(chained && *chained == edges.output, std::string("settings + edges chain served: ") + file);
        const auto satisfied = patch_settings_movie(classify_settings_movie(file), *chained);
        check(satisfied.success() && satisfied.report.already_satisfied,
            std::string("chained movie still has the settings patch (tab, LineHelpWide): ") + file);
        const auto lookup = cache.find(fnv1a(input), input.size(), 0);
        check(lookup.found && lookup.output == settings_served, std::string("settings entry stays found: ") + file);
    }
}

} // namespace

int main() {
    test_unbounded();
#ifdef ERNATIVEUI_TEST_HUD_GFX_DIRS
    std::string list = ERNATIVEUI_TEST_HUD_GFX_DIRS;
    test_real_movies(list.substr(0, list.find('|')));
#else
    std::printf("skip real movies (ERNATIVEUI_TEST_HUD_GFX_DIRS not set)\n");
#endif
    std::printf(g_failures ? "FAILED (%d)\n" : "PASSED\n", g_failures);
    return g_failures ? 1 : 0;
}
