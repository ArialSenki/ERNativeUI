#include "runtime_gfx_locate.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace erui::host {
namespace {

template <class T>
T read_at(const void* base, std::size_t offset) noexcept {
    T value{};
    std::memcpy(&value, static_cast<const std::uint8_t*>(base) + offset, sizeof(T));
    return value;
}

const erui::native::ModuleSection* section_named(
    const erui::native::ModuleView& game, std::string_view name) noexcept {
    for (const auto& section : game.sections()) {
        if (name == section.name) return &section;
    }
    return nullptr;
}

bool code_is(const erui::native::ModuleView& game, void* function,
    std::string_view pattern) noexcept {
    return function && game.is_executable(function) && game.matches(function, pattern);
}

} // namespace

void** find_rtti_vtable(
    const erui::native::ModuleView& game, std::string_view decorated_name) noexcept {
    auto* const base = game.base();
    if (!base || decorated_name.empty()) return nullptr;

    // TypeDescriptor: vftable pointer, spare pointer, NUL-terminated name.
    std::uint32_t descriptor_rva = 0;
    for (const auto& section : game.sections()) {
        if (section.executable || section.size < decorated_name.size() + 17) continue;
        for (std::size_t offset = 16;
             offset + decorated_name.size() + 1 <= section.size; ++offset) {
            if (section.begin[offset] != static_cast<std::uint8_t>(decorated_name[0]) ||
                std::memcmp(section.begin + offset, decorated_name.data(),
                    decorated_name.size()) != 0 ||
                section.begin[offset + decorated_name.size()] != 0) {
                continue;
            }
            descriptor_rva = static_cast<std::uint32_t>(section.begin + offset - 16 - base);
            break;
        }
        if (descriptor_rva) break;
    }
    if (!descriptor_rva) return nullptr;

    const auto* rdata = section_named(game, ".rdata");
    if (!rdata) return nullptr;
    // x64 CompleteObjectLocator: signature 1, offset 0, cd offset, type
    // descriptor RVA (+0x0C), class descriptor RVA, self RVA (+0x14).
    const std::uint8_t* locator = nullptr;
    for (std::size_t offset = 0; offset + 24 <= rdata->size; offset += 4) {
        const std::uint8_t* col = rdata->begin + offset;
        if (read_at<std::uint32_t>(col, 0) != 1 || read_at<std::uint32_t>(col, 4) != 0 ||
            read_at<std::uint32_t>(col, 12) != descriptor_rva ||
            read_at<std::uint32_t>(col, 20) != static_cast<std::uint32_t>(col - base)) {
            continue;
        }
        locator = col;
        break;
    }
    if (!locator) return nullptr;
    const auto locator_address = reinterpret_cast<std::uintptr_t>(locator);
    for (std::size_t offset = 0; offset + 16 <= rdata->size; offset += 8) {
        if (read_at<std::uintptr_t>(rdata->begin, offset) != locator_address) continue;
        return reinterpret_cast<void**>(rdata->begin + offset + 8);
    }
    return nullptr;
}

bool locate_runtime_gfx_targets(
    const erui::native::ModuleView& game, RuntimeGfxTargets& targets) noexcept {
    targets = {};
    targets.file_opener_vtable = find_rtti_vtable(game, ".?AVCSScaleformFileOpener@CS@@");
    if (!targets.file_opener_vtable) {
        targets.failure = "CSScaleformFileOpener RTTI not found";
        return false;
    }
    targets.memory_file_vtable = find_rtti_vtable(game, ".?AVMemoryFile@Scaleform@@");
    if (!targets.memory_file_vtable) {
        targets.failure = "Scaleform::MemoryFile RTTI not found";
        return false;
    }
    void** memory_file = targets.memory_file_vtable;
    for (std::size_t slot = 0; slot < 20; ++slot) {
        if (!game.contains(memory_file + slot, sizeof(void*)) ||
            !game.is_executable(memory_file[slot])) {
            targets.failure = "MemoryFile vtable has fewer than 20 code slots";
            return false;
        }
    }
    // IsValid (+0x28 byte), Tell (+0x24), GetLength (+0x20).
    if (!code_is(game, memory_file[2], "0F B6 41 28 C3") ||
        !code_is(game, memory_file[4], "8B 41 24 C3") ||
        !code_is(game, memory_file[6], "8B 41 20 C3")) {
        targets.failure = "MemoryFile field layout differs from the validated build";
        return false;
    }
    if (!game.contains(targets.file_opener_vtable + 1, sizeof(void*)) ||
        !game.is_executable(targets.file_opener_vtable[1])) {
        targets.failure = "CSScaleformFileOpener::OpenFile is not code";
        return false;
    }
    targets.open_file = targets.file_opener_vtable[1];
    return true;
}

} // namespace erui::host
