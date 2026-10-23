#pragma once

#include "menu_compiler.hpp"

#include <cstddef>
#include <cstdint>

namespace erui::native {

struct GameAddresses;
class ModuleView;

enum class WidgetTextLayout : std::uint8_t {
    none,
    family_a,
    family_b,
};

enum class WidgetTextRole : std::uint8_t {
    row_label,
    control_caption,
};

enum class WidgetTextTarget : std::uint8_t {
    none,
    direct_text_0,
    nested_text_0,
    nested_text_1,
};

// The two supported movie families contain the same nested Text_1/Text path,
// but assign it opposite semantic roles. Select a layout only from a complete
// structural signature; a single resolvable path is intentionally ambiguous.
[[nodiscard]] constexpr WidgetTextLayout select_widget_text_layout(
    bool common_nested_text,
    bool family_a_nested_caption,
    bool family_b_direct_label) noexcept {
    if (!common_nested_text) return WidgetTextLayout::none;
    if (family_a_nested_caption) return WidgetTextLayout::family_a;
    if (family_b_direct_label) return WidgetTextLayout::family_b;
    return WidgetTextLayout::none;
}

// Family A and Family B deliberately reverse the semantic roles of their
// nested text fields. Keep this mapping separate from path resolution so it
// can be regression-tested without a live Scaleform movie.
[[nodiscard]] constexpr WidgetTextTarget select_widget_text_target(
    WidgetTextLayout layout,
    WidgetTextRole role) noexcept {
    switch (layout) {
    case WidgetTextLayout::family_a:
        return role == WidgetTextRole::row_label
            ? WidgetTextTarget::nested_text_1
            : WidgetTextTarget::nested_text_0;
    case WidgetTextLayout::family_b:
        return role == WidgetTextRole::row_label
            ? WidgetTextTarget::direct_text_0
            : WidgetTextTarget::nested_text_1;
    default:
        return WidgetTextTarget::none;
    }
}

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
// page-frame callback. The normal hook calls this after the native frame so
// action-like widgets cannot overwrite edited row labels or control captions.
// A paused frame may also call it directly while a native modal dialog owns
// the update path.
void apply_native_row_presentations(
    void* page,
    const GameAddresses& addresses) noexcept;

void retire_native_row_presentation_page(void* page) noexcept;
void reset_native_row_presentations() noexcept;

} // namespace erui::native
