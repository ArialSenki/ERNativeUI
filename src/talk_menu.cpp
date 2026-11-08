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

#include <safetyhook.hpp>

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
using erui_talk_install_fn = std::uint32_t (*)(std::uintptr_t at);
std::uint32_t erui_talk_start(erui_talk_log_fn log, erui_talk_before_open_fn before_open,
    erui_talk_install_fn install_text_hook, const std::uint16_t* key, std::size_t key_length);
const std::uint16_t* erui_talk_text(std::int32_t id);
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
void erui_talk_set_back_text(const std::uint16_t* text, std::size_t length);
}

namespace erui::talk {
namespace {

std::atomic_bool g_started{};

void log_sink(std::uint32_t level, const char* text, std::size_t length) {
    const LogLevel mapped = level >= 2 ? LogLevel::error
        : level == 1 ? LogLevel::warning : LogLevel::info;
    host::log::write(mapped, "%.*s", static_cast<int>(length), text);
}

// ---- the message lookup (talk/src/texts.rs)

// MsgRepositoryImp::LookupEntry(repo, bank, category, id) -> text. A chained
// inline hook, put in the first time the menu opens and kept: our ids get our
// texts, every other call goes on to whatever was there (the game, or another
// mod's hook such as Glorious Merchant's).
using LookupEntryFn = const std::uint16_t* (*)(void* repo, std::uint32_t bank,
    std::uint32_t category, std::int32_t id);
SafetyHookInline g_lookup_hook{};
std::atomic<LookupEntryFn> g_lookup_original{};

const std::uint16_t* lookup_detour(void* repo, std::uint32_t bank, std::uint32_t category,
    std::int32_t id) {
    if (const std::uint16_t* ours = erui_talk_text(id)) return ours;
    const LookupEntryFn original = g_lookup_original.load(std::memory_order_acquire);
    return original ? original(repo, bank, category, id) : nullptr;
}

std::uint32_t install_text_hook(std::uintptr_t at) {
    if (g_lookup_hook) return 1;
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(at);
    auto result = SafetyHookInline::create(reinterpret_cast<void*>(at),
        reinterpret_cast<void*>(&lookup_detour), SafetyHookInline::StartDisabled);
    if (!result) {
        host::log::write(LogLevel::error,
            "talk menu: message lookup hook failed (SafetyHook error type %u, first bytes %02X %02X %02X %02X %02X)",
            static_cast<unsigned>(result.error().type), bytes[0], bytes[1], bytes[2], bytes[3], bytes[4]);
        return 0;
    }
    g_lookup_hook = std::move(*result);
    g_lookup_original.store(g_lookup_hook.original<LookupEntryFn>(), std::memory_order_release);
    if (auto enabled = g_lookup_hook.enable(); !enabled) {
        host::log::write(LogLevel::error,
            "talk menu: message lookup hook could not be enabled (SafetyHook error type %u)",
            static_cast<unsigned>(enabled.error().type));
        return 0;
    }
    host::log::write("talk menu: message lookup hook in place (entry was %s)",
        bytes[0] == 0xE9 || (bytes[0] == 0xFF && bytes[1] == 0x25) ? "already hooked by another mod"
                                                                   : "the game's own");
    return 1;
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

std::wstring page_counter(const std::wstring& what, std::uint32_t number, std::uint32_t count) {
    return what + L" (" + std::to_wstring(number) + L"/" + std::to_wstring(count) + L")";
}

void pagination(Bridge& bridge, ERUI_TalkMenu* menu, std::vector<Action>& shown,
    const BridgePage& at, std::uint32_t total, bool before, const Registry::TalkWords& words) {
    const std::uint32_t pages = (total + kPerPage - 1) / kPerPage;
    const std::uint32_t number = at.offset / kPerPage + 1;
    if (before && at.offset > 0) {
        add(menu, shown, page_counter(words.previous_page, number - 1, pages),
            {Act::turn, 0, 0, intern(bridge, at.page, at.values_row, at.offset - kPerPage)});
    }
    if (!before && at.offset + kPerPage < total) {
        add(menu, shown, page_counter(words.next_page, number + 1, pages),
            {Act::turn, 0, 0, intern(bridge, at.page, at.values_row, at.offset + kPerPage)});
    }
}

void build_page(void* user, ERUI_TalkMenu* menu, std::uint32_t id) {
    Bridge& bridge = *static_cast<Bridge*>(user);
    if (id >= bridge.pages.size()) {
        (void)erui_talk_back(menu);
        return;
    }
    const BridgePage at = bridge.pages[id];
    std::vector<Action>& shown = bridge.shown[id];
    shown.clear();
    Registry& registry = host::registry();
    const Registry::TalkWords words = registry.talk_words();

    if (at.values_row != ERUI_INVALID_ROW) {
        std::wstring label;
        std::vector<std::wstring> texts;
        std::uint32_t current = 0;
        if (!registry.talk_values(bridge.provider, at.values_row, label, texts, current)) {
            (void)erui_talk_back(menu);
            return;
        }
        const auto total = static_cast<std::uint32_t>(texts.size());
        pagination(bridge, menu, shown, at, total, true, words);
        for (std::uint32_t i = at.offset; i < total && i < at.offset + kPerPage; ++i) {
            add(menu, shown, i == current ? texts[i] + L"  " + words.current : texts[i],
                {Act::set, at.values_row, i, 0});
        }
        pagination(bridge, menu, shown, at, total, false, words);
        return;
    }

    std::vector<Registry::TalkRowView> rows;
    if (!registry.talk_page(bridge.provider, at.page, rows)) {
        (void)erui_talk_back(menu);
        return;
    }
    const auto total = static_cast<std::uint32_t>(rows.size());
    pagination(bridge, menu, shown, at, total, true, words);
    for (std::uint32_t i = at.offset; i < total && i < at.offset + kPerPage; ++i) {
        const Registry::TalkRowView& row = rows[i];
        std::wstring text = row.label;
        Action action{};
        switch (row.kind) {
        case Registry::TalkRowKind::button:
            action = {Act::press, row.handle, 0, 0};
            break;
        case Registry::TalkRowKind::submenu:
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
    pagination(bridge, menu, shown, at, total, false, words);
}

void choose_row(void* user, ERUI_TalkMenu* menu, std::uint32_t id, std::uint32_t row) {
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

// The callbacks the Rust side calls: no C++ exception may cross into it.
void ERUI_CALL bridge_build(void* user, ERUI_TalkMenu* menu, std::uint32_t id) noexcept {
    try {
        build_page(user, menu, id);
    } catch (...) {
        host::log::write(LogLevel::error, "talk menu: a Mod Settings page could not be built");
        (void)erui_talk_back(menu);
    }
}

void ERUI_CALL bridge_choose(void* user, ERUI_TalkMenu* menu, std::uint32_t id,
    std::uint32_t row) noexcept {
    try {
        choose_row(user, menu, id, row);
    } catch (...) {
        host::log::write(LogLevel::error, "talk menu: a Mod Settings choice failed");
        (void)erui_talk_back(menu);
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
            // tried again at the next open (the bridge was never handed out)
            g_bridged.erase(provider.handle);
            g_bridges.pop_back();
        }
    }
}

void before_open() {
    try {
        sync_providers();
        // the Back row of every page, in the host's language
        const std::wstring back = host::registry().talk_words().back;
        erui_talk_set_back_text(reinterpret_cast<const std::uint16_t*>(back.data()), back.size());
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
    const std::uint32_t result = erui_talk_start(&log_sink, &before_open, &install_text_hook,
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
