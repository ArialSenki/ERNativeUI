#include "runtime_log.hpp"

#include <atomic>
#include <cstdarg>
#include <cstdio>

namespace erui::detail {
namespace {

std::atomic<LogSink> g_sink{nullptr};
std::atomic_bool g_diagnostics_enabled{false};

} // namespace

void set_log_sink(LogSink sink) noexcept {
    g_sink.store(sink, std::memory_order_release);
}

void set_diagnostic_logging(bool enabled) noexcept {
    g_diagnostics_enabled.store(enabled, std::memory_order_release);
}

bool diagnostic_logging_enabled() noexcept {
    return g_diagnostics_enabled.load(std::memory_order_acquire);
}

void logf(LogLevel level, const char* format, ...) noexcept {
    if (!should_emit_log_level(level, diagnostic_logging_enabled())) {
        return;
    }
    LogSink sink = g_sink.load(std::memory_order_acquire);
    if (!sink || !format) {
        return;
    }

    // r146: no 2 KB zero-fill per call; the formatter NUL-terminates, and the
    // first byte covers a formatter that writes nothing.
    char buffer[2048];
    buffer[0] = '\0';
    va_list args;
    va_start(args, format);
#if defined(_MSC_VER)
    ::_vsnprintf_s(buffer, sizeof(buffer), _TRUNCATE, format, args);
#else
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    buffer[sizeof(buffer) - 1] = '\0';
#endif
    va_end(args);
    sink(level, buffer);
}

} // namespace erui::detail
