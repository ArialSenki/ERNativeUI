#pragma once

#include "game_build_profiles.hpp"

#include <Windows.h>

#include <cstdint>

namespace erui::native {

// Layout fingerprint of a mapped PE image (see LayoutFingerprint), or 0 when
// the headers are not a 64-bit PE. Bounded header reads only, so it is safe
// under the loader lock.
[[nodiscard]] inline std::uint64_t image_layout_fingerprint(
    const void* image_base) noexcept {
    const auto* const base = static_cast<const std::uint8_t*>(image_base);
    if (!base) return 0;
    const auto* const dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 ||
        dos->e_lfanew > 0x100000) {
        return 0;
    }
    const auto* const nt =
        reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        return 0;
    }
    const std::uint16_t count = nt->FileHeader.NumberOfSections;
    const auto* const sections = IMAGE_FIRST_SECTION(nt);
    LayoutFingerprint fingerprint{};
    fingerprint.add_u32(count);
    for (std::uint16_t index = 0; index < count; ++index) {
        const IMAGE_SECTION_HEADER& section = sections[index];
        for (const BYTE byte : section.Name) fingerprint.add_byte(byte);
        fingerprint.add_u32(section.VirtualAddress);
        fingerprint.add_u32(section.Misc.VirtualSize);
        fingerprint.add_u32(section.SizeOfRawData);
        fingerprint.add_u32(section.Characteristics);
    }
    fingerprint.add_u32(nt->OptionalHeader.SizeOfImage);
    fingerprint.add_u32(nt->OptionalHeader.AddressOfEntryPoint);
    return fingerprint.value();
}

// Fork: adopts a mapped image whose timestamp is unknown but whose layout is
// a known build's (for example a Japanese rebuild). Returns that build's
// identity, or nullptr for an exact or unknown image.
inline const EldenRingBuildIdentity* adopt_layout_equivalent_image(
    const void* image_base) noexcept {
    const auto* const base = static_cast<const std::uint8_t*>(image_base);
    const std::uint64_t fingerprint = image_layout_fingerprint(base);
    if (!fingerprint) return nullptr;
    const auto* const dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* const nt =
        reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    return adopt_layout_equivalent_build(
        nt->FileHeader.TimeDateStamp,
        nt->OptionalHeader.SizeOfImage,
        fingerprint);
}

// Build of the game image this process runs (eldenring.exe), identified once
// from its PE timestamp and image size, or from its layout (see above).
[[nodiscard]] inline EldenRingBuild running_elden_ring_build() noexcept {
    static const EldenRingBuild build = [] {
        const auto* const base =
            reinterpret_cast<const std::uint8_t*>(GetModuleHandleW(nullptr));
        if (!base) return EldenRingBuild::unsupported;
        (void)adopt_layout_equivalent_image(base);
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
