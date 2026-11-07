// Fork (r157): the shared talk menu (F11 by default). See talk_menu.hpp.

#include "talk_menu.hpp"

#include "host_registry.hpp"
#include "logger.hpp"

#include "ernativeui/erui_fork_talk.h"

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <iterator>

// The Rust side (talk/src/lib.rs).
extern "C" {
using erui_talk_log_fn = void (*)(std::uint32_t level, const char* text, std::size_t length);
using erui_talk_start_job_fn = std::uint32_t (*)(
    std::uintptr_t start_job, std::uintptr_t function_vtable, std::uintptr_t factory);
std::uint32_t erui_talk_start(erui_talk_log_fn log, erui_talk_start_job_fn start_job,
    const std::uint16_t* key, std::size_t key_length);
std::uint32_t erui_talk_register(const std::uint16_t* name, std::size_t length,
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

using StartMenuJobFn = void* (*)(void* out, const void* function);

// start_job(out, std::function<DLRefPtr<MenuJob>(ComponentStack&)>) with a
// plain function-pointer impl, laid out like the game's own (impl inline,
// the pointer to it in the last slot). The returned handle does not own the
// job (the job manager does), so nothing is released.
std::uint32_t start_menu_job(
    std::uintptr_t start_job, std::uintptr_t function_vtable, std::uintptr_t factory) {
    alignas(16) void* function[8]{};
    function[0] = reinterpret_cast<void*>(function_vtable);
    function[1] = reinterpret_cast<void*>(factory);
    function[7] = &function[0];
    void* handle[2]{};
    unsigned long code = 0;
    __try {
        reinterpret_cast<StartMenuJobFn>(start_job)(handle, function);
    } __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
    }
    if (code != 0) {
        host::log::write(LogLevel::error,
            "talk menu: starting the Configuration job faulted (%08lX)", code);
        return 0;
    }
    return 1;
}

// ---- the test provider ([TalkMenu] TestProvider=1 in ERNativeUI.ini)

std::uint32_t g_test_counter{};
bool g_test_toggle{};

void add(ERUI_TalkMenu* menu, std::uint32_t row, const wchar_t* text) {
    const ERUI_Utf16View view{reinterpret_cast<const std::uint16_t*>(text),
        static_cast<std::uint32_t>(std::wcslen(text)), 0u};
    (void)erui_talk_add_row(menu, row, view.data, view.length);
}

void ERUI_CALL test_build(void*, ERUI_TalkMenu* menu, std::uint32_t page) {
    wchar_t text[96]{};
    if (page == 0) {
        std::swprintf(text, 96, L"Counter: %u (choose to add one)", g_test_counter);
        add(menu, 1, text);
        add(menu, 2, g_test_toggle ? L"Toggle: on" : L"Toggle: off");
        add(menu, 3, L"A long list...");
        add(menu, 4, L"Close the menu");
        return;
    }
    for (std::uint32_t row = 1; row <= 30; ++row) {
        std::swprintf(text, 96, L"Row %u (goes back)", row);
        add(menu, row, text);
    }
}

void ERUI_CALL test_choose(void*, ERUI_TalkMenu* menu, std::uint32_t page, std::uint32_t row) {
    host::log::write("talk menu test provider: page %u row %u", page, row);
    if (page != 0) {
        (void)erui_talk_back(menu);
        return;
    }
    switch (row) {
    case 1: ++g_test_counter; break;
    case 2: g_test_toggle = !g_test_toggle; break;
    case 3: (void)erui_talk_open_page(menu, 1); break;
    case 4: (void)erui_talk_close(menu); break;
    default: break;
    }
}

bool passive() noexcept {
    return host::api_state() == host::ApiState::failed;
}

} // namespace

void start(bool test_provider, const wchar_t* key) noexcept {
    if (g_started.exchange(true)) return;
    if (test_provider) {
        static constexpr wchar_t kName[] = L"ERNativeUI test";
        std::uint64_t entry = 0;
        const std::uint32_t result = erui_talk_register(
            reinterpret_cast<const std::uint16_t*>(kName), std::size(kName) - 1,
            &test_build, &test_choose, nullptr, &entry);
        host::log::write("talk menu: test provider row %s (%u)",
            result == ERUI_OK ? "added" : "failed", static_cast<unsigned>(result));
    }
    const std::uint32_t result = erui_talk_start(&log_sink, &start_menu_job,
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
