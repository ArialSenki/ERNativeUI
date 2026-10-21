#include "runtime_log.hpp"

#include <atomic>
#include <cstdarg>
#include <cstdio>

namespace erui::detail {
namespace {

std::atomic<LogSink> g_sink{nullptr};
std::atomic<bool> g_enabled{true};
std::atomic<bool> g_trace{true};

} // namespace

void set_log_sink(LogSink sink) noexcept {
    g_sink.store(sink, std::memory_order_release);
}

void set_log_filter(bool enabled, bool trace) noexcept {
    g_enabled.store(enabled, std::memory_order_relaxed);
    g_trace.store(trace, std::memory_order_relaxed);
}

void logf(LogLevel level, const char* format, ...) noexcept {
    if (!g_enabled.load(std::memory_order_relaxed) ||
        (level == LogLevel::trace && !g_trace.load(std::memory_order_relaxed))) {
        return;
    }
    LogSink sink = g_sink.load(std::memory_order_acquire);
    if (!sink || !format) {
        return;
    }

    char buffer[2048]{};
    va_list args;
    va_start(args, format);
#if defined(_MSC_VER)
    ::_vsnprintf_s(buffer, sizeof(buffer), _TRUNCATE, format, args);
#else
    std::vsnprintf(buffer, sizeof(buffer), format, args);
#endif
    va_end(args);
    sink(level, buffer);
}

} // namespace erui::detail
