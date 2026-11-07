#pragma once

// Fork (r157): the shared talk menu (F11 by default). The dialog itself is the
// Rust static library in talk/ (erui_talk); this side starts it, gives it the
// log, shows the Mod Settings pages in it, and exports ERNativeUIFork_Talk*
// (include/ernativeui/erui_fork_talk.h, docs/fork/api.md).

namespace erui::talk {

// Starts the menu once. key is [TalkMenu] Key ("F11", "Ctrl+Home"...;
// empty = F11). Every provider's Mod Settings page gets a row.
void start(const wchar_t* key) noexcept;

} // namespace erui::talk
