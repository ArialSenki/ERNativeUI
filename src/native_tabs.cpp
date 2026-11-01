// Shared native-tab state (definitions of every global), build profile, accessors, logging and GFX value helpers.
#include "native_tabs_internal.hpp"

namespace native_tabs_detail {

const NativeTabsBuildProfile* g_native_tabs_build_profile{};

[[nodiscard]] const NativeTabsBuildProfile* find_native_tabs_build_profile(
    std::uint32_t timestamp,
    std::size_t image_size) noexcept {
    for (const NativeTabsBuildProfile& profile : kNativeTabsBuildProfiles) {
        if (profile.timestamp == timestamp &&
            profile.image_size == image_size) {
            return &profile;
        }
    }
    // Fork: an adopted layout-equivalent image uses its build's profile.
    if (const erui::native::EldenRingBuildIdentity* const equivalent =
            erui::native::adopted_layout_equivalent_build(
                timestamp, image_size)) {
        for (const NativeTabsBuildProfile& profile :
             kNativeTabsBuildProfiles) {
            if (std::string_view{profile.version} ==
                equivalent->product_version) {
                return &profile;
            }
        }
    }
    return nullptr;
}

[[nodiscard]] const NativeTabsBuildProfile& tabs_build() noexcept {
    return *g_native_tabs_build_profile;
}
constexpr NativeTabsRva kSwitchPanelVisibilityDecisionRva{
    &NativeTabsBuildProfile::switch_panel_visibility_decision};
constexpr NativeTabsRva kAs3ValueSetMember271Rva{
    &NativeTabsBuildProfile::as3_value_set_member};
constexpr NativeTabsRva kStaticShapeVtable271Rva{
    &NativeTabsBuildProfile::static_shape_vtable};
constexpr NativeTabsRva kAlternateAuthoredIconVtable271Rva{
    &NativeTabsBuildProfile::alternate_authored_icon_vtable};
[[nodiscard]] std::size_t active_custom_tab_count() noexcept {
#if defined(ER_NATIVE_UI_PRODUCTION_TABS)
    return g_native_tab_runtime ? g_native_tab_runtime->size() : 0U;
#else
    return kCustomTabCount;
#endif
}
[[nodiscard]] std::size_t active_logical_tab_count() noexcept {
    return kStockLogicalTabCount + active_custom_tab_count();
}
[[nodiscard]] std::size_t active_tab_host_count() noexcept {
    return active_physical_tab_count() * 2U;
}
[[nodiscard]] TabHostMask active_tab_host_mask() noexcept {
    const std::size_t count = active_tab_host_count();
    return count == 0U
        ? TabHostMask{0}
        : (TabHostMask{1} << count) - TabHostMask{1};
}
// r145: the cap was process-wide, so a failure repeated on every frame used
// it up and hid every later, different failure. Each call site (format
// string) now has its own budget; kMaximumFailureLogs bounds the total.
constexpr std::uint32_t kMaximumFailureLogs = 256;
constexpr std::uint32_t kMaximumFailureLogsPerSite = 4;
constexpr std::uint32_t kMaximumNativeImageResourceLogs = 8;
constexpr std::uint32_t kMaximumCarrierNormalizationLogs = 32;

// The live probe checks every AS3 pointer against the active exact-build
// profile before making its one harmless Invoke.
// Network and Keyboard/Mouse can retain this second exact authored-icon
// display-object type when a synthetic presentation is painted directly into
// their physical cells. Static comparison shows that it shares 33 of its
// first 40 virtual entries with the already-proven StaticShape target. The
// probe never calls either native vtable; it operates through the separately
// validated AS3 bridge and uses this RVA only as a narrow target allowlist.

constexpr std::int32_t kCrossIconId = 90001;

#if !defined(ER_NATIVE_UI_PRODUCTION_TABS)
static_assert(kRgba8IconIds.front() != kCrossIconId);

#endif
constexpr std::uint8_t kIconPropertyBankA = 0;
ResearchPrepareCarrierFn g_research_prepare_carrier{};
ResearchVerifyCarrierFn g_research_verify_carrier{};
SafetyHookInline g_research_switch_hook{};
std::atomic<SwitchPanelFn> g_original_research_switch{};
bool g_research_explicit_switch{};
bool g_research_switch_succeeded{};
bool g_research_failed{};
std::uint64_t g_research_carrier_teardown_generation{};
std::uint64_t g_research_completed_composite_frames{};

[[nodiscard]] Rgba8Pixel rgba8_probe_pixel(
    std::size_t icon,
    std::uint32_t x,
    std::uint32_t y) noexcept {
#if defined(ER_NATIVE_UI_PRODUCTION_TABS)
    if (!g_native_tab_catalogue || icon >= active_custom_tab_count() ||
        x >= ERUI_TAB_ICON_WIDTH || y >= ERUI_TAB_ICON_HEIGHT) {
        return {0xFFU, 0x00U, 0xFFU, 0xFFU};
    }
    const auto custom = g_native_tab_catalogue->custom_entries();
    if (icon >= custom.size() ||
        custom[icon].icon.source == erui::TabIconSource::fallback ||
        custom[icon].icon.rgba8_pixels.size() !=
            ERUI_TAB_ICON_RGBA8_BYTE_COUNT) {
        return {0xFFU, 0x00U, 0xFFU, 0xFFU};
    }
    const std::size_t offset =
        (static_cast<std::size_t>(y) * ERUI_TAB_ICON_WIDTH + x) * 4U;
    Rgba8Pixel pixel{
        custom[icon].icon.rgba8_pixels[offset + 0U],
        custom[icon].icon.rgba8_pixels[offset + 1U],
        custom[icon].icon.rgba8_pixels[offset + 2U],
        custom[icon].icon.rgba8_pixels[offset + 3U],
    };
    // Scaleform's RawImage path consumes premultiplied colour. Keep that
    // conversion private so clients always submit ordinary RGBA bytes.
    pixel.red = static_cast<std::uint8_t>(
        (static_cast<std::uint32_t>(pixel.red) * pixel.alpha + 127U) / 255U);
    pixel.green = static_cast<std::uint8_t>(
        (static_cast<std::uint32_t>(pixel.green) * pixel.alpha + 127U) / 255U);
    pixel.blue = static_cast<std::uint8_t>(
        (static_cast<std::uint32_t>(pixel.blue) * pixel.alpha + 127U) / 255U);
    return pixel;
#else
    // Human-readable 01..50 labels make a long stress run auditable by eye.
    // The background is also an explicit alpha-semantics diagnostic. Its top
    // half stores straight RGB while its bottom half stores the same colour
    // premultiplied by a left-to-right 0..255 alpha ramp. Whichever half
    // blends smoothly identifies the convention consumed by Elden Ring's
    // Scaleform upload path. Opaque white digits remain readable in either
    // case, and alpha-zero pixels retain nonzero RGB in the straight half so
    // an ignored-alpha failure becomes an unmistakable rectangle.
    constexpr std::array<std::uint8_t, 10> kSevenSegmentMasks{
        0x3FU, 0x06U, 0x5BU, 0x4FU, 0x66U,
        0x6DU, 0x7DU, 0x07U, 0x7FU, 0x6FU};
    if (icon >= kCustomTabCount) {
        return {0xFFU, 0x00U, 0xFFU, 0xFFU};
    }

    const std::uint32_t label = static_cast<std::uint32_t>(icon + 1U);
    const std::array<std::uint32_t, 2> digits{
        (label / 10U) % 10U,
        label % 10U};
    bool lit{};
    for (std::size_t digit_index = 0; digit_index < digits.size();
         ++digit_index) {
        const std::uint32_t left = digit_index == 0 ? 3U : 17U;
        const std::uint8_t segments =
            kSevenSegmentMasks[digits[digit_index]];
        const bool top = (segments & 0x01U) != 0 &&
            x >= left + 2U && x <= left + 9U && y >= 3U && y <= 5U;
        const bool upper_right = (segments & 0x02U) != 0 &&
            x >= left + 9U && x <= left + 11U && y >= 5U && y <= 14U;
        const bool lower_right = (segments & 0x04U) != 0 &&
            x >= left + 9U && x <= left + 11U && y >= 16U && y <= 25U;
        const bool bottom = (segments & 0x08U) != 0 &&
            x >= left + 2U && x <= left + 9U && y >= 25U && y <= 27U;
        const bool lower_left = (segments & 0x10U) != 0 &&
            x >= left && x <= left + 2U && y >= 16U && y <= 25U;
        const bool upper_left = (segments & 0x20U) != 0 &&
            x >= left && x <= left + 2U && y >= 5U && y <= 14U;
        const bool middle = (segments & 0x40U) != 0 &&
            x >= left + 2U && x <= left + 9U && y >= 14U && y <= 16U;
        lit = lit || top || upper_right || lower_right || bottom ||
            lower_left || upper_left || middle;
    }

    const std::uint8_t red = static_cast<std::uint8_t>(
        80U + (icon * 97U) % 176U);
    const std::uint8_t green = static_cast<std::uint8_t>(
        80U + (icon * 57U) % 176U);
    const std::uint8_t blue = static_cast<std::uint8_t>(
        80U + (icon * 137U) % 176U);
    if (lit) return {0xFFU, 0xFFU, 0xFFU, 0xFFU};

    const std::uint8_t alpha = static_cast<std::uint8_t>(
        (x * 255U) / 31U);
    if (y < 16U) return {red, green, blue, alpha};
    return {
        static_cast<std::uint8_t>((red * alpha + 127U) / 255U),
        static_cast<std::uint8_t>((green * alpha + 127U) / 255U),
        static_cast<std::uint8_t>((blue * alpha + 127U) / 255U),
        alpha};
#endif
}

[[nodiscard]] bool rgba8_probe_icon_has_alpha_profile(
    std::size_t icon) noexcept {
    const Rgba8Pixel straight_zero = rgba8_probe_pixel(icon, 0U, 0U);
    const Rgba8Pixel premultiplied_zero =
        rgba8_probe_pixel(icon, 0U, 31U);
    const Rgba8Pixel straight_opaque =
        rgba8_probe_pixel(icon, 31U, 0U);
    const Rgba8Pixel premultiplied_opaque =
        rgba8_probe_pixel(icon, 31U, 31U);
    const bool exact_discriminator = straight_zero.alpha == 0U &&
        (straight_zero.red != 0U || straight_zero.green != 0U ||
            straight_zero.blue != 0U) &&
        premultiplied_zero.red == 0U &&
        premultiplied_zero.green == 0U &&
        premultiplied_zero.blue == 0U &&
        premultiplied_zero.alpha == 0U &&
        straight_opaque.red == premultiplied_opaque.red &&
        straight_opaque.green == premultiplied_opaque.green &&
        straight_opaque.blue == premultiplied_opaque.blue &&
        straight_opaque.alpha == 0xFFU &&
        premultiplied_opaque.alpha == 0xFFU;
    if (!exact_discriminator) return false;

    bool transparent{};
    bool partial{};
    bool opaque{};
    for (std::uint32_t y = 0; y < 32U; ++y) {
        for (std::uint32_t x = 0; x < 32U; ++x) {
            const std::uint8_t alpha = rgba8_probe_pixel(icon, x, y).alpha;
            transparent = transparent || alpha == 0U;
            partial = partial || (alpha > 0U && alpha < 0xFFU);
            opaque = opaque || alpha == 0xFFU;
        }
    }
    return transparent && partial && opaque;
}

[[nodiscard]] std::uint64_t rgba8_probe_icon_checksum(
    std::size_t icon) noexcept {
    std::uint64_t checksum = 1469598103934665603ULL;
    for (std::uint32_t y = 0; y < 32U; ++y) {
        for (std::uint32_t x = 0; x < 32U; ++x) {
            const Rgba8Pixel pixel = rgba8_probe_pixel(icon, x, y);
            const std::array<std::uint8_t, 4> bytes{
                pixel.red, pixel.green, pixel.blue, pixel.alpha};
            for (const std::uint8_t value : bytes) {
                checksum ^= value;
                checksum *= 1099511628211ULL;
            }
        }
    }
    return checksum;
}

[[nodiscard]] bool catalogue_icon_requires_raster(
    std::size_t icon) noexcept {
#if defined(ER_NATIVE_UI_PRODUCTION_TABS)
    const erui::detail::NativeTabRuntimeEntry* const entry =
        g_native_tab_runtime ? g_native_tab_runtime->tab(icon) : nullptr;
    return entry && entry->icon().raster_requested;
#else
    return icon < kCustomTabCount;
#endif
}

template <std::size_t Count>
[[nodiscard]] std::array<std::uint64_t, Count>
make_rgba8_probe_icon_checksums() noexcept {
    std::array<std::uint64_t, Count> result{};
    for (std::size_t icon = 0; icon < Count; ++icon) {
        result[icon] = rgba8_probe_icon_checksum(icon);
    }
    return result;
}

[[nodiscard]] bool rgba8_probe_catalogue_is_unique_and_alpha_profiled(
    std::span<const std::uint64_t> checksums) noexcept {
    for (std::size_t icon = 0; icon < checksums.size(); ++icon) {
        if (!rgba8_probe_icon_has_alpha_profile(icon) ||
            checksums[icon] == 0) {
            return false;
        }
        for (std::size_t other = 0; other < icon; ++other) {
            if (checksums[icon] == checksums[other]) {
                return false;
            }
        }
    }
    return true;
}

const char* bitmapdata_property_verification_phase_name(
    BitmapDataPropertyVerificationPhase phase) noexcept {
    switch (phase) {
    case BitmapDataPropertyVerificationPhase::pre_mutation_identity:
        return "pre-mutation-identity";
    case BitmapDataPropertyVerificationPhase::post_mutation_identity:
        return "post-mutation-identity";
    default:
        return "none";
    }
}

HMODULE g_module{};
HANDLE g_log_file{INVALID_HANDLE_VALUE};
SRWLOCK g_log_lock = SRWLOCK_INIT;
GameImage g_game{};
RuntimeState g_runtime{};

#if defined(ER_NATIVE_UI_PRODUCTION_TABS)
const erui::detail::NativeTabCatalogue* g_native_tab_catalogue{};
std::unique_ptr<erui::detail::NativeTabRuntimeState> g_native_tab_runtime{};
std::unique_ptr<PinnedLogicalTabCatalogue> g_logical_tab_catalogue{};
std::size_t g_navigation_margin{kNavigationMargin};
bool g_mouse_dwell_enabled{true};
TabPointerDwellTiming g_pointer_dwell_timing{
    kPointerDwellInitialDelayMs,
    kPointerDwellRepeatDelayMs,
    kPointerDwellDecayMs,
    kPointerDwellMinimumDelayMs,
};
std::atomic_bool g_native_tabs_installed{};
std::size_t g_minimum_tab_slots{kStockLogicalTabCount};

#else
constexpr std::size_t g_custom_tab_count{kCustomTabCount};

#endif
std::array<LogicalTab, kStockLogicalTabCount> g_preserved_stock_catalogue{};
bool g_preserved_stock_catalogue_ready{};

SafetyHookInline g_selection_hook{};
SafetyHookInline g_composite_frame_hook{};
SafetyHookInline g_texture_lookup_hook{};
SafetyHookInline g_image_bind_hook{};
SafetyHookMid g_panel_state_copy_hook{};
SafetyHookMid g_panel_alias_visibility_hook{};
SafetyHookMid g_input_dispatch_frame_begin_hook{};
SafetyHookMid g_pointer_dispatch_source_hook{};
SafetyHookMid g_horizontal_navigation_source_hook{};
SafetyHookMid g_selection_dispatch_hook{};
SafetyHookMid g_pointer_cell_sample_hook{};

#if defined(ER_NATIVE_UI_PRODUCTION_TABS)
SafetyHookMid g_slot_growth_early_hook{};
SafetyHookMid g_slot_growth_late_hook{};
SafetyHookMid g_slot_growth_post_constructor_hook{};
std::atomic_bool g_slot_growth_callbacks_armed{};
CategoryAppendFn g_category_append{};
thread_local bool g_inside_slot_growth_append{};
thread_local SlotGrowthConstructionContext g_slot_growth_construction{};
thread_local std::array<
    SlotGrowthCellStorage,
    kSlotGrowthProbeCellCount> g_slot_growth_cell_storage{};

#endif
std::atomic_bool g_viewport_hooks_exposed{};
std::atomic_bool g_viewport_callbacks_armed{};
std::atomic<SelectionCallbackFn> g_original_selection{};
std::atomic<CompositeFrameFn> g_original_composite_frame{};
std::atomic<TextureLookupFn> g_original_texture_lookup{};
std::atomic<ImageBindFn> g_original_image_bind{};

SelectorSelectFn g_selector_select{};
GetPointerPositionFn g_get_pointer_position{};
GridHitTestFn g_grid_hit_test{};
NativePointerSelectFn g_native_pointer_select{};
SwitchPanelFn g_switch_panel{};
RefreshTitleFn g_refresh_title{};
ResolvePathFn g_resolve_path{};
ValueExistsFn g_value_exists{};
PresentTabFn g_present_tab{};
MakeIconDescriptorFn g_make_icon_descriptor{};
ScaleformConstructorFn g_construct_scaleform_value{};
ScaleformGetMemberFn g_get_scaleform_member{};
ScaleformSetPositionFn g_set_scaleform_position{};
ScaleformSetVisibleFn g_set_scaleform_visible{};
ScaleformDestructorFn g_destroy_scaleform_value{};

std::atomic_uint32_t g_transition_log_count{};
std::atomic_uint32_t g_serialized_selection_log_count{};
erui::detail::FailureLogBudget<kFailureLogSites> g_failure_log_budget{
    kMaximumFailureLogsPerSite, kMaximumFailureLogs};
std::atomic_uint32_t g_record_mismatch_log_count{};
std::atomic_uint32_t g_wrap_diagnostic_log_count{};
std::atomic_uint32_t g_alias_log_count{};
std::atomic_uint32_t g_image_bind_log_count{};
std::atomic_uint32_t g_native_image_resource_log_count{};
std::atomic_uint32_t g_shape_attach_attempt_count{};
std::atomic_uint32_t g_state_isolation_log_count{};
std::atomic_uint32_t g_alias_visibility_log_count{};
std::atomic_uint32_t g_carrier_normalization_log_count{};
std::atomic_uint32_t g_carrier_selection_log_count{};
std::atomic_uint32_t g_pointer_sample_log_count{};
std::atomic_uint32_t g_pointer_dwell_log_count{};
std::atomic_uint32_t g_rgba8_lookup_log_count{};
std::atomic_uint32_t g_rgba8_bind_log_count{};
std::atomic_uint32_t g_rgba8_lifecycle_log_count{};
std::atomic_bool g_value_invoke_probe_attempted{};
std::atomic_bool g_value_invoke_probe_validated{};
std::atomic_bool g_bitmapdata_constructor_probe_attempted{};
std::atomic_bool g_bitmapdata_pixel_mutation_probe_attempted{};
std::atomic_bool g_raw_image_lifecycle_probe_attempted{};
std::atomic_bool g_rgba8_publication_disabled{};
std::atomic_bool g_rgba8_catalogue_ready{};

#if !defined(ER_NATIVE_UI_PRODUCTION_TABS)
std::array<std::atomic<void*>, kCustomTabCount> g_retained_rgba8_images{};
std::array<std::atomic<void*>, kCustomTabCount> g_retained_rgba8_resources{};
std::array<std::atomic_uint64_t, kCustomTabCount>
    g_retained_rgba8_checksums{};
std::array<std::atomic_bool, kCustomTabCount> g_retained_rgba8_ready{};

#endif
std::atomic_uint64_t g_presentation_epoch_counter{};
std::atomic_uint64_t g_rgba8_successful_attach_count{};
std::atomic_uint64_t g_rgba8_successful_detach_count{};
AtomicTabHostMask g_injected_icon_hosts{};
AtomicTabHostMask g_rgba8_icon_hosts{};

#if !defined(ER_NATIVE_UI_PRODUCTION_TABS)
std::array<std::atomic_size_t, kTabHostCapacity>
    g_rgba8_host_assets{};

#endif
AtomicTabHostMask g_staged_injected_icon_hosts{};
AtomicTabHostMask g_staged_rgba8_icon_hosts{};

#if !defined(ER_NATIVE_UI_PRODUCTION_TABS)
std::array<std::atomic_size_t, kTabHostCapacity>
    g_staged_rgba8_host_assets{};

#endif
AtomicTabHostMask g_icon_bank_b_hosts{};

#if !defined(ER_NATIVE_UI_PRODUCTION_TABS)
std::array<std::atomic_uint32_t, kCustomTabCount>
    g_rgba8_retirement_debt{};

#endif
InjectedMovieFingerprint g_injected_movie_fingerprint{};
void* g_retained_bitmapdata_wrapper{};
void* g_retained_bitmapdata_same_frame_wrapper{};
void* g_retained_bitmapdata_next_frame_wrapper{};
thread_local bool g_mutating_viewport{};
thread_local TabSelectionSource g_tab_selection_source{
    TabSelectionSource::unknown};
thread_local PointerCellSampleTrace g_pointer_cell_sample_trace{};
thread_local PointerDwellAdapter g_pointer_dwell{};
thread_local bool g_blocking_composite_input{};
thread_local PanelStateIsolationExpectation g_state_isolation_expectation{};
thread_local bool g_resolving_icon_alias{};
thread_local RepaintContext g_repaint_context{};
thread_local bool g_initial_target_repaint{};
thread_local PendingAliasedIcon g_pending_aliased_icon{};
thread_local InPlaceRgba8RebindTransaction
    g_in_place_rgba8_rebind_transaction{};
thread_local DeferredBitmapDataProbe g_deferred_bitmapdata_probe{};
thread_local BitmapDataPropertyVerification
    g_bitmapdata_property_verification{};
thread_local std::uint64_t g_composite_frame_sequence{};

[[nodiscard]] std::size_t active_physical_tab_count() noexcept {
#if defined(ER_NATIVE_UI_PRODUCTION_TABS)
    if (g_native_tab_runtime) {
        const std::size_t count =
            g_native_tab_runtime->active_physical_slot_count();
        if (count != 0U) return count;
    }
#endif
    // Before a live movie epoch has been validated, only the eight stock
    // logical records are safe to address. Research builds retain their
    // original fixed-eight contract.
    return kStockLogicalTabCount;
}

[[nodiscard]] LogicalTab* runtime_logical_tab(std::size_t logical) noexcept {
#if defined(ER_NATIVE_UI_PRODUCTION_TABS)
    return g_logical_tab_catalogue &&
            logical < g_logical_tab_catalogue->tabs.size()
        ? &g_logical_tab_catalogue->tabs[logical]
        : nullptr;
#else
    return logical < g_runtime.catalogue.size()
        ? &g_runtime.catalogue[logical]
        : nullptr;
#endif
}

#if defined(ER_NATIVE_UI_PRODUCTION_TABS)
[[nodiscard]] erui::detail::NativeTabRuntimeEntry* runtime_custom_tab(
    std::size_t custom) noexcept {
    return g_native_tab_runtime ? g_native_tab_runtime->tab(custom) : nullptr;
}

#endif
[[nodiscard]] std::int32_t rgba8_icon_id(std::size_t custom) noexcept {
#if defined(ER_NATIVE_UI_PRODUCTION_TABS)
    const auto* const tab = runtime_custom_tab(custom);
    return tab ? tab->icon().resource_id : -1;
#else
    return custom < kRgba8IconIds.size() ? kRgba8IconIds[custom] : -1;
#endif
}

[[nodiscard]] const wchar_t* rgba8_icon_resource_name(
    std::size_t custom) noexcept {
#if defined(ER_NATIVE_UI_PRODUCTION_TABS)
    const auto* const tab = runtime_custom_tab(custom);
    return tab ? tab->icon().resource_name.data() : nullptr;
#else
    return custom < kRgba8IconResourceNames.size()
        ? kRgba8IconResourceNames[custom].data()
        : nullptr;
#endif
}

[[nodiscard]] const wchar_t* raw_image_resource_name(
    std::size_t custom) noexcept {
#if defined(ER_NATIVE_UI_PRODUCTION_TABS)
    const auto* const tab = runtime_custom_tab(custom);
    return tab ? tab->icon().raw_resource_name.data() : nullptr;
#else
    return custom < kRawImageResourceProbeNames.size()
        ? kRawImageResourceProbeNames[custom].data()
        : nullptr;
#endif
}

[[nodiscard]] void* retained_rgba8_image(std::size_t custom) noexcept {
#if defined(ER_NATIVE_UI_PRODUCTION_TABS)
    const auto* const tab = runtime_custom_tab(custom);
    return tab ? tab->icon().image.load(std::memory_order_acquire) : nullptr;
#else
    return custom < g_retained_rgba8_images.size()
        ? g_retained_rgba8_images[custom].load(std::memory_order_acquire)
        : nullptr;
#endif
}

[[nodiscard]] void* retained_rgba8_resource(std::size_t custom) noexcept {
#if defined(ER_NATIVE_UI_PRODUCTION_TABS)
    const auto* const tab = runtime_custom_tab(custom);
    return tab ? tab->icon().resource.load(std::memory_order_acquire) : nullptr;
#else
    return custom < g_retained_rgba8_resources.size()
        ? g_retained_rgba8_resources[custom].load(std::memory_order_acquire)
        : nullptr;
#endif
}

[[nodiscard]] std::uint64_t retained_rgba8_checksum(
    std::size_t custom) noexcept {
#if defined(ER_NATIVE_UI_PRODUCTION_TABS)
    const auto* const tab = runtime_custom_tab(custom);
    return tab ? tab->icon().checksum.load(std::memory_order_acquire) : 0U;
#else
    return custom < g_retained_rgba8_checksums.size()
        ? g_retained_rgba8_checksums[custom].load(std::memory_order_acquire)
        : 0U;
#endif
}

[[nodiscard]] bool retained_rgba8_ready(std::size_t custom) noexcept {
#if defined(ER_NATIVE_UI_PRODUCTION_TABS)
    const auto* const tab = runtime_custom_tab(custom);
    return tab && tab->icon().ready.load(std::memory_order_acquire);
#else
    return custom < g_retained_rgba8_ready.size() &&
        g_retained_rgba8_ready[custom].load(std::memory_order_acquire);
#endif
}

void publish_retained_rgba8(
    std::size_t custom,
    const RetainedRgba8Resource& retained) noexcept {
#if defined(ER_NATIVE_UI_PRODUCTION_TABS)
    auto* const tab = runtime_custom_tab(custom);
    if (!tab) return;
    tab->icon().image.store(retained.image, std::memory_order_relaxed);
    tab->icon().checksum.store(retained.checksum, std::memory_order_relaxed);
    tab->icon().resource.store(retained.resource, std::memory_order_relaxed);
    tab->icon().ready.store(retained.ready, std::memory_order_release);
#else
    if (custom >= kCustomTabCount) return;
    g_retained_rgba8_images[custom].store(
        retained.image, std::memory_order_relaxed);
    g_retained_rgba8_checksums[custom].store(
        retained.checksum, std::memory_order_relaxed);
    g_retained_rgba8_resources[custom].store(
        retained.resource, std::memory_order_relaxed);
    g_retained_rgba8_ready[custom].store(
        retained.ready, std::memory_order_release);
#endif
}

[[nodiscard]] std::uint32_t rgba8_retirement_debt(
    std::size_t custom) noexcept {
#if defined(ER_NATIVE_UI_PRODUCTION_TABS)
    const auto* const tab = runtime_custom_tab(custom);
    return tab ? tab->retirement_debt() : 0U;
#else
    return custom < g_rgba8_retirement_debt.size()
        ? g_rgba8_retirement_debt[custom].load(std::memory_order_acquire)
        : 0U;
#endif
}

[[nodiscard]] bool set_rgba8_retirement_debt(
    std::size_t custom,
    std::uint32_t debt) noexcept {
#if defined(ER_NATIVE_UI_PRODUCTION_TABS)
    return g_native_tab_runtime &&
        g_native_tab_runtime->set_retirement_debt(custom, debt);
#else
    if (custom >= g_rgba8_retirement_debt.size()) return false;
    g_rgba8_retirement_debt[custom].store(debt, std::memory_order_release);
    return true;
#endif
}

[[nodiscard]] std::size_t active_host_asset(std::size_t host) noexcept {
#if defined(ER_NATIVE_UI_PRODUCTION_TABS)
    const auto* const state = g_native_tab_runtime
        ? g_native_tab_runtime->host(host)
        : nullptr;
    return state
        ? state->active_tab_index.load(std::memory_order_acquire)
        : kNoRgba8Asset;
#else
    return host < g_rgba8_host_assets.size()
        ? g_rgba8_host_assets[host].load(std::memory_order_acquire)
        : kNoRgba8Asset;
#endif
}

[[nodiscard]] std::size_t staged_host_asset(std::size_t host) noexcept {
#if defined(ER_NATIVE_UI_PRODUCTION_TABS)
    const auto* const state = g_native_tab_runtime
        ? g_native_tab_runtime->host(host)
        : nullptr;
    return state
        ? state->staged_tab_index.load(std::memory_order_acquire)
        : kNoRgba8Asset;
#else
    return host < g_staged_rgba8_host_assets.size()
        ? g_staged_rgba8_host_assets[host].load(std::memory_order_acquire)
        : kNoRgba8Asset;
#endif
}

[[nodiscard]] bool set_active_host_asset(
    std::size_t host,
    std::size_t custom) noexcept {
#if defined(ER_NATIVE_UI_PRODUCTION_TABS)
    if (!g_native_tab_runtime) return false;
    return custom == kNoRgba8Asset
        ? g_native_tab_runtime->clear_active_host(host)
        : g_native_tab_runtime->set_active_host(host, custom);
#else
    if (host >= g_rgba8_host_assets.size() ||
        (custom != kNoRgba8Asset && custom >= active_custom_tab_count())) {
        return false;
    }
    g_rgba8_host_assets[host].store(custom, std::memory_order_release);
    return true;
#endif
}

[[nodiscard]] bool set_staged_host_asset(
    std::size_t host,
    std::size_t custom) noexcept {
#if defined(ER_NATIVE_UI_PRODUCTION_TABS)
    if (!g_native_tab_runtime) return false;
    return custom == kNoRgba8Asset
        ? g_native_tab_runtime->clear_staged_host(host)
        : g_native_tab_runtime->set_staged_host(host, custom);
#else
    if (host >= g_staged_rgba8_host_assets.size() ||
        (custom != kNoRgba8Asset && custom >= active_custom_tab_count())) {
        return false;
    }
    g_staged_rgba8_host_assets[host].store(
        custom, std::memory_order_release);
    return true;
#endif
}

#if defined(ER_NATIVE_UI_PRODUCTION_TABS)
RuntimeIconAtomicView<&erui::detail::NativeTabIconRuntimeState::image>
    g_retained_rgba8_images{};
RuntimeIconAtomicView<&erui::detail::NativeTabIconRuntimeState::resource>
    g_retained_rgba8_resources{};
RuntimeIconAtomicView<&erui::detail::NativeTabIconRuntimeState::checksum>
    g_retained_rgba8_checksums{};
RuntimeIconAtomicView<&erui::detail::NativeTabIconRuntimeState::ready>
    g_retained_rgba8_ready{};
RuntimeRetirementDebtView g_rgba8_retirement_debt{};
EncodedHostAssetView g_rgba8_host_assets{false};
EncodedHostAssetView g_staged_rgba8_host_assets{true};
RuntimeIconIdView kRgba8IconIds{};
RuntimeIconResourceNameView kRgba8IconResourceNames{};
RuntimeRawResourceNameView kRawImageResourceProbeNames{};

#endif
[[nodiscard]] bool all_rgba8_retirement_debts_zero() noexcept {
#if defined(ER_NATIVE_UI_PRODUCTION_TABS)
    return !g_native_tab_runtime ||
        g_native_tab_runtime->retiring_tab_indices().empty();
#else
    for (std::size_t custom = 0;
         custom < active_custom_tab_count();
         ++custom) {
        if (rgba8_retirement_debt(custom) != 0U) return false;
    }
    return true;
#endif
}

void clear_all_rgba8_retirement_debts() noexcept {
#if defined(ER_NATIVE_UI_PRODUCTION_TABS)
    while (g_native_tab_runtime &&
           !g_native_tab_runtime->retiring_tab_indices().empty()) {
        const std::size_t custom =
            g_native_tab_runtime->retiring_tab_indices().back();
        if (!set_rgba8_retirement_debt(custom, 0U)) break;
    }
#else
    for (std::size_t custom = 0;
         custom < active_custom_tab_count();
         ++custom) {
        (void)set_rgba8_retirement_debt(custom, 0U);
    }
#endif
}

// Requested raster ownership and effective raster publication are distinct.
// A provider may request RGBA8, yet the all-or-nothing native resource
// catalogue can fail safely and leave that tab on the vector fallback.  Only
// successfully published resources may enter a target host layout.  The
// requested predicate remains the authority for auditing resources and host
// ledgers that were already published before a later fail-closed condition.
[[nodiscard]] bool catalogue_icon_has_retained_raster(
    std::size_t icon) noexcept {
    if (!catalogue_icon_requires_raster(icon) ||
        !g_rgba8_catalogue_ready.load(std::memory_order_acquire)) {
        return false;
    }
    return g_retained_rgba8_ready[icon].load(
               std::memory_order_acquire) &&
        g_retained_rgba8_resources[icon].load(
               std::memory_order_acquire) != nullptr &&
        g_retained_rgba8_images[icon].load(
               std::memory_order_acquire) != nullptr;
}

[[nodiscard]] bool catalogue_icon_has_effective_raster(
    std::size_t icon) noexcept {
    return !g_rgba8_publication_disabled.load(
               std::memory_order_acquire) &&
        catalogue_icon_has_retained_raster(icon);
}

std::size_t repaint_host_bit() noexcept {
    if (!g_repaint_context.active ||
        g_repaint_context.layer == TabLayer::none ||
        g_repaint_context.physical >= active_physical_tab_count()) {
        return invalid_tab_index;
    }
    return g_repaint_context.physical +
        (g_repaint_context.layer == TabLayer::back
            ? active_physical_tab_count()
            : 0);
}

std::uint8_t active_icon_property_bank(std::size_t bit) noexcept {
    if (bit >= active_tab_host_count()) return kIconPropertyBankA;
    const TabHostMask mask = TabHostMask{1} << bit;
    return (g_icon_bank_b_hosts.load(std::memory_order_acquire) & mask) != 0
        ? kIconPropertyBankB
        : kIconPropertyBankA;
}

bool repaint_stages_fresh_host(std::size_t bit) noexcept {
    if (bit >= active_tab_host_count()) return false;
    const auto& transaction = g_in_place_rgba8_rebind_transaction;
    const TabHostMask mask = TabHostMask{1} << bit;
    return transaction.active && !transaction.failed &&
        transaction.stage == InPlaceRgba8RebindStage::repainting &&
        (transaction.expected_host_mask & mask) != 0;
}

std::uint8_t repaint_icon_property_bank() noexcept {
    const std::size_t bit = repaint_host_bit();
    const std::uint8_t active = active_icon_property_bank(bit);
    return repaint_stages_fresh_host(bit)
        ? static_cast<std::uint8_t>(active ^ 1U)
        : active;
}

std::uint64_t query_performance_ticks() noexcept {
    LARGE_INTEGER ticks{};
    if (!QueryPerformanceCounter(&ticks) || ticks.QuadPart <= 0) return 0;
    return static_cast<std::uint64_t>(ticks.QuadPart);
}

std::uint64_t query_performance_frequency() noexcept {
    static const std::uint64_t frequency = []() noexcept {
        LARGE_INTEGER value{};
        if (!QueryPerformanceFrequency(&value) || value.QuadPart <= 0) {
            return std::uint64_t{};
        }
        return static_cast<std::uint64_t>(value.QuadPart);
    }();
    return frequency;
}

std::uint64_t elapsed_qpc_microseconds(
    std::uint64_t begin,
    std::uint64_t end) noexcept {
    const std::uint64_t frequency = query_performance_frequency();
    if (begin == 0 || end < begin || frequency == 0) return 0;
    const std::uint64_t delta = end - begin;
    constexpr std::uint64_t microseconds_per_second = 1'000'000;
    return (delta / frequency) * microseconds_per_second +
        ((delta % frequency) * microseconds_per_second) / frequency;
}

GamePointerLocation locate_game_pointer(const void* pointer) noexcept {
    GamePointerLocation result{};
    if (!pointer || !g_game.base || g_game.size == 0) return result;

    const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(pointer);
    const std::uintptr_t base =
        reinterpret_cast<std::uintptr_t>(g_game.base);
    if (address < base) return result;
    const std::uintptr_t delta = address - base;
    if (delta >= g_game.size) return result;
    result.in_image = true;
    result.rva = delta;
    return result;
}

// Snapshot only scalar fields from the known-good resource that Elden Ring is
// about to bind. This deliberately performs no virtual call, AddRef, Release,
// mutation, or cross-frame retention. The original binder consumes the lookup
// reference immediately after this function returns.
void log_native_image_resource_snapshot(
    void* resource,
    void* image) noexcept {
    const std::uint32_t event =
        g_native_image_resource_log_count.fetch_add(
            1, std::memory_order_relaxed) + 1;
    if (event > kMaximumNativeImageResourceLogs || !resource || !image) {
        return;
    }

    const auto* const resource_bytes =
        static_cast<const std::byte*>(resource);
    void* resource_vtable{};
    std::uint32_t resource_refcount{};
    void* delegate_vtable{};
    std::uint32_t delegate_refcount{};
    void* delegate_underlying_image{};
    std::uint32_t resource_kind{};
    const bool resource_vtable_read = guarded_read(
        resource_bytes + 0x00, resource_vtable);
    const bool resource_refcount_read = guarded_read(
        resource_bytes + 0x08, resource_refcount);
    const bool delegate_vtable_read = guarded_read(
        resource_bytes + 0x20, delegate_vtable);
    const bool delegate_refcount_read = guarded_read(
        resource_bytes + 0x28, delegate_refcount);
    const bool delegate_underlying_read = guarded_read(
        resource_bytes + 0x48, delegate_underlying_image);
    const bool resource_kind_read = guarded_read(
        resource_bytes + 0x60, resource_kind);

    const auto* const image_bytes = static_cast<const std::byte*>(image);
    void* image_vtable{};
    std::uint32_t image_refcount{};
    const bool image_vtable_read = guarded_read(
        image_bytes + 0x00, image_vtable);
    const bool image_refcount_read = guarded_read(
        image_bytes + 0x08, image_refcount);

    void* image_type_slot{};
    void* image_format_slot{};
    void* image_size_slot{};
    bool image_slots_read{};
    const GamePointerLocation image_vtable_location =
        locate_game_pointer(image_vtable);
    if (image_vtable_read && image_vtable_location.in_image) {
        const auto* const vtable_bytes =
            static_cast<const std::byte*>(image_vtable);
        image_slots_read =
            guarded_read(vtable_bytes + 0x18, image_type_slot) &&
            guarded_read(vtable_bytes + 0x20, image_format_slot) &&
            guarded_read(vtable_bytes + 0x28, image_size_slot);
    }

    std::uint32_t field_28{};
    std::uint32_t field_2c{};
    std::uint32_t field_30{};
    std::uint32_t field_34{};
    std::uint8_t field_70{};
    std::uint32_t field_74{};
    std::uint32_t field_78{};
    std::uint32_t field_7c{};
    std::uint32_t field_80{};
    std::uint32_t field_84{};
    std::uint32_t field_88{};
    const bool image_fields_read =
        guarded_read(image_bytes + 0x28, field_28) &&
        guarded_read(image_bytes + 0x2C, field_2c) &&
        guarded_read(image_bytes + 0x30, field_30) &&
        guarded_read(image_bytes + 0x34, field_34) &&
        guarded_read(image_bytes + 0x70, field_70) &&
        guarded_read(image_bytes + 0x74, field_74) &&
        guarded_read(image_bytes + 0x78, field_78) &&
        guarded_read(image_bytes + 0x7C, field_7c) &&
        guarded_read(image_bytes + 0x80, field_80) &&
        guarded_read(image_bytes + 0x84, field_84) &&
        guarded_read(image_bytes + 0x88, field_88);

    const GamePointerLocation resource_vtable_location =
        locate_game_pointer(resource_vtable);
    const GamePointerLocation delegate_vtable_location =
        locate_game_pointer(delegate_vtable);
    const GamePointerLocation image_type_location =
        locate_game_pointer(image_type_slot);
    const GamePointerLocation image_format_location =
        locate_game_pointer(image_format_slot);
    const GamePointerLocation image_size_location =
        locate_game_pointer(image_size_slot);
    const void* const inline_delegate = resource_bytes + 0x20;

    log_line(
        "nativeImageResource[%u] passiveOnly=1 resource=%p reads={vtable:%u ref:%u delegateVtable:%u delegateRef:%u underlying:%u kind:%u} vtable=%p{game:%u rva:0x%llX} ref=%u exposedImage=%p inlineDelegate=%p exposedIsInline=%u delegate={vtable:%p game:%u rva:0x%llX ref:%u underlying:%p} kind=0x%08X",
        static_cast<unsigned>(event),
        resource,
        resource_vtable_read ? 1U : 0U,
        resource_refcount_read ? 1U : 0U,
        delegate_vtable_read ? 1U : 0U,
        delegate_refcount_read ? 1U : 0U,
        delegate_underlying_read ? 1U : 0U,
        resource_kind_read ? 1U : 0U,
        resource_vtable,
        resource_vtable_location.in_image ? 1U : 0U,
        static_cast<unsigned long long>(resource_vtable_location.rva),
        static_cast<unsigned>(resource_refcount),
        image,
        inline_delegate,
        image == inline_delegate ? 1U : 0U,
        delegate_vtable,
        delegate_vtable_location.in_image ? 1U : 0U,
        static_cast<unsigned long long>(delegate_vtable_location.rva),
        static_cast<unsigned>(delegate_refcount),
        delegate_underlying_image,
        static_cast<unsigned>(resource_kind));
    log_line(
        "nativeImageResource[%u] image=%p reads={vtable:%u ref:%u slots:%u fields:%u} vtable=%p{game:%u rva:0x%llX} ref=%u slots={type:%p/0x%llX format:%p/0x%llX size:%p/0x%llX} raw={28:0x%08X 2C:%u 30:%u 34:0x%08X 70:%u 74:0x%08X 78:0x%08X 7C:0x%08X 80:0x%08X 84:%u 88:%u}",
        static_cast<unsigned>(event),
        image,
        image_vtable_read ? 1U : 0U,
        image_refcount_read ? 1U : 0U,
        image_slots_read ? 1U : 0U,
        image_fields_read ? 1U : 0U,
        image_vtable,
        image_vtable_location.in_image ? 1U : 0U,
        static_cast<unsigned long long>(image_vtable_location.rva),
        static_cast<unsigned>(image_refcount),
        image_type_slot,
        static_cast<unsigned long long>(image_type_location.rva),
        image_format_slot,
        static_cast<unsigned long long>(image_format_location.rva),
        image_size_slot,
        static_cast<unsigned long long>(image_size_location.rva),
        static_cast<unsigned>(field_28),
        static_cast<unsigned>(field_2c),
        static_cast<unsigned>(field_30),
        static_cast<unsigned>(field_34),
        static_cast<unsigned>(field_70),
        static_cast<unsigned>(field_74),
        static_cast<unsigned>(field_78),
        static_cast<unsigned>(field_7c),
        static_cast<unsigned>(field_80),
        static_cast<unsigned>(field_84),
        static_cast<unsigned>(field_88));
    if (event == kMaximumNativeImageResourceLogs) {
        log_line(
            "Further passive native-image resource snapshots are suppressed");
    }
}

bool retain_stress_log_format(const char* format) noexcept {
#if defined(ER_NATIVE_UI_PRODUCTION_TABS)
    if (!format) return false;
    if (erui::detail::trace_logging()) return true;
    // Routine per-cell and per-frame proof records are available through the
    // existing trace-level diagnostic switch. Always retain safety failures
    // without putting a one-tab production install into the old 50-tab probe
    // logging mode.
    return std::strncmp(format, "ERROR:", 6) == 0 ||
        std::strncmp(format, "WARNING:", 8) == 0;
#else
    if (!kStressQuietSuccessLogging || !format) return format != nullptr;
    const auto starts_with = [format](const char* prefix) noexcept {
        return std::strncmp(format, prefix, std::strlen(prefix)) == 0;
    };
    return format[0] == '=' || starts_with("ERROR:") ||
        starts_with("WARNING:") || starts_with("STRESS ") ||
        starts_with("pointerDwell[") ||
        starts_with("pointerPoll[") ||
        starts_with("selection[") ||
        starts_with("Configuration-tab viewport mutation probe starting") ||
        starts_with("Game image base=") ||
        starts_with("Viewport probe hooks installed:") ||
        starts_with("Further viewport-probe warnings are suppressed");
#endif
}

void log_line(const char* format, ...) noexcept {
#if defined(ER_NATIVE_UI_PRODUCTION_TABS)
    if (!format || !retain_stress_log_format(format)) return;
    std::array<char, 4096> message{};
    va_list arguments;
    va_start(arguments, format);
    const int result = vsnprintf_s(
        message.data(), message.size(), _TRUNCATE, format, arguments);
    va_end(arguments);
    if (result < 0 && message[0] == '\0') return;
    erui::LogLevel level = erui::LogLevel::trace;
    if (std::strncmp(message.data(), "ERROR:", 6) == 0) {
        level = erui::LogLevel::error;
    } else if (std::strncmp(message.data(), "WARNING:", 8) == 0) {
        level = erui::LogLevel::warning;
    }
    erui::detail::logf(level, "NativeTabs: %s", message.data());
#else
    if (g_log_file == INVALID_HANDLE_VALUE || !format) return;
    if (!retain_stress_log_format(format)) return;

    std::array<char, 4096> message{};
    va_list arguments;
    va_start(arguments, format);
    const int result = vsnprintf_s(
        message.data(), message.size(), _TRUNCATE, format, arguments);
    va_end(arguments);
    if (result < 0 && message[0] == '\0') return;

    SYSTEMTIME time{};
    GetLocalTime(&time);
    std::array<char, 4608> line{};
    const int line_length = snprintf(
        line.data(),
        line.size(),
        "%04u-%02u-%02u %02u:%02u:%02u.%03u [T%lu] [TabViewportProbe] %s\r\n",
        static_cast<unsigned>(time.wYear),
        static_cast<unsigned>(time.wMonth),
        static_cast<unsigned>(time.wDay),
        static_cast<unsigned>(time.wHour),
        static_cast<unsigned>(time.wMinute),
        static_cast<unsigned>(time.wSecond),
        static_cast<unsigned>(time.wMilliseconds),
        static_cast<unsigned long>(GetCurrentThreadId()),
        message.data());
    if (line_length <= 0) return;

    const DWORD bytes_to_write = static_cast<DWORD>((std::min)(
        static_cast<std::size_t>(line_length), line.size() - 1));
    AcquireSRWLockExclusive(&g_log_lock);
    DWORD bytes_written{};
    (void)WriteFile(
        g_log_file, line.data(), bytes_to_write, &bytes_written, nullptr);
    ReleaseSRWLockExclusive(&g_log_lock);
#endif
}

void log_failure(const char* format, ...) noexcept {
    const auto budget = g_failure_log_budget.record(format);
    if (!budget.log) return;

    std::array<char, 2048> message{};
    va_list arguments;
    va_start(arguments, format);
    const int result = vsnprintf_s(
        message.data(), message.size(), _TRUNCATE, format, arguments);
    va_end(arguments);
    if (result >= 0 || message[0] != '\0') {
#if defined(ER_NATIVE_UI_PRODUCTION_TABS)
        log_line("ERROR: %s", message.data());
#else
        log_line("WARNING: %s", message.data());
#endif
    }
    if (budget.last_for_site) {
        log_line("Further repeats of this native-tab failure are suppressed");
    }
    if (budget.last_overall) {
#if defined(ER_NATIVE_UI_PRODUCTION_TABS)
        log_line("ERROR: Further native-tab failures are suppressed");
#else
        log_line("Further viewport-probe warnings are suppressed");
#endif
    }
}

bool exact_resource_name(
    const wchar_t* candidate,
    const wchar_t* expected) noexcept {
    if (!candidate || !expected) return false;
#if defined(_MSC_VER)
    __try {
#endif
        for (std::size_t index = 0;; ++index) {
            if (candidate[index] != expected[index]) return false;
            if (expected[index] == L'\0') return true;
        }
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#endif
}

bool parse_rgba8_icon_resource_name(
    const wchar_t* candidate,
    std::size_t& icon_index) noexcept {
    icon_index = invalid_tab_index;
    if (!candidate) return false;
    constexpr wchar_t kPrefix[] = L"MENU_ItemIcon_";
    constexpr std::size_t kPrefixLength = std::size(kPrefix) - 1U;
#if defined(_MSC_VER)
    __try {
#endif
        for (std::size_t index = 0; index < kPrefixLength; ++index) {
            if (candidate[index] != kPrefix[index]) return false;
        }
        std::uint32_t id{};
        for (std::size_t digit = 0; digit < 5U; ++digit) {
            const wchar_t value = candidate[kPrefixLength + digit];
            if (value < L'0' || value > L'9') return false;
            id = id * 10U + static_cast<std::uint32_t>(value - L'0');
        }
        if (candidate[kPrefixLength + 5U] != L'\0' ||
            id < static_cast<std::uint32_t>(kFirstRgba8IconId) ||
            id >= static_cast<std::uint32_t>(
                kFirstRgba8IconId + active_custom_tab_count())) {
            return false;
        }
        icon_index = static_cast<std::size_t>(
            id - static_cast<std::uint32_t>(kFirstRgba8IconId));
        return exact_resource_name(
            candidate, kRgba8IconResourceNames[icon_index].data());
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        icon_index = invalid_tab_index;
        return false;
    }
#endif
}

template <typename Value>
RawGfxValue make_primitive_value(
    std::uint32_t type,
    const Value& value) noexcept {
    static_assert(sizeof(Value) <= 8);
    RawGfxValue result{};
    result.type = type;
    std::memcpy(result.payload.data(), &value, sizeof(value));
    return result;
}

RawGfxValue make_bool_value(bool value) noexcept {
    const std::uint8_t encoded = value ? 1U : 0U;
    return make_primitive_value(2U, encoded);
}

RawGfxValue make_int_value(std::int32_t value) noexcept {
    return make_primitive_value(3U, value);
}

RawGfxValue make_uint_value(std::uint32_t value) noexcept {
    return make_primitive_value(4U, value);
}

RawGfxValue make_number_value(double value) noexcept {
    return make_primitive_value(5U, value);
}

bool inspect_object_value(
    const void* scaleform_wrapper,
    ObjectValueView& view) noexcept {
    view = {};
    if (!scaleform_wrapper ||
        !guarded_read(
            static_cast<const std::byte*>(scaleform_wrapper) + 0x18,
            view.object_interface) ||
        !view.object_interface ||
        !guarded_read(
            view.object_interface,
            view.object_interface_vtable) ||
        !view.object_interface_vtable ||
        !guarded_read(
            static_cast<const std::byte*>(view.object_interface_vtable) + 0x20,
            view.get_member_slot) ||
        !guarded_read(
            static_cast<const std::byte*>(view.object_interface_vtable) + 0x28,
            view.set_member_slot) ||
        !guarded_read(
            static_cast<const std::byte*>(view.object_interface_vtable) + 0x30,
            view.invoke_slot) ||
        !guarded_read(
            static_cast<const std::byte*>(scaleform_wrapper) + 0x20,
            view.flags) ||
        !guarded_read(
            static_cast<const std::byte*>(scaleform_wrapper) + 0x28,
            view.object_payload) ||
        !view.object_payload) {
        return false;
    }
    return (view.flags & 0x8FU) == 0x08U;
}

bool exact_as3_object_bridge(const ObjectValueView& view) noexcept {
    return view.object_interface_vtable ==
            g_game.base + kAs3ValueInterfaceVtable271Rva &&
        view.get_member_slot == g_game.base + kAs3ValueGetMember271Rva &&
        view.set_member_slot == g_game.base + kAs3ValueSetMember271Rva &&
        view.invoke_slot == g_game.base + kAs3ValueInvoke271Rva;
}

bool invoke_object_method(
    const ObjectValueView& object,
    const char* method,
    const RawGfxValue* arguments,
    std::size_t argument_count,
    unsigned long& seh_code) noexcept {
    if (!method || !exact_as3_object_bridge(object)) return false;

    bool invoked{};
#if defined(_MSC_VER)
    __try {
#endif
        invoked = reinterpret_cast<ValueInvokeFn>(object.invoke_slot)(
            object.object_interface,
            object.object_payload,
            nullptr,
            method,
            arguments,
            argument_count,
            false);
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (seh_code == 0) {
            seh_code = static_cast<unsigned long>(GetExceptionCode());
        }
    }
#endif
    return invoked;
}

bool draw_graphics_rectangle(
    const ObjectValueView& graphics,
    std::uint32_t color,
    double alpha,
    double x,
    double y,
    double width,
    double height,
    unsigned long& seh_code) noexcept {
    const std::array<RawGfxValue, 2> fill_arguments{
        make_uint_value(color),
        make_number_value(alpha),
    };
    const bool fill_begun = invoke_object_method(
        graphics,
        "beginFill",
        fill_arguments.data(),
        fill_arguments.size(),
        seh_code);
    if (!fill_begun) return false;

    const std::array<RawGfxValue, 4> rectangle_arguments{
        make_number_value(x),
        make_number_value(y),
        make_number_value(width),
        make_number_value(height),
    };
    const bool rectangle_drawn = invoke_object_method(
        graphics,
        "drawRect",
        rectangle_arguments.data(),
        rectangle_arguments.size(),
        seh_code);

    // Balance every successful beginFill even when drawRect returns false or
    // faults. This keeps the fresh Shape's Graphics state well formed.
    const bool fill_ended = invoke_object_method(
        graphics, "endFill", nullptr, 0, seh_code);
    return rectangle_drawn && fill_ended;
}

bool inspect_display_value(
    const void* scaleform_wrapper,
    DisplayValueView& view) noexcept {
    view = {};
    if (!scaleform_wrapper ||
        !guarded_read(
            static_cast<const std::byte*>(scaleform_wrapper) + 0x18,
            view.object_interface) ||
        !view.object_interface ||
        !guarded_read(
            view.object_interface,
            view.object_interface_vtable) ||
        !view.object_interface_vtable ||
        !guarded_read(
            static_cast<const std::byte*>(view.object_interface_vtable) + 0x20,
            view.get_member_slot) ||
        !guarded_read(
            static_cast<const std::byte*>(view.object_interface_vtable) + 0x28,
            view.set_member_slot) ||
        !guarded_read(
            static_cast<const std::byte*>(view.object_interface_vtable) + 0x30,
            view.invoke_slot) ||
        !guarded_read(
            static_cast<const std::byte*>(scaleform_wrapper) + 0x20,
            view.flags) ||
        !guarded_read(
            static_cast<const std::byte*>(scaleform_wrapper) + 0x28,
            view.object_payload) ||
        !view.object_payload ||
        !guarded_read(
            static_cast<const std::byte*>(view.object_payload) + 0x88,
            view.native_display_object) ||
        !view.native_display_object ||
        !guarded_read(
            view.native_display_object,
            view.native_display_vtable)) {
        return false;
    }
    return (view.flags & 0x8FU) == 0x0AU;
}

bool exact_as3_display_bridge(const DisplayValueView& view) noexcept {
    return view.object_interface_vtable ==
            g_game.base + kAs3ValueInterfaceVtable271Rva &&
        view.get_member_slot == g_game.base + kAs3ValueGetMember271Rva &&
        view.set_member_slot == g_game.base + kAs3ValueSetMember271Rva &&
        view.invoke_slot == g_game.base + kAs3ValueInvoke271Rva;
}

bool validated_authored_icon_target_vtable(void* vtable) noexcept {
    return g_native_tabs_build_profile && g_game.base &&
        (vtable == g_game.base + kStaticShapeVtable271Rva ||
            vtable ==
                g_game.base + kAlternateAuthoredIconVtable271Rva);
}

bool destroy_local_scaleform_value(
    std::array<std::byte, 0x38>& wrapper,
    bool& constructed,
    const char* label) noexcept {
    if (!constructed) return true;
    if (!g_destroy_scaleform_value) return false;
#if defined(_MSC_VER)
    __try {
#endif
        g_destroy_scaleform_value(wrapper.data());
        constructed = false;
        return true;
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        log_failure(
            "%s cleanup faulted (seh=0x%08lX)",
            label ? label : "Scaleform value",
            static_cast<unsigned long>(GetExceptionCode()));
        return false;
    }
#endif
}

std::uint32_t count_tracked_icon_hosts(TabHostMask bits) noexcept {
    std::uint32_t count{};
    while (bits != 0) {
        count += bits & 1U;
        bits >>= 1U;
    }
    return count;
}

std::uint32_t count_active_rgba8_asset_hosts(
    std::size_t asset_index,
    std::size_t override_host,
    std::size_t override_asset) noexcept {
    if (asset_index >= active_custom_tab_count()) return 0;
    const std::size_t encoded_asset = asset_index + 1U;
    std::uint32_t count{};
    for (std::size_t host = 0; host < g_rgba8_host_assets.size(); ++host) {
        const std::size_t encoded = host == override_host
            ? override_asset
            : g_rgba8_host_assets[host].load(std::memory_order_acquire);
        if (encoded == encoded_asset) ++count;
        const std::size_t staged =
            g_staged_rgba8_host_assets[host].load(
                std::memory_order_acquire);
        if (staged == encoded_asset) ++count;
    }
    return count;
}

} // namespace native_tabs_detail
