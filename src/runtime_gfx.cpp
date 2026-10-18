#include "runtime_gfx.hpp"

#include "hooks.hpp"
#include "hud_movie.hpp"
#include "logger.hpp"
#include "module.hpp"
#include "movie_cache.hpp"
#include "runtime_gfx_locate.hpp"
#include "settings_movie.hpp"

#include <MinHook.h>
#include <ernativeui/erui.h>
#include <Windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
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
std::atomic<unsigned> g_menu_background_alpha{100};
std::atomic<bool> g_hud_screen_edges{false};
std::atomic<bool> g_menu_edges{false};
std::atomic<unsigned> g_hud_scale_percent{100};
// Layout (layout_key) in force when the first HUD/menu movie was opened; the
// game opens them all once at startup, so later changes need a restart.
constexpr std::uint32_t kLayoutUnset = 0xFFFFFFFFu;
std::atomic<std::uint32_t> g_layout_used{kLayoutUnset};

std::uint32_t layout_key() noexcept {
    return (g_hud_screen_edges.load(std::memory_order_relaxed) ? 1u : 0u) |
        (g_menu_edges.load(std::memory_order_relaxed) ? 2u : 0u) |
        (g_hud_scale_percent.load(std::memory_order_relaxed) << 8);
}
// The game's back-buffer size (width, height): the same globals the movie
// player passes to Scaleform's SetViewport (2.7.1.0 RVA 0x3B43CF8).
const std::uint32_t* g_screen_size{};

// Served buffers live for the whole process (see movie_cache.hpp); no limit.
MovieCache g_cache{};

struct MovieStatus {
    std::atomic<unsigned> opened{};
    std::atomic<unsigned> patched{};
    std::atomic<unsigned> failed{};
    std::atomic<bool> warned{};
};
MovieStatus g_status[2]{};
MovieStatus g_hud_status[64]{};  // by index in layout_movies()

MovieStatus& status_for(SettingsMovie movie) noexcept {
    return g_status[movie == SettingsMovie::game_options ? 0 : 1];
}

MovieStatus& status_for(erui::gfx::HudMovie movie) noexcept {
    const auto index = static_cast<std::size_t>(movie - erui::gfx::layout_movies().data());
    return g_hud_status[index < std::size(g_hud_status) ? index : std::size(g_hud_status) - 1];
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
        if (const MovieCache::Lookup cached = g_cache.find(hash, size, 0); cached.found) return cached.output;

        erui::gfx::SettingsMovieOptions options{};
        options.menu_background_alpha_percent = static_cast<std::uint8_t>(
            g_menu_background_alpha.load(std::memory_order_relaxed));
        const erui::gfx::PatchResult result = erui::gfx::patch_settings_movie(
            movie, std::span<const std::uint8_t>(input, size), options);
        if (result.success() && options.menu_background_alpha_percent < 100) {
            log::write(
                result.report.menu_background_skipped
                    ? "WARN: Runtime GFX: %s has no MENU_BaseMain_01 backdrop; MenuBackgroundAlpha = %u ignored"
                    : "Runtime GFX: %s backdrop alpha %u%%",
                erui::gfx::settings_movie_name(movie),
                static_cast<unsigned>(options.menu_background_alpha_percent));
        }
        const bool patched = result.success() && !result.report.already_satisfied;
        if (result.success()) {
            log::write(
                "Runtime GFX: %s %s (%zu -> %zu bytes)",
                erui::gfx::settings_movie_name(movie),
                result.report.already_satisfied ? "already patched" : "patched in memory",
                size,
                patched ? result.output.size() : size);
        } else {
            log::write(
                "WARN: Runtime GFX: %s could not be patched [%s]: %s; the game uses it unchanged",
                erui::gfx::settings_movie_name(movie),
                erui::gfx::error_name(result.error),
                result.message.c_str());
            status_for(movie).failed.fetch_add(1, std::memory_order_acq_rel);
        }
        const std::vector<std::uint8_t>* output = g_cache.insert(hash, size, 0,
            patched ? result.output : std::vector<std::uint8_t>{}, patched);
        if (!result.success()) report_failure_if_ready();
        return output;
    } catch (...) {
        log::write("WARN: Runtime GFX: exception while patching; movie left unchanged");
        return nullptr;
    }
}

erui::gfx::HudLayout current_hud_layout(erui::gfx::HudMovie movie) noexcept {
    // Menus have their own switch and are never scaled.
    const bool menu = movie->group == erui::gfx::LayoutGroup::menu;
    erui::gfx::HudLayout layout{};
    if (!menu) {
        layout.scale = static_cast<float>(g_hud_scale_percent.load(std::memory_order_relaxed)) / 100.0f;
    }
    if ((menu ? g_menu_edges : g_hud_screen_edges).load(std::memory_order_relaxed)) {
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        if (g_screen_size) {
            width = g_screen_size[0];
            height = g_screen_size[1];
        }
        if (width == 0 || height == 0) {
            width = static_cast<std::uint32_t>(GetSystemMetrics(SM_CXSCREEN));
            height = static_cast<std::uint32_t>(GetSystemMetrics(SM_CYSCREEN));
        }
        layout.edge_offset = erui::gfx::hud_edge_offset_for(width, height);
        log::write("Runtime GFX: HUD screen %ux%u (%s), edge offset %.1f",
            width, height, g_screen_size ? "game" : "monitor", layout.edge_offset);
    }
    return layout;
}

// Same caching rules as patched_bytes; the layout is part of the key because
// the HUD is reloaded with a new layout after a settings change.
const std::vector<std::uint8_t>* hud_patched_bytes(
    erui::gfx::HudMovie movie,
    const erui::gfx::HudLayout& layout,
    const std::uint8_t* input,
    std::size_t size) noexcept {
    try {
        const std::uint64_t variant = 1ull << 63 |
            static_cast<std::uint64_t>(static_cast<std::uint32_t>(layout.edge_offset * 20.0f)) << 16 |
            static_cast<std::uint64_t>(layout.scale * 100.0f + 0.5f);
        const std::uint64_t hash = fnv1a(input, size);
        if (const MovieCache::Lookup cached = g_cache.find(hash, size, variant); cached.found) return cached.output;

        const erui::gfx::PatchResult result = erui::gfx::patch_hud_movie(
            movie, std::span<const std::uint8_t>(input, size), layout);
        const bool patched = result.success() && !result.report.already_satisfied;
        if (result.success()) {
            log::write("Runtime GFX: %s HUD layout (offset %.1f, scale %.2f): %s",
                erui::gfx::hud_movie_name(movie), layout.edge_offset, layout.scale, result.message.c_str());
        } else {
            log::write("WARN: Runtime GFX: %s HUD layout not applied [%s]: %s; the game uses it unchanged",
                erui::gfx::hud_movie_name(movie), erui::gfx::error_name(result.error), result.message.c_str());
            status_for(movie).failed.fetch_add(1, std::memory_order_acq_rel);
        }
        return g_cache.insert(hash, size, variant,
            patched ? result.output : std::vector<std::uint8_t>{}, patched);
    } catch (...) {
        log::write("WARN: Runtime GFX: exception while patching the HUD; movie left unchanged");
        return nullptr;
    }
}

using PatchBytes = std::function<const std::vector<std::uint8_t>*(const std::uint8_t*, std::size_t)>;

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
    const char* name,
    MovieStatus& status,
    const PatchBytes& patch,
    const char* url,
    void* file) noexcept {
    status.opened.fetch_add(1, std::memory_order_acq_rel);

    if (vtable_of(file) == g_memory_file_vtable) {
        if (!read_field<bool>(file, kValidOffset)) return file;
        const auto* buffer = read_field<const std::uint8_t*>(file, kBufferOffset);
        const auto size = read_field<std::int32_t>(file, kSizeOffset);
        if (!buffer || size <= 0 || size > kMaxMovieBytes) return file;
        const std::vector<std::uint8_t>* output =
            patch(buffer, static_cast<std::size_t>(size));
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
            name, total, length);
    }
    const std::vector<std::uint8_t>* output =
        total == length ? patch(input.data(), input.size()) : nullptr;

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
            input.resize(static_cast<std::size_t>(total));
            const std::vector<std::uint8_t>* kept = g_cache.keep_alive(std::move(input));
            wrapper->buffer = kept->data();
            wrapper->size = static_cast<std::int32_t>(kept->size());
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

void* route_settings_movie(SettingsMovie movie, const char* url, void* file) noexcept {
    try {
        return replace_with_patched(erui::gfx::settings_movie_name(movie), status_for(movie),
            [movie, url](const std::uint8_t* input, std::size_t size) {
                const std::vector<std::uint8_t>* settings = patched_bytes(movie, input, size);
                // 02_040/02_042 also have an edge layout: it goes on top of the
                // settings patch (cached by the settings output, which is stable).
                const erui::gfx::HudMovie layout_movie = erui::gfx::classify_hud_movie(url);
                if (!layout_movie) return settings;
                const erui::gfx::HudLayout layout = current_hud_layout(layout_movie);
                if (layout.identity()) return settings;
                const std::vector<std::uint8_t>* edges = settings
                    ? hud_patched_bytes(layout_movie, layout, settings->data(), settings->size())
                    : hud_patched_bytes(layout_movie, layout, input, size);
                return edges ? edges : settings;
            },
            url, file);
    } catch (...) {
        return file;
    }
}

void* route_hud_movie(erui::gfx::HudMovie movie, const char* url, void* file) noexcept {
    try {
        std::uint32_t unset = kLayoutUnset;
        g_layout_used.compare_exchange_strong(unset, layout_key(), std::memory_order_acq_rel);
        const erui::gfx::HudLayout layout = current_hud_layout(movie);
        if (layout.identity()) {
            status_for(movie).opened.fetch_add(1, std::memory_order_acq_rel);
            return file;
        }
        return replace_with_patched(erui::gfx::hud_movie_name(movie), status_for(movie),
            [movie, layout](const std::uint8_t* input, std::size_t size) {
                return hud_patched_bytes(movie, layout, input, size);
            },
            url, file);
    } catch (...) {
        return file;
    }
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
    const erui::gfx::HudMovie hud = movie == SettingsMovie::none
        ? erui::gfx::classify_hud_movie(url) : nullptr;
    if (movie == SettingsMovie::none && !hud) return file;
    log::write("Runtime GFX: opening %s (%s)", url,
        vtable_of(file) == g_memory_file_vtable ? "MemoryFile" : "other File type");
    __try {
        return movie != SettingsMovie::none
            ? route_settings_movie(movie, url, file)
            : route_hud_movie(hud, url, file);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return file;
    }
}

// The movie player's constructor builds a full-screen Scaleform viewport from
// the back-buffer size globals (2.7.1.0 RVA 0xD71BDE; unique in 2.7.1.0 and
// 2.7.1.1): mov eax,[width]; mov edx,[height]; then the Viewport fields.
void locate_screen_size(const erui::native::ModuleView& game) noexcept {
    try {
        constexpr std::string_view pattern =
            "8B 05 ?? ?? ?? ?? 8B 15 ?? ?? ?? ?? 89 45 ?? 89 55 ?? "
            "4C 89 65 ?? 89 45 ?? 89 55 ??";
        const erui::native::ScanResult scan = game.scan_executable(pattern);
        if (!scan.address || scan.matches != 1) {
            log::write("WARN: Runtime GFX: screen size not found (%zu matches); the HUD uses the monitor size",
                scan.matches);
            return;
        }
        auto* width = reinterpret_cast<const std::uint32_t*>(
            erui::native::resolve_rip_relative(game, scan.address, 2, 6));
        auto* height = reinterpret_cast<const std::uint32_t*>(
            erui::native::resolve_rip_relative(game, scan.address + 6, 2, 6));
        if (!width || height != width + 1 || !game.contains(width, 8)) {
            log::write("WARN: Runtime GFX: unexpected screen size layout; the HUD uses the monitor size");
            return;
        }
        g_screen_size = width;
        log::write("Runtime GFX: screen size at RVA 0x%llX",
            static_cast<unsigned long long>(reinterpret_cast<const std::uint8_t*>(width) - game.base()));
    } catch (...) {
    }
}

} // namespace

void set_hud_layout(bool screen_edges, unsigned scale_percent, bool menu_edges) noexcept {
    if (scale_percent < 50) scale_percent = 50;
    if (scale_percent > 150) scale_percent = 150;
    const bool edges_changed =
        g_hud_screen_edges.exchange(screen_edges, std::memory_order_relaxed) != screen_edges;
    const bool scale_changed =
        g_hud_scale_percent.exchange(scale_percent, std::memory_order_relaxed) != scale_percent;
    const bool pause_changed =
        g_menu_edges.exchange(menu_edges, std::memory_order_relaxed) != menu_edges;
    if (edges_changed || scale_changed || pause_changed) {
        log::write("Runtime GFX: HUD layout set (screen edges %s, scale %u%%, menu edges %s); applies when the movies reload",
            screen_edges ? "on" : "off", scale_percent, menu_edges ? "on" : "off");
        // The game opens every movie once at startup: only a layout that differs
        // from the one those movies were opened with needs a restart.
        const std::uint32_t used = g_layout_used.load(std::memory_order_acquire);
        unsigned opened = 0;
        for (const MovieStatus& status : g_hud_status) opened += status.opened.load(std::memory_order_acquire);
        if (used != kLayoutUnset && used != layout_key() && opened) {
            log::write("WARN: Runtime GFX: %u HUD/menu movies were already loaded with the previous layout; restart the game to apply",
                opened);
        }
    }
}

bool install_runtime_gfx(bool trace_urls, unsigned menu_background_alpha_percent) noexcept {
    g_menu_background_alpha.store(
        menu_background_alpha_percent > 100 ? 100u : menu_background_alpha_percent,
        std::memory_order_relaxed);
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
        locate_screen_size(game);
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
    for (const erui::gfx::LayoutMovie& entry : erui::gfx::layout_movies()) {
        const erui::gfx::HudMovie movie = &entry;
        const MovieStatus& status = status_for(movie);
        if (!status.opened.load()) continue;
        log::write("Runtime GFX status: %s opened=%u patched=%u failed=%u",
            erui::gfx::hud_movie_name(movie),
            status.opened.load(), status.patched.load(), status.failed.load());
    }
    try {
        return warning_text(spanish);
    } catch (...) {
        return {};
    }
}

} // namespace erui::host

// Fork extension (not part of the official ERUI_Api table): HUD layout for
// the in-memory HUD movie patch. flags bit 0 moves the edge-anchored HUD
// blocks to the screen edges on screens wider than 16:9, bit 1 does the same
// for the menus with a layout table (never scaled); scale_percent
// (50-150) resizes them around their corner. Applies the next time the game
// loads the HUD. Callable at any time, also before the API is ready.
extern "C" ERUI_EXPORT ERUI_Result ERUI_CALL ERNativeUIFork_SetHudLayout(
    std::uint32_t flags,
    std::uint32_t scale_percent) {
    if (scale_percent < 50u || scale_percent > 150u || (flags & ~3u) != 0) {
        return ERUI_INVALID_ARGUMENT;
    }
    erui::host::set_hud_layout((flags & 1u) != 0, scale_percent, (flags & 2u) != 0);
    return ERUI_OK;
}
