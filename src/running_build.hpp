#pragma once

#include "game_build_profiles.hpp"

#include <Windows.h>

#include <cstdint>

namespace erui::native {

// Build of the game image this process runs (eldenring.exe), identified once
// from its PE timestamp and image size.
[[nodiscard]] inline EldenRingBuild running_elden_ring_build() noexcept {
    static const EldenRingBuild build = [] {
        const auto* const base =
            reinterpret_cast<const std::uint8_t*>(GetModuleHandleW(nullptr));
        if (!base) return EldenRingBuild::unsupported;
        const auto* const dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* const nt =
            reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        return identify_elden_ring_build(
            nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage);
    }();
    return build;
}

// Menu-object profile of the running build, or nullptr on an unsupported
// build (every check that needs it then fails closed).
[[nodiscard]] inline const MenuObjectAddressProfile*
running_menu_object_profile() noexcept {
    return find_menu_object_address_profile(running_elden_ring_build());
}

} // namespace erui::native
