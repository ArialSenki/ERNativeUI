#include "gfx_check.hpp"

#include "logger.hpp"

#include <Windows.h>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string_view>
#include <system_error>
#include <vector>

namespace erui::host {
namespace {

// Markers written by the fork's GFX patcher. LineHelpWide carries long
// provider help; Item_0_8 is the ninth (Mod Settings) top-level tab slot.
constexpr std::string_view kWideHelpMarker = "LineHelpWide";
constexpr std::string_view kModSettingsTabMarker = "Item_0_8";

std::filesystem::path game_directory() {
    wchar_t buffer[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, buffer, _countof(buffer));
    if (!length || length >= _countof(buffer)) return {};
    return std::filesystem::path(buffer).parent_path();
}

bool is_inside(const std::filesystem::path& child, const std::filesystem::path& parent) {
    std::error_code error{};
    const auto relative = std::filesystem::relative(child, parent, error);
    if (error || relative.empty()) return false;
    const std::wstring text = relative.native();
    return text.rfind(L"..", 0) != 0;
}

enum class MovieState { missing, unreadable, compressed, read };

MovieState read_movie(const std::filesystem::path& path, std::vector<char>& bytes) {
    std::error_code error{};
    if (!std::filesystem::exists(path, error)) return MovieState::missing;
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return MovieState::unreadable;
    bytes.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    if (bytes.size() < 8) return MovieState::unreadable;
    // GFX (uncompressed) or FWS; CFX/CWS are zlib-compressed and cannot be
    // searched without inflating them.
    if (bytes[0] == 'C') return MovieState::compressed;
    return MovieState::read;
}

bool contains(const std::vector<char>& bytes, std::string_view marker) {
    return std::search(bytes.begin(), bytes.end(), marker.begin(), marker.end()) !=
        bytes.end();
}

} // namespace

std::wstring check_settings_gfx(
    const std::filesystem::path& host_directory,
    bool spanish) noexcept {
    try {
        const std::filesystem::path game = game_directory();
        if (game.empty() || !is_inside(host_directory, game)) {
            log::write("GFX check skipped: host DLL is not inside the game folder");
            return {};
        }

        struct Movie {
            const wchar_t* file;
            bool needs_tab;
        };
        constexpr Movie movies[] = {
            {L"02_040_optionsetting.gfx", true},
            {L"02_042_pc_graphicsetting.gfx", false},
        };

        std::wstring problems{};
        for (const Movie& movie : movies) {
            const std::filesystem::path path = game / L"menu" / L"win" / movie.file;
            std::vector<char> bytes{};
            const MovieState state = read_movie(path, bytes);
            const std::string name = path.filename().string();
            if (state == MovieState::compressed || state == MovieState::unreadable) {
                log::write("GFX check: %s could not be inspected (%s)", name.c_str(),
                    state == MovieState::compressed ? "compressed" : "unreadable");
                continue;
            }
            const bool help = state == MovieState::read && contains(bytes, kWideHelpMarker);
            const bool tab = !movie.needs_tab ||
                (state == MovieState::read && contains(bytes, kModSettingsTabMarker));
            log::write("GFX check: %s present=%u wideHelp=%u modSettingsTab=%u",
                name.c_str(), state == MovieState::read ? 1u : 0u,
                help ? 1u : 0u, movie.needs_tab ? (tab ? 1u : 0u) : 2u);
            if (help && tab) continue;
            problems += L"\n- ";
            problems += movie.file;
            if (state == MovieState::missing) {
                problems += spanish ? L" (no está instalado)" : L" (not installed)";
            } else if (!tab) {
                problems += spanish ? L" (sin pestaña Mod Settings)" : L" (no Mod Settings tab)";
            } else {
                problems += spanish ? L" (sin ayuda ancha)" : L" (no wide help line)";
            }
        }
        if (problems.empty()) return {};
        log::write("WARN: settings GFX files are missing or replaced by another mod");
        return (spanish
            ? L"NativeTweaks: otro mod ha reemplazado o falta un archivo del menú:"
            : L"NativeTweaks: a menu file is missing or replaced by another mod:") +
            problems +
            (spanish
                ? L"\nDale prioridad a NativeTweaks en tu gestor de mods."
                : L"\nGive NativeTweaks priority in your mod manager.");
    } catch (...) {
        log::write("GFX check failed with an exception");
        return {};
    }
}

} // namespace erui::host
