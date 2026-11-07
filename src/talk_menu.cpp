// Fork (r157): the shared talk menu (F11 by default). See talk_menu.hpp.
//
// Besides the ERNativeUIFork_Talk* exports, every committed ERNativeUI
// provider with a Mod Settings page gets a row: its page and submenus become
// talk pages, a toggle flips when chosen, a slider or choice opens a page with
// its values, and a button runs its callback (the menu closes, as the button
// may open a game menu). Values change as a player edit in the menu would
// (Registry::talk_set_value), so the provider's callbacks and saving run as
// usual. A provider that registered talk pages under the same name keeps only
// those (NPC Spawner).

#include "talk_menu.hpp"

#include "host_registry.hpp"
#include "logger.hpp"

#include "ernativeui/erui_fork_talk.h"

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <deque>
#include <iterator>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// The Rust side (talk/src/lib.rs).
extern "C" {
using erui_talk_log_fn = void (*)(std::uint32_t level, const char* text, std::size_t length);
using erui_talk_before_open_fn = void (*)();
std::uint32_t erui_talk_start(erui_talk_log_fn log, erui_talk_before_open_fn before_open,
    const std::uint16_t* key, std::size_t key_length);
std::uint32_t erui_talk_register(const std::uint16_t* name, std::size_t length,
    ERUI_TalkBuildFn build, ERUI_TalkChooseFn choose, void* user, std::uint64_t* out);
std::uint32_t erui_talk_register_settings(const std::uint16_t* name, std::size_t length,
    ERUI_TalkBuildFn build, ERUI_TalkChooseFn choose, void* user, std::uint64_t* out);
std::uint32_t erui_talk_unregister(std::uint64_t entry);
std::uint32_t erui_talk_add_row(
    ERUI_TalkMenu* menu, std::uint32_t row, const std::uint16_t* text, std::size_t length);
std::uint32_t erui_talk_open_page(ERUI_TalkMenu* menu, std::uint32_t page);
std::uint32_t erui_talk_replace_page(ERUI_TalkMenu* menu, std::uint32_t page);
std::uint32_t erui_talk_back(ERUI_TalkMenu* menu);
std::uint32_t erui_talk_close(ERUI_TalkMenu* menu);
std::uint32_t erui_talk_open(std::uint64_t entry);
std::uint32_t erui_talk_is_open();
}

namespace erui::talk {
namespace {

std::atomic_bool g_started{};

void log_sink(std::uint32_t level, const char* text, std::size_t length) {
    const LogLevel mapped = level >= 2 ? LogLevel::error
        : level == 1 ? LogLevel::warning : LogLevel::info;
    host::log::write(mapped, "%.*s", static_cast<int>(length), text);
}

// ---- Mod Settings pages as talk pages (game thread only)

using Registry = host::Registry;

// Rows per talk page before "Next page" (the host adds Back; a page has
// room for 63 rows).
constexpr std::uint32_t kPerPage = 60;

struct BridgePage {
    ERUI_PageHandle page{};       // a Mod Settings page, or
    ERUI_RowHandle values_row{};  // the row whose values are listed
    std::uint32_t offset{};       // first row shown
};

enum class Act : std::uint8_t { none, press, toggle, values, submenu, set, turn };

struct Action {
    Act act{Act::none};
    ERUI_RowHandle row{};
    std::uint32_t value{};  // toggle: new value; set: index; values: current
    std::uint32_t page{};   // submenu / turn: talk page id
};

struct Bridge {
    ERUI_ProviderHandle provider{};
    std::string name{};
    std::vector<BridgePage> pages{};  // talk page id = index (0 = root)
    std::unordered_map<std::uint32_t, std::vector<Action>> shown{};
};

std::deque<Bridge> g_bridges{};
std::unordered_set<ERUI_ProviderHandle> g_bridged{};

std::uint32_t intern(Bridge& bridge, ERUI_PageHandle page, ERUI_RowHandle values_row,
    std::uint32_t offset) {
    for (std::uint32_t id = 0; id < bridge.pages.size(); ++id) {
        const BridgePage& known = bridge.pages[id];
        if (known.page == page && known.values_row == values_row && known.offset == offset) return id;
    }
    bridge.pages.push_back({page, values_row, offset});
    return static_cast<std::uint32_t>(bridge.pages.size() - 1);
}

void add(ERUI_TalkMenu* menu, std::vector<Action>& shown, const std::wstring& text, Action action) {
    const std::uint32_t row = static_cast<std::uint32_t>(shown.size());
    if (erui_talk_add_row(menu, row, reinterpret_cast<const std::uint16_t*>(text.data()),
            text.size()) == ERUI_OK) {
        shown.push_back(action);
    }
}

std::wstring page_counter(const wchar_t* what, std::uint32_t number, std::uint32_t count) {
    return std::wstring(what) + L" (" + std::to_wstring(number) + L"/" + std::to_wstring(count) + L")";
}

void pagination(Bridge& bridge, ERUI_TalkMenu* menu, std::vector<Action>& shown,
    const BridgePage& at, std::uint32_t total, bool before) {
    const std::uint32_t pages = (total + kPerPage - 1) / kPerPage;
    const std::uint32_t number = at.offset / kPerPage + 1;
    if (before && at.offset > 0) {
        add(menu, shown, page_counter(L"Previous page", number - 1, pages),
            {Act::turn, 0, 0, intern(bridge, at.page, at.values_row, at.offset - kPerPage)});
    }
    if (!before && at.offset + kPerPage < total) {
        add(menu, shown, page_counter(L"Next page", number + 1, pages),
            {Act::turn, 0, 0, intern(bridge, at.page, at.values_row, at.offset + kPerPage)});
    }
}

void ERUI_CALL bridge_build(void* user, ERUI_TalkMenu* menu, std::uint32_t id) {
    Bridge& bridge = *static_cast<Bridge*>(user);
    if (id >= bridge.pages.size()) {
        (void)erui_talk_back(menu);
        return;
    }
    const BridgePage at = bridge.pages[id];
    std::vector<Action>& shown = bridge.shown[id];
    shown.clear();
    Registry& registry = host::registry();

    if (at.values_row != ERUI_INVALID_ROW) {
        std::wstring label;
        std::vector<std::wstring> texts;
        std::uint32_t current = 0;
        if (!registry.talk_values(bridge.provider, at.values_row, label, texts, current)) {
            (void)erui_talk_back(menu);
            return;
        }
        const auto total = static_cast<std::uint32_t>(texts.size());
        pagination(bridge, menu, shown, at, total, true);
        for (std::uint32_t i = at.offset; i < total && i < at.offset + kPerPage; ++i) {
            add(menu, shown, i == current ? texts[i] + L"  (current)" : texts[i],
                {Act::set, at.values_row, i, 0});
        }
        pagination(bridge, menu, shown, at, total, false);
        return;
    }

    std::vector<Registry::TalkRowView> rows;
    if (!registry.talk_page(bridge.provider, at.page, rows)) {
        (void)erui_talk_back(menu);
        return;
    }
    const auto total = static_cast<std::uint32_t>(rows.size());
    pagination(bridge, menu, shown, at, total, true);
    for (std::uint32_t i = at.offset; i < total && i < at.offset + kPerPage; ++i) {
        const Registry::TalkRowView& row = rows[i];
        std::wstring text = row.label;
        Action action{};
        switch (row.kind) {
        case Registry::TalkRowKind::button:
            action = {Act::press, row.handle, 0, 0};
            break;
        case Registry::TalkRowKind::submenu:
            if (text.size() < 3 || text.compare(text.size() - 3, 3, L"...") != 0) text += L"...";
            action = {Act::submenu, row.handle, 0, intern(bridge, row.child_page, ERUI_INVALID_ROW, 0)};
            break;
        case Registry::TalkRowKind::toggle:
            text += L": " + row.value;
            action = {Act::toggle, row.handle, row.current ? 0u : 1u, 0};
            break;
        case Registry::TalkRowKind::values:
            text += L": " + row.value;
            action = {Act::values, row.handle, 0, 0};
            break;
        case Registry::TalkRowKind::text:
            if (!row.value.empty()) text += L": " + row.value;
            break;
        default:
            break;
        }
        if (text.empty()) continue;
        if (!row.enabled) action = {};
        add(menu, shown, text, action);
    }
    pagination(bridge, menu, shown, at, total, false);
}

void ERUI_CALL bridge_choose(void* user, ERUI_TalkMenu* menu, std::uint32_t id, std::uint32_t row) {
    Bridge& bridge = *static_cast<Bridge*>(user);
    const auto shown = bridge.shown.find(id);
    if (shown == bridge.shown.end() || row >= shown->second.size()) return;
    const Action action = shown->second[row];
    Registry& registry = host::registry();
    switch (action.act) {
    case Act::press: {
        const ERUI_Result result = registry.talk_press(bridge.provider, action.row);
        host::log::write("talk menu: %s button %llu pressed (%u)", bridge.name.c_str(),
            static_cast<unsigned long long>(action.row), static_cast<unsigned>(result));
        (void)erui_talk_close(menu);
        break;
    }
    case Act::toggle:
    case Act::set: {
        const ERUI_Result result = registry.talk_set_value(bridge.provider, action.row, action.value);
        host::log::write("talk menu: %s row %llu set to %u (%u)", bridge.name.c_str(),
            static_cast<unsigned long long>(action.row), static_cast<unsigned>(action.value),
            static_cast<unsigned>(result));
        if (action.act == Act::set) (void)erui_talk_back(menu);
        break;
    }
    case Act::values: {
        std::wstring label;
        std::vector<std::wstring> texts;
        std::uint32_t current = 0;
        std::uint32_t offset = 0;
        if (registry.talk_values(bridge.provider, action.row, label, texts, current)) {
            offset = current / kPerPage * kPerPage;
        }
        (void)erui_talk_open_page(menu, intern(bridge, 0, action.row, offset));
        break;
    }
    case Act::submenu:
        (void)erui_talk_open_page(menu, action.page);
        break;
    case Act::turn:
        (void)erui_talk_replace_page(menu, action.page);
        break;
    default:
        break;
    }
}

// Before the top list is built: a row for every provider committed since.
void sync_providers() {
    for (Registry::TalkProviderView& provider : host::registry().talk_providers()) {
        if (!g_bridged.insert(provider.handle).second) continue;
        Bridge& bridge = g_bridges.emplace_back();
        bridge.provider = provider.handle;
        char name[256]{};
        WideCharToMultiByte(CP_UTF8, 0, provider.name.c_str(), -1, name, sizeof(name) - 1,
            nullptr, nullptr);
        bridge.name = name;
        (void)intern(bridge, provider.root_page, ERUI_INVALID_ROW, 0);
        std::uint64_t entry = 0;
        const std::uint32_t result = erui_talk_register_settings(
            reinterpret_cast<const std::uint16_t*>(provider.name.c_str()), provider.name.size(),
            &bridge_build, &bridge_choose, &bridge, &entry);
        if (result != ERUI_OK) {
            host::log::write(LogLevel::warning, "talk menu: no row for \"%s\" (%u)", name,
                static_cast<unsigned>(result));
        }
    }
}

void before_open() {
    try {
        sync_providers();
    } catch (...) {
        host::log::write(LogLevel::error, "talk menu: listing the Mod Settings pages failed");
    }
}

bool passive() noexcept {
    return host::api_state() == host::ApiState::failed;
}

} // namespace

void start(const wchar_t* key) noexcept {
    if (g_started.exchange(true)) return;
    const std::uint32_t result = erui_talk_start(&log_sink, &before_open,
        reinterpret_cast<const std::uint16_t*>(key), key ? std::wcslen(key) : 0u);
    if (result != ERUI_OK) {
        host::log::write(LogLevel::error, "talk menu: start failed (%u)",
            static_cast<unsigned>(result));
    }
}

} // namespace erui::talk

// ---- exports (include/ernativeui/erui_fork_talk.h)

namespace {

bool text_ok(const ERUI_Utf16View* text) noexcept {
    return text && (text->data || text->length == 0);
}

} // namespace

extern "C" ERUI_EXPORT ERUI_Result ERUI_CALL ERNativeUIFork_TalkRegister(
    const ERUI_Utf16View* name, ERUI_TalkBuildFn build, ERUI_TalkChooseFn choose,
    void* user, ERUI_TalkEntryHandle* out) {
    if (erui::talk::passive()) return ERUI_HOST_FAILED;
    if (!text_ok(name)) return ERUI_INVALID_ARGUMENT;
    return erui_talk_register(name->data, name->length, build, choose, user, out);
}

extern "C" ERUI_EXPORT ERUI_Result ERUI_CALL ERNativeUIFork_TalkUnregister(
    ERUI_TalkEntryHandle entry) {
    if (erui::talk::passive()) return ERUI_HOST_FAILED;
    return erui_talk_unregister(entry);
}

extern "C" ERUI_EXPORT ERUI_Result ERUI_CALL ERNativeUIFork_TalkAddRow(
    ERUI_TalkMenu* menu, std::uint32_t row, const ERUI_Utf16View* text) {
    if (!text_ok(text)) return ERUI_INVALID_ARGUMENT;
    return erui_talk_add_row(menu, row, text->data, text->length);
}

extern "C" ERUI_EXPORT ERUI_Result ERUI_CALL ERNativeUIFork_TalkOpenPage(
    ERUI_TalkMenu* menu, std::uint32_t page) {
    return erui_talk_open_page(menu, page);
}

extern "C" ERUI_EXPORT ERUI_Result ERUI_CALL ERNativeUIFork_TalkReplacePage(
    ERUI_TalkMenu* menu, std::uint32_t page) {
    return erui_talk_replace_page(menu, page);
}

extern "C" ERUI_EXPORT ERUI_Result ERUI_CALL ERNativeUIFork_TalkBack(ERUI_TalkMenu* menu) {
    return erui_talk_back(menu);
}

extern "C" ERUI_EXPORT ERUI_Result ERUI_CALL ERNativeUIFork_TalkClose(ERUI_TalkMenu* menu) {
    return erui_talk_close(menu);
}

extern "C" ERUI_EXPORT ERUI_Result ERUI_CALL ERNativeUIFork_TalkOpen(
    ERUI_TalkEntryHandle entry) {
    if (erui::talk::passive()) return ERUI_HOST_FAILED;
    return erui_talk_open(entry);
}

extern "C" ERUI_EXPORT std::uint32_t ERUI_CALL ERNativeUIFork_TalkIsOpen() {
    return erui_talk_is_open();
}
