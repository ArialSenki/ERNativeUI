#pragma once

#include "menu_compiler.hpp"

#include <cstddef>
#include <cstdint>

namespace erui::native {

struct GameAddresses;
class ModuleView;

struct RowPresentationMaterializationTicket {
    void* page{};
    std::size_t presentation_slot{detail::invalid_compiled_index};
    std::size_t previous_count{};
    std::uint64_t page_epoch{};
    bool armed{};
};

// Exact-build availability check used before API provider registration opens.
[[nodiscard]] bool native_row_presentation_build_supported(
    const ModuleView& game) noexcept;

// Starts a new physical-page binding epoch. Existing bindings for the same
// address are retired without dereferencing them.
void begin_native_row_presentation_page(
    void* page,
    const GameAddresses& addresses) noexcept;

[[nodiscard]] RowPresentationMaterializationTicket
begin_native_row_presentation_materialization(
    void* page,
    const GameAddresses& addresses,
    const detail::CompiledRow& row) noexcept;

void complete_native_row_presentation_materialization(
    void* page,
    const GameAddresses& addresses,
    const detail::CompiledRow& row,
    RowPresentationMaterializationTicket ticket,
    bool constructor_succeeded) noexcept;

// Synchronizes only bindings owned by the exact page supplied by the game's
// page-frame callback. Must run before the native frame original.
void apply_native_row_presentations(
    void* page,
    const GameAddresses& addresses) noexcept;

void retire_native_row_presentation_page(void* page) noexcept;
void reset_native_row_presentations() noexcept;

} // namespace erui::native
