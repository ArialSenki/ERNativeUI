#pragma once

// r150: names the module that owns an inline hook jump found on a game entry,
// so a log says "already hooked by Solid Uncapper.dll" instead of only
// "another module". Log-only: nothing here decides what gets installed.

#include "guarded_memory.hpp"

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace erui::native {

struct ForeignHookOwner {
    bool hooked{};              // the entry starts with a recognised hook jump
    const void* destination{};  // where the (chained) jumps finally lead
    char name[128]{};           // UTF-8 file name, or "unknown module (0x...)"
};

namespace foreign_hook_detail {

// r150: committed, readable memory only; the SEH copy below is the second
// guard (it is a plain memcpy outside MSVC).
[[nodiscard]] inline bool readable(const void* address, std::size_t size) noexcept {
    const auto* cursor = static_cast<const std::uint8_t*>(address);
    const auto* const end = cursor + size;
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION memory{};
        if (VirtualQuery(cursor, &memory, sizeof(memory)) != sizeof(memory) ||
            memory.State != MEM_COMMIT ||
            (memory.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0 ||
            memory.Protect == 0) {
            return false;
        }
        cursor = static_cast<const std::uint8_t*>(memory.BaseAddress) +
            memory.RegionSize;
    }
    return true;
}

[[nodiscard]] inline bool read(void* out, const void* address, std::size_t size) noexcept {
    return address && readable(address, size) &&
        erui::detail::guarded_copy(out, address, size);
}

// r150: the three jump forms matches_behind_hook_jump accepts:
// E9 rel32, FF 25 [rip+disp32], and 48 B8 imm64 FF E0. Returns the jump
// destination, or nullptr when the entry starts with none of them.
[[nodiscard]] inline const std::uint8_t* decode_jump(const std::uint8_t* at) noexcept {
    std::uint8_t head[2]{};
    if (!read(head, at, sizeof(head))) return nullptr;
    if (head[0] == 0xE9) {
        std::int32_t rel{};
        if (!read(&rel, at + 1, sizeof(rel))) return nullptr;
        return at + 5 + rel;
    }
    if (head[0] == 0xFF && head[1] == 0x25) {
        std::int32_t disp{};
        if (!read(&disp, at + 2, sizeof(disp))) return nullptr;
        const std::uint8_t* slot = at + 6 + disp;
        std::uintptr_t target{};
        if (!read(&target, slot, sizeof(target))) return nullptr;
        return reinterpret_cast<const std::uint8_t*>(target);
    }
    if (head[0] == 0x48 && head[1] == 0xB8) {
        std::uint8_t rest[10]{};
        if (!read(rest, at + 2, sizeof(rest)) || rest[8] != 0xFF || rest[9] != 0xE0) {
            return nullptr;
        }
        std::uintptr_t target{};
        std::memcpy(&target, rest, sizeof(target));
        return reinterpret_cast<const std::uint8_t*>(target);
    }
    return nullptr;
}

[[nodiscard]] inline HMODULE module_at(const void* address) noexcept {
    HMODULE module{};
    if (!address || !GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            static_cast<LPCWSTR>(address),
            &module)) {
        return nullptr;
    }
    return module;
}

// r150: file name only, so user folders never reach the log.
[[nodiscard]] inline bool module_file_name(
    HMODULE module, char* out, std::size_t out_size) noexcept {
    wchar_t path[1024]{};
    const DWORD length = GetModuleFileNameW(
        module, path, static_cast<DWORD>(sizeof(path) / sizeof(path[0])));
    if (length == 0 || length >= sizeof(path) / sizeof(path[0])) return false;
    const wchar_t* file = path;
    for (const wchar_t* cursor = path; *cursor; ++cursor) {
        if (*cursor == L'\\' || *cursor == L'/') file = cursor + 1;
    }
    if (!*file) return false;
    const int written = WideCharToMultiByte(
        CP_UTF8, 0, file, -1, out, static_cast<int>(out_size), nullptr, nullptr);
    if (written <= 0) {
        out[0] = '\0';
        return false;
    }
    return true;
}

} // namespace foreign_hook_detail

// r150: decodes the hook jump at `entry` and follows at most a few chained
// jumps (MinHook/SafetyHook relays near the target usually hold a FF 25 to
// the real detour) while the destination is in no module or in the game
// executable itself; the first module found elsewhere owns the detour.
[[nodiscard]] inline ForeignHookOwner find_foreign_hook_owner(const void* entry) noexcept {
    using namespace foreign_hook_detail;
    ForeignHookOwner owner{};
    const auto* destination = decode_jump(static_cast<const std::uint8_t*>(entry));
    if (!destination) return owner;
    owner.hooked = true;

    const HMODULE game = GetModuleHandleW(nullptr);
    HMODULE module = module_at(destination);
    for (int hop = 0; hop < 3 && (!module || module == game); ++hop) {
        const std::uint8_t* const next = decode_jump(destination);
        if (!next) break;
        destination = next;
        module = module_at(destination);
    }
    owner.destination = destination;

    if (!module || !module_file_name(module, owner.name, sizeof(owner.name))) {
        std::snprintf(
            owner.name,
            sizeof(owner.name),
            "unknown module (%p)",
            static_cast<const void*>(destination));
    }
    return owner;
}

} // namespace erui::native
