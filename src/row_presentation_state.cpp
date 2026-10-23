#include "row_presentation_state.hpp"

#include <algorithm>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace erui::detail {
namespace {

struct RowIdentity {
    ERUI_ProviderHandle provider{};
    ERUI_RowHandle row{};

    friend bool operator==(const RowIdentity&, const RowIdentity&) = default;
};

struct RowIdentityHash {
    std::size_t operator()(const RowIdentity& value) const noexcept {
        const std::size_t first = std::hash<std::uint64_t>{}(value.provider);
        const std::size_t second = std::hash<std::uint64_t>{}(value.row);
        return first ^ (second + static_cast<std::size_t>(0x9E3779B9u) +
            (first << 6u) + (first >> 2u));
    }
};

struct PendingSnapshot {
    std::wstring label{};
    std::wstring help{};
    std::uint64_t revision{};
};

struct Entry {
    RowIdentity identity{};
    std::wstring desired_label{};
    std::wstring desired_help{};
    std::unique_ptr<wchar_t[]> label_buffer{};
    std::unique_ptr<wchar_t[]> help_buffer{};
    std::uint64_t desired_revision{};
    std::uint64_t applied_revision{};
    bool dirty{};
};

std::mutex g_mutex{};
std::vector<std::unique_ptr<Entry>> g_entries{};
std::unordered_map<RowIdentity, std::size_t, RowIdentityHash> g_slots{};
std::unordered_map<RowIdentity, PendingSnapshot, RowIdentityHash>
    g_preconfiguration_updates{};
std::uint64_t g_revision{};
bool g_configured{};
bool g_pending{};

bool valid_snapshot(const RowPresentationUpdate& update) noexcept {
    return update.provider != ERUI_INVALID_PROVIDER &&
        update.row != ERUI_INVALID_ROW && !update.label.empty() &&
        update.label.size() <= row_presentation_max_text_units &&
        update.help.size() <= row_presentation_max_text_units;
}

void copy_to_buffer(
    const std::wstring& source,
    wchar_t* destination) noexcept {
    std::copy(source.begin(), source.end(), destination);
    destination[source.size()] = L'\0';
}

} // namespace

ERUI_Result submit_row_presentation_updates(
    std::vector<RowPresentationUpdate> updates) noexcept {
    if (updates.empty()) return ERUI_INVALID_ARGUMENT;
    for (const RowPresentationUpdate& update : updates) {
        if (!valid_snapshot(update)) return ERUI_INVALID_ARGUMENT;
    }

    try {
        struct PreparedBuffers {
            std::unique_ptr<wchar_t[]> label{};
            std::unique_ptr<wchar_t[]> help{};
        };
        std::lock_guard lock(g_mutex);
        std::uint64_t revision = g_revision + 1u;
        if (revision == 0) revision = 1;

        if (!g_configured) {
            // The registry has already validated duplicate targets. Reserve
            // all nodes first so map growth cannot partially publish.
            auto staged = g_preconfiguration_updates;
            for (RowPresentationUpdate& update : updates) {
                staged[RowIdentity{update.provider, update.row}] = {
                    .label = std::move(update.label),
                    .help = std::move(update.help),
                    .revision = revision,
                };
            }
            g_preconfiguration_updates = std::move(staged);
            g_revision = revision;
            g_pending = true;
            return ERUI_OK;
        }

        std::vector<std::size_t> slots{};
        slots.reserve(updates.size());
        for (const RowPresentationUpdate& update : updates) {
            const auto found = g_slots.find(
                RowIdentity{update.provider, update.row});
            if (found == g_slots.end() || found->second >= g_entries.size()) {
                return ERUI_INVALID_HANDLE;
            }
            slots.push_back(found->second);
        }

        // Allocate only buffers that do not exist yet. Every allocation is
        // completed before publication, preserving all-or-nothing OOM
        // behavior without churning two 4097-unit buffers on every edit.
        // This work never occurs at the UI frame boundary.
        std::vector<PreparedBuffers> prepared(updates.size());
        for (std::size_t index = 0; index < updates.size(); ++index) {
            const Entry& entry = *g_entries[slots[index]];
            if (!entry.label_buffer) {
                prepared[index].label = std::make_unique<wchar_t[]>(
                    row_presentation_max_text_units + 1u);
            }
            if (!entry.help_buffer) {
                prepared[index].help = std::make_unique<wchar_t[]>(
                    row_presentation_max_text_units + 1u);
            }
        }

        // All validation and allocations succeeded. Moving strings and
        // unique_ptrs is noexcept, so publication below cannot be partial.
        for (std::size_t index = 0; index < updates.size(); ++index) {
            Entry& entry = *g_entries[slots[index]];
            if (!entry.label_buffer) {
                entry.label_buffer = std::move(prepared[index].label);
            }
            if (!entry.help_buffer) {
                entry.help_buffer = std::move(prepared[index].help);
            }
            entry.desired_label = std::move(updates[index].label);
            entry.desired_help = std::move(updates[index].help);
            entry.desired_revision = revision;
            entry.dirty = true;
        }
        g_revision = revision;
        g_pending = true;
        return ERUI_OK;
    } catch (const std::bad_alloc&) {
        return ERUI_OUT_OF_MEMORY;
    } catch (...) {
        return ERUI_INTERNAL_ERROR;
    }
}

bool configure_row_presentation_state(CompiledMenu& menu) noexcept {
    try {
        std::lock_guard lock(g_mutex);
        if (g_configured || !g_entries.empty() || !g_slots.empty()) {
            return false;
        }

        std::vector<std::unique_ptr<Entry>> entries{};
        std::unordered_map<RowIdentity, std::size_t, RowIdentityHash> slots{};
        std::size_t count = 0;
        for (const CompiledPage& page : menu.pages) {
            for (const CompiledRow& row : page.rows) {
                if (row.provider != ERUI_INVALID_PROVIDER &&
                    row.row != ERUI_INVALID_ROW) {
                    ++count;
                }
            }
        }
        entries.reserve(count);
        slots.reserve(count);

        for (CompiledPage& page : menu.pages) {
            for (CompiledRow& row : page.rows) {
                if (row.provider == ERUI_INVALID_PROVIDER ||
                    row.row == ERUI_INVALID_ROW) {
                    row.presentation_slot = invalid_compiled_index;
                    continue;
                }
                const RowIdentity identity{row.provider, row.row};
                auto entry = std::make_unique<Entry>();
                entry->identity = identity;
                const wchar_t* const label = menu.texts.lookup(
                    static_cast<int>(row.label_id));
                const wchar_t* const help = menu.texts.lookup(
                    static_cast<int>(row.help_id));
                entry->desired_label = label ? label : L"";
                entry->desired_help = help ? help : L"";

                if (const auto pending =
                        g_preconfiguration_updates.find(identity);
                    pending != g_preconfiguration_updates.end()) {
                    entry->desired_label = pending->second.label;
                    entry->desired_help = pending->second.help;
                    entry->desired_revision = pending->second.revision;
                    entry->label_buffer = std::make_unique<wchar_t[]>(
                        row_presentation_max_text_units + 1u);
                    entry->help_buffer = std::make_unique<wchar_t[]>(
                        row_presentation_max_text_units + 1u);
                    entry->dirty = true;
                    g_pending = true;
                }

                const std::size_t slot = entries.size();
                if (!slots.emplace(identity, slot).second) return false;
                row.presentation_slot = slot;
                entries.push_back(std::move(entry));
            }
        }

        // A pre-runtime update naming a row that never reached the compiled
        // menu is a violated registry/compiler invariant. Fail closed.
        for (const auto& pending : g_preconfiguration_updates) {
            if (slots.find(pending.first) == slots.end()) return false;
        }

        g_entries = std::move(entries);
        g_slots = std::move(slots);
        g_preconfiguration_updates.clear();
        g_configured = true;
        return true;
    } catch (...) {
        return false;
    }
}

void synchronize_pending_row_presentations() noexcept {
    std::lock_guard lock(g_mutex);
    if (!g_configured || !g_pending) return;
    bool still_pending = false;
    for (const std::unique_ptr<Entry>& entry_pointer : g_entries) {
        Entry& entry = *entry_pointer;
        if (!entry.dirty) continue;
        if (!entry.label_buffer || !entry.help_buffer ||
            entry.desired_label.empty() ||
            entry.desired_label.size() > row_presentation_max_text_units ||
            entry.desired_help.size() > row_presentation_max_text_units) {
            still_pending = true;
            continue;
        }
        copy_to_buffer(entry.desired_label, entry.label_buffer.get());
        copy_to_buffer(entry.desired_help, entry.help_buffer.get());
        entry.applied_revision = entry.desired_revision;
        entry.dirty = false;
    }
    g_pending = still_pending;
}

RowPresentationView row_presentation_view(std::size_t slot) noexcept {
    std::lock_guard lock(g_mutex);
    if (!g_configured || slot >= g_entries.size()) return {};
    const Entry& entry = *g_entries[slot];
    if (entry.applied_revision == 0 || !entry.label_buffer ||
        !entry.help_buffer) {
        return {};
    }
    return {
        .label = entry.label_buffer.get(),
        .help = entry.help_buffer.get(),
        .revision = entry.applied_revision,
    };
}

bool row_presentation_updates_pending() noexcept {
    std::lock_guard lock(g_mutex);
    return g_pending;
}

std::size_t editable_row_presentation_count() noexcept {
    std::lock_guard lock(g_mutex);
    return g_entries.size();
}

void reset_row_presentation_state() noexcept {
    std::lock_guard lock(g_mutex);
    g_entries.clear();
    g_slots.clear();
    g_preconfiguration_updates.clear();
    g_revision = 0;
    g_configured = false;
    g_pending = false;
}

} // namespace erui::detail
