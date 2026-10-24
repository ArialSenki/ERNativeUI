#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace erui::native {

// Private native-integration identity. This is deliberately separate from
// both the ERNativeUI release version and the public ERUI API version.
enum class EldenRingBuild : std::uint8_t {
    unsupported,
    v2_7_0_0,
    v2_7_1_0,
    v2_7_1_1,
    v2_7_2_0,
};

struct EldenRingBuildIdentity {
    EldenRingBuild build{};
    std::string_view product_version{};
    std::uint32_t pe_timestamp{};
    std::size_t image_size{};
    // Fork: FNV-1a over the section table, image size and entry point (see
    // LayoutFingerprint). Lets a rebuild with another timestamp but the same
    // layout (the Japanese executables) use this build's profiles.
    std::uint64_t layout_fingerprint{};
};

inline constexpr std::array<EldenRingBuildIdentity, 4>
    kSupportedEldenRingBuilds{{
        {
            EldenRingBuild::v2_7_0_0,
            "2.7.0.0",
            0x69E9C9B9,
            0x5E09600,
            0x1E99AF9FC51CC948,
        },
        {
            EldenRingBuild::v2_7_1_0,
            "2.7.1.0",
            0x6A96B418,
            0x5E0DA00,
            0xB6F3EBAD13CB47C7,
        },
        // Rebuilt 28 minutes after 2.7.1.0 with the same section layout; only
        // the protection layer and inter-function padding differ.
        {
            EldenRingBuild::v2_7_1_1,
            "2.7.1.1",
            0x6A96BA95,
            0x5E0DA00,
            0xB6F3EBAD13CB47C7,
        },
        {
            EldenRingBuild::v2_7_2_0,
            "2.7.2.0",
            0x6AABF7E7,
            0x5DFD600,
            0x662F34083CB8FE85,
        },
    }};

// Fork: section count, then per section its 8 name bytes, VirtualAddress,
// VirtualSize, SizeOfRawData and Characteristics, then SizeOfImage and
// AddressOfEntryPoint, all as little-endian u32 (the name as raw bytes).
class LayoutFingerprint {
public:
    constexpr void add_byte(std::uint8_t value) noexcept {
        hash_ ^= value;
        hash_ *= 0x100000001B3ULL;
    }
    constexpr void add_u32(std::uint32_t value) noexcept {
        for (int shift = 0; shift < 32; shift += 8) {
            add_byte(static_cast<std::uint8_t>(value >> shift));
        }
    }
    [[nodiscard]] constexpr std::uint64_t value() const noexcept {
        return hash_;
    }

private:
    std::uint64_t hash_ = 0xCBF29CE484222325ULL;
};

[[nodiscard]] constexpr const EldenRingBuildIdentity*
find_elden_ring_build(
    std::uint32_t pe_timestamp,
    std::size_t image_size) noexcept {
    for (const EldenRingBuildIdentity& identity :
         kSupportedEldenRingBuilds) {
        if (identity.pe_timestamp == pe_timestamp &&
            identity.image_size == image_size) {
            return &identity;
        }
    }
    return nullptr;
}

// Fork: the known build an unknown image has the same layout as, or nullptr.
// Exact identities never take this path.
[[nodiscard]] constexpr const EldenRingBuildIdentity*
find_layout_equivalent_build(
    std::uint32_t pe_timestamp,
    std::size_t image_size,
    std::uint64_t layout_fingerprint) noexcept {
    if (find_elden_ring_build(pe_timestamp, image_size)) return nullptr;
    for (const EldenRingBuildIdentity& identity :
         kSupportedEldenRingBuilds) {
        if (identity.image_size == image_size &&
            identity.layout_fingerprint == layout_fingerprint) {
            return &identity;
        }
    }
    return nullptr;
}

// (timestamp << 8) | build of the one adopted layout-equivalent image; 0 when
// none. Its image size is the adopted identity's. (Not in a nested `detail`:
// that would hide erui::detail inside erui::native.)
inline std::atomic<std::uint64_t> g_layout_equivalent_image{0};

// Fork: records the image so every later identify_elden_ring_build call maps
// it to the equivalent build. Each profile still proves its own addresses
// (prologues, vtables, AOB) before use, as for an exact build.
inline const EldenRingBuildIdentity* adopt_layout_equivalent_build(
    std::uint32_t pe_timestamp,
    std::size_t image_size,
    std::uint64_t layout_fingerprint) noexcept {
    const EldenRingBuildIdentity* const identity =
        find_layout_equivalent_build(
            pe_timestamp, image_size, layout_fingerprint);
    if (identity) {
        g_layout_equivalent_image.store(
            (static_cast<std::uint64_t>(pe_timestamp) << 8) |
                static_cast<std::uint8_t>(identity->build),
            std::memory_order_release);
    }
    return identity;
}

[[nodiscard]] inline const EldenRingBuildIdentity*
adopted_layout_equivalent_build(
    std::uint32_t pe_timestamp,
    std::size_t image_size) noexcept {
    const std::uint64_t adopted = g_layout_equivalent_image.load(
        std::memory_order_acquire);
    if (!adopted ||
        static_cast<std::uint32_t>(adopted >> 8) != pe_timestamp) {
        return nullptr;
    }
    const auto build = static_cast<EldenRingBuild>(adopted & 0xFF);
    for (const EldenRingBuildIdentity& identity :
         kSupportedEldenRingBuilds) {
        if (identity.build == build && identity.image_size == image_size) {
            return &identity;
        }
    }
    return nullptr;
}

// Constant-evaluable for exact identities; the adopted-image lookup only runs
// at run time, on the path an exact identity never takes.
[[nodiscard]] constexpr EldenRingBuild identify_elden_ring_build(
    std::uint32_t pe_timestamp,
    std::size_t image_size) noexcept {
    if (const EldenRingBuildIdentity* const identity =
            find_elden_ring_build(pe_timestamp, image_size)) {
        return identity->build;
    }
    const EldenRingBuildIdentity* const equivalent =
        adopted_layout_equivalent_build(pe_timestamp, image_size);
    return equivalent ? equivalent->build : EldenRingBuild::unsupported;
}

[[nodiscard]] constexpr std::string_view elden_ring_build_name(
    EldenRingBuild build) noexcept {
    for (const EldenRingBuildIdentity& identity :
         kSupportedEldenRingBuilds) {
        if (identity.build == build) return identity.product_version;
    }
    return "unsupported";
}

} // namespace erui::native
