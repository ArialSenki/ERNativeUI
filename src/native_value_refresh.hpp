#pragma once

#include <cstdint>

namespace erui::native {

// Fork: repaints visible toggle/slider rows after a client write (for example
// a "restore defaults" button). The native controls cache their value when the
// page is built, so a write through the API used to show only after leaving
// and reopening the page.
//
// Elden Ring 2.7.1.0 only: every entry point is validated by its prologue and
// the feature stays off when any check fails.
void install_native_value_refresh() noexcept;
void reset_native_value_refresh() noexcept;

// Any thread: the bound byte of a row received a programmatic value.
void mark_native_value_dirty(const volatile std::uint8_t* bound) noexcept;

// UI thread (page frame): repaints the live controls of the dirty bytes and
// recolours row labels whose value left (or returned to) its default.
void refresh_dirty_native_values() noexcept;

// Any thread: rows bound to this byte show a coloured label while the byte
// differs from default_index. Inverted sliders (higher is worse) show red
// above the default and green below it.
void set_native_value_default(const volatile std::uint8_t* bound,
    std::uint8_t default_index, bool inverted = false) noexcept;

} // namespace erui::native
