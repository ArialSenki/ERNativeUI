#pragma once

#include "log_level.hpp"

#include <filesystem>

namespace erui::host::log {

bool initialize(const std::filesystem::path& path) noexcept;
void set_diagnostics_enabled(bool enabled) noexcept;
// Ordinary lines go through the background writer from here on; before this
// call they are written synchronously.
void enable_buffering() noexcept;
// While on (and the log is open), hardware faults on any thread are logged
// before the process may end. Turned on at startup, off once the host is ready.
void set_startup_fault_logging(bool enabled) noexcept;
void shutdown() noexcept;

#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 1, 2)))
#endif
void write(const char* format, ...) noexcept;

#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 2, 3)))
#endif
void write(LogLevel level, const char* format, ...) noexcept;

} // namespace erui::host::log
