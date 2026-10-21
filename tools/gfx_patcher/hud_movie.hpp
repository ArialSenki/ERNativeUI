#pragma once

// Fork: in-game HUD layout. Elden Ring's HUD movies are authored on a fixed
// 1920x1080 stage with absolute positions and no ActionScript layout code;
// the game fits that stage into the screen with Scaleform's ShowAll mode, so
// on wider screens everything stays in the centred 16:9 frame. Moving the
// edge-anchored placements outward (they still render outside the stage) and
// scaling them around their screen anchor gives "HUD to the screen edges" and
// "HUD scale" without touching the menus or the executable.

#include "gfx_patch.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace erui::gfx {

// Movies with a layout table. HUD movies take the HUD switch and scale;
// menu movies only move to the screen edges (never scaled).
enum class LayoutGroup : std::uint8_t { hud, menu };

struct LayoutMovie {
    const char* file;  // lower-case file name, e.g. "01_000_fe.gfx"
    LayoutGroup group;
};

// A movie of the table, or nullptr for any other file.
using HudMovie = const LayoutMovie*;

// Every movie with a layout table (stable order; used for status counters).
[[nodiscard]] std::span<const LayoutMovie> layout_movies() noexcept;

struct HudLayout {
    // Pixels (stage units) that left/right-anchored elements move outward.
    float edge_offset{0.0f};
    // Size factor applied around each element's screen anchor.
    float scale{1.0f};

    [[nodiscard]] bool identity() const noexcept {
        return edge_offset == 0.0f && scale == 1.0f;
    }
};

// Same rules as classify_settings_movie: file name only, any case.
[[nodiscard]] HudMovie classify_hud_movie(std::string_view url) noexcept;

[[nodiscard]] const char* hud_movie_name(HudMovie movie) noexcept;

// Stage pixels between the 16:9 frame and the screen edge for a back buffer
// of width x height under ShowAll; 0 for 16:9 or narrower screens.
[[nodiscard]] float hud_edge_offset_for(
    std::uint32_t width, std::uint32_t height) noexcept;

// One anchored placement as found in a movie (stage pixels, after any
// parent translation). Used by the tests and the patch report.
struct HudPlacement {
    std::string name{};
    float x{};
    float y{};
    float scale_x{};
    float scale_y{};
    std::uint32_t tags{};  // placement tags rewritten for this element
    char horizontal{};  // anchor: 'l'eft, 'c'entre, 'r'ight, or 's'tretched
                        // in X around the centre to cover the screen width
    char vertical{};    // anchor: 't'op, 'm'iddle, 'b'ottom
};

struct HudInspection {
    ErrorCode error{ErrorCode::none};
    std::string message{};
    std::vector<HudPlacement> found{};
    std::vector<std::string> missing{};

    [[nodiscard]] bool success() const noexcept { return error == ErrorCode::none; }
};

// Lists the anchored placements of the movie (the ones patch_hud_movie moves).
[[nodiscard]] HudInspection inspect_hud_movie(
    HudMovie movie,
    std::span<const std::uint8_t> input) noexcept;

// Moves and scales the anchored placements. Elements another mod renamed or
// removed are skipped (listed in the message); the patch fails only when the
// file is not a readable uncompressed GFX or none of the elements exist. An
// identity layout returns report.already_satisfied with no output.
[[nodiscard]] PatchResult patch_hud_movie(
    HudMovie movie,
    std::span<const std::uint8_t> input,
    const HudLayout& layout) noexcept;

} // namespace erui::gfx
