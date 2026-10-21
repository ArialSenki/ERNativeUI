// Fork: the AOB scanner jumps between candidates with memchr. This compares
// it with a plain every-offset scan on this test's own image and, when
// NT_GAME_EXE names an eldenring.exe, on the game image (mapped, not run),
// and prints how long both take there.

#include "module.hpp"

#include <Windows.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
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

} // namespace

int main() {
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
