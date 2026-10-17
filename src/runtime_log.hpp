#pragma once

#include "runtime.hpp"

namespace erui::detail {

void set_log_sink(LogSink sink) noexcept;

// Fork: drops messages before they are formatted. Hot paths (text resolver,
// Scaleform bridges) log at TRACE, so formatting them only to discard them
// costs time on the game thread. Both default to true.
void set_log_filter(bool enabled, bool trace) noexcept;

#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 2, 3)))
#endif
void logf(LogLevel level, const char* format, ...) noexcept;

} // namespace erui::detail
