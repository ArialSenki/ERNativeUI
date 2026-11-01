#pragma once

#include "module.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace erui::native {

// r148: where a fallback RVA comes from. A per-build profile value was
// verified for the running exact build, so a unique scan hit elsewhere is
// not the same function (another module may have hooked the real prologue)
// and must not be accepted. A constant RVA may be right for one build only,
// so a unique hit elsewhere keeps winning there.
enum class FallbackRvaSource : std::uint8_t {
    constant,
    exact_build_profile,
};

enum class DirectLookupSource : std::uint8_t {
    known_rva,         // the fallback RVA matched the whole pattern
    unique_scan,       // exactly one match in the executable sections
    profile_mismatch,  // r148: unique match, but not at the profile RVA
    ambiguous,         // more than one match
    not_found,
};

struct DirectLookup {
    std::uint8_t* address{};   // accepted address; null leaves it to the fallback
    DirectLookupSource source{DirectLookupSource::not_found};
    std::size_t matches{};     // scan matches (1 for known_rva, capped at 2)
    std::uint8_t* rejected{};  // the unique match refused by profile_mismatch
};

// r145/r148: when the whole scan pattern (and the fallback validation)
// already matches at the known RVA, the scan can only return that address:
// a unique match is this one, and an ambiguous scan falls back to it.
[[nodiscard]] inline std::uint8_t* known_rva_match(
    const ModuleView& game,
    std::string_view scan_pattern,
    std::uintptr_t fallback_rva,
    std::string_view fallback_validation,
    bool require_validation) noexcept {
    if (fallback_rva == 0 || scan_pattern.empty()) return nullptr;
    if (require_validation && fallback_validation.empty()) return nullptr;
    auto* const address = game.at_rva(fallback_rva);
    if (!address || !game.is_executable(address) ||
        !game.matches(address, scan_pattern) ||
        (!fallback_validation.empty() && !game.matches(address, fallback_validation))) {
        return nullptr;
    }
    return address;
}

// r148: the decision part of resolve_direct / resolve_optional_direct,
// without logging and without the fallback validation that follows a null
// address (it differs between the two callers).
[[nodiscard]] inline DirectLookup lookup_direct(
    const ModuleView& game,
    std::string_view scan_pattern,
    std::uintptr_t fallback_rva,
    std::string_view fallback_validation,
    bool require_validation,
    FallbackRvaSource rva_source) {
    if (auto* const known = known_rva_match(
            game, scan_pattern, fallback_rva, fallback_validation, require_validation)) {
        return DirectLookup{known, DirectLookupSource::known_rva, 1, nullptr};
    }
    const ScanResult scan = game.scan_executable(scan_pattern);
    if (scan.address && scan.matches == 1) {
        if (rva_source == FallbackRvaSource::exact_build_profile && fallback_rva != 0 &&
            scan.address != game.at_rva(fallback_rva)) {
            return DirectLookup{
                nullptr, DirectLookupSource::profile_mismatch, 1, scan.address};
        }
        return DirectLookup{scan.address, DirectLookupSource::unique_scan, 1, nullptr};
    }
    return DirectLookup{
        nullptr,
        scan.matches > 1 ? DirectLookupSource::ambiguous : DirectLookupSource::not_found,
        scan.matches,
        nullptr};
}

} // namespace erui::native
