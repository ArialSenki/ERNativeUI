#include "module.hpp"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <string>
#include <system_error>

namespace erui::native {
namespace {

struct PatternByte {
    std::uint8_t value{};
    bool wildcard{};
};

std::vector<PatternByte> parse_pattern(std::string_view pattern) {
    std::vector<PatternByte> bytes;
    bytes.reserve(pattern.size() / 2);

    std::size_t cursor = 0;
    while (cursor < pattern.size()) {
        while (cursor < pattern.size() && pattern[cursor] == ' ') {
            ++cursor;
        }
        if (cursor >= pattern.size()) {
            break;
        }

        const std::size_t token_begin = cursor;
        while (cursor < pattern.size() && pattern[cursor] != ' ') {
            ++cursor;
        }
        const auto token = pattern.substr(token_begin, cursor - token_begin);
        if (token == "?" || token == "??") {
            bytes.push_back(PatternByte{0, true});
            continue;
        }

        unsigned value = 0;
        const char* first = token.data();
        const char* last = token.data() + token.size();
        const auto parsed = std::from_chars(first, last, value, 16);
        if (parsed.ec != std::errc{} || parsed.ptr != last || value > 0xFFu) {
            return {};
        }
        bytes.push_back(PatternByte{static_cast<std::uint8_t>(value), false});
    }

    return bytes;
}

bool guarded_matches(
    const std::uint8_t* address,
    const std::vector<PatternByte>& pattern) noexcept {
#if defined(_MSC_VER)
    __try {
#endif
        for (std::size_t i = 0; i < pattern.size(); ++i) {
            if (!pattern[i].wildcard && address[i] != pattern[i].value) {
                return false;
            }
        }
        return true;
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#endif
}

// Fork: how often each byte value appears in Elden Ring's code, per million
// bytes (2.7.2.0 .text sections). The scanner anchors memchr on the pattern's
// rarest fixed byte: 0x48 is 8% of x64 code, the rarest values about 0.1%.
constexpr std::uint16_t kCodeByteFrequency[256] = {
    65535, 15203,  6819,  7592,  6078,  6748,  2312,  2673, 14315,  1919,  1998,  1942,  3043,  3880,  1506, 25167,
    10012,  3033,  1269,  1437,  2400,  2355,  1165,  1434,  4649,  1611,  1088,  1176,  1881,  1343,  1208,  2056,
     8874,  1120,  1145,  1222, 36761,  1328,   963,  1138,  7882,  2534,  1030,  1888,  1538,  1200,  1397,  1267,
     6125,  1141,  1245,  4351,  2003,  1167,   913,   926,  3799,  1620,  1009,  3384,  1697,  1337,  1205,  1472,
     7988, 10887,  2384,  3098, 12661,  7774,  1742,  1925, 65535,  7802,  1514,  2092, 14806,  4235,  1409,  1537,
     5750,  1612,  1329,  2260,  3975,  3515,  1869,  3199,  4163,  2983,  1265,  2270,  5647,  2135,  1982,  3018,
     3002,   931,   910,  1882,  9152,  1262,  3532,   959,  2793,   995,   969,  1156,  2146,  1004,  1084,  1248,
     2760,  1136,  1662,  1614,  7548,  4524,  1343,  1273,  2780,  1461,  1135,  1483,  2842,  1611,  1465,  1702,
     4088,  2491,  1433, 12385,  3622,  7313,  1309,  1885,  2786, 21877,  2933, 43195,  1558, 21770,  1281,  1222,
     6778,   928,   957,  1011,  1311,  1403,   865,   993,  1634,   940,   957,   867,  1323,  1020,   988,  1029,
     2486,  1573,  1537,  1554,  1504,  1425,  1377,  1486,  2075,  1515,  1814,  1586,  1600,  1498,  1460,  1574,
     2025,   998,   922,  1020,  1373,  1029,  2491,  1646,  2987,  1768,  2729,  1518,  1237,  1083,  1295,  1432,
     7663,  5302,  2964,  6853,  4904,  1550,  4189,  5134,  4256,  3564,  2225,  2666,  6380,  1445,  1963,  2306,
     2876,  1644,  2379,  1819,  1221,  1131,  1468,  1561,  2849,  1931,  1539,  1915,  1204,  1032,   997,  1237,
     2657,  1780,  1454,  1529,  1484,  1210,  1498,  1413, 12298,  7950,  2016,  3384,  4030,  1291,  1286,  1400,
     2366,  1322,  1830,  4654,  1131,   977,  2219,  1568,  9690,  1810,  1887,  1761,  1785,  3036,  3576, 24060,
};

// Fork: finds the next byte equal to `value`; a fault while reading the
// block is reported instead of propagated.
const std::uint8_t* guarded_find_byte(
    const std::uint8_t* begin,
    std::size_t size,
    std::uint8_t value,
    bool& faulted) noexcept {
    faulted = false;
#if defined(_MSC_VER)
    __try {
#endif
        return static_cast<const std::uint8_t*>(std::memchr(begin, value, size));
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        faulted = true;
        return nullptr;
    }
#endif
}

// Fork: memchr jumps to the next occurrence of the pattern's rarest fixed
// byte and only those offsets are compared in full. Same matches, in the same
// order, as comparing every offset, but about 30x faster over the game's
// ~60 MB of code (the per-offset loop took ~150 ms per pattern at startup).
// A block whose search faults is checked offset by offset instead.
// on_match returns false to stop; scan_section then returns false too.
template <typename OnMatch>
bool scan_section(
    std::uint8_t* begin,
    std::size_t size,
    const std::vector<PatternByte>& pattern,
    OnMatch&& on_match) {
    if (size < pattern.size()) return true;
    const std::size_t last = size - pattern.size();
    std::size_t anchor = pattern.size();
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        if (pattern[i].wildcard) continue;
        if (anchor == pattern.size() ||
            kCodeByteFrequency[pattern[i].value] <
                kCodeByteFrequency[pattern[anchor].value]) {
            anchor = i;
        }
    }
    if (anchor == pattern.size()) {
        for (std::size_t offset = 0; offset <= last; ++offset) {
            if (!on_match(begin + offset)) return false;
        }
        return true;
    }
    const std::uint8_t value = pattern[anchor].value;
    constexpr std::size_t kBlock = std::size_t{1} << 16;
    std::size_t offset = 0;
    while (offset <= last) {
        const std::size_t from = offset + anchor;
        const std::size_t end = (std::min)(last + anchor + 1, from + kBlock);
        bool faulted = false;
        const std::uint8_t* const hit =
            guarded_find_byte(begin + from, end - from, value, faulted);
        if (faulted) {
            for (std::size_t candidate = offset; candidate + anchor < end; ++candidate) {
                if (guarded_matches(begin + candidate, pattern) &&
                    !on_match(begin + candidate)) {
                    return false;
                }
            }
            offset = end - anchor;
            continue;
        }
        if (!hit) {
            offset = end - anchor;
            continue;
        }
        const std::size_t candidate = static_cast<std::size_t>(hit - begin) - anchor;
        if (guarded_matches(begin + candidate, pattern) && !on_match(begin + candidate)) {
            return false;
        }
        offset = candidate + 1;
    }
    return true;
}

} // namespace

bool ModuleView::initialize(HMODULE module) noexcept {
    sections_.clear();
    base_ = reinterpret_cast<std::uint8_t*>(module);
    image_size_ = 0;
    timestamp_ = 0;
    if (!base_) {
        return false;
    }

    // The main module handle is expected to point at a mapped PE image. Keep
    // every derived range inside SizeOfImage before exposing it to the scanner.
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base_);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 ||
        static_cast<std::uint64_t>(dos->e_lfanew) > 0x100000ull) {
        base_ = nullptr;
        return false;
    }

    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base_ + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt->OptionalHeader.SizeOfImage < 0x1000) {
        base_ = nullptr;
        return false;
    }

    image_size_ = nt->OptionalHeader.SizeOfImage;
    timestamp_ = nt->FileHeader.TimeDateStamp;

    const auto* image_end = base_ + image_size_;
    const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    const auto* section_bytes = reinterpret_cast<const std::uint8_t*>(section);
    const std::size_t section_table_size =
        static_cast<std::size_t>(nt->FileHeader.NumberOfSections) * sizeof(IMAGE_SECTION_HEADER);
    if (section_bytes < base_ || section_bytes > image_end ||
        section_table_size > static_cast<std::size_t>(image_end - section_bytes)) {
        sections_.clear();
        base_ = nullptr;
        image_size_ = 0;
        timestamp_ = 0;
        return false;
    }

    try {
        sections_.reserve(nt->FileHeader.NumberOfSections);
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
            const std::size_t virtual_address = section[i].VirtualAddress;
            if (virtual_address >= image_size_) {
                continue;
            }

            const std::size_t declared_size = std::max<std::size_t>(
                section[i].Misc.VirtualSize,
                section[i].SizeOfRawData);
            const std::size_t bounded_size = std::min(
                declared_size,
                image_size_ - virtual_address);
            if (bounded_size == 0) {
                continue;
            }

            ModuleSection view{};
            view.begin = base_ + virtual_address;
            view.size = bounded_size;
            view.executable = (section[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
            const std::size_t name_length = std::min<std::size_t>(8, strnlen_s(
                reinterpret_cast<const char*>(section[i].Name), 8));
            std::memcpy(view.name, section[i].Name, name_length);
            view.name[name_length] = '\0';
            sections_.push_back(view);
        }
    } catch (...) {
        sections_.clear();
        base_ = nullptr;
        image_size_ = 0;
        timestamp_ = 0;
        return false;
    }

    return !sections_.empty();
}

bool ModuleView::contains(const void* address, std::size_t size) const noexcept {
    if (!base_ || !address || size == 0) {
        return false;
    }
    const auto start = reinterpret_cast<std::uintptr_t>(address);
    const auto image_start = reinterpret_cast<std::uintptr_t>(base_);
    const auto image_end = image_start + image_size_;
    if (start < image_start || start >= image_end) {
        return false;
    }
    return size <= image_end - start;
}

bool ModuleView::is_executable(const void* address) const noexcept {
    const auto value = reinterpret_cast<std::uintptr_t>(address);
    for (const auto& section : sections_) {
        if (!section.executable) {
            continue;
        }
        const auto begin = reinterpret_cast<std::uintptr_t>(section.begin);
        const auto end = begin + section.size;
        if (value >= begin && value < end) {
            return true;
        }
    }
    return false;
}

ScanResult ModuleView::scan_executable(std::string_view text_pattern) const {
    const auto pattern = parse_pattern(text_pattern);
    if (pattern.empty()) {
        return {};
    }

    ScanResult result{};
    for (const auto& section : sections_) {
        if (!section.executable) {
            continue;
        }
        const bool keep_going = scan_section(
            section.begin, section.size, pattern, [&](std::uint8_t* candidate) {
                if (!result.address) {
                    result.address = candidate;
                }
                ++result.matches;
                return result.matches < 2;
            });
        if (!keep_going) {
            return result;
        }
    }
    return result;
}

std::vector<std::uint8_t*> ModuleView::scan_executable_all(
    std::string_view text_pattern) const {
    const auto pattern = parse_pattern(text_pattern);
    std::vector<std::uint8_t*> matches;
    if (pattern.empty()) return matches;
    for (const auto& section : sections_) {
        if (!section.executable) continue;
        (void)scan_section(
            section.begin, section.size, pattern, [&](std::uint8_t* candidate) {
                matches.push_back(candidate);
                return true;
            });
    }
    return matches;
}

bool ModuleView::matches(const void* address, std::string_view text_pattern) const noexcept {
    const auto pattern = parse_pattern(text_pattern);
    if (pattern.empty() || !contains(address, pattern.size())) {
        return false;
    }
    return guarded_matches(reinterpret_cast<const std::uint8_t*>(address), pattern);
}

std::uint8_t* ModuleView::at_rva(std::uintptr_t rva) const noexcept {
    if (!base_ || rva >= image_size_) {
        return nullptr;
    }
    return base_ + rva;
}

std::uint8_t* resolve_rel32_call(
    const ModuleView& module,
    std::uint8_t* instruction) noexcept {
    if (!instruction || !module.contains(instruction, 5) || instruction[0] != 0xE8) {
        return nullptr;
    }

    std::int32_t displacement = 0;
    std::memcpy(&displacement, instruction + 1, sizeof(displacement));
    auto* target = instruction + 5 + displacement;
    return module.is_executable(target) ? target : nullptr;
}


std::uint8_t* resolve_rip_relative(
    const ModuleView& module,
    std::uint8_t* instruction,
    std::size_t displacement_offset,
    std::size_t instruction_size) noexcept {
    if (!instruction || instruction_size < displacement_offset + sizeof(std::int32_t) ||
        !module.contains(instruction, instruction_size)) {
        return nullptr;
    }

    std::int32_t displacement = 0;
    std::memcpy(
        &displacement,
        instruction + displacement_offset,
        sizeof(displacement));
    auto* target = instruction + instruction_size + displacement;
    return module.contains(target) ? target : nullptr;
}

} // namespace erui::native
