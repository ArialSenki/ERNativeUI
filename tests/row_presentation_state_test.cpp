#include "menu.hpp"
#include "menu_compiler.hpp"
#include "row_presentation_state.hpp"

#include <cassert>
#include <string>
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
    const wchar_t* const stable_label = first.label;
    const wchar_t* const stable_help = first.help;
    const std::uint64_t first_revision = first.revision;

    assert(erui::detail::submit_row_presentation_updates({
        RowPresentationUpdate{
            .provider = kProvider,
            .row = kRow,
            .label = L"After compile",
            .help = L"",
        },
    }) == ERUI_OK);

    // Publication never mutates buffers from the caller thread.
    auto before_sync = erui::detail::row_presentation_view(0);
    assert(before_sync.revision == first_revision);
    assert(std::wstring(before_sync.label) == L"Before compile");
    assert(std::wstring(before_sync.help) == L"Queued before compile");

    erui::detail::synchronize_pending_row_presentations();
    auto second = erui::detail::row_presentation_view(0);
    assert(second.revision > first_revision);
    assert(second.label == stable_label);
    assert(second.help == stable_help);
    assert(std::wstring(second.label) == L"After compile");
    assert(std::wstring(second.help).empty());

    // Invalid snapshots are rejected before touching the accepted state.
    assert(erui::detail::submit_row_presentation_updates({
        RowPresentationUpdate{
            .provider = kProvider,
            .row = kRow,
            .label = L"",
            .help = L"invalid",
        },
    }) == ERUI_INVALID_ARGUMENT);
    erui::detail::synchronize_pending_row_presentations();
    auto after_invalid = erui::detail::row_presentation_view(0);
    assert(after_invalid.revision == second.revision);
    assert(std::wstring(after_invalid.label) == L"After compile");

    // A configured store rejects unknown identities without publishing any
    // other record in the same generation.
    assert(erui::detail::submit_row_presentation_updates({
        RowPresentationUpdate{
            .provider = kProvider,
            .row = kRow,
            .label = L"Must not publish",
            .help = L"Must not publish",
        },
        RowPresentationUpdate{
            .provider = kProvider,
            .row = 999,
            .label = L"Unknown",
            .help = L"Unknown",
        },
    }) == ERUI_INVALID_HANDLE);
    erui::detail::synchronize_pending_row_presentations();
    auto atomic = erui::detail::row_presentation_view(0);
    assert(atomic.revision == second.revision);
    assert(std::wstring(atomic.label) == L"After compile");

    erui::detail::reset_row_presentation_state();
    assert(erui::detail::editable_row_presentation_count() == 0);
    assert(!erui::detail::row_presentation_view(0));
    return 0;
}
