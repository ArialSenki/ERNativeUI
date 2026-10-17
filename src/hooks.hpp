#pragma once

#include "addresses.hpp"

#include <cstdint>
#include <string>

namespace erui::native {

enum class HookInstallStatus : std::uint8_t {
    success,
    hook_failed,
};

HookInstallStatus install_native_menu_hooks(const GameAddresses& addresses) noexcept;
void remove_native_menu_hooks() noexcept;

// Fork: a warning shown once as a native alert the first time Game Options
// opens (startup checks run before any menu can display it).
void set_pending_menu_warning(std::wstring message) noexcept;

// Fork: [Diagnostics] PerfLog = 1 counts calls and time spent in the hooks
// that run on every UI text (log_hook_perf writes and resets the totals).
void set_hook_perf_enabled(bool enabled) noexcept;
void log_hook_perf(std::uint64_t interval_ms) noexcept;

} // namespace erui::native
