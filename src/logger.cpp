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
// r147: wait = false (process exit) gives up if a dead thread holds the lock.
void drain_pending_locked(bool wait = true) noexcept {
    if (wait) {
        AcquireSRWLockExclusive(&g_pending_lock);
    } else if (!TryAcquireSRWLockExclusive(&g_pending_lock)) {
        return;
    }
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

// r146: flush = false skips FlushFileBuffers for an urgent line; WriteFile
// still hands it to the system cache, which outlives a crashing process.
void emit(LogLevel level, const char* text, DWORD length, bool flush = true) noexcept {
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
        if (urgent && flush) {
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

    SYSTEMTIME now{};
    GetLocalTime(&now);

    // r146: the prefix, the message and the line end are formatted straight
    // into one buffer: no zero-fill and no intermediate message copy on the
    // game thread. The line format is unchanged.
    constexpr char kLineEnd[] = "\r\n";
    char line[2048];
    const int prefix_length = ::_snprintf_s(
        line,
        sizeof(line),
        _TRUNCATE,
        "[%04u-%02u-%02u %02u:%02u:%02u.%03u] [T%lu] [ERNativeUI/%s] ",
        static_cast<unsigned>(now.wYear),
        static_cast<unsigned>(now.wMonth),
        static_cast<unsigned>(now.wDay),
        static_cast<unsigned>(now.wHour),
        static_cast<unsigned>(now.wMinute),
        static_cast<unsigned>(now.wSecond),
        static_cast<unsigned>(now.wMilliseconds),
        static_cast<unsigned long>(GetCurrentThreadId()),
        erui::detail::log_level_name(level));

    if (prefix_length <= 0 ||
        static_cast<std::size_t>(prefix_length) + sizeof(kLineEnd) >= sizeof(line)) {
        static constexpr char fallback[] =
            "[ERNativeUI/ERROR] logging failure\r\n";
        emit(
            LogLevel::error,
            fallback,
            static_cast<DWORD>(sizeof(fallback) - 1));
        return;
    }

    std::size_t length = static_cast<std::size_t>(prefix_length);
    // Leaves room for the line end; sizeof(kLineEnd) includes the NUL slot.
    const std::size_t room = sizeof(line) - length - (sizeof(kLineEnd) - 1);
    const int message_length = vsnprintf_s(
        line + length,
        room,
        _TRUNCATE,
        format,
        arguments);
    if (message_length >= 0) {
        length += static_cast<std::size_t>(message_length);
    } else {
        // -1 is also returned for a message cut at the buffer size; only an
        // empty result is a formatting error.
        const std::size_t written = strnlen(line + length, room);
        if (written != 0) {
            length += written;
        } else {
            static constexpr char error[] = "<formatting error>";
            std::memcpy(line + length, error, sizeof(error) - 1);
            length += sizeof(error) - 1;
        }
    }
    std::memcpy(line + length, kLineEnd, sizeof(kLineEnd) - 1);
    length += sizeof(kLineEnd) - 1;

    emit(level, line, static_cast<DWORD>(length));
}

// Fork: while the host starts, hardware faults on any thread are written to
// the log (synchronously) before the process may die, with the module and
// offset of the faulting code and a few unwound frames. Every handler call
// returns EXCEPTION_CONTINUE_SEARCH, so guarded reads and the game's own
// handlers behave as before. Linux/Proton startup crash, r140.
constexpr long kMaximumLoggedFaults = 32;
PVOID g_fault_handler{};
std::atomic_long g_logged_faults{};
thread_local bool t_in_fault_handler{};

bool fault_worth_logging(DWORD code) noexcept {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
        return true;
    // r146: breakpoints are left out (debuggers, int3 probes, anti-tamper
    // checks the game handles itself), as are C++ throws (0xE06D7363),
    // DBG_PRINTEXCEPTION and thread naming (0x406D1388).
    default:
        return false;
    }
}

// "module+0xOFFSET" for an address, or the bare address outside any module.
void describe_address(DWORD64 address, char* text, std::size_t size) noexcept {
    HMODULE module{};
    // r148: wide path converted to UTF-8; the ANSI API turned non-ASCII
    // module names (a mod folder in another script) into '?'.
    wchar_t path[MAX_PATH]{};
    if (GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(address), &module) &&
        GetModuleFileNameW(module, path, MAX_PATH) != 0) {
        const wchar_t* name = path;
        for (const wchar_t* at = path; *at != L'\0'; ++at) {
            if (*at == L'\\' || *at == L'/') name = at + 1;
        }
        char name_utf8[MAX_PATH * 3]{};
        if (WideCharToMultiByte(CP_UTF8, 0, name, -1, name_utf8,
                static_cast<int>(sizeof(name_utf8)), nullptr, nullptr) == 0) {
            (void)::_snprintf_s(name_utf8, sizeof(name_utf8), _TRUNCATE, "?");
        }
        (void)::_snprintf_s(text, size, _TRUNCATE, "%s+0x%llX", name_utf8,
            static_cast<unsigned long long>(
                address - reinterpret_cast<DWORD64>(module)));
        return;
    }
    (void)::_snprintf_s(text, size, _TRUNCATE, "0x%llX",
        static_cast<unsigned long long>(address));
}

// One step of the walk below. r146: guarded, so a damaged stack pointer or
// unwind data ends the walk instead of faulting inside the handler.
bool unwind_one_frame(CONTEXT& context) noexcept {
#if defined(_MSC_VER)
    __try {
#endif
        DWORD64 image_base{};
        PRUNTIME_FUNCTION function =
            RtlLookupFunctionEntry(context.Rip, &image_base, nullptr);
        if (function) {
            void* handler_data{};
            DWORD64 establisher{};
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip,
                function, &context, &handler_data, &establisher, nullptr);
        } else {
            // A leaf function: its return address is at the stack pointer.
            context.Rip = *reinterpret_cast<const DWORD64*>(context.Rsp);
            context.Rsp += sizeof(DWORD64);
        }
        return true;
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#endif
}

LONG CALLBACK startup_fault_handler(EXCEPTION_POINTERS* pointers) noexcept {
    if (!pointers || !pointers->ExceptionRecord || !pointers->ContextRecord ||
        t_in_fault_handler) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const EXCEPTION_RECORD& record = *pointers->ExceptionRecord;
    if (!fault_worth_logging(record.ExceptionCode) ||
        g_logged_faults.fetch_add(1, std::memory_order_relaxed) >=
            kMaximumLoggedFaults) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    t_in_fault_handler = true;
    char where[MAX_PATH + 32]{};
    describe_address(reinterpret_cast<DWORD64>(record.ExceptionAddress),
        where, sizeof(where));
    char access[96]{};
    if (record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
        record.NumberParameters >= 2) {
        const ULONG_PTR kind = record.ExceptionInformation[0];
        (void)::_snprintf_s(access, sizeof(access), _TRUNCATE, " %s 0x%llX",
            kind == 0 ? "reading" : kind == 1 ? "writing" :
                kind == 8 ? "executing" : "accessing",
            static_cast<unsigned long long>(record.ExceptionInformation[1]));
    }
    // Up to eight callers, unwound from a copy of the faulting context.
    char frames[640]{};
    std::size_t used = 0;
    CONTEXT context = *pointers->ContextRecord;
    const int max_depth =
        record.ExceptionCode == EXCEPTION_STACK_OVERFLOW ? 0 : 8;
    for (int depth = 0; depth < max_depth && context.Rip != 0; ++depth) {
        if (!unwind_one_frame(context) || context.Rip == 0) break;
        char caller[MAX_PATH + 32]{};
        describe_address(context.Rip, caller, sizeof(caller));
        const int written = ::_snprintf_s(frames + used, sizeof(frames) - used,
            _TRUNCATE, " < %s", caller);
        if (written <= 0) break;
        used += static_cast<std::size_t>(written);
    }
    char line[1400]{};
    const int length = ::_snprintf_s(line, sizeof(line), _TRUNCATE,
        "[ERNativeUI/ERROR] Startup fault on T%lu: code=0x%08lX at %s%s%s\r\n",
        static_cast<unsigned long>(GetCurrentThreadId()),
        static_cast<unsigned long>(record.ExceptionCode), where, access, frames);
    // r146: written synchronously but not flushed per fault; a guarded probe
    // that faults repeatedly must not cost a disk flush each time.
    if (length > 0) emit(LogLevel::error, line, static_cast<DWORD>(length), false);
    t_in_fault_handler = false;
    return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

void set_startup_fault_logging(bool enabled) noexcept {
    if (enabled && !g_fault_handler && g_enabled.load(std::memory_order_acquire)) {
        g_fault_handler = AddVectoredExceptionHandler(1, &startup_fault_handler);
    } else if (!enabled && g_fault_handler) {
        RemoveVectoredExceptionHandler(g_fault_handler);
        g_fault_handler = nullptr;
    }
}

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
    drain_pending_locked(false);
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
