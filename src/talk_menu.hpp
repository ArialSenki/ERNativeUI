#pragma once

// Fork (r157): the shared talk menu (F11 by default). The dialog itself is the
// Rust static library in talk\ (erui_talk); this side starts it, gives it the
// log and a fault-guarded menu-job call, and exports ERNativeUIFork_Talk*
// (include/ernativeui/erui_fork_talk.h, docs/fork/api.md).

namespace erui::talk {

// Starts the menu once. test_provider adds an "ERNativeUI test" row; key
// is [TalkMenu] Key ("F11", "Ctrl+Home"...; empty = F11).
void start(bool test_provider, const wchar_t* key) noexcept;

} // namespace erui::talk
