#include "menu.hpp"
#include "menu_compiler.hpp"
#include "native_row_presentation.hpp"
#include "row_presentation_state.hpp"

#include <cassert>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr ERUI_ProviderHandle kProvider = 41;
constexpr ERUI_RowHandle kRow = 73;

std::unique_ptr<erui::detail::CompiledMenu> make_menu() {
    erui::Menu menu(L"Host", L"Help");
    volatile std::uint8_t value = 1;
    menu.root().add_slider(
        L"Original label", L"Original help", value, {0, 2, 1});
    menu.root().identify_last_row(kProvider, kRow);
    return erui::detail::MenuCompiler::compile(menu);
}

} // namespace

int main() {
    using erui::detail::RowPresentationUpdate;
    using erui::native::WidgetTextLayout;
    using erui::native::WidgetTextRole;
    using erui::native::WidgetTextTarget;

    // A shared Text_1/Text child is not enough to identify its semantic role.
    // Family A adds nested Text_0/Text; Family B instead has a direct Text_0
    // row label. This guards the exact ambiguity that once edited the control
    // caption/value instead of the left row label.
    assert(erui::native::select_widget_text_layout(false, false, false) ==
        WidgetTextLayout::none);
    assert(erui::native::select_widget_text_layout(true, false, false) ==
        WidgetTextLayout::none);
    assert(erui::native::select_widget_text_layout(false, true, true) ==
        WidgetTextLayout::none);
    assert(erui::native::select_widget_text_layout(true, true, false) ==
        WidgetTextLayout::family_a);
    assert(erui::native::select_widget_text_layout(true, true, true) ==
        WidgetTextLayout::family_a);
    assert(erui::native::select_widget_text_layout(true, false, true) ==
        WidgetTextLayout::family_b);
    assert(erui::native::select_widget_text_target(
        WidgetTextLayout::none, WidgetTextRole::row_label) ==
        WidgetTextTarget::none);
    assert(erui::native::select_widget_text_target(
        WidgetTextLayout::family_a, WidgetTextRole::row_label) ==
        WidgetTextTarget::nested_text_1);
    assert(erui::native::select_widget_text_target(
        WidgetTextLayout::family_a, WidgetTextRole::control_caption) ==
        WidgetTextTarget::nested_text_0);
    assert(erui::native::select_widget_text_target(
        WidgetTextLayout::family_b, WidgetTextRole::row_label) ==
        WidgetTextTarget::direct_text_0);
    assert(erui::native::select_widget_text_target(
        WidgetTextLayout::family_b, WidgetTextRole::control_caption) ==
        WidgetTextTarget::nested_text_1);

    erui::detail::reset_row_presentation_state();
    assert(erui::detail::submit_row_presentation_updates({
        RowPresentationUpdate{
            .provider = kProvider,
            .row = kRow,
            .label = L"Before compile",
            .help = L"Queued before compile",
        },
    }) == ERUI_OK);

    auto menu = make_menu();
    assert(erui::detail::configure_row_presentation_state(*menu));
    assert(erui::detail::editable_row_presentation_count() == 1);
    assert(menu->root_page().rows[0].presentation_slot == 0);
    assert(erui::detail::row_presentation_updates_pending());

    erui::detail::synchronize_pending_row_presentations();
    auto first = erui::detail::row_presentation_view(0);
    assert(first);
    assert(std::wstring(first.label) == L"Before compile");
    assert(std::wstring(first.help) == L"Queued before compile");
    assert(first.caption == nullptr);
    const wchar_t* const stable_label = first.label;
    const wchar_t* const stable_help = first.help;
    const std::uint64_t first_revision = first.revision;

    assert(erui::detail::submit_row_presentation_updates({
        RowPresentationUpdate{
            .provider = kProvider,
            .row = kRow,
            .label = L"After compile",
            .help = L"",
            .caption = L"Caption after compile",
        },
    }) == ERUI_OK);

    // Publication never mutates buffers from the caller thread.
    auto before_sync = erui::detail::row_presentation_view(0);
    assert(before_sync.revision == first_revision);
    assert(std::wstring(before_sync.label) == L"Before compile");
    assert(std::wstring(before_sync.help) == L"Queued before compile");
    assert(before_sync.caption == nullptr);

    erui::detail::synchronize_pending_row_presentations();
    auto second = erui::detail::row_presentation_view(0);
    assert(second.revision > first_revision);
    assert(second.label == stable_label);
    assert(second.help == stable_help);
    assert(second.caption != nullptr);
    assert(std::wstring(second.label) == L"After compile");
    assert(std::wstring(second.help).empty());
    assert(std::wstring(second.caption) == L"Caption after compile");
    const wchar_t* const stable_caption = second.caption;

    assert(erui::detail::submit_row_presentation_updates({
        RowPresentationUpdate{
            .provider = kProvider,
            .row = kRow,
            .label = L"After compile",
            .help = L"Caption-only generation preserves companions",
            .caption = L"Second caption",
        },
    }) == ERUI_OK);
    const auto caption_before_sync =
        erui::detail::row_presentation_view(0);
    assert(caption_before_sync.revision == second.revision);
    assert(caption_before_sync.caption == stable_caption);
    assert(std::wstring(caption_before_sync.caption) ==
        L"Caption after compile");

    erui::detail::synchronize_pending_row_presentations();
    const auto caption_second = erui::detail::row_presentation_view(0);
    assert(caption_second.revision > second.revision);
    assert(caption_second.label == stable_label);
    assert(caption_second.help == stable_help);
    assert(caption_second.caption == stable_caption);
    assert(std::wstring(caption_second.label) == L"After compile");
    assert(std::wstring(caption_second.help) ==
        L"Caption-only generation preserves companions");
    assert(std::wstring(caption_second.caption) == L"Second caption");

    // Invalid snapshots are rejected before touching the accepted state.
    assert(erui::detail::submit_row_presentation_updates({
        RowPresentationUpdate{
            .provider = kProvider,
            .row = kRow,
            .label = L"",
            .help = L"invalid",
            .caption = L"Must not publish",
        },
    }) == ERUI_INVALID_ARGUMENT);
    erui::detail::synchronize_pending_row_presentations();
    auto after_invalid = erui::detail::row_presentation_view(0);
    assert(after_invalid.revision == caption_second.revision);
    assert(std::wstring(after_invalid.label) == L"After compile");
    assert(std::wstring(after_invalid.caption) == L"Second caption");

    // Caption overrides are optional, but an override itself is non-empty and
    // bounded like the other encoded Scaleform strings.
    assert(erui::detail::submit_row_presentation_updates({
        RowPresentationUpdate{
            .provider = kProvider,
            .row = kRow,
            .label = L"Still valid",
            .help = L"Still valid",
            .caption = L"",
        },
    }) == ERUI_INVALID_ARGUMENT);
    std::wstring oversized_caption(
        erui::detail::row_presentation_max_text_units + 1u, L'X');
    assert(erui::detail::submit_row_presentation_updates({
        RowPresentationUpdate{
            .provider = kProvider,
            .row = kRow,
            .label = L"Still valid",
            .help = L"Still valid",
            .caption = std::move(oversized_caption),
        },
    }) == ERUI_INVALID_ARGUMENT);

    // A configured store rejects unknown identities without publishing any
    // other record in the same generation.
    assert(erui::detail::submit_row_presentation_updates({
        RowPresentationUpdate{
            .provider = kProvider,
            .row = kRow,
            .label = L"Must not publish",
            .help = L"Must not publish",
            .caption = L"Must not publish",
        },
        RowPresentationUpdate{
            .provider = kProvider,
            .row = 999,
            .label = L"Unknown",
            .help = L"Unknown",
            .caption = L"Unknown",
        },
    }) == ERUI_INVALID_HANDLE);
    erui::detail::synchronize_pending_row_presentations();
    auto atomic = erui::detail::row_presentation_view(0);
    assert(atomic.revision == caption_second.revision);
    assert(std::wstring(atomic.label) == L"After compile");
    assert(std::wstring(atomic.caption) == L"Second caption");

    erui::detail::reset_row_presentation_state();
    assert(erui::detail::editable_row_presentation_count() == 0);
    assert(!erui::detail::row_presentation_view(0));

    // A caption accepted before compilation becomes part of the first stable
    // UI-boundary snapshot just like label and help.
    assert(erui::detail::submit_row_presentation_updates({
        RowPresentationUpdate{
            .provider = kProvider,
            .row = kRow,
            .label = L"Preconfigured label",
            .help = L"Preconfigured help",
            .caption = L"Preconfigured caption",
        },
    }) == ERUI_OK);
    auto preconfigured_menu = make_menu();
    assert(erui::detail::configure_row_presentation_state(
        *preconfigured_menu));
    erui::detail::synchronize_pending_row_presentations();
    const auto preconfigured = erui::detail::row_presentation_view(0);
    assert(preconfigured);
    assert(std::wstring(preconfigured.label) == L"Preconfigured label");
    assert(std::wstring(preconfigured.help) == L"Preconfigured help");
    assert(std::wstring(preconfigured.caption) == L"Preconfigured caption");
    erui::detail::reset_row_presentation_state();
    return 0;
}
