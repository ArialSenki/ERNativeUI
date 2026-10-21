#include "hooks.hpp"

#include "dialog_input_gate.hpp"
#include "game_build.hpp"
#include "native_dialog.hpp"
#include "native_menu.hpp"
#include "native_popup_choice.hpp"
#include "native_slider_presentation.hpp"
#include "native_text_input.hpp"
#include "native_text_result.hpp"
#include "native_value_refresh.hpp"
#include "native_title_bridge.hpp"
#include "pagination.hpp"
#include "runtime_log.hpp"
#include "runtime_requirements.hpp"
#include "runtime_state.hpp"
#include "root_button_text_override.hpp"
#include "submenu_navigation.hpp"
#include "submenu_runtime.hpp"
#include "color_picker.hpp"
#include "running_build.hpp"

#include <Windows.h>
#include <MinHook.h>
#include <safetyhook.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <type_traits>
#include <utility>

namespace erui::native {
namespace {

// [Diagnostics] PerfLog: per-hook call count and time. "game" is the time
// inside the original function, so total - game is the hook's own cost.
struct HookPerfCounter {
    const char* name;
    std::atomic<std::uint64_t> calls{};
    std::atomic<std::uint64_t> total_ticks{};
    std::atomic<std::uint64_t> game_ticks{};
};

std::atomic<bool> g_hook_perf{false};
HookPerfCounter g_perf_text_setter{"ScaleformTextSetter"};
HookPerfCounter g_perf_scaleform_path{"ScaleformPathObserver"};
HookPerfCounter g_perf_text_resolver{"TextResolver"};

std::uint64_t perf_now() noexcept {
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return static_cast<std::uint64_t>(value.QuadPart);
}

// Times one hook call (scope) or the original function inside it (game).
class HookPerfScope {
public:
    HookPerfScope(HookPerfCounter& counter, bool game = false) noexcept
        : counter_(g_hook_perf.load(std::memory_order_relaxed) ? &counter : nullptr),
          game_(game),
          start_(counter_ ? perf_now() : 0) {}
    ~HookPerfScope() {
        if (!counter_) return;
        const std::uint64_t elapsed = perf_now() - start_;
        if (game_) {
            counter_->game_ticks.fetch_add(elapsed, std::memory_order_relaxed);
            return;
        }
        counter_->calls.fetch_add(1, std::memory_order_relaxed);
        counter_->total_ticks.fetch_add(elapsed, std::memory_order_relaxed);
    }
    HookPerfScope(const HookPerfScope&) = delete;
    HookPerfScope& operator=(const HookPerfScope&) = delete;

private:
    HookPerfCounter* counter_;
    bool game_;
    std::uint64_t start_;
};

constexpr std::uint32_t kCustomTextFallbackId = 0x1B20A;
constexpr std::uint64_t kNativeBackAction = 3;
constexpr std::uint64_t kSubmenuRequestTimeoutMs = 2000;
constexpr std::uint32_t kSubmenuDiagnosticLogLimit = 192;
constexpr std::uint32_t kSubmenuFaultLogLimit = 32;
constexpr std::uint32_t kTitleFaultLogLimit = 4;
constexpr std::uint8_t kModSettingsNativeCategory = 4;
constexpr std::uint32_t kModSettingsNativeTitleId = 0x1AE78;
// Vtables of the running build (MenuObjectAddressProfile); 0 on an
// unsupported build, so every comparison against them fails closed.
std::uintptr_t option_setting_top_dialog_vtable_rva() noexcept {
    const auto* const profile = erui::native::running_menu_object_profile();
    return profile ? profile->option_setting_top_dialog_vtable_rva : 0;
}
constexpr std::size_t kOptionSettingTopCategoryListOffset = 0x1208;
constexpr std::size_t kOptionSettingCategoryCountOffset = 0x558;
constexpr std::size_t kOptionSettingCategoryRecordSize = 0x88;
constexpr std::size_t kOptionSettingCategoryCodeOffset = 0x40;
constexpr std::uint64_t kMaxDiagnosticCategoryCount = 12;
constexpr std::uint32_t kNativePanelDiagnosticLogLimit = 32;
constexpr std::uint32_t kCategoryRouteDiagnosticLogLimit = 128;
constexpr std::uint32_t kCategoryAppendDiagnosticLogLimit = 128;
constexpr std::uint32_t kTabTransitionDiagnosticLogLimit = 256;
constexpr std::uint32_t kNativeRowBuilderDiagnosticLogLimit = 96;
constexpr std::uint32_t kScaleformVisibilityDiagnosticLogLimit = 128;
constexpr std::uint32_t kScaleformTextDiagnosticLogLimit = 256;
constexpr std::size_t kScaleformPathDiagnosticBufferSize = 192;
constexpr std::size_t kScaleformPathDiagnosticSlotsPerContext = 128;
constexpr std::size_t kDiagnosticTextCategoryBucketCount = 11;
constexpr std::size_t kDiagnosticTextIdsPerCategory = 96;
constexpr std::size_t kDiagnosticMenuContextCount = 3;
constexpr std::size_t kDiagnosticCategoryCount = 10;

using DiagnosticTextIdSlots =
    std::array<std::atomic<std::uint32_t>, kDiagnosticTextIdsPerCategory>;
using DiagnosticTextIdCategoryBuckets =
    std::array<DiagnosticTextIdSlots, kDiagnosticTextCategoryBucketCount>;
using DiagnosticTextIdBuckets = std::array<
    DiagnosticTextIdCategoryBuckets,
    kDiagnosticMenuContextCount>;
using DiagnosticScaleformPathSlots =
    std::array<std::atomic<std::uint64_t>,
               kScaleformPathDiagnosticSlotsPerContext>;
using DiagnosticScaleformPathCategories =
    std::array<DiagnosticScaleformPathSlots, 10>;
using DiagnosticScaleformPathContexts =
    std::array<DiagnosticScaleformPathCategories, 3>;

DiagnosticTextIdBuckets g_text_resolver_logged_ids{};
DiagnosticTextIdBuckets g_custom_text_logged_ids{};
DiagnosticScaleformPathContexts g_scaleform_paths_logged{};

std::size_t diagnostic_text_bucket(std::uint32_t category) noexcept {
    // Native categories are sparse but still fit 0..9. Bucket 10 preserves
    // startup requests made before the tab observer has a category.
    return category < 10 ? static_cast<std::size_t>(category) : 10u;
}

bool claim_diagnostic_text_id(
    DiagnosticTextIdBuckets& buckets,
    std::uint32_t context,
    std::uint32_t category,
    std::uint32_t message_id) noexcept {
    if (message_id == 0) return false;
    const std::size_t context_bucket = context < kDiagnosticMenuContextCount
        ? static_cast<std::size_t>(context)
        : 0u;
    auto& slots = buckets[context_bucket][diagnostic_text_bucket(category)];
    for (auto& slot : slots) {
        std::uint32_t existing = slot.load(std::memory_order_relaxed);
        if (existing == message_id) return false;
        if (existing == 0) {
            if (slot.compare_exchange_strong(
                    existing,
                    message_id,
                    std::memory_order_relaxed,
                    std::memory_order_relaxed)) {
                return true;
            }
            if (existing == message_id) return false;
        }
    }
    return false;
}

void reset_diagnostic_text_ids() noexcept {
    for (auto& context : g_text_resolver_logged_ids) {
        for (auto& category : context) {
            for (auto& id : category) id.store(0, std::memory_order_relaxed);
        }
    }
    for (auto& context : g_custom_text_logged_ids) {
        for (auto& category : context) {
            for (auto& id : category) id.store(0, std::memory_order_relaxed);
        }
    }
}

void reset_diagnostic_scaleform_paths() noexcept {
    for (auto& context : g_scaleform_paths_logged) {
        for (auto& category : context) {
            for (auto& path : category) {
                path.store(0, std::memory_order_relaxed);
            }
        }
    }
}

bool claim_diagnostic_scaleform_path(
    std::uint32_t context,
    std::uint32_t category,
    std::uint64_t hash) noexcept {
    if (context >= g_scaleform_paths_logged.size() || category >= 10 ||
        hash == 0) {
        return false;
    }
    auto& slots = g_scaleform_paths_logged[context][category];
    for (auto& slot : slots) {
        std::uint64_t existing = slot.load(std::memory_order_relaxed);
        if (existing == hash) return false;
        if (existing == 0) {
            if (slot.compare_exchange_strong(
                    existing,
                    hash,
                    std::memory_order_relaxed,
                    std::memory_order_relaxed)) {
                return true;
            }
            if (existing == hash) return false;
        }
    }
    return false;
}

using ConfigurationCategoryAppendFn = void(__fastcall*)(void*, void*);

GameAddresses g_addresses{};
SafetyHookInline g_game_options_hook{};
#if defined(ER_NATIVE_UI_CUSTOM_TABS_BACKEND)
struct TabCarrierAllocatorTarget;
struct TabCarrierBuildProfile {
    EldenRingBuild build{};
    std::uint32_t timestamp{};
    std::size_t image_size{};
    std::uintptr_t category8_pre_rva{};
    std::uintptr_t category8_materializer_rva{};
    std::uintptr_t category8_extra_callback_rva{};
    std::uintptr_t composite_teardown_rva{};
    std::uintptr_t option_setting_dialog_vtable_rva{};
    std::uintptr_t scene_obj_proxy_vtable_rva{};
    std::uintptr_t scene_obj_proxy_get_value_rva{};
    std::uintptr_t carrier_manager_rva{};
    std::uintptr_t carrier_deactivate_rva{};
    std::uintptr_t composite_vtable_rva{};
    std::uintptr_t top_vtable_rva{};
    std::uintptr_t extra_callback_anchor_rva{};
    std::uintptr_t row_callback_anchor_rva{};
    std::uintptr_t builder_anchor_rva{};
    std::uintptr_t route_anchor_rva{};
    std::array<std::uint8_t, 16> manager_entry{};
    const TabCarrierAllocatorTarget* allocator_targets{};
    std::size_t allocator_target_count{};
};

SafetyHookMid g_tab_content_category8_pre_hook{};
SafetyHookInline g_tab_content_composite_teardown_hook{};
using TabCarrierCompositeTeardownFn = void(__fastcall*)(void*);
std::atomic<TabCarrierCompositeTeardownFn>
    g_original_tab_content_composite_teardown{};
std::atomic_bool g_tab_content_hooks_exposed{};
std::atomic_bool g_tab_content_callbacks_armed{};
std::atomic_uint64_t g_tab_content_teardown_generation{};
std::atomic_uint32_t g_tab_content_route_log_count{};
std::atomic_uint32_t g_tab_content_route_fault_log_count{};
const TabCarrierBuildProfile* g_tab_carrier_build_profile{};
// The first-page capacity is discovered from each live Graphics carrier. Use
// one row only when reserving pagination bindings: it is the conservative
// worst case and is not a claim about the authored GFX capacity.
constexpr std::uint8_t kTabCarrierMinimumPlanCapacity = 1;
#endif
SafetyHookInline g_camera_panel_materializer_hook{};
SafetyHookInline g_display_panel_materializer_hook{};
SafetyHookInline g_sound_panel_materializer_hook{};
SafetyHookInline g_network_panel_materializer_hook{};
SafetyHookInline g_keyboard_mouse_panel_materializer_hook{};
SafetyHookInline g_graphics_panel_materializer_hook{};
SafetyHookInline g_configuration_category_append_hook{};
SafetyHookMid g_option_setting_tab_frame_hook{};
SafetyHookMid g_mod_settings_panel_route_hook{};
SafetyHookMid g_mod_settings_panel_route_return_hook{};
SafetyHookMid g_add_toggle_diagnostic_hook{};
SafetyHookMid g_add_slider_diagnostic_hook{};
SafetyHookMid g_add_choice_diagnostic_hook{};
SafetyHookMid g_add_button_diagnostic_hook{};
SafetyHookMid g_add_unclassified_row_diagnostic_hook{};
SafetyHookMid g_scaleform_visibility_diagnostic_hook{};
SafetyHookMid g_scaleform_text_diagnostic_hook{};
std::atomic<BuiltinPanelMaterializerFn> g_original_camera_panel_materializer{};
std::atomic<BuiltinPanelMaterializerFn> g_original_display_panel_materializer{};
std::atomic<BuiltinPanelMaterializerFn> g_original_sound_panel_materializer{};
std::atomic<BuiltinPanelMaterializerFn> g_original_network_panel_materializer{};
std::atomic<BuiltinPanelMaterializerFn>
    g_original_keyboard_mouse_panel_materializer{};
std::atomic<BuiltinPanelMaterializerFn> g_original_graphics_panel_materializer{};
std::atomic<ConfigurationCategoryAppendFn> g_original_configuration_category_append{};
std::atomic_bool g_mod_settings_tab_hooks_ready{};
// Native-tab candidate: route category 4 through the already-installed
// six-argument Game Options hook, replacing both the brightness callback and
// the panel type, with no second hook at the same address.
std::atomic_bool g_experimental_mod_settings_route_enabled{true};
std::atomic<std::uint32_t> g_mod_settings_category_insertions{};
std::atomic<std::uint32_t> g_configuration_category_append_calls{};
std::atomic<std::uint32_t> g_tab_transition_count{};
std::atomic<void*> g_last_tab_control{};
std::atomic<void*> g_last_tab_selector{};
std::atomic<void*> g_last_tab_manager{};
std::atomic<void*> g_last_tab_active_panel{};
std::atomic<std::uint32_t> g_last_tab_selected_index{
    std::numeric_limits<std::uint32_t>::max()};
std::atomic<std::uint32_t> g_last_tab_selected_category{
    std::numeric_limits<std::uint32_t>::max()};
// 0=unknown, 1=title-menu category set (no Quit Game), 2=in-game set.
std::atomic<std::uint32_t> g_last_tab_context{};
std::atomic<std::uint64_t> g_last_tab_selection_tick{};
std::atomic_bool g_configuration_category_append_hook_installed{};
std::atomic<std::uint32_t> g_mod_settings_route_scopes{};
std::atomic<std::uint32_t> g_mod_settings_route_calls{};
std::atomic<std::uint32_t> g_mod_settings_route_returns{};
thread_local std::uint32_t t_mod_settings_route_depth{};
std::array<std::atomic<std::uint32_t>, 9>
    g_native_panel_materialization_calls{};
std::array<std::atomic<std::uint32_t>,
           kDiagnosticMenuContextCount * kDiagnosticCategoryCount>
    g_native_row_builder_diagnostic_calls{};
std::array<std::atomic<std::uint32_t>,
           kDiagnosticMenuContextCount * kDiagnosticCategoryCount>
    g_scaleform_visibility_diagnostic_calls{};
std::array<std::atomic<std::uint32_t>,
           kDiagnosticMenuContextCount * kDiagnosticCategoryCount>
    g_scaleform_text_diagnostic_calls{};
struct DiagnosticScaleformTextWriteKey {
    std::uintptr_t value{};
    std::uintptr_t caller{};
    std::uint64_t text_hash{};
    std::uint32_t category{std::numeric_limits<std::uint32_t>::max()};
    std::uint32_t menu_context{std::numeric_limits<std::uint32_t>::max()};
};
struct DiagnosticScaleformVisibilityKey {
    std::uintptr_t active_panel{};
    std::uintptr_t proxy{};
    std::uintptr_t caller{};
    std::uint32_t category{std::numeric_limits<std::uint32_t>::max()};
    std::uint32_t menu_context{std::numeric_limits<std::uint32_t>::max()};
    std::uint8_t visible{};
};
thread_local std::array<DiagnosticScaleformTextWriteKey, 512>
    t_logged_scaleform_text_writes{};
thread_local std::array<DiagnosticScaleformVisibilityKey, 512>
    t_logged_scaleform_visibility_changes{};
thread_local std::uint32_t t_diagnostic_native_category{
    std::numeric_limits<std::uint32_t>::max()};

struct DiagnosticNativeCategoryScope {
    std::uint32_t previous{t_diagnostic_native_category};

    explicit DiagnosticNativeCategoryScope(std::uint32_t category) noexcept {
        t_diagnostic_native_category = category;
    }

    ~DiagnosticNativeCategoryScope() {
        t_diagnostic_native_category = previous;
    }
};

std::uint32_t diagnostic_category() noexcept {
    if (t_diagnostic_native_category < kDiagnosticCategoryCount) {
        return t_diagnostic_native_category;
    }
    return g_last_tab_selected_category.load(std::memory_order_relaxed);
}

std::uint32_t diagnostic_menu_context() noexcept {
    return g_last_tab_context.load(std::memory_order_relaxed);
}

std::uint64_t hash_diagnostic_utf16(
    const wchar_t* text,
    std::size_t text_length) noexcept {
    std::uint64_t hash = 1469598103934665603ull;
    for (std::size_t index = 0; index < text_length; ++index) {
        hash ^= static_cast<std::uint16_t>(text[index]);
        hash *= 1099511628211ull;
    }
    hash ^= static_cast<std::uint64_t>(text_length);
    return hash;
}

bool claim_unique_scaleform_text_write(
    std::uint32_t menu_context,
    std::uint32_t category,
    std::uintptr_t value,
    std::uintptr_t caller,
    std::uint64_t text_hash) noexcept {
    for (auto& key : t_logged_scaleform_text_writes) {
        if (key.category == std::numeric_limits<std::uint32_t>::max()) {
            key = {value, caller, text_hash, category, menu_context};
            return true;
        }
        if (key.category == category && key.menu_context == menu_context &&
            key.value == value && key.caller == caller &&
            key.text_hash == text_hash) {
            return false;
        }
    }
    return false;
}

bool claim_unique_scaleform_visibility(
    std::uint32_t menu_context,
    std::uint32_t category,
    std::uintptr_t active_panel,
    std::uintptr_t proxy,
    std::uintptr_t caller,
    std::uint8_t visible) noexcept {
    for (auto& key : t_logged_scaleform_visibility_changes) {
        if (key.category == std::numeric_limits<std::uint32_t>::max()) {
            key = {active_panel, proxy, caller, category, menu_context, visible};
            return true;
        }
        if (key.category == category && key.menu_context == menu_context &&
            key.active_panel == active_panel && key.proxy == proxy &&
            key.caller == caller && key.visible == visible) {
            return false;
        }
    }
    return false;
}

std::uint64_t diagnostic_tab_age_ms() noexcept {
    const std::uint64_t selected_tick =
        g_last_tab_selection_tick.load(std::memory_order_relaxed);
    if (selected_tick == 0) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    const std::uint64_t now = GetTickCount64();
    return now >= selected_tick ? now - selected_tick : 0;
}

std::size_t diagnostic_menu_context_bucket(std::uint32_t context) noexcept {
    return context < kDiagnosticMenuContextCount
        ? static_cast<std::size_t>(context)
        : 0u;
}

const char* native_category_name(std::uint32_t code) noexcept;

bool read_diagnostic_stack_argument(
    std::uintptr_t stack_pointer,
    std::size_t offset,
    std::uintptr_t& value) noexcept {
    value = 0;
    if (stack_pointer < 0x10000) return false;
#if defined(_MSC_VER)
    __try {
#endif
        value = *reinterpret_cast<const std::uintptr_t*>(stack_pointer + offset);
        return true;
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        value = 0;
        return false;
    }
#endif
}

bool read_diagnostic_byte(const void* address, std::uint8_t& value) noexcept {
    value = 0;
    if (reinterpret_cast<std::uintptr_t>(address) < 0x10000) return false;
#if defined(_MSC_VER)
    __try {
#endif
        value = *static_cast<const std::uint8_t*>(address);
        return true;
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        value = 0;
        return false;
    }
#endif
}

template <typename T>
bool read_diagnostic_value(const void* address, T& value) noexcept {
    static_assert(std::is_trivially_copyable_v<T>);
    value = T{};
    if (reinterpret_cast<std::uintptr_t>(address) < 0x10000) return false;
#if defined(_MSC_VER)
    __try {
#endif
        std::memcpy(&value, address, sizeof(value));
        return true;
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        value = T{};
        return false;
    }
#endif
}

bool copy_diagnostic_utf16(
    const wchar_t* source,
    wchar_t* destination,
    std::size_t capacity,
    std::size_t& length,
    bool& truncated) noexcept {
    length = 0;
    truncated = false;
    if (!source || !destination || capacity < 2 ||
        reinterpret_cast<std::uintptr_t>(source) < 0x10000) {
        return false;
    }
#if defined(_MSC_VER)
    __try {
#endif
        for (std::size_t index = 0; index + 1 < capacity; ++index) {
            destination[index] = source[index];
            if (destination[index] == L'\0') {
                length = index;
                return true;
            }
        }
        destination[capacity - 1] = L'\0';
        length = capacity - 1;
        truncated = true;
        return true;
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        destination[0] = L'\0';
        length = 0;
        truncated = false;
        return false;
    }
#endif
}
std::atomic<std::uint16_t> g_builtin_panel_observer_only_mask{};
std::atomic<std::uint16_t> g_builtin_panel_warning_mask{};
SafetyHookInline g_sub_hook{};
SafetyHookInline g_page_frame_gate_hook{};
SafetyHookInline g_dialog_back_gate_hook{};
std::atomic<PageFrameFn> g_original_page_frame{};
std::atomic<NativeBackFn> g_original_dialog_back{};
SafetyHookMid g_scaleform_path_resolver_hook{};
constexpr std::array<std::uint8_t, 16> kScaleformPathResolverEntry{
    0x4C, 0x89, 0x44, 0x24, 0x18, 0x4C, 0x89, 0x4C,
    0x24, 0x20, 0x55, 0x53, 0x56, 0x57, 0x41, 0x56};
std::mutex g_scaleform_path_bridge_mutex{};
void* g_scaleform_path_bridge_target{};
std::array<std::uint8_t, 16> g_scaleform_path_bridge_patch{};
bool g_scaleform_path_bridge_proven{};
std::atomic<GameOptionsHandlerFn> g_original_game_options{nullptr};
std::atomic<SubHandlerFn> g_original_sub{nullptr};
std::atomic<TextResolverFn> g_original_text_resolver{nullptr};
void* g_text_hook_target{};
bool g_text_hook_created{};
bool g_text_hook_enabled{};

std::atomic<void*> g_last_page{};
// Exact physical page currently materialized by ERNativeUI. The native frame
// dispatcher is shared; modal gating every instance also suppresses the
// dialog's own confirmation work.
std::atomic<void*> g_dialog_input_page{};
std::atomic<std::uint64_t> g_last_injection_tick{};
std::atomic<std::uint32_t> g_game_options_entry_count{};
std::atomic<std::uint32_t> g_game_options_injection_count{};
std::atomic<std::uint32_t> g_text_entry_count{};
std::atomic<std::uint32_t> g_text_fault_count{};
std::atomic<std::uint32_t> g_sub_entry_count{};
std::atomic<std::uint32_t> g_sub_injection_count{};
std::atomic<std::uint32_t> g_title_fault_count{};
erui::detail::SubmenuNavigation g_submenu_navigation{};
TitleCaptureState g_title_capture{};

bool mod_settings_page_registered() noexcept {
    const erui::detail::RuntimeState& runtime =
        erui::detail::runtime_state();
    if (!runtime.menu) return false;
    const std::size_t page_index = runtime.menu->builtin_page_index(
        kModSettingsNativeCategory);
    return page_index < runtime.menu->pages.size() &&
        !runtime.menu->pages[page_index].rows.empty();
}

std::uint32_t mod_settings_page_title_id() noexcept {
    const erui::detail::RuntimeState& runtime =
        erui::detail::runtime_state();
    if (!runtime.menu) return 0;
    const std::size_t page_index = runtime.menu->builtin_page_index(
        kModSettingsNativeCategory);
    return page_index < runtime.menu->pages.size()
        ? runtime.menu->pages[page_index].title_id
        : 0;
}

constexpr std::size_t kPanelVisualCapacityOffset = 0xB14;
constexpr std::size_t kPanelObservedFieldD84Offset = 0xD84;
constexpr std::size_t kPanelObservedFieldD88Offset = 0xD88;
constexpr std::size_t kPanelNativeCountOffset = 0x1AF0;
std::uintptr_t pad_setting_dialog_vtable_rva() noexcept {
    const auto* const profile = erui::native::running_menu_object_profile();
    return profile ? profile->pad_setting_dialog_vtable_rva : 0;
}
constexpr std::size_t kPadSettingDialogEmbeddedObjectOffset = 0x1DA8;
constexpr std::size_t kPadPropertyCollectionOffset = 0x1268;
constexpr std::size_t kPadPropertyRowStride = 0x88;
constexpr std::size_t kPadPropertyCountOffset = 0x1AF0;
constexpr std::size_t kPadPropertyControllerOffset = 0x78;
constexpr std::size_t kPadControllerBoundValueOffset = 0x1D0;
std::uintptr_t edit_property_vtable_rva() noexcept {
    const auto* const profile = erui::native::running_menu_object_profile();
    return profile ? profile->edit_property_vtable_rva : 0;
}
std::uintptr_t text_input_controller_vtable_rva() noexcept {
    const auto* const profile = erui::native::running_menu_object_profile();
    return profile ? profile->text_input_controller_vtable_rva : 0;
}
constexpr std::size_t kOptionSettingTabSelectorOffset = 0x10;
constexpr std::size_t kOptionSettingTabManagerOffset = 0x18;
constexpr std::size_t kCompositeCurrentPanelOffset = 0xB8;
constexpr std::size_t kOptionSettingTopTabControlOffset = 0x1870;
constexpr std::size_t kRootVisualCapacityOffset = 0xB14;
constexpr std::size_t kRootMaterializedRowCountOffset = 0x1AF0;

[[nodiscard]] std::uintptr_t option_setting_dialog_vtable_rva() noexcept {
    if (g_addresses.game_image_base &&
        g_addresses.option_setting_dialog_vtable) {
        const auto base = reinterpret_cast<std::uintptr_t>(
            g_addresses.game_image_base);
        const auto value = reinterpret_cast<std::uintptr_t>(
            g_addresses.option_setting_dialog_vtable);
        if (value >= base && value - base < g_addresses.game_image_size) {
            return value - base;
        }
    }
#if defined(ER_NATIVE_UI_CUSTOM_TABS_BACKEND)
    if (g_tab_carrier_build_profile) {
        return g_tab_carrier_build_profile->option_setting_dialog_vtable_rva;
    }
#endif
    // Fork: the running build's profile (2.7.1.1 and 2.7.2.0 move it), then
    // 0 so a comparison on an unknown build fails closed.
    const auto* const profile = erui::native::running_menu_object_profile();
    return profile ? profile->option_setting_dialog_vtable_rva : 0;
}

bool read_scaleform_path_entry(
    const void* target,
    std::array<std::uint8_t, 16>& bytes) noexcept {
    if (!target) return false;
#if defined(_MSC_VER)
    __try {
#endif
        std::memcpy(bytes.data(), target, bytes.size());
        return true;
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        bytes = {};
        return false;
    }
#endif
}

struct BuiltinPanelSnapshot {
    void* vtable{};
    std::uint32_t observed_field_d84{};
    std::uint32_t observed_field_d88{};
    std::uint32_t visual_capacity{};
    std::uint64_t native_count{};
    bool fields_read{};
    bool vtable_match{};
    bool capacity_in_range{};
    bool count_within_capacity{};
    bool valid{};
};

struct NativeRootSnapshot {
    std::uint32_t visual_capacity{};
    std::uint64_t materialized_rows{};
    bool valid{};
};

const char* native_category_name(std::uint32_t code) noexcept;

NativeRootSnapshot inspect_native_root(void* page) noexcept {
    NativeRootSnapshot result{};
    if (!page) return result;
#if defined(_MSC_VER)
    __try {
#endif
        const auto* bytes = static_cast<const std::byte*>(page);
        result.visual_capacity = *reinterpret_cast<const std::uint32_t*>(
            bytes + kRootVisualCapacityOffset);
        result.materialized_rows = *reinterpret_cast<const std::uint64_t*>(
            bytes + kRootMaterializedRowCountOffset);
        result.valid = result.visual_capacity >= 4 &&
            result.visual_capacity <= 15 && result.materialized_rows <= 64;
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        result = {};
    }
#endif
    return result;
}

void log_native_root_state(
    std::uint32_t entry,
    void* page,
    const char* phase) noexcept {
    if (!erui::detail::diagnostics_enabled() || entry > 64) return;
    const NativeRootSnapshot snapshot = inspect_native_root(page);
    erui::detail::logf(
        erui::LogLevel::info,
        "diagnostic: native root state[%u] phase=%s page=%p valid=%u nativeRows=%llu visualRows=%u",
        static_cast<unsigned>(entry),
        phase ? phase : "unknown",
        page,
        snapshot.valid ? 1u : 0u,
        static_cast<unsigned long long>(snapshot.materialized_rows),
        static_cast<unsigned>(snapshot.visual_capacity));
}

BuiltinPanelSnapshot inspect_builtin_panel(void* page) noexcept {
    BuiltinPanelSnapshot result{};
    if (!page) return result;
#if defined(_MSC_VER)
    __try {
#endif
        const auto* bytes = static_cast<const std::byte*>(page);
        result.vtable = *reinterpret_cast<void* const*>(bytes);
        result.visual_capacity = *reinterpret_cast<const std::uint32_t*>(
            bytes + kPanelVisualCapacityOffset);
        result.observed_field_d84 = *reinterpret_cast<const std::uint32_t*>(
            bytes + kPanelObservedFieldD84Offset);
        result.observed_field_d88 = *reinterpret_cast<const std::uint32_t*>(
            bytes + kPanelObservedFieldD88Offset);
        result.native_count = *reinterpret_cast<const std::uint64_t*>(
            bytes + kPanelNativeCountOffset);
        result.fields_read = true;
        const std::uintptr_t image =
            reinterpret_cast<std::uintptr_t>(g_addresses.game_image_base);
        const std::uintptr_t vtable_rva =
            option_setting_dialog_vtable_rva();
        const void* expected_vtable = reinterpret_cast<const void*>(
            image + vtable_rva);
        result.vtable_match = image &&
            vtable_rva < g_addresses.game_image_size &&
            result.vtable == expected_vtable;
        result.capacity_in_range = result.visual_capacity >= 1 &&
            result.visual_capacity <=
                erui::detail::builtin_page_max_first_capacity;
        result.count_within_capacity = result.native_count <= result.visual_capacity;
        result.valid = result.fields_read && result.vtable_match &&
            result.capacity_in_range && result.count_within_capacity;
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        result = {};
    }
#endif
    return result;
}

// Diagnostic for the dedicated Mod Settings tab when another mod reorganizes
// the option-setting pages (Solid Uncapper 2.3+). Logs which object the
// category-4 route actually handed to Game Options so it can be compared
// with a clean run. Read-only; every read is fault-guarded.
void log_scoped_panel_identity(void* page, const char* phase) noexcept {
    if (!page) {
        erui::detail::logf(erui::LogLevel::info,
            "diagnostic: scoped panel identity phase=%s page=null", phase);
        return;
    }
    const std::uintptr_t image =
        reinterpret_cast<std::uintptr_t>(g_addresses.game_image_base);
    const std::uintptr_t image_end = image + g_addresses.game_image_size;
    const auto rva_of = [&](std::uintptr_t value) -> unsigned long long {
        return value >= image && value < image_end
            ? static_cast<unsigned long long>(value - image)
            : 0ull;
    };
    const BuiltinPanelSnapshot panel = inspect_builtin_panel(page);
    std::uint64_t window_b00[8]{};
    std::uint64_t window_1ae0[4]{};
    std::uint64_t head[8]{};
    bool read_ok = false;
#if defined(_MSC_VER)
    __try {
#endif
        const auto* bytes = static_cast<const std::byte*>(page);
        std::memcpy(head, bytes, sizeof(head));
        std::memcpy(window_b00, bytes + 0xB00, sizeof(window_b00));
        std::memcpy(window_1ae0, bytes + 0x1AE0, sizeof(window_1ae0));
        read_ok = true;
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        read_ok = false;
    }
#endif
    const auto vtable = reinterpret_cast<std::uintptr_t>(panel.vtable);
    erui::detail::logf(
        erui::LogLevel::info,
        "diagnostic: scoped panel identity phase=%s page=%p read=%u vtable=%p vtableRva=0x%llX expectedOptionSettingRva=0x%llX vtableMatch=%u capacity@B14=%u d84=%u d88=%u count@1AF0=%llu",
        phase, page, read_ok ? 1u : 0u, panel.vtable,
        rva_of(vtable),
        static_cast<unsigned long long>(option_setting_dialog_vtable_rva()),
        panel.vtable_match ? 1u : 0u,
        static_cast<unsigned>(panel.visual_capacity),
        static_cast<unsigned>(panel.observed_field_d84),
        static_cast<unsigned>(panel.observed_field_d88),
        static_cast<unsigned long long>(panel.native_count));
    if (!read_ok) return;
    erui::detail::logf(
        erui::LogLevel::info,
        "diagnostic: scoped panel head phase=%s q0=%llX(rva %llX) q1=%llX(rva %llX) q2=%llX q3=%llX q4=%llX q5=%llX q6=%llX q7=%llX",
        phase,
        static_cast<unsigned long long>(head[0]), rva_of(head[0]),
        static_cast<unsigned long long>(head[1]), rva_of(head[1]),
        static_cast<unsigned long long>(head[2]),
        static_cast<unsigned long long>(head[3]),
        static_cast<unsigned long long>(head[4]),
        static_cast<unsigned long long>(head[5]),
        static_cast<unsigned long long>(head[6]),
        static_cast<unsigned long long>(head[7]));
    erui::detail::logf(
        erui::LogLevel::info,
        "diagnostic: scoped panel window phase=%s B00=%llX B08=%llX B10=%llX B18=%llX B20=%llX B28=%llX B30=%llX B38=%llX 1AE0=%llX 1AE8=%llX 1AF0=%llX 1AF8=%llX",
        phase,
        static_cast<unsigned long long>(window_b00[0]),
        static_cast<unsigned long long>(window_b00[1]),
        static_cast<unsigned long long>(window_b00[2]),
        static_cast<unsigned long long>(window_b00[3]),
        static_cast<unsigned long long>(window_b00[4]),
        static_cast<unsigned long long>(window_b00[5]),
        static_cast<unsigned long long>(window_b00[6]),
        static_cast<unsigned long long>(window_b00[7]),
        static_cast<unsigned long long>(window_1ae0[0]),
        static_cast<unsigned long long>(window_1ae0[1]),
        static_cast<unsigned long long>(window_1ae0[2]),
        static_cast<unsigned long long>(window_1ae0[3]));
}

void log_native_panel_state(
    const char* panel_name,
    std::uint8_t native_category,
    void* page,
    void* menu_option_data,
    const char* phase) noexcept {
    if (!erui::detail::diagnostics_enabled() ||
        native_category >= g_native_panel_materialization_calls.size()) {
        return;
    }
    const std::uint32_t entry =
        g_native_panel_materialization_calls[native_category].fetch_add(
            1, std::memory_order_relaxed) + 1;
    if (entry > kNativePanelDiagnosticLogLimit) return;

    const BuiltinPanelSnapshot snapshot = inspect_builtin_panel(page);
    erui::detail::logf(
        erui::LogLevel::info,
        "diagnostic: native panel state[%u] phase=%s category=%u panel=%s page=%p optionData=%p vtable=%p fieldsRead=%u vtableMatch=%u countAt1AF0=%llu rawB14=%u rawB14RangeCheck=%u countVsRawB14=%u fieldD84=%u fieldD88=%u legacyLayoutChecksPass=%u",
        static_cast<unsigned>(entry),
        phase ? phase : "unknown",
        static_cast<unsigned>(native_category),
        panel_name ? panel_name : "unknown",
        page,
        menu_option_data,
        snapshot.vtable,
        snapshot.fields_read ? 1u : 0u,
        snapshot.vtable_match ? 1u : 0u,
        static_cast<unsigned long long>(snapshot.native_count),
        static_cast<unsigned>(snapshot.visual_capacity),
        snapshot.capacity_in_range ? 1u : 0u,
        snapshot.count_within_capacity ? 1u : 0u,
        static_cast<unsigned>(snapshot.observed_field_d84),
        static_cast<unsigned>(snapshot.observed_field_d88),
        snapshot.valid ? 1u : 0u);
}

void log_vanilla_subpage_state(
    std::uint32_t entry,
    void* page,
    const char* phase) noexcept {
    if (!erui::detail::diagnostics_enabled() || entry > 64) return;
    const BuiltinPanelSnapshot snapshot = inspect_builtin_panel(page);
    const std::uint32_t category = diagnostic_category();
    erui::detail::logf(
        erui::LogLevel::trace,
        "diagnostic: vanilla subpage[%u] phase=%s category=%u categoryName=%s page=%p vtable=%p fieldsRead=%u vtableMatch=%u countAt1AF0=%llu rawB14=%u rawB14RangeCheck=%u countVsRawB14=%u fieldD84=%u fieldD88=%u legacyLayoutChecksPass=%u",
        static_cast<unsigned>(entry),
        phase ? phase : "unknown",
        static_cast<unsigned>(category),
        native_category_name(category),
        page,
        snapshot.vtable,
        snapshot.fields_read ? 1u : 0u,
        snapshot.vtable_match ? 1u : 0u,
        static_cast<unsigned long long>(snapshot.native_count),
        static_cast<unsigned>(snapshot.visual_capacity),
        snapshot.capacity_in_range ? 1u : 0u,
        snapshot.count_within_capacity ? 1u : 0u,
        static_cast<unsigned>(snapshot.observed_field_d84),
        static_cast<unsigned>(snapshot.observed_field_d88),
        snapshot.valid ? 1u : 0u);
}

#if defined(ER_NATIVE_UI_CUSTOM_TABS_BACKEND)
void __fastcall tab_content_category8_materializer(
    void* page,
    void* menu_option_data) noexcept;

constexpr std::size_t kPanelSceneProxyOffset = 0x1200;
constexpr std::size_t kSceneProxyWrapperFlagsOffset = 0x20;
constexpr std::size_t kSceneProxyWrapperPayloadOffset = 0x28;
struct TabCarrierIdentity {
    void* panel{};
    void* wrapper{};
    void* payload{};
    std::uint32_t wrapper_flags{};
    std::size_t logical_page_index{
        erui::detail::invalid_compiled_index};
    std::uint8_t category{};
    bool valid{};
    bool committed{};
};

std::uint64_t g_tab_carrier_epoch{};

TabCarrierIdentity inspect_tab_carrier(
    void* page,
    std::uint8_t category,
    std::size_t logical_page_index) noexcept {
    TabCarrierIdentity result{};
    result.panel = page;
    result.category = category;
    result.logical_page_index = logical_page_index;
    const TabCarrierBuildProfile* const profile =
        g_tab_carrier_build_profile;
    if (!page || !profile || !g_addresses.game_image_base ||
        profile->scene_obj_proxy_get_value_rva >=
            g_addresses.game_image_size ||
        profile->scene_obj_proxy_vtable_rva >=
            g_addresses.game_image_size) {
        return result;
    }
#if defined(_MSC_VER)
    __try {
#endif
        const std::uintptr_t image = reinterpret_cast<std::uintptr_t>(
            g_addresses.game_image_base);
        auto* const proxy = static_cast<std::byte*>(page) +
            kPanelSceneProxyOffset;
        void* const proxy_vtable = *reinterpret_cast<void**>(proxy);
        void* const expected_vtable = reinterpret_cast<void*>(
            image + profile->scene_obj_proxy_vtable_rva);
        void* const getter = proxy_vtable
            ? *reinterpret_cast<void**>(
                  static_cast<std::byte*>(proxy_vtable) + sizeof(void*))
            : nullptr;
        if (proxy_vtable != expected_vtable ||
            getter != reinterpret_cast<void*>(
                image + profile->scene_obj_proxy_get_value_rva)) {
            return result;
        }
        using GetterFn = void*(__fastcall*)(void*);
        result.wrapper = reinterpret_cast<GetterFn>(getter)(proxy);
        if (!result.wrapper) return result;
        result.wrapper_flags = *reinterpret_cast<const std::uint32_t*>(
            static_cast<const std::byte*>(result.wrapper) +
                kSceneProxyWrapperFlagsOffset);
        result.payload = *reinterpret_cast<void* const*>(
            static_cast<const std::byte*>(result.wrapper) +
                kSceneProxyWrapperPayloadOffset);
        result.valid = result.payload != nullptr &&
            (result.wrapper_flags & 0x8Fu) >= 2u;
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        result = {};
        result.panel = page;
        result.category = category;
        result.logical_page_index = logical_page_index;
    }
#endif
    return result;
}

// Graphics carrier V2. Category 8 is the sole recyclable
// rows-only carrier. Category 5 is used only as synchronous native parking.
TabCarrierIdentity g_tab_carrier{};
std::size_t g_reusable_route{erui::detail::invalid_compiled_index};
void* g_reusable_composite{};
std::uint64_t g_reusable_activation_frame{};
std::atomic_bool g_reusable_failed{};
using TabCarrierParkingSwitchFn = bool(__cdecl*)(void*, std::int32_t);
using TabCarrierQueueFn = void(__fastcall*)(void*, void*);

std::size_t custom_tab_route_count() noexcept {
    const auto& runtime = erui::detail::runtime_state();
    return runtime.menu ? runtime.menu->tab_page_indices.size() : 0;
}

std::size_t native_graphics_route() noexcept {
    return custom_tab_route_count();
}

std::size_t graphics_route_count() noexcept {
    const std::size_t custom_count = custom_tab_route_count();
    return custom_count == (std::numeric_limits<std::size_t>::max)()
        ? custom_count
        : custom_count + 1;
}

struct TabCarrierAllocatorTarget {
    std::uintptr_t vtable;
    std::uintptr_t method;
    std::uint64_t entry;
};

// Exact DLAllocator descendants validated for Configuration panels in each
// admitted executable. No unlisted vtable/queue pair is called.
constexpr std::array<TabCarrierAllocatorTarget, 26>
    kTabCarrierAllocatorTargets271{{
        {0x29D54F0,0x2657530,0xFF864788E9CA8B48},
        {0x2B7D418,0xB97EC0,0x74894808245C8948},
        {0x2BDE708,0xE14B60,0x8B4820EC83485340},
        {0x2BDE8F0,0xE156B0,0xC74840EC83485740},
        {0x2BDECA0,0xE16260,0x28C1834808498B48},
        {0x2BDEA48,0xE15BA0,0x28C1834808498B48},
        {0x2BDED78,0xE166D0,0xC74840EC83485740},
        {0x2BDE818,0xE150C0,0xC74840EC83485740},
        {0x2BDEB20,0xE15F30,0x28C1834808498B48},
        {0x2BDEE50,0xE16CA0,0xC74840EC83485740},
        {0x2BE1870,0xE1F250,0x8B4820EC83485340},
        {0x2BE1A20,0xE1F7B0,0x8B4820EC83485340},
        {0x2F26FA8,0x1C59C30,0x00262088E9CA8B48},
        {0x2F273D0,0x1AF6110,0x5FA5EF08900000C2},
        {0x30357D8,0x1B44400,0x28C1834808498B48},
        {0x3051240,0x1C29FE0,0xA33CC7EC900000C2},
        {0x30B0298,0x1EBCE90,0xEB17900000065BE9},
        {0x30C0228,0x1F38D90,0xC74840EC83485740},
        {0x30CA908,0x1F61F50,0x48018B4808498B48},
        {0x30CAE70,0x1F64450,0xC74830EC83485740},
        {0x30CFE80,0x1F81AA0,0x3176CC030E302AE9},
        {0x30CFF58,0x1F81AB0,0x35A19002D12495E9},
        {0x3278EA0,0x26398D0,0xC74830EC83485740},
        {0x30C74A0,0x1A4900,0x28C1834808498B48},
        {0x2BBFF38,0x1538B0,0xC74840EC83485740},
        {0x2BE0F30,0x1F1A8D0,0x48018B4808498B48},
    }};

constexpr std::array<TabCarrierAllocatorTarget, 26>
    kTabCarrierAllocatorTargets272{{
        {0x29D74F0,0x2658C60,0xFF864748E9CA8B48},
        {0x2B7F4A0,0xB981C0,0x83485308244C8948},
        {0x2BE09B8,0xE16250,0x8B4820EC83485340},
        {0x2BE0BA0,0xE16DA0,0xC74840EC83485740},
        {0x2BE0F50,0xE17950,0x28C1834808498B48},
        {0x2BE0CF8,0xE17290,0x28C1834808498B48},
        {0x2BE1028,0xE17DC0,0xC74840EC83485740},
        {0x2BE0AC8,0xE167B0,0xC74840EC83485740},
        {0x2BE0DD0,0xE17620,0x28C1834808498B48},
        {0x2BE1100,0xE18390,0xC74840EC83485740},
        {0x2BE3B20,0xE20940,0x8B4820EC83485340},
        {0x2BE3CD0,0xE20EA0,0x8B4820EC83485340},
        {0x2F29248,0x1C5B320,0x00262088E9CA8B48},
        {0x2F29670,0x1AF7800,0x2745C748CC0000C2},
        {0x3037A78,0x1B45AF0,0x28C1834808498B48},
        {0x30534E0,0x1C2B6D0,0xFF418D48CC0000C2},
        {0x30B2558,0x1EBE580,0xE872CC0000065BE9},
        {0x30C24E8,0x1F3A480,0xC74840EC83485740},
        {0x30CCBC8,0x1F63640,0x48018B4808498B48},
        {0x30CD130,0x1F65B40,0xC74830EC83485740},
        {0x30D2140,0x1F83190,0x8B41CC02D5E587E9},
        {0x30D2218,0x1F831A0,0x8B489003B215C8E9},
        {0x327B160,0x263B000,0xC74830EC83485740},
        {0x30C9760,0x1A4900,0x28C1834808498B48},
        {0x2BC21E8,0x1538B0,0xC74840EC83485740},
        {0x2BE31E0,0x1F1BFC0,0x48018B4808498B48},
    }};

constexpr std::array<TabCarrierBuildProfile, 2>
    kTabCarrierBuildProfiles{{
        {
            EldenRingBuild::v2_7_1_0,
            0x6A96B418,
            0x5E0DA00,
            0x93CF89,
            0x95C050,
            0x95A820,
            0x93C1A0,
            0x2B14888,
            0x2A97AF0,
            0x74C930,
            0xE1C080,
            0x953520,
            0x2B0C950,
            0x2B16B48,
            0x93CF20,
            0x93CF32,
            0x93CF58,
            0x93CF84,
            {
                0x4C,0x63,0x05,0x81,0xF3,0xF6,0x02,0x33,
                0xD2,0x4D,0x85,0xC0,0x7E,0x1E,0x48,0x8D,
            },
            kTabCarrierAllocatorTargets271.data(),
            kTabCarrierAllocatorTargets271.size(),
        },
        {
            EldenRingBuild::v2_7_2_0,
            0x6AABF7E7,
            0x5DFD600,
            0x93D439,
            0x95C500,
            0x95ACD0,
            0x93C650,
            0x2B16978,
            0x2A99BE0,
            0x74CDE0,
            0xE1D770,
            0x9539D0,
            0x2B0EA40,
            0x2B18C38,
            0x93D3D0,
            0x93D3E2,
            0x93D408,
            0x93D434,
            {
                0x4C,0x63,0x05,0xB1,0x0C,0xF7,0x02,0x33,
                0xD2,0x4D,0x85,0xC0,0x7E,0x1E,0x48,0x8D,
            },
            kTabCarrierAllocatorTargets272.data(),
            kTabCarrierAllocatorTargets272.size(),
        },
    }};

[[nodiscard]] constexpr const TabCarrierBuildProfile*
find_tab_carrier_build_profile(
    std::uint32_t timestamp,
    std::size_t image_size) noexcept {
    for (const TabCarrierBuildProfile& profile :
         kTabCarrierBuildProfiles) {
        if (profile.timestamp == timestamp &&
            profile.image_size == image_size) {
            return &profile;
        }
    }
    return nullptr;
}

bool reusable_fault(const char* reason) noexcept {
    g_reusable_failed.store(true, std::memory_order_release);
    erui::detail::logf(erui::LogLevel::error,
        "Native-tab Graphics carrier FAILED CLOSED: %s; custom transitions frozen, native input remains live",
        reason);
    return false;
}

bool exact_allocator_target(
    std::uintptr_t image,
    void* manager_vtable,
    void* queue) noexcept {
    const TabCarrierBuildProfile* const profile =
        g_tab_carrier_build_profile;
    if (!profile || !profile->allocator_targets ||
        profile->allocator_target_count == 0) return false;
    for (std::size_t index = 0;
         index < profile->allocator_target_count;
         ++index) {
        const TabCarrierAllocatorTarget& target =
            profile->allocator_targets[index];
        if (manager_vtable == reinterpret_cast<void*>(
                image + target.vtable) &&
            queue == reinterpret_cast<void*>(image + target.method) &&
            std::memcmp(queue, &target.entry, sizeof(target.entry)) == 0) {
            return true;
        }
    }
    return false;
}

bool cached_identity_is_globally_unique(
    void** slots,
    std::size_t owner_slot,
    const TabCarrierIdentity& owner,
    const char* label) noexcept {
    if (!slots || owner_slot >= 10 || !owner.valid ||
        slots[owner_slot] != owner.panel || !owner.wrapper ||
        !owner.payload) {
        erui::detail::logf(erui::LogLevel::error,
            "Native-tab Graphics carrier identity audit rejected %s owner slot=%zu panel=%p wrapper=%p payload=%p valid=%d",
            label ? label : "unknown", owner_slot, owner.panel,
            owner.wrapper, owner.payload, owner.valid ? 1 : 0);
        return false;
    }
    for (std::size_t slot = 0; slot < 10; ++slot) {
        if (slot == owner_slot || !slots[slot]) continue;
        const auto other = inspect_tab_carrier(
            slots[slot], static_cast<std::uint8_t>(slot),
            erui::detail::invalid_compiled_index);
        const bool distinct = other.valid &&
            other.panel != owner.panel &&
            other.wrapper != owner.wrapper &&
            other.payload != owner.payload;
        if (!distinct) {
            erui::detail::logf(erui::LogLevel::error,
                "Native-tab Graphics carrier identity audit rejected %s ownerSlot=%zu owner=%p/%p/%p conflictingSlot=%zu other=%p/%p/%p valid=%d",
                label ? label : "unknown", owner_slot,
                owner.panel, owner.wrapper, owner.payload, slot,
                other.panel, other.wrapper, other.payload,
                other.valid ? 1 : 0);
            return false;
        }
    }
    return true;
}

void clear_retired_page_references(void* panel) noexcept {
    if (!panel) return;
    void* expected = panel;
    const bool dialog_cleared = g_dialog_input_page.compare_exchange_strong(
        expected, nullptr, std::memory_order_acq_rel,
        std::memory_order_acquire);
    expected = panel;
    const bool root_cleared = g_last_page.compare_exchange_strong(
        expected, nullptr, std::memory_order_acq_rel,
        std::memory_order_acquire);

    // A tab transition cannot legitimately overlap a pending subpage open.
    // Cancel the request before destroying its possible parent page. Existing
    // child bindings are left intact and are normally removed by native Back.
    const bool pending_subpage =
        g_submenu_navigation.pending_route().valid();
    if (pending_subpage) g_submenu_navigation.cancel_request();
    reset_color_picker_page_widgets(
        panel, ColorPickerWidgetHost::subpage);
    erui::detail::logf(erui::LogLevel::trace,
        "Native-tab Graphics carrier cleared retiring page references panel=%p dialog=%d root=%d pendingSubpageCancelled=%d",
        panel, dialog_cleared ? 1 : 0, root_cleared ? 1 : 0,
        pending_subpage ? 1 : 0);
}

std::uint32_t graphics_carrier_version_v2_impl() noexcept {
    return 2;
}

bool prepare_graphics_carrier_v2_impl(
    void* composite,
    std::size_t route,
    std::uint64_t completed_frame,
    std::int32_t* category,
    TabCarrierParkingSwitchFn park_native) noexcept {
    if (g_reusable_failed.load(std::memory_order_acquire)) return false;
    const auto& runtime = erui::detail::runtime_state();
    const std::size_t custom_count = custom_tab_route_count();
    if (!composite || !category || !park_native || !runtime.menu ||
        custom_count == 0 || route >= graphics_route_count() ||
        (g_reusable_composite && composite != g_reusable_composite)) {
        return reusable_fault(
            "invalid composite, catalogue, route or output");
    }
    const auto image = reinterpret_cast<std::uintptr_t>(
        g_addresses.game_image_base);
    const TabCarrierBuildProfile* const profile =
        g_tab_carrier_build_profile;
    if (!image || !profile ||
        g_addresses.game_image_size != profile->image_size) {
        return reusable_fault(
            "retirement requires an exact admitted Elden Ring image");
    }

#if defined(_MSC_VER)
    __try {
#endif
        constexpr std::array<std::uint8_t, 16> deactivate_entry{
            0x48,0x89,0x5C,0x24,0x08,0x57,0x48,0x83,
            0xEC,0x20,0x8B,0xDA,0x48,0x8B,0xF9,0xE8};
        if (std::memcmp(
                reinterpret_cast<const void*>(
                    image + profile->carrier_manager_rva),
                profile->manager_entry.data(),
                profile->manager_entry.size()) != 0 ||
            std::memcmp(reinterpret_cast<const void*>(
                    image + profile->carrier_deactivate_rva),
                deactivate_entry.data(), deactivate_entry.size()) != 0) {
            return reusable_fault(
                "retirement manager/deactivate bytes differed");
        }

        auto* const bytes = static_cast<std::byte*>(composite);
        if (*reinterpret_cast<void**>(bytes) !=
                reinterpret_cast<void*>(
                    image + profile->composite_vtable_rva) ||
            *reinterpret_cast<void**>(bytes - 0x1768) !=
                reinterpret_cast<void*>(image + profile->top_vtable_rva)) {
            return reusable_fault(
                "composite/top vtable differed from validated objects");
        }

        auto** const slots = reinterpret_cast<void**>(bytes + 0x68);
        void* current = *reinterpret_cast<void**>(bytes + 0xB8);
        g_reusable_composite = composite;
        constexpr std::int32_t target = 8;
        constexpr std::int32_t parking = 5;

        if (g_reusable_route == route && slots[target]) {
            const auto observed = inspect_tab_carrier(
                slots[target], target,
                g_tab_carrier.logical_page_index);
            if (!g_tab_carrier.committed || !observed.valid ||
                slots[target] != g_tab_carrier.panel ||
                observed.wrapper != g_tab_carrier.wrapper ||
                observed.payload != g_tab_carrier.payload ||
                !cached_identity_is_globally_unique(
                    slots, target, observed,
                    "same-route Graphics carrier")) {
                return reusable_fault(
                    "cached route lost its exact committed Graphics carrier");
            }
            *category = target;
            g_reusable_activation_frame = completed_frame;
            return true;
        }

        void* const old_panel = slots[target];
        if (old_panel) {
            if (completed_frame <= g_reusable_activation_frame) {
                return reusable_fault(
                    "Graphics carrier retirement requires one completed frame");
            }
            const auto observed = inspect_tab_carrier(
                old_panel, target,
                g_tab_carrier.logical_page_index);
            const auto panel = inspect_builtin_panel(old_panel);
            if (!g_tab_carrier.committed || !observed.valid ||
                !panel.valid || g_reusable_route >= graphics_route_count() ||
                observed.panel != g_tab_carrier.panel ||
                observed.wrapper != g_tab_carrier.wrapper ||
                observed.payload != g_tab_carrier.payload ||
                !cached_identity_is_globally_unique(
                    slots, target, observed,
                    "pre-retirement Graphics carrier")) {
                return reusable_fault(
                    "Graphics identity failed before native retirement");
            }

            using ManagerFn = void*(__fastcall*)(void*);
            using DeactivateFn = void(__fastcall*)(void*, bool);
            void* const manager =
                reinterpret_cast<ManagerFn>(
                    image + profile->carrier_manager_rva)(old_panel);
            if (!manager) return reusable_fault("retirement manager was null");
            void* const deactivate = *reinterpret_cast<void**>(
                static_cast<std::byte*>(panel.vtable) + 0x08);
            void* const manager_vtable = *reinterpret_cast<void**>(manager);
            void* const queue = manager_vtable
                ? *reinterpret_cast<void**>(
                      static_cast<std::byte*>(manager_vtable) + 0x68)
                : nullptr;
            if (deactivate != reinterpret_cast<void*>(
                    image + profile->carrier_deactivate_rva) ||
                !manager_vtable || !queue ||
                !exact_allocator_target(image, manager_vtable, queue)) {
                return reusable_fault(
                    "retirement allocator/deactivate target was not exact");
            }

            if (old_panel == current) {
                void* const parking_before = slots[parking];
                const auto parking_identity_before = inspect_tab_carrier(
                    parking_before, parking,
                    erui::detail::invalid_compiled_index);
                if (parking_before &&
                    (!inspect_builtin_panel(parking_before).valid ||
                        !parking_identity_before.valid ||
                        parking_before == old_panel ||
                        parking_identity_before.wrapper == observed.wrapper ||
                        parking_identity_before.payload == observed.payload ||
                        !cached_identity_is_globally_unique(
                            slots, parking, parking_identity_before,
                            "cached Network parking panel"))) {
                    return reusable_fault(
                        "cached Network parking identity was invalid or aliased");
                }
                if (!park_native(composite, parking)) {
                    return reusable_fault(
                        "native Network parking switch/state isolation failed");
                }
                void* const parked_panel = slots[parking];
                const auto parked = inspect_tab_carrier(
                    parked_panel, parking,
                    erui::detail::invalid_compiled_index);
                const auto old_after = inspect_tab_carrier(
                    old_panel, target,
                    g_tab_carrier.logical_page_index);
                if (!parked_panel || !parked.valid ||
                    !inspect_builtin_panel(parked_panel).valid ||
                    *reinterpret_cast<void**>(bytes + 0xB8) != parked_panel ||
                    parked_panel == old_panel ||
                    parked.wrapper == observed.wrapper ||
                    parked.payload == observed.payload ||
                    slots[target] != old_panel || !old_after.valid ||
                    old_after.wrapper != observed.wrapper ||
                    old_after.payload != observed.payload ||
                    !cached_identity_is_globally_unique(
                        slots, target, old_after,
                        "parked Graphics carrier") ||
                    !cached_identity_is_globally_unique(
                        slots, parking, parked,
                        "active Network parking panel") ||
                    (parking_before &&
                        (parked_panel != parking_before ||
                            parked.wrapper != parking_identity_before.wrapper ||
                            parked.payload != parking_identity_before.payload))) {
                    return reusable_fault(
                        "Network parking did not leave Graphics intact/inactive");
                }
                current = parked_panel;
                erui::detail::logf(erui::LogLevel::trace,
                    "Native-tab Graphics carrier parked frame=%llu route=%zu category=5 panel=%p wrapper=%p payload=%p cached=%d currentExact=1 graphicsInactive=1 stateCopySuppressed=1 synchronous=1",
                    static_cast<unsigned long long>(completed_frame), route,
                    parked_panel, parked.wrapper, parked.payload,
                    parking_before != nullptr);
            }

            if (old_panel == *reinterpret_cast<void**>(bytes + 0xB8) ||
                *reinterpret_cast<void**>(bytes + 0xB8) != current) {
                return reusable_fault(
                    "refused retirement because Graphics was current/current changed");
            }
            void* const parking_pre_retire = slots[parking];
            const auto parking_pre_identity = inspect_tab_carrier(
                parking_pre_retire, parking,
                erui::detail::invalid_compiled_index);
            if (parking_pre_retire &&
                (!parking_pre_identity.valid ||
                    !cached_identity_is_globally_unique(
                        slots, parking, parking_pre_identity,
                        "pre-retirement Network parking panel"))) {
                return reusable_fault(
                    "Network parking identity was not globally unique before retirement");
            }
            clear_retired_page_references(old_panel);
            reinterpret_cast<DeactivateFn>(deactivate)(old_panel, false);
            if (*reinterpret_cast<void**>(bytes + 0xB8) != current ||
                slots[target] != old_panel) {
                return reusable_fault(
                    "current/cache changed during Graphics deactivation");
            }
            {
                bool queue_returned{};
#if defined(_MSC_VER)
                __try {
#endif
                    reinterpret_cast<TabCarrierQueueFn>(queue)(
                        manager, old_panel);
                    queue_returned = true;
#if defined(_MSC_VER)
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    // Ownership may already have transferred. Clear the sole
                    // cache owner before failing so native teardown cannot
                    // queue the same panel a second time.
                    slots[target] = nullptr;
                }
#endif
                const bool post_current_unchanged =
                    *reinterpret_cast<void**>(bytes + 0xB8) == current;
                const bool post_graphics_pointer_unchanged =
                    slots[target] == old_panel;
                const bool post_network_pointer_unchanged =
                    slots[parking] == parking_pre_retire;
                // From this point the allocator owns old_panel. Never inspect
                // or dereference it again through slot 8.
                slots[target] = nullptr;
                g_tab_carrier = {};
                ++g_tab_carrier_epoch;
                if (g_tab_carrier_epoch == 0) g_tab_carrier_epoch = 1;
                if (!queue_returned || !post_current_unchanged ||
                    !post_graphics_pointer_unchanged ||
                    !post_network_pointer_unchanged) {
                    return reusable_fault(
                        "post-queue raw cache/current invariants differed; slot 8 was cleared to prevent double retirement");
                }
                erui::detail::logf(erui::LogLevel::trace,
                    "Native-tab Graphics carrier retired frame=%llu activationFrame=%llu ageFrames=%llu priorRoute=%zu nextRoute=%zu old=%p/%p/%p active=%p manager=%p managerVtable=%p deactivate=%p queue=%p allocatorExact=1 slot8Null=%d currentUntouched=%d networkUntouched=1",
                    static_cast<unsigned long long>(completed_frame),
                    static_cast<unsigned long long>(
                        g_reusable_activation_frame),
                    static_cast<unsigned long long>(
                        completed_frame - g_reusable_activation_frame),
                    g_reusable_route, route, old_panel, observed.wrapper,
                    observed.payload, current, manager, manager_vtable,
                    deactivate, queue, slots[target] == nullptr,
                    post_current_unchanged);
            }
        } else if (g_tab_carrier.committed) {
            return reusable_fault(
                "empty Graphics cache retained committed identity");
        }

        g_reusable_route = route;
        g_reusable_activation_frame = completed_frame;
        *category = target;
        erui::detail::logf(erui::LogLevel::trace,
            "Native-tab Graphics carrier prepared frame=%llu route=%zu kind=%s current=%p slot8Empty=%d",
            static_cast<unsigned long long>(completed_frame), route,
            route == native_graphics_route() ? "native-graphics" : "custom",
            current, slots[target] == nullptr);
        return true;
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return reusable_fault(
            "SEH during Graphics staging/retirement");
    }
#endif
}

bool verify_graphics_carrier_v2_impl(
    void* composite,
    std::size_t route,
    std::int32_t category) noexcept {
    if (g_reusable_failed.load(std::memory_order_acquire) ||
        composite != g_reusable_composite ||
        route >= graphics_route_count() || category != 8) return false;
#if defined(_MSC_VER)
    __try {
#endif
        const auto& identity = g_tab_carrier;
        const auto observed = inspect_tab_carrier(
            identity.panel, 8, identity.logical_page_index);
        const auto panel = inspect_builtin_panel(identity.panel);
        auto* const bytes = static_cast<std::byte*>(composite);
        const bool exact = identity.committed &&
            g_reusable_route == route && observed.valid && panel.valid &&
            observed.wrapper == identity.wrapper &&
            observed.payload == identity.payload &&
            *reinterpret_cast<void**>(bytes + 0xB8) == identity.panel &&
            *reinterpret_cast<void**>(bytes + 0x68 + 8 * 8) ==
                identity.panel &&
            cached_identity_is_globally_unique(
                reinterpret_cast<void**>(bytes + 0x68), 8, observed,
                "activated Graphics carrier");
        erui::detail::logf(
            exact ? erui::LogLevel::trace : erui::LogLevel::error,
            "Native-tab Graphics carrier activated route=%zu kind=%s category=8 panel=%p wrapper=%p payload=%p rows=%llu committed=%d currentExact=%d",
            route,
            route == native_graphics_route() ? "native-graphics" : "custom",
            identity.panel, identity.wrapper, identity.payload,
            static_cast<unsigned long long>(panel.native_count),
            identity.committed, exact);
        return exact || reusable_fault(
            "activation did not commit requested Graphics route");
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return reusable_fault("SEH during Graphics activation verification");
    }
#endif
}


void log_tab_content_route_fault(const char* message) noexcept {
    const std::uint32_t event =
        g_tab_content_route_fault_log_count.fetch_add(
            1, std::memory_order_relaxed) + 1;
    if (event <= 8) {
        erui::detail::logf(erui::LogLevel::error,
            "Native-tab Graphics-carrier route fault[%u]: %s",
            static_cast<unsigned>(event), message);
    }
}

void begin_tab_content_category8_route(
    safetyhook::Context& context) noexcept {
    if (!g_tab_content_callbacks_armed.load(std::memory_order_acquire) ||
        g_reusable_failed.load(std::memory_order_acquire)) {
        // A sticky carrier fault must degrade to Elden Ring's original
        // Graphics construction. Replacing the callback and then letting the
        // host materializer reject it would otherwise create an empty page.
        return;
    }
    const std::uintptr_t image = reinterpret_cast<std::uintptr_t>(
        g_addresses.game_image_base);
    const TabCarrierBuildProfile* const profile =
        g_tab_carrier_build_profile;
    if (!image || !profile || profile->category8_materializer_rva >=
            g_addresses.game_image_size) {
        log_tab_content_route_fault("invalid image bounds");
        return;
    }
    bool replaced{};
    bool secondary_zeroed{};
    std::uint8_t observed_selector{};
    std::uintptr_t observed_primary_slot{};
    std::uintptr_t observed_secondary_slot{};
    std::uint8_t observed_enabled{};
    std::uint8_t observed_reserved{};
    std::uintptr_t observed_primary_callback{};
    std::uint32_t observed_primary_metadata{};
    std::uintptr_t observed_secondary_callback{};
    std::uint32_t observed_secondary_metadata{};
#if defined(_MSC_VER)
    __try {
#endif
        auto* const stack = reinterpret_cast<std::byte*>(context.rsp);
        observed_selector =
            *reinterpret_cast<const std::uint8_t*>(stack + 0x20);
        observed_primary_slot =
            *reinterpret_cast<const std::uintptr_t*>(stack + 0x28);
        observed_secondary_slot =
            *reinterpret_cast<const std::uintptr_t*>(stack + 0x30);
        observed_enabled =
            *reinterpret_cast<const std::uint8_t*>(stack + 0x38);
        observed_reserved =
            *reinterpret_cast<const std::uint8_t*>(stack + 0x40);
        auto* const secondary_callback =
            reinterpret_cast<std::uintptr_t*>(
            stack + 0x50);
        auto* const secondary_metadata =
            reinterpret_cast<std::uint32_t*>(stack + 0x58);
        auto* const primary_callback =
            reinterpret_cast<std::uintptr_t*>(
            stack + 0x60);
        auto* const primary_metadata =
            reinterpret_cast<std::uint32_t*>(stack + 0x68);
        observed_primary_callback = *primary_callback;
        observed_primary_metadata = *primary_metadata;
        observed_secondary_callback = *secondary_callback;
        observed_secondary_metadata = *secondary_metadata;
        // Both callback descriptors occupy 16 stack bytes, but the wrapper
        // initializes only an eight-byte callback and four-byte metadata
        // field. The final four bytes are padding and must neither be read as
        // metadata nor mutated.
        const bool exact = observed_selector == 11 &&
            observed_primary_slot == context.rsp + 0x60 &&
            observed_secondary_slot == context.rsp + 0x50 &&
            observed_enabled == 1 && observed_reserved == 0 &&
            observed_primary_callback ==
                image + profile->category8_materializer_rva &&
            observed_primary_metadata == 0 &&
            observed_secondary_callback ==
                image + profile->category8_extra_callback_rva &&
            observed_secondary_metadata == 0;
        if (exact) {
            *primary_callback = reinterpret_cast<std::uintptr_t>(
                &tab_content_category8_materializer);
            if (g_reusable_route != native_graphics_route()) {
                // Selector 11 invokes this callback after row materialization.
                // A custom route must use the generic post-init branch.
                *secondary_callback = 0;
                *secondary_metadata = 0;
                secondary_zeroed = true;
            }
            replaced = true;
        }
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        log_tab_content_route_fault("SEH reading callback locals");
        return;
    }
#endif
    if (!replaced) {
        const std::uint32_t event =
            g_tab_content_route_fault_log_count.fetch_add(
                1, std::memory_order_relaxed) + 1;
        if (event <= 8) {
            erui::detail::logf(erui::LogLevel::error,
                "Native-tab Graphics-carrier route fault[%u]: unexpected selector/callback stack-local contract observed={selector:%u primarySlot:%p secondarySlot:%p enabled:%u reserved:%u primary:%p/0x%08X secondary:%p/0x%08X}",
                static_cast<unsigned>(event),
                static_cast<unsigned>(observed_selector),
                reinterpret_cast<void*>(observed_primary_slot),
                reinterpret_cast<void*>(observed_secondary_slot),
                static_cast<unsigned>(observed_enabled),
                static_cast<unsigned>(observed_reserved),
                reinterpret_cast<void*>(observed_primary_callback),
                static_cast<unsigned>(observed_primary_metadata),
                reinterpret_cast<void*>(observed_secondary_callback),
                static_cast<unsigned>(observed_secondary_metadata));
        }
        return;
    }
    const std::uint32_t event =
        g_tab_content_route_log_count.fetch_add(
            1, std::memory_order_relaxed) + 1;
    if (event <= 32) {
        erui::detail::logf(erui::LogLevel::trace,
            "Native-tab Graphics route begin[%u]: route=%zu kind=%s selector=11 primary=host-thunk secondary=%s",
            static_cast<unsigned>(event), g_reusable_route,
            g_reusable_route == native_graphics_route()
                ? "native-graphics" : "custom",
            secondary_zeroed ? "zero/generic-post-init" :
                "native-authored");
    }
}

void materialize_native_graphics(void* page) noexcept {
    const std::uintptr_t image = reinterpret_cast<std::uintptr_t>(
        g_addresses.game_image_base);
    const TabCarrierBuildProfile* const profile =
        g_tab_carrier_build_profile;
    if (!page || !image || !profile ||
        profile->category8_materializer_rva >=
            g_addresses.game_image_size) return;
    reinterpret_cast<BuiltinPanelMaterializerFn>(
        image + profile->category8_materializer_rva)(
            page, static_cast<std::byte*>(page) + 0x1B38);
}

void __fastcall tab_content_category8_materializer(
    void* page,
    void* untrusted_argument2) noexcept {
    (void)untrusted_argument2;
    if (!page || g_reusable_failed.load(std::memory_order_acquire) ||
        g_tab_carrier.committed) {
        reusable_fault(
            "materialization occurred without exact prior Graphics retirement");
        return;
    }
    if (g_tab_carrier_epoch == 0) g_tab_carrier_epoch = 1;

    const BuiltinPanelSnapshot before = inspect_builtin_panel(page);
    const TabCarrierIdentity initial = inspect_tab_carrier(
        page, 8, erui::detail::invalid_compiled_index);
    if (!before.valid || before.native_count != 0 || !initial.valid) {
        reusable_fault("fresh Graphics panel failed pre-mutation identity");
        return;
    }

    if (g_reusable_route == native_graphics_route()) {
        materialize_native_graphics(page);
        const BuiltinPanelSnapshot after = inspect_builtin_panel(page);
        TabCarrierIdentity identity = inspect_tab_carrier(
            page, 8, erui::detail::invalid_compiled_index);
        const bool committed = after.valid && after.native_count > 0 &&
            after.native_count <= after.visual_capacity && identity.valid &&
            identity.wrapper == initial.wrapper &&
            identity.payload == initial.payload;
        if (committed) {
            identity.committed = true;
            g_tab_carrier = identity;
        }
        erui::detail::logf(
            committed ? erui::LogLevel::trace : erui::LogLevel::error,
            "Native-tab Graphics carrier construct: epoch=%llu route=%zu kind=native-graphics page=%p wrapper=%p payload=%p before=%llu after=%llu visualRows=%u committed=%d extraCallback=native",
            static_cast<unsigned long long>(g_tab_carrier_epoch),
            g_reusable_route,
            page, identity.wrapper, identity.payload,
            static_cast<unsigned long long>(before.native_count),
            static_cast<unsigned long long>(after.native_count),
            static_cast<unsigned>(after.visual_capacity),
            committed ? 1 : 0);
        if (!committed) reusable_fault("native Graphics restoration failed");
        return;
    }

    erui::detail::RuntimeState& runtime = erui::detail::runtime_state();
    if (!runtime.options.enable_row_injection || !runtime.menu ||
        runtime.menu->tab_page_indices.empty() ||
        g_reusable_route >= runtime.menu->tab_page_indices.size()) {
        reusable_fault("custom Graphics route had no compiled tab page");
        return;
    }
    const std::size_t logical_page_index =
        runtime.menu->tab_page_indices[g_reusable_route];
    if (logical_page_index >= runtime.menu->pages.size() ||
        runtime.menu->pages[logical_page_index].kind != erui::PageKind::tab) {
        reusable_fault("custom Graphics route resolved invalid tab page");
        return;
    }
    const std::size_t logical_rows =
        runtime.menu->pages[logical_page_index].rows.size();
    const erui::detail::PageRoute route =
        erui::detail::PageRoute::tab_main(
            logical_page_index,
            static_cast<std::uint8_t>(before.visual_capacity));
    erui::detail::PageSlice expected_slice{};
    if (!resolve_page_route_slice(*runtime.menu, route, expected_slice)) {
        reusable_fault("custom Graphics route slice could not resolve");
        return;
    }
    const std::size_t expected_native_rows = expected_slice.row_count +
        (expected_slice.has_previous ? 1u : 0u) +
        (expected_slice.has_next ? 1u : 0u);
    if (expected_native_rows > before.visual_capacity) {
        reusable_fault("custom rows exceeded Graphics capacity");
        return;
    }
    const RowInjectionOutcome outcome = inject_page_route(
        page, route, g_addresses);
    const BuiltinPanelSnapshot after = inspect_builtin_panel(page);
    TabCarrierIdentity identity = inspect_tab_carrier(
        page, 8, logical_page_index);
    const bool identity_stable = identity.valid &&
        identity.wrapper == initial.wrapper &&
        identity.payload == initial.payload;
    const bool committed = after.valid && outcome.faulted == 0 &&
        outcome.skipped == 0 && outcome.attempted == expected_native_rows &&
        outcome.added == expected_native_rows &&
        after.native_count == expected_native_rows &&
        after.native_count <= after.visual_capacity && identity_stable;
    if (committed) {
        identity.committed = true;
        g_tab_carrier = identity;
        // Publish dialog/Back ownership only after the carrier identity and
        // complete row injection have committed. A failed materialization
        // must never leave input routed to an unowned category-8 page.
        g_dialog_input_page.store(page, std::memory_order_release);
    }
    erui::detail::logf(
        committed ? erui::LogLevel::trace : erui::LogLevel::error,
        "Native-tab Graphics carrier construct: epoch=%llu route=%zu kind=custom page=%p wrapper=%p payload=%p logicalPage=%zu before=%llu after=%llu visualRows=%u logicalRows=%zu attempted=%u added=%u faulted=%u skipped=%u identityStable=%d committed=%d extraCallback=zero/generic-post-init",
        static_cast<unsigned long long>(g_tab_carrier_epoch),
        g_reusable_route, page, identity.wrapper, identity.payload,
        logical_page_index,
        static_cast<unsigned long long>(before.native_count),
        static_cast<unsigned long long>(after.native_count),
        static_cast<unsigned>(before.visual_capacity), logical_rows,
        static_cast<unsigned>(outcome.attempted),
        static_cast<unsigned>(outcome.added),
        static_cast<unsigned>(outcome.faulted),
        static_cast<unsigned>(outcome.skipped),
        identity_stable ? 1 : 0, committed ? 1 : 0);
    if (!committed) reusable_fault("custom Graphics materialization failed");
}

void __fastcall tab_content_composite_teardown_detour(
    void* composite) noexcept {
    const auto original = g_original_tab_content_composite_teardown.load(
        std::memory_order_acquire);
    if (!original) return;
    if (!g_tab_content_callbacks_armed.load(std::memory_order_acquire)) {
        original(composite);
        return;
    }
    TabCarrierIdentity category8{};
    bool slot_read{};
    if (composite) {
#if defined(_MSC_VER)
        __try {
#endif
            void* const panel = *reinterpret_cast<void**>(
                static_cast<std::byte*>(composite) + 0x68 + 8 * 8);
            category8 = inspect_tab_carrier(
                panel,
                8, g_tab_carrier.logical_page_index);
            slot_read = true;
#if defined(_MSC_VER)
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
#endif
    }
    const bool tracked = g_tab_carrier.committed;
    const bool exact_tracked = tracked && slot_read && category8.valid &&
        composite == g_reusable_composite &&
        category8.panel == g_tab_carrier.panel &&
        category8.wrapper == g_tab_carrier.wrapper &&
        category8.payload == g_tab_carrier.payload;
    const bool clean_empty = !tracked && slot_read && !category8.panel &&
        (!g_reusable_composite || composite == g_reusable_composite);
    if (category8.panel) {
        clear_retired_page_references(category8.panel);
    }
    original(composite);
    bool cleared{};
#if defined(_MSC_VER)
    __try {
#endif
        cleared = composite && !*reinterpret_cast<void**>(
            static_cast<std::byte*>(composite) + 0x68 + 8 * 8);
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
#endif
    const bool exact_teardown = cleared &&
        (exact_tracked || clean_empty);
    erui::detail::logf(
        exact_teardown ? erui::LogLevel::trace : erui::LogLevel::error,
        "Native-tab Graphics carrier native teardown: epoch=%llu composite=%p route=%zu category8=%p/%p/%p tracked=%d exactTracked=%d cleanEmpty=%d slot8Null=%d networkRetiredByHost=0",
        static_cast<unsigned long long>(g_tab_carrier_epoch),
        composite, g_reusable_route, category8.panel, category8.wrapper,
        category8.payload, tracked ? 1 : 0, exact_tracked ? 1 : 0,
        clean_empty ? 1 : 0, cleared ? 1 : 0);
    if (exact_teardown) {
        g_tab_carrier = {};
        g_reusable_route = native_graphics_route();
        g_reusable_composite = nullptr;
        g_reusable_activation_frame = 0;
        // Exact native destruction is the sole recovery boundary for a
        // sticky carrier fault. Until this point category 8 may still cache a
        // custom or partially constructed panel and must not be treated as
        // native Graphics.
        g_reusable_failed.store(false, std::memory_order_release);
        g_tab_content_teardown_generation.fetch_add(
            1, std::memory_order_acq_rel);
        ++g_tab_carrier_epoch;
        if (g_tab_carrier_epoch == 0) {
            g_tab_carrier_epoch = 1;
        }
    } else {
        (void)reusable_fault(
            "native teardown did not match the tracked Graphics identity; retained diagnostics were not reset");
    }
}

#endif

void inject_builtin_panel_rows(
    void* page,
    const char* panel_name,
    std::uint8_t native_category) noexcept {
    const std::uint16_t observer_bit = static_cast<std::uint16_t>(
        1u << native_category);
    if ((g_builtin_panel_observer_only_mask.load(
             std::memory_order_acquire) & observer_bit) != 0) {
        return;
    }
    erui::detail::RuntimeState& runtime = erui::detail::runtime_state();
    if (!runtime.options.enable_row_injection || !runtime.menu) return;
    const std::size_t logical_page_index =
        runtime.menu->builtin_page_index(native_category);
    if (logical_page_index >= runtime.menu->pages.size() ||
        runtime.menu->pages[logical_page_index].rows.empty()) {
        return;
    }

    const BuiltinPanelSnapshot before = inspect_builtin_panel(page);
    g_dialog_input_page.store(page, std::memory_order_release);
    if (!before.valid || before.native_count >= before.visual_capacity) {
        const std::uint16_t bit = static_cast<std::uint16_t>(
            1u << native_category);
        const bool first_warning =
            (g_builtin_panel_warning_mask.fetch_or(
                bit, std::memory_order_relaxed) & bit) == 0;
        if (!first_warning) return;
        erui::detail::logf(
            erui::LogLevel::warning,
            "Built-in %s rows disabled safely: page=%p nativeRows=%llu visualRows=%u valid=%d (the GFX panel has no free row capacity or its layout is unsupported)",
            panel_name,
            page,
            static_cast<unsigned long long>(before.native_count),
            static_cast<unsigned>(before.visual_capacity),
            before.valid ? 1 : 0);
        return;
    }

    const std::uint8_t free_capacity = static_cast<std::uint8_t>(
        before.visual_capacity - before.native_count);
    const erui::detail::PageRoute route =
        erui::detail::PageRoute::builtin_main(
            logical_page_index,
            native_category,
            free_capacity);
    const RowInjectionOutcome outcome = inject_page_route(
        page,
        route,
        g_addresses);
    const BuiltinPanelSnapshot after = inspect_builtin_panel(page);
    const bool committed = after.valid && outcome.faulted == 0 &&
        after.native_count == before.native_count + outcome.added;
    if (!committed || runtime.options.enable_diagnostics) {
        erui::detail::logf(
            committed ? erui::LogLevel::trace : erui::LogLevel::error,
            "built-in %s materializer: page=%p logicalPage=%zu before=%llu after=%llu visualRows=%u freeRows=%u slice=%zu/%zu attempted=%u added=%u faulted=%u committed=%d",
            panel_name,
            page,
            logical_page_index,
            static_cast<unsigned long long>(before.native_count),
            static_cast<unsigned long long>(after.native_count),
            static_cast<unsigned>(before.visual_capacity),
            static_cast<unsigned>(free_capacity),
            outcome.slice_index + 1,
            outcome.slice_count,
            static_cast<unsigned>(outcome.attempted),
            static_cast<unsigned>(outcome.added),
            static_cast<unsigned>(outcome.faulted),
            committed ? 1 : 0);
    }
}

template <typename Function>
Function wait_for_panel_materializer(
    const std::atomic<Function>& slot) noexcept {
    Function original = slot.load(std::memory_order_acquire);
    for (unsigned attempt = 0; !original && attempt < 64; ++attempt) {
        Sleep(0);
        original = slot.load(std::memory_order_acquire);
    }
    return original;
}

void __fastcall camera_panel_materializer_detour(
    void* page,
    void* menu_option_data) noexcept {
    BuiltinPanelMaterializerFn original = wait_for_panel_materializer(
        g_original_camera_panel_materializer);
    if (!original) return;
    DiagnosticNativeCategoryScope category_scope(1);
    log_native_panel_state(
        "Camera", 1, page, menu_option_data, "before-native");
    original(page, menu_option_data);
    log_native_panel_state(
        "Camera", 1, page, menu_option_data, "after-native-before-hub");
    inject_builtin_panel_rows(page, "Camera Options", 1);
    log_native_panel_state(
        "Camera", 1, page, menu_option_data, "after-hub-injection");
}

void __fastcall display_panel_materializer_detour(
    void* page,
    void* menu_option_data) noexcept {
    BuiltinPanelMaterializerFn original = wait_for_panel_materializer(
        g_original_display_panel_materializer);
    if (!original) return;
    DiagnosticNativeCategoryScope category_scope(2);
    log_native_panel_state(
        "Display", 2, page, menu_option_data, "before-native");
    original(page, menu_option_data);
    log_native_panel_state(
        "Display", 2, page, menu_option_data, "after-native-before-hub");
    inject_builtin_panel_rows(page, "Display", 2);
    log_native_panel_state(
        "Display", 2, page, menu_option_data, "after-hub-injection");
}

void __fastcall sound_panel_materializer_detour(
    void* page,
    void* menu_option_data) noexcept {
    BuiltinPanelMaterializerFn original = wait_for_panel_materializer(
        g_original_sound_panel_materializer);
    if (!original) return;
    DiagnosticNativeCategoryScope category_scope(3);
    log_native_panel_state(
        "Sound", 3, page, menu_option_data, "before-native");
    original(page, menu_option_data);
    log_native_panel_state(
        "Sound", 3, page, menu_option_data, "after-native-before-hub");
    inject_builtin_panel_rows(page, "Sound", 3);
    log_native_panel_state(
        "Sound", 3, page, menu_option_data, "after-hub-injection");
}

void __fastcall network_panel_materializer_detour(
    void* page,
    void* menu_option_data) noexcept {
    BuiltinPanelMaterializerFn original = wait_for_panel_materializer(
        g_original_network_panel_materializer);
    if (!original) return;
    DiagnosticNativeCategoryScope category_scope(5);
    log_native_panel_state(
        "Network", 5, page, menu_option_data, "before-native");
    original(page, menu_option_data);
    log_native_panel_state(
        "Network", 5, page, menu_option_data, "after-native-before-hub");
    inject_builtin_panel_rows(page, "Network", 5);
    log_native_panel_state(
        "Network", 5, page, menu_option_data, "after-hub-injection");
}

void __fastcall keyboard_mouse_panel_materializer_detour(
    void* page,
    void* menu_option_data) noexcept {
    BuiltinPanelMaterializerFn original = wait_for_panel_materializer(
        g_original_keyboard_mouse_panel_materializer);
    if (!original) return;
    DiagnosticNativeCategoryScope category_scope(7);
    log_native_panel_state(
        "Keyboard-Mouse", 7, page, menu_option_data, "before-native");
    original(page, menu_option_data);
    log_native_panel_state(
        "Keyboard-Mouse", 7, page, menu_option_data,
        "after-native-before-hub");
    inject_builtin_panel_rows(page, "Keyboard/Mouse", 7);
    log_native_panel_state(
        "Keyboard-Mouse", 7, page, menu_option_data, "after-hub-injection");
}

void __fastcall graphics_panel_materializer_detour(
    void* page,
    void* menu_option_data) noexcept {
    BuiltinPanelMaterializerFn original = wait_for_panel_materializer(
        g_original_graphics_panel_materializer);
    if (!original) return;
    DiagnosticNativeCategoryScope category_scope(8);
    log_native_panel_state(
        "Graphics", 8, page, menu_option_data, "before-native");
    original(page, menu_option_data);
    log_native_panel_state(
        "Graphics", 8, page, menu_option_data, "after-native-before-hub");
    inject_builtin_panel_rows(page, "Graphics", 8);
    log_native_panel_state(
        "Graphics", 8, page, menu_option_data, "after-hub-injection");
}

struct TopCategoryListSnapshot {
    std::uint64_t count{};
    std::array<std::uint32_t, kMaxDiagnosticCategoryCount> codes{};
    std::uint32_t mod_settings_count{};
    std::uint32_t duplicate_code_count{};
    bool valid{};
};

TopCategoryListSnapshot inspect_top_category_list_for_diagnostics(
    void* list) noexcept {
    TopCategoryListSnapshot result{};
    if (!list) return result;
#if defined(_MSC_VER)
    __try {
#endif
        const std::uintptr_t image =
            reinterpret_cast<std::uintptr_t>(g_addresses.game_image_base);
        const auto list_address = reinterpret_cast<std::uintptr_t>(list);
        if (!image ||
            option_setting_top_dialog_vtable_rva() >= g_addresses.game_image_size ||
            list_address < kOptionSettingTopCategoryListOffset) {
            return result;
        }
        const auto* const top = reinterpret_cast<const std::byte*>(
            list_address - kOptionSettingTopCategoryListOffset);
        if (*reinterpret_cast<void* const*>(top) != reinterpret_cast<void*>(
                image + option_setting_top_dialog_vtable_rva())) {
            return result;
        }
        const auto* const list_bytes = static_cast<const std::byte*>(list);
        result.count = *reinterpret_cast<const std::uint64_t*>(
            list_bytes + kOptionSettingCategoryCountOffset);
        if (result.count > kMaxDiagnosticCategoryCount) {
            result.count = 0;
            return result;
        }

        const std::uintptr_t first_record = list_address +
            ((std::uintptr_t{0} - list_address) & std::uintptr_t{7});
        for (std::uint64_t index = 0; index < result.count; ++index) {
            if (index > (static_cast<std::uintptr_t>(-1) - first_record) /
                    kOptionSettingCategoryRecordSize) {
                result = {};
                return result;
            }
            const auto* const record = reinterpret_cast<const std::byte*>(
                first_record + index * kOptionSettingCategoryRecordSize);
            const std::uint32_t code = *reinterpret_cast<const std::uint32_t*>(
                record + kOptionSettingCategoryCodeOffset);
            result.codes[index] = code;
            if (code == kModSettingsNativeCategory) {
                ++result.mod_settings_count;
            }
            for (std::uint64_t previous = 0; previous < index; ++previous) {
                if (result.codes[previous] == code) {
                    ++result.duplicate_code_count;
                    break;
                }
            }
        }
        result.valid = true;
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        result = {};
    }
#endif
    return result;
}

void log_top_category_list_snapshot(
    const char* phase,
    std::uint32_t event,
    void* list) noexcept {
    if (!erui::detail::diagnostics_enabled() ||
        event > kCategoryAppendDiagnosticLogLimit) return;

    const TopCategoryListSnapshot snapshot =
        inspect_top_category_list_for_diagnostics(list);
    char codes[96]{};
    std::size_t used = 0;
    if (snapshot.valid) {
        for (std::uint64_t index = 0; index < snapshot.count; ++index) {
            const int written = std::snprintf(
                codes + used,
                sizeof(codes) - used,
                "%s%u",
                index == 0 ? "" : ",",
                static_cast<unsigned>(snapshot.codes[index]));
            if (written <= 0 ||
                static_cast<std::size_t>(written) >= sizeof(codes) - used) {
                break;
            }
            used += static_cast<std::size_t>(written);
        }
    }
    erui::detail::logf(
        erui::LogLevel::info,
        "diagnostic: native category list[%u] phase=%s list=%p valid=%u count=%llu codes=%s hubCount=%u duplicateCodes=%u",
        static_cast<unsigned>(event),
        phase ? phase : "unknown",
        list,
        snapshot.valid ? 1u : 0u,
        static_cast<unsigned long long>(snapshot.count),
        snapshot.valid ? codes : "unreadable",
        static_cast<unsigned>(snapshot.mod_settings_count),
        static_cast<unsigned>(snapshot.duplicate_code_count));
}

const char* native_category_name(std::uint32_t code) noexcept {
    switch (code) {
    case 0: return "Game-Options";
    case 1: return "Camera";
    case 2: return "Display";
    case 3: return "Sound";
    case 4: return "Adjust-Brightness";
    case 5: return "Network";
    case 6: return "Controller";
    case 7: return "Keyboard-Mouse";
    case 8: return "Graphics";
    case 9: return "Quit-Game";
    default: return "unknown";
    }
}

struct NativeTabFrameSnapshot {
    void* tab_control{};
    void* selector{};
    void* manager{};
    void* active_panel{};
    void* panel_vtable{};
    std::uint32_t selected_index{
        std::numeric_limits<std::uint32_t>::max()};
    bool selector_read{};
    bool manager_read{};
    bool selected_index_read{};
    bool active_panel_read{};
    bool panel_vtable_read{};
};

NativeTabFrameSnapshot inspect_native_tab_frame(
    void* tab_control,
    TabSelectionIndexFn selected_index) noexcept {
    NativeTabFrameSnapshot result{};
    result.tab_control = tab_control;
    if (!tab_control || !selected_index) return result;
#if defined(_MSC_VER)
    __try {
#endif
        const auto* const control_bytes =
            static_cast<const std::byte*>(tab_control);
        result.selector = *reinterpret_cast<void* const*>(
            control_bytes + kOptionSettingTabSelectorOffset);
        result.selector_read = true;
        result.manager = *reinterpret_cast<void* const*>(
            control_bytes + kOptionSettingTabManagerOffset);
        result.manager_read = true;
        if (result.selector &&
            reinterpret_cast<std::uintptr_t>(result.selector) >= 0x10000) {
            result.selected_index = selected_index(result.selector);
            result.selected_index_read = true;
        }
        if (result.manager &&
            reinterpret_cast<std::uintptr_t>(result.manager) >= 0x10000) {
            const auto* const manager_bytes =
                static_cast<const std::byte*>(result.manager);
            result.active_panel = *reinterpret_cast<void* const*>(
                manager_bytes + kCompositeCurrentPanelOffset);
            result.active_panel_read = true;
        }
        if (result.active_panel &&
            reinterpret_cast<std::uintptr_t>(result.active_panel) >= 0x10000) {
            const auto* const panel_bytes =
                static_cast<const std::byte*>(result.active_panel);
            result.panel_vtable = *reinterpret_cast<void* const*>(panel_bytes);
            result.panel_vtable_read = true;
        }
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        result.selected_index_read = false;
        result.active_panel_read = false;
        result.panel_vtable_read = false;
    }
#endif
    return result;
}

constexpr std::size_t kMaxDiagnosticPadPropertyRows = 16;

struct DiagnosticPadPropertyRow {
    void* row_vtable{};
    void* controller{};
    void* controller_vtable{};
    void* bound_value{};
    bool row_vtable_read{};
    bool controller_read{};
    bool controller_vtable_read{};
    bool bound_value_read{};
};

struct DiagnosticPadPage {
    void* page_vtable{};
    std::uint64_t row_count{};
    bool page_vtable_read{};
    bool page_vtable_match{};
    bool row_count_read{};
    bool row_count_valid{};
    std::array<DiagnosticPadPropertyRow, kMaxDiagnosticPadPropertyRows> rows{};
};

DiagnosticPadPage inspect_pad_setting_page(void* page) noexcept {
    DiagnosticPadPage result{};
    if (!page || !g_addresses.game_image_base ||
        pad_setting_dialog_vtable_rva() >= g_addresses.game_image_size) {
        return result;
    }
    result.page_vtable_read = read_diagnostic_value(page, result.page_vtable);
    result.page_vtable_match = result.page_vtable_read &&
        result.page_vtable == g_addresses.game_image_base +
            pad_setting_dialog_vtable_rva();
    if (!result.page_vtable_match) return result;

    result.row_count_read = read_diagnostic_value(
        static_cast<const std::byte*>(page) + kPadPropertyCountOffset,
        result.row_count);
    result.row_count_valid = result.row_count_read &&
        result.row_count <= kMaxDiagnosticPadPropertyRows;
    if (!result.row_count_valid) return result;

    auto* const page_bytes = static_cast<const std::byte*>(page);
    for (std::uint64_t index = 0; index < result.row_count; ++index) {
        auto* const row = const_cast<std::byte*>(page_bytes) +
            kPadPropertyCollectionOffset +
            static_cast<std::size_t>(index) * kPadPropertyRowStride;
        auto& output = result.rows[static_cast<std::size_t>(index)];
        output.row_vtable_read = read_diagnostic_value(
            row, output.row_vtable);
        output.controller_read = read_diagnostic_value(
            row + kPadPropertyControllerOffset, output.controller);
        if (output.controller_read && output.controller) {
            output.controller_vtable_read = read_diagnostic_value(
                output.controller, output.controller_vtable);
            output.bound_value_read = read_diagnostic_value(
                static_cast<const std::byte*>(output.controller) +
                    kPadControllerBoundValueOffset,
                output.bound_value);
        }
    }
    return result;
}

void log_pad_setting_page(
    std::uint32_t transition,
    std::uint32_t menu_context,
    std::uint32_t selected_index,
    void* page,
    const DiagnosticPadPage& snapshot) noexcept {
    erui::detail::logf(
        erui::LogLevel::info,
        "diagnostic: controller page[%u] menuContext=%u selectedIndex=%u page=%p pageVtable=%p pageVtableRead=%u expectedPadVtableMatch=%u rowCount=%llu rowCountRead=%u rowCountValid=%u",
        static_cast<unsigned>(transition),
        static_cast<unsigned>(menu_context),
        static_cast<unsigned>(selected_index),
        page,
        snapshot.page_vtable,
        snapshot.page_vtable_read ? 1u : 0u,
        snapshot.page_vtable_match ? 1u : 0u,
        static_cast<unsigned long long>(snapshot.row_count),
        snapshot.row_count_read ? 1u : 0u,
        snapshot.row_count_valid ? 1u : 0u);

    const std::uintptr_t image = reinterpret_cast<std::uintptr_t>(
        g_addresses.game_image_base);
    const void* const expected_row_vtable =
        edit_property_vtable_rva() < g_addresses.game_image_size
        ? reinterpret_cast<const void*>(image + edit_property_vtable_rva())
        : nullptr;
    const void* const expected_controller_vtable =
        text_input_controller_vtable_rva() < g_addresses.game_image_size
        ? reinterpret_cast<const void*>(image + text_input_controller_vtable_rva())
        : nullptr;
    if (!snapshot.row_count_valid) return;
    auto* const page_bytes = static_cast<std::byte*>(page);
    for (std::uint64_t index = 0; index < snapshot.row_count; ++index) {
        const auto& row_snapshot = snapshot.rows[
            static_cast<std::size_t>(index)];
        const void* const row_address = page_bytes +
            kPadPropertyCollectionOffset +
            static_cast<std::size_t>(index) * kPadPropertyRowStride;
        erui::detail::logf(
            erui::LogLevel::trace,
            "diagnostic: controller property row[%u:%llu] row=%p rowVtable=%p rowVtableRead=%u expectedEditPropertyMatch=%u controller=%p controllerRead=%u controllerVtable=%p controllerVtableRead=%u expectedControllerMatch=%u boundValue=%p boundValueRead=%u",
            static_cast<unsigned>(transition),
            static_cast<unsigned long long>(index),
            row_address,
            row_snapshot.row_vtable,
            row_snapshot.row_vtable_read ? 1u : 0u,
            row_snapshot.row_vtable_read &&
                    row_snapshot.row_vtable == expected_row_vtable
                ? 1u : 0u,
            row_snapshot.controller,
            row_snapshot.controller_read ? 1u : 0u,
            row_snapshot.controller_vtable,
            row_snapshot.controller_vtable_read ? 1u : 0u,
            row_snapshot.controller_vtable_read &&
                    row_snapshot.controller_vtable ==
                        expected_controller_vtable
                ? 1u : 0u,
            row_snapshot.bound_value,
            row_snapshot.bound_value_read ? 1u : 0u);
    }
}

void observe_option_setting_tab_frame(safetyhook::Context& context) noexcept {
    if (!erui::detail::diagnostics_enabled() ||
        !g_addresses.option_setting_tab_selected_index) {
        return;
    }
    auto snapshot = inspect_native_tab_frame(
        reinterpret_cast<void*>(context.rcx),
        g_addresses.option_setting_tab_selected_index);
    if (!snapshot.selected_index_read) return;

    const bool changed_control = g_last_tab_control.exchange(
        snapshot.tab_control, std::memory_order_relaxed) != snapshot.tab_control;
    const bool changed_selector = g_last_tab_selector.exchange(
        snapshot.selector, std::memory_order_relaxed) != snapshot.selector;
    const bool changed_manager = g_last_tab_manager.exchange(
        snapshot.manager, std::memory_order_relaxed) != snapshot.manager;
    const bool changed_panel = g_last_tab_active_panel.exchange(
        snapshot.active_panel, std::memory_order_relaxed) != snapshot.active_panel;
    const bool changed_index = g_last_tab_selected_index.exchange(
        snapshot.selected_index, std::memory_order_relaxed) !=
        snapshot.selected_index;
    if (!(changed_control || changed_selector || changed_manager ||
          changed_panel || changed_index)) {
        return;
    }

    std::uint32_t category = std::numeric_limits<std::uint32_t>::max();
    bool category_list_valid = false;
    bool quit_game_present = false;
    std::uint64_t category_count = 0;
    const std::uintptr_t control_address =
        reinterpret_cast<std::uintptr_t>(snapshot.tab_control);
    if (control_address >= kOptionSettingTopTabControlOffset) {
        auto* const top = reinterpret_cast<std::byte*>(
            control_address - kOptionSettingTopTabControlOffset);
        auto* const list = top + kOptionSettingTopCategoryListOffset;
        const TopCategoryListSnapshot categories =
            inspect_top_category_list_for_diagnostics(list);
        category_list_valid = categories.valid;
        category_count = categories.count;
        if (categories.valid) {
            for (std::uint64_t index = 0; index < categories.count; ++index) {
                quit_game_present = quit_game_present ||
                    categories.codes[index] == 9;
            }
        }
        if (categories.valid && snapshot.selected_index < categories.count) {
            category = categories.codes[snapshot.selected_index];
        }
    }
    g_last_tab_selected_category.store(category, std::memory_order_relaxed);
    const std::uint32_t context_mode = !category_list_valid
        ? 0u
        : (quit_game_present ? 2u : 1u);
    g_last_tab_context.store(context_mode, std::memory_order_relaxed);
    g_last_tab_selection_tick.store(GetTickCount64(), std::memory_order_relaxed);

    const char* const context_name = context_mode == 2
        ? "in-game"
        : (context_mode == 1 ? "title-menu" : "unknown");
    BuiltinPanelSnapshot ordinary_panel{};
    bool ordinary_panel_snapshot = false;
    const std::uintptr_t image =
        reinterpret_cast<std::uintptr_t>(g_addresses.game_image_base);
    const void* const expected_option_panel_vtable = reinterpret_cast<void*>(
        image + option_setting_dialog_vtable_rva());
    if (snapshot.active_panel && snapshot.panel_vtable ==
            expected_option_panel_vtable &&
        option_setting_dialog_vtable_rva() < g_addresses.game_image_size) {
        ordinary_panel = inspect_builtin_panel(snapshot.active_panel);
        ordinary_panel_snapshot = ordinary_panel.fields_read;
    }

    DiagnosticPadPage pad_page{};
    bool pad_embedded_vtable_read = false;
    void* pad_embedded_vtable = nullptr;
    if (category == 6 && snapshot.active_panel &&
        snapshot.panel_vtable == reinterpret_cast<void*>(
            image + pad_setting_dialog_vtable_rva()) &&
        pad_setting_dialog_vtable_rva() < g_addresses.game_image_size) {
        pad_page = inspect_pad_setting_page(snapshot.active_panel);
        const auto* const panel_bytes =
            static_cast<const std::byte*>(snapshot.active_panel);
        pad_embedded_vtable_read = read_diagnostic_value(
            panel_bytes + kPadSettingDialogEmbeddedObjectOffset,
            pad_embedded_vtable);
    }

    const std::uint32_t event = g_tab_transition_count.fetch_add(
        1, std::memory_order_relaxed) + 1;
    if (event > kTabTransitionDiagnosticLogLimit) return;
    erui::detail::logf(
        erui::LogLevel::info,
        "diagnostic: native tab transition[%u] context=%s selectedIndex=%u category=%u categoryName=%s categoryListValid=%u categoryCount=%llu quitGamePresent=%u control=%p selector=%p manager=%p activePanel=%p panelVtable=%p selectorRead=%u managerRead=%u panelRead=%u panelFieldsRead=%u panelRowsAt1AF0=%llu panelRawB14=%u panelFieldD84=%u panelFieldD88=%u padPageVtableMatch=%u padRowCount=%llu padRowCountRead=%u padEmbeddedVtable=%p padEmbeddedVtableRead=%u",
        static_cast<unsigned>(event),
        context_name,
        static_cast<unsigned>(snapshot.selected_index),
        static_cast<unsigned>(category),
        native_category_name(category),
        category_list_valid ? 1u : 0u,
        static_cast<unsigned long long>(category_count),
        quit_game_present ? 1u : 0u,
        snapshot.tab_control,
        snapshot.selector,
        snapshot.manager,
        snapshot.active_panel,
        snapshot.panel_vtable,
        snapshot.selector_read ? 1u : 0u,
        snapshot.manager_read ? 1u : 0u,
        snapshot.active_panel_read ? 1u : 0u,
        ordinary_panel_snapshot ? 1u : 0u,
        static_cast<unsigned long long>(ordinary_panel.native_count),
        static_cast<unsigned>(ordinary_panel.visual_capacity),
        static_cast<unsigned>(ordinary_panel.observed_field_d84),
        static_cast<unsigned>(ordinary_panel.observed_field_d88),
        pad_page.page_vtable_match ? 1u : 0u,
        static_cast<unsigned long long>(pad_page.row_count),
        pad_page.row_count_read ? 1u : 0u,
        pad_embedded_vtable,
        pad_embedded_vtable_read ? 1u : 0u);
    if (category == 6) {
        log_pad_setting_page(
            event,
            context_mode,
            snapshot.selected_index,
            snapshot.active_panel,
            pad_page);
    }
}

void install_option_setting_tab_observer(
    const GameAddresses& addresses) noexcept {
    if (!erui::detail::diagnostics_enabled()) return;
    if (!addresses.option_setting_tab_frame_dispatch ||
        !addresses.option_setting_tab_selected_index) {
        erui::detail::logf(
            erui::LogLevel::warning,
            "Configuration tab transition observer unavailable: exact-build selector interfaces did not resolve");
        return;
    }
    auto result = SafetyHookMid::create(
        addresses.option_setting_tab_frame_dispatch,
        &observe_option_setting_tab_frame);
    if (!result) {
        erui::detail::logf(
            erui::LogLevel::warning,
            "Configuration tab transition observer could not be installed (SafetyHook error type %u)",
            static_cast<unsigned>(result.error().type));
        return;
    }
    g_option_setting_tab_frame_hook = std::move(*result);
    erui::detail::logf(
        erui::LogLevel::info,
        "Hook installed: read-only Configuration tab transition observer (logs only on selected-index or active-panel changes)");
}

bool inspect_top_category_list(
    void* list,
    std::uint64_t& count,
    bool& contains_mod_settings) noexcept {
    count = 0;
    contains_mod_settings = false;
    if (!list) return false;
#if defined(_MSC_VER)
    __try {
#endif
        const std::uintptr_t image =
            reinterpret_cast<std::uintptr_t>(g_addresses.game_image_base);
        if (!image ||
            option_setting_top_dialog_vtable_rva() >= g_addresses.game_image_size) {
            return false;
        }
        const auto list_address = reinterpret_cast<std::uintptr_t>(list);
        if (list_address < kOptionSettingTopCategoryListOffset) return false;
        const auto top = reinterpret_cast<const std::byte*>(
            list_address - kOptionSettingTopCategoryListOffset);
        if (*reinterpret_cast<void* const*>(top) != reinterpret_cast<void*>(
                image + option_setting_top_dialog_vtable_rva())) {
            return false;
        }
        const auto* const list_bytes = static_cast<const std::byte*>(list);
        count = *reinterpret_cast<const std::uint64_t*>(
            list_bytes + kOptionSettingCategoryCountOffset);
        if (count > kMaxDiagnosticCategoryCount) return false;

        const std::uintptr_t first_record = list_address +
            ((std::uintptr_t{0} - list_address) & std::uintptr_t{7});
        for (std::uint64_t index = 0; index < count; ++index) {
            const auto* const record = reinterpret_cast<const std::byte*>(
                first_record + index * kOptionSettingCategoryRecordSize);
            const std::uint32_t code = *reinterpret_cast<const std::uint32_t*>(
                record + kOptionSettingCategoryCodeOffset);
            if (code == kModSettingsNativeCategory) {
                contains_mod_settings = true;
                break;
            }
        }
        return true;
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        count = 0;
        contains_mod_settings = false;
        return false;
    }
#endif
}

void* build_mod_settings_category_record(
    void* destination) noexcept {
    if (!destination || !g_addresses.mod_settings_category_builder) {
        return nullptr;
    }
#if defined(_MSC_VER)
    __try {
        return g_addresses.mod_settings_category_builder(destination);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
#else
    return g_addresses.mod_settings_category_builder(destination);
#endif
}

void destroy_mod_settings_category_record(void* value) noexcept {
    if (!value || !g_addresses.mod_settings_category_destructor) return;
#if defined(_MSC_VER)
    __try {
        g_addresses.mod_settings_category_destructor(value);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        erui::detail::logf(
            erui::LogLevel::warning,
            "Native Mod Settings category cleanup faulted; the category list remains active");
    }
#else
    g_addresses.mod_settings_category_destructor(value);
#endif
}

void try_insert_mod_settings_category(
    void* list,
    ConfigurationCategoryAppendFn original,
    std::uint32_t event,
    bool diagnostics) noexcept {
    if (!original ||
        !g_mod_settings_tab_hooks_ready.load(std::memory_order_acquire) ||
        !g_addresses.mod_settings_category_builder ||
        !g_addresses.mod_settings_category_destructor) {
        if (diagnostics && event <= 64) {
            erui::detail::logf(
                erui::LogLevel::info,
                "diagnostic: native Mod Settings insertion skipped event=%u phase=prerequisite-check hooksReady=%u",
                static_cast<unsigned>(event),
                g_mod_settings_tab_hooks_ready.load(std::memory_order_acquire)
                    ? 1u
                    : 0u);
        }
        return;
    }

    std::uint64_t count{};
    bool contains_mod_settings{};
    const bool valid_list =
        inspect_top_category_list(list, count, contains_mod_settings);
    if (!valid_list && !option_setting_top_dialog_vtable_rva()) {
        // Other category lists also pass through here, so only a missing
        // profile (the options dialog can never be recognised) is reported.
        static std::atomic_bool warned{};
        if (!warned.exchange(true, std::memory_order_acq_rel)) {
            erui::detail::logf(
                erui::LogLevel::warning,
                "Mod Settings category cannot be inserted: no options-dialog address profile for this game build");
        }
    }
    if (!valid_list || contains_mod_settings) {
        if (diagnostics && event <= 64) {
            erui::detail::logf(
                erui::LogLevel::info,
                "diagnostic: native Mod Settings insertion skipped event=%u phase=list-check valid=%u containsHub=%u count=%llu",
                static_cast<unsigned>(event),
                valid_list ? 1u : 0u,
                contains_mod_settings ? 1u : 0u,
                static_cast<unsigned long long>(count));
            log_top_category_list_snapshot(
                "hub-insertion-skipped", event, list);
        }
        return;
    }
    if (count > 8) {
        erui::detail::logf(
            erui::LogLevel::warning,
            "Mod Settings category was not inserted: native category list did not match the validated top-dialog layout (count=%llu)",
            static_cast<unsigned long long>(count));
        return;
    }

    alignas(16) std::array<std::byte,
        kOptionSettingCategoryRecordSize> record{};
    if (diagnostics && event <= kCategoryAppendDiagnosticLogLimit) {
        erui::detail::logf(
            erui::LogLevel::info,
                "diagnostic: Configuration category append[%u] building category 4 at the end of the category list (count=%llu)",
            static_cast<unsigned>(event),
            static_cast<unsigned long long>(count));
    }
    void* built = build_mod_settings_category_record(record.data());
    bool valid_record = false;
    if (built == record.data()) {
#if defined(_MSC_VER)
        __try {
#endif
            valid_record = *reinterpret_cast<const std::uint32_t*>(
                record.data() + kOptionSettingCategoryCodeOffset) ==
                kModSettingsNativeCategory;
#if defined(_MSC_VER)
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            valid_record = false;
        }
#endif
    }
    if (diagnostics && event <= kCategoryAppendDiagnosticLogLimit) {
        erui::detail::logf(
            erui::LogLevel::info,
            "diagnostic: Configuration category append[%u] builder returned=%p validCategory4=%u",
            static_cast<unsigned>(event),
            built,
            valid_record ? 1u : 0u);
    }
    if (valid_record) {
        if (diagnostics && event <= kCategoryAppendDiagnosticLogLimit) {
            log_top_category_list_snapshot(
                "before-hub-append", event, list);
        }
        if (diagnostics && event <= kCategoryAppendDiagnosticLogLimit) {
            erui::detail::logf(
                erui::LogLevel::info,
                "diagnostic: Configuration category append[%u] calling native append for Hub record=%p",
                static_cast<unsigned>(event),
                record.data());
        }
        original(list, record.data());
        if (diagnostics && event <= kCategoryAppendDiagnosticLogLimit) {
            log_top_category_list_snapshot(
                "after-hub-append", event, list);
        }
        if (diagnostics && event <= kCategoryAppendDiagnosticLogLimit) {
            erui::detail::logf(
                erui::LogLevel::info,
                "diagnostic: Configuration category append[%u] native append returned for Hub record",
                static_cast<unsigned>(event));
        }
        const std::uint32_t previous =
            g_mod_settings_category_insertions.fetch_add(
                1, std::memory_order_relaxed);
        if (previous == 0 || erui::detail::diagnostics_enabled()) {
        erui::detail::logf(
            erui::LogLevel::info,
            "Native Mod Settings category appended after Graphics (insertion %u)",
                previous + 1);
        }
    } else {
        erui::detail::logf(
            erui::LogLevel::warning,
            "Mod Settings category was not inserted: native category-4 builder did not match the expected record");
    }
    if (built) {
        if (diagnostics && event <= kCategoryAppendDiagnosticLogLimit) {
            erui::detail::logf(
                erui::LogLevel::info,
                "diagnostic: Configuration category append[%u] destroying temporary Hub category record=%p",
                static_cast<unsigned>(event),
                built);
        }
        destroy_mod_settings_category_record(built);
        if (diagnostics && event <= kCategoryAppendDiagnosticLogLimit) {
            erui::detail::logf(
                erui::LogLevel::info,
                "diagnostic: Configuration category append[%u] temporary Hub category record destroyed",
                static_cast<unsigned>(event));
        }
    }
}

void __fastcall configuration_category_append_detour(
    void* list,
    void* category_record) noexcept {
    ConfigurationCategoryAppendFn original =
        g_original_configuration_category_append.load(
            std::memory_order_acquire);
    if (!original) return;

    const bool diagnostics = erui::detail::diagnostics_enabled();
    const std::uint32_t event = diagnostics
        ? g_configuration_category_append_calls.fetch_add(
              1, std::memory_order_relaxed) + 1
        : 0;
    std::uint32_t trace_code{};
    bool trace_readable{};
    if (category_record) {
#if defined(_MSC_VER)
        __try {
#endif
            trace_code = *reinterpret_cast<const std::uint32_t*>(
                static_cast<const std::byte*>(category_record) +
                kOptionSettingCategoryCodeOffset);
            trace_readable = true;
#if defined(_MSC_VER)
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            trace_readable = false;
        }
#endif
    }
    if (diagnostics && event <= kCategoryAppendDiagnosticLogLimit) {
        std::uint64_t count{};
        bool contains_mod_settings{};
        const bool valid_list = inspect_top_category_list(
            list, count, contains_mod_settings);
        erui::detail::logf(
            erui::LogLevel::info,
            "diagnostic: Configuration category append[%u] entered list=%p record=%p code=%u readable=%u count=%llu validated=%u containsHub=%u hooksReady=%u",
            static_cast<unsigned>(event),
            list,
            category_record,
            static_cast<unsigned>(trace_code),
            trace_readable ? 1u : 0u,
            static_cast<unsigned long long>(count),
            valid_list ? 1u : 0u,
            contains_mod_settings ? 1u : 0u,
            g_mod_settings_tab_hooks_ready.load(std::memory_order_acquire)
                ? 1u
                : 0u);
        log_top_category_list_snapshot(
            "before-native-append", event, list);
    }

    const auto append_original = [&]() noexcept {
        if (diagnostics && event <= kCategoryAppendDiagnosticLogLimit) {
            erui::detail::logf(
                erui::LogLevel::info,
                "diagnostic: Configuration category append[%u] calling native append for original code=%u readable=%u",
                static_cast<unsigned>(event),
                static_cast<unsigned>(trace_code),
                trace_readable ? 1u : 0u);
        }
        original(list, category_record);
        if (diagnostics && event <= kCategoryAppendDiagnosticLogLimit) {
            erui::detail::logf(
                erui::LogLevel::info,
                "diagnostic: Configuration category append[%u] native append returned for original code=%u",
                static_cast<unsigned>(event),
                static_cast<unsigned>(trace_code));
            log_top_category_list_snapshot(
                "after-native-append", event, list);
        }
    };

    if (trace_readable && trace_code == 8u) {
        // Category 4 occupies the first added slot after Graphics. If the
        // in-game menu adds Quit Game afterward, it uses the next native GFX
        // slot, preserving both categories without changing their order.
        append_original();
        try_insert_mod_settings_category(
            list, original, event, diagnostics);
        return;
    }

    append_original();
}

void install_configuration_category_append_observer(
    const GameAddresses& addresses) noexcept {
    if (!erui::detail::diagnostics_enabled() ||
        g_configuration_category_append_hook_installed.load(
            std::memory_order_acquire)) {
        return;
    }
    if (!addresses.configuration_category_append) {
        erui::detail::logf(
            erui::LogLevel::warning,
            "Configuration category-list observer unavailable: exact-build append function did not resolve");
        return;
    }
    auto result = SafetyHookInline::create(
        addresses.configuration_category_append,
        reinterpret_cast<void*>(&configuration_category_append_detour),
        SafetyHookInline::StartDisabled);
    if (!result) {
        erui::detail::logf(
            erui::LogLevel::warning,
            "Configuration category-list observer could not be prepared (SafetyHook error type %u)",
            static_cast<unsigned>(result.error().type));
        return;
    }
    g_configuration_category_append_hook = std::move(*result);
    g_original_configuration_category_append.store(
        g_configuration_category_append_hook.original<
            ConfigurationCategoryAppendFn>(),
        std::memory_order_release);
    if (auto enabled = g_configuration_category_append_hook.enable();
        !enabled) {
        erui::detail::logf(
            erui::LogLevel::warning,
            "Configuration category-list observer could not be enabled (SafetyHook error type %u)",
            static_cast<unsigned>(enabled.error().type));
        g_configuration_category_append_hook.reset();
        g_original_configuration_category_append.store(
            nullptr, std::memory_order_release);
        return;
    }
    g_configuration_category_append_hook_installed.store(
        true, std::memory_order_release);
    erui::detail::logf(
        erui::LogLevel::info,
        "Hook installed: read-only Configuration category-list observer (captures vanilla category codes and count)");
}

void scope_mod_settings_panel_route(
    safetyhook::Context& context) noexcept {
    if (!g_mod_settings_tab_hooks_ready.load(std::memory_order_acquire)) {
        return;
    }
    const bool diagnostics = erui::detail::diagnostics_enabled();
    const std::uint32_t event = diagnostics
        ? g_mod_settings_route_calls.fetch_add(
              1, std::memory_order_relaxed) + 1
        : 0;
#if defined(_MSC_VER)
    __try {
#endif
        auto* const panel_type = reinterpret_cast<std::uint8_t*>(
            context.rsp + 0x20);
        // In the exact 2.7.1.0 category-4 wrapper, the six-argument row
        // callback is stored beside the panel type at RSP+0x60. Changing only
        // the type leaves the two-argument Adjust Brightness materializer in
        // place, which is why the custom tab displays the brightness slider.
        auto* const panel_callback = reinterpret_cast<std::uintptr_t*>(
            context.rsp + 0x60);
        const std::uint8_t before = *panel_type;
        const std::uintptr_t callback_before = *panel_callback;
        const std::uintptr_t brightness_callback = reinterpret_cast<
            std::uintptr_t>(g_addresses.mod_settings_panel_materializer);
        const std::uintptr_t game_options_callback = reinterpret_cast<
            std::uintptr_t>(g_addresses.game_options_handler);
        const bool game_options_hook_ready =
            g_original_game_options.load(std::memory_order_acquire) != nullptr;
        // This site is inside category 4's route wrapper. RDI contains the
        // caller's input-mode variant (0/1), not the category code. The
        // category is established by the wrapper's native category-4 data.
        const std::uint8_t route_argument = static_cast<std::uint8_t>(
            context.rdi & 0xFFu);
        const bool scoped_route = before == 6u &&
            callback_before == brightness_callback &&
            brightness_callback != 0 &&
            game_options_callback != 0 &&
            game_options_hook_ready;
        if (scoped_route) {
            *panel_callback = game_options_callback;
            *panel_type = 2u;
            if (t_mod_settings_route_depth != UINT32_MAX) {
                ++t_mod_settings_route_depth;
            }
            // Use the existing six-argument Game Options callback and shell
            // only for our inserted category. The detour recognizes the
            // scoped depth and materializes Hub rows without native options.
            const std::uint32_t previous =
                g_mod_settings_route_scopes.fetch_add(
                    1, std::memory_order_relaxed);
            if (previous == 0 || erui::detail::diagnostics_enabled()) {
                erui::detail::logf(
                    erui::LogLevel::info,
                    "Native Mod Settings route redirected from brightness callback %p to Game Options callback %p and type-2 shell (scope %u)",
                    reinterpret_cast<void*>(callback_before),
                    reinterpret_cast<void*>(game_options_callback),
                    previous + 1);
            }
        }
        const bool rejected_brightness_route = before == 6u && !scoped_route;
        if (diagnostics && event <= kCategoryRouteDiagnosticLogLimit) {
            erui::detail::logf(
                erui::LogLevel::info,
                "diagnostic: Mod Settings route[%u] routeArgument=%u stack=%p panelTypeBefore=%u panelTypeAfter=%u callbackBefore=%p callbackAfter=%p redirected=%u rejected=%u gameOptionsHookReady=%u hubBuildDepth=%u",
                static_cast<unsigned>(event),
                static_cast<unsigned>(route_argument),
                reinterpret_cast<void*>(context.rsp),
                static_cast<unsigned>(before),
                static_cast<unsigned>(*panel_type),
                reinterpret_cast<void*>(callback_before),
                reinterpret_cast<void*>(*panel_callback),
                scoped_route ? 1u : 0u,
                rejected_brightness_route ? 1u : 0u,
                game_options_hook_ready ? 1u : 0u,
                static_cast<unsigned>(t_mod_settings_route_depth));
        }
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        erui::detail::logf(
            erui::LogLevel::warning,
            "Mod Settings panel route could not be scoped safely; native rows will remain on their original route");
    }
#endif
}

bool copy_native_title_source_path(
    const char* source,
    char (&destination)[sizeof(native_menu_title_source_path)]) noexcept {
    if (!source) {
        return false;
    }
#if defined(_MSC_VER)
    __try {
#endif
        for (std::size_t index = 0; index < sizeof(destination); ++index) {
            destination[index] = source[index];
        }
        return true;
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#endif
}

bool copy_scaleform_diagnostic_path(
    const char* source,
    char (&destination)[kScaleformPathDiagnosticBufferSize]) noexcept {
    if (!source) return false;
#if defined(_MSC_VER)
    __try {
#endif
        for (std::size_t index = 0; index < sizeof(destination); ++index) {
            destination[index] = source[index];
            if (source[index] == '\0') return index != 0;
        }
        destination[sizeof(destination) - 1] = '\0';
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        destination[0] = '\0';
        return false;
    }
#endif
    return false;
}

std::uint64_t diagnostic_scaleform_path_hash(
    const char* path) noexcept {
    std::uint64_t hash = 14695981039346656037ull;
    for (std::size_t index = 0; path[index] != '\0'; ++index) {
        hash ^= static_cast<unsigned char>(path[index]);
        hash *= 1099511628211ull;
    }
    return hash == 0 ? 1 : hash;
}

void observe_scaleform_path(safetyhook::Context& context) noexcept {
    HookPerfScope perf_scope(g_perf_scaleform_path);
    if (context.r8 < 0x10000) {
        return;
    }

    char observed_path[sizeof(native_menu_title_source_path)]{};
    if (!copy_native_title_source_path(
            reinterpret_cast<const char*>(context.r8), observed_path)) {
        return;
    }

    if (erui::detail::diagnostics_enabled()) {
        const std::uint32_t category = diagnostic_category();
        const std::uint32_t menu_context = diagnostic_menu_context();
        const bool target_category = category == 0 || category == 4 ||
            category == 6 || category == 7 || category == 8 || category == 9;
        char diagnostic_path[kScaleformPathDiagnosticBufferSize]{};
        if (target_category && copy_scaleform_diagnostic_path(
                reinterpret_cast<const char*>(context.r8), diagnostic_path) &&
            claim_diagnostic_scaleform_path(
                menu_context,
                category,
                diagnostic_scaleform_path_hash(diagnostic_path))) {
            erui::detail::logf(
                erui::LogLevel::trace,
                "diagnostic: Scaleform object path menuContext=%u category=%u categoryName=%s categorySource=%s tabAgeMs=%llu movieContext=%p destination=%p path=%s",
                static_cast<unsigned>(menu_context),
                static_cast<unsigned>(category),
                native_category_name(category),
                t_diagnostic_native_category < kDiagnosticCategoryCount
                    ? "native-materializer" : "selected-tab",
                static_cast<unsigned long long>(diagnostic_tab_age_ms()),
                reinterpret_cast<void*>(context.rcx),
                reinterpret_cast<void*>(context.rdx),
                diagnostic_path);
        }
    }

    // The native action producer hard-codes Widgets/Button. During one
    // matched color-row construction, bind that same native controller to
    // the standalone sibling instead. The TLS source check prevents another
    // row, thread, or unrelated resolver call from being redirected.
    if (const char* replacement = color_picker_widget_path_override(
            observed_path,
            reinterpret_cast<const void*>(context.rcx))) {
        context.r8 = reinterpret_cast<std::uintptr_t>(replacement);
        return;
    }

    // The Hub is built on the Game Options shell, whose constructor binds
    // WindowList.ControllSetting. Give it the dedicated sibling clip added by
    // the GFX patcher so it never shares General's display object.
    if (t_mod_settings_route_depth != 0 &&
        std::memcmp(observed_path, "ControllSetting",
            sizeof("ControllSetting")) == 0) {
        static constexpr char kHubPanelPath[] = "ERModSettingsHub";
        context.r8 = reinterpret_cast<std::uintptr_t>(kHubPanelPath);
        return;
    }

    // The native page constructor resolves MenuTitle/Text_0 into its own
    // persistent 0x60-byte result. Redirect only that exact construction call
    // while a custom physical route is pending. The page remains responsible
    // for the result's lifetime; we retain only non-owning pointers until the
    // immediately following subpage-handler callback.
    const char* replacement = g_title_capture.observe(
        observed_path,
        g_submenu_navigation.pending_route(),
        reinterpret_cast<void*>(context.rcx),
        reinterpret_cast<void*>(context.rdx));
    if (replacement) {
        context.r8 = reinterpret_cast<std::uintptr_t>(replacement);
    }
}

bool diagnostic_category_is_relevant(std::uint32_t category) noexcept {
    return category == 0 || category == 4 || category == 6 ||
        category == 7 || category == 8 || category == 9;
}

bool read_native_row_count_for_diagnostics(
    void* page,
    std::uint32_t category,
    std::uint64_t& count) noexcept {
    count = 0;
    if (!page || !g_addresses.game_image_base) return false;
    const std::uintptr_t expected_rva = category == 6
        ? pad_setting_dialog_vtable_rva()
        : option_setting_dialog_vtable_rva();
    if (expected_rva >= g_addresses.game_image_size) return false;
    void* page_vtable{};
    if (!read_diagnostic_value(page, page_vtable) ||
        page_vtable != g_addresses.game_image_base + expected_rva) {
        return false;
    }
    return read_diagnostic_value(
        static_cast<const std::byte*>(page) + kPanelNativeCountOffset,
        count);
}

void observe_native_row_builder(
    const char* row_kind,
    safetyhook::Context& context,
    bool read_pointed_byte) noexcept {
    if (!erui::detail::diagnostics_enabled()) return;
    const std::uint32_t category = diagnostic_category();
    if (!diagnostic_category_is_relevant(category)) return;

    const std::uint32_t menu_context = diagnostic_menu_context();
    const std::size_t counter_index =
        diagnostic_menu_context_bucket(menu_context) *
            kDiagnosticCategoryCount + category;
    if (counter_index >= g_native_row_builder_diagnostic_calls.size()) return;
    const std::uint32_t event =
        g_native_row_builder_diagnostic_calls[counter_index].fetch_add(
            1, std::memory_order_relaxed) + 1;
    if (event > kNativeRowBuilderDiagnosticLogLimit) return;

    std::uintptr_t stack_arg5{};
    std::uintptr_t stack_arg6{};
    const bool has_arg5 = read_diagnostic_stack_argument(
        context.rsp, 0x28, stack_arg5);
    const bool has_arg6 = read_diagnostic_stack_argument(
        context.rsp, 0x30, stack_arg6);
    std::uint8_t current_value{};
    const bool current_value_read = read_pointed_byte && read_diagnostic_byte(
        reinterpret_cast<const void*>(context.r8), current_value);
    std::uint64_t row_index_before{};
    const bool row_index_read = read_native_row_count_for_diagnostics(
        reinterpret_cast<void*>(context.rcx), category, row_index_before);
    erui::detail::logf(
        erui::LogLevel::trace,
        "diagnostic: native row builder[%u] kind=%s menuContext=%u category=%u categoryName=%s categorySource=%s tabAgeMs=%llu selectedIndex=%u activePanel=%p page=%p pageRowCountBefore=%llu pageRowCountRead=%u textReferences=%p valueOrSelection=%p auxiliary=%p stackArg5=0x%llX stackArg5Read=%u stackArg6=0x%llX stackArg6Read=%u pointedByte=%u pointedByteRead=%u",
        static_cast<unsigned>(event),
        row_kind ? row_kind : "unknown",
        static_cast<unsigned>(menu_context),
        static_cast<unsigned>(category),
        native_category_name(category),
        t_diagnostic_native_category < kDiagnosticCategoryCount
            ? "native-materializer" : "selected-tab",
        static_cast<unsigned long long>(diagnostic_tab_age_ms()),
        static_cast<unsigned>(g_last_tab_selected_index.load(
            std::memory_order_relaxed)),
        g_last_tab_active_panel.load(std::memory_order_relaxed),
        reinterpret_cast<void*>(context.rcx),
        static_cast<unsigned long long>(row_index_before),
        row_index_read ? 1u : 0u,
        reinterpret_cast<void*>(context.rdx),
        reinterpret_cast<void*>(context.r8),
        reinterpret_cast<void*>(context.r9),
        static_cast<unsigned long long>(stack_arg5),
        has_arg5 ? 1u : 0u,
        static_cast<unsigned long long>(stack_arg6),
        has_arg6 ? 1u : 0u,
        static_cast<unsigned>(current_value),
        current_value_read ? 1u : 0u);
}

void observe_add_toggle(safetyhook::Context& context) noexcept {
    observe_native_row_builder("toggle", context, true);
}

void observe_add_slider(safetyhook::Context& context) noexcept {
    observe_native_row_builder("slider", context, true);
}

void observe_add_choice(safetyhook::Context& context) noexcept {
    observe_native_row_builder("inline-choice", context, true);
}

void observe_add_button(safetyhook::Context& context) noexcept {
    observe_native_row_builder("button", context, false);
}

void observe_add_unclassified_row(safetyhook::Context& context) noexcept {
    observe_native_row_builder("unclassified-94A430", context, true);
}

void observe_scaleform_visibility(safetyhook::Context& context) noexcept {
    if (!erui::detail::diagnostics_enabled()) return;
    const std::uint32_t category = diagnostic_category();
    if (!diagnostic_category_is_relevant(category)) return;

    const std::uint32_t menu_context = diagnostic_menu_context();
    const std::size_t counter_index =
        diagnostic_menu_context_bucket(menu_context) *
            kDiagnosticCategoryCount + category;
    if (counter_index >= g_scaleform_visibility_diagnostic_calls.size()) return;
    const std::uint8_t visible = static_cast<std::uint8_t>(context.rdx & 0xFFu);
    std::uintptr_t caller{};
    const bool caller_read = read_diagnostic_stack_argument(
        context.rsp, 0, caller);
    const std::uintptr_t active_panel = reinterpret_cast<std::uintptr_t>(
        g_last_tab_active_panel.load(std::memory_order_relaxed));
    if (!claim_unique_scaleform_visibility(
            menu_context,
            category,
            active_panel,
            context.rcx,
            caller_read ? caller : 0,
            visible)) {
        return;
    }
    const std::uint32_t event =
        g_scaleform_visibility_diagnostic_calls[counter_index].fetch_add(
            1, std::memory_order_relaxed) + 1;
    if (event > kScaleformVisibilityDiagnosticLogLimit) return;
    erui::detail::logf(
        erui::LogLevel::trace,
        "diagnostic: Scaleform visibility[%u] menuContext=%u category=%u categoryName=%s categorySource=%s tabAgeMs=%llu selectedIndex=%u activePanel=%p sceneObjectProxy=%p visible=%u caller=%p callerRead=%u",
        static_cast<unsigned>(event),
        static_cast<unsigned>(menu_context),
        static_cast<unsigned>(category),
        native_category_name(category),
        t_diagnostic_native_category < kDiagnosticCategoryCount
            ? "native-materializer" : "selected-tab",
        static_cast<unsigned long long>(diagnostic_tab_age_ms()),
        static_cast<unsigned>(g_last_tab_selected_index.load(
            std::memory_order_relaxed)),
        g_last_tab_active_panel.load(std::memory_order_relaxed),
        reinterpret_cast<void*>(context.rcx),
        static_cast<unsigned>(visible),
        reinterpret_cast<void*>(caller),
        caller_read ? 1u : 0u);
}

void observe_scaleform_text(safetyhook::Context& context) noexcept {
    if (!erui::detail::diagnostics_enabled()) return;
    const std::uint32_t category = diagnostic_category();
    if (!diagnostic_category_is_relevant(category)) return;

    const std::uint32_t menu_context = diagnostic_menu_context();
    const std::size_t counter_index =
        diagnostic_menu_context_bucket(menu_context) *
            kDiagnosticCategoryCount + category;
    if (counter_index >= g_scaleform_text_diagnostic_calls.size()) return;

    wchar_t wide_text[256]{};
    std::size_t text_length{};
    bool text_truncated{};
    const bool text_read = copy_diagnostic_utf16(
        reinterpret_cast<const wchar_t*>(context.rdx),
        wide_text,
        sizeof(wide_text) / sizeof(wide_text[0]),
        text_length,
        text_truncated);
    std::uintptr_t caller{};
    const bool caller_read = read_diagnostic_stack_argument(
        context.rsp, 0, caller);
    const std::uint64_t text_hash = text_read
        ? hash_diagnostic_utf16(wide_text, text_length)
        : 0;
    if (!claim_unique_scaleform_text_write(
            menu_context,
            category,
            context.rcx,
            caller_read ? caller : 0,
            text_hash)) {
        return;
    }
    const std::uint32_t event =
        g_scaleform_text_diagnostic_calls[counter_index].fetch_add(
            1, std::memory_order_relaxed) + 1;
    if (event > kScaleformTextDiagnosticLogLimit) return;

    char utf8_text[1024]{};
    bool utf8_converted = false;
    if (text_read) {
        if (text_length == 0) {
            utf8_converted = true;
        } else {
            const int bytes_written = WideCharToMultiByte(
                CP_UTF8,
                0,
                wide_text,
                static_cast<int>(text_length),
                utf8_text,
                static_cast<int>(sizeof(utf8_text) - 1),
                nullptr,
                nullptr);
            if (bytes_written > 0) {
                utf8_text[bytes_written] = '\0';
                utf8_converted = true;
                for (int index = 0; index < bytes_written; ++index) {
                    if (utf8_text[index] == '\r' || utf8_text[index] == '\n' ||
                        utf8_text[index] == '\t' || utf8_text[index] == '"') {
                        utf8_text[index] = ' ';
                    }
                }
            }
        }
    }
    if (!text_read) {
        std::snprintf(utf8_text, sizeof(utf8_text), "<unreadable>");
    } else if (!utf8_converted) {
        std::snprintf(utf8_text, sizeof(utf8_text), "<utf8-conversion-failed>");
    }

    erui::detail::logf(
        erui::LogLevel::trace,
        "diagnostic: Scaleform text setter[%u] menuContext=%u category=%u categoryName=%s categorySource=%s tabAgeMs=%llu selectedIndex=%u activePanel=%p scaleformValue=%p textPointer=%p textRead=%u textUnits=%llu textTruncated=%u utf8Converted=%u textUtf8=\"%s\" caller=%p callerRead=%u",
        static_cast<unsigned>(event),
        static_cast<unsigned>(menu_context),
        static_cast<unsigned>(category),
        native_category_name(category),
        t_diagnostic_native_category < kDiagnosticCategoryCount
            ? "native-materializer" : "selected-tab",
        static_cast<unsigned long long>(diagnostic_tab_age_ms()),
        static_cast<unsigned>(g_last_tab_selected_index.load(
            std::memory_order_relaxed)),
        g_last_tab_active_panel.load(std::memory_order_relaxed),
        reinterpret_cast<void*>(context.rcx),
        reinterpret_cast<void*>(context.rdx),
        text_read ? 1u : 0u,
        static_cast<unsigned long long>(text_length),
        text_truncated ? 1u : 0u,
        utf8_converted ? 1u : 0u,
        utf8_text,
        reinterpret_cast<void*>(caller),
        caller_read ? 1u : 0u);
}

void finish_mod_settings_panel_route(safetyhook::Context&) noexcept {
    if (t_mod_settings_route_depth == 0) return;
    --t_mod_settings_route_depth;
    const std::uint32_t event = erui::detail::diagnostics_enabled()
        ? g_mod_settings_route_returns.fetch_add(
              1, std::memory_order_relaxed) + 1
        : 0;
    if (event != 0 && event <= kCategoryRouteDiagnosticLogLimit) {
        erui::detail::logf(
            erui::LogLevel::trace,
            "diagnostic: Mod Settings route factory returned[%u]; hubBuildDepth=%u",
            static_cast<unsigned>(event),
            static_cast<unsigned>(t_mod_settings_route_depth));
    }
}

bool invoke_scaleform_text_setter(
    ScaleformTextSetterFn function,
    void* value,
    const wchar_t* text,
    unsigned long& seh_exception_code) noexcept {
    seh_exception_code = 0;
    if (!function || !value || !text || !*text) {
        return false;
    }
#if defined(_MSC_VER)
    __try {
        function(value, text);
        return true;
    } __except (seh_exception_code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    function(value, text);
    return true;
#endif
}

bool invoke_scaleform_path_resolver(
    ScaleformPathResolverFn function,
    void* movie_context,
    void* destination,
    const char* path,
    unsigned long& seh_exception_code) noexcept {
    seh_exception_code = 0;
    if (!function || !movie_context || !destination || !path || !*path) {
        return false;
    }
#if defined(_MSC_VER)
    __try {
        return function(movie_context, destination, path) == destination;
    } __except (seh_exception_code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    return function(movie_context, destination, path) == destination;
#endif
}

bool invoke_scaleform_result_destructor(
    ScaleformResultDestructorFn function,
    void* nested_member,
    unsigned long& seh_exception_code) noexcept {
    seh_exception_code = 0;
    if (!function || !nested_member) {
        return false;
    }
#if defined(_MSC_VER)
    __try {
        function(nested_member);
        return true;
    } __except (seh_exception_code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    function(nested_member);
    return true;
#endif
}

void log_title_fault(const char* operation, unsigned long exception_code) noexcept {
    const std::uint32_t fault =
        g_title_fault_count.fetch_add(1, std::memory_order_relaxed) + 1;
    if (fault <= kTitleFaultLogLimit) {
        erui::detail::logf(
            erui::LogLevel::warning,
            "Scaleform title bridge %s failed (fault=%u seh=0x%08lX); menu rows remain active",
            operation,
            static_cast<unsigned>(fault),
            exception_code);
    }
}

void apply_physical_page_titles(
    const CapturedTitleTarget& captured,
    const wchar_t* outer_title,
    const wchar_t* page_title) noexcept {
    if (!captured.valid() || !g_addresses.title_bridge_complete()) {
        return;
    }

    // Preserve the page-owned persistent result for the inner heading. The
    // outer heading is a one-shot assignment through a temporary native result
    // whose construction and cleanup mirror Elden Ring's own call sites.
    if (outer_title && *outer_title) {
        alignas(16) std::array<std::byte, scaleform_path_result_size> temporary{};
        unsigned long exception_code = 0;
        const bool resolved = invoke_scaleform_path_resolver(
            g_addresses.scaleform_path_resolver,
            captured.movie_context,
            temporary.data(),
            custom_outer_title_path,
            exception_code);
        if (!resolved) {
            log_title_fault("outer-path resolution", exception_code);
        } else {
            if (!invoke_scaleform_text_setter(
                    g_addresses.scaleform_text_setter,
                    temporary.data() + scaleform_text_value_offset,
                    outer_title,
                    exception_code)) {
                log_title_fault("outer-title assignment", exception_code);
            }
            if (!invoke_scaleform_result_destructor(
                    g_addresses.scaleform_result_destructor,
                    temporary.data() + scaleform_destructor_member_offset,
                    exception_code)) {
                log_title_fault("temporary-result cleanup", exception_code);
            }
        }
    }

    if (page_title && *page_title) {
        unsigned long exception_code = 0;
        if (!invoke_scaleform_text_setter(
                g_addresses.scaleform_text_setter,
                captured.text_value(),
                page_title,
                exception_code)) {
            log_title_fault("page-title assignment", exception_code);
        }
    }
}

// Fork: provider help can be longer than the native 1140 px LineHelp field.
// The patched settings movies carry an empty twin, LineHelpWide, beside it.
// When a status bar assigns a provider-registered text to its LineHelp, the
// text goes to the twin and LineHelp is cleared. Any other text reaches
// LineHelp unchanged and empties the twin, so vanilla rows, vanilla menus and
// movies without the twin keep the native presentation.
constexpr std::size_t kStatusBarLineHelpOffset = 0x60;
constexpr const char* kWideLineHelpPath = "LineHelpWide";
// Two-line twin (multiline, word wrap, bottom line where LineHelp sits). Help
// that overflows LineHelpWide moves here; the marquee is the fallback.
constexpr const char* kTwoLineHelpPath = "LineHelpWide2";
// Options binds one status bar per panel and tab (30+ per visit); slots must
// outlive a visit or vanilla panels stop restoring the native line.
constexpr std::size_t kStatusBarSlotCount = 256;
constexpr std::uint32_t kWideLineHelpLogLimit = 4;

struct StatusBarSlot {
    std::atomic<std::uint8_t*> bar{};
    std::atomic_bool wide_active{};
};

std::array<StatusBarSlot, kStatusBarSlotCount> g_status_bars{};
std::atomic<std::size_t> g_next_status_bar{};
SafetyHookInline g_status_bar_binder_hook{};
SafetyHookInline g_line_help_text_hook{};
std::atomic<StatusBarBinderFn> g_original_status_bar_binder{};
std::atomic<ScaleformTextSetterFn> g_original_line_help_text_setter{};
std::atomic<std::uint32_t> g_wide_line_help_events{};
std::atomic<std::uint8_t*> g_marquee_bar{};  // status bar whose help scrolls

// Every text write of the whole UI (HUD included) passes line_help_text_detour.
// One bit per hashed LineHelp field address answers "not a known status bar"
// with a single load for almost all of them. The binder sets the bit of each
// new bar; when the slot ring wraps it rebuilds the other buffer from the live
// slots and swaps, so bars that were overwritten stop matching. A bit that is
// set for a stale bar only costs the slot scan, as before.
constexpr std::size_t kLineHelpFilterBits = 4096;
using LineHelpFilter = std::array<std::atomic<std::uint64_t>, kLineHelpFilterBits / 64>;
std::array<LineHelpFilter, 2> g_line_help_filters{};
std::atomic<LineHelpFilter*> g_line_help_filter{&g_line_help_filters[0]};

const void* line_help_field(const std::uint8_t* bar) noexcept {
    return bar + kStatusBarLineHelpOffset + scaleform_text_value_offset;
}

std::size_t line_help_filter_bit(const void* field) noexcept {
    const auto key = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(field) >> 3);
    return static_cast<std::size_t>((key * 0x9E3779B97F4A7C15ull) >> (64 - 12));
}

bool line_help_filter_has(const void* field) noexcept {
    const std::size_t bit = line_help_filter_bit(field);
    const LineHelpFilter& filter = *g_line_help_filter.load(std::memory_order_acquire);
    return (filter[bit / 64].load(std::memory_order_relaxed) >> (bit % 64)) & 1u;
}

void line_help_filter_add(LineHelpFilter& filter, const std::uint8_t* bar) noexcept {
    const std::size_t bit = line_help_filter_bit(line_help_field(bar));
    filter[bit / 64].fetch_or(std::uint64_t{1} << (bit % 64), std::memory_order_relaxed);
}

// The game runs its own marquee for long LineHelp text and rewrites the field
// outside the hooked setter, so the native field is hidden, not just emptied.
void set_native_line_help_visible(std::uint8_t* bar, bool visible) noexcept {
    using SetVisibleFn = void(__fastcall*)(void* proxy, std::uint8_t visible);
    const auto set_visible =
        reinterpret_cast<SetVisibleFn>(g_addresses.scaleform_visibility_setter);
    if (!set_visible) return;
#if defined(_MSC_VER)
    __try {
#endif
        set_visible(bar + kStatusBarLineHelpOffset, visible ? 1 : 0);
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
#endif
}

void* __fastcall status_bar_binder_detour(
    void* status_bar, void* source, void* arg3, void* arg4) noexcept {
    const StatusBarBinderFn original =
        g_original_status_bar_binder.load(std::memory_order_acquire);
    void* const result = original ? original(status_bar, source, arg3, arg4)
                                   : status_bar;
    auto* const bar = static_cast<std::uint8_t*>(status_bar);
    if (!bar) return result;
    if (g_marquee_bar.load(std::memory_order_relaxed) == bar) {
        g_marquee_bar.store(nullptr, std::memory_order_release);
    }
    for (StatusBarSlot& slot : g_status_bars) {
        if (slot.bar.load(std::memory_order_relaxed) == bar) {
            if (slot.wide_active.exchange(false, std::memory_order_relaxed)) {
                set_native_line_help_visible(bar, true);
            }
            return result;
        }
    }
    const std::size_t next =
        g_next_status_bar.fetch_add(1, std::memory_order_relaxed);
    StatusBarSlot& slot = g_status_bars[next % kStatusBarSlotCount];
    slot.wide_active.store(false, std::memory_order_relaxed);
    slot.bar.store(bar, std::memory_order_release);
    LineHelpFilter* const current = g_line_help_filter.load(std::memory_order_acquire);
    if ((next + 1) % kStatusBarSlotCount != 0) {
        line_help_filter_add(*current, bar);
        return result;
    }
    LineHelpFilter& rebuilt = current == &g_line_help_filters[0]
        ? g_line_help_filters[1] : g_line_help_filters[0];
    for (auto& word : rebuilt) word.store(0, std::memory_order_relaxed);
    for (const StatusBarSlot& live : g_status_bars) {
        if (std::uint8_t* const live_bar = live.bar.load(std::memory_order_acquire)) {
            line_help_filter_add(rebuilt, live_bar);
        }
    }
    g_line_help_filter.store(&rebuilt, std::memory_order_release);
    return result;
}

bool call_text_setter_guarded(
    ScaleformTextSetterFn function,
    void* value,
    const wchar_t* text) noexcept {
#if defined(_MSC_VER)
    __try {
        function(value, text);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    function(value, text);
    return true;
#endif
}

bool resolve_wide_line_help_guarded(
    std::uint8_t* bar,
    std::byte* destination,
    bool& constructed,
    const char* path) noexcept {
    constructed = false;
#if defined(_MSC_VER)
    __try {
#endif
        if (g_addresses.scaleform_path_resolver(
                bar, destination, path) != destination) {
            return false;
        }
        constructed = true;
        return g_addresses.scaleform_value_exists(destination);
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#endif
}

bool scroll_wide_line_help_guarded(
    void* value,
    int position,
    int* max_scroll) noexcept {
    if (!g_addresses.scaleform_set_hscroll || !g_addresses.scaleform_max_hscroll) {
        return false;
    }
#if defined(_MSC_VER)
    __try {
#endif
        g_addresses.scaleform_set_hscroll(value, position);
        if (max_scroll) *max_scroll = g_addresses.scaleform_max_hscroll(value);
        return true;
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#endif
}

// Writes text (when not null) into the bar's LineHelpWide and sets its
// horizontal scroll. False when the movie has no twin.
bool assign_wide_line_help(
    std::uint8_t* bar,
    const wchar_t* text,
    ScaleformTextSetterFn setter,
    int scroll = 0,
    int* max_scroll = nullptr,
    const char* path = kWideLineHelpPath) noexcept {
    alignas(16) std::array<std::byte, scaleform_path_result_size> proxy{};
    bool constructed = false;
    bool assigned = resolve_wide_line_help_guarded(bar, proxy.data(), constructed, path);
    void* const value = proxy.data() + scaleform_text_value_offset;
    if (assigned && text) {
        assigned = call_text_setter_guarded(setter, value, text);
    }
    if (assigned) {
        (void)scroll_wide_line_help_guarded(value, scroll, max_scroll);
    }
    if (constructed) {
        unsigned long exception_code = 0;
        (void)invoke_scaleform_result_destructor(
            g_addresses.scaleform_result_destructor,
            proxy.data() + scaleform_destructor_member_offset,
            exception_code);
    }
    return assigned;
}

// Marquee for provider help longer than LineHelpWide: hold at the start,
// scroll to the end, hold, jump back. Advanced from the page-frame pump, and
// only while the status bar lies inside the page being drawn (so it is alive).
constexpr std::uint64_t kMarqueeHoldMs = 1500;
constexpr std::uint64_t kMarqueePixelsPerSecond = 90;
constexpr std::uintptr_t kMarqueeMaxBarOffset = 0x10000;

// Copy of the provider help being measured, for the move to LineHelpWide2.
SRWLOCK g_help_text_lock = SRWLOCK_INIT;
std::array<wchar_t, 1024> g_help_text{};

void remember_help_text(const wchar_t* text) noexcept {
    AcquireSRWLockExclusive(&g_help_text_lock);
    wcsncpy_s(g_help_text.data(), g_help_text.size(), text ? text : L"", _TRUNCATE);
    ReleaseSRWLockExclusive(&g_help_text_lock);
}

// Moves the remembered help to the two-line field; false without that field.
bool move_help_to_two_lines(std::uint8_t* bar, ScaleformTextSetterFn setter) noexcept {
    std::array<wchar_t, 1024> text{};
    AcquireSRWLockShared(&g_help_text_lock);
    text = g_help_text;
    ReleaseSRWLockShared(&g_help_text_lock);
    if (!text[0] || !assign_wide_line_help(bar, text.data(), setter, 0, nullptr, kTwoLineHelpPath)) {
        return false;
    }
    (void)assign_wide_line_help(bar, L"", setter);
    return true;
}

std::atomic<int> g_marquee_max{};
std::atomic<std::uint64_t> g_marquee_start{};
std::atomic<int> g_marquee_position{};
std::atomic_bool g_marquee_offset_logged{};

// The overflow is measured from the page-frame pump: right after SetText the
// TextField has not been laid out yet and reports a stale width.
void arm_marquee(std::uint8_t* bar) noexcept {
    g_marquee_bar.store(nullptr, std::memory_order_release);
    g_marquee_max.store(0, std::memory_order_relaxed);
    g_marquee_position.store(0, std::memory_order_relaxed);
    g_marquee_start.store(GetTickCount64(), std::memory_order_relaxed);
    g_marquee_bar.store(bar, std::memory_order_release);
}

void tick_marquee(void* page) noexcept {
    std::uint8_t* const bar = g_marquee_bar.load(std::memory_order_acquire);
    if (!bar || !page) return;
    const auto page_address = reinterpret_cast<std::uintptr_t>(page);
    const auto bar_address = reinterpret_cast<std::uintptr_t>(bar);
    const bool inside = bar_address >= page_address &&
        bar_address - page_address < kMarqueeMaxBarOffset;
    if (!g_marquee_offset_logged.exchange(true, std::memory_order_relaxed)) {
        erui::detail::logf(erui::LogLevel::info,
            "Help marquee: statusBar=%p page=%p offset=0x%llX inside=%u",
            static_cast<void*>(bar), page,
            static_cast<unsigned long long>(bar_address - page_address),
            inside ? 1u : 0u);
    }
    if (!inside) return;

    int max_scroll = g_marquee_max.load(std::memory_order_relaxed);
    if (max_scroll <= 0) {
        // Measure (scroll stays at 0) until the layout reports an overflow.
        const ScaleformTextSetterFn setter =
            g_original_line_help_text_setter.load(std::memory_order_acquire);
        if (!setter || !assign_wide_line_help(bar, nullptr, setter, 0, &max_scroll)) {
            g_marquee_bar.store(nullptr, std::memory_order_release);
            return;
        }
        if (max_scroll <= 0) {
            // Short text: stop measuring once the layout has surely settled.
            if (GetTickCount64() - g_marquee_start.load(std::memory_order_relaxed) > 1000) {
                g_marquee_bar.store(nullptr, std::memory_order_release);
            }
            return;
        }
        if (move_help_to_two_lines(bar, setter)) {
            g_marquee_bar.store(nullptr, std::memory_order_release);
            static std::atomic<std::uint32_t> two_line_logs{};
            if (two_line_logs.fetch_add(1, std::memory_order_relaxed) < 4) {
                erui::detail::logf(erui::LogLevel::info,
                    "Provider help moved to LineHelpWide2 (overflowPx=%d)", max_scroll);
            }
            return;
        }
        g_marquee_max.store(max_scroll, std::memory_order_relaxed);
        g_marquee_start.store(GetTickCount64(), std::memory_order_relaxed);
        static std::atomic<std::uint32_t> armed_logs{};
        if (armed_logs.fetch_add(1, std::memory_order_relaxed) < 4) {
            erui::detail::logf(erui::LogLevel::info,
                "Help marquee armed: overflowPx=%d", max_scroll);
        }
    }
    const std::uint64_t travel_ms =
        static_cast<std::uint64_t>(max_scroll) * 1000 / kMarqueePixelsPerSecond;
    const std::uint64_t cycle_ms = kMarqueeHoldMs * 2 + travel_ms;
    const std::uint64_t elapsed =
        (GetTickCount64() - g_marquee_start.load(std::memory_order_relaxed)) % cycle_ms;
    int position = 0;
    if (elapsed >= kMarqueeHoldMs + travel_ms) {
        position = max_scroll;
    } else if (elapsed > kMarqueeHoldMs) {
        position = static_cast<int>(
            (elapsed - kMarqueeHoldMs) * kMarqueePixelsPerSecond / 1000);
    }
    if (position == g_marquee_position.exchange(position, std::memory_order_relaxed)) {
        return;
    }
    const ScaleformTextSetterFn setter =
        g_original_line_help_text_setter.load(std::memory_order_acquire);
    if (!setter || !assign_wide_line_help(bar, nullptr, setter, position)) {
        g_marquee_bar.store(nullptr, std::memory_order_release);
    }
}

bool is_provider_help_text(const wchar_t* text) noexcept {
    if (!text || !*text) return false;
    const erui::detail::RuntimeState& runtime = erui::detail::runtime_state();
    return runtime.menu && runtime.menu->texts.contains(text);
}

void __fastcall line_help_text_detour(void* value, const wchar_t* text) noexcept {
    HookPerfScope perf_scope(g_perf_text_setter);
    const ScaleformTextSetterFn original =
        g_original_line_help_text_setter.load(std::memory_order_acquire);
    if (!original) return;
    // This runs for every text of the whole UI: almost every write is not a
    // known LineHelp field and leaves after the filter; otherwise scan only
    // the slots in use.
    if (!line_help_filter_has(value)) {
        { HookPerfScope game(g_perf_text_setter, true); original(value, text); }
        return;
    }
    const std::size_t used_slots = (std::min)(
        g_next_status_bar.load(std::memory_order_acquire), kStatusBarSlotCount);
    StatusBarSlot* match{};
    for (std::size_t index = 0; index < used_slots; ++index) {
        StatusBarSlot& slot = g_status_bars[index];
        std::uint8_t* const bar = slot.bar.load(std::memory_order_acquire);
        if (bar && value == bar + kStatusBarLineHelpOffset +
                scaleform_text_value_offset) {
            match = &slot;
            break;
        }
    }
    if (!match) {
        { HookPerfScope game(g_perf_text_setter, true); original(value, text); }
        return;
    }

    std::uint8_t* const bar = match->bar.load(std::memory_order_acquire);
    const bool provider_text = is_provider_help_text(text);
    int max_scroll = 0;
    if (provider_text && !assign_wide_line_help(bar, text, original, 0, &max_scroll)) {
        g_marquee_bar.store(nullptr, std::memory_order_release);
        static std::atomic_bool warned{};
        if (!warned.exchange(true, std::memory_order_relaxed)) {
            erui::detail::logf(erui::LogLevel::warning,
                "Provider help shown in the native LineHelp: this settings movie has no LineHelpWide (replaced by another mod?)");
        }
    } else if (provider_text) {
        remember_help_text(text);
        (void)assign_wide_line_help(bar, L"", original, 0, nullptr, kTwoLineHelpPath);
        { HookPerfScope game(g_perf_text_setter, true); original(value, text); }
        set_native_line_help_visible(bar, false);
        match->wide_active.store(true, std::memory_order_relaxed);
        arm_marquee(bar);
        const std::uint32_t event =
            g_wide_line_help_events.fetch_add(1, std::memory_order_relaxed) + 1;
        if (event <= kWideLineHelpLogLimit) {
            erui::detail::logf(erui::LogLevel::info,
                "Provider help routed to LineHelpWide (statusBar=%p event=%u overflowPx=%d)",
                static_cast<void*>(bar), static_cast<unsigned>(event), max_scroll);
        }
        return;
    }
    { HookPerfScope game(g_perf_text_setter, true); original(value, text); }
    g_marquee_bar.store(nullptr, std::memory_order_release);
    // Pages own separate status bar objects that share the movie's fields, so
    // the wide state is global: any non-provider text restores the native line.
    bool was_active = false;
    for (StatusBarSlot& slot : g_status_bars) {
        was_active |= slot.wide_active.exchange(false, std::memory_order_relaxed);
    }
    if (was_active) {
        (void)assign_wide_line_help(bar, L"", original);
        (void)assign_wide_line_help(bar, L"", original, 0, nullptr, kTwoLineHelpPath);
        set_native_line_help_visible(bar, true);
    }
}

void install_wide_line_help(const GameAddresses& addresses) noexcept {
    if (!addresses.status_bar_binder || !addresses.scaleform_text_setter ||
        !addresses.scaleform_value_exists || !addresses.title_bridge_complete()) {
        erui::detail::logf(erui::LogLevel::warning,
            "Wide provider help unavailable: status bar or Scaleform interfaces unresolved");
        return;
    }
    auto binder = SafetyHookInline::create(
        reinterpret_cast<void*>(addresses.status_bar_binder),
        reinterpret_cast<void*>(&status_bar_binder_detour),
        SafetyHookInline::StartDisabled);
    auto setter = SafetyHookInline::create(
        reinterpret_cast<void*>(addresses.scaleform_text_setter),
        reinterpret_cast<void*>(&line_help_text_detour),
        SafetyHookInline::StartDisabled);
    if (!binder || !setter) {
        erui::detail::logf(erui::LogLevel::warning,
            "Wide provider help hooks could not be prepared");
        return;
    }
    g_status_bar_binder_hook = std::move(*binder);
    g_line_help_text_hook = std::move(*setter);
    g_original_status_bar_binder.store(
        g_status_bar_binder_hook.original<StatusBarBinderFn>(),
        std::memory_order_release);
    g_original_line_help_text_setter.store(
        g_line_help_text_hook.original<ScaleformTextSetterFn>(),
        std::memory_order_release);
    if (!g_line_help_text_hook.enable() || !g_status_bar_binder_hook.enable()) {
        erui::detail::logf(erui::LogLevel::warning,
            "Wide provider help hooks could not be enabled");
        g_status_bar_binder_hook.reset();
        g_line_help_text_hook.reset();
        return;
    }
    erui::detail::logf(erui::LogLevel::info,
        "Wide provider help (LineHelpWide) hooks installed");
}

void reset_wide_line_help() noexcept {
    g_status_bar_binder_hook.reset();
    g_line_help_text_hook.reset();
    g_original_status_bar_binder.store(nullptr, std::memory_order_release);
    g_original_line_help_text_setter.store(nullptr, std::memory_order_release);
    for (StatusBarSlot& slot : g_status_bars) {
        slot.bar.store(nullptr, std::memory_order_release);
        slot.wide_active.store(false, std::memory_order_relaxed);
    }
    for (LineHelpFilter& filter : g_line_help_filters) {
        for (auto& word : filter) word.store(0, std::memory_order_relaxed);
    }
    g_line_help_filter.store(&g_line_help_filters[0], std::memory_order_release);
    g_wide_line_help_events.store(0, std::memory_order_relaxed);
    g_marquee_bar.store(nullptr, std::memory_order_release);
}

SRWLOCK g_menu_warning_lock = SRWLOCK_INIT;
std::wstring g_menu_warning{};
std::atomic_bool g_menu_warning_pending{};

void show_pending_menu_warning() noexcept {
    if (!g_menu_warning_pending.exchange(false, std::memory_order_acq_rel)) return;
    std::wstring message{};
    AcquireSRWLockExclusive(&g_menu_warning_lock);
    message.swap(g_menu_warning);
    ReleaseSRWLockExclusive(&g_menu_warning_lock);
    if (message.empty()) return;
    const ERUI_Result result = enqueue_native_alert(std::move(message),
        ERUI_ALERT_BUTTONS_OK, ERUI_ALERT_PLACEMENT_CENTER, nullptr, nullptr);
    erui::detail::logf(result == ERUI_OK ? erui::LogLevel::info : erui::LogLevel::warning,
        "Menu file warning alert queued: result=%u", static_cast<unsigned>(result));
}

const char* minhook_status_name(MH_STATUS status) noexcept {
    const char* name = MH_StatusToString(status);
    return name ? name : "MH_UNKNOWN";
}

const char* route_kind_name(erui::detail::PageRouteKind kind) noexcept {
    switch (kind) {
    case erui::detail::PageRouteKind::submenu:
        return "submenu";
    case erui::detail::PageRouteKind::root_continuation:
        return "root-continuation";
    case erui::detail::PageRouteKind::root_main:
        return "root-main";
    case erui::detail::PageRouteKind::builtin_continuation:
        return "built-in-continuation";
    case erui::detail::PageRouteKind::builtin_main:
        return "built-in-main";
    case erui::detail::PageRouteKind::tab_continuation:
        return "tab-continuation";
    case erui::detail::PageRouteKind::tab_main:
        return "tab-main";
    }
    return "unknown";
}

const char* request_kind_name(NavigationRequestKind kind) noexcept {
    switch (kind) {
    case NavigationRequestKind::submenu:
        return "submenu";
    case NavigationRequestKind::pagination_next:
        return "next";
    case NavigationRequestKind::pagination_previous:
        return "previous";
    }
    return "unknown";
}

bool copy_native_text_result_utf8(
    void* text_result,
    char* destination,
    std::size_t destination_capacity,
    bool& truncated) noexcept {
    truncated = false;
    if (!destination || destination_capacity == 0) return false;
    destination[0] = '\0';
    if (reinterpret_cast<std::uintptr_t>(text_result) < 0x10000) {
        return false;
    }

    wchar_t wide_text[192]{};
    bool pointer_read = false;
    bool terminated = false;
#if defined(_MSC_VER)
    __try {
#endif
        const wchar_t* const source =
            *reinterpret_cast<const wchar_t* const*>(text_result);
        if (!source) return false;
        pointer_read = true;
        for (std::size_t index = 0; index < 191; ++index) {
            wide_text[index] = source[index];
            if (wide_text[index] == L'\0') {
                terminated = true;
                break;
            }
        }
        if (!terminated) {
            wide_text[191] = L'\0';
            truncated = true;
        }
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        destination[0] = '\0';
        return false;
    }
#endif
    if (!pointer_read) return false;

    const int converted = WideCharToMultiByte(
        CP_UTF8,
        0,
        wide_text,
        -1,
        destination,
        static_cast<int>(destination_capacity),
        nullptr,
        nullptr);
    if (converted <= 0) {
        destination[0] = '\0';
        return false;
    }
    for (int index = 0; index < converted - 1; ++index) {
        const unsigned char character =
            static_cast<unsigned char>(destination[index]);
        if (character < 0x20 || character == '"') {
            destination[index] = ' ';
        }
    }
    return true;
}

void* __fastcall text_resolver_detour(
    void* text_result,
    std::uintptr_t argument2,
    std::uint32_t message_id,
    std::uintptr_t argument4,
    std::uintptr_t argument5) noexcept {
    HookPerfScope perf_scope(g_perf_text_resolver);
    TextResolverFn original = g_original_text_resolver.load(std::memory_order_acquire);
    if (!original) {
        // MH_CreateHook publishes the trampoline before the queued hook is
        // enabled. This branch is defensive and should never be reached.
        return nullptr;
    }

    erui::detail::RuntimeState& runtime = erui::detail::runtime_state();
    const std::uint32_t entry =
        g_text_entry_count.fetch_add(1, std::memory_order_relaxed) + 1;

    std::uint32_t lookup_id = message_id;
    if (message_id == kModSettingsNativeTitleId &&
        mod_settings_page_registered()) {
        const std::uint32_t title_id = mod_settings_page_title_id();
        if (title_id != 0) lookup_id = title_id;
    }
    if (root_button_text_override.active) {
        if (message_id == root_button_native_label_id) {
            lookup_id = root_button_text_override.label_id;
        } else if (message_id == root_button_native_help_id) {
            lookup_id = root_button_text_override.help_id;
        }
    }

    const wchar_t* replacement = runtime.options.enable_custom_text && runtime.menu
        ? runtime.menu->texts.lookup(static_cast<int>(lookup_id))
        : nullptr;
    if (!replacement && runtime.options.enable_custom_text) {
        replacement = lookup_native_dialog_text(message_id);
    }

    if (!replacement) {
        void* result{};
        {
            HookPerfScope game(g_perf_text_resolver, true);
            result = original(
                text_result,
                argument2,
                message_id,
                argument4,
                argument5);
        }

        if (erui::detail::message_trace_logging()) {
            const std::uint32_t category = diagnostic_category();
            const std::uint32_t context = diagnostic_menu_context();
            if (claim_diagnostic_text_id(
                    g_text_resolver_logged_ids,
                    context,
                    category,
                    message_id)) {
                char resolved_text[512]{};
                bool truncated = false;
                const bool text_read = copy_native_text_result_utf8(
                    text_result,
                    resolved_text,
                    sizeof(resolved_text),
                    truncated);
                erui::detail::logf(
                    erui::LogLevel::trace,
                    "diagnostic: textResolver[%u] menuContext=%s category=%u categoryName=%s categorySource=%s resultObject=%p a2=0x%llX id=%u/0x%X a4=0x%llX a5=0x%llX return=%p customText=%u resolvedTextRead=%u resolvedTextTruncated=%u resolvedTextUtf8=\"%s\"",
                    static_cast<unsigned>(entry),
                    context == 2 ? "in-game" : (context == 1 ? "title-menu" : "unknown"),
                    static_cast<unsigned>(category),
                    native_category_name(category),
                    t_diagnostic_native_category < kDiagnosticCategoryCount
                        ? "native-materializer" : "selected-tab",
                    text_result,
                    static_cast<unsigned long long>(argument2),
                    static_cast<unsigned>(message_id),
                    static_cast<unsigned>(message_id),
                    static_cast<unsigned long long>(argument4),
                    static_cast<unsigned long long>(argument5),
                    result,
                    runtime.options.enable_custom_text ? 1u : 0u,
                    text_read ? 1u : 0u,
                    truncated ? 1u : 0u,
                    text_read ? resolved_text : "<unreadable>");
            }
        }
        return result;
    }

    // Match the native-UI technique observed in Solid Uncapper: ask Elden
    // Ring to build a valid result object using a known vanilla message, then
    // replace the result object's UTF-16 pointer/storage/length with our
    // stable registry string.
    void* result{};
    {
        HookPerfScope game(g_perf_text_resolver, true);
        result = original(
            text_result,
            argument2,
            kCustomTextFallbackId,
            argument4,
            argument5);
    }

    const erui::detail::NativeTextPatchOutcome patch =
        erui::detail::patch_native_text_result(text_result, replacement);
    const std::uint64_t hit =
        runtime.custom_text_hits.fetch_add(1, std::memory_order_relaxed) + 1;

    if (patch.status == erui::detail::NativeTextPatchStatus::access_violation) {
        const std::uint32_t fault =
            g_text_fault_count.fetch_add(1, std::memory_order_relaxed) + 1;
        if (fault <= 16) {
            erui::detail::logf(
                erui::LogLevel::error,
                "custom text patch fault=%u hit=%llu id=0x%X resultObject=%p return=%p",
                static_cast<unsigned>(fault),
                static_cast<unsigned long long>(hit),
                static_cast<unsigned>(message_id),
                text_result,
                result);
        }
    }

    if (erui::detail::message_trace_logging()) {
        const std::uint32_t category = diagnostic_category();
        const std::uint32_t context = diagnostic_menu_context();
        if (claim_diagnostic_text_id(
                g_custom_text_logged_ids, context, category, message_id)) {
            const char* mode = "invalid";
            switch (patch.status) {
            case erui::detail::NativeTextPatchStatus::direct_pointer_only:
                mode = "direct";
                break;
            case erui::detail::NativeTextPatchStatus::direct_pointer_and_buffer:
                mode = "direct+buffer";
                break;
            case erui::detail::NativeTextPatchStatus::access_violation:
                mode = "fault";
                break;
            case erui::detail::NativeTextPatchStatus::invalid_argument:
                break;
            }
            char resolved_text[512]{};
            bool truncated = false;
            const bool text_read = copy_native_text_result_utf8(
                text_result,
                resolved_text,
                sizeof(resolved_text),
                truncated);
            erui::detail::logf(
                erui::LogLevel::trace,
                "diagnostic: custom text hit=%llu menuContext=%s category=%u categoryName=%s categorySource=%s id=0x%X object=%p return=%p patch=%s length=%zu capacity=%llu resolvedTextRead=%u resolvedTextTruncated=%u resolvedTextUtf8=\"%s\"",
                static_cast<unsigned long long>(hit),
                context == 2 ? "in-game" : (context == 1 ? "title-menu" : "unknown"),
                static_cast<unsigned>(category),
                native_category_name(category),
                t_diagnostic_native_category < kDiagnosticCategoryCount
                    ? "native-materializer" : "selected-tab",
                static_cast<unsigned>(message_id),
                text_result,
                result,
                mode,
                patch.text_length,
                static_cast<unsigned long long>(patch.native_capacity),
                text_read ? 1u : 0u,
                truncated ? 1u : 0u,
                text_read ? resolved_text : "<unreadable>");
        }
    }

    return result;
}

void __fastcall game_options_handler_detour(
    void* page,
    std::uintptr_t argument2,
    std::uintptr_t argument3,
    std::uintptr_t argument4,
    std::uintptr_t argument5,
    std::uintptr_t argument6) noexcept {
    GameOptionsHandlerFn original =
        g_original_game_options.load(std::memory_order_acquire);
    for (unsigned attempt = 0; !original && attempt < 64; ++attempt) {
        Sleep(0);
        original = g_original_game_options.load(std::memory_order_acquire);
    }
    if (!original) {
        return;
    }

    const std::uint32_t entry =
        g_game_options_entry_count.fetch_add(1, std::memory_order_relaxed) + 1;
    const bool trace_this_call = erui::detail::trace_logging() && entry <= 64;
    erui::detail::RuntimeState& runtime = erui::detail::runtime_state();
    if (trace_this_call) {
        erui::detail::logf(
            erui::LogLevel::trace,
            "gameOptions[%u] enter page=%p original=%p a2=0x%llX a3=0x%llX a4=0x%llX a5=0x%llX a6=0x%llX",
            static_cast<unsigned>(entry),
            page,
            reinterpret_cast<void*>(original),
            static_cast<unsigned long long>(argument2),
            static_cast<unsigned long long>(argument3),
            static_cast<unsigned long long>(argument4),
            static_cast<unsigned long long>(argument5),
            static_cast<unsigned long long>(argument6));
    }

    if (g_mod_settings_tab_hooks_ready.load(std::memory_order_acquire) &&
        t_mod_settings_route_depth != 0) {
        const std::size_t logical_page_index = runtime.menu
            ? runtime.menu->builtin_page_index(kModSettingsNativeCategory)
            : erui::detail::invalid_compiled_index;
        NativeRootSnapshot before = inspect_native_root(page);
        log_scoped_panel_identity(page, "route-enter");
        if (!before.valid && before.materialized_rows == 0) {
            // Solid Uncapper 2.3+ re-tags the option-setting movie so this
            // shell reports a single visual row (its own one-item list),
            // although it is still the genuine, empty Game Options panel and
            // the native list scrolls. Accept it with the vanilla capacity.
            const BuiltinPanelSnapshot panel = inspect_builtin_panel(page);
            if (panel.fields_read && panel.vtable_match &&
                panel.visual_capacity >= 1 && panel.visual_capacity < 4 &&
                panel.native_count == 0) {
                erui::detail::logf(
                    erui::LogLevel::info,
                    "Scoped Mods panel reports visualRows=%u (layout replaced by another mod); using the vanilla capacity of 6",
                    static_cast<unsigned>(panel.visual_capacity));
                before.visual_capacity = 6;
                before.valid = true;
            }
        }
        const bool page_ready = runtime.options.enable_row_injection &&
            runtime.menu && page &&
            logical_page_index < runtime.menu->pages.size() &&
            !runtime.menu->pages[logical_page_index].rows.empty() &&
            before.valid && before.materialized_rows == 0;
        if (!page_ready) {
            erui::detail::logf(
                erui::LogLevel::warning,
                "Scoped Mods panel did not match the empty type-2 Game Options shell: page=%p menu=%u logicalPage=%zu nativeRows=%llu visualRows=%u valid=%u; forwarding native Game Options rows as a safe fallback",
                page,
                runtime.menu ? 1u : 0u,
                logical_page_index,
                static_cast<unsigned long long>(before.materialized_rows),
                static_cast<unsigned>(before.visual_capacity),
                before.valid ? 1u : 0u);
            original(page, argument2, argument3, argument4, argument5, argument6);
            log_scoped_panel_identity(page, "fallback-after-native");
            return;
        }

        // The factory has initialized a Game Options-compatible page shell,
        // but no vanilla rows are materialized yet. Use its full visual
        // capacity for the dedicated Hub page and do not append the normal
        // Game Options rows or the root-page provider rows.
        g_dialog_input_page.store(page, std::memory_order_release);
        const erui::detail::PageRoute route =
            erui::detail::PageRoute::builtin_main(
                logical_page_index,
                kModSettingsNativeCategory,
                static_cast<std::uint8_t>(before.visual_capacity));
        const RowInjectionOutcome outcome = inject_page_route(
            page, route, g_addresses);
        const NativeRootSnapshot after = inspect_native_root(page);
        // The layout-replaced shell keeps reporting its foreign capacity, so
        // only the row count proves the commit there.
        const bool after_valid = after.valid ||
            (before.valid && after.materialized_rows <= 64);
        const bool committed = after_valid && outcome.faulted == 0 &&
            after.materialized_rows == before.materialized_rows + outcome.added;
        erui::detail::logf(
            committed ? erui::LogLevel::info : erui::LogLevel::error,
            "scoped Mod Settings Game Options shell: page=%p logicalPage=%zu nativeRowsBefore=%llu nativeRowsAfter=%llu visualRows=%u capacity=%u attempted=%u added=%u navigation=%u faulted=%u committed=%u",
            page,
            logical_page_index,
            static_cast<unsigned long long>(before.materialized_rows),
            static_cast<unsigned long long>(after.materialized_rows),
            static_cast<unsigned>(before.visual_capacity),
            static_cast<unsigned>(route.root_capacity),
            static_cast<unsigned>(outcome.attempted),
            static_cast<unsigned>(outcome.added),
            static_cast<unsigned>(outcome.navigation),
            static_cast<unsigned>(outcome.faulted),
            committed ? 1u : 0u);
        return;
    }

    original(page, argument2, argument3, argument4, argument5, argument6);

    log_native_root_state(entry, page, "after-native-before-hub");
    if (trace_this_call) {
        erui::detail::logf(
            erui::LogLevel::trace,
            "gameOptions[%u] original returned page=%p a2=0x%llX a3=0x%llX a4=0x%llX a5=0x%llX a6=0x%llX",
            static_cast<unsigned>(entry),
            page,
            static_cast<unsigned long long>(argument2),
            static_cast<unsigned long long>(argument3),
            static_cast<unsigned long long>(argument4),
            static_cast<unsigned long long>(argument5),
            static_cast<unsigned long long>(argument6));
    }

    if (page) show_pending_menu_warning();
    if (!runtime.options.enable_row_injection || !runtime.menu || !page) {
        return;
    }
    if (runtime.menu->root_page().rows.empty()) {
        return;
    }

    // The shared root is a live native page too. Subpages publish this pointer
    // in sub_handler_detour, but a dialog opened directly from the initial
    // root never visits that path. Publish before the injection cooldown so
    // page-frame and Back gates can match root-origin alerts as well.
    g_dialog_input_page.store(page, std::memory_order_release);

    const std::uint64_t now = GetTickCount64();
    const void* previous_page = g_last_page.load(std::memory_order_relaxed);
    const std::uint64_t previous_tick =
        g_last_injection_tick.load(std::memory_order_relaxed);
    const std::uint64_t cooldown = runtime.options.injection_cooldown_ms;
    if (previous_page == page && now >= previous_tick && now - previous_tick < cooldown) {
        if (trace_this_call) {
            erui::detail::logf(
                erui::LogLevel::trace,
                "gameOptions[%u] row injection skipped by cooldown",
                static_cast<unsigned>(entry));
        }
        return;
    }

    g_last_page.store(page, std::memory_order_relaxed);
    g_last_injection_tick.store(now, std::memory_order_relaxed);

    const std::uint32_t injection =
        g_game_options_injection_count.fetch_add(
            1,
            std::memory_order_relaxed) + 1;
    const RowInjectionOutcome outcome = inject_registered_rows(page, g_addresses);
    const std::uint32_t log_limit = erui::detail::diagnostics_enabled() ? 128u : 4u;
    if (injection <= log_limit || outcome.faulted != 0) {
        erui::detail::logf(
            outcome.faulted == 0 ? erui::LogLevel::trace : erui::LogLevel::error,
            "injection[%u] page=%p route=%s logicalPage=%zu slice=%zu/%zu capacity=%u attempted=%u added=%u navigation=%u faulted=%u skipped=%u",
            static_cast<unsigned>(injection),
            page,
            route_kind_name(outcome.route.kind),
            outcome.route.logical_page_index,
            outcome.slice_index + 1,
            outcome.slice_count,
            static_cast<unsigned>(outcome.root_capacity),
            static_cast<unsigned>(outcome.attempted),
            static_cast<unsigned>(outcome.added),
            static_cast<unsigned>(outcome.navigation),
            static_cast<unsigned>(outcome.faulted),
            static_cast<unsigned>(outcome.skipped));
    }
}

const char* submenu_resolution_name(
    erui::detail::SubmenuResolutionKind kind) noexcept {
    switch (kind) {
    case erui::detail::SubmenuResolutionKind::none:
        return "none";
    case erui::detail::SubmenuResolutionKind::pending_request:
        return "pending";
    case erui::detail::SubmenuResolutionKind::bound_page:
        return "bound";
    case erui::detail::SubmenuResolutionKind::expired_request:
        return "expired";
    }
    return "unknown";
}

void __fastcall sub_handler_detour(
    void* page,
    std::uintptr_t argument2,
    std::uintptr_t argument3,
    std::uintptr_t argument4) noexcept {
    SubHandlerFn original = g_original_sub.load(std::memory_order_acquire);
    for (unsigned attempt = 0; !original && attempt < 64; ++attempt) {
        Sleep(0);
        original = g_original_sub.load(std::memory_order_acquire);
    }
    if (!original) {
        return;
    }

    erui::detail::RuntimeState& runtime = erui::detail::runtime_state();
    const std::uint32_t entry =
        g_sub_entry_count.fetch_add(1, std::memory_order_relaxed) + 1;
    if (runtime.options.enable_diagnostics && entry <= 64) {
        erui::detail::logf(
            erui::LogLevel::trace,
            "subHandler[%u] enter page=%p original=%p a2=0x%llX a3=0x%llX a4=0x%llX",
            static_cast<unsigned>(entry),
            page,
            reinterpret_cast<void*>(original),
            static_cast<unsigned long long>(argument2),
            static_cast<unsigned long long>(argument3),
            static_cast<unsigned long long>(argument4));
    }
    const std::uint64_t now = GetTickCount64();
    const erui::detail::SubmenuResolution resolution =
        g_submenu_navigation.resolve(page, now, kSubmenuRequestTimeoutMs);

    if (resolution.kind == erui::detail::SubmenuResolutionKind::expired_request) {
        g_title_capture.reset();
        if (runtime.options.enable_diagnostics && entry <= kSubmenuDiagnosticLogLimit) {
            erui::detail::logf(
                erui::LogLevel::warning,
                "diagnostic: subHandler[%u] expired pending route before page=%p; forwarding original",
                static_cast<unsigned>(entry),
                page);
        }
        log_vanilla_subpage_state(entry, page, "before-native");
        reset_color_picker_page_widgets(
            page, ColorPickerWidgetHost::subpage);
        original(page, argument2, argument3, argument4);
        log_vanilla_subpage_state(entry, page, "after-native");
        return;
    }

    const erui::detail::PageRoute route = resolution.route;
    const CapturedTitleTarget captured_title =
        resolution.kind == erui::detail::SubmenuResolutionKind::pending_request
        ? g_title_capture.consume(route)
        : (g_title_capture.reset(), CapturedTitleTarget{});
    erui::detail::PageSlice resolved_slice{};
    const bool slice_resolved = runtime.menu &&
        resolve_page_route_slice(*runtime.menu, route, resolved_slice);
    if (resolution.kind == erui::detail::SubmenuResolutionKind::none ||
        !runtime.options.enable_row_injection || !runtime.menu ||
        !slice_resolved) {
        if (runtime.options.enable_diagnostics && entry <= 32) {
            const erui::detail::PageRoute pending =
                g_submenu_navigation.pending_route();
            erui::detail::logf(
                erui::LogLevel::trace,
                "diagnostic: subHandler[%u] vanilla page=%p pendingKind=%s pendingLogical=%zu pendingSlice=%zu; forwarding original",
                static_cast<unsigned>(entry),
                page,
                route_kind_name(pending.kind),
                pending.logical_page_index,
                pending.slice_index);
        }
        log_vanilla_subpage_state(entry, page, "before-native");
        reset_color_picker_page_widgets(
            page, ColorPickerWidgetHost::subpage);
        original(page, argument2, argument3, argument4);
        log_vanilla_subpage_state(entry, page, "after-native");
        return;
    }

    g_dialog_input_page.store(page, std::memory_order_release);

    const erui::detail::CompiledPage& compiled_page =
        runtime.menu->pages[route.logical_page_index];
    const bool route_matches_page =
        (route.kind == erui::detail::PageRouteKind::submenu &&
            compiled_page.kind == erui::PageKind::submenu) ||
        (route.kind == erui::detail::PageRouteKind::root_continuation &&
            route.logical_page_index == runtime.menu->root_page_index) ||
        (route.kind ==
                erui::detail::PageRouteKind::builtin_continuation &&
            runtime.menu->builtin_page_index(route.builtin_category) ==
                route.logical_page_index) ||
        (route.kind ==
                erui::detail::PageRouteKind::tab_continuation &&
            compiled_page.kind == erui::PageKind::tab);
    if (!route_matches_page) {
        erui::detail::logf(
            erui::LogLevel::error,
            "subHandler[%u] route kind=%s logicalPage=%zu does not match compiled page kind; forwarding original",
            static_cast<unsigned>(entry),
            route_kind_name(route.kind),
            route.logical_page_index);
        log_vanilla_subpage_state(entry, page, "before-native");
        reset_color_picker_page_widgets(
            page, ColorPickerWidgetHost::subpage);
        original(page, argument2, argument3, argument4);
        log_vanilla_subpage_state(entry, page, "after-native");
        return;
    }

    const std::uint64_t hit =
        runtime.submenu_page_hits.fetch_add(1, std::memory_order_relaxed) + 1;
    if (!g_submenu_navigation.should_inject(
            route,
            page,
            now,
            runtime.options.injection_cooldown_ms)) {
        if (runtime.options.enable_diagnostics && hit <= kSubmenuDiagnosticLogLimit) {
            erui::detail::logf(
                erui::LogLevel::trace,
                "diagnostic: custom subHandler[%llu] page=%p route=%s logicalPage=%zu slice=%zu skipped by cooldown",
                static_cast<unsigned long long>(hit),
                page,
                route_kind_name(route.kind),
                route.logical_page_index,
                route.slice_index);
        }
        return;
    }

    const std::uint32_t injection =
        g_sub_injection_count.fetch_add(1, std::memory_order_relaxed) + 1;
    const RowInjectionOutcome outcome = inject_page_route(page, route, g_addresses);
    const erui::detail::ResolvedPagePresentation presentation =
        runtime.menu->resolve_presentation(route);
    const wchar_t* const outer_title = runtime.menu->texts.lookup(
        static_cast<int>(presentation.outer_title_id));
    const wchar_t* const page_title = runtime.menu->texts.lookup(
        static_cast<int>(presentation.page_title_id));
    apply_physical_page_titles(captured_title, outer_title, page_title);
    const std::uint32_t log_limit = runtime.options.enable_diagnostics ? 192u : 8u;
    if (injection <= log_limit || outcome.faulted != 0) {
        erui::detail::logf(
            outcome.faulted == 0 ? erui::LogLevel::trace : erui::LogLevel::error,
            "physicalPageMaterialize[%u] page=%p source=%s route=%s logicalPage=%zu slice=%zu/%zu firstRow=%zu contentRows=%zu attempted=%u added=%u navigation=%u faulted=%u skipped=%u",
            static_cast<unsigned>(injection),
            page,
            submenu_resolution_name(resolution.kind),
            route_kind_name(route.kind),
            route.logical_page_index,
            outcome.slice_index + 1,
            outcome.slice_count,
            outcome.first_row,
            outcome.content_rows,
            static_cast<unsigned>(outcome.attempted),
            static_cast<unsigned>(outcome.added),
            static_cast<unsigned>(outcome.navigation),
            static_cast<unsigned>(outcome.faulted),
            static_cast<unsigned>(outcome.skipped));
    }
}

void __fastcall page_frame_gate_detour(
    void* page,
    float frame_value,
    std::uint8_t* input_enabled) noexcept {
    // TextInput setters may run on the host/client thread. Publish their
    // canonical values into the native MenuString shadow only from this UI
    // frame boundary, including while a native alert owns page input.
    synchronize_pending_text_inputs();
    synchronize_color_picker_previews(page);

    PageFrameFn original = g_original_page_frame.load(std::memory_order_acquire);
    if (!original) return;
    if (native_dialog_owns_menu_input() &&
        page == g_dialog_input_page.load(std::memory_order_acquire)) {
        return;
    }
    original(page, frame_value, input_enabled);
    tick_marquee(page);
    refresh_dirty_native_values();
}

bool install_page_frame_gate(const GameAddresses& addresses) noexcept {
    if (!addresses.page_frame) return false;
    if (g_original_page_frame.load(std::memory_order_acquire)) {
        return true;
    }
    auto result = SafetyHookInline::create(
        reinterpret_cast<void*>(addresses.page_frame),
        reinterpret_cast<void*>(&page_frame_gate_detour),
        SafetyHookInline::StartDisabled);
    if (!result) {
        erui::detail::logf(erui::LogLevel::warning,
            "Page frame gate could not be prepared (SafetyHook error type %u)",
            static_cast<unsigned>(result.error().type));
        return false;
    }
    g_page_frame_gate_hook = std::move(*result);
    g_original_page_frame.store(
        g_page_frame_gate_hook.original<PageFrameFn>(), std::memory_order_release);
    if (auto enabled = g_page_frame_gate_hook.enable(); !enabled) {
        erui::detail::logf(erui::LogLevel::warning,
            "Page frame gate could not be enabled (SafetyHook error type %u)",
            static_cast<unsigned>(enabled.error().type));
        g_page_frame_gate_hook.reset();
        g_original_page_frame.store(nullptr, std::memory_order_release);
        return false;
    }
    erui::detail::logf(erui::LogLevel::info,
        "Page-frame UI pump/input gate installed");
    return true;
}

void __fastcall dialog_back_gate_detour(
    void* page,
    std::uint64_t action) noexcept {
    NativeBackFn original = g_original_dialog_back.load(std::memory_order_acquire);
    if (!original) return;
    // Action 3 is both menu Back and the right/secondary action of a native
    // two-button dialog. The popup action is emitted synchronously from the
    // owned CSPopupMenu update; suppress action 3 everywhere else so it cannot
    // navigate the page underneath the dialog.
    const bool owns_dialog = native_dialog_owns_menu_input();
    const bool popup_scope = native_dialog_dispatching_popup_input();
    const bool suppress = should_suppress_native_back(
        owns_dialog, popup_scope, action);
    if (suppress) {
        return;
    }
    void* const restored_parent = action == kNativeBackAction
        ? g_submenu_navigation.parent_page(page)
        : nullptr;
    original(page, action);
    if (restored_parent) {
        g_submenu_navigation.unbind_native_page(page);
        g_dialog_input_page.store(restored_parent, std::memory_order_release);
    }
}

bool install_dialog_back_gate(const GameAddresses& addresses) noexcept {
    if (!addresses.native_back) return false;
    auto result = SafetyHookInline::create(
        reinterpret_cast<void*>(addresses.native_back),
        reinterpret_cast<void*>(&dialog_back_gate_detour),
        SafetyHookInline::StartDisabled);
    if (!result) {
        erui::detail::logf(erui::LogLevel::warning,
            "Dialog Back gate could not be prepared (SafetyHook error type %u)",
            static_cast<unsigned>(result.error().type));
        return false;
    }
    g_dialog_back_gate_hook = std::move(*result);
    g_original_dialog_back.store(
        g_dialog_back_gate_hook.original<NativeBackFn>(), std::memory_order_release);
    if (auto enabled = g_dialog_back_gate_hook.enable(); !enabled) {
        erui::detail::logf(erui::LogLevel::warning,
            "Dialog Back gate could not be enabled (SafetyHook error type %u)",
            static_cast<unsigned>(enabled.error().type));
        g_dialog_back_gate_hook.reset();
        g_original_dialog_back.store(nullptr, std::memory_order_release);
        return false;
    }
    erui::detail::logf(erui::LogLevel::info,
        "Dialog native Back input gate installed");
    return true;
}

void reset_dialog_back_gate() noexcept {
    g_dialog_back_gate_hook.reset();
    g_original_dialog_back.store(nullptr, std::memory_order_release);
}

void reset_page_frame_gate() noexcept {
    g_page_frame_gate_hook.reset();
    g_original_page_frame.store(nullptr, std::memory_order_release);
}

void reset_dialog_input_gates(bool preserve_page_frame) noexcept {
    reset_dialog_back_gate();
    if (!preserve_page_frame) {
        reset_page_frame_gate();
    }
}

bool install_dialog_input_gates(
    const GameAddresses& addresses,
    bool preserve_page_frame) noexcept {
    reset_dialog_back_gate();
    if (!addresses.page_frame || !addresses.native_back) {
        erui::detail::logf(
            erui::LogLevel::warning,
            "Native alert input ownership unavailable: page-frame or Back interface unresolved");
        if (!preserve_page_frame) {
            reset_page_frame_gate();
        }
        return false;
    }
    if (!install_page_frame_gate(addresses) ||
        !install_dialog_back_gate(addresses)) {
        reset_dialog_input_gates(preserve_page_frame);
        return false;
    }
    return true;
}

bool revalidate_shared_menu_entry_before_hook(
    const ModuleView* game,
    const PreservedSharedMenuAddresses* preserved,
    PreservedSharedMenuEntry entry,
    const char* name) noexcept {
    if (!preserved) return true;
    if (game && revalidate_preserved_shared_menu_entry(
            *game, *preserved, entry)) {
        return true;
    }
    erui::detail::logf(
        erui::LogLevel::error,
        "Approved shared menu entry '%s' changed immediately before hook creation",
        name ? name : "unknown");
    return false;
}

bool install_game_options_hook(
    const GameAddresses& addresses,
    const ModuleView* game,
    const PreservedSharedMenuAddresses* preserved) noexcept {
    if (erui::detail::diagnostics_enabled()) {
        const auto* code = reinterpret_cast<const unsigned char*>(
            addresses.game_options_handler);
        erui::detail::logf(
            erui::LogLevel::trace,
            "diagnostic: Game Options entry before hook=%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
            code[0], code[1], code[2], code[3], code[4], code[5], code[6], code[7],
            code[8], code[9], code[10], code[11], code[12], code[13], code[14], code[15]);
    }
    if (!revalidate_shared_menu_entry_before_hook(
            game,
            preserved,
            PreservedSharedMenuEntry::game_options,
            "Game Options")) {
        return false;
    }
    auto game_options_result = SafetyHookInline::create(
        reinterpret_cast<void*>(addresses.game_options_handler),
        reinterpret_cast<void*>(&game_options_handler_detour),
        SafetyHookInline::StartDisabled);
    if (!game_options_result) {
        erui::detail::logf(
            erui::LogLevel::error,
            "SafetyHook failed to prepare Game Options handler hook (error type %u)",
            static_cast<unsigned>(game_options_result.error().type));
        return false;
    }

    g_game_options_hook = std::move(*game_options_result);
    g_original_game_options.store(
        g_game_options_hook.original<GameOptionsHandlerFn>(),
        std::memory_order_release);
    if (auto enable_result = g_game_options_hook.enable(); !enable_result) {
        erui::detail::logf(
            erui::LogLevel::error,
            "SafetyHook failed to enable Game Options handler hook (error type %u)",
            static_cast<unsigned>(enable_result.error().type));
        return false;
    }

    erui::detail::logf(
        erui::LogLevel::info,
        "Hook installed: Game Options handler (six forwarded ABI slots)");
    if (erui::detail::diagnostics_enabled()) {
        erui::detail::logf(
            erui::LogLevel::trace,
            "diagnostic: Game Options hook target=%p detour=%p trampoline=%p",
            reinterpret_cast<void*>(addresses.game_options_handler),
            reinterpret_cast<void*>(&game_options_handler_detour),
            reinterpret_cast<void*>(
                g_original_game_options.load(std::memory_order_acquire)));
    }
    return true;
}

#if defined(ER_NATIVE_UI_CUSTOM_TABS_BACKEND)
template <std::size_t Size>
bool tab_content_bytes_match(
    const std::uint8_t* address,
    const std::array<std::uint8_t, Size>& expected) noexcept {
    if (!address) return false;
#if defined(_MSC_VER)
    __try {
#endif
        return std::memcmp(address, expected.data(), expected.size()) == 0;
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#endif
}

bool tab_carrier_signatures_match(
    const GameAddresses& addresses,
    const TabCarrierBuildProfile** matched_profile = nullptr) noexcept {
    if (matched_profile) {
        *matched_profile = nullptr;
    }
    // Category 8 (Graphics). Native Graphics preserves
    // both callbacks. Each 16-byte descriptor contains an eight-byte callback,
    // four-byte metadata field and four bytes of padding. A validated custom
    // route substitutes the callback at stack+0x60 and clears only the
    // authored callback/metadata fields at stack+0x50.
    constexpr std::array<std::uint8_t, 7> extra_callback{
        0x48, 0x8D, 0x05, 0xF9, 0xD8, 0x01, 0x00};
    constexpr std::array<std::uint8_t, 7> row_callback{
        0x48, 0x8D, 0x05, 0x17, 0xF1, 0x01, 0x00};
    constexpr std::array<std::uint8_t, 13> builder{
        0x48, 0x8D, 0x8C, 0x24, 0x80, 0x00, 0x00, 0x00,
        0xE8, 0xCB, 0x04, 0x00, 0x00};
    constexpr std::array<std::uint8_t, 23> route{
        0xC6, 0x44, 0x24, 0x20, 0x0B,
        0x44, 0x0F, 0xB6, 0xCF, 0x4C, 0x8B, 0xC3,
        0x48, 0x8B, 0xD0, 0x48, 0x8B, 0xCE,
        0xE8, 0x95, 0x2D, 0x02, 0x00};
    constexpr std::array<std::uint8_t, 16> teardown_entry{
        0x48, 0x89, 0x4C, 0x24, 0x08, 0x55, 0x56, 0x57,
        0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x30};
    constexpr std::array<std::uint8_t, 16> deactivate_entry{
        0x48,0x89,0x5C,0x24,0x08,0x57,0x48,0x83,
        0xEC,0x20,0x8B,0xDA,0x48,0x8B,0xF9,0xE8};
    if (!addresses.game_image_base) {
        return false;
    }

    bool exact_image{};
    const TabCarrierBuildProfile* profile{};
#if defined(_MSC_VER)
    __try {
#endif
        const auto* const dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(
            addresses.game_image_base);
        if (dos->e_magic == IMAGE_DOS_SIGNATURE && dos->e_lfanew > 0 &&
            static_cast<std::size_t>(dos->e_lfanew) +
                    sizeof(IMAGE_NT_HEADERS64) <= addresses.game_image_size) {
            const auto* const nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
                addresses.game_image_base + dos->e_lfanew);
            const bool valid_pe = nt->Signature == IMAGE_NT_SIGNATURE &&
                nt->FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64 &&
                nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC &&
                nt->OptionalHeader.SizeOfImage == addresses.game_image_size;
            if (valid_pe) {
                profile = find_tab_carrier_build_profile(
                    nt->FileHeader.TimeDateStamp,
                    nt->OptionalHeader.SizeOfImage);
                exact_image = profile != nullptr;
            }
        }
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        exact_image = false;
    }
#endif
    if (!exact_image || !profile ||
        reinterpret_cast<void*>(addresses.graphics_panel_materializer) !=
            addresses.game_image_base +
                profile->category8_materializer_rva ||
        !tab_content_bytes_match(
            addresses.game_image_base +
                profile->extra_callback_anchor_rva,
            extra_callback) ||
        !tab_content_bytes_match(
            addresses.game_image_base + profile->row_callback_anchor_rva,
            row_callback) ||
        !tab_content_bytes_match(
            addresses.game_image_base + profile->builder_anchor_rva,
            builder) ||
        !tab_content_bytes_match(
            addresses.game_image_base + profile->route_anchor_rva,
            route) ||
        !tab_content_bytes_match(
            addresses.game_image_base + profile->composite_teardown_rva,
            teardown_entry) ||
        !tab_content_bytes_match(
            addresses.game_image_base + profile->carrier_manager_rva,
            profile->manager_entry) ||
        !tab_content_bytes_match(
            addresses.game_image_base + profile->carrier_deactivate_rva,
            deactivate_entry)) {
        return false;
    }
    if (matched_profile) {
        *matched_profile = profile;
    }
    return true;
}

bool install_tab_content_carrier_hooks(
    const GameAddresses& addresses) noexcept {
    if (g_tab_content_hooks_exposed.load(std::memory_order_acquire)) {
        erui::detail::logf(erui::LogLevel::error,
            "Native-tab Graphics-carrier hooks were already exposed; refusing a second installation attempt");
        return false;
    }
    const TabCarrierBuildProfile* profile{};
    if (!tab_carrier_signatures_match(addresses, &profile)) {
        erui::detail::logf(erui::LogLevel::error,
            "Native-tab Graphics-carrier build/signatures differed; no carrier hooks installed");
        return false;
    }
    if (!profile) return false;

    auto pre8 = SafetyHookMid::create(
        addresses.game_image_base + profile->category8_pre_rva,
        &begin_tab_content_category8_route,
        SafetyHookMid::StartDisabled);
    if (!pre8) return false;
    g_tab_content_category8_pre_hook = std::move(*pre8);

    auto teardown = SafetyHookInline::create(
        addresses.game_image_base + profile->composite_teardown_rva,
        reinterpret_cast<void*>(&tab_content_composite_teardown_detour),
        SafetyHookInline::StartDisabled);
    if (!teardown) {
        g_tab_content_category8_pre_hook.reset();
        return false;
    }
    g_tab_content_composite_teardown_hook = std::move(*teardown);
    g_original_tab_content_composite_teardown.store(
        g_tab_content_composite_teardown_hook
            .original<TabCarrierCompositeTeardownFn>(),
        std::memory_order_release);

    // A callback may enter as soon as the first patch is published. From this
    // point forward the hook objects and original trampoline are retained for
    // the process lifetime. Until both patches are known enabled, the inline
    // detour is exact pass-through and the mid hook is a no-op.
    g_tab_carrier_build_profile = profile;
    g_tab_content_callbacks_armed.store(false, std::memory_order_release);
    g_tab_content_hooks_exposed.store(true, std::memory_order_release);
    if (!g_tab_content_composite_teardown_hook.enable()) {
        erui::detail::logf(erui::LogLevel::error,
            "Native-tab Graphics-carrier teardown hook could not be enabled; retaining the unarmed process-lifetime trampoline");
        return false;
    }
    if (!g_tab_content_category8_pre_hook.enable()) {
        erui::detail::logf(erui::LogLevel::error,
            "Native-tab Graphics-carrier pre-hook could not be enabled; retaining unarmed process-lifetime hooks");
        return false;
    }
    g_tab_content_callbacks_armed.store(true, std::memory_order_release);

    erui::detail::logf(erui::LogLevel::info,
        "Native-tab Graphics carrier installed category=8 selector=11 pre=+0x%llX nativeRows=+0x%llX extraCallback=+0x%llX policy={native:preserve custom:validated-zero} teardown=+0x%llX parking=stock-category5 synchronous=1",
        static_cast<unsigned long long>(profile->category8_pre_rva),
        static_cast<unsigned long long>(
            profile->category8_materializer_rva),
        static_cast<unsigned long long>(
            profile->category8_extra_callback_rva),
        static_cast<unsigned long long>(
            profile->composite_teardown_rva));
    return true;
}

#endif

template <typename Function>
bool install_panel_materializer_hook(
    const char* panel_name,
    std::uint8_t native_category,
    Function target,
    void* detour,
    SafetyHookInline& hook,
    std::atomic<Function>& original,
    bool allow_diagnostic_observer_without_rows = false) noexcept {
    const erui::detail::RuntimeState& runtime = erui::detail::runtime_state();
    const erui::detail::CompiledPage* const page = runtime.menu
        ? runtime.menu->builtin_page(native_category)
        : nullptr;
    const bool has_rows = page && !page->rows.empty();
    const bool diagnostics = runtime.options.enable_diagnostics;
    const bool diagnostic_observer_only = !has_rows && diagnostics &&
        allow_diagnostic_observer_without_rows;
    if (!has_rows && !diagnostic_observer_only) {
        if (diagnostics) {
            erui::detail::logf(
                erui::LogLevel::info,
                "Built-in %s diagnostic observer skipped; no Hub rows are registered, leaving the vanilla page constructor untouched",
                panel_name);
        }
        return true;
    }

    if (!target) {
        erui::detail::logf(
            erui::LogLevel::warning,
            diagnostic_observer_only
                ? "Built-in %s diagnostic observer unavailable: native panel materializer was not resolved"
                : "Built-in %s rows disabled: native row materializer was not resolved",
            panel_name);
        return false;
    }

    auto result = SafetyHookInline::create(
        reinterpret_cast<void*>(target),
        detour,
        SafetyHookInline::StartDisabled);
    if (!result) {
        erui::detail::logf(
            erui::LogLevel::warning,
            "Built-in %s materializer hook creation failed (SafetyHook error type %u)",
            panel_name,
            static_cast<unsigned>(result.error().type));
        return false;
    }

    hook = std::move(*result);
    original.store(hook.original<Function>(), std::memory_order_release);
    if (auto enabled = hook.enable(); !enabled) {
        erui::detail::logf(
            erui::LogLevel::warning,
            "Built-in %s materializer hook enable failed (SafetyHook error type %u)",
            panel_name,
            static_cast<unsigned>(enabled.error().type));
        hook.reset();
        original.store(nullptr, std::memory_order_release);
        return false;
    }

    erui::detail::logf(
        erui::LogLevel::info,
        "Hook installed: built-in %s %s (native category %u, logical rows=%zu, handler=%p)",
        panel_name,
        diagnostic_observer_only ? "diagnostic observer only" : "row materializer",
        static_cast<unsigned>(native_category),
        page ? page->rows.size() : 0,
        reinterpret_cast<void*>(target));
    if (diagnostic_observer_only) {
        g_builtin_panel_observer_only_mask.fetch_or(
            static_cast<std::uint16_t>(1u << native_category),
            std::memory_order_release);
    }
    return true;
}

void install_builtin_panel_materializers(
    const GameAddresses& addresses) noexcept {
    // Every target is optional and isolated. One incompatible third-party
    // detour or game-update signature disables only that native destination.
    (void)install_panel_materializer_hook(
        "Camera Options",
        1,
        addresses.camera_panel_materializer,
        reinterpret_cast<void*>(&camera_panel_materializer_detour),
        g_camera_panel_materializer_hook,
        g_original_camera_panel_materializer,
        true);
    (void)install_panel_materializer_hook(
        "Display",
        2,
        addresses.display_panel_materializer,
        reinterpret_cast<void*>(&display_panel_materializer_detour),
        g_display_panel_materializer_hook,
        g_original_display_panel_materializer,
        true);
    (void)install_panel_materializer_hook(
        "Sound",
        3,
        addresses.sound_panel_materializer,
        reinterpret_cast<void*>(&sound_panel_materializer_detour),
        g_sound_panel_materializer_hook,
        g_original_sound_panel_materializer,
        true);
    (void)install_panel_materializer_hook(
        "Network",
        5,
        addresses.network_panel_materializer,
        reinterpret_cast<void*>(&network_panel_materializer_detour),
        g_network_panel_materializer_hook,
        g_original_network_panel_materializer,
        true);
    // These observers intentionally stay installed with no Hub rows: their
    // detours forward to the vanilla builder, log its finished page, and the
    // row injector immediately returns when the provider is empty.
    (void)install_panel_materializer_hook(
        "Keyboard/Mouse",
        7,
        addresses.keyboard_mouse_panel_materializer,
        reinterpret_cast<void*>(&keyboard_mouse_panel_materializer_detour),
        g_keyboard_mouse_panel_materializer_hook,
        g_original_keyboard_mouse_panel_materializer,
        true);
    (void)install_panel_materializer_hook(
        "Graphics",
        8,
        addresses.graphics_panel_materializer,
        reinterpret_cast<void*>(&graphics_panel_materializer_detour),
        g_graphics_panel_materializer_hook,
        g_original_graphics_panel_materializer,
        true);
}

template <typename Callback>
bool install_mid_diagnostic_observer(
    const char* name,
    void* target,
    Callback callback,
    SafetyHookMid& hook) noexcept {
    if (!target) {
        erui::detail::logf(
            erui::LogLevel::warning,
            "Diagnostic observer unavailable: exact-build %s target was not resolved",
            name ? name : "unknown");
        return false;
    }
    auto result = SafetyHookMid::create(target, callback);
    if (!result) {
        erui::detail::logf(
            erui::LogLevel::warning,
            "Diagnostic observer for %s could not be installed (SafetyHook error type %u)",
            name ? name : "unknown",
            static_cast<unsigned>(result.error().type));
        return false;
    }
    hook = std::move(*result);
    erui::detail::logf(
        erui::LogLevel::info,
        "Diagnostic observer installed: %s target=%p",
        name ? name : "unknown",
        target);
    return true;
}

void install_native_render_diagnostic_observers(
    const GameAddresses& addresses) noexcept {
    if (!erui::detail::runtime_state().options.enable_diagnostics) return;
    const bool toggle = install_mid_diagnostic_observer(
        "native toggle row builder",
        reinterpret_cast<void*>(addresses.add_toggle),
        &observe_add_toggle,
        g_add_toggle_diagnostic_hook);
    const bool slider = install_mid_diagnostic_observer(
        "native slider row builder",
        reinterpret_cast<void*>(addresses.add_slider),
        &observe_add_slider,
        g_add_slider_diagnostic_hook);
    const bool choice = install_mid_diagnostic_observer(
        "native inline-choice row builder",
        reinterpret_cast<void*>(addresses.add_inline_choice),
        &observe_add_choice,
        g_add_choice_diagnostic_hook);
    const bool button = install_mid_diagnostic_observer(
        "native button row builder",
        reinterpret_cast<void*>(addresses.add_button),
        &observe_add_button,
        g_add_button_diagnostic_hook);
    const bool unclassified_row = install_mid_diagnostic_observer(
        "unclassified native row builder",
        addresses.add_unclassified_row_builder_diagnostic,
        &observe_add_unclassified_row,
        g_add_unclassified_row_diagnostic_hook);
    const bool visibility = install_mid_diagnostic_observer(
        "Scaleform visibility setter",
        addresses.scaleform_visibility_setter,
        &observe_scaleform_visibility,
        g_scaleform_visibility_diagnostic_hook);
    // The wide-help hook already owns this entry point.
    const bool text = !g_original_line_help_text_setter.load(
            std::memory_order_acquire) &&
        install_mid_diagnostic_observer(
        "Scaleform text setter",
        reinterpret_cast<void*>(addresses.scaleform_text_setter),
        &observe_scaleform_text,
        g_scaleform_text_diagnostic_hook);
    erui::detail::logf(
        erui::LogLevel::info,
        "Native UI render diagnostic observers ready: toggle=%u slider=%u inlineChoice=%u button=%u unclassifiedRow=%u scaleformVisibility=%u scaleformVisibilityUniqueLogLimit=%u scaleformText=%u scaleformTextUniqueLogLimit=%u; callbacks are read-only and deduplicate repeated writes per menu context/category",
        toggle ? 1u : 0u,
        slider ? 1u : 0u,
        choice ? 1u : 0u,
        button ? 1u : 0u,
        unclassified_row ? 1u : 0u,
        visibility ? 1u : 0u,
        kScaleformVisibilityDiagnosticLogLimit,
        text ? 1u : 0u,
        kScaleformTextDiagnosticLogLimit);
}

bool install_mod_settings_tab_hooks(
    const GameAddresses& addresses) noexcept {
    if (!mod_settings_page_registered()) return true;

    // Native category 4 reaches the specialized type-6 Adjust Brightness
    // constructor. This candidate redirects only that scoped category route
    // to the type-2 shell and shares the existing six-argument Game Options
    // hook, avoiding the r11 duplicate-hook collision.
    if (!g_experimental_mod_settings_route_enabled.load(
            std::memory_order_acquire)) {
        erui::detail::logf(
            erui::LogLevel::warning,
            "Dedicated Mod Settings tab disabled safely: native category 4 routes to Adjust Brightness; no category-4 rows or tab will be installed");
        return true;
    }

    const auto cleanup = []() noexcept {
        g_mod_settings_tab_hooks_ready.store(
            false, std::memory_order_release);
        g_configuration_category_append_hook.reset();
        g_configuration_category_append_hook_installed.store(
            false, std::memory_order_release);
        g_original_configuration_category_append.store(
            nullptr, std::memory_order_release);
        g_mod_settings_panel_route_hook.reset();
        g_mod_settings_panel_route_return_hook.reset();
        t_mod_settings_route_depth = 0;
    };

    if (!erui::detail::runtime_state().options.enable_custom_text ||
        !addresses.mod_settings_category_builder ||
        !addresses.mod_settings_category_destructor ||
        !addresses.configuration_category_append ||
        !addresses.mod_settings_panel_route_site ||
        !addresses.mod_settings_panel_route_return_site) {
        erui::detail::logf(
            erui::LogLevel::warning,
            "Dedicated Mod Settings tab disabled safely: custom text or one of the exact-build category/Game Options route interfaces is unavailable");
        return false;
    }

    auto route_result = SafetyHookMid::create(
        addresses.mod_settings_panel_route_site,
        &scope_mod_settings_panel_route);
    if (!route_result) {
        erui::detail::logf(
            erui::LogLevel::warning,
            "Dedicated Mod Settings tab disabled safely: native panel route hook could not be created (SafetyHook error type %u)",
            static_cast<unsigned>(route_result.error().type));
        cleanup();
        return false;
    }
    g_mod_settings_panel_route_hook = std::move(*route_result);

    auto route_return_result = SafetyHookMid::create(
        addresses.mod_settings_panel_route_return_site,
        &finish_mod_settings_panel_route);
    if (!route_return_result) {
        erui::detail::logf(
            erui::LogLevel::warning,
            "Dedicated Mod Settings tab disabled safely: native route-return hook could not be created (SafetyHook error type %u)",
            static_cast<unsigned>(route_return_result.error().type));
        cleanup();
        return false;
    }
    g_mod_settings_panel_route_return_hook =
        std::move(*route_return_result);

    auto append_result = SafetyHookInline::create(
        addresses.configuration_category_append,
        reinterpret_cast<void*>(&configuration_category_append_detour),
        SafetyHookInline::StartDisabled);
    if (!append_result) {
        erui::detail::logf(
            erui::LogLevel::warning,
            "Dedicated Mod Settings tab disabled safely: native category append hook could not be prepared (SafetyHook error type %u)",
            static_cast<unsigned>(append_result.error().type));
        cleanup();
        return false;
    }
    g_configuration_category_append_hook = std::move(*append_result);
    g_original_configuration_category_append.store(
        g_configuration_category_append_hook.original<
            ConfigurationCategoryAppendFn>(),
        std::memory_order_release);
    if (auto enabled = g_configuration_category_append_hook.enable();
        !enabled) {
        erui::detail::logf(
            erui::LogLevel::warning,
            "Dedicated Mod Settings tab disabled safely: native category append hook could not be enabled (SafetyHook error type %u)",
            static_cast<unsigned>(enabled.error().type));
        cleanup();
        return false;
    }
    g_configuration_category_append_hook_installed.store(
        true, std::memory_order_release);

    g_mod_settings_tab_hooks_ready.store(true, std::memory_order_release);
    erui::detail::logf(
        erui::LogLevel::info,
            "Dedicated Mod Settings tab ready: category 4 is appended after Graphics; matching brightness callback and type are redirected to the single Game Options hook");
    return true;
}

bool install_sub_hook(
    const GameAddresses& addresses,
    const ModuleView* game,
    const PreservedSharedMenuAddresses* preserved) noexcept {
    if (erui::detail::diagnostics_enabled()) {
        const auto* code = reinterpret_cast<const unsigned char*>(addresses.sub_handler);
        erui::detail::logf(
            erui::LogLevel::trace,
            "diagnostic: subpage entry before hook=%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
            code[0], code[1], code[2], code[3], code[4], code[5], code[6], code[7],
            code[8], code[9], code[10], code[11], code[12], code[13], code[14], code[15]);
    }
    if (!revalidate_shared_menu_entry_before_hook(
            game,
            preserved,
            PreservedSharedMenuEntry::sub_handler,
            "Game Options subpage")) {
        return false;
    }
    auto sub_result = SafetyHookInline::create(
        reinterpret_cast<void*>(addresses.sub_handler),
        reinterpret_cast<void*>(&sub_handler_detour),
        SafetyHookInline::StartDisabled);
    if (!sub_result) {
        erui::detail::logf(
            erui::LogLevel::error,
            "SafetyHook failed to prepare Game Options subpage handler hook (error type %u)",
            static_cast<unsigned>(sub_result.error().type));
        return false;
    }

    g_sub_hook = std::move(*sub_result);
    g_original_sub.store(g_sub_hook.original<SubHandlerFn>(), std::memory_order_release);
    if (auto enable_result = g_sub_hook.enable(); !enable_result) {
        erui::detail::logf(
            erui::LogLevel::error,
            "SafetyHook failed to enable Game Options subpage handler hook (error type %u)",
            static_cast<unsigned>(enable_result.error().type));
        return false;
    }

    erui::detail::logf(
        erui::LogLevel::info,
        "Hook installed: Game Options subpage handler (four forwarded ABI slots)");
    if (erui::detail::diagnostics_enabled()) {
        erui::detail::logf(
            erui::LogLevel::trace,
            "diagnostic: subpage hook target=%p detour=%p trampoline=%p openSubPage=%p",
            reinterpret_cast<void*>(addresses.sub_handler),
            reinterpret_cast<void*>(&sub_handler_detour),
            reinterpret_cast<void*>(g_original_sub.load(std::memory_order_acquire)),
            reinterpret_cast<void*>(addresses.open_sub_page));
    }
    return true;
}

bool invoke_open_sub_page(
    OpenSubPageFn function,
    void** parent_page_slot,
    unsigned long& seh_exception_code) noexcept {
    seh_exception_code = 0;
    if (!function || !parent_page_slot) {
        return false;
    }
#if defined(_MSC_VER)
    __try {
        function(parent_page_slot);
        return true;
    } __except (seh_exception_code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    function(parent_page_slot);
    return true;
#endif
}

bool invoke_native_back(
    NativeBackFn function,
    void* page,
    unsigned long& seh_exception_code) noexcept {
    seh_exception_code = 0;
    if (!function || !page) {
        return false;
    }
#if defined(_MSC_VER)
    __try {
        // The concrete page's real Back path constructs the packed action
        // {kind=3, flags=0}. Use that exact payload for synthetic Previous.
        function(page, kNativeBackAction);
        return true;
    } __except (seh_exception_code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    function(page, kNativeBackAction);
    return true;
#endif
}

enum class TextHookInstallStatus : std::uint8_t {
    success,
    hook_failed,
};

TextHookInstallStatus install_text_hook(
    const GameAddresses& addresses,
    const ModuleView* game,
    const PreservedSharedMenuAddresses* preserved) noexcept {
    const MH_STATUS initialize_status = MH_Initialize();
    if (initialize_status != MH_OK && initialize_status != MH_ERROR_ALREADY_INITIALIZED) {
        erui::detail::logf(
            erui::LogLevel::error,
            "MinHook initialization failed: %s (%d)",
            minhook_status_name(initialize_status),
            static_cast<int>(initialize_status));
        return TextHookInstallStatus::hook_failed;
    }

    if (!revalidate_shared_menu_entry_before_hook(
            game,
            preserved,
            PreservedSharedMenuEntry::text_resolver,
            "native text resolver")) {
        return TextHookInstallStatus::hook_failed;
    }
    g_text_hook_target = reinterpret_cast<void*>(addresses.text_resolver);
    void* trampoline = nullptr;
    const MH_STATUS create_status = MH_CreateHook(
        g_text_hook_target,
        reinterpret_cast<void*>(&text_resolver_detour),
        &trampoline);
    if (create_status != MH_OK) {
        erui::detail::logf(
            erui::LogLevel::error,
            "MinHook failed to create native text-resolver hook: %s (%d)",
            minhook_status_name(create_status),
            static_cast<int>(create_status));
        g_text_hook_target = nullptr;
        return TextHookInstallStatus::hook_failed;
    }

    g_text_hook_created = true;
    g_original_text_resolver.store(
        reinterpret_cast<TextResolverFn>(trampoline),
        std::memory_order_release);

    const MH_STATUS queue_status = MH_QueueEnableHook(g_text_hook_target);
    if (queue_status != MH_OK) {
        erui::detail::logf(
            erui::LogLevel::error,
            "MinHook failed to queue native text-resolver hook: %s (%d)",
            minhook_status_name(queue_status),
            static_cast<int>(queue_status));
        return TextHookInstallStatus::hook_failed;
    }

    const MH_STATUS apply_status = MH_ApplyQueued();
    if (apply_status != MH_OK) {
        erui::detail::logf(
            erui::LogLevel::error,
            "MinHook failed to enable native text-resolver hook: %s (%d)",
            minhook_status_name(apply_status),
            static_cast<int>(apply_status));
        return TextHookInstallStatus::hook_failed;
    }

    g_text_hook_enabled = true;
    erui::detail::logf(
        erui::LogLevel::info,
        "Hook installed: native UI text resolver via MinHook (custom IDs enabled, five-argument result-object ABI)");
    if (erui::detail::diagnostics_enabled()) {
        erui::detail::logf(
            erui::LogLevel::trace,
            "diagnostic: text hook target=%p detour=%p trampoline=%p",
            g_text_hook_target,
            reinterpret_cast<void*>(&text_resolver_detour),
            trampoline);
    }
    return TextHookInstallStatus::success;
}

void reset_hooks() noexcept {
#if defined(ER_NATIVE_UI_CUSTOM_TABS_BACKEND)
    if (g_tab_content_hooks_exposed.load(std::memory_order_acquire)) {
        // Game callbacks do not participate in a host-side quiescence
        // protocol. Resetting any part of this interdependent hook graph after
        // carrier exposure could invalidate an in-flight trampoline or state
        // it can reach, so the complete runtime remains pinned.
        erui::detail::logf(
            erui::LogLevel::warning,
            "Native UI hook reset refused after Graphics-carrier exposure; the complete runtime remains process-lifetime");
        return;
    }
#endif
    remove_native_slider_presentation();
    g_mod_settings_tab_hooks_ready.store(false, std::memory_order_release);
    g_configuration_category_append_hook.reset();
    g_configuration_category_append_hook_installed.store(
        false, std::memory_order_release);
    g_original_configuration_category_append.store(
        nullptr, std::memory_order_release);
    g_option_setting_tab_frame_hook.reset();
    g_tab_transition_count.store(0, std::memory_order_release);
    g_last_tab_control.store(nullptr, std::memory_order_release);
    g_last_tab_selector.store(nullptr, std::memory_order_release);
    g_last_tab_manager.store(nullptr, std::memory_order_release);
    g_last_tab_active_panel.store(nullptr, std::memory_order_release);
    g_last_tab_selected_index.store(
        std::numeric_limits<std::uint32_t>::max(),
        std::memory_order_release);
    g_last_tab_selected_category.store(
        std::numeric_limits<std::uint32_t>::max(),
        std::memory_order_release);
    g_last_tab_context.store(0, std::memory_order_release);
    g_last_tab_selection_tick.store(0, std::memory_order_release);
    reset_diagnostic_scaleform_paths();
    g_mod_settings_panel_route_hook.reset();
    g_mod_settings_panel_route_return_hook.reset();
    g_mod_settings_category_insertions.store(0, std::memory_order_release);
    g_configuration_category_append_calls.store(0, std::memory_order_release);
    g_mod_settings_route_scopes.store(0, std::memory_order_release);
    g_mod_settings_route_calls.store(0, std::memory_order_release);
    g_mod_settings_route_returns.store(0, std::memory_order_release);
    t_mod_settings_route_depth = 0;
    for (auto& call : g_native_panel_materialization_calls) {
        call.store(0, std::memory_order_release);
    }
    for (auto& call : g_native_row_builder_diagnostic_calls) {
        call.store(0, std::memory_order_release);
    }
    for (auto& call : g_scaleform_visibility_diagnostic_calls) {
        call.store(0, std::memory_order_release);
    }
    for (auto& call : g_scaleform_text_diagnostic_calls) {
        call.store(0, std::memory_order_release);
    }
    t_diagnostic_native_category =
        std::numeric_limits<std::uint32_t>::max();
    g_builtin_panel_observer_only_mask.store(0, std::memory_order_release);
    set_color_picker_widget_path_bridge_available(false);
    {
        std::scoped_lock lock{g_scaleform_path_bridge_mutex};
        // Invalidate the ownership proof before restoring executable bytes so
        // a concurrent validator can never mistake a tearing-down hook for a
        // live host-owned bridge.
        g_scaleform_path_bridge_proven = false;
        g_scaleform_path_bridge_target = nullptr;
        g_scaleform_path_bridge_patch = {};
        g_scaleform_path_resolver_hook.reset();
    }
#if defined(ER_NATIVE_UI_CUSTOM_TABS_BACKEND)
    g_tab_content_category8_pre_hook.reset();
    g_tab_content_composite_teardown_hook.reset();
    g_original_tab_content_composite_teardown.store(
        nullptr, std::memory_order_release);
    g_tab_carrier = {};
    g_reusable_route = erui::detail::invalid_compiled_index;
    g_reusable_composite = nullptr;
    g_reusable_activation_frame = 0;
    g_reusable_failed.store(false, std::memory_order_release);
    g_tab_carrier_epoch = 0;
    g_tab_content_route_log_count.store(0, std::memory_order_release);
    g_tab_content_route_fault_log_count.store(0, std::memory_order_release);
    g_tab_content_callbacks_armed.store(false, std::memory_order_release);
    g_tab_carrier_build_profile = nullptr;
#endif
    g_scaleform_text_diagnostic_hook.reset();
    reset_wide_line_help();
    reset_native_value_refresh();
    g_scaleform_visibility_diagnostic_hook.reset();
    g_add_unclassified_row_diagnostic_hook.reset();
    g_add_button_diagnostic_hook.reset();
    g_add_choice_diagnostic_hook.reset();
    g_add_slider_diagnostic_hook.reset();
    g_add_toggle_diagnostic_hook.reset();
    reset_dialog_input_gates(false);
    remove_native_popup_choice_bridge();
    g_sub_hook.reset();
    g_graphics_panel_materializer_hook.reset();
    g_original_graphics_panel_materializer.store(
        nullptr,
        std::memory_order_release);
    g_keyboard_mouse_panel_materializer_hook.reset();
    g_original_keyboard_mouse_panel_materializer.store(
        nullptr,
        std::memory_order_release);
    g_network_panel_materializer_hook.reset();
    g_original_network_panel_materializer.store(
        nullptr,
        std::memory_order_release);
    g_sound_panel_materializer_hook.reset();
    g_original_sound_panel_materializer.store(
        nullptr,
        std::memory_order_release);
    g_display_panel_materializer_hook.reset();
    g_original_display_panel_materializer.store(
        nullptr,
        std::memory_order_release);
    g_camera_panel_materializer_hook.reset();
    g_original_camera_panel_materializer.store(
        nullptr,
        std::memory_order_release);
    g_builtin_panel_warning_mask.store(0, std::memory_order_release);
    g_game_options_hook.reset();

    if (g_text_hook_target && g_text_hook_enabled) {
        const MH_STATUS disable_status = MH_DisableHook(g_text_hook_target);
        if (disable_status != MH_OK && disable_status != MH_ERROR_DISABLED) {
            erui::detail::logf(
                erui::LogLevel::warning,
                "MinHook could not disable native UI text resolver during cleanup: %s (%d)",
                minhook_status_name(disable_status),
                static_cast<int>(disable_status));
        }
    }
    if (g_text_hook_target && g_text_hook_created) {
        const MH_STATUS remove_status = MH_RemoveHook(g_text_hook_target);
        if (remove_status != MH_OK && remove_status != MH_ERROR_NOT_CREATED) {
            erui::detail::logf(
                erui::LogLevel::warning,
                "MinHook could not remove native UI text resolver during cleanup: %s (%d)",
                minhook_status_name(remove_status),
                static_cast<int>(remove_status));
        }
    }

    g_text_hook_target = nullptr;
    g_text_hook_created = false;
    g_text_hook_enabled = false;
    g_original_game_options.store(nullptr, std::memory_order_release);
    g_original_sub.store(nullptr, std::memory_order_release);
    g_original_text_resolver.store(nullptr, std::memory_order_release);
    g_title_capture.reset();
    g_submenu_navigation.reset();
}

} // namespace

std::uint32_t native_tab_graphics_carrier_version() noexcept {
    return graphics_carrier_version_v2_impl();
}

bool prepare_native_tab_graphics_carrier(
    void* composite,
    std::size_t route,
    std::uint64_t completed_frame,
    std::int32_t* category,
    NativeTabParkingSwitchFn park_native) noexcept {
    return prepare_graphics_carrier_v2_impl(
        composite, route, completed_frame, category, park_native);
}

bool verify_native_tab_graphics_carrier(
    void* composite,
    std::size_t route,
    std::int32_t category) noexcept {
    return verify_graphics_carrier_v2_impl(composite, route, category);
}

#if defined(ER_NATIVE_UI_TAB_CONTENT_RESEARCH_PROBE)
// The standalone viewport research DLL predates the in-process production
// bridge and resolves these names dynamically. Keep that compatibility only
// in an explicitly opted-in research host; release hosts do not export them.
extern "C" __declspec(dllexport) std::uint32_t __cdecl
ERUI_ResearchGraphicsCarrierVersionV2() noexcept {
    return native_tab_graphics_carrier_version();
}

extern "C" __declspec(dllexport) bool __cdecl
ERUI_ResearchPrepareGraphicsCarrierV2(
    void* composite,
    std::size_t route,
    std::uint64_t completed_frame,
    std::int32_t* category,
    NativeTabParkingSwitchFn park_native) noexcept {
    return prepare_native_tab_graphics_carrier(
        composite, route, completed_frame, category, park_native);
}

extern "C" __declspec(dllexport) bool __cdecl
ERUI_ResearchVerifyGraphicsCarrierV2(
    void* composite,
    std::size_t route,
    std::int32_t category) noexcept {
    return verify_native_tab_graphics_carrier(composite, route, category);
}
#endif

bool preflight_native_tab_carrier(const GameAddresses& addresses) noexcept {
#if defined(ER_NATIVE_UI_CUSTOM_TABS_BACKEND)
    const TabCarrierBuildProfile* matched_profile{};
    return tab_carrier_signatures_match(addresses, &matched_profile) &&
        matched_profile != nullptr;
#else
    (void)addresses;
    return false;
#endif
}

bool native_tab_graphics_carrier_installed() noexcept {
#if defined(ER_NATIVE_UI_CUSTOM_TABS_BACKEND)
    return g_tab_content_callbacks_armed.load(std::memory_order_acquire) &&
        !g_reusable_failed.load(std::memory_order_acquire) &&
        g_tab_content_category8_pre_hook.enabled() &&
        g_tab_content_composite_teardown_hook.enabled() &&
        g_original_tab_content_composite_teardown.load(
            std::memory_order_acquire) != nullptr;
#else
    return false;
#endif
}

bool native_scaleform_path_bridge_installed_at(
    const void* target) noexcept {
    std::scoped_lock lock{g_scaleform_path_bridge_mutex};
    if (!target || !g_scaleform_path_bridge_proven ||
        g_scaleform_path_bridge_target != target ||
        !g_scaleform_path_resolver_hook.enabled() ||
        g_scaleform_path_resolver_hook.target() != target ||
        g_scaleform_path_resolver_hook.destination() !=
            &observe_scaleform_path) {
        return false;
    }

    const auto& original =
        g_scaleform_path_resolver_hook.original_bytes();
    if (original.empty() ||
        original.size() > kScaleformPathResolverEntry.size() ||
        !std::equal(
            original.begin(), original.end(),
            kScaleformPathResolverEntry.begin())) {
        return false;
    }

    std::array<std::uint8_t, 16> live_patch{};
    return read_scaleform_path_entry(target, live_patch) &&
        live_patch == g_scaleform_path_bridge_patch;
}

bool native_tab_graphics_carrier_exposed() noexcept {
#if defined(ER_NATIVE_UI_CUSTOM_TABS_BACKEND)
    return g_tab_content_hooks_exposed.load(std::memory_order_acquire);
#else
    return false;
#endif
}

std::uint64_t native_tab_graphics_carrier_teardown_generation() noexcept {
#if defined(ER_NATIVE_UI_CUSTOM_TABS_BACKEND)
    return g_tab_content_teardown_generation.load(
        std::memory_order_acquire);
#else
    return 0;
#endif
}

void request_navigate_page(
    void** parent_page_slot,
    erui::detail::PageRoute target,
    NavigationRequestKind kind) noexcept {
    // Retained button thunks already reject input while a native alert owns
    // the modal slot. Keep the navigation boundary defensive as well so a new
    // internal caller cannot bypass modal ownership accidentally.
    if (native_dialog_owns_menu_input()) return;

    erui::detail::RuntimeState& runtime = erui::detail::runtime_state();
    void* parent_page = parent_page_slot ? *parent_page_slot : nullptr;

    if (kind == NavigationRequestKind::pagination_next) {
        runtime.pagination_next_hits.fetch_add(1, std::memory_order_relaxed);
    } else if (kind == NavigationRequestKind::pagination_previous) {
        runtime.pagination_previous_hits.fetch_add(1, std::memory_order_relaxed);
    }
    g_title_capture.reset();

    if (kind == NavigationRequestKind::pagination_previous) {
        const erui::detail::PageRoute current =
            g_submenu_navigation.route_for_native_page(parent_page);
        if (!parent_page || !g_addresses.native_back ||
            !erui::detail::is_previous_route_target(current, target)) {
            erui::detail::logf(
                erui::LogLevel::error,
                "paginationPrevious rejected page=%p currentKind=%s currentLogical=%zu currentSlice=%zu targetKind=%s targetLogical=%zu targetSlice=%zu nativeBack=%p",
                parent_page,
                route_kind_name(current.kind),
                current.logical_page_index,
                current.slice_index + 1,
                route_kind_name(target.kind),
                target.logical_page_index,
                target.slice_index + 1,
                reinterpret_cast<void*>(g_addresses.native_back));
            return;
        }
        // Native Back restores an already-constructed parent and therefore
        // does not re-enter the subpage handler that normally publishes the
        // page protected by the dialog input gates. Resolve that parent while
        // its binding is still live, then publish it only after Back succeeds.
        void* restored_page{};
        if (target.kind == erui::detail::PageRouteKind::root_main) {
            restored_page = g_last_page.load(std::memory_order_acquire);
        } else if (
            target.kind == erui::detail::PageRouteKind::builtin_main ||
            target.kind == erui::detail::PageRouteKind::tab_main) {
            restored_page = g_submenu_navigation.parent_page(parent_page);
        } else {
            restored_page = g_submenu_navigation.native_page(target);
        }
        unsigned long seh_exception_code = 0;
        const bool invoked = invoke_native_back(
            g_addresses.native_back, parent_page, seh_exception_code);
        if (invoked) {
            g_submenu_navigation.unbind_native_page(parent_page);
            if (restored_page) {
                g_dialog_input_page.store(
                    restored_page, std::memory_order_release);
            }
        }
        erui::detail::logf(
            invoked ? erui::LogLevel::trace : erui::LogLevel::error,
            "paginationPrevious page=%p currentKind=%s currentLogical=%zu currentSlice=%zu targetKind=%s targetLogical=%zu targetSlice=%zu nativeBack=%p invoked=%d seh=0x%08lX",
            parent_page,
            route_kind_name(current.kind),
            current.logical_page_index,
            current.slice_index + 1,
            route_kind_name(target.kind),
            target.logical_page_index,
            target.slice_index + 1,
            reinterpret_cast<void*>(g_addresses.native_back),
            invoked ? 1 : 0,
            seh_exception_code);
        return;
    }

    erui::detail::PageSlice target_slice{};
    const bool target_resolved = runtime.menu &&
        resolve_page_route_slice(*runtime.menu, target, target_slice);
    if (!parent_page_slot || !parent_page || !target_resolved) {
        erui::detail::logf(
            erui::LogLevel::error,
            "navigation request rejected kind=%s parentSlot=%p parentPage=%p targetKind=%s logicalPage=%zu slice=%zu capacity=%u",
            request_kind_name(kind),
            reinterpret_cast<void*>(parent_page_slot),
            parent_page,
            route_kind_name(target.kind),
            target.logical_page_index,
            target.slice_index,
            static_cast<unsigned>(target.root_capacity));
        return;
    }

    const std::uint64_t request =
        runtime.submenu_open_requests.fetch_add(1, std::memory_order_relaxed) + 1;
    if (!g_addresses.open_sub_page) {
        erui::detail::logf(
            erui::LogLevel::error,
            "submenu open request[%llu] has no openSubPage function",
            static_cast<unsigned long long>(request));
        return;
    }

    const std::uint64_t now = GetTickCount64();
    if (!g_submenu_navigation.begin_request(target, now, parent_page)) {
        erui::detail::logf(
            erui::LogLevel::error,
            "submenu open request[%llu] could not arm route kind=%s logicalPage=%zu slice=%zu",
            static_cast<unsigned long long>(request),
            route_kind_name(target.kind),
            target.logical_page_index,
            target.slice_index);
        return;
    }

    erui::detail::logf(
        erui::LogLevel::trace,
        "submenuOpen[%llu] kind=%s parent=%p targetKind=%s logicalPage=%zu slice=%zu/%zu",
        static_cast<unsigned long long>(request),
        request_kind_name(kind),
        parent_page,
        route_kind_name(target.kind),
        target.logical_page_index,
        target.slice_index + 1,
        target_slice.slice_count);

    if (runtime.options.enable_diagnostics) {
        const std::uint32_t diagnostic_index =
            runtime.diagnostic_submenu_logs.fetch_add(1, std::memory_order_relaxed);
        if (diagnostic_index < kSubmenuDiagnosticLogLimit) {
            erui::detail::logf(
                erui::LogLevel::trace,
                "diagnostic: submenu open request[%llu] parentSlot=%p parentPage=%p targetKind=%s logicalPage=%zu slice=%zu tick=%llu openSubPage=%p",
                static_cast<unsigned long long>(request),
                reinterpret_cast<void*>(parent_page_slot),
                parent_page,
                route_kind_name(target.kind),
                target.logical_page_index,
                target.slice_index,
                static_cast<unsigned long long>(now),
                reinterpret_cast<void*>(g_addresses.open_sub_page));
        }
    }

    unsigned long seh_exception_code = 0;
    const bool opened = invoke_open_sub_page(
            g_addresses.open_sub_page,
            parent_page_slot,
            seh_exception_code);
    if (!opened) {
        g_submenu_navigation.cancel_request();
        g_title_capture.reset();
        const std::uint32_t fault =
            runtime.submenu_open_faults.fetch_add(1, std::memory_order_relaxed) + 1;
        if (fault <= kSubmenuFaultLogLimit) {
            erui::detail::logf(
                erui::LogLevel::error,
                "submenu open request[%llu] failed with SEH exception 0x%08lX (fault=%u parentPage=%p targetKind=%s logicalPage=%zu slice=%zu)",
                static_cast<unsigned long long>(request),
                seh_exception_code,
                static_cast<unsigned>(fault),
                parent_page,
                route_kind_name(target.kind),
                target.logical_page_index,
                target.slice_index);
        }
        return;
    }

    if (runtime.options.enable_diagnostics && request <= kSubmenuDiagnosticLogLimit) {
        erui::detail::logf(
            erui::LogLevel::trace,
            "diagnostic: submenu open request[%llu] returned pendingKind=%s pendingLogical=%zu pendingSlice=%zu boundNativePage=%p",
            static_cast<unsigned long long>(request),
            route_kind_name(g_submenu_navigation.pending_route().kind),
            g_submenu_navigation.pending_route().logical_page_index,
            g_submenu_navigation.pending_route().slice_index,
            g_submenu_navigation.native_page(target));
    }
}

HookInstallStatus install_native_menu_hooks(
    const GameAddresses& addresses,
    const PreservedSharedMenuAddresses* preserved_shared) noexcept {
    g_addresses = addresses;
    g_mod_settings_tab_hooks_ready.store(false, std::memory_order_release);
    g_mod_settings_category_insertions.store(0, std::memory_order_release);
    g_configuration_category_append_calls.store(0, std::memory_order_release);
    g_mod_settings_route_scopes.store(0, std::memory_order_release);
    g_mod_settings_route_calls.store(0, std::memory_order_release);
    g_mod_settings_route_returns.store(0, std::memory_order_release);
    t_mod_settings_route_depth = 0;
    for (auto& call : g_native_panel_materialization_calls) {
        call.store(0, std::memory_order_release);
    }
    for (auto& call : g_native_row_builder_diagnostic_calls) {
        call.store(0, std::memory_order_release);
    }
    for (auto& call : g_scaleform_visibility_diagnostic_calls) {
        call.store(0, std::memory_order_release);
    }
    t_diagnostic_native_category =
        std::numeric_limits<std::uint32_t>::max();
    g_last_tab_selection_tick.store(0, std::memory_order_release);
    g_last_page.store(nullptr, std::memory_order_release);
    g_dialog_input_page.store(nullptr, std::memory_order_release);
    g_last_injection_tick.store(0, std::memory_order_release);
    g_game_options_entry_count.store(0, std::memory_order_release);
    g_game_options_injection_count.store(0, std::memory_order_release);
    g_builtin_panel_warning_mask.store(0, std::memory_order_release);
    g_text_entry_count.store(0, std::memory_order_release);
    reset_diagnostic_text_ids();
    g_text_fault_count.store(0, std::memory_order_release);
    g_sub_entry_count.store(0, std::memory_order_release);
    g_sub_injection_count.store(0, std::memory_order_release);
    g_title_fault_count.store(0, std::memory_order_release);
    g_title_capture.reset();

    ModuleView game{};
    if (preserved_shared &&
        !game.initialize(GetModuleHandleW(nullptr))) {
        erui::detail::logf(
            erui::LogLevel::error,
            "Could not initialize the game image for per-entry shared-menu revalidation");
        return HookInstallStatus::hook_failed;
    }

    erui::detail::RuntimeState& runtime = erui::detail::runtime_state();
    const bool enable_rows = runtime.options.enable_row_injection;
    const bool enable_text = runtime.options.enable_custom_text;
    const bool root_overflow = runtime.menu &&
        runtime.menu->root_plan(erui::detail::game_options_min_plan_capacity)
            .pages.slices.size() > 1;
    const bool root_handler_required = enable_rows && runtime.menu &&
        !runtime.menu->root_page().rows.empty();
    const bool root_diagnostic_observer = enable_rows &&
        runtime.options.enable_diagnostics;
    const bool mod_settings_handler_required = enable_rows && enable_text &&
        g_experimental_mod_settings_route_enabled.load(
            std::memory_order_acquire) &&
        mod_settings_page_registered();
    const bool builtin_rows_may_require_runtime_pagination = runtime.menu &&
        runtime.menu->builtin_rows_may_require_runtime_pagination();
    bool has_tab_overflow = false;
#if defined(ER_NATIVE_UI_CUSTOM_TABS_BACKEND)
    if (runtime.menu) {
        for (const std::size_t page_index :
             runtime.menu->tab_page_indices) {
            if (page_index < runtime.menu->pages.size() &&
                runtime.menu->pages[page_index].rows.size() >
                    kTabCarrierMinimumPlanCapacity) {
                has_tab_overflow = true;
                break;
            }
        }
    }
#endif
    const bool enable_submenus = enable_rows && runtime.menu &&
        (runtime.menu->modeled_submenu_count != 0 || root_overflow ||
            builtin_rows_may_require_runtime_pagination || has_tab_overflow);
    const bool enable_popup_choices = enable_rows && runtime.menu &&
        runtime.menu->modeled_popup_choice_count != 0;
    const bool enable_text_inputs = enable_rows && runtime.menu &&
        runtime.menu->modeled_text_input_count != 0;

    if (enable_submenus) {
        std::size_t binding_capacity = 0;
        for (std::size_t page_index = 0;
             page_index < runtime.menu->page_plans.size(); ++page_index) {
            if (runtime.menu->page_reachable(page_index)) {
                binding_capacity += runtime.menu->page_plans[page_index].slices.size();
            }
        }
        std::size_t maximum_root_slices = 0;
        for (const erui::detail::RootPagePlan& plan : runtime.menu->root_plans) {
            maximum_root_slices =
                std::max(maximum_root_slices, plan.pages.slices.size());
        }
        binding_capacity += maximum_root_slices;
        // Built-in first-page capacity is discovered only after native rows
        // exist. Reserve against the worst useful case (one free slot, which
        // becomes a Next-only main page on overflow) so the UI callback never
        // allocates or exhausts bindings for continuation pages.
        for (std::uint8_t native_category = 1;
             native_category < runtime.menu->builtin_page_indices.size();
             ++native_category) {
            const erui::detail::CompiledPage* const builtin =
                runtime.menu->builtin_page(native_category);
            if (!builtin || builtin->rows.empty()) continue;
            erui::detail::PageSlice first{};
            if (erui::detail::resolve_paginated_slice(
                    runtime.menu->builtin_page_index(native_category),
                    builtin->rows.size(),
                    1,
                    erui::detail::native_subpage_capacity,
                    0,
                    first) && first.slice_count > 1) {
                binding_capacity += first.slice_count - 1;
            }
        }
#if defined(ER_NATIVE_UI_CUSTOM_TABS_BACKEND)
        // Custom-tab main panels already exist in the Configuration cache;
        // reserve bindings only for their native continuation subpages.
        for (const std::size_t page_index :
             runtime.menu->tab_page_indices) {
            if (page_index >= runtime.menu->pages.size()) continue;
            erui::detail::PageSlice first{};
            if (erui::detail::resolve_paginated_slice(
                    page_index,
                    runtime.menu->pages[page_index].rows.size(),
                    kTabCarrierMinimumPlanCapacity,
                    erui::detail::native_subpage_capacity,
                    0,
                    first) && first.slice_count > 1) {
                binding_capacity += first.slice_count - 1;
            }
        }
#endif
        binding_capacity = std::max<std::size_t>(binding_capacity + 8, 16);
        if (!g_submenu_navigation.initialize(
                runtime.menu->pages.size(),
                binding_capacity)) {
            erui::detail::logf(
                erui::LogLevel::error,
                "Failed to allocate native submenu page bindings for %zu logical pages (%zu physical bindings)",
                runtime.menu->pages.size(),
                binding_capacity);
            return HookInstallStatus::hook_failed;
        }
    } else {
        g_submenu_navigation.reset();
    }

    if (!enable_rows) {
        erui::detail::logf(erui::LogLevel::error, "row injection is disabled");
        return HookInstallStatus::hook_failed;
    }

    if (enable_popup_choices &&
        !install_native_popup_choice_bridge(addresses)) {
        erui::detail::logf(
            erui::LogLevel::error,
            "Popup-choice rows require the native popup-choice bridge");
        reset_hooks();
        return HookInstallStatus::hook_failed;
    }

    if (enable_rows) {
        if (root_handler_required || root_diagnostic_observer ||
            mod_settings_handler_required) {
            if (!addresses.game_options_handler ||
                !install_game_options_hook(
                    addresses, &game, preserved_shared)) {
                reset_hooks();
                return HookInstallStatus::hook_failed;
            }
        } else {
            erui::detail::logf(
                erui::LogLevel::info,
                "Game Options root hook skipped: no ERNativeUI rows are registered on the vanilla root page");
        }
        if (runtime.options.enable_diagnostics) {
            erui::detail::logf(
                erui::LogLevel::info,
                "Native menu comparison diagnostics r30 enabled: scoped page attribution, one extra unclassified row builder, unique Scaleform UTF-8 writes and visibility changes, indexed row-builder arguments, controller property rows, and Scaleform path callsites; per-category limits rows=%u visibilityUnique=%u scaleformTextUnique=%u paths=%zu textIds=%zu",
                kNativeRowBuilderDiagnosticLogLimit,
                kScaleformVisibilityDiagnosticLogLimit,
                kScaleformTextDiagnosticLogLimit,
                kScaleformPathDiagnosticSlotsPerContext,
                kDiagnosticTextIdsPerCategory);
        }
        // Install the ordinary Graphics materializer first. The reusable
        // carrier deliberately invokes the exact native Graphics entry, so a
        // registered provider's Graphics rows must already be in that chain.
        install_builtin_panel_materializers(addresses);
        install_wide_line_help(addresses);
        install_native_value_refresh();
        install_native_render_diagnostic_observers(addresses);
        if (enable_submenus) {
            if (!addresses.sub_handler || !addresses.open_sub_page ||
                !install_sub_hook(addresses, &game, preserved_shared)) {
                erui::detail::logf(
                    erui::LogLevel::error,
                    "pagination/submenu routes require resolved subpage interfaces");
                reset_hooks();
                return HookInstallStatus::hook_failed;
            }
        }
    }


    if (enable_text) {
        if (!addresses.text_resolver ||
            install_text_hook(addresses, &game, preserved_shared) !=
                TextHookInstallStatus::success) {
            reset_hooks();
            return HookInstallStatus::hook_failed;
        }
        // The category title is remapped by the custom text resolver, so the
        // dedicated native tab must be armed only after that resolver is live.
        // Failure is isolated to the optional tab; the rest of the native UI
        // remains usable and the warning identifies the unsupported route.
        (void)install_mod_settings_tab_hooks(addresses);
    } else {
        erui::detail::logf(
            erui::LogLevel::warning,
            "Custom text hook disabled; rows use a vanilla fallback message ID");
    }

    if (!install_native_slider_presentation(
            addresses, &game, preserved_shared)) {
        erui::detail::logf(
            erui::LogLevel::error,
            "Formatted-slider presentation could not be installed");
        reset_hooks();
        return HookInstallStatus::hook_failed;
    }
    install_configuration_category_append_observer(addresses);
    install_option_setting_tab_observer(addresses);

    // TextInput state may be changed from a client callback or the host
    // worker. Its native MenuString shadow is therefore synchronized at the
    // game's page-frame boundary. This frame hook is required independently
    // of the optional native-alert transport, which merely shares it as an
    // input gate when available.
    if (enable_text_inputs && !install_page_frame_gate(addresses)) {
        erui::detail::logf(
            erui::LogLevel::error,
            "TextInput rows require the page-frame UI pump");
        reset_hooks();
        return HookInstallStatus::hook_failed;
    }

    const bool dialog_input_ready = enable_text &&
        install_dialog_input_gates(addresses, enable_text_inputs);
    if (!enable_text) {
        erui::detail::logf(
            erui::LogLevel::warning,
            "Native alert transport unavailable: custom text support is disabled");
    } else if (!dialog_input_ready) {
        erui::detail::logf(
            erui::LogLevel::warning,
            "Native alert transport unavailable: modal input gates were not installed");
    } else if (!game.base() &&
        !game.initialize(GetModuleHandleW(nullptr))) {
        erui::detail::logf(
            erui::LogLevel::warning,
            "Native alert transport unavailable: main executable could not be parsed");
        reset_dialog_input_gates(enable_text_inputs);
    } else if (!install_native_dialog_transport(game)) {
        // Alerts are an optional native route. A game update may invalidate
        // their semantic signatures without disabling registered menu rows.
        erui::detail::logf(
            erui::LogLevel::warning,
            "Native alert transport was not installed; menu rows remain active");
        reset_dialog_input_gates(enable_text_inputs);
    }

    bool color_picker_installed = false;
    const bool color_picker_requested = runtime.menu &&
        runtime.menu->modeled_color_picker_count != 0;
    // Resolve and validate ColorPicker before either our title bridge
    // patches Scaleform_ResolvePath or the optional observation trace detours
    // ColorPalette/ColorControl entries.
    if (!color_picker_requested) {
        erui::detail::logf(
            erui::LogLevel::trace,
            "Color Picker backend not requested by the compiled menu");
    } else if (!native_dialog_available()) {
        erui::detail::logf(
            erui::LogLevel::error,
            "Color Picker unavailable: native dialog transport was not installed");
    } else if (!install_color_picker(game)) {
        erui::detail::logf(
            erui::LogLevel::error,
            "Color Picker was requested but its native backend could not be installed");
    } else {
        color_picker_installed = true;
    }
    if (color_picker_requested && !color_picker_installed) {
        reset_hooks();
        remove_native_dialog_transport();
        return HookInstallStatus::hook_failed;
    }

    const bool title_path_bridge_requested =
        enable_submenus && addresses.title_bridge_complete();
    const bool color_widget_path_bridge_requested = color_picker_installed;
    const bool diagnostic_path_observer_requested =
        runtime.options.enable_diagnostics &&
        addresses.scaleform_path_resolver;
    const bool hub_panel_path_bridge_requested =
        g_mod_settings_tab_hooks_ready.load(std::memory_order_acquire) &&
        addresses.scaleform_path_resolver;
    bool exact_path_ownership_required{};
#if defined(ER_NATIVE_UI_CUSTOM_TABS_BACKEND)
    exact_path_ownership_required = runtime.menu &&
        !runtime.menu->tab_page_indices.empty();
#endif
    if (title_path_bridge_requested || color_widget_path_bridge_requested ||
        diagnostic_path_observer_requested ||
        hub_panel_path_bridge_requested) {
        void* path_hook_target = reinterpret_cast<void*>(
            addresses.scaleform_path_resolver);
        if (!path_hook_target && color_picker_installed) {
            path_hook_target = color_picker_widget_path_hook_target();
        }
        if (path_hook_target) {
            std::array<std::uint8_t, 16> entry_before_hook{};
            const bool pristine_entry =
                read_scaleform_path_entry(
                    path_hook_target, entry_before_hook) &&
                entry_before_hook == kScaleformPathResolverEntry;
            if (exact_path_ownership_required && !pristine_entry) {
                erui::detail::logf(
                    erui::LogLevel::error,
                    "Shared Scaleform path target changed after native-tab preflight; refusing to chain or expose process-lifetime custom-tab hooks");
                reset_hooks();
                remove_native_dialog_transport();
                return HookInstallStatus::hook_failed;
            }
            auto path_result = SafetyHookMid::create(
                path_hook_target,
                &observe_scaleform_path);
            if (path_result) {
                bool exact_ownership{};
                {
                    std::scoped_lock lock{
                        g_scaleform_path_bridge_mutex};
                    g_scaleform_path_bridge_proven = false;
                    g_scaleform_path_bridge_target = nullptr;
                    g_scaleform_path_bridge_patch = {};
                    g_scaleform_path_resolver_hook =
                        std::move(*path_result);

                    const auto& original =
                        g_scaleform_path_resolver_hook.original_bytes();
                    std::array<std::uint8_t, 16> installed_patch{};
                    const bool original_matches = !original.empty() &&
                        original.size() <= entry_before_hook.size() &&
                        std::equal(
                            original.begin(), original.end(),
                            entry_before_hook.begin());
                    const bool patch_captured =
                        read_scaleform_path_entry(
                            path_hook_target, installed_patch) &&
                        installed_patch != entry_before_hook;
                    exact_ownership = pristine_entry &&
                        g_scaleform_path_resolver_hook.enabled() &&
                        g_scaleform_path_resolver_hook.target() ==
                            static_cast<std::uint8_t*>(path_hook_target) &&
                        g_scaleform_path_resolver_hook.destination() ==
                            &observe_scaleform_path &&
                        original_matches && patch_captured;
                    if (exact_ownership) {
                        g_scaleform_path_bridge_target = path_hook_target;
                        g_scaleform_path_bridge_patch = installed_patch;
                        g_scaleform_path_bridge_proven = true;
                    } else if (exact_path_ownership_required) {
                        // We are still before process-lifetime carrier
                        // exposure. Restore the pristine entry immediately;
                        // never pin an unproven shared hook.
                        g_scaleform_path_resolver_hook.reset();
                    }
                }
                if (exact_path_ownership_required && !exact_ownership) {
                    set_color_picker_widget_path_bridge_available(false);
                    erui::detail::logf(
                        erui::LogLevel::error,
                        "Shared Scaleform path bridge could not prove exact target ownership; custom-tab hooks were not exposed");
                    reset_hooks();
                    remove_native_dialog_transport();
                    return HookInstallStatus::hook_failed;
                }
                set_color_picker_widget_path_bridge_available(
                    color_picker_installed);
                erui::detail::logf(
                    erui::LogLevel::info,
                    "Shared Scaleform path bridge installed (titles=%d colorWidget=%d diagnostics=%d hubPanel=%d exactOwnership=%d)",
                    title_path_bridge_requested ? 1 : 0,
                    color_widget_path_bridge_requested ? 1 : 0,
                    diagnostic_path_observer_requested ? 1 : 0,
                    hub_panel_path_bridge_requested ? 1 : 0,
                    exact_ownership ? 1 : 0);
            } else {
                set_color_picker_widget_path_bridge_available(false);
                erui::detail::logf(
                    erui::LogLevel::warning,
                    "Shared Scaleform path bridge could not be installed (SafetyHook error type %u); dependent presentation features are disabled",
                    static_cast<unsigned>(path_result.error().type));
            }
        } else {
            erui::detail::logf(
                erui::LogLevel::warning,
                "Shared Scaleform path target is unavailable; dependent presentation features and path diagnostics are disabled");
        }
    } else {
        if (runtime.options.enable_diagnostics) {
            erui::detail::logf(
                erui::LogLevel::warning,
                "Scaleform path diagnostics unavailable: exact-build path resolver was not resolved");
        }
        if (enable_submenus) {
        erui::detail::logf(
            erui::LogLevel::warning,
            "Scaleform title interfaces are incomplete; custom page titles are disabled while menu rows remain active");
        }
    }

#if defined(ER_NATIVE_UI_CUSTOM_TABS_BACKEND)
    // Expose the process-lifetime Graphics carrier only after every other
    // fallible menu-hook stage has completed. If either carrier enable is
    // ambiguous, its unarmed pass-through hooks and the complete runtime are
    // retained rather than rolling back executable trampolines in flight.
    if (runtime.menu && !runtime.menu->tab_page_indices.empty()) {
        g_reusable_route = native_graphics_route();
        if (!install_tab_content_carrier_hooks(addresses)) {
            erui::detail::logf(
                erui::LogLevel::error,
                "Custom tabs require the validated category-8 selector-11 Graphics carrier");
            reset_hooks();
            return HookInstallStatus::hook_failed;
        }
    }
#endif

    erui::detail::logf(
        erui::LogLevel::info,
        "Native UI runtime active (rows=%d submenus=%d pagination=%d customText=%d diagnostics=%d cooldown=%u ms)",
        enable_rows ? 1 : 0,
        enable_submenus ? 1 : 0,
        runtime.menu && runtime.menu->pagination_required_for_capacity(
            erui::detail::game_options_min_plan_capacity) ? 1 : 0,
        enable_text ? 1 : 0,
        runtime.options.enable_diagnostics ? 1 : 0,
        static_cast<unsigned>(runtime.options.injection_cooldown_ms));
    return HookInstallStatus::success;
}

void set_pending_menu_warning(std::wstring message) noexcept {
    if (message.empty()) return;
    AcquireSRWLockExclusive(&g_menu_warning_lock);
    g_menu_warning.swap(message);
    ReleaseSRWLockExclusive(&g_menu_warning_lock);
    g_menu_warning_pending.store(true, std::memory_order_release);
}

void set_hook_perf_enabled(bool enabled) noexcept {
    g_hook_perf.store(enabled, std::memory_order_relaxed);
}

void log_hook_perf(std::uint64_t interval_ms) noexcept {
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    const double us_per_tick = frequency.QuadPart > 0
        ? 1000000.0 / static_cast<double>(frequency.QuadPart) : 0.0;
    char line[768]{};
    int used = std::snprintf(line, sizeof(line),
        "Perf %llu s, hooks (calls / own us / game us):",
        static_cast<unsigned long long>(interval_ms / 1000));
    for (HookPerfCounter* counter :
            {&g_perf_text_setter, &g_perf_scaleform_path, &g_perf_text_resolver}) {
        const std::uint64_t calls = counter->calls.exchange(0, std::memory_order_relaxed);
        const std::uint64_t total = counter->total_ticks.exchange(0, std::memory_order_relaxed);
        const std::uint64_t game = counter->game_ticks.exchange(0, std::memory_order_relaxed);
        const std::uint64_t own = total > game ? total - game : 0;
        if (used <= 0 || static_cast<std::size_t>(used) >= sizeof(line)) break;
        const int added = std::snprintf(line + used, sizeof(line) - used,
            " %s %llu / %.0f / %.0f;", counter->name,
            static_cast<unsigned long long>(calls),
            static_cast<double>(own) * us_per_tick,
            static_cast<double>(game) * us_per_tick);
        if (added < 0) break;
        used += added;
    }
    erui::detail::logf(erui::LogLevel::info, "%s", line);
}

void remove_native_menu_hooks() noexcept {
    remove_color_picker();
    remove_native_dialog_transport();
    reset_hooks();
}

} // namespace erui::native
