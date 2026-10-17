#include "runtime_gfx.hpp"

#include "hooks.hpp"
#include "logger.hpp"
#include "module.hpp"
#include "runtime_gfx_locate.hpp"
#include "settings_movie.hpp"

#include <MinHook.h>
#include <Windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace erui::host {
namespace {

using erui::gfx::SettingsMovie;

// Scaleform::File virtual slots (validated against the game's MemoryFile):
// 0 deleting destructor, 1 GetFilePath, 2 IsValid, 4 Tell, 6 GetLength,
// 10 Read. RefCountImpl keeps the 32-bit reference count at +0x08.
constexpr std::size_t kSlotDestructor = 0;
constexpr std::size_t kSlotGetFilePath = 1;
constexpr std::size_t kSlotIsValid = 2;
constexpr std::size_t kSlotTell = 4;
constexpr std::size_t kSlotGetLength = 6;
constexpr std::size_t kSlotRead = 10;
constexpr std::size_t kMemoryFileSlots = 20;
constexpr std::size_t kRefCountOffset = 0x08;
// Scaleform::MemoryFile fields.
constexpr std::size_t kBufferOffset = 0x18;
constexpr std::size_t kSizeOffset = 0x20;
constexpr std::size_t kIndexOffset = 0x24;
constexpr std::size_t kValidOffset = 0x28;
constexpr std::int32_t kMaxMovieBytes = 16 * 1024 * 1024;

using OpenFileFn = void*(__fastcall*)(void* opener, const char* url, int flags, int mode);
using DestructorFn = void*(__fastcall*)(void* file, unsigned flags);
using GetLengthFn = int(__fastcall*)(void* file);
using ReadFn = int(__fastcall*)(void* file, std::uint8_t* buffer, int bytes);
using IsValidFn = bool(__fastcall*)(void* file);

void* g_open_file_target{};
std::atomic<OpenFileFn> g_original_open_file{};
void** g_memory_file_vtable{};
std::array<void*, kMemoryFileSlots> g_wrapper_vtable{};
std::atomic<bool> g_installed{false};
std::atomic<bool> g_ready{false};
std::atomic<bool> g_spanish{false};
std::atomic<bool> g_trace_urls{false};
std::atomic<unsigned> g_traced_urls{0};

// Patched buffers live for the whole process: MemoryFile does not own its
// buffer and the game may keep a MovieDef that still points into it.
struct CachedMovie {
    std::uint64_t input_hash{};
    std::size_t input_size{};
    std::vector<std::uint8_t> output{};
    bool patched{};
};
SRWLOCK g_cache_lock = SRWLOCK_INIT;
std::vector<std::unique_ptr<CachedMovie>> g_cache{};
constexpr std::size_t kMaxCachedMovies = 16;

struct MovieStatus {
    std::atomic<unsigned> opened{};
    std::atomic<unsigned> patched{};
    std::atomic<unsigned> failed{};
    std::atomic<bool> warned{};
};
MovieStatus g_status[2]{};

MovieStatus& status_for(SettingsMovie movie) noexcept {
    return g_status[movie == SettingsMovie::game_options ? 0 : 1];
}

std::uint64_t fnv1a(const std::uint8_t* data, std::size_t size) noexcept {
    std::uint64_t hash = 0xCBF29CE484222325ull;
    for (std::size_t index = 0; index < size; ++index) {
        hash ^= data[index];
        hash *= 0x100000001B3ull;
    }
    return hash;
}

template <class T>
T read_field(const void* object, std::size_t offset) noexcept {
    T value{};
    std::memcpy(&value, static_cast<const std::uint8_t*>(object) + offset, sizeof(T));
    return value;
}

template <class T>
void write_field(void* object, std::size_t offset, T value) noexcept {
    std::memcpy(static_cast<std::uint8_t*>(object) + offset, &value, sizeof(T));
}

void** vtable_of(void* object) noexcept {
    return *static_cast<void***>(object);
}

std::wstring warning_text(bool spanish) {
    std::wstring problems{};
    const SettingsMovie movies[] = {
        SettingsMovie::game_options, SettingsMovie::advanced_settings};
    for (SettingsMovie movie : movies) {
        MovieStatus& status = status_for(movie);
        if (!status.failed.load(std::memory_order_acquire)) continue;
        if (status.warned.exchange(true, std::memory_order_acq_rel)) continue;
        problems += L"\n- ";
        const char* name = erui::gfx::settings_movie_name(movie);
        problems += std::wstring(name, name + std::strlen(name));
    }
    if (problems.empty()) return {};
    return (spanish
        ? L"NativeTweaks no pudo adaptar este archivo del menú (otro mod lo cambia demasiado):"
        : L"NativeTweaks could not adapt this menu file (another mod changes it too much):") +
        problems +
        (spanish
            ? L"\nLa pestaña o algunos controles pueden faltar. Revisa ERNativeUI.log."
            : L"\nThe tab or some controls may be missing. See ERNativeUI.log.");
}

void report_failure_if_ready() noexcept {
    if (!g_ready.load(std::memory_order_acquire)) return;
    try {
        erui::native::set_pending_menu_warning(
            warning_text(g_spanish.load(std::memory_order_relaxed)));
    } catch (...) {
    }
}

// Returns the bytes Scaleform should see, or nullptr to keep the original.
const std::vector<std::uint8_t>* patched_bytes(
    SettingsMovie movie,
    const std::uint8_t* input,
    std::size_t size) noexcept {
    try {
        const std::uint64_t hash = fnv1a(input, size);
        AcquireSRWLockShared(&g_cache_lock);
        for (const auto& entry : g_cache) {
            if (entry->input_hash == hash && entry->input_size == size) {
                const std::vector<std::uint8_t>* result =
                    entry->patched ? &entry->output : nullptr;
                ReleaseSRWLockShared(&g_cache_lock);
                return result;
            }
        }
        ReleaseSRWLockShared(&g_cache_lock);

        const erui::gfx::PatchResult result = erui::gfx::patch_settings_movie(
            movie, std::span<const std::uint8_t>(input, size));
        auto entry = std::make_unique<CachedMovie>();
        entry->input_hash = hash;
        entry->input_size = size;
        if (result.success()) {
            entry->patched = !result.report.already_satisfied;
            if (entry->patched) entry->output = result.output;
            log::write(
                "Runtime GFX: %s %s (%zu -> %zu bytes)",
                erui::gfx::settings_movie_name(movie),
                result.report.already_satisfied ? "already patched" : "patched in memory",
                size,
                entry->patched ? entry->output.size() : size);
        } else {
            log::write(
                "WARN: Runtime GFX: %s could not be patched [%s]: %s; the game uses it unchanged",
                erui::gfx::settings_movie_name(movie),
                erui::gfx::error_name(result.error),
                result.message.c_str());
            status_for(movie).failed.fetch_add(1, std::memory_order_acq_rel);
        }
        const std::vector<std::uint8_t>* output =
            entry->patched ? &entry->output : nullptr;
        AcquireSRWLockExclusive(&g_cache_lock);
        if (g_cache.size() < kMaxCachedMovies) {
            g_cache.push_back(std::move(entry));
        } else {
            output = nullptr;  // never hand out a buffer we cannot keep alive
            log::write("WARN: Runtime GFX: cache full; movie left unchanged");
        }
        ReleaseSRWLockExclusive(&g_cache_lock);
        if (!result.success()) report_failure_if_ready();
        return output;
    } catch (...) {
        log::write("WARN: Runtime GFX: exception while patching; movie left unchanged");
        return nullptr;
    }
}

// Wrapper file used when the opener returns something other than a
// MemoryFile. It reuses the game's MemoryFile implementation for every slot
// except the destructor and GetFilePath.
struct WrapperFile {
    void** vtable;                       // +0x00
    volatile long reference_count;       // +0x08
    std::uint32_t padding0;              // +0x0C
    std::uintptr_t native_path;          // +0x10 (unused; GetFilePath is ours)
    const std::uint8_t* buffer;          // +0x18
    std::int32_t size;                   // +0x20
    std::int32_t index;                  // +0x24
    bool valid;                          // +0x28
    std::uint8_t padding1[7];
    std::string path;                    // our own storage
};
static_assert(offsetof(WrapperFile, reference_count) == kRefCountOffset);
static_assert(offsetof(WrapperFile, buffer) == kBufferOffset);
static_assert(offsetof(WrapperFile, size) == kSizeOffset);
static_assert(offsetof(WrapperFile, index) == kIndexOffset);
static_assert(offsetof(WrapperFile, valid) == kValidOffset);

void* __fastcall wrapper_destructor(void* file, unsigned flags) noexcept {
    auto* wrapper = static_cast<WrapperFile*>(file);
    if (flags & 1u) {
        delete wrapper;
    } else {
        wrapper->path.~basic_string();
    }
    return file;
}

const char* __fastcall wrapper_path(void* file) noexcept {
    return static_cast<WrapperFile*>(file)->path.c_str();
}

void release_native_file(void* file) noexcept {
    auto* count = reinterpret_cast<volatile long*>(
        static_cast<std::uint8_t*>(file) + kRefCountOffset);
    if (InterlockedDecrement(count) == 0) {
        reinterpret_cast<DestructorFn>(vtable_of(file)[kSlotDestructor])(file, 1u);
    }
}

void* replace_with_patched(
    SettingsMovie movie,
    const char* url,
    void* file) noexcept {
    MovieStatus& status = status_for(movie);
    status.opened.fetch_add(1, std::memory_order_acq_rel);

    if (vtable_of(file) == g_memory_file_vtable) {
        if (!read_field<bool>(file, kValidOffset)) return file;
        const auto* buffer = read_field<const std::uint8_t*>(file, kBufferOffset);
        const auto size = read_field<std::int32_t>(file, kSizeOffset);
        if (!buffer || size <= 0 || size > kMaxMovieBytes) return file;
        const std::vector<std::uint8_t>* output =
            patched_bytes(movie, buffer, static_cast<std::size_t>(size));
        if (!output) return file;
        write_field(file, kBufferOffset, output->data());
        write_field(file, kSizeOffset, static_cast<std::int32_t>(output->size()));
        write_field(file, kIndexOffset, std::int32_t{0});
        status.patched.fetch_add(1, std::memory_order_acq_rel);
        return file;
    }

    // Generic Scaleform::File: read it through its own virtual interface.
    void** vtable = vtable_of(file);
    if (!reinterpret_cast<IsValidFn>(vtable[kSlotIsValid])(file)) return file;
    const int length = reinterpret_cast<GetLengthFn>(vtable[kSlotGetLength])(file);
    if (length <= 0 || length > kMaxMovieBytes) return file;
    std::vector<std::uint8_t> input(static_cast<std::size_t>(length));
    int total = 0;
    while (total < length) {
        const int read = reinterpret_cast<ReadFn>(vtable[kSlotRead])(
            file, input.data() + total, length - total);
        if (read <= 0) break;
        total += read;
    }
    if (total != length) {
        // Partially consumed: the original can no longer be handed back.
        log::write(
            "WARN: Runtime GFX: %s short read (%d of %d bytes); serving it unchanged",
            erui::gfx::settings_movie_name(movie), total, length);
    }
    const std::vector<std::uint8_t>* output =
        total == length ? patched_bytes(movie, input.data(), input.size()) : nullptr;

    WrapperFile* wrapper = nullptr;
    try {
        wrapper = new WrapperFile{};
        wrapper->vtable = g_wrapper_vtable.data();
        wrapper->reference_count = 1;
        wrapper->path = url ? url : "";
        if (output) {
            wrapper->buffer = output->data();
            wrapper->size = static_cast<std::int32_t>(output->size());
        } else {
            // Keep the original bytes alive with the same lifetime rule.
            auto entry = std::make_unique<CachedMovie>();
            entry->output = std::move(input);
            entry->output.resize(static_cast<std::size_t>(total));
            wrapper->buffer = entry->output.data();
            wrapper->size = static_cast<std::int32_t>(entry->output.size());
            AcquireSRWLockExclusive(&g_cache_lock);
            g_cache.push_back(std::move(entry));
            ReleaseSRWLockExclusive(&g_cache_lock);
        }
        wrapper->index = 0;
        wrapper->valid = wrapper->size > 0;
    } catch (...) {
        delete wrapper;
        return file;  // the original may be consumed, but this is the best fallback
    }
    release_native_file(file);
    if (output) status.patched.fetch_add(1, std::memory_order_acq_rel);
    return wrapper;
}

void* __fastcall open_file_detour(void* opener, const char* url, int flags, int mode) noexcept {
    const OpenFileFn original = g_original_open_file.load(std::memory_order_acquire);
    void* file = original(opener, url, flags, mode);
    if (!file || !url) return file;
    if (g_trace_urls.load(std::memory_order_relaxed) &&
        g_traced_urls.fetch_add(1, std::memory_order_relaxed) < 400) {
        log::write("Runtime GFX trace: OpenFile %s", url);
    }
    const SettingsMovie movie = erui::gfx::classify_settings_movie(url);
    if (movie == SettingsMovie::none) return file;
    log::write("Runtime GFX: opening %s (%s)", url,
        vtable_of(file) == g_memory_file_vtable ? "MemoryFile" : "other File type");
    __try {
        return replace_with_patched(movie, url, file);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return file;
    }
}

} // namespace

bool install_runtime_gfx(bool trace_urls) noexcept {
    g_trace_urls.store(trace_urls, std::memory_order_relaxed);
    if (g_installed.load(std::memory_order_acquire)) return true;
    try {
        erui::native::ModuleView game{};
        if (!game.initialize(GetModuleHandleW(nullptr))) {
            log::write("WARN: Runtime GFX: game image unavailable");
            return false;
        }
        RuntimeGfxTargets targets{};
        if (!locate_runtime_gfx_targets(game, targets)) {
            log::write("WARN: Runtime GFX: %s; not hooking",
                targets.failure ? targets.failure : "lookup failed");
            return false;
        }
        void** memory_file = targets.memory_file_vtable;
        for (std::size_t slot = 0; slot < kMemoryFileSlots; ++slot) {
            g_wrapper_vtable[slot] = memory_file[slot];
        }
        g_wrapper_vtable[kSlotDestructor] = reinterpret_cast<void*>(&wrapper_destructor);
        g_wrapper_vtable[kSlotGetFilePath] = reinterpret_cast<void*>(&wrapper_path);
        g_memory_file_vtable = memory_file;
        void* target = targets.open_file;
        const MH_STATUS init = MH_Initialize();
        if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
            log::write("WARN: Runtime GFX: MinHook init failed (%d)", static_cast<int>(init));
            return false;
        }
        void* trampoline = nullptr;
        MH_STATUS status = MH_CreateHook(target, reinterpret_cast<void*>(&open_file_detour), &trampoline);
        if (status != MH_OK) {
            log::write("WARN: Runtime GFX: hook creation failed (%d)", static_cast<int>(status));
            return false;
        }
        g_original_open_file.store(reinterpret_cast<OpenFileFn>(trampoline), std::memory_order_release);
        status = MH_EnableHook(target);
        if (status != MH_OK) {
            MH_RemoveHook(target);
            log::write("WARN: Runtime GFX: hook enable failed (%d)", static_cast<int>(status));
            return false;
        }
        g_open_file_target = target;
        g_installed.store(true, std::memory_order_release);
        log::write(
            "Runtime GFX: settings movies are patched in memory (OpenFile RVA 0x%llX, MemoryFile vtable RVA 0x%llX)",
            static_cast<unsigned long long>(static_cast<std::uint8_t*>(target) - game.base()),
            static_cast<unsigned long long>(
                reinterpret_cast<std::uint8_t*>(memory_file) - game.base()));
        return true;
    } catch (...) {
        log::write("WARN: Runtime GFX: installation exception");
        return false;
    }
}

bool runtime_gfx_installed() noexcept {
    return g_installed.load(std::memory_order_acquire);
}

std::wstring runtime_gfx_ready(bool spanish) noexcept {
    g_spanish.store(spanish, std::memory_order_relaxed);
    g_ready.store(true, std::memory_order_release);
    for (SettingsMovie movie : {SettingsMovie::game_options, SettingsMovie::advanced_settings}) {
        const MovieStatus& status = status_for(movie);
        log::write("Runtime GFX status: %s opened=%u patched=%u failed=%u",
            erui::gfx::settings_movie_name(movie),
            status.opened.load(), status.patched.load(), status.failed.load());
    }
    try {
        return warning_text(spanish);
    } catch (...) {
        return {};
    }
}

} // namespace erui::host
