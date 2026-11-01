#pragma once

// Internal to the native-tabs translation units (native_tabs*.cpp); never include
// it from public headers or other subsystems. Everything that used to live in
// native_tabs.cpp's anonymous namespace and is shared by more than one unit is
// declared here. The namespace sits at global scope, exactly where the
// anonymous namespace was, so unqualified name lookup is unchanged. All global
// state is defined in native_tabs.cpp, in its original order.

#include "tab_viewport.hpp"
#include "tab_initial_presentation_refresh.hpp"
#include "tab_icon_retirement_policy.hpp"
#include "tab_pointer_dwell.hpp"
#include "tab_record_epoch.hpp"
#include "tab_transition_input_gate.hpp"
#include "native_tab_runtime_state.hpp"
#include "native_tab_slot_growth_policy.hpp"
#include "failure_log_budget.hpp"
#include "guarded_memory.hpp"
// r150: also outside production: find_native_tabs_build_profile uses the
// layout adoption in every configuration (the research probe failed since r135).
#include "game_build.hpp"

#if defined(ER_NATIVE_UI_PRODUCTION_TABS)
#include "addresses.hpp"
#include "hooks.hpp"
#include "native_tab_catalogue.hpp"
#include "native_tabs.hpp"
#include "runtime_log.hpp"
#include "runtime_state.hpp"
#endif

#include <Windows.h>
#include <safetyhook.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <exception>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace erui::native {
struct PreservedSharedMenuAddresses;
}

namespace native_tabs_detail {

using erui::detail::TabBoundaryBehavior;
using erui::detail::TabDirection;
using erui::detail::DeferredTabPresentationRefresh;
using erui::detail::DeferredTabPresentationRefreshPoll;
using erui::detail::Rgba8HostRemovalPlan;
using erui::detail::Rgba8RapidEpochHandoffState;
using erui::detail::Rgba8RetirementLimits;
using erui::detail::TabOverflowIndicator;
using erui::detail::TabPointerDwell;
using erui::detail::TabPointerDwellEdge;
using erui::detail::TabPointerDwellPhase;
using erui::detail::TabPointerDwellTiming;
using erui::detail::TabTransitionCommitResult;
using erui::detail::TabTransitionInputAction;
using erui::detail::TabTransitionSelectorAnchor;
using erui::detail::TabViewport;
using erui::detail::TabViewportChange;
using erui::detail::NativeTabSlotGrowthPlan;
using erui::detail::invalid_tab_index;
using erui::detail::native_tab_active_slot_target;
using erui::detail::plan_rgba8_host_removal;
using erui::detail::plan_rgba8_retirement_reconciliation;
using erui::detail::plan_native_tab_slot_growth;
using erui::detail::can_begin_vector_only_epoch_during_rgba8_retirement;
using erui::detail::should_cancel_pointer_dwell_for_composite_input;
using erui::detail::should_use_transparent_initial_raster_placeholder;
using erui::detail::tab_overflow_indicator;
using erui::detail::tab_overflow_indicators_changed;
using erui::detail::tab_transition_input_action;
using erui::detail::tab_transition_selector_anchor;

// Every native tab coordinate is selected from an exact executable identity.
// Do not derive a new build from an older one by a blanket RVA delta: the
// 2.7.2.0 image has several independently shifted code, vtable, and data
// regions. Keeping the complete audited coordinates together makes support
// additive and leaves the proven 2.7.1.0 path byte-for-byte address stable.
struct NativeTabsBuildProfile {
    const char* version{};
    std::uint32_t timestamp{};
    std::size_t image_size{};

    std::uintptr_t selection_callback{};
    std::uintptr_t composite_frame{};
    std::uintptr_t selector_select{};
    std::uintptr_t input_dispatch_frame_begin{};
    std::uintptr_t pointer_dispatch_result{};
    std::uintptr_t horizontal_navigation_commit{};
    std::uintptr_t selection_dispatch{};
    std::uintptr_t grid_hit_test{};
    std::uintptr_t get_pointer_position{};
    std::uintptr_t native_pointer_select{};
    std::uintptr_t pointer_cell_sample{};
    std::uintptr_t switch_panel{};
    std::uintptr_t switch_panel_state_copy_signature{};
    std::uintptr_t switch_panel_state_copy{};
    std::uintptr_t switch_panel_state_copy_resume{};
    std::uintptr_t switch_panel_visibility_signature{};
    std::uintptr_t switch_panel_visibility_decision{};
    std::uintptr_t refresh_title{};
    std::uintptr_t resolve_path{};
    std::uintptr_t value_exists{};
    std::uintptr_t present_tab{};
    std::uintptr_t make_icon_descriptor{};
    std::uintptr_t texture_lookup_call{};
    std::uintptr_t image_bind_call{};
    std::uintptr_t texture_lookup{};
    std::uintptr_t image_bind{};

    std::uintptr_t scaleform_constructor{};
    std::uintptr_t scaleform_get_member{};
    std::uintptr_t scaleform_set_position{};
    std::uintptr_t scaleform_set_visible{};
    std::uintptr_t scaleform_destructor{};
    std::uintptr_t raw_image_create{};
    std::uintptr_t raw_image_get_data{};
    std::uintptr_t image_data_clear{};
    std::uintptr_t raw_image_release{};
    std::uintptr_t raw_image_vtable{};
    std::uintptr_t raw_image_get_type{};
    std::uintptr_t image_resource_constructor{};
    std::uintptr_t image_resource_add_ref{};
    std::uintptr_t image_resource_release{};
    std::uintptr_t image_resource_deleting_destructor{};
    std::uintptr_t image_resource_allocation_call{};
    std::uintptr_t image_resource_vtable{};
    std::uintptr_t image_delegate_vtable{};
    std::uintptr_t scaleform_global_heap{};

    std::uintptr_t top_vtable{};
    std::uintptr_t category_record_vtable{};
    std::uintptr_t selector_vtable{};
    std::uintptr_t option_setting_panel_vtable{};
    std::uintptr_t scene_obj_proxy_vtable{};
    std::uintptr_t scene_obj_proxy_get_value{};

    std::uintptr_t as3_value_interface_vtable{};
    std::uintptr_t as3_value_get_member{};
    std::uintptr_t as3_value_set_member{};
    std::uintptr_t as3_value_invoke{};
    std::uintptr_t as3_value_get_movie{};
    std::uintptr_t as3_movie_vtable{};
    std::uintptr_t as3_movie_create_object{};
    std::uintptr_t static_shape_vtable{};
    std::uintptr_t alternate_authored_icon_vtable{};
    std::uintptr_t dynamic_shape_vtable{};

    std::uintptr_t graphics_panel_materializer{};

    // Configuration construction seams used only when the immutable player
    // configuration requests more than the eight stock physical slots.  Keep
    // the complete ownership-sensitive profile beside the viewport profile:
    // no coordinate is inferred from a neighbouring executable build.
    std::uintptr_t top_constructor{};
    std::uintptr_t early_slot_growth{};
    std::uintptr_t first_count_publication{};
    std::uintptr_t selector_geometry_dispatch{};
    std::uintptr_t post_geometry_observation{};
    std::uintptr_t count_propagation_dispatch{};
    std::uintptr_t post_constructor_observation{};
    std::uintptr_t category_append{};
    std::uintptr_t category_count_getter{};
    std::uintptr_t selector_set_active_count{};
    std::uintptr_t selector_geometry_enumerator{};
    std::uintptr_t category_list_vtable{};
    std::uintptr_t scene_obj_proxy_value_vtable{};
};

inline constexpr std::array<NativeTabsBuildProfile, 2>
    kNativeTabsBuildProfiles{{
        {
            "2.7.1.0", 0x6A96B418, 0x5E0DA00,
            0x968510, 0x93D5E0, 0x739B90, 0x73A196, 0x73A1C7,
            0x73A358, 0x73A394, 0x737AE0, 0x758940, 0x73CA60,
            0x73B880, 0x93C900, 0x93C932, 0x93C93C, 0x93C963,
            0x93C977, 0x93C989, 0x968C40, 0x74B140, 0x733FA0,
            0x967DE0, 0x73E230, 0x74CCDF, 0x74CDC7, 0xD65C00,
            0xD833F0,
            0xD814A0, 0xD816F0, 0xD85BD0, 0xD86280, 0xD81600,
            0x114A820, 0x114AA40, 0x1146AD0, 0x112D640, 0x2CC27E0,
            0x114B9C0, 0xD61C90, 0x113D9E0, 0x113DA00, 0xD62340,
            0xD65DA5, 0x2BBAAF8, 0x2BBA9E8, 0x45972D0,
            0x2B16B48, 0x2AD6F20, 0x2A94438, 0x2B14888, 0x2A97AF0,
            0x74C930,
            0x2CBF988, 0x10DDD10, 0x10DE0D0, 0x10DE510, 0x10E26D0,
            0x2CBE5C0, 0x10D4460, 0x2CBD4E0, 0x2CC6268, 0x2CBD158,
            0x95C050,
            0x9672C0, 0x96773F, 0x967757, 0x967935, 0x96793E,
            0x96795B, 0x8074A7, 0x968CF0, 0x968DA0, 0x739C10,
            0x738AB0, 0x2B16B10, 0x2A97AD8,
        },
        {
            "2.7.2.0", 0x6AABF7E7, 0x5DFD600,
            0x9689C0, 0x93DA90, 0x73A040, 0x73A646, 0x73A677,
            0x73A808, 0x73A844, 0x737F90, 0x758DF0, 0x73CF10,
            0x73BD30, 0x93CDB0, 0x93CDE2, 0x93CDEC, 0x93CE13,
            0x93CE27, 0x93CE39, 0x9690F0, 0x74B5F0, 0x734450,
            0x968290, 0x73E6E0, 0x74D18F, 0x74D277, 0xD672F0,
            0xD84AE0,
            0xD82B90, 0xD82DE0, 0xD872C0, 0xD87970, 0xD82CF0,
            0x114BF10, 0x114C130, 0x11481C0, 0x112ED30, 0x2CC4A90,
            0x114D0B0, 0xD63380, 0x113F0D0, 0x113F0F0, 0xD63A30,
            0xD67495, 0x2BBCDA8, 0x2BBCC98, 0x459A2F0,
            0x2B18C38, 0x2AD9010, 0x2A96528, 0x2B16978, 0x2A99BE0,
            0x74CDE0,
            0x2CC1C38, 0x10DF400, 0x10DF7C0, 0x10DFC00, 0x10E3DC0,
            0x2CC0870, 0x10D5B50, 0x2CBF790, 0x2CC8518, 0x2CBF408,
            0x95C500,
            0x967770, 0x967BEF, 0x967C07, 0x967DE5, 0x967DEE,
            0x967E0B, 0x807957, 0x9691A0, 0x969250, 0x73A0C0,
            0x738F60, 0x2B18C00, 0x2A99BC8,
        },
    }};
static_assert(
    kNativeTabsBuildProfiles[0].timestamp == 0x6A96B418 &&
    kNativeTabsBuildProfiles[0].selection_callback == 0x968510 &&
    kNativeTabsBuildProfiles[0].image_bind == 0xD833F0 &&
    kNativeTabsBuildProfiles[0].raw_image_vtable == 0x2CC27E0 &&
    kNativeTabsBuildProfiles[0].scaleform_global_heap == 0x45972D0 &&
    kNativeTabsBuildProfiles[0].top_vtable == 0x2B16B48 &&
    kNativeTabsBuildProfiles[0].as3_value_interface_vtable == 0x2CBF988 &&
    kNativeTabsBuildProfiles[0].graphics_panel_materializer == 0x95C050 &&
    kNativeTabsBuildProfiles[0].early_slot_growth == 0x96773F &&
    kNativeTabsBuildProfiles[0].category_append == 0x968CF0);
static_assert(
    kNativeTabsBuildProfiles[1].timestamp == 0x6AABF7E7 &&
    kNativeTabsBuildProfiles[1].selection_callback == 0x9689C0 &&
    kNativeTabsBuildProfiles[1].image_bind == 0xD84AE0 &&
    kNativeTabsBuildProfiles[1].raw_image_vtable == 0x2CC4A90 &&
    kNativeTabsBuildProfiles[1].scaleform_global_heap == 0x459A2F0 &&
    kNativeTabsBuildProfiles[1].top_vtable == 0x2B18C38 &&
    kNativeTabsBuildProfiles[1].as3_value_interface_vtable == 0x2CC1C38 &&
    kNativeTabsBuildProfiles[1].graphics_panel_materializer == 0x95C500 &&
    kNativeTabsBuildProfiles[1].early_slot_growth == 0x967BEF &&
    kNativeTabsBuildProfiles[1].category_append == 0x9691A0);

extern const NativeTabsBuildProfile* g_native_tabs_build_profile;

[[nodiscard]] const NativeTabsBuildProfile* find_native_tabs_build_profile(
    std::uint32_t timestamp,
    std::size_t image_size) noexcept;

[[nodiscard]] const NativeTabsBuildProfile& tabs_build() noexcept;

// A typed RVA handle keeps the call sites readable while making every value
// come from the one active, exact-build profile above. It deliberately has no
// fallback value: callers select a profile before any native validation or
// callback can run.
struct NativeTabsRva {
    std::uintptr_t NativeTabsBuildProfile::*member{};

    [[nodiscard]] operator std::uintptr_t() const noexcept {
        return tabs_build().*member;
    }
};

inline constexpr NativeTabsRva kSelectionCallbackRva{
    &NativeTabsBuildProfile::selection_callback};
inline constexpr NativeTabsRva kCompositeFrameRva{
    &NativeTabsBuildProfile::composite_frame};
inline constexpr NativeTabsRva kSelectorSelectRva{
    &NativeTabsBuildProfile::selector_select};
inline constexpr NativeTabsRva kInputDispatchFrameBeginRva{
    &NativeTabsBuildProfile::input_dispatch_frame_begin};
inline constexpr NativeTabsRva kPointerDispatchResultRva{
    &NativeTabsBuildProfile::pointer_dispatch_result};
inline constexpr NativeTabsRva kHorizontalNavigationCommitRva{
    &NativeTabsBuildProfile::horizontal_navigation_commit};
inline constexpr NativeTabsRva kSelectionDispatchRva{
    &NativeTabsBuildProfile::selection_dispatch};
inline constexpr NativeTabsRva kGridHitTestRva{
    &NativeTabsBuildProfile::grid_hit_test};
inline constexpr NativeTabsRva kGetPointerPositionRva{
    &NativeTabsBuildProfile::get_pointer_position};
inline constexpr NativeTabsRva kNativePointerSelectRva{
    &NativeTabsBuildProfile::native_pointer_select};
inline constexpr NativeTabsRva kPointerCellSampleRva{
    &NativeTabsBuildProfile::pointer_cell_sample};
inline constexpr NativeTabsRva kSwitchPanelRva{
    &NativeTabsBuildProfile::switch_panel};
inline constexpr NativeTabsRva kSwitchPanelStateCopyRva{
    &NativeTabsBuildProfile::switch_panel_state_copy};
inline constexpr NativeTabsRva kSwitchPanelStateCopyResumeRva{
    &NativeTabsBuildProfile::switch_panel_state_copy_resume};
inline constexpr NativeTabsRva kRefreshTitleRva{
    &NativeTabsBuildProfile::refresh_title};
inline constexpr NativeTabsRva kResolvePathRva{
    &NativeTabsBuildProfile::resolve_path};
inline constexpr NativeTabsRva kValueExistsRva{
    &NativeTabsBuildProfile::value_exists};
inline constexpr NativeTabsRva kPresentTabRva{
    &NativeTabsBuildProfile::present_tab};
inline constexpr NativeTabsRva kMakeIconDescriptorRva{
    &NativeTabsBuildProfile::make_icon_descriptor};
inline constexpr NativeTabsRva kTextureLookupCallRva{
    &NativeTabsBuildProfile::texture_lookup_call};
inline constexpr NativeTabsRva kImageBindCallRva{
    &NativeTabsBuildProfile::image_bind_call};
inline constexpr NativeTabsRva kTextureLookup271Rva{
    &NativeTabsBuildProfile::texture_lookup};
inline constexpr NativeTabsRva kImageBind271Rva{
    &NativeTabsBuildProfile::image_bind};
inline constexpr NativeTabsRva kScaleformConstructor271Rva{
    &NativeTabsBuildProfile::scaleform_constructor};
inline constexpr NativeTabsRva kScaleformGetMember271Rva{
    &NativeTabsBuildProfile::scaleform_get_member};
inline constexpr NativeTabsRva kScaleformSetPosition271Rva{
    &NativeTabsBuildProfile::scaleform_set_position};
inline constexpr NativeTabsRva kScaleformSetVisible271Rva{
    &NativeTabsBuildProfile::scaleform_set_visible};
inline constexpr NativeTabsRva kScaleformDestructor271Rva{
    &NativeTabsBuildProfile::scaleform_destructor};
inline constexpr NativeTabsRva kRawImageCreate271Rva{
    &NativeTabsBuildProfile::raw_image_create};
inline constexpr NativeTabsRva kRawImageGetData271Rva{
    &NativeTabsBuildProfile::raw_image_get_data};
inline constexpr NativeTabsRva kImageDataClear271Rva{
    &NativeTabsBuildProfile::image_data_clear};
inline constexpr NativeTabsRva kRawImageRelease271Rva{
    &NativeTabsBuildProfile::raw_image_release};
inline constexpr NativeTabsRva kRawImageVtableRva{
    &NativeTabsBuildProfile::raw_image_vtable};
inline constexpr NativeTabsRva kRawImageGetType271Rva{
    &NativeTabsBuildProfile::raw_image_get_type};
inline constexpr NativeTabsRva kScaleformImageResourceCtor271Rva{
    &NativeTabsBuildProfile::image_resource_constructor};
inline constexpr NativeTabsRva kScaleformImageResourceAddRef271Rva{
    &NativeTabsBuildProfile::image_resource_add_ref};
inline constexpr NativeTabsRva kScaleformImageResourceRelease271Rva{
    &NativeTabsBuildProfile::image_resource_release};
inline constexpr NativeTabsRva kScaleformImageResourceDeletingDestructor271Rva{
    &NativeTabsBuildProfile::image_resource_deleting_destructor};
inline constexpr NativeTabsRva kScaleformImageResourceAllocationCall271Rva{
    &NativeTabsBuildProfile::image_resource_allocation_call};
inline constexpr NativeTabsRva kScaleformImageResourceVtable271Rva{
    &NativeTabsBuildProfile::image_resource_vtable};
inline constexpr NativeTabsRva kScaleformImageDelegateVtable271Rva{
    &NativeTabsBuildProfile::image_delegate_vtable};
inline constexpr NativeTabsRva kScaleformGlobalHeap271Rva{
    &NativeTabsBuildProfile::scaleform_global_heap};
inline constexpr NativeTabsRva kTopVtableRva{
    &NativeTabsBuildProfile::top_vtable};
inline constexpr NativeTabsRva kCategoryRecordVtableRva{
    &NativeTabsBuildProfile::category_record_vtable};
inline constexpr NativeTabsRva kSelectorVtableRva{
    &NativeTabsBuildProfile::selector_vtable};
inline constexpr NativeTabsRva kOptionSettingPanelVtableRva{
    &NativeTabsBuildProfile::option_setting_panel_vtable};
inline constexpr NativeTabsRva kSceneObjProxyVtableRva{
    &NativeTabsBuildProfile::scene_obj_proxy_vtable};
inline constexpr NativeTabsRva kSceneObjProxyGetValueRva{
    &NativeTabsBuildProfile::scene_obj_proxy_get_value};
inline constexpr NativeTabsRva kAs3ValueInterfaceVtable271Rva{
    &NativeTabsBuildProfile::as3_value_interface_vtable};
inline constexpr NativeTabsRva kAs3ValueGetMember271Rva{
    &NativeTabsBuildProfile::as3_value_get_member};
inline constexpr NativeTabsRva kAs3ValueInvoke271Rva{
    &NativeTabsBuildProfile::as3_value_invoke};
inline constexpr NativeTabsRva kAs3ValueGetMovie271Rva{
    &NativeTabsBuildProfile::as3_value_get_movie};
inline constexpr NativeTabsRva kAs3MovieVtable271Rva{
    &NativeTabsBuildProfile::as3_movie_vtable};
inline constexpr NativeTabsRva kAs3MovieCreateObject271Rva{
    &NativeTabsBuildProfile::as3_movie_create_object};
inline constexpr NativeTabsRva kDynamicShapeVtable271Rva{
    &NativeTabsBuildProfile::dynamic_shape_vtable};
inline constexpr NativeTabsRva kTopConstructorRva{
    &NativeTabsBuildProfile::top_constructor};
inline constexpr NativeTabsRva kEarlySlotGrowthRva{
    &NativeTabsBuildProfile::early_slot_growth};
inline constexpr NativeTabsRva kFirstCountPublicationRva{
    &NativeTabsBuildProfile::first_count_publication};
inline constexpr NativeTabsRva kSelectorGeometryDispatchRva{
    &NativeTabsBuildProfile::selector_geometry_dispatch};
inline constexpr NativeTabsRva kPostGeometryObservationRva{
    &NativeTabsBuildProfile::post_geometry_observation};
inline constexpr NativeTabsRva kCountPropagationDispatchRva{
    &NativeTabsBuildProfile::count_propagation_dispatch};
inline constexpr NativeTabsRva kPostConstructorObservationRva{
    &NativeTabsBuildProfile::post_constructor_observation};
inline constexpr NativeTabsRva kCategoryAppendRva{
    &NativeTabsBuildProfile::category_append};
inline constexpr NativeTabsRva kCategoryCountGetterRva{
    &NativeTabsBuildProfile::category_count_getter};
inline constexpr NativeTabsRva kSelectorSetActiveCountRva{
    &NativeTabsBuildProfile::selector_set_active_count};
inline constexpr NativeTabsRva kSelectorGeometryEnumeratorRva{
    &NativeTabsBuildProfile::selector_geometry_enumerator};
inline constexpr NativeTabsRva kCategoryListVtableRva{
    &NativeTabsBuildProfile::category_list_vtable};
inline constexpr NativeTabsRva kSceneObjProxyValueVtableRva{
    &NativeTabsBuildProfile::scene_obj_proxy_value_vtable};

// The GridControl frame clears provenance before it enters input dispatch.
// Pointer selection is published by the earlier input helper. Non-pointer
// horizontal navigation converges at one validated commit seam carrying a
// signed x delta. These seams carry exact provenance into the ordinary
// selection callback without guessing from the resulting index or timing.
// Every native input path has finished choosing a physical tab when execution
// reaches this comparison. R12D is the physical selection from frame entry,
// RDI is the selector, and [RDI+0xD4] is the final selection. A timed pointer
// dwell uses the same native cell setter as the ordinary pointer path just
// before this comparison; Elden Ring then owns its normal change dispatch.
// The GridControl pointer path has already converted the native cursor
// coordinates and completed its per-cell hit test at this seam. RSI is the
// selector, signed EAX is the hovered physical cell (or -1), and the two
// floats at RSP+0x20/RSP+0x24 are the exact coordinates used by that hit test.
// During an armed dwell, rewriting EAX to -1 takes the native no-hit branch
// and suppresses only repaint-induced reselection under a stationary cursor.
// Scaleform::Render::RawImage is a system-memory image and does not require
// the DrawableImage provider that this Configuration movie lacks. The first
// RawImage gate first creates, inspects, fills, and verifies an immutable
// system-memory image. The current visual follow-up wraps that exact image in
// the native ScaleformImageResource used by the game's texture repository,
// keeps one process-lifetime owner, and returns ordinary temporary references
// through the validated texture-lookup contract. Elden Ring's unchanged image
// binder then paints a runtime-created flash.display.Shape owned by the same
// tab-icon parent; the authored static Shape is retained only as a fallback.
inline constexpr std::size_t kScaleformImageResourceSize = 0x98;

inline constexpr std::size_t kMovieContextOffset = 0x120;
inline constexpr std::size_t kSelectorOffset = 0x0A38;
inline constexpr std::size_t kSelectorActiveCountOffset = 0x00D0;
inline constexpr std::size_t kSelectorSelectedIndexOffset = 0x00D4;
inline constexpr std::size_t kSelectorHorizontalExtentOffset = 0x00D8;
inline constexpr std::size_t kSelectorVerticalExtentOffset = 0x00DC;
inline constexpr std::size_t kCategoryListOffset = 0x1200;
inline constexpr std::size_t kCategoryStorageOffset = 0x1208;
inline constexpr std::size_t kCategoryCountOffset = 0x1760;
inline constexpr std::size_t kCompositeOffset = 0x1768;
inline constexpr std::size_t kTopControllerOffset = 0x1870;
inline constexpr std::size_t kCompositePanelCacheOffset = 0x68;
inline constexpr std::size_t kCompositeCurrentPanelOffset = 0xB8;
inline constexpr std::size_t kCategoryRecordSize = 0x88;
inline constexpr std::size_t kTitleOffset = 0x08;
inline constexpr std::size_t kCategoryIdOffset = 0x40;
inline constexpr std::size_t kPresentationOffset = 0x48;
inline constexpr std::size_t kPresentationSize = 0x40;
inline constexpr std::size_t kSceneObjProxySize = 0x60;
inline constexpr std::size_t kSceneObjProxyValueOffset = 0x28;
inline constexpr std::size_t kPanelVisualCapacityOffset = 0xB14;
inline constexpr std::size_t kPanelNativeRowCountOffset = 0x1AF0;

// Elden Ring contributes eight stock logical routes. This is deliberately
// distinct from the live number of selectable physical cells exposed by the
// current Configuration movie. The native category container has room for at
// most ten records; selector + 0xD0 decides how many are active per epoch.
inline constexpr std::size_t kStockLogicalTabCount = 8;
inline constexpr std::size_t kPhysicalTabCapacity = 10;
inline constexpr std::size_t kTabHostCapacity = kPhysicalTabCapacity * 2U;

#if defined(ER_NATIVE_UI_PRODUCTION_TABS)
static_assert(
    kStockLogicalTabCount == erui::detail::native_tab_stock_slot_count);
static_assert(
    kPhysicalTabCapacity ==
        erui::detail::native_tab_physical_slot_capacity);
static_assert(
    kPhysicalTabCapacity == erui::detail::native_tab_growth_capacity);
static_assert(
    kTabHostCapacity == erui::detail::native_tab_physical_host_capacity);

#endif
using TabHostMask = std::uint32_t;
using AtomicTabHostMask = std::atomic_uint32_t;

#if defined(ER_NATIVE_UI_PRODUCTION_TABS)
// Native Graphics and each registered custom route sequentially reuse
// category 8's selector-11 rows-only carrier. Stock Network is synchronous
// parking. The standalone research fixture below remains fixed at 50 routes.
inline constexpr std::size_t kCustomTabCount = ERUI_CUSTOM_TAB_MAX_COUNT;

#else
inline constexpr std::size_t kCustomTabCount = 50;

#endif
inline constexpr std::size_t kLogicalTabCount =
    kStockLogicalTabCount + kCustomTabCount;

#if defined(ER_NATIVE_UI_PRODUCTION_TABS)
inline constexpr bool kStressQuietSuccessLogging = false;

#else
inline constexpr bool kStressQuietSuccessLogging = kCustomTabCount >= 40;

#endif
inline constexpr std::size_t kTitleCapacity = 96;

#if defined(ER_NATIVE_UI_PRODUCTION_TABS)
static_assert(
    kTitleCapacity == ERUI_CUSTOM_TAB_TITLE_MAX_CODE_UNITS + 1U,
    "native tab title storage must match the public API 1.3 contract");

#endif
inline constexpr std::size_t kNavigationMargin = 1;
static_assert(
    kTabHostCapacity <= std::numeric_limits<TabHostMask>::digits);

#if !defined(ER_NATIVE_UI_PRODUCTION_TABS)
static_assert(kCustomTabCount <=
    std::numeric_limits<std::uint8_t>::max());
static_assert(kCustomTabCount < std::numeric_limits<std::uint64_t>::digits);
static_assert(kCustomTabCount <= 99U);

#endif
inline constexpr std::uint64_t kInPlaceRebindRetirementTimeoutFrames = 120;
inline constexpr std::uint64_t kPointerDwellInitialDelayMs = 250;
inline constexpr std::uint64_t kPointerDwellRepeatDelayMs = 200;
inline constexpr std::uint64_t kPointerDwellDecayMs = 20;
inline constexpr std::uint64_t kPointerDwellMinimumDelayMs = 50;
inline constexpr std::array<std::int32_t, kStockLogicalTabCount>
    kExpectedStockCategories{0, 1, 2, 3, 5, 6, 7, 8};
constexpr bool is_research_custom_category(std::int32_t category) noexcept {
    return category == 8;
}

#if defined(ER_NATIVE_UI_PRODUCTION_TABS)
extern const erui::detail::NativeTabCatalogue* g_native_tab_catalogue;
extern std::unique_ptr<erui::detail::NativeTabRuntimeState>
    g_native_tab_runtime;

#endif
[[nodiscard]] std::size_t active_custom_tab_count() noexcept;
[[nodiscard]] std::size_t active_logical_tab_count() noexcept;

[[nodiscard]] std::size_t active_physical_tab_count() noexcept;
[[nodiscard]] std::size_t active_tab_host_count() noexcept;
[[nodiscard]] TabHostMask active_tab_host_mask() noexcept;
inline constexpr std::uint32_t kMaximumTransitionLogs = 512;
inline constexpr std::size_t kFailureLogSites = 64;
inline constexpr std::uint32_t kMaximumRecordMismatchLogs = 8;
inline constexpr std::uint32_t kMaximumWrapDiagnosticLogs = 36;
inline constexpr std::uint32_t kMaximumAliasLogs = 128;
inline constexpr std::uint32_t kMaximumImageBindLogs = 1024;
inline constexpr std::uint32_t kMaximumShapeAttachLogs = 1024;
inline constexpr std::uint32_t kMaximumStateIsolationLogs = 64;
inline constexpr std::uint32_t kMaximumAliasVisibilityLogs = 64;
inline constexpr std::uint32_t kMaximumCarrierSelectionLogs = 64;
inline constexpr std::uint32_t kMaximumPointerSampleLogs = 192;
inline constexpr std::uint32_t kMaximumPointerDwellLogs = 1024;
inline constexpr std::uint64_t kPointerPollReportMs = 1000;
inline constexpr std::int32_t kProbeBitmapWidth = 32;
inline constexpr std::int32_t kProbeBitmapHeight = 32;
// A single post-native-composite BitmapData.setPixel32 call deterministically
// blackened the renderer even though Invoke returned true, no SEH was raised,
// and the exact rooted object survived the following frame. Keep the failed
// gate in the research source, but make deployed builds return to the proven
// ownership-only baseline until a different pixel-upload boundary is isolated.
inline constexpr bool kEnableBitmapDataPixelMutationProbe = false;
// The ownership-only BitmapData lifecycle is proven, and the passive VM probe
// found both its DrawableImage context and provider null. Keep that experiment
// available as research history, but do not construct another BitmapData while
// the next gate characterizes a known-good immutable native image resource.
inline constexpr bool kEnableBitmapDataOwnershipProbe = false;
inline constexpr bool kEnableRawImageLifecycleProbe = true;
inline constexpr bool kEnableNativeImageResourceLifecycleProbe = true;

inline constexpr std::uint32_t kTabContentResearchMarker = 0x54414231;
inline constexpr char kTabContentResearchMarkerExport[] =
    "ERUI_TabContentResearchProbeMarker";
inline constexpr std::int32_t kOverflowIconId = 90002;
inline constexpr std::int32_t kFirstRgba8IconId = 91000;

template <std::size_t Count>
constexpr std::array<std::int32_t, Count> make_rgba8_icon_ids() noexcept {
    std::array<std::int32_t, Count> result{};
    for (std::size_t index = 0; index < Count; ++index) {
        result[index] = kFirstRgba8IconId +
            static_cast<std::int32_t>(index);
    }
    return result;
}

template <std::size_t Capacity, std::size_t PrefixSize>
constexpr void make_fixed_decimal_name(
    std::array<wchar_t, Capacity>& output,
    const wchar_t (&prefix)[PrefixSize],
    std::uint32_t value,
    std::size_t digits) noexcept {
    static_assert(PrefixSize > 0);
    const std::size_t prefix_length = PrefixSize - 1U;
    for (std::size_t index = 0; index < prefix_length; ++index) {
        output[index] = prefix[index];
    }
    for (std::size_t digit = 0; digit < digits; ++digit) {
        const std::size_t reverse = digits - 1U - digit;
        output[prefix_length + reverse] =
            static_cast<wchar_t>(L'0' + value % 10U);
        value /= 10U;
    }
    output[prefix_length + digits] = L'\0';
}

template <std::size_t Count>
constexpr std::array<std::array<wchar_t, 32>, Count>
make_rgba8_icon_resource_names() noexcept {
    std::array<std::array<wchar_t, 32>, Count> result{};
    for (std::size_t index = 0; index < Count; ++index) {
        make_fixed_decimal_name(
            result[index],
            L"MENU_ItemIcon_",
            static_cast<std::uint32_t>(kFirstRgba8IconId + index),
            5U);
    }
    return result;
}

template <std::size_t Count>
constexpr std::array<std::array<wchar_t, 48>, Count>
make_raw_image_resource_names() noexcept {
    std::array<std::array<wchar_t, 48>, Count> result{};
    for (std::size_t index = 0; index < Count; ++index) {
        make_fixed_decimal_name(
            result[index],
            L"ERNativeUI.TabProbe.RawRGBA8.",
            static_cast<std::uint32_t>(index + 1U),
            2U);
    }
    return result;
}

#if !defined(ER_NATIVE_UI_PRODUCTION_TABS)
inline constexpr auto kRgba8IconIds = make_rgba8_icon_ids<kCustomTabCount>();
inline constexpr auto kRgba8IconResourceNames =
    make_rgba8_icon_resource_names<kCustomTabCount>();
inline constexpr auto kRawImageResourceProbeNames =
    make_raw_image_resource_names<kCustomTabCount>();
static_assert(kRgba8IconIds.front() == 91000);
static_assert(kRgba8IconIds.back() == 91049);
static_assert(kRgba8IconIds.front() != kOverflowIconId);
static_assert(kRgba8IconResourceNames.front()[14] == L'9');
static_assert(kRawImageResourceProbeNames.front()[29] == L'0');

#endif
// The forward-only path proves that Elden Ring's Graphics icon bounds are a
// stable canvas for every synthetic cross. A direct reverse wrap can otherwise
// expose the old Network, Controller, or Keyboard/Mouse bounds in each cell,
// which makes identical percentage-based drawings appear at different scales.
inline constexpr double kCrossCanvasLeft = -27.200;
inline constexpr double kCrossCanvasTop = -23.050;
inline constexpr double kCrossCanvasWidth = 54.599;
inline constexpr double kCrossCanvasHeight = 48.100;
inline constexpr wchar_t kCrossIconResource[] = L"MENU_ItemIcon_90001";
inline constexpr wchar_t kOverflowIconResource[] = L"MENU_ItemIcon_90002";
// Use a known-native property icon for the first alias experiment. No client
// bytes or custom texture allocation are involved yet.
inline constexpr wchar_t kAliasedNativeIconResource[] = L"MENU_PropertyIcon_30050";
// Runtime Shapes use two parent-owned property banks.  A remap paints a
// changed target into the inactive bank while the active Shape remains in the
// display list, then removes the old bank only after the target has been
// attached and verified.  Scaleform therefore never gets a render boundary
// at which only the authored slot icon is visible.
inline constexpr char kInjectedIconProperties[2][48]{
    "__ernativeui_tab_viewport_probe_icon_a",
    "__ernativeui_tab_viewport_probe_icon_b",
};
inline constexpr char kInjectedBitmapDataProperty[] =
    "__ernativeui_tab_viewport_probe_bitmap_data";
inline constexpr char kAuthoredIconProperties[2][56]{
    "__ernativeui_tab_viewport_probe_authored_icon_a",
    "__ernativeui_tab_viewport_probe_authored_icon_b",
};
inline constexpr std::uint8_t kIconPropertyBankB = 1;

constexpr const char* injected_icon_property(
    std::uint8_t bank) noexcept {
    return kInjectedIconProperties[bank == kIconPropertyBankB ? 1 : 0];
}

constexpr const char* authored_icon_property(
    std::uint8_t bank) noexcept {
    return kAuthoredIconProperties[bank == kIconPropertyBankB ? 1 : 0];
}

// Non-viewport research probes remain pinned to bank A.  The production-path
// stress transaction below selects a bank explicitly for every tab host.
inline constexpr const char* kInjectedIconProperty = kInjectedIconProperties[0];
inline constexpr const char* kAuthoredIconProperty = kAuthoredIconProperties[0];

using SelectionCallbackFn = void(__fastcall*)(
    void* callable,
    const std::int32_t* physical_index);
using CompositeFrameFn = void(__fastcall*)(
    void* composite,
    float frame_value,
    const std::uint8_t* input_enabled);
using SelectorSelectFn = bool(__fastcall*)(
    void* selector,
    std::uint32_t physical_index,
    std::uint8_t suppress_selection_callbacks);
struct PointerPosition {
    float x{};
    float y{};
};
static_assert(sizeof(PointerPosition) == 8);
using GetPointerPositionFn = PointerPosition*(__fastcall*)(
    const std::uint8_t* input_gate,
    PointerPosition* output);
using GridHitTestFn = std::int32_t(__fastcall*)(
    void* selector,
    const PointerPosition* point);
using NativePointerSelectFn = std::int32_t(__fastcall*)(
    void* selector,
    std::uint32_t physical_index);
using SwitchPanelFn = void(__fastcall*)(
    void* composite,
    std::int32_t category);
using ResearchParkingSwitchFn = bool(__cdecl*)(void*, std::int32_t);
using ResearchPrepareCarrierFn = bool(__cdecl*)(void*, std::size_t, std::uint64_t, std::int32_t*, ResearchParkingSwitchFn);
using ResearchVerifyCarrierFn = bool(__cdecl*)(void*, std::size_t, std::int32_t);
using ResearchCarrierVersionFn = std::uint32_t(__cdecl*)();
extern ResearchPrepareCarrierFn g_research_prepare_carrier;
extern ResearchVerifyCarrierFn g_research_verify_carrier;
extern SafetyHookInline g_research_switch_hook;
extern std::atomic<SwitchPanelFn> g_original_research_switch;
extern bool g_research_explicit_switch;
extern bool g_research_switch_succeeded;
extern bool g_research_failed;
extern std::uint64_t g_research_carrier_teardown_generation;
extern std::uint64_t g_research_completed_composite_frames;
using RefreshTitleFn = void(__fastcall*)(void* top);
using ResolvePathFn = void*(__fastcall*)(
    void* movie_context,
    void* destination,
    const char* path_format,
    ...);
using ValueExistsFn = bool(__fastcall*)(void* scene_obj_proxy);
using PresentTabFn = void(__fastcall*)(
    void* top_reference,
    void* scene_obj_proxy,
    std::uint32_t physical_index);
using MakeIconDescriptorFn = void*(__fastcall*)(
    void* descriptor,
    std::int32_t icon_id);
using TextureLookupFn = void**(__fastcall*)(
    void* repository,
    void** out_resource,
    const wchar_t* resource_name);
using ImageBindFn = bool(__fastcall*)(
    void* scaleform_value,
    void** resource,
    const float* rectangle);
using ValueInvokeFn = bool(__fastcall*)(
    void* object_interface,
    void* object_payload,
    void* result,
    const char* method_name,
    const void* arguments,
    std::size_t argument_count,
    bool is_display_object);
using ValueSetMemberFn = bool(__fastcall*)(
    void* object_interface,
    void* object_payload,
    const char* member_name,
    const void* value,
    bool is_display_object);
using ValueGetMovieFn = void*(__fastcall*)(void* object_interface);
using MovieCreateObjectFn = void(__fastcall*)(
    void* movie_root,
    void* out_value,
    const char* class_name,
    const void* arguments,
    std::uint32_t argument_count);
using ScaleformConstructorFn = void*(__fastcall*)(void* nested_value);
using ScaleformGetMemberFn = void*(__fastcall*)(
    void* source_value,
    void* destination_value,
    const char* const* member_name);
using ScaleformSetPositionFn = void*(__fastcall*)(
    void* scaleform_value,
    float x,
    float y);
using ScaleformSetVisibleFn = void*(__fastcall*)(
    void* scaleform_value,
    std::uint8_t visible);
using ScaleformDestructorFn = void(__fastcall*)(void* nested_value);
using RawImageCreateFn = void*(__fastcall*)(
    std::uint32_t format,
    std::uint32_t mip_levels,
    const void* size,
    std::uint32_t use,
    void* heap,
    void* update_sync);
using RawImageGetDataFn = bool(__fastcall*)(void* image, void* image_data);
using ImageDataClearFn = void(__fastcall*)(void* image_data);
using RawImageReleaseFn = void(__fastcall*)(void* image);
using NativeHeapAllocFn = void*(__fastcall*)(
    void* heap,
    std::size_t size,
    void* allocation_info);
using ScaleformImageResourceCtorFn = void*(__fastcall*)(
    void* storage,
    const wchar_t* name,
    void* image,
    std::uint32_t kind);
using ScaleformImageResourceAddRefFn = void(__fastcall*)(void* resource);
using ScaleformImageResourceReleaseFn = void(__fastcall*)(void* resource);
using CategoryAppendFn = void(__fastcall*)(
    void* storage,
    const void* source_record);

struct RawImageSize {
    std::uint32_t width{};
    std::uint32_t height{};
};

struct RawImagePlane {
    std::uint32_t width{};
    std::uint32_t height{};
    std::uintptr_t pitch{};
    std::uintptr_t data_size{};
    std::byte* data{};
};

struct RawImageData {
    std::uint32_t format{};
    std::uint32_t use{};
    std::uint8_t flags{};
    std::uint8_t level_count{};
    std::uint16_t raw_plane_count{};
    std::uint32_t padding{};
    RawImagePlane* planes{};
    void* palette{};
    RawImagePlane plane0{};
};

struct Rgba8Pixel {
    std::uint8_t red{};
    std::uint8_t green{};
    std::uint8_t blue{};
    std::uint8_t alpha{};
};

[[nodiscard]] Rgba8Pixel rgba8_probe_pixel(
    std::size_t icon,
    std::uint32_t x,
    std::uint32_t y) noexcept;

[[nodiscard]] std::uint64_t rgba8_probe_icon_checksum(
    std::size_t icon) noexcept;

[[nodiscard]] bool catalogue_icon_requires_raster(
    std::size_t icon) noexcept;

[[nodiscard]] bool rgba8_probe_catalogue_is_unique_and_alpha_profiled(
    std::span<const std::uint64_t> checksums) noexcept;

static_assert(sizeof(RawImageSize) == 0x08);
static_assert(sizeof(RawImagePlane) == 0x20);
static_assert(offsetof(RawImagePlane, pitch) == 0x08);
static_assert(offsetof(RawImagePlane, data_size) == 0x10);
static_assert(offsetof(RawImagePlane, data) == 0x18);
static_assert(sizeof(RawImageData) == 0x40);
static_assert(offsetof(RawImageData, flags) == 0x08);
static_assert(offsetof(RawImageData, level_count) == 0x09);
static_assert(offsetof(RawImageData, raw_plane_count) == 0x0A);
static_assert(offsetof(RawImageData, planes) == 0x10);
static_assert(offsetof(RawImageData, palette) == 0x18);
static_assert(offsetof(RawImageData, plane0) == 0x20);

struct GameImage {
    std::uint8_t* base{};
    std::size_t size{};
    std::uint32_t timestamp{};
};

struct ShapeAttachProbeResult {
    bool resource_consumed{};
    bool reused_existing_shape{};
    bool created_new_shape{};
    bool custom_visible{};
    bool cleanup_exact{};
    bool bound{};
};

struct Rgba8ShapeAttachProbeResult {
    bool resource_consumed{};
    bool raster_bind_attempted{};
    bool raster_bound{};
    bool pre_bind_graphics_clear_required{};
    bool pre_bind_graphics_clear_attempted{};
    bool pre_bind_graphics_cleared{};
    bool pre_bind_old_raw_before_read{};
    std::uint32_t pre_bind_old_raw_before{};
    bool pre_bind_old_raw_after_read{};
    std::uint32_t pre_bind_old_raw_after{};
    bool pre_bind_old_raw_released_exact{};
    bool reference_before_read{};
    std::uint32_t reference_before{};
    bool reference_after_read{};
    std::uint32_t reference_after{};
    bool exact_reference_transition{};
    bool vector_fallback_visible{};
    bool custom_visible{};
    bool host_retained{};
    bool reused_existing_shape{};
    bool created_new_shape{};
    bool same_asset_bind_skipped{};
    bool cleanup_exact{};
    bool bound{};
    unsigned long graphics_clear_seh{};
    unsigned long consume_seh{};
};

struct RetainedRgba8Resource {
    void* image{};
    void* resource{};
    std::uint64_t checksum{};
    bool ready{};
};

// Every GFx wrapper participates in an intrusive ObjectInterface list while it
// is constructed. Keep the RGBA probe's wrappers in stable heap storage so a
// guarded native fault can retain that storage instead of returning with an
// intrusive link into a dead stack frame.
struct alignas(16) StableScaleformWrapper {
    std::array<std::byte, 0x38> value{};
    std::array<std::byte, 0x08> alignment_padding{};
};

static_assert(sizeof(StableScaleformWrapper) == 0x40);
static_assert(alignof(StableScaleformWrapper) == 16);

struct alignas(16) Rgba8ShapeWrapperArena {
    StableScaleformWrapper parent{};
    StableScaleformWrapper existing{};
    StableScaleformWrapper created{};
    StableScaleformWrapper graphics{};
    StableScaleformWrapper verified_icon{};
    StableScaleformWrapper verified_authored{};
    StableScaleformWrapper undefined{};
};

static_assert(alignof(Rgba8ShapeWrapperArena) == 16);

struct alignas(16) RawGfxValue {
    std::array<std::byte, 0x18> prefix{};
    std::uint32_t type{};
    std::uint32_t padding{};
    std::array<std::byte, 0x10> payload{};
};

static_assert(sizeof(RawGfxValue) == 0x30);

enum class TabLayer : std::uint8_t {
    none,
    front,
    back,
};

enum class VectorIconKind : std::uint8_t {
    none,
    cross,
    ellipsis,
};

enum class TabSelectionSource : std::uint8_t {
    unknown,
    pointer,
    navigation_previous,
    navigation_next,
};

struct RepaintContext {
    bool active{};
    void* top{};
    TabLayer layer{TabLayer::none};
    std::size_t physical{};
    std::size_t logical{invalid_tab_index};
};

struct PendingAliasedIcon {
    void* resource{};
    VectorIconKind kind{VectorIconKind::none};
    RepaintContext context{};
    bool direct_rgba8{};
    bool transparent_vector_placeholder{};
    std::size_t rgba8_index{invalid_tab_index};
    std::size_t logical{invalid_tab_index};
    std::uint64_t checksum{};
    std::uint64_t presentation_epoch{};
};

struct LogicalTab {
    std::array<wchar_t, kTitleCapacity> title{};
    std::int32_t category{-1};
    std::array<std::byte, kPresentationSize> presentation{};
#if defined(ER_NATIVE_UI_PRODUCTION_TABS)
    const erui::detail::NativeTabCatalogueEntry* source{};
    erui::detail::NativeTabRuntimeEntry* custom_runtime{};
#endif
};

#if defined(ER_NATIVE_UI_PRODUCTION_TABS)
struct PinnedLogicalTabCatalogue {
    std::vector<LogicalTab> tabs{};
};

#endif
struct RecordFields {
    const wchar_t* title{};
    std::int32_t category{-1};
    std::array<std::byte, kPresentationSize> presentation{};
};

struct alignas(16) ResolvedTabProxy {
    std::array<std::byte, kSceneObjProxySize> storage{};
    bool constructed{};
    std::array<std::byte, 15> alignment_padding{};
};

static_assert(alignof(ResolvedTabProxy) == 16);
static_assert(sizeof(ResolvedTabProxy) == 0x70);

#if defined(ER_NATIVE_UI_PRODUCTION_TABS)
inline constexpr std::size_t kSlotGrowthProbeCellCount = 6U;

struct SlotGrowthMovieFingerprint final {
    void* proxy_owner{};
    void* object_interface{};
    std::uint32_t flags{};
    void* object_payload{};
    void* auxiliary{};
    void* owner_link{};
    void* movie_root{};
};

struct SlotGrowthCellProof final {
    SlotGrowthMovieFingerprint movie{};
    bool resolver_returned_destination{};
    bool existence_call_exact{};
    bool exists{};
    bool fingerprint_exact{};
    bool lineage_exact{};
    bool cleanup_exact{};
    unsigned long seh_code{};
};

struct SlotGrowthCellStorage final {
    ResolvedTabProxy proxy{};
    bool poisoned{};
};

struct SlotGrowthConstructionContext final {
    void* top{};
    void* scene_proxy{};
    SlotGrowthMovieFingerprint movie{};
    bool active{};
    bool title_menu{};
    bool early_seam_seen{};
    bool late_seam_seen{};
    bool mutation_attempted{};
    bool mutation_committed{};
    bool early_proof_exact{};
    bool late_proof_exact{};
    bool live_cells_symmetric{};
    std::size_t configured_minimum{kStockLogicalTabCount};
    std::size_t discovered_capacity{kStockLogicalTabCount};
    std::size_t active_target{kStockLogicalTabCount};
    std::size_t append_checkpoints{};
    std::uint64_t count_before{};
    std::int32_t active_before{-1};
};

#endif
struct ResolvedTabLayers {
    std::array<ResolvedTabProxy, kPhysicalTabCapacity> tab_list{};
    std::array<ResolvedTabProxy, kPhysicalTabCapacity> back_tab_list{};
};

struct alignas(16) InjectedIconDetachArena {
    ResolvedTabProxy parent{};
    StableScaleformWrapper injected{};
    StableScaleformWrapper authored{};
    StableScaleformWrapper graphics{};
    StableScaleformWrapper undefined{};
    StableScaleformWrapper verify_injected{};
    StableScaleformWrapper verify_authored{};
};

static_assert(alignof(InjectedIconDetachArena) == 16);

struct alignas(16) InPlaceRebindPreflightArena {
    ResolvedTabProxy parent{};
    StableScaleformWrapper injected{};
    StableScaleformWrapper authored{};
};

static_assert(alignof(InPlaceRebindPreflightArena) == 16);

struct alignas(16) InPlaceRebindClearArena {
    ResolvedTabProxy parent{};
    StableScaleformWrapper injected{};
    StableScaleformWrapper authored{};
    StableScaleformWrapper graphics{};
};

static_assert(alignof(InPlaceRebindClearArena) == 16);

enum class InPlaceRebindHostExpectation : std::uint8_t {
    untracked,
    vector_shape,
    rgba8_shape,
};

enum class InPlaceRgba8RebindStage : std::uint8_t {
    none,
    clearing,
    waiting_retirement,
    ready_to_repaint,
    repainting,
};

struct Rgba8ReplacementPlan {
    std::uint64_t transition_sequence{};
    std::size_t source_window{};
    std::size_t target_window{};
    TabHostMask source_injected_mask{};
    TabHostMask source_rgba_mask{};
    TabHostMask source_vector_mask{};
    TabHostMask raster_detach_mask{};
    TabHostMask vector_detach_mask{};
    TabHostMask expected_detach_mask{};
    TabHostMask retired_injected_mask{};
    TabHostMask target_injected_mask{};
    TabHostMask target_rgba_mask{};
    TabHostMask target_vector_mask{};
    TabHostMask fresh_host_mask{};
    TabHostMask preserved_host_mask{};
    TabHostMask audit_mask{};
    std::array<std::size_t, kTabHostCapacity> source_assets{};
    std::array<std::size_t, kTabHostCapacity> target_assets{};
};

struct InjectedHostLayout {
    TabHostMask injected_mask{};
    TabHostMask rgba_mask{};
    TabHostMask vector_mask{};
    std::array<std::size_t, kTabHostCapacity> assets{};
};

using PendingTransitionCommitResult = TabTransitionCommitResult;

struct PendingTransition {
    bool active{};
    bool diagnose_wrap{};
    // A retained RGBA8 catalogue is published after the first native
    // composite. If that publication changes a currently visible custom icon
    // from its vector fallback to its real raster, schedule the same
    // ownership-safe Shape handoff as a viewport remap without pretending
    // that the logical selection or record window changed.
    bool presentation_refresh{};
    void* top{};
    TabViewport candidate{};
    std::uint32_t native_physical{};
    std::uint32_t desired_physical{};
    std::size_t provisional_logical{invalid_tab_index};
    std::size_t desired_logical{invalid_tab_index};
    std::size_t previous_logical{invalid_tab_index};
    std::int32_t provisional_category{-1};
    std::int32_t desired_category{-1};
    std::uint64_t pointer_dwell_ticket{};
    std::uint64_t requested_qpc{};
};

struct PointerDwellNaturalIntent {
    bool active{};
    void* top{};
    TabPointerDwellEdge edge{TabPointerDwellEdge::none};
    std::uint32_t physical{};
    std::uint32_t x_bits{};
    std::uint32_t y_bits{};
};

struct PointerDwellTimedIntent {
    bool active{};
    void* top{};
    std::uint32_t physical{};
    std::size_t logical{invalid_tab_index};
};

struct PointerDwellAdapter {
    TabPointerDwell policy{TabPointerDwellTiming{
        kPointerDwellInitialDelayMs,
        kPointerDwellRepeatDelayMs,
        kPointerDwellDecayMs,
        kPointerDwellMinimumDelayMs,
    }};
    void* top{};
    TabDirection direction{TabDirection::next};
    std::uint64_t ticket_counter{};
    std::uint64_t transaction_ticket{};
    PointerDwellNaturalIntent natural_intent{};
    PointerDwellTimedIntent timed_intent{};
};

class ScopedBooleanTrue {
public:
    explicit ScopedBooleanTrue(bool& value) noexcept
        : value_(value), previous_(value) {
        value_ = true;
    }

    ~ScopedBooleanTrue() noexcept { value_ = previous_; }

    ScopedBooleanTrue(const ScopedBooleanTrue&) = delete;
    ScopedBooleanTrue& operator=(const ScopedBooleanTrue&) = delete;

private:
    bool& value_;
    bool previous_{};
};

enum class PanelStateIsolationReason : std::uint8_t {
    none,
    provisional,
    corrective,
};

struct PanelStateIsolationExpectation {
    bool active{};
    bool consumed{};
    void* composite{};
    void* source{};
    std::int32_t target_category{-1};
    PanelStateIsolationReason reason{PanelStateIsolationReason::none};
};

struct PanelSnapshot {
    void* panel{};
    void* vtable{};
    std::uint32_t visual_capacity{};
    std::uint64_t native_row_count{};
    bool pointer_read{};
    bool vtable_read{};
    bool option_setting{};
    bool fields_read{};
};

struct PanelCarrierSnapshot {
    void* proxy_vtable{};
    void* getter{};
    void* wrapper{};
    void* payload{};
    std::uint32_t wrapper_flags{};
    bool valid{};
};

struct RuntimeState {
    bool active{};
    void* top{};
#if !defined(ER_NATIVE_UI_PRODUCTION_TABS)
    std::array<LogicalTab, kLogicalTabCount> catalogue{};
#endif
    std::array<std::byte, kPresentationSize> overflow_presentation{};
    TabViewport viewport{};
    PendingTransition pending{};
    DeferredTabPresentationRefresh catalogue_presentation_refresh{};
    std::uint64_t presentation_epoch{};
    std::uint64_t completed_shape_replacement_transitions{};
    bool destructive_transition_failed{};
};

struct InjectedMovieFingerprint {
    void* object_interface{};
    void* owner_link{};
    void* movie_root{};
    std::uint64_t presentation_epoch{};
};

struct InPlaceRgba8RebindTransaction {
    bool active{};
    bool failed{};
    std::uint64_t transition_sequence{};
    InPlaceRgba8RebindStage stage{InPlaceRgba8RebindStage::none};
    void* top{};
    std::uint64_t presentation_epoch{};
    std::uint64_t source_generation{};
    std::uint64_t target_generation{};
    std::size_t source_window{};
    std::size_t target_window{};
    std::uint64_t clear_frame{};
    std::uint64_t requested_qpc{};
    std::uint64_t phase_a_started_qpc{};
    std::uint64_t detach_completed_qpc{};
    std::uint64_t retirement_completed_qpc{};
    std::uint64_t repaint_started_qpc{};
    std::uint64_t retirement_wait_frames{};
    TabHostMask expected_clear_mask{};
    TabHostMask completed_clear_mask{};
    TabHostMask expected_raster_clear_mask{};
    TabHostMask expected_vector_clear_mask{};
    TabHostMask source_injected_mask{};
    TabHostMask source_rgba_mask{};
    TabHostMask retired_injected_mask{};
    TabHostMask target_injected_mask{};
    TabHostMask target_rgba_mask{};
    TabHostMask audit_mask{};
    TabHostMask expected_host_mask{};
    TabHostMask completed_host_mask{};
    TabHostMask expected_rgba_presentation_mask{};
    TabHostMask completed_rgba_presentation_mask{};
    TabHostMask expected_vector_host_mask{};
    TabHostMask completed_vector_host_mask{};
    TabHostMask preserved_host_mask{};
    std::array<std::size_t, kTabHostCapacity> source_assets{};
    std::array<std::size_t, kTabHostCapacity> target_assets{};
};

struct PointerCellSampleTrace {
    bool initialized{};
    bool seen_this_dispatch{};
    std::int32_t natural_hit{-1};
    std::uint32_t natural_x_bits{};
    std::uint32_t natural_y_bits{};
    std::int32_t last_hit{-1};
    std::uint32_t last_x_bits{};
    std::uint32_t last_y_bits{};
    std::uint64_t last_position_change_ms{};
    std::uint64_t last_report_ms{};
    std::uint32_t dispatch_frames{};
    std::uint32_t successful_polls{};
    std::uint32_t natural_frames{};
    std::uint32_t fallback_frames{};
    std::uint32_t unavailable_frames{};
    std::uint32_t coordinate_changes{};
    std::uint32_t hit_changes{};
    std::uint32_t comparison_frames{};
    std::uint32_t coordinate_mismatches{};
    std::uint32_t hit_mismatches{};
    std::uint32_t poll_faults{};
    unsigned long last_poll_seh{};
};

struct NativePointerSample {
    std::int32_t hit{-1};
    std::uint32_t x_bits{};
    std::uint32_t y_bits{};
    unsigned long seh_code{};
};

struct DeferredBitmapDataProbe {
    bool scheduled{};
    void* top{};
    TabLayer layer{TabLayer::none};
    std::size_t physical{invalid_tab_index};
    std::size_t expected_logical{invalid_tab_index};
    std::uint64_t mapping_generation{};
    std::uint64_t scheduled_after_frame{};
    std::uint32_t scheduling_thread{};
};

enum class BitmapDataPropertyVerificationPhase : std::uint8_t {
    none = 0,
    pre_mutation_identity,
    post_mutation_identity,
};

const char* bitmapdata_property_verification_phase_name(
    BitmapDataPropertyVerificationPhase phase) noexcept;

struct BitmapDataPropertyVerification {
    BitmapDataPropertyVerificationPhase phase{
        BitmapDataPropertyVerificationPhase::none};
    void* top{};
    TabLayer layer{TabLayer::none};
    std::size_t physical{invalid_tab_index};
    std::size_t expected_logical{invalid_tab_index};
    std::uint64_t mapping_generation{};
    std::uint64_t created_frame{};
    std::uint64_t phase_armed_frame{};
    std::uint32_t creation_thread{};
    void* expected_payload{};
    // Passive diagnostics only. These are scalar addresses sampled from the
    // exact AS3 MovieRoot at creation; no VM-owned wrapper crosses the frame.
    std::uintptr_t vm_address{};
    std::uintptr_t drawable_image_context_address{};
    std::uintptr_t drawable_image_provider_address{};
};

struct BitmapDataVmContextSnapshot {
    std::uintptr_t vm_address{};
    std::uintptr_t drawable_image_context_address{};
    std::uintptr_t drawable_image_provider_address{};
    bool vm_read{};
    bool drawable_image_context_read{};
    bool drawable_image_provider_read{};
};

extern HMODULE g_module;
extern HANDLE g_log_file;
extern SRWLOCK g_log_lock;
extern GameImage g_game;
extern RuntimeState g_runtime;

#if defined(ER_NATIVE_UI_PRODUCTION_TABS)
extern const erui::detail::NativeTabCatalogue* g_native_tab_catalogue;
extern std::unique_ptr<erui::detail::NativeTabRuntimeState> g_native_tab_runtime;
extern std::unique_ptr<PinnedLogicalTabCatalogue> g_logical_tab_catalogue;
extern std::size_t g_navigation_margin;
extern bool g_mouse_dwell_enabled;
extern TabPointerDwellTiming g_pointer_dwell_timing;
extern std::atomic_bool g_native_tabs_installed;
extern std::size_t g_minimum_tab_slots;

#else
inline constexpr std::size_t g_navigation_margin{kNavigationMargin};
inline constexpr bool g_mouse_dwell_enabled{true};
inline constexpr TabPointerDwellTiming g_pointer_dwell_timing{
    kPointerDwellInitialDelayMs,
    kPointerDwellRepeatDelayMs,
    kPointerDwellDecayMs,
    kPointerDwellMinimumDelayMs,
};

#endif
// The first exact stock snapshot is copied into process-stable storage. A
// Scaleform movie can be recreated at the same native Configuration address
// while the live records still contain our mapped titles/descriptors; those
// records must be restored from this pristine catalogue, never reinterpreted
// as a fresh stock snapshot.
extern std::array<LogicalTab, kStockLogicalTabCount> g_preserved_stock_catalogue;
extern bool g_preserved_stock_catalogue_ready;

extern SafetyHookInline g_selection_hook;
extern SafetyHookInline g_composite_frame_hook;
extern SafetyHookInline g_texture_lookup_hook;
extern SafetyHookInline g_image_bind_hook;
extern SafetyHookMid g_panel_state_copy_hook;
extern SafetyHookMid g_panel_alias_visibility_hook;
extern SafetyHookMid g_input_dispatch_frame_begin_hook;
extern SafetyHookMid g_pointer_dispatch_source_hook;
extern SafetyHookMid g_horizontal_navigation_source_hook;
extern SafetyHookMid g_selection_dispatch_hook;
extern SafetyHookMid g_pointer_cell_sample_hook;

#if defined(ER_NATIVE_UI_PRODUCTION_TABS)
extern SafetyHookMid g_slot_growth_early_hook;
extern SafetyHookMid g_slot_growth_late_hook;
extern SafetyHookMid g_slot_growth_post_constructor_hook;
extern std::atomic_bool g_slot_growth_callbacks_armed;
extern CategoryAppendFn g_category_append;
extern thread_local bool g_inside_slot_growth_append;
extern thread_local SlotGrowthConstructionContext g_slot_growth_construction;
extern thread_local std::array<
    SlotGrowthCellStorage,
    kSlotGrowthProbeCellCount> g_slot_growth_cell_storage;

#endif
// Hook creation is transactional only until the first target is exposed. Once
// enable() has been attempted, hook objects and all borrowed runtime storage
// remain process-lifetime. Every callback stays inert until the complete set
// is armed, so a partially enabled set is an exact native pass-through.
extern std::atomic_bool g_viewport_hooks_exposed;
extern std::atomic_bool g_viewport_callbacks_armed;
extern std::atomic<SelectionCallbackFn> g_original_selection;
extern std::atomic<CompositeFrameFn> g_original_composite_frame;
extern std::atomic<TextureLookupFn> g_original_texture_lookup;
extern std::atomic<ImageBindFn> g_original_image_bind;

extern SelectorSelectFn g_selector_select;
extern GetPointerPositionFn g_get_pointer_position;
extern GridHitTestFn g_grid_hit_test;
extern NativePointerSelectFn g_native_pointer_select;
extern SwitchPanelFn g_switch_panel;
extern RefreshTitleFn g_refresh_title;
extern ResolvePathFn g_resolve_path;
extern ValueExistsFn g_value_exists;
extern PresentTabFn g_present_tab;
extern MakeIconDescriptorFn g_make_icon_descriptor;
extern ScaleformConstructorFn g_construct_scaleform_value;
extern ScaleformGetMemberFn g_get_scaleform_member;
extern ScaleformSetPositionFn g_set_scaleform_position;
extern ScaleformSetVisibleFn g_set_scaleform_visible;
extern ScaleformDestructorFn g_destroy_scaleform_value;

extern std::atomic_uint32_t g_transition_log_count;
extern std::atomic_uint32_t g_serialized_selection_log_count;
// r148: the per-site table and total cap live in failure_log_budget.hpp,
// where they are unit-tested.
extern erui::detail::FailureLogBudget<kFailureLogSites> g_failure_log_budget;
extern std::atomic_uint32_t g_record_mismatch_log_count;
extern std::atomic_uint32_t g_wrap_diagnostic_log_count;
extern std::atomic_uint32_t g_alias_log_count;
extern std::atomic_uint32_t g_image_bind_log_count;
extern std::atomic_uint32_t g_native_image_resource_log_count;
extern std::atomic_uint32_t g_shape_attach_attempt_count;
extern std::atomic_uint32_t g_state_isolation_log_count;
extern std::atomic_uint32_t g_alias_visibility_log_count;
extern std::atomic_uint32_t g_carrier_normalization_log_count;
extern std::atomic_uint32_t g_carrier_selection_log_count;
extern std::atomic_uint32_t g_pointer_sample_log_count;
extern std::atomic_uint32_t g_pointer_dwell_log_count;
extern std::atomic_uint32_t g_rgba8_lookup_log_count;
extern std::atomic_uint32_t g_rgba8_bind_log_count;
extern std::atomic_uint32_t g_rgba8_lifecycle_log_count;
extern std::atomic_bool g_value_invoke_probe_attempted;
extern std::atomic_bool g_value_invoke_probe_validated;
extern std::atomic_bool g_bitmapdata_constructor_probe_attempted;
extern std::atomic_bool g_bitmapdata_pixel_mutation_probe_attempted;
extern std::atomic_bool g_raw_image_lifecycle_probe_attempted;
extern std::atomic_bool g_rgba8_publication_disabled;
// Readers may run from native presentation callbacks. Individual pointer
// stores are never sufficient evidence that the complete registered
// catalogue exists; this release/acquire flag is the single publication
// point after every image/resource/checksum identity has been installed.
extern std::atomic_bool g_rgba8_catalogue_ready;

#if !defined(ER_NATIVE_UI_PRODUCTION_TABS)
// Each immutable, movie-independent native image resource is intentionally
// retained until process exit. The DLL is pinned, and no game code is called
// from DllMain to release it under the loader lock.
extern std::array<std::atomic<void*>, kCustomTabCount> g_retained_rgba8_images;
extern std::array<std::atomic<void*>, kCustomTabCount> g_retained_rgba8_resources;
extern std::array<std::atomic_uint64_t, kCustomTabCount>
    g_retained_rgba8_checksums;
// Readiness is explicit: every 4096-byte RGBA8 payload is valid, including a
// payload whose diagnostic FNV checksum happens to be zero.
extern std::array<std::atomic_bool, kCustomTabCount> g_retained_rgba8_ready;

#endif
extern std::atomic_uint64_t g_presentation_epoch_counter;
extern std::atomic_uint64_t g_rgba8_successful_attach_count;
extern std::atomic_uint64_t g_rgba8_successful_detach_count;
// The attachment ledger outlives RuntimeState. Resetting an invalidated or
// failed viewport must never erase ownership that still belongs to a live
// Scaleform movie; only an exact detach/absence proof clears a bit.
extern AtomicTabHostMask g_injected_icon_hosts;
extern AtomicTabHostMask g_rgba8_icon_hosts;

#if !defined(ER_NATIVE_UI_PRODUCTION_TABS)
// Zero means no retained RGBA Shape; otherwise value-1 is the immutable asset
// index currently bound to that front/back physical host.
extern std::array<std::atomic_size_t, kTabHostCapacity>
    g_rgba8_host_assets;

#endif
// A changed target is first attached in the host's inactive property bank.
// These ledgers describe that short, same-callback overlap.  They are never
// published as the active mapping until the old bank has been removed exactly.
extern AtomicTabHostMask g_staged_injected_icon_hosts;
extern AtomicTabHostMask g_staged_rgba8_icon_hosts;

#if !defined(ER_NATIVE_UI_PRODUCTION_TABS)
extern std::array<std::atomic_size_t, kTabHostCapacity>
    g_staged_rgba8_host_assets;

#endif
// One bit per front/back host; set means bank B is the active property bank.
extern AtomicTabHostMask g_icon_bank_b_hosts;
// A native Shape can release its previous RawImage on a later renderer/trace
// boundary even though the in-place binder already returned successfully.
// Keep those old, no-longer-active references separate from the physical-host
// map. The debt is bounded by the total number of tab-strip hosts and may only
// reconcile downward after an observed RawImage refcount drop.
inline constexpr std::uint32_t kMaximumRgba8RetirementDebt =
    static_cast<std::uint32_t>(kTabHostCapacity);
inline constexpr Rgba8RetirementLimits kRgba8RetirementLimits{
    // Active and inactive property banks may coexist during one atomic
    // handoff, so the policy bounds both complete host sets.
    static_cast<std::uint32_t>(kTabHostCapacity * 2U),
    kMaximumRgba8RetirementDebt,
};

#if !defined(ER_NATIVE_UI_PRODUCTION_TABS)
extern std::array<std::atomic_uint32_t, kCustomTabCount>
    g_rgba8_retirement_debt;

#endif
extern InjectedMovieFingerprint g_injected_movie_fingerprint;
// A managed GFx::Value is linked into its ObjectInterface while alive. Every
// wrapper used by this fault-isolation gate therefore lives in process-heap
// storage. A fully successful operation destroys and frees it; an uncertain
// native failure retains at most these three blocks for the process lifetime
// rather than leaving an intrusive-list link in dead stack storage.
extern void* g_retained_bitmapdata_wrapper;
extern void* g_retained_bitmapdata_same_frame_wrapper;
extern void* g_retained_bitmapdata_next_frame_wrapper;
extern thread_local bool g_mutating_viewport;
extern thread_local TabSelectionSource g_tab_selection_source;
extern thread_local PointerCellSampleTrace g_pointer_cell_sample_trace;
extern thread_local PointerDwellAdapter g_pointer_dwell;
extern thread_local bool g_blocking_composite_input;
extern thread_local PanelStateIsolationExpectation g_state_isolation_expectation;
extern thread_local bool g_resolving_icon_alias;
extern thread_local RepaintContext g_repaint_context;
// True only while the desired cells of a new Configuration epoch are being
// presented. Rollback repainting deliberately runs outside this scope so a
// failed initial publication always restores the visible vector fallback.
extern thread_local bool g_initial_target_repaint;
// The alias lookup and image binding are adjacent calls in the validated
// presenter. Mark the exact resource, icon kind, and repaint cell, then
// consume the marker at the next image-bind call on the same UI thread.
extern thread_local PendingAliasedIcon g_pending_aliased_icon;
extern thread_local InPlaceRgba8RebindTransaction
    g_in_place_rgba8_rebind_transaction;
extern thread_local DeferredBitmapDataProbe g_deferred_bitmapdata_probe;
extern thread_local BitmapDataPropertyVerification
    g_bitmapdata_property_verification;
extern thread_local std::uint64_t g_composite_frame_sequence;

inline constexpr std::size_t kNoRgba8Asset = invalid_tab_index;

[[nodiscard]] LogicalTab* runtime_logical_tab(std::size_t logical) noexcept;

#if defined(ER_NATIVE_UI_PRODUCTION_TABS)
[[nodiscard]] erui::detail::NativeTabRuntimeEntry* runtime_custom_tab(
    std::size_t custom) noexcept;

#endif
[[nodiscard]] std::int32_t rgba8_icon_id(std::size_t custom) noexcept;

void publish_retained_rgba8(
    std::size_t custom,
    const RetainedRgba8Resource& retained) noexcept;

[[nodiscard]] std::uint32_t rgba8_retirement_debt(
    std::size_t custom) noexcept;

[[nodiscard]] bool set_rgba8_retirement_debt(
    std::size_t custom,
    std::uint32_t debt) noexcept;

[[nodiscard]] std::size_t active_host_asset(std::size_t host) noexcept;

[[nodiscard]] std::size_t staged_host_asset(std::size_t host) noexcept;

[[nodiscard]] bool set_active_host_asset(
    std::size_t host,
    std::size_t custom) noexcept;

[[nodiscard]] bool set_staged_host_asset(
    std::size_t host,
    std::size_t custom) noexcept;

#if defined(ER_NATIVE_UI_PRODUCTION_TABS)
// Compatibility views keep the proven hook/lifecycle code readable while the
// actual ownership is consolidated in NativeTabRuntimeState. These objects do
// not contain per-tab storage: every access resolves tabs[index] and therefore
// remains exact-sized and stable for the process lifetime.
template <auto Member>
class RuntimeIconAtomicView final {
public:
    [[nodiscard]] std::size_t size() const noexcept {
        return active_custom_tab_count();
    }

    [[nodiscard]] decltype(auto) operator[](
        std::size_t custom) const noexcept {
        return (runtime_custom_tab(custom)->icon().*Member);
    }
};

class RuntimeRetirementDebtReference final {
public:
    explicit RuntimeRetirementDebtReference(std::size_t custom) noexcept
        : custom_(custom) {}

    [[nodiscard]] std::uint32_t load(
        std::memory_order = std::memory_order_seq_cst) const noexcept {
        return rgba8_retirement_debt(custom_);
    }

    void store(
        std::uint32_t debt,
        std::memory_order = std::memory_order_seq_cst) const noexcept {
        (void)set_rgba8_retirement_debt(custom_, debt);
    }

private:
    std::size_t custom_{};
};

class RuntimeRetirementDebtView final {
public:
    [[nodiscard]] std::size_t size() const noexcept {
        return active_custom_tab_count();
    }

    [[nodiscard]] RuntimeRetirementDebtReference operator[](
        std::size_t custom) const noexcept {
        return RuntimeRetirementDebtReference(custom);
    }
};

class EncodedHostAssetReference final {
public:
    EncodedHostAssetReference(
        std::size_t host,
        bool staged) noexcept
        : host_(host), staged_(staged) {}

    // Legacy transition code uses zero as empty and index+1 as occupied. The
    // pinned runtime itself stores the real size_t tab index; this boundary is
    // only an adapter and is intentionally not an 8-bit value.
    [[nodiscard]] std::size_t load(
        std::memory_order = std::memory_order_seq_cst) const noexcept {
        const std::size_t custom = staged_
            ? staged_host_asset(host_)
            : active_host_asset(host_);
        return custom == kNoRgba8Asset ? 0U : custom + 1U;
    }

    void store(
        std::size_t encoded,
        std::memory_order = std::memory_order_seq_cst) const noexcept {
        const std::size_t custom = encoded == 0U
            ? kNoRgba8Asset
            : encoded - 1U;
        if (staged_) {
            (void)set_staged_host_asset(host_, custom);
        } else {
            (void)set_active_host_asset(host_, custom);
        }
    }

private:
    std::size_t host_{};
    bool staged_{};
};

class EncodedHostAssetView final {
public:
    explicit EncodedHostAssetView(bool staged) noexcept : staged_(staged) {}

    [[nodiscard]] std::size_t size() const noexcept {
        return active_tab_host_count();
    }

    [[nodiscard]] EncodedHostAssetReference operator[](
        std::size_t host) const noexcept {
        return EncodedHostAssetReference(host, staged_);
    }

private:
    bool staged_{};
};

class RuntimeIconIdView final {
public:
    [[nodiscard]] std::size_t size() const noexcept {
        return active_custom_tab_count();
    }

    [[nodiscard]] std::int32_t operator[](
        std::size_t custom) const noexcept {
        return rgba8_icon_id(custom);
    }
};

class RuntimeIconResourceNameView final {
public:
    [[nodiscard]] std::size_t size() const noexcept {
        return active_custom_tab_count();
    }

    [[nodiscard]] const std::array<wchar_t, 32>& operator[](
        std::size_t custom) const noexcept {
        return runtime_custom_tab(custom)->icon().resource_name;
    }
};

class RuntimeRawResourceNameView final {
public:
    [[nodiscard]] std::size_t size() const noexcept {
        return active_custom_tab_count();
    }

    [[nodiscard]] const std::array<wchar_t, 48>& operator[](
        std::size_t custom) const noexcept {
        return runtime_custom_tab(custom)->icon().raw_resource_name;
    }
};

extern RuntimeIconAtomicView<&erui::detail::NativeTabIconRuntimeState::image>
    g_retained_rgba8_images;
extern RuntimeIconAtomicView<&erui::detail::NativeTabIconRuntimeState::resource>
    g_retained_rgba8_resources;
extern RuntimeIconAtomicView<&erui::detail::NativeTabIconRuntimeState::checksum>
    g_retained_rgba8_checksums;
extern RuntimeIconAtomicView<&erui::detail::NativeTabIconRuntimeState::ready>
    g_retained_rgba8_ready;
extern RuntimeRetirementDebtView g_rgba8_retirement_debt;
extern EncodedHostAssetView g_rgba8_host_assets;
extern EncodedHostAssetView g_staged_rgba8_host_assets;
extern RuntimeIconIdView kRgba8IconIds;
extern RuntimeIconResourceNameView kRgba8IconResourceNames;
extern RuntimeRawResourceNameView kRawImageResourceProbeNames;

#endif
[[nodiscard]] bool all_rgba8_retirement_debts_zero() noexcept;

void clear_all_rgba8_retirement_debts() noexcept;

[[nodiscard]] bool catalogue_icon_has_retained_raster(
    std::size_t icon) noexcept;

[[nodiscard]] bool catalogue_icon_has_effective_raster(
    std::size_t icon) noexcept;

std::uint8_t active_icon_property_bank(std::size_t bit) noexcept;

bool repaint_stages_fresh_host(std::size_t bit) noexcept;

std::uint8_t repaint_icon_property_bank() noexcept;

void reset_pointer_dwell(
    const char* reason = nullptr) noexcept;
void run_deferred_bitmapdata_constructor_probe(
    void* top,
    std::uint64_t frame_sequence) noexcept;
void run_bitmapdata_property_survival_verification(
    void* top,
    std::uint64_t frame_sequence) noexcept;
void run_raw_image_lifecycle_probe(void* top) noexcept;
bool validate_top(void* top) noexcept;
bool guarded_release_scaleform_image_resource(
    ScaleformImageResourceReleaseFn release,
    void* resource,
    unsigned long& seh_code) noexcept;

std::uint64_t query_performance_ticks() noexcept;

std::uint64_t elapsed_qpc_microseconds(
    std::uint64_t begin,
    std::uint64_t end) noexcept;

using erui::detail::guarded_copy;

using erui::detail::guarded_read;

void log_line(const char* format, ...) noexcept;

struct GamePointerLocation {
    bool in_image{};
    std::uintptr_t rva{};
};

GamePointerLocation locate_game_pointer(const void* pointer) noexcept;

void log_native_image_resource_snapshot(
    void* resource,
    void* image) noexcept;

bool retain_stress_log_format(const char* format) noexcept;

void log_failure(const char* format, ...) noexcept;

template <std::size_t Size>
bool exact_resource_name(
    const wchar_t* candidate,
    const wchar_t (&expected)[Size]) noexcept {
    if (!candidate) return false;
#if defined(_MSC_VER)
    __try {
#endif
        // Size includes the terminator, so a matching prefix with trailing
        // characters is intentionally rejected.
        for (std::size_t index = 0; index < Size; ++index) {
            if (candidate[index] != expected[index]) return false;
        }
        return true;
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#endif
}

bool exact_resource_name(
    const wchar_t* candidate,
    const wchar_t* expected) noexcept;

bool parse_rgba8_icon_resource_name(
    const wchar_t* candidate,
    std::size_t& icon_index) noexcept;

struct DisplayValueView {
    std::uint32_t flags{};
    void* object_interface{};
    void* object_interface_vtable{};
    void* get_member_slot{};
    void* set_member_slot{};
    void* invoke_slot{};
    void* object_payload{};
    void* native_display_object{};
    void* native_display_vtable{};
};

struct ObjectValueView {
    std::uint32_t flags{};
    void* object_interface{};
    void* object_interface_vtable{};
    void* get_member_slot{};
    void* set_member_slot{};
    void* invoke_slot{};
    void* object_payload{};
};

RawGfxValue make_bool_value(bool value) noexcept;

RawGfxValue make_int_value(std::int32_t value) noexcept;

RawGfxValue make_uint_value(std::uint32_t value) noexcept;

bool inspect_object_value(
    const void* scaleform_wrapper,
    ObjectValueView& view) noexcept;

bool exact_as3_object_bridge(const ObjectValueView& view) noexcept;

bool invoke_object_method(
    const ObjectValueView& object,
    const char* method,
    const RawGfxValue* arguments,
    std::size_t argument_count,
    unsigned long& seh_code) noexcept;

bool draw_graphics_rectangle(
    const ObjectValueView& graphics,
    std::uint32_t color,
    double alpha,
    double x,
    double y,
    double width,
    double height,
    unsigned long& seh_code) noexcept;

bool inspect_display_value(
    const void* scaleform_wrapper,
    DisplayValueView& view) noexcept;

bool exact_as3_display_bridge(const DisplayValueView& view) noexcept;

bool validated_authored_icon_target_vtable(void* vtable) noexcept;

bool destroy_local_scaleform_value(
    std::array<std::byte, 0x38>& wrapper,
    bool& constructed,
    const char* label) noexcept;

std::uint32_t count_tracked_icon_hosts(TabHostMask bits) noexcept;

std::uint32_t count_active_rgba8_asset_hosts(
    std::size_t asset_index,
    std::size_t override_host = invalid_tab_index,
    std::size_t override_asset = 0) noexcept;

struct Rgba8ReferenceSnapshot {
    bool resource_read{};
    std::uint32_t resource{};
    bool raw_read{};
    std::uint32_t raw{};
};

Rgba8ReferenceSnapshot snapshot_rgba8_references(
    std::size_t asset_index) noexcept;

bool reconcile_rgba8_retirement_debt(
    const char* phase) noexcept;

bool rgba8_references_match_tracked_hosts(
    const char* phase,
    std::size_t override_host = invalid_tab_index,
    std::size_t override_asset = 0,
    bool override_staged = false) noexcept;

void log_rgba8_lifecycle_snapshot(
    const char* phase,
    void* top) noexcept;

void** __fastcall texture_lookup_detour(
    void* repository,
    void** out_resource,
    const wchar_t* resource_name) noexcept;

void schedule_deferred_bitmapdata_constructor_probe(
    void* top,
    TabLayer layer,
    std::size_t physical) noexcept;

bool __fastcall image_bind_detour(
    void* scaleform_value,
    void** resource,
    const float* rectangle) noexcept;

bool image_contains(std::uintptr_t rva, std::size_t size) noexcept;

template <std::size_t Size>
bool entry_matches(
    std::uintptr_t rva,
    const std::array<std::uint8_t, Size>& expected) noexcept {
    if (!image_contains(rva, Size)) return false;
    std::array<std::uint8_t, Size> actual{};
    return guarded_copy(actual.data(), g_game.base + rva, Size) &&
        actual == expected;
}

bool resolve_rip_relative_data_target(
    std::uintptr_t instruction_rva,
    std::uintptr_t& target_rva) noexcept;

bool copy_title(
    const wchar_t* source,
    std::array<wchar_t, kTitleCapacity>& destination) noexcept;

std::byte* record_at(void* top, std::size_t physical) noexcept;

struct LiveTabGeometry final {
    std::size_t active{};
    std::size_t authored{};
    std::size_t selected{};
};

bool read_live_tab_geometry(
    void* top,
    LiveTabGeometry& geometry) noexcept;

bool records_match_mapping(void* top, const TabViewport& viewport) noexcept;

bool destroy_proxy(ResolvedTabProxy& proxy) noexcept;

#if defined(ER_NATIVE_UI_PRODUCTION_TABS)
void append_graphics_records_at_construction_seam(
    safetyhook::Context& cpu) noexcept;

void observe_slot_growth_post_geometry(safetyhook::Context& cpu) noexcept;

void observe_slot_growth_post_constructor(
    safetyhook::Context& cpu) noexcept;

#endif
bool destroy_tab_layers(ResolvedTabLayers& layers) noexcept;

bool resolve_tab_layers(
    void* top,
    ResolvedTabLayers& layers) noexcept;

bool resolve_injected_icon_parent(
    void* top,
    TabLayer layer,
    std::size_t physical,
    ResolvedTabProxy& proxy) noexcept;

bool preflight_in_place_rgba8_rebind_host(
    void* top,
    TabLayer layer,
    std::size_t physical,
    InPlaceRebindHostExpectation expectation,
    std::size_t expected_asset = 0,
    std::uint8_t property_bank = 0xFFU,
    bool enforce_ledger = true) noexcept;

enum class InjectedIconsClearResult : std::uint8_t {
    failed,
    waiting_for_renderer_retirement,
    cleared,
};

bool detach_rgba8_host_for_replacement(
    void* top,
    TabLayer layer,
    std::size_t physical,
    InPlaceRgba8RebindTransaction& transaction,
    bool restore_authored = true) noexcept;

bool detach_vector_host_for_replacement(
    void* top,
    TabLayer layer,
    std::size_t physical,
    InPlaceRgba8RebindTransaction& transaction,
    bool restore_authored = true) noexcept;

InjectedIconsClearResult clear_injected_icons(
    void* top,
    bool allow_already_absent = false) noexcept;

[[nodiscard]] bool accept_vector_only_epoch_clear_result(
    void* top,
    InjectedIconsClearResult clear_result,
    const char* phase) noexcept;

bool sample_current_movie_fingerprint(
    void* top,
    InjectedMovieFingerprint& fingerprint) noexcept;

bool same_movie_fingerprint(
    const InjectedMovieFingerprint& left,
    const InjectedMovieFingerprint& right) noexcept;

bool invoke_selector_select(
    void* top,
    std::uint32_t physical) noexcept;

bool begin_panel_state_isolation(
    void* top,
    std::int32_t target_category,
    PanelStateIsolationReason reason) noexcept;
void end_panel_state_isolation(bool armed) noexcept;
PanelSnapshot inspect_panel_slot(const void* slot) noexcept;
PanelCarrierSnapshot inspect_panel_carrier(const PanelSnapshot& panel) noexcept;
[[nodiscard]] std::size_t native_graphics_route() noexcept;

bool logical_uses_graphics_carrier(
    std::size_t logical) noexcept;

bool is_custom_logical(std::size_t logical) noexcept;

std::size_t graphics_route_for_logical(
    std::size_t logical) noexcept;

void __fastcall research_switch_panel_detour(
    void* composite,
    std::int32_t category) noexcept;

void invoke_switch_panel(
    void* top,
    std::int32_t category,
    PanelStateIsolationReason isolation_reason =
        PanelStateIsolationReason::none) noexcept;

void invoke_refresh_title(void* top) noexcept;

[[nodiscard]] bool derive_injected_host_layout(
    const TabViewport& viewport,
    InjectedHostLayout& output) noexcept;

[[nodiscard]] bool snapshot_injected_host_layout(
    InjectedHostLayout& output) noexcept;

[[nodiscard]] bool same_injected_host_layout(
    const InjectedHostLayout& left,
    const InjectedHostLayout& right) noexcept;

[[nodiscard]] bool exact_initial_catalogue_upgrade(
    const InjectedHostLayout& source,
    const InjectedHostLayout& target) noexcept;

bool apply_mapping(
    void* top,
    const TabViewport& viewport,
    ResolvedTabLayers& layers,
    bool allow_transparent_initial_raster_placeholder = false) noexcept;

bool read_selected_physical(void* top, std::uint32_t& selected) noexcept;

void log_research_carrier_selection(
    void* top,
    const char* stage,
    std::size_t logical,
    std::int32_t category) noexcept;

void isolate_panel_state_copy(safetyhook::Context& context) noexcept;

void log_wrap_panel_state(
    void* top,
    const char* stage,
    std::size_t previous_logical,
    std::size_t desired_logical) noexcept;

bool freeze_active_two_phase_rgba8_transaction(
    const char* reason) noexcept;

void reset_runtime() noexcept;

bool acknowledge_exact_carrier_teardown() noexcept;

bool ensure_runtime(void* top) noexcept;

void log_pointer_dwell(
    const char* event,
    const char* detail = "") noexcept;

std::uint64_t next_pointer_dwell_ticket() noexcept;

TabPointerDwellEdge overflow_edge_for_physical(
    const TabViewport& viewport,
    std::size_t physical) noexcept;

TabDirection direction_for_pointer_edge(
    TabPointerDwellEdge edge) noexcept;

void finish_pointer_dwell_transition(
    std::uint64_t ticket,
    const char* detail) noexcept;

void fail_pointer_dwell_transition(
    std::uint64_t ticket,
    const char* reason) noexcept;

void observe_pointer_cell_sample(
    safetyhook::Context& context) noexcept;

void observe_input_dispatch_frame_begin(
    safetyhook::Context& context) noexcept;

void observe_pointer_dispatch_result(
    safetyhook::Context& context) noexcept;

void observe_horizontal_navigation_commit(
    safetyhook::Context& context) noexcept;

void dispatch_due_pointer_dwell(
    safetyhook::Context& context) noexcept;

void __fastcall selection_callback_detour(
    void* callable,
    const std::int32_t* physical_index) noexcept;

void progress_two_phase_rgba8_retirement(
    void* top,
    std::uint64_t frame_sequence) noexcept;

void __fastcall composite_frame_detour(
    void* composite,
    float frame_value,
    const std::uint8_t* input_enabled) noexcept;

} // namespace native_tabs_detail
