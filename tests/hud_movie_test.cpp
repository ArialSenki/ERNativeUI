// Fork: HUD layout patch (hud_movie.cpp). Fixtures are optional local files
// (game assets are never committed); missing fixtures are reported as skipped.
// ERNATIVEUI_TEST_HUD_GFX_DIRS is a '|'-separated list of folders, each with
// any of 01_000_fe.gfx, 01_001_fe_soul.gfx and 01_002_fe_saveicon.gfx. The
// first folder must hold the vanilla movies.

#include "hud_movie.hpp"
#include "settings_movie.hpp"

#include <algorithm>
#include <cmath>
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

bool near(float left, float right) {
    return std::fabs(left - right) < 0.06f;
}

const erui::gfx::HudPlacement* find(const erui::gfx::HudInspection& inspection, const std::string& name) {
    for (const auto& placement : inspection.found) {
        if (placement.name == name) return &placement;
    }
    return nullptr;
}

void test_classification() {
    using erui::gfx::classify_hud_movie;
    using erui::gfx::hud_movie_name;
    const auto named = [](const char* url, const char* file) {
        return std::string(hud_movie_name(classify_hud_movie(url))) == file;
    };
    check(named("data0:/menu/01_000_fe.gfx", "01_000_fe.gfx"), "classify HUD URL");
    check(named("C:\\mods\\menu\\01_001_FE_SOUL.GFX", "01_001_fe_soul.gfx"), "classify runes path ignoring case");
    check(named("data0:/menu/01_002_fe_saveicon.gfx", "01_002_fe_saveicon.gfx"), "classify save icon");
    check(classify_hud_movie("data0:/menu/01_020_sign.gfx") == nullptr, "ignore other movies");
    check(classify_hud_movie("01_000_fe.gfx.bak") == nullptr, "ignore backups");
    check(named("data0:/menu/02_000_ingametop.gfx", "02_000_ingametop.gfx"), "classify pause menu");
    check(named("data0:/menu/win/02_040_optionsetting.gfx", "02_040_optionsetting.gfx"), "classify options (win)");
    check(classify_hud_movie("data0:/menu/01_000_fe.gfx")->group == erui::gfx::LayoutGroup::hud &&
            classify_hud_movie("data0:/menu/02_010_equiptop.gfx")->group == erui::gfx::LayoutGroup::menu,
        "HUD and menu groups");
    check(erui::gfx::hud_edge_offset_for(1920, 1080) == 0.0f, "16:9 has no edge offset");
    check(erui::gfx::hud_edge_offset_for(1920, 1200) == 0.0f, "16:10 has no edge offset");
    check(near(erui::gfx::hud_edge_offset_for(2560, 1080), 320.0f), "2560x1080 edge offset 320");
    check(near(erui::gfx::hud_edge_offset_for(3440, 1440), 330.0f), "3440x1440 edge offset 330");
    check(near(erui::gfx::hud_edge_offset_for(5120, 1440), 960.0f), "32:9 edge offset 960");
}

void test_invalid() {
    using namespace erui::gfx;
    const std::vector<std::uint8_t> junk(64, 0x41);
    check(!patch_hud_movie(classify_hud_movie("01_000_fe.gfx"), junk, {100.0f, 1.0f}).success(), "reject non-GFX input");
    check(!patch_hud_movie(classify_hud_movie("01_000_fe.gfx"), junk, {-1.0f, 1.0f}).success(), "reject negative offset");
    check(!patch_hud_movie(nullptr, junk, {100.0f, 1.0f}).success(), "reject unknown movie");
}

void test_movie(const std::filesystem::path& folder, erui::gfx::HudMovie movie, bool vanilla) {
    using namespace erui::gfx;
    // The game opens the PC variant (menu/win/) when there is one.
    std::filesystem::path path = folder / "win" / hud_movie_name(movie);
    if (!std::filesystem::exists(path)) path = folder / hud_movie_name(movie);
    const std::string label = path.string();
    const std::string file = hud_movie_name(movie);
    if (!std::filesystem::exists(path)) {
        std::printf("skip %s (missing)\n", label.c_str());
        return;
    }
    const auto input = read_file(path);
    const HudInspection before = inspect_hud_movie(movie, input);
    check(before.success(), "inspect " + label + (before.message.empty() ? "" : ": " + before.message));
    if (!before.success()) return;
    if (vanilla) {
        check(before.missing.empty(), "vanilla has every anchored element: " + label);
        const std::size_t expected = file == "01_000_fe.gfx" ? 14u
            : file == "02_000_ingametop.gfx" ? 33u
            : file == "02_010_equiptop.gfx" ? 11u
            : file == "02_011_equip.gfx" ? 9u
            : file == "02_020_inventory.gfx" ? 10u
            : file == "02_070_status.gfx" ? 7u
            : file == "02_030_inventory_commandlist.gfx" ? 2u
            : file == "02_040_optionsetting.gfx" || file == "02_042_pc_graphicsetting.gfx" ? 3u
            : file == "01_001_fe_soul.gfx" || file == "01_002_fe_saveicon.gfx" ? 1u
            : file == "03_020_spell.gfx" ? 11u : file == "02_903_nowloading2.gfx" ? 4u : 0u;
        // 0: no fixed count; missing.empty() above already covers the table.
        if (expected) check(before.found.size() == expected, "vanilla anchored element count: " + label);
        const auto anchored = [&](const char* name, char horizontal, char vertical, float x) {
            const HudPlacement* placement = find(before, name);
            if (placement && !near(placement->x, x)) std::printf("     %s x=%.3f\n", name, placement->x);
            check(placement && placement->horizontal == horizontal && placement->vertical == vertical &&
                    near(placement->x, x),
                std::string("vanilla anchor of ") + name);
        };
        if (file == "01_000_fe.gfx") {
            anchored("PlayerHUD.Gauge", 'l', 't', 7.0f);
            anchored("ActionGuide", 'r', 'b', 1901.6f);
        } else if (file == "01_001_fe_soul.gfx") {
            anchored("Soul", 'r', 'b', 1762.0f);
        } else if (file == "01_002_fe_saveicon.gfx") {
            anchored("AutoSaveIcon", 'r', 't', 1800.0f);
        } else if (file == "02_010_equiptop.gfx") {
            anchored("BG.BG_L", 's', 'm', 960.0f);
            anchored("BG.BG_S", 'l', 'm', 380.0f);
            anchored("DetailStatusView.Status_Player", 'r', 'm', 1540.0f);
        } else if (file == "02_903_nowloading2.gfx") {
            anchored("BackImage", 'v', 'm', 0.0f);
            anchored("Gauge", 'r', 'b', 205.2f);
        } else if (file == "02_120_worldmap.gfx") {
            anchored("Body.Mask", 's', 'm', 0.0f);
            anchored("Body.Time", 'r', 'b', 1786.2f);
        } else if (file == "05_001_title_logo.gfx") {
            anchored("#3.logo backdrop", 's', 'm', 0.0f);
        } else if (file == "02_040_optionsetting.gfx") {
            anchored("backdrop", 's', 'm', 960.0f);
            anchored("title base", 'l', 't', 960.0f);
        } else if (file == "02_000_ingametop.gfx") {
            anchored("ItemList.backdrop left", 'l', 'm', -151.8f);
            anchored("ItemList.backdrop right", 'r', 'm', 2082.6f);
            anchored("ItemList.Item_0_0", 'l', 'm', 88.3f);
            anchored("ItemList.Item_1_1", 'r', 'm', 1864.2f);
        }
    }
    for (const auto& name : before.missing) std::printf("     missing %s\n", name.c_str());

    const auto identity = patch_hud_movie(movie, input, {});
    check(identity.success() && identity.report.already_satisfied && identity.output.empty(),
        "identity layout leaves " + label + " unchanged");

    // The game hands over the archive buffer with zero padding after the
    // declared length (01_001_fe_soul: 3540 declared, 3552 in memory).
    for (const std::size_t alignment : {std::size_t{16}, std::size_t{0x100}}) {
        std::vector<std::uint8_t> padded = input;
        padded.resize((input.size() / alignment + 1) * alignment, 0);
        const std::size_t padding = padded.size() - input.size();
        const std::string what = label + " padded by " + std::to_string(padding);
        check(inspect_hud_movie(movie, padded).found.size() == before.found.size(), "inspect " + what);
        const auto plain = patch_hud_movie(movie, input, {320.0f, 0.9f});
        const auto patched = patch_hud_movie(movie, padded, {320.0f, 0.9f});
        bool same = patched.success() && plain.success() &&
            patched.output.size() == plain.output.size() + padding &&
            std::equal(plain.output.begin(), plain.output.end(), patched.output.begin());
        if (same) {
            same = std::all_of(patched.output.end() - static_cast<std::ptrdiff_t>(padding),
                patched.output.end(), [](std::uint8_t byte) { return byte == 0; });
        }
        check(same, "patch " + what + " = unpadded patch + same padding: " + patched.message);
        check(same && inspect_hud_movie(movie, patched.output).found.size() == before.found.size(),
            "padded output parses back: " + what);
        std::vector<std::uint8_t> dirty = padded;
        dirty.back() = 1;
        check(!patch_hud_movie(movie, dirty, {320.0f, 0.9f}).success(), "reject non-zero padding: " + what);
    }

    // 21:9 at 1.0, then 32:9 at 0.8.
    const HudLayout layouts[] = {{320.0f, 1.0f}, {960.0f, 0.8f}, {0.0f, 1.25f}};
    for (const HudLayout& layout : layouts) {
        char suffix[64];
        std::snprintf(suffix, sizeof suffix, " (offset %.0f, scale %.2f)", layout.edge_offset, layout.scale);
        const auto patched = patch_hud_movie(movie, input, layout);
        check(patched.success(), "patch " + label + suffix + ": " + patched.message);
        if (!patched.success()) continue;
        const HudInspection after = inspect_hud_movie(movie, patched.output);
        check(after.success() && after.found.size() == before.found.size(),
            "patched movie parses back with the same elements" + std::string(suffix));
        for (const auto& original : before.found) {
            const HudPlacement* moved = find(after, original.name);
            if (!moved) {
                check(false, "element survives: " + original.name);
                continue;
            }
            if (original.horizontal == 'v') {
                // Picture scaled uniformly around the centre to cover the width.
                const float factor = (960.0f + layout.edge_offset) / 960.0f;
                check(near(moved->x, 960.0f + (original.x - 960.0f) * factor) &&
                        near(moved->y, 540.0f + (original.y - 540.0f) * factor) &&
                        near(moved->scale_x, original.scale_x * factor) && near(moved->scale_y, original.scale_y * factor),
                    "  " + original.name + " covers" + suffix);
                continue;
            }
            if (original.horizontal == 's') {
                // Backdrop widened around the centre to the visible width.
                const float factor = (960.0f + layout.edge_offset) / 960.0f;
                check(near(moved->x, 960.0f + (original.x - 960.0f) * factor) && near(moved->y, original.y) &&
                        near(moved->scale_x, original.scale_x * factor) && near(moved->scale_y, original.scale_y),
                    "  " + original.name + " stretched" + suffix);
                continue;
            }
            // Expected position from the element's anchor.
            const bool left = original.horizontal == 'l';
            const bool right = original.horizontal == 'r';
            const bool top = original.vertical == 't';
            const bool bottom = original.vertical == 'b';
            const float ax = left ? 0.0f : right ? 1920.0f : 960.0f;
            const float ay = top ? 0.0f : bottom ? 1080.0f : 540.0f;
            const float shift = left ? -layout.edge_offset : right ? layout.edge_offset : 0.0f;
            const float x = ax + shift + (original.x - ax) * layout.scale;
            const float y = ay + (original.y - ay) * layout.scale;
            check(near(moved->x, x) && near(moved->y, y) &&
                    near(moved->scale_x, original.scale_x * layout.scale) &&
                    near(moved->scale_y, original.scale_y * layout.scale),
                "  " + original.name + suffix);
        }
    }
}

// The runtime applies settings_movie to 02_040/02_042 first and the edge
// layout to its output. The chain must succeed, be deterministic and leave
// the settings patch satisfied.
void test_settings_chain(const std::filesystem::path& folder, const char* file) {
    using namespace erui::gfx;
    const std::filesystem::path path = folder / "win" / file;
    if (!std::filesystem::exists(path)) {
        std::printf("skip settings chain %s (missing)\n", file);
        return;
    }
    const auto input = read_file(path);
    const SettingsMovie kind = classify_settings_movie(file);
    const HudMovie movie = classify_hud_movie(file);
    const HudLayout layout{320.0f, 1.0f};
    const auto settings = patch_settings_movie(kind, input);
    check(settings.success() && !settings.output.empty(), std::string("settings patch of ") + file);
    if (!settings.success()) return;
    const auto chained = patch_hud_movie(movie, settings.output, layout);
    check(chained.success(), std::string("edge layout after settings patch: ") + file + ": " + chained.message);
    if (!chained.success()) return;
    const auto again = patch_hud_movie(movie, patch_settings_movie(kind, input).output, layout);
    check(again.success() && again.output == chained.output, std::string("chain is deterministic: ") + file);
    const auto settings_again = patch_settings_movie(kind, chained.output);
    check(settings_again.success() && settings_again.report.already_satisfied,
        std::string("settings patch still satisfied after the edge layout: ") + file + ": " + settings_again.message);
    const HudInspection inspection = inspect_hud_movie(movie, chained.output);
    check(inspection.success() && inspection.found.size() == 3u && inspection.missing.empty(),
        std::string("chained output keeps every anchored element: ") + file);
}

std::vector<std::filesystem::path> fixture_folders() {
    std::vector<std::filesystem::path> folders{};
#ifdef ERNATIVEUI_TEST_HUD_GFX_DIRS
    std::string list = ERNATIVEUI_TEST_HUD_GFX_DIRS;
    std::size_t begin = 0;
    while (begin <= list.size()) {
        const std::size_t end = list.find('|', begin);
        const std::string item = list.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
        if (!item.empty()) folders.emplace_back(item);
        if (end == std::string::npos) break;
        begin = end + 1;
    }
#endif
    return folders;
}

} // namespace

int main() {
    test_classification();
    test_invalid();
    const auto folders = fixture_folders();
    if (folders.empty()) std::printf("skip HUD movie fixtures (ERNATIVEUI_TEST_HUD_GFX_DIRS not set)\n");
    for (std::size_t index = 0; index < folders.size(); ++index) {
        for (const erui::gfx::LayoutMovie& movie : erui::gfx::layout_movies()) {
            test_movie(folders[index], &movie, index == 0);
        }
        if (index == 0) {
            test_settings_chain(folders[index], "02_040_optionsetting.gfx");
            test_settings_chain(folders[index], "02_042_pc_graphicsetting.gfx");
        }
    }
    std::printf(g_failures ? "FAILED (%d)\n" : "PASSED\n", g_failures);
    return g_failures ? 1 : 0;
}
