#include "native_value_refresh.hpp"

#include "running_build.hpp"
#include "runtime_log.hpp"

#include <Windows.h>
#include <safetyhook.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <string_view>

namespace erui::native {
namespace {

// Per-build addresses (MenuObjectAddressProfile and ColorPickerAddressProfile),
// set by install_native_value_refresh. Item constructors are 0x978 (toggle)
// and 0xE50 (slider) bytes; list select is (list, index, flag), list repaint
// (list), toggle index-of (item, const char* value), slider refresh is
// SliderCtrl slot 13.
std::uintptr_t g_toggle_item_ctor_rva{};
std::uintptr_t g_slider_item_ctor_rva{};
std::uintptr_t g_list_select_rva{};
std::uintptr_t g_list_repaint_rva{};
std::uintptr_t g_toggle_index_of_rva{};
std::uintptr_t g_slider_refresh_rva{};
std::uintptr_t g_toggle_item_vtable_rva{};
std::uintptr_t g_slider_item_vtable_rva{};

constexpr std::size_t kToggleListOffset = 0xF0;
constexpr std::size_t kToggleBoundOffset = 0x8B8;
constexpr std::size_t kSliderCtrlOffset = 0xB0;
constexpr std::size_t kSliderBoundOffset = 0xE40;
constexpr std::size_t kSliderCurrentOffset = 0xD28;  // inside SliderCtrl

struct Prologue {
    const std::uintptr_t* rva;
    std::array<std::uint8_t, 12> bytes;
};
constexpr Prologue kPrologues[]{
    {&g_toggle_item_ctor_rva, {0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41}},
    {&g_slider_item_ctor_rva, {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x48, 0x08, 0x56, 0x57, 0x41, 0x56, 0x48}},
    {&g_list_select_rva, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89}},
    {&g_list_repaint_rva, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x60, 0x48, 0x83, 0xB9, 0x18, 0x07, 0x00}},
    {&g_toggle_index_of_rva, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89}},
};

// Label colouring (same Scaleform calls as the colour picker preview):
// path resolver (source, dest, path) -> dest, value exists (proxy), colour
// transform (value, 0xAABBGGRR), proxy destructor (proxy + 0x28).
std::uintptr_t g_path_resolver_rva{};
std::uintptr_t g_value_exists_rva{};
std::uintptr_t g_color_transform_rva{};
std::uintptr_t g_proxy_destructor_rva{};
struct ShortPrologue {
    const std::uintptr_t* rva;
    std::array<std::uint8_t, 8> bytes;
};
constexpr ShortPrologue kColorPrologues[]{
    {&g_path_resolver_rva, {0x4C, 0x89, 0x44, 0x24, 0x18, 0x4C, 0x89, 0x4C}},
    {&g_value_exists_rva, {0x48, 0x83, 0xEC, 0x28, 0x48, 0x8B, 0x01, 0xFF}},
    {&g_color_transform_rva, {0x48, 0x89, 0x5C, 0x24, 0x20, 0x57, 0x48, 0x81}},
    {&g_proxy_destructor_rva, {0x48, 0x89, 0x4C, 0x24, 0x08, 0x53, 0x48, 0x83}},
};
constexpr std::size_t kProxyStorageSize = 0x60;
constexpr std::size_t kProxyValueOffset = 0x28;
constexpr std::size_t kSliderMovieOffset = 0xA38;       // inside SliderCtrl
constexpr std::size_t kToggleSourceOffsets[]{0x10, kToggleListOffset + 0x148};
constexpr const char* kLabelPath = "Caption";

using PathResolverFn = void*(__fastcall*)(void*, void*, const char*);
using ValueExistsFn = char(__fastcall*)(void*);
using ProxyDestructorFn = void(__fastcall*)(void*);

using ItemCtorFn =void*(__fastcall*)(void*, void*, void*, void*, void*, void*, void*, void*,
    void*, void*, void*, void*);
using ListSelectFn = char(__fastcall*)(void*, int, std::uint8_t);
using ListRepaintFn = void(__fastcall*)(void*);
using ToggleIndexOfFn = int(__fastcall*)(void*, const volatile std::uint8_t*);
using SliderRefreshFn = std::uint64_t(__fastcall*)(void*);

enum class Kind : std::uint8_t { none, toggle, slider };

struct Slot {
    std::atomic<std::uint8_t*> item{};
    std::atomic<const volatile std::uint8_t*> bound{};
    std::atomic<Kind> kind{Kind::none};
    std::atomic<int> colored{-1};  // -1 unknown, 0 default colour, 1 changed colour
    // Index into g_defaults, or -1. Looked up when the control is recorded
    // and again only after a new default is added (default_generation), so
    // the per-frame colour pass never searches the default table.
    int default_index{-1};
    std::uint32_t default_generation{};
    // Caption colour transform before the first tint (game thread only).
    std::array<float, 8> native{};
    bool has_native{};
};

struct DefaultEntry {
    std::atomic<const volatile std::uint8_t*> bound{};
    std::atomic<std::uint8_t> value{};
    std::atomic_bool inverted{};  // higher is worse: above default shows red
};
constexpr std::size_t kDefaultCount = 256;
std::array<DefaultEntry, kDefaultCount> g_defaults{};
std::atomic<std::size_t> g_default_total{};
// Bumped whenever an entry is added to g_defaults.
std::atomic<std::uint32_t> g_default_generation{};
std::atomic_bool g_color_ready{};
std::atomic_bool g_highlight_enabled{true};
std::atomic<int> g_toggle_source{-1};  // index into kToggleSourceOffsets that resolved
std::atomic<std::uint32_t> g_color_logs{};

int default_index_of(const volatile std::uint8_t* bound) noexcept {
    const std::size_t total = std::min(g_default_total.load(std::memory_order_acquire), kDefaultCount);
    for (std::size_t i = 0; i < total; ++i) {
        if (g_defaults[i].bound.load(std::memory_order_acquire) == bound) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

constexpr std::size_t kSlotCount = 512;
std::array<Slot, kSlotCount> g_slots{};
std::atomic<std::size_t> g_next_slot{};

constexpr std::size_t kDirtyCount = 128;
std::array<std::atomic<const volatile std::uint8_t*>, kDirtyCount> g_dirty{};
std::atomic_bool g_any_dirty{};

std::uint8_t* g_base{};
SafetyHookInline g_toggle_ctor_hook{};
SafetyHookInline g_slider_ctor_hook{};
std::atomic<ItemCtorFn> g_original_toggle_ctor{};
std::atomic<ItemCtorFn> g_original_slider_ctor{};
std::atomic_bool g_active{};
std::atomic<std::uint32_t> g_refresh_logs{};

void remember(std::uint8_t* item, std::size_t bound_offset, Kind kind) noexcept {
    if (!item) return;
    const volatile std::uint8_t* bound{};
#if defined(_MSC_VER)
    __try {
#endif
        bound = *reinterpret_cast<const volatile std::uint8_t* const*>(item + bound_offset);
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
#endif
    if (!bound) return;
    // A rebuilt page creates a new control for the same byte; the old one
    // belongs to a destroyed page and must never be touched again.
    for (Slot& old : g_slots) {
        if (old.bound.load(std::memory_order_relaxed) == bound) {
            old.kind.store(Kind::none, std::memory_order_release);
        }
    }
    Slot& slot = g_slots[g_next_slot.fetch_add(1, std::memory_order_relaxed) % kSlotCount];
    slot.kind.store(Kind::none, std::memory_order_release);
    slot.item.store(item, std::memory_order_relaxed);
    slot.bound.store(bound, std::memory_order_relaxed);
    slot.colored.store(-1, std::memory_order_relaxed);
    slot.has_native = false;
    slot.default_generation = g_default_generation.load(std::memory_order_acquire);
    slot.default_index = default_index_of(bound);
    slot.kind.store(kind, std::memory_order_release);
}

void* __fastcall toggle_ctor_detour(void* a1, void* a2, void* a3, void* a4, void* a5, void* a6,
    void* a7, void* a8, void* a9, void* a10, void* a11, void* a12) noexcept {
    const ItemCtorFn original = g_original_toggle_ctor.load(std::memory_order_acquire);
    void* const result = original ? original(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12) : nullptr;
    remember(static_cast<std::uint8_t*>(a1), kToggleBoundOffset, Kind::toggle);
    return result;
}

void* __fastcall slider_ctor_detour(void* a1, void* a2, void* a3, void* a4, void* a5, void* a6,
    void* a7, void* a8, void* a9, void* a10, void* a11, void* a12) noexcept {
    const ItemCtorFn original = g_original_slider_ctor.load(std::memory_order_acquire);
    void* const result = original ? original(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12) : nullptr;
    remember(static_cast<std::uint8_t*>(a1), kSliderBoundOffset, Kind::slider);
    return result;
}

// Returns false when the item no longer looks like the control recorded for
// this byte (destroyed page, reused memory).
bool refresh_item(std::uint8_t* item, Kind kind, const volatile std::uint8_t* bound,
    unsigned long& seh) noexcept {
    seh = 0;
#if defined(_MSC_VER)
    __try {
#endif
        const auto vtable = *reinterpret_cast<std::uint8_t* const*>(item);
        if (kind == Kind::toggle) {
            if (vtable != g_base + g_toggle_item_vtable_rva ||
                *reinterpret_cast<const volatile std::uint8_t* const*>(item + kToggleBoundOffset) != bound) {
                return false;
            }
            const int index = reinterpret_cast<ToggleIndexOfFn>(g_base + g_toggle_index_of_rva)(item, bound);
            std::uint8_t* const list = item + kToggleListOffset;
            (void)reinterpret_cast<ListSelectFn>(g_base + g_list_select_rva)(list, index, 0);
            reinterpret_cast<ListRepaintFn>(g_base + g_list_repaint_rva)(list);
            return true;
        }
        if (vtable != g_base + g_slider_item_vtable_rva ||
            *reinterpret_cast<const volatile std::uint8_t* const*>(item + kSliderBoundOffset) != bound) {
            return false;
        }
        std::uint8_t* const ctrl = item + kSliderCtrlOffset;
        *reinterpret_cast<std::int32_t*>(ctrl + kSliderCurrentOffset) = *bound;
        // Through the hooked entry so formatted-slider texts are reapplied.
        (void)reinterpret_cast<SliderRefreshFn>(g_base + g_slider_refresh_rva)(ctrl);
        return true;
#if defined(_MSC_VER)
    } __except (seh = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#endif
}

// Reads the bound byte of a live control; false when the control is gone.
bool read_live_value(std::uint8_t* item, Kind kind, const volatile std::uint8_t* bound,
    std::uint8_t& value) noexcept {
#if defined(_MSC_VER)
    __try {
#endif
        const auto vtable = *reinterpret_cast<std::uint8_t* const*>(item);
        const std::size_t offset = kind == Kind::toggle ? kToggleBoundOffset : kSliderBoundOffset;
        const std::uintptr_t expected = kind == Kind::toggle ? g_toggle_item_vtable_rva : g_slider_item_vtable_rva;
        if (vtable != g_base + expected ||
            *reinterpret_cast<const volatile std::uint8_t* const*>(item + offset) != bound) {
            return false;
        }
        value = *bound;
        return true;
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#endif
}

// Display-object colour transform (the non-text branch of 0xD85680): vfunc
// 0x110 reads and 0x118 writes 8 floats on [value+0x18] for [value+0x28].
// Tinting through it leaves the text format alone, and writing back the
// captured transform restores the native look exactly.
constexpr std::size_t kValueInterfaceOffset = 0x18;
constexpr std::size_t kValueObjectOffset = 0x28;
constexpr std::size_t kGetCxformSlot = 0x110 / sizeof(void*);
constexpr std::size_t kSetCxformSlot = 0x118 / sizeof(void*);
// Tints relative to the native caption (#CCCCCA), indexed by Tint.
enum Tint : int { kTintNative = 0, kTintChanged = 1, kTintAbove = 2, kTintBelow = 3 };
constexpr float kTintScale[4][3]{
    {1.0f, 1.0f, 1.0f},
    {1.127f, 0.951f, 0.545f},  // gold #E6C26E (toggles)
    {0.700f, 1.000f, 0.604f},  // soft green #8FCC7A (slider above default)
    {1.098f, 0.600f, 0.545f},  // soft red #E07A6E (slider below default)
};

using CxformFn = void(__fastcall*)(void*, void*, float*);

// Resolves "Caption" under source and applies the native or changed colour
// transform. 1 applied, 0 path missing, -1 fault.
int tint_label_from(void* source, int tint, Slot& slot) noexcept {
    alignas(16) std::array<std::byte, kProxyStorageSize> proxy{};
    bool constructed{};
    int result = 0;
#if defined(_MSC_VER)
    __try {
#endif
        if (reinterpret_cast<PathResolverFn>(g_base + g_path_resolver_rva)(
                source, proxy.data(), kLabelPath) == proxy.data()) {
            constructed = true;
            if (reinterpret_cast<ValueExistsFn>(g_base + g_value_exists_rva)(proxy.data())) {
                auto*** const object = reinterpret_cast<void***>(proxy.data());
                using AccessorFn = void*(__fastcall*)(void*);
                auto* const value = static_cast<std::uint8_t*>((*object && (*object)[1])
                    ? reinterpret_cast<AccessorFn>((*object)[1])(proxy.data()) : nullptr);
                void* const iface = value ? *reinterpret_cast<void**>(value + kValueInterfaceOffset) : nullptr;
                void* const target = value ? *reinterpret_cast<void**>(value + kValueObjectOffset) : nullptr;
                if (iface && target) {
                    void** const vtable = *reinterpret_cast<void***>(iface);
                    if (!slot.has_native) {
                        alignas(16) float captured[8]{};
                        reinterpret_cast<CxformFn>(vtable[kGetCxformSlot])(iface, target, captured);
                        std::memcpy(slot.native.data(), captured, sizeof(captured));
                        slot.has_native = true;
                        if (g_color_logs.fetch_add(1, std::memory_order_relaxed) < 8) {
                            erui::detail::logf(erui::LogLevel::info,
                                "Changed-value colour: native cxform %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f",
                                captured[0], captured[1], captured[2], captured[3],
                                captured[4], captured[5], captured[6], captured[7]);
                        }
                    }
                    alignas(16) float cxform[8]{};
                    std::memcpy(cxform, slot.native.data(), sizeof(cxform));
                    // The multiplier row is the one with a non-zero alpha.
                    const int mult = cxform[3] != 0.0f ? 0 : 4;
                    for (int i = 0; i < 3; ++i) cxform[mult + i] *= kTintScale[tint][i];
                    reinterpret_cast<CxformFn>(vtable[kSetCxformSlot])(iface, target, cxform);
                    result = 1;
                }
            }
        }
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        result = -1;
    }
    if (constructed) {
        __try {
            reinterpret_cast<ProxyDestructorFn>(g_base + g_proxy_destructor_rva)(
                proxy.data() + kProxyValueOffset);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            result = -1;
        }
    }
#endif
    return result;
}

bool tint_label(std::uint8_t* item, Kind kind, int tint, Slot& slot) noexcept {
    if (kind == Kind::slider) {
        return tint_label_from(item + kSliderCtrlOffset + kSliderMovieOffset, tint, slot) == 1;
    }
    const int known = g_toggle_source.load(std::memory_order_acquire);
    if (known >= 0) return tint_label_from(item + kToggleSourceOffsets[known], tint, slot) == 1;
    for (int i = 0; i < static_cast<int>(std::size(kToggleSourceOffsets)); ++i) {
        const int outcome = tint_label_from(item + kToggleSourceOffsets[i], tint, slot);
        if (g_color_logs.fetch_add(1, std::memory_order_relaxed) < 8) {
            erui::detail::logf(erui::LogLevel::info,
                "Changed-value colour: toggle label source +0x%zX -> %d",
                kToggleSourceOffsets[i], outcome);
        }
        if (outcome == 1) {
            g_toggle_source.store(i, std::memory_order_release);
            return true;
        }
    }
    return false;
}

void update_label_colors() noexcept {
    if (!g_color_ready.load(std::memory_order_acquire) ||
        !g_default_total.load(std::memory_order_acquire)) {
        return;
    }
    for (Slot& slot : g_slots) {
        const Kind kind = slot.kind.load(std::memory_order_acquire);
        if (kind == Kind::none) continue;
        const volatile std::uint8_t* const bound = slot.bound.load(std::memory_order_relaxed);
        if (slot.default_index < 0) {
            const std::uint32_t generation = g_default_generation.load(std::memory_order_acquire);
            if (slot.default_generation == generation) continue;
            slot.default_generation = generation;
            slot.default_index = default_index_of(bound);
            if (slot.default_index < 0) continue;
        }
        const DefaultEntry& entry = g_defaults[static_cast<std::size_t>(slot.default_index)];
        const std::uint8_t fallback = entry.value.load(std::memory_order_relaxed);
        const bool inverted = entry.inverted.load(std::memory_order_relaxed);
        std::uint8_t* const item = slot.item.load(std::memory_order_relaxed);
        std::uint8_t value{};
        if (!read_live_value(item, kind, bound, value)) {
            slot.kind.store(Kind::none, std::memory_order_release);
            continue;
        }
        // Slider positions grow with the value, so they show the direction of
        // the change; toggle indices have no order and only show "changed".
        // Inverted rows (higher is worse) swap green and red.
        int tint = kTintNative;
        if (value != fallback && g_highlight_enabled.load(std::memory_order_relaxed)) {
            tint = kind == Kind::slider ? ((value > fallback) != inverted ? kTintAbove : kTintBelow)
                                        : kTintChanged;
        }
        const int previous = slot.colored.load(std::memory_order_relaxed);
        if (previous == tint) continue;
        // A fresh control already shows the default colour.
        if (previous < 0 && tint == kTintNative) {
            slot.colored.store(kTintNative, std::memory_order_relaxed);
            continue;
        }
        // Stored even on failure so a missing label is not retried every frame.
        (void)tint_label(item, kind, tint, slot);
        slot.colored.store(tint, std::memory_order_relaxed);
    }
}

} // namespace

void install_native_value_refresh() noexcept {
    if (g_active.load(std::memory_order_acquire)) return;
    g_base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
    if (!g_base) return;
    const EldenRingBuild build = running_elden_ring_build();
    const std::string_view build_name = elden_ring_build_name(build);
    const MenuObjectAddressProfile* const menu = find_menu_object_address_profile(build);
    if (!menu) {
        erui::detail::logf(erui::LogLevel::warning,
            "Live value refresh unavailable: no address profile for this game build "
            "(menu values only update when a page is reopened)");
        return;
    }
    g_toggle_item_ctor_rva = menu->toggle_item_constructor_rva;
    g_slider_item_ctor_rva = menu->slider_item_constructor_rva;
    g_list_select_rva = menu->list_select_rva;
    g_list_repaint_rva = menu->list_repaint_rva;
    g_toggle_index_of_rva = menu->toggle_index_of_rva;
    g_slider_refresh_rva = menu->slider_refresh_rva;
    g_toggle_item_vtable_rva = menu->toggle_item_vtable_rva;
    g_slider_item_vtable_rva = menu->slider_item_vtable_rva;
    for (const Prologue& p : kPrologues) {
        if (std::memcmp(g_base + *p.rva, p.bytes.data(), p.bytes.size()) != 0) {
            erui::detail::logf(erui::LogLevel::warning,
                "Live value refresh unavailable: entry RVA 0x%llX does not match Elden Ring %.*s",
                static_cast<unsigned long long>(*p.rva),
                static_cast<int>(build_name.size()), build_name.data());
            return;
        }
    }
    auto toggle = SafetyHookInline::create(g_base + g_toggle_item_ctor_rva,
        reinterpret_cast<void*>(&toggle_ctor_detour), SafetyHookInline::StartDisabled);
    auto slider = SafetyHookInline::create(g_base + g_slider_item_ctor_rva,
        reinterpret_cast<void*>(&slider_ctor_detour), SafetyHookInline::StartDisabled);
    if (!toggle || !slider) {
        erui::detail::logf(erui::LogLevel::warning, "Live value refresh hooks could not be prepared");
        return;
    }
    g_toggle_ctor_hook = std::move(*toggle);
    g_slider_ctor_hook = std::move(*slider);
    g_original_toggle_ctor.store(g_toggle_ctor_hook.original<ItemCtorFn>(), std::memory_order_release);
    g_original_slider_ctor.store(g_slider_ctor_hook.original<ItemCtorFn>(), std::memory_order_release);
    if (!g_toggle_ctor_hook.enable() || !g_slider_ctor_hook.enable()) {
        erui::detail::logf(erui::LogLevel::warning, "Live value refresh hooks could not be enabled");
        reset_native_value_refresh();
        return;
    }
    g_active.store(true, std::memory_order_release);
    erui::detail::logf(erui::LogLevel::info, "Live value refresh hooks installed");
    const ColorPickerAddressProfile* const color = find_color_picker_address_profile(build);
    bool color_ok = color != nullptr;
    if (color) {
        g_path_resolver_rva = color->scaleform_path_resolver_rva;
        g_value_exists_rva = color->scaleform_value_exists_rva;
        g_color_transform_rva = color->scaleform_color_transform_setter_rva;
        g_proxy_destructor_rva = color->scene_obj_proxy_destructor_rva;
        for (const ShortPrologue& p : kColorPrologues) {
            color_ok = color_ok && std::memcmp(g_base + *p.rva, p.bytes.data(), p.bytes.size()) == 0;
        }
    }
    g_color_ready.store(color_ok, std::memory_order_release);
    if (color_ok) {
        erui::detail::logf(erui::LogLevel::info, "Changed-value label colour available");
    } else {
        erui::detail::logf(erui::LogLevel::warning,
            "Changed-value label colour unavailable: Scaleform entry points do not match Elden Ring %.*s",
            static_cast<int>(build_name.size()), build_name.data());
    }
}

void reset_native_value_refresh() noexcept {
    g_active.store(false, std::memory_order_release);
    g_toggle_ctor_hook.reset();
    g_slider_ctor_hook.reset();
    g_original_toggle_ctor.store(nullptr, std::memory_order_release);
    g_original_slider_ctor.store(nullptr, std::memory_order_release);
    for (Slot& slot : g_slots) slot.kind.store(Kind::none, std::memory_order_release);
    for (auto& dirty : g_dirty) dirty.store(nullptr, std::memory_order_relaxed);
    g_any_dirty.store(false, std::memory_order_release);
}

void mark_native_value_dirty(const volatile std::uint8_t* bound) noexcept {
    if (!bound || !g_active.load(std::memory_order_acquire)) return;
    for (auto& dirty : g_dirty) {
        const volatile std::uint8_t* expected = nullptr;
        if (dirty.load(std::memory_order_relaxed) == bound) break;
        if (dirty.compare_exchange_strong(expected, bound, std::memory_order_acq_rel)) break;
    }
    g_any_dirty.store(true, std::memory_order_release);
}

void set_native_value_highlight(bool enabled) noexcept {
    g_highlight_enabled.store(enabled, std::memory_order_relaxed);
}

void set_native_value_default(const volatile std::uint8_t* bound,
    std::uint8_t default_index, bool inverted) noexcept {
    if (!bound) return;
    const std::size_t total = std::min(g_default_total.load(std::memory_order_acquire), kDefaultCount);
    for (std::size_t i = 0; i < total; ++i) {
        if (g_defaults[i].bound.load(std::memory_order_acquire) == bound) {
            g_defaults[i].value.store(default_index, std::memory_order_relaxed);
            g_defaults[i].inverted.store(inverted, std::memory_order_relaxed);
            return;
        }
    }
    const std::size_t index = g_default_total.load(std::memory_order_acquire);
    if (index >= kDefaultCount) return;
    g_defaults[index].value.store(default_index, std::memory_order_relaxed);
    g_defaults[index].inverted.store(inverted, std::memory_order_relaxed);
    g_defaults[index].bound.store(bound, std::memory_order_release);
    g_default_total.store(index + 1, std::memory_order_release);
    g_default_generation.fetch_add(1, std::memory_order_acq_rel);
}

void refresh_dirty_native_values() noexcept {
    if (!g_active.load(std::memory_order_acquire)) return;
    update_label_colors();
    if (!g_any_dirty.exchange(false, std::memory_order_acq_rel)) return;
    unsigned refreshed = 0;
    for (auto& dirty : g_dirty) {
        const volatile std::uint8_t* const bound = dirty.exchange(nullptr, std::memory_order_acq_rel);
        if (!bound) continue;
        for (Slot& slot : g_slots) {
            const Kind kind = slot.kind.load(std::memory_order_acquire);
            if (kind == Kind::none || slot.bound.load(std::memory_order_relaxed) != bound) continue;
            unsigned long seh = 0;
            if (refresh_item(slot.item.load(std::memory_order_relaxed), kind, bound, seh)) {
                ++refreshed;
            } else {
                slot.kind.store(Kind::none, std::memory_order_release);
                if (seh && g_refresh_logs.fetch_add(1, std::memory_order_relaxed) < 8) {
                    erui::detail::logf(erui::LogLevel::warning,
                        "Live value refresh faulted (seh=0x%08lX); control skipped", seh);
                }
            }
        }
    }
    if (refreshed && g_refresh_logs.fetch_add(1, std::memory_order_relaxed) < 8) {
        erui::detail::logf(erui::LogLevel::info, "Live value refresh: %u visible control(s) repainted",
            refreshed);
    }
}

} // namespace erui::native
