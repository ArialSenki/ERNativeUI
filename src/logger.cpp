#include "logger.hpp"

#include <Windows.h>

#include <cstdarg>
#include <atomic>
#include <cstdio>
#include <cstring>

namespace erui::host::log {
namespace {

HANDLE g_file = INVALID_HANDLE_VALUE;
SRWLOCK g_lock = SRWLOCK_INIT;
std::atomic_bool g_enabled{};
std::atomic_bool g_diagnostics_enabled{};

// Fork: ordinary lines are appended to a buffer and written by a background
// thread, so a game thread that logs never waits on the disk (or on MO2's
// virtual file system). ERROR/FATAL lines first drain the buffer and are then
// written and flushed synchronously, as before, because they may precede a
// crash. A full buffer drops lines and the writer reports how many.
// Until the host reports itself ready every line is written synchronously, as
// upstream does: a crash during startup must not leave the log empty.
constexpr std::size_t kPendingBytes = 1U << 20;
constexpr std::size_t kWakeBytes = 64U * 1024U;
constexpr DWORD kWriterIntervalMs = 250;
SRWLOCK g_pending_lock = SRWLOCK_INIT;
char g_pending[kPendingBytes];
std::size_t g_pending_size{};
std::size_t g_dropped_lines{};
char g_writing[kPendingBytes];
HANDLE g_writer_wake{};
std::atomic_bool g_writer_running{};
std::atomic_bool g_buffering{};

// Writes whatever is pending. Callers serialize through g_lock.
void drain_pending_locked() noexcept {
    AcquireSRWLockExclusive(&g_pending_lock);
    const std::size_t size = g_pending_size;
    const std::size_t dropped = g_dropped_lines;
    if (size != 0) std::memcpy(g_writing, g_pending, size);
    g_pending_size = 0;
    g_dropped_lines = 0;
    ReleaseSRWLockExclusive(&g_pending_lock);
    if (g_file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    if (size != 0) {
        WriteFile(g_file, g_writing, static_cast<DWORD>(size), &written, nullptr);
    }
    if (dropped != 0) {
        char note[96]{};
        const int length = ::_snprintf_s(note, sizeof(note), _TRUNCATE,
            "[ERNativeUI/WARN] %zu log line(s) dropped: log buffer full\r\n", dropped);
        if (length > 0) {
            WriteFile(g_file, note, static_cast<DWORD>(length), &written, nullptr);
        }
    }
}

DWORD WINAPI writer_thread(void*) noexcept {
    while (g_writer_running.load(std::memory_order_acquire)) {
        WaitForSingleObject(g_writer_wake, kWriterIntervalMs);
        AcquireSRWLockExclusive(&g_lock);
        drain_pending_locked();
        ReleaseSRWLockExclusive(&g_lock);
    }
    return 0;
}

void emit(LogLevel level, const char* text, DWORD length) noexcept {
    const bool urgent = erui::detail::should_flush_log_level(level);
    if (urgent || IsDebuggerPresent()) OutputDebugStringA(text);

    if (!urgent && g_buffering.load(std::memory_order_acquire) &&
        g_writer_running.load(std::memory_order_acquire)) {
        bool wake = false;
        AcquireSRWLockExclusive(&g_pending_lock);
        if (g_pending_size + length <= kPendingBytes) {
            std::memcpy(g_pending + g_pending_size, text, length);
            g_pending_size += length;
            wake = g_pending_size >= kWakeBytes;
        } else {
            ++g_dropped_lines;
            wake = true;
        }
        ReleaseSRWLockExclusive(&g_pending_lock);
        if (wake) SetEvent(g_writer_wake);
        return;
    }

    AcquireSRWLockExclusive(&g_lock);
    drain_pending_locked();
    if (g_file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(g_file, text, length, &written, nullptr);
        if (urgent) {
            FlushFileBuffers(g_file);
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
}

void write_v(LogLevel level, const char* format, va_list arguments) noexcept {
    if (!g_enabled.load(std::memory_order_acquire) || !format ||
        !erui::detail::should_emit_log_level(
            level,
            g_diagnostics_enabled.load(std::memory_order_acquire))) {
        return;
    }

    char message[1792]{};
    const int message_length = vsnprintf_s(
        message,
        sizeof(message),
        _TRUNCATE,
        format,
        arguments);

    SYSTEMTIME now{};
    GetLocalTime(&now);

    char line[2048]{};
    const int line_length = ::_snprintf_s(
        line,
        sizeof(line),
        _TRUNCATE,
        "[%04u-%02u-%02u %02u:%02u:%02u.%03u] [T%lu] [ERNativeUI/%s] %s\r\n",
        static_cast<unsigned>(now.wYear),
        static_cast<unsigned>(now.wMonth),
        static_cast<unsigned>(now.wDay),
        static_cast<unsigned>(now.wHour),
        static_cast<unsigned>(now.wMinute),
        static_cast<unsigned>(now.wSecond),
        static_cast<unsigned>(now.wMilliseconds),
        static_cast<unsigned long>(GetCurrentThreadId()),
        erui::detail::log_level_name(level),
        message_length < 0 ? "<formatting error>" : message);

    if (line_length <= 0) {
        static constexpr char fallback[] =
            "[ERNativeUI/ERROR] logging failure\r\n";
        emit(
            LogLevel::error,
            fallback,
            static_cast<DWORD>(sizeof(fallback) - 1));
        return;
    }

    emit(level, line, static_cast<DWORD>(line_length));
}

} // namespace

bool initialize(const std::filesystem::path& path) noexcept {
    AcquireSRWLockExclusive(&g_lock);
    if (g_file != INVALID_HANDLE_VALUE) {
        CloseHandle(g_file);
        g_file = INVALID_HANDLE_VALUE;
    }

    g_file = CreateFileW(
        path.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    const bool ok = g_file != INVALID_HANDLE_VALUE;
    g_buffering.store(false, std::memory_order_release);
    if (ok && !g_writer_running.load(std::memory_order_acquire)) {
        if (!g_writer_wake) {
            g_writer_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        }
        if (g_writer_wake) {
            g_writer_running.store(true, std::memory_order_release);
            if (HANDLE thread = CreateThread(nullptr, 0, &writer_thread, nullptr, 0, nullptr)) {
                CloseHandle(thread);
            } else {
                g_writer_running.store(false, std::memory_order_release);
            }
        }
    }
    g_enabled.store(ok, std::memory_order_release);
    ReleaseSRWLockExclusive(&g_lock);
    return ok;
}

void enable_buffering() noexcept {
    g_buffering.store(true, std::memory_order_release);
}

void set_diagnostics_enabled(bool enabled) noexcept {
    g_diagnostics_enabled.store(enabled, std::memory_order_release);
}

void shutdown() noexcept {
    g_enabled.store(false, std::memory_order_release);
    g_writer_running.store(false, std::memory_order_release);
    // During process exit another thread may have died holding the lock.
    if (!TryAcquireSRWLockExclusive(&g_lock)) return;
    drain_pending_locked();
    if (g_file != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(g_file);
        CloseHandle(g_file);
        g_file = INVALID_HANDLE_VALUE;
    }
    ReleaseSRWLockExclusive(&g_lock);
}

void write(const char* format, ...) noexcept {
    va_list arguments;
    va_start(arguments, format);
    write_v(LogLevel::info, format, arguments);
    va_end(arguments);
}

void write(LogLevel level, const char* format, ...) noexcept {
    va_list arguments;
    va_start(arguments, format);
    write_v(level, format, arguments);
    va_end(arguments);
}

} // namespace erui::host::log
