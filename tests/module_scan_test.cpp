// Fork: the AOB scanner jumps between candidates with memchr. This compares
// it with a plain every-offset scan on this test's own image and, when
// NT_GAME_EXE names an eldenring.exe, on the game image (mapped, not run),
// and prints how long both take there.

#include "known_rva.hpp"
#include "module.hpp"

#include <Windows.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace {

int g_failures = 0;

void check(bool condition, const std::string& what) {
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", what.c_str());
    if (!condition) ++g_failures;
}

struct Byte {
    std::uint8_t value{};
    bool wildcard{};
};

std::string pattern_text(const std::vector<Byte>& pattern) {
    std::string text;
    char buffer[4]{};
    for (const Byte& byte : pattern) {
        if (!text.empty()) text += ' ';
        if (byte.wildcard) {
            text += "??";
        } else {
            std::snprintf(buffer, sizeof(buffer), "%02X", byte.value);
            text += buffer;
        }
    }
    return text;
}

std::vector<std::uint8_t*> reference_scan(
    const erui::native::ModuleView& module, const std::vector<Byte>& pattern) {
    std::vector<std::uint8_t*> matches;
    for (const auto& section : module.sections()) {
        if (!section.executable || section.size < pattern.size()) continue;
        for (std::size_t offset = 0; offset + pattern.size() <= section.size; ++offset) {
            bool equal = true;
            for (std::size_t i = 0; i < pattern.size() && equal; ++i) {
                equal = pattern[i].wildcard || section.begin[offset + i] == pattern[i].value;
            }
            if (equal) matches.push_back(section.begin + offset);
        }
    }
    return matches;
}

// Patterns cut from the module's own code: some unique, some with leading
// or inner wildcards, one made only of wildcards.
std::vector<std::vector<Byte>> sample_patterns(const erui::native::ModuleView& module) {
    std::vector<std::vector<Byte>> patterns;
    for (const auto& section : module.sections()) {
        if (!section.executable || section.size < 0x10000) continue;
        for (const std::size_t at : {std::size_t{0x400}, section.size / 3, section.size / 2,
                 section.size - 0x200}) {
            std::vector<Byte> pattern;
            for (std::size_t i = 0; i < 24; ++i) {
                pattern.push_back({section.begin[at + i], false});
            }
            patterns.push_back(pattern);
            std::vector<Byte> leading = pattern;
            for (std::size_t i = 0; i < 3; ++i) leading[i].wildcard = true;
            leading[10].wildcard = leading[11].wildcard = true;
            patterns.push_back(leading);
            patterns.push_back(std::vector<Byte>(pattern.begin(), pattern.begin() + 3));
        }
        break;
    }
    patterns.push_back(std::vector<Byte>(4, Byte{0, true}));
    return patterns;
}

void compare(const erui::native::ModuleView& module, const char* label, bool timing) {
    double fast_ms = 0.0;
    double reference_ms = 0.0;
    for (const std::vector<Byte>& pattern : sample_patterns(module)) {
        const std::string text = pattern_text(pattern);
        const auto t0 = std::chrono::steady_clock::now();
        const std::vector<std::uint8_t*> all = module.scan_executable_all(text);
        const erui::native::ScanResult first = module.scan_executable(text);
        const auto t1 = std::chrono::steady_clock::now();
        const std::vector<std::uint8_t*> expected = reference_scan(module, pattern);
        const auto t2 = std::chrono::steady_clock::now();
        // Time only signature-sized patterns, like the ones the host uses.
        if (pattern.size() >= 16) {
            fast_ms += std::chrono::duration<double, std::milli>(t1 - t0).count();
            reference_ms += std::chrono::duration<double, std::milli>(t2 - t1).count();
        }
        const bool same_all = all == expected;
        const bool same_first =
            first.address == (expected.empty() ? nullptr : expected.front()) &&
            first.matches == (expected.size() < 2 ? expected.size() : 2);
        check(same_all && same_first,
            std::string(label) + ": " + text.substr(0, 26) + "... (" +
                std::to_string(expected.size()) + " matches)");
    }
    if (timing) {
        std::printf("     %s: memchr scanner %.1f ms, every-offset reference %.1f ms\n",
            label, fast_ms, reference_ms);
    }
}

// r148: a minimal mapped PE image in memory: headers and one executable
// section at RVA 0x1000 filled with int3, so known_rva.hpp can be tested
// against exact addresses without the game.
constexpr std::size_t kImageSize = 0x3000;
constexpr std::size_t kTextRva = 0x1000;
constexpr std::size_t kTextSize = 0x2000;

std::vector<std::uint8_t> synthetic_image() {
    std::vector<std::uint8_t> image(kImageSize, 0);
    auto* const dos = reinterpret_cast<IMAGE_DOS_HEADER*>(image.data());
    dos->e_magic = IMAGE_DOS_SIGNATURE;
    dos->e_lfanew = 0x80;
    auto* const nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(image.data() + 0x80);
    nt->Signature = IMAGE_NT_SIGNATURE;
    nt->FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
    nt->FileHeader.NumberOfSections = 1;
    nt->FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
    nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    nt->OptionalHeader.SizeOfImage = static_cast<DWORD>(kImageSize);
    IMAGE_SECTION_HEADER* const text = IMAGE_FIRST_SECTION(nt);
    std::memcpy(text->Name, ".text", 5);
    text->VirtualAddress = static_cast<DWORD>(kTextRva);
    text->Misc.VirtualSize = static_cast<DWORD>(kTextSize);
    text->Characteristics = IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ;
    std::memset(image.data() + kTextRva, 0xCC, kTextSize);
    return image;
}

void put(std::vector<std::uint8_t>& image, std::size_t rva,
    std::initializer_list<std::uint8_t> bytes) {
    std::memcpy(image.data() + rva, bytes.begin(), bytes.size());
}

// The resolver before r145: scan first, the validated fallback RVA only when
// the scan is not unique.
std::uint8_t* scan_first_reference(const erui::native::ModuleView& game,
    std::string_view pattern, std::uintptr_t fallback_rva, std::string_view validation) {
    const erui::native::ScanResult scan = game.scan_executable(pattern);
    if (scan.address && scan.matches == 1) return scan.address;
    std::uint8_t* const fallback = game.at_rva(fallback_rva);
    if (fallback && game.is_executable(fallback) &&
        (validation.empty() || game.matches(fallback, validation))) {
        return fallback;
    }
    return nullptr;
}

void test_known_rva_lookup() {
    using erui::native::DirectLookupSource;
    using erui::native::FallbackRvaSource;
    using erui::native::lookup_direct;
    constexpr std::string_view kUnique = "40 53 48 83 EC 20 48 8B D9 B9 37 13 00 00";
    constexpr std::string_view kTwin = "4C 8B DC 55 56 57 41 54 B8 42 ?? 00 00";
    std::vector<std::uint8_t> image = synthetic_image();
    put(image, 0x1100, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0xB9, 0x37, 0x13, 0x00, 0x00});
    put(image, 0x1200, {0x4C, 0x8B, 0xDC, 0x55, 0x56, 0x57, 0x41, 0x54, 0xB8, 0x42, 0x01, 0x00, 0x00});
    put(image, 0x1800, {0x4C, 0x8B, 0xDC, 0x55, 0x56, 0x57, 0x41, 0x54, 0xB8, 0x42, 0x02, 0x00, 0x00});
    // Another module's hook jump over the prologue at a profile RVA.
    put(image, 0x1400, {0xE9, 0x10, 0x20, 0x30, 0x40, 0x20, 0x48, 0x8B, 0xD9, 0xB9, 0x37, 0x13, 0x00, 0x00});
    // Pattern bytes in the last bytes of .text, and a prefix cut by its end.
    put(image, kTextRva + kTextSize - 6, {0x0F, 0x1F, 0x44, 0x00, 0x00, 0xC3});
    erui::native::ModuleView game{};
    check(game.initialize(reinterpret_cast<HMODULE>(image.data())), "synthetic image");
    std::uint8_t* const base = game.base();

    // Unique pattern: known RVA first gives the scan's own address.
    auto lookup = lookup_direct(game, kUnique, 0x1100, kUnique, false,
        FallbackRvaSource::exact_build_profile);
    check(lookup.source == DirectLookupSource::known_rva && lookup.address == base + 0x1100,
        "known RVA: unique pattern found at its RVA");
    check(lookup.address == scan_first_reference(game, kUnique, 0x1100, kUnique),
        "known RVA: same address as scanning first (unique)");
    lookup = lookup_direct(game, kUnique, 0, {}, false, FallbackRvaSource::constant);
    check(lookup.source == DirectLookupSource::unique_scan && lookup.address == base + 0x1100,
        "known RVA: no RVA scans and finds the unique match");

    // Ambiguous pattern: the scan falls back to the RVA; checking it first
    // returns that same RVA without scanning.
    for (const std::uintptr_t rva : {std::uintptr_t{0x1200}, std::uintptr_t{0x1800}}) {
        lookup = lookup_direct(game, kTwin, rva, kTwin, true, FallbackRvaSource::constant);
        check(lookup.source == DirectLookupSource::known_rva && lookup.address == base + rva &&
                lookup.address == scan_first_reference(game, kTwin, rva, kTwin),
            "known RVA: same address as scanning first (ambiguous, RVA " +
                std::to_string(rva) + ")");
    }
    lookup = lookup_direct(game, kTwin, 0, kTwin, false, FallbackRvaSource::constant);
    check(lookup.source == DirectLookupSource::ambiguous && !lookup.address &&
            lookup.matches == 2,
        "known RVA: ambiguous scan without an RVA accepts nothing");

    // A unique hit away from the RVA: kept for a constant RVA, refused when
    // the RVA comes from the running build's exact profile.
    lookup = lookup_direct(game, kUnique, 0x1400, kUnique, false, FallbackRvaSource::constant);
    check(lookup.source == DirectLookupSource::unique_scan && lookup.address == base + 0x1100,
        "known RVA: constant RVA mismatch keeps the unique scan hit");
    lookup = lookup_direct(game, kUnique, 0x1400, kUnique, false,
        FallbackRvaSource::exact_build_profile);
    check(lookup.source == DirectLookupSource::profile_mismatch && !lookup.address &&
            lookup.rejected == base + 0x1100 && lookup.matches == 1,
        "known RVA: profile RVA mismatch refuses the unique scan hit");
    // The unique hit at the profile RVA itself is accepted even when the
    // known-RVA check is skipped (no validation pattern).
    lookup = lookup_direct(game, kUnique, 0x1100, {}, true, FallbackRvaSource::exact_build_profile);
    check(lookup.source == DirectLookupSource::unique_scan && lookup.address == base + 0x1100,
        "known RVA: unique hit at the profile RVA is accepted");
    lookup = lookup_direct(game, "11 22 33 44 55", 0x1100, {}, false,
        FallbackRvaSource::exact_build_profile);
    check(lookup.source == DirectLookupSource::not_found && !lookup.address,
        "known RVA: missing pattern accepts nothing");

    // Bounds at the end of .text (and of this image): a match that ends on
    // the last byte is found; one that would run past it is not.
    const erui::native::ScanResult tail = game.scan_executable("0F 1F 44 00 00 C3");
    check(tail.matches == 1 && tail.address == base + kTextRva + kTextSize - 6,
        "end of .text: pattern ending on the last byte is found");
    check(game.scan_executable("44 00 00 C3 CC CC").matches == 0 &&
            !game.matches(base + kImageSize - 4, "44 00 00 C3 CC CC"),
        "end of .text: pattern running past the end is not matched");
}

} // namespace

int main() {
    test_known_rva_lookup();

    erui::native::ModuleView self{};
    check(self.initialize(GetModuleHandleW(nullptr)), "own image");
    compare(self, "own image", false);

    const char* exe = std::getenv("NT_GAME_EXE");
    if (exe && *exe) {
        const std::wstring path(exe, exe + std::strlen(exe));
        HMODULE game = LoadLibraryExW(path.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
        if (!game) {
            std::printf("skip game image (%s could not be mapped)\n", exe);
        } else {
            erui::native::ModuleView view{};
            check(view.initialize(game), "game image");
            compare(view, "game image", true);
            FreeLibrary(game);
        }
    } else {
        std::printf("skip game image (NT_GAME_EXE not set)\n");
    }
    std::printf("%d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
