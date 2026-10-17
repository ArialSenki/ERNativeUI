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

} // namespace erui::native
