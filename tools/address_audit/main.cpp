// Fork: ERNativeUIAudit maps an eldenring.exe as an image (no code runs) and
// resolves every native address ERNativeUI needs, reporting which were found
// by pattern scan and which fell back to build-specific RVAs. Run it after a
// game update, before launching the game:
//
//   ERNativeUIAudit.exe "C:\...\ELDEN RING\Game\eldenring.exe"

#include "addresses.hpp"
#include "module.hpp"
#include "native_input_bindings.hpp"
#include "runtime_gfx_locate.hpp"
#include "runtime_log.hpp"
#include "running_build.hpp"

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
    // Trace and verbose lines stay off (diagnostic logging is off by default).
    erui::detail::set_log_sink(&sink);

    erui::native::ModuleView game{};
    if (!game.initialize(image)) {
        std::printf("ERROR could not read the PE image\n");
        return 2;
    }
    const std::uint64_t layout = erui::native::image_layout_fingerprint(image);
    if (const erui::native::EldenRingBuildIdentity* const exact =
            erui::native::find_elden_ring_build(game.timestamp(), game.image_size())) {
        std::printf("      Build: Elden Ring %.*s (exact identity)\n",
            static_cast<int>(exact->product_version.size()), exact->product_version.data());
    } else if (const erui::native::EldenRingBuildIdentity* const equivalent =
                   erui::native::adopt_layout_equivalent_image(image)) {
        std::printf("WARN  Build: unlisted (PE timestamp 0x%08X), same layout as Elden Ring %.*s; auditing with its profiles\n",
            static_cast<unsigned>(game.timestamp()),
            static_cast<int>(equivalent->product_version.size()), equivalent->product_version.data());
    } else {
        std::printf("WARN  Build: unknown (PE timestamp 0x%08X, image size 0x%zX, layout 0x%016llX)\n",
            static_cast<unsigned>(game.timestamp()), game.image_size(),
            static_cast<unsigned long long>(layout));
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

    // Input bindings (hotkeys) resolve on their own path; a broken one used
    // to abort the whole install only for players with bindings (r132).
    const bool input_ok = erui::native::audit_native_input_bindings(image);
    if (input_ok) {
        std::printf("      Input bindings: every interface validated\n");
    } else {
        std::printf("ERROR Input bindings: interfaces did not validate (see the lines above)\n");
    }

    std::printf("\nSummary: required addresses %s, %d error(s), %d fallback RVA(s), runtime GFX %s, input bindings %s\n",
        complete ? "complete" : "INCOMPLETE", g_errors, g_fallbacks, gfx_ok ? "ok" : "unavailable",
        input_ok ? "ok" : "FAILED");
    FreeLibrary(image);
    return complete && gfx_ok && input_ok && g_errors == 0 ? 0 : 1;
}
