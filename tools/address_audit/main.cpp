// Fork: ERNativeUIAudit maps an eldenring.exe as an image (no code runs) and
// resolves every native address ERNativeUI needs, reporting which were found
// by pattern scan and which fell back to build-specific RVAs. Run it after a
// game update, before launching the game:
//
//   ERNativeUIAudit.exe "C:\...\ELDEN RING\Game\eldenring.exe"

#include "addresses.hpp"
#include "module.hpp"
#include "runtime_gfx_locate.hpp"
#include "runtime_log.hpp"

#include <Windows.h>

#include <cstdio>
#include <cstring>

namespace {

int g_errors = 0;
int g_fallbacks = 0;

void sink(erui::LogLevel level, const char* message) noexcept {
    if (!message) return;
    if (level == erui::LogLevel::error) ++g_errors;
    if (std::strstr(message, "fallback")) ++g_fallbacks;
    const char* tag = level == erui::LogLevel::error ? "ERROR"
        : level == erui::LogLevel::warning ? "WARN " : "     ";
    std::printf("%s %s\n", tag, message);
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    const wchar_t* path = argc > 1 ? argv[1]
        : L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\ELDEN RING\\Game\\eldenring.exe";
    HMODULE image = LoadLibraryExW(path, nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!image) {
        std::wprintf(L"Could not map %ls (error %lu).\n", path, GetLastError());
        return 2;
    }
    std::wprintf(L"Auditing %ls\n", path);
    erui::detail::set_log_sink(&sink);
    erui::detail::set_log_filter(true, false);

    erui::native::ModuleView game{};
    if (!game.initialize(image)) {
        std::printf("ERROR could not read the PE image\n");
        return 2;
    }
    erui::native::GameAddresses addresses{};
    const bool complete = erui::native::resolve_game_addresses(
        game, addresses, true, true, true, true, true, true, true);

    erui::host::RuntimeGfxTargets gfx{};
    const bool gfx_ok = erui::host::locate_runtime_gfx_targets(game, gfx);
    if (gfx_ok) {
        std::printf("      Runtime GFX: OpenFile RVA 0x%llX, MemoryFile vtable RVA 0x%llX\n",
            static_cast<unsigned long long>(static_cast<std::uint8_t*>(gfx.open_file) - game.base()),
            static_cast<unsigned long long>(
                reinterpret_cast<std::uint8_t*>(gfx.memory_file_vtable) - game.base()));
    } else {
        std::printf("ERROR Runtime GFX: %s\n", gfx.failure ? gfx.failure : "lookup failed");
    }

    std::printf("\nSummary: required addresses %s, %d error(s), %d fallback RVA(s), runtime GFX %s\n",
        complete ? "complete" : "INCOMPLETE", g_errors, g_fallbacks, gfx_ok ? "ok" : "unavailable");
    FreeLibrary(image);
    return complete && gfx_ok && g_errors == 0 ? 0 : 1;
}
