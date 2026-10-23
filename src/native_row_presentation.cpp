#include "native_row_presentation.hpp"

#include "addresses.hpp"
#include "game_build.hpp"
#include "module.hpp"
#include "native_title_bridge.hpp"
#include "row_presentation_state.hpp"
#include "runtime_log.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>

namespace erui::native {
namespace {

constexpr std::size_t kPropertyCollectionOffset = 0x1268;
constexpr std::size_t kPropertyRowStride = 0x88;
constexpr std::size_t kPropertyRowHelpDirectOffset = 0x40;
constexpr std::size_t kPropertyRowOwnedHelpOffset = 0x48;
constexpr std::size_t kPropertyControllerOffset = 0x78;
constexpr std::size_t kPropertyCountOffset = 0x1AF0;
constexpr std::size_t kMaximumPropertyRows = 16;
constexpr std::size_t kControllerMovieContextOffset = 0x10;
constexpr std::size_t kControllerCaptionAccessorSlot = 6;
constexpr std::size_t kSceneProxyFirstSelfOffset = 0x08;
constexpr std::size_t kSceneProxySecondSelfOffset = 0x10;
constexpr std::size_t kSceneProxyThirdSelfOffset = 0x18;
constexpr std::size_t kSceneProxyOwnerOffset = 0x20;
constexpr std::size_t kSceneProxyValueOffset = 0x28;
constexpr std::size_t kScaleformValueFlagsOffset = 0x20;
constexpr std::uint32_t kScaleformValueKindMask = 0x8F;
constexpr std::uint32_t kScaleformValueKindDisplayObject = 0x0A;
constexpr std::size_t kMaximumTrackedPages = 64;
constexpr std::uint32_t kFaultLogLimit = 16;
constexpr char kOrdinaryRowLabelPath[] = "Caption/Text_0";
constexpr char kFamilyARowLabelPath[] = "Text_1/Text";
constexpr char kFamilyAControlCaptionPath[] = "Text_0/Text";
constexpr char kFamilyBRowLabelPath[] = "Text_0";
constexpr char kFamilyBControlCaptionPath[] = "Text_1/Text";

static_assert(
    kSceneProxyValueOffset + kScaleformValueFlagsOffset +
            sizeof(std::uint32_t) <=
        scaleform_path_result_size);
static_assert(
    kSceneProxyValueOffset + 0x28 + sizeof(void*) <=
        scaleform_path_result_size);

enum class LabelTargetKind : std::uint8_t {
    none,
    ordinary_row,
    widget_unresolved,
    family_a_row,
    family_b_row,
};

struct PhysicalBinding {
    std::size_t presentation_slot{detail::invalid_compiled_index};
    std::size_t physical_index{};
    void* native_row{};
    void* row_vtable{};
    void* controller{};
    void* controller_vtable{};
    void* caption_accessor{};
    void* row_movie_proxy{};
    void* row_movie_proxy_vtable{};
    void* row_movie_owner{};
    LabelTargetKind label_target{LabelTargetKind::none};
    RowKind row_kind{RowKind::toggle};
    const wchar_t* original_help{};
    const wchar_t* installed_help{};
    std::uint64_t label_revision{};
    std::uint64_t help_revision{};
    std::uint64_t caption_revision{};
    std::uint64_t label_failed_revision{};
    std::uint64_t help_failed_revision{};
    std::uint64_t caption_failed_revision{};
    std::uint8_t label_failures{};
    std::uint8_t help_failures{};
    std::uint8_t caption_failures{};
    bool help_conflict{};
};

struct PageBinding {
    void* page{};
    void* page_vtable{};
    std::uint64_t epoch{};
    std::uint64_t use_serial{};
    std::array<PhysicalBinding, kMaximumPropertyRows> rows{};
    std::size_t row_count{};
    bool active{};
};

std::mutex g_mutex{};
std::array<PageBinding, kMaximumTrackedPages> g_pages{};
std::uint64_t g_epoch{};
std::uint64_t g_use_serial{};
std::atomic<std::uint32_t> g_fault_logs{};

void log_fault(
    const char* operation,
    void* page,
    std::size_t physical_index,
    unsigned long seh_code = 0) noexcept {
    const std::uint32_t fault =
        g_fault_logs.fetch_add(1, std::memory_order_relaxed) + 1u;
    if (fault <= kFaultLogLimit) {
        erui::detail::logf(
            erui::LogLevel::warning,
            "Runtime row presentation %s failed (fault=%u page=%p physical=%zu seh=0x%08lX)",
            operation,
            static_cast<unsigned>(fault),
            page,
            physical_index,
            seh_code);
    } else if (fault == kFaultLogLimit + 1u) {
        erui::detail::logf(
            erui::LogLevel::warning,
            "Runtime row presentation suppressed further fault logs");
    }
}

PageBinding* find_page(void* page) noexcept {
    const auto found = std::find_if(
        g_pages.begin(), g_pages.end(),
        [page](const PageBinding& candidate) {
            return candidate.active && candidate.page == page;
        });
    return found == g_pages.end() ? nullptr : &*found;
}

bool read_page_identity(
    void* page,
    const GameAddresses& addresses,
    void*& out_vtable,
    std::size_t& out_count,
    unsigned long& out_seh) noexcept {
    out_vtable = nullptr;
    out_count = 0;
    out_seh = 0;
    if (!page || !addresses.option_setting_dialog_vtable) return false;
#if defined(_MSC_VER)
    __try {
#endif
        std::memcpy(&out_vtable, page, sizeof(out_vtable));
        std::memcpy(
            &out_count,
            static_cast<const std::byte*>(page) + kPropertyCountOffset,
            sizeof(out_count));
        return out_vtable == addresses.option_setting_dialog_vtable &&
            out_count <= kMaximumPropertyRows;
#if defined(_MSC_VER)
    } __except (out_seh = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#endif
}

WidgetTextLayout widget_text_layout(LabelTargetKind target) noexcept {
    switch (target) {
    case LabelTargetKind::family_a_row:
        return WidgetTextLayout::family_a;
    case LabelTargetKind::family_b_row:
        return WidgetTextLayout::family_b;
    default:
        return WidgetTextLayout::none;
    }
}

const char* widget_text_target_path(WidgetTextTarget target) noexcept {
    switch (target) {
    case WidgetTextTarget::direct_text_0:
        return kFamilyBRowLabelPath;
    case WidgetTextTarget::nested_text_0:
        return kFamilyAControlCaptionPath;
    case WidgetTextTarget::nested_text_1:
        return kFamilyARowLabelPath;
    default:
        return nullptr;
    }
}

const char* label_target_path(LabelTargetKind target) noexcept {
    if (target == LabelTargetKind::ordinary_row) {
        return kOrdinaryRowLabelPath;
    }
    return widget_text_target_path(select_widget_text_target(
        widget_text_layout(target), WidgetTextRole::row_label));
}

const char* caption_target_path(LabelTargetKind target) noexcept {
    return widget_text_target_path(select_widget_text_target(
        widget_text_layout(target), WidgetTextRole::control_caption));
}

constexpr bool uses_widget_text_pair(RowKind kind) noexcept {
    return kind == RowKind::button || kind == RowKind::popup_choice ||
        kind == RowKind::color_picker || kind == RowKind::submenu;
}

constexpr bool uses_action_controller(RowKind kind) noexcept {
    return kind == RowKind::button || kind == RowKind::popup_choice ||
        kind == RowKind::color_picker || kind == RowKind::submenu;
}

constexpr bool supports_control_caption(RowKind kind) noexcept {
    return kind == RowKind::button || kind == RowKind::popup_choice ||
        kind == RowKind::submenu;
}

static_assert(uses_action_controller(RowKind::popup_choice));
static_assert(!uses_action_controller(RowKind::inline_choice));
static_assert(supports_control_caption(RowKind::button));
static_assert(supports_control_caption(RowKind::popup_choice));
static_assert(supports_control_caption(RowKind::submenu));
static_assert(!supports_control_caption(RowKind::color_picker));

bool destroy_label_result(
    const GameAddresses& addresses,
    std::byte* result,
    unsigned long& out_seh) noexcept {
    out_seh = 0;
#if defined(_MSC_VER)
    __try {
#endif
        addresses.scaleform_result_destructor(
            result + scaleform_destructor_member_offset);
        return true;
#if defined(_MSC_VER)
    } __except (out_seh = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#endif
}

bool resolve_validated_label_path(
    const GameAddresses& addresses,
    void* movie_context,
    const char* path,
    std::byte* result,
    unsigned long& out_seh) noexcept {
    out_seh = 0;
    if (!movie_context || !path || !*path || !result ||
        !addresses.title_bridge_complete()) {
        return false;
    }

    // A resolver return equal to destination transfers one constructed native
    // result to this function. A valid return remains owned by the caller;
    // every invalid constructed result is destroyed here exactly once.
    bool constructed = false;
    bool display_object = false;
#if defined(_MSC_VER)
    __try {
#endif
        constructed = addresses.scaleform_path_resolver(
            movie_context, result, path) == result;
        if (constructed) {
            void* source_vtable{};
            void* source_owner{};
            void* result_vtable{};
            void* result_self0{};
            void* result_self1{};
            void* result_self2{};
            void* result_owner{};
            std::uint32_t value_flags{};
            void* value_payload{};
            std::memcpy(
                &source_vtable,
                movie_context,
                sizeof(source_vtable));
            std::memcpy(
                &source_owner,
                static_cast<const std::byte*>(movie_context) +
                    kSceneProxyOwnerOffset,
                sizeof(source_owner));
            std::memcpy(
                &result_vtable,
                result,
                sizeof(result_vtable));
            std::memcpy(
                &result_self0,
                result + kSceneProxyFirstSelfOffset,
                sizeof(result_self0));
            std::memcpy(
                &result_self1,
                result + kSceneProxySecondSelfOffset,
                sizeof(result_self1));
            std::memcpy(
                &result_self2,
                result + kSceneProxyThirdSelfOffset,
                sizeof(result_self2));
            std::memcpy(
                &result_owner,
                result + kSceneProxyOwnerOffset,
                sizeof(result_owner));
            std::memcpy(
                &value_flags,
                result + kSceneProxyValueOffset +
                    kScaleformValueFlagsOffset,
                sizeof(value_flags));
            std::memcpy(
                &value_payload,
                result + kSceneProxyValueOffset + 0x28,
                sizeof(value_payload));
            display_object = source_vtable && source_owner &&
                result_vtable == source_vtable &&
                result_self0 == result && result_self1 == result &&
                result_self2 == result && result_owner == source_owner &&
                value_payload &&
                (value_flags & kScaleformValueKindMask) ==
                    kScaleformValueKindDisplayObject;
        }
#if defined(_MSC_VER)
    } __except (out_seh = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        display_object = false;
    }
#endif

    if (!constructed || display_object) return display_object;

    unsigned long destructor_seh{};
    if (!destroy_label_result(addresses, result, destructor_seh) &&
        out_seh == 0) {
        out_seh = destructor_seh;
    }
    return false;
}

bool probe_label_path(
    const GameAddresses& addresses,
    void* movie_context,
    const char* path,
    unsigned long& out_seh) noexcept {
    alignas(16) std::byte result[scaleform_path_result_size]{};
    if (!resolve_validated_label_path(
            addresses, movie_context, path, result, out_seh)) {
        return false;
    }

    unsigned long destructor_seh{};
    if (!destroy_label_result(addresses, result, destructor_seh)) {
        out_seh = destructor_seh;
        return false;
    }
    return true;
}

LabelTargetKind discover_widget_row_label_target(
    const GameAddresses& addresses,
    void* movie_context,
    unsigned long& out_seh) noexcept {
    out_seh = 0;

    // Every supported action-like widget has one nested Text_1/Text object.
    // Its semantic role differs between the two authored movie families, so
    // its presence alone can never identify the row label.
    unsigned long common_seh{};
    const bool common_nested_text = probe_label_path(
        addresses,
        movie_context,
        kFamilyBControlCaptionPath,
        common_seh);
    if (common_seh != 0) {
        out_seh = common_seh;
        return LabelTargetKind::none;
    }

    // Family A stores the right control caption in nested Text_0/Text and the
    // left row label in the common nested Text_1/Text object.
    unsigned long family_a_seh{};
    const bool family_a_caption = probe_label_path(
        addresses,
        movie_context,
        kFamilyAControlCaptionPath,
        family_a_seh);
    if (family_a_seh != 0) {
        out_seh = family_a_seh;
        return LabelTargetKind::none;
    }

    WidgetTextLayout layout = select_widget_text_layout(
        common_nested_text,
        family_a_caption,
        false);
    if (layout == WidgetTextLayout::family_a) {
        return LabelTargetKind::family_a_row;
    }

    // Family B has no nested Text_0/Text caption. Its left row label is the
    // direct Text_0 object and the common Text_1/Text object is the right
    // control caption/value. Require that complete pair before selecting it.
    unsigned long family_b_seh{};
    const bool family_b_label = probe_label_path(
        addresses,
        movie_context,
        kFamilyBRowLabelPath,
        family_b_seh);
    if (family_b_seh != 0) {
        out_seh = family_b_seh;
        return LabelTargetKind::none;
    }

    layout = select_widget_text_layout(
        common_nested_text,
        family_a_caption,
        family_b_label);
    if (layout == WidgetTextLayout::family_b) {
        return LabelTargetKind::family_b_row;
    }
    return LabelTargetKind::none;
}

bool resolve_text_target(
    const GameAddresses& addresses,
    PhysicalBinding& binding,
    WidgetTextRole role,
    std::byte* result,
    unsigned long& out_seh) noexcept {
    out_seh = 0;
#if defined(_MSC_VER)
    __try {
#endif
        void* current_controller_vtable{};
        std::memcpy(
            &current_controller_vtable,
            binding.controller,
            sizeof(current_controller_vtable));
        if (current_controller_vtable != binding.controller_vtable) {
            return false;
        }
        void* current_caption_accessor{};
        std::memcpy(
            &current_caption_accessor,
            static_cast<const std::byte*>(current_controller_vtable) +
                kControllerCaptionAccessorSlot * sizeof(void*),
            sizeof(current_caption_accessor));
        auto* const movie_context = static_cast<std::byte*>(
            binding.row_movie_proxy);
        void* proxy_vtable{};
        void* proxy_self0{};
        void* proxy_self1{};
        void* proxy_self2{};
        void* proxy_owner{};
        std::uint32_t value_flags{};
        std::memcpy(&proxy_vtable, movie_context, sizeof(proxy_vtable));
        std::memcpy(&proxy_self0,
            movie_context + kSceneProxyFirstSelfOffset,
            sizeof(proxy_self0));
        std::memcpy(&proxy_self1,
            movie_context + kSceneProxySecondSelfOffset,
            sizeof(proxy_self1));
        std::memcpy(&proxy_self2,
            movie_context + kSceneProxyThirdSelfOffset,
            sizeof(proxy_self2));
        std::memcpy(&proxy_owner,
            movie_context + kSceneProxyOwnerOffset,
            sizeof(proxy_owner));
        std::memcpy(&value_flags,
            movie_context + kSceneProxyValueOffset +
                kScaleformValueFlagsOffset,
            sizeof(value_flags));
        if (current_caption_accessor != binding.caption_accessor ||
            proxy_vtable != binding.row_movie_proxy_vtable ||
            proxy_self0 != movie_context || proxy_self1 != movie_context ||
            proxy_self2 != movie_context ||
            proxy_owner != binding.row_movie_owner ||
            (value_flags & kScaleformValueKindMask) !=
                kScaleformValueKindDisplayObject) {
            return false;
        }
#if defined(_MSC_VER)
    } __except (out_seh = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#endif
    if (binding.label_target == LabelTargetKind::widget_unresolved) {
        const LabelTargetKind discovered = discover_widget_row_label_target(
            addresses,
            binding.row_movie_proxy,
            out_seh);
        if (discovered == LabelTargetKind::none) return false;
        binding.label_target = discovered;
    }
    const char* const path = role == WidgetTextRole::row_label
        ? label_target_path(binding.label_target)
        : caption_target_path(binding.label_target);
    if (!path) return false;
    return resolve_validated_label_path(
        addresses,
        binding.row_movie_proxy,
        path,
        result,
        out_seh);
}

bool set_resolved_label(
    const GameAddresses& addresses,
    std::byte* result,
    const wchar_t* label,
    unsigned long& out_seh) noexcept {
    out_seh = 0;
#if defined(_MSC_VER)
    __try {
#endif
        addresses.scaleform_text_setter(
            result + scaleform_text_value_offset,
            label);
        return true;
#if defined(_MSC_VER)
    } __except (out_seh = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#endif
}

bool assign_label(
    const GameAddresses& addresses,
    PhysicalBinding& binding,
    const wchar_t* label,
    unsigned long& out_seh) noexcept {
    out_seh = 0;
    if (!binding.controller || !binding.controller_vtable ||
        !binding.row_movie_proxy || !label || !*label ||
        !addresses.title_bridge_complete()) {
        return false;
    }

    alignas(16) std::byte result[scaleform_path_result_size]{};
    if (!resolve_text_target(
            addresses,
            binding,
            WidgetTextRole::row_label,
            result,
            out_seh)) {
        return false;
    }
    const bool assigned =
        set_resolved_label(addresses, result, label, out_seh);
    unsigned long destructor_seh{};
    const bool destroyed =
        destroy_label_result(addresses, result, destructor_seh);
    if (!destroyed) out_seh = destructor_seh;
    return assigned && destroyed;
}

bool assign_caption(
    const GameAddresses& addresses,
    PhysicalBinding& binding,
    const wchar_t* caption,
    unsigned long& out_seh) noexcept {
    out_seh = 0;
    if (!supports_control_caption(binding.row_kind) ||
        !binding.controller || !binding.controller_vtable ||
        !binding.row_movie_proxy || !caption || !*caption ||
        !addresses.title_bridge_complete()) {
        return false;
    }

    alignas(16) std::byte result[scaleform_path_result_size]{};
    if (!resolve_text_target(
            addresses,
            binding,
            WidgetTextRole::control_caption,
            result,
            out_seh)) {
        return false;
    }
    const bool assigned =
        set_resolved_label(addresses, result, caption, out_seh);
    unsigned long destructor_seh{};
    const bool destroyed =
        destroy_label_result(addresses, result, destructor_seh);
    if (!destroyed) out_seh = destructor_seh;
    return assigned && destroyed;
}

bool assign_help(
    PhysicalBinding& binding,
    const wchar_t* help,
    unsigned long& out_seh) noexcept {
    out_seh = 0;
    if (!binding.native_row || !help || binding.help_conflict) return false;
#if defined(_MSC_VER)
    __try {
#endif
        auto* const row = static_cast<std::byte*>(binding.native_row);
        void* current_vtable{};
        const wchar_t* current_help{};
        std::array<std::byte, 32> owned_before{};
        std::array<std::byte, 32> owned_after{};
        std::memcpy(&current_vtable, row, sizeof(current_vtable));
        std::memcpy(
            &current_help,
            row + kPropertyRowHelpDirectOffset,
            sizeof(current_help));
        if (current_vtable != binding.row_vtable ||
            (current_help != binding.original_help &&
             current_help != binding.installed_help)) {
            binding.help_conflict = true;
            return false;
        }
        std::memcpy(
            owned_before.data(),
            row + kPropertyRowOwnedHelpOffset,
            owned_before.size());
        std::memcpy(
            row + kPropertyRowHelpDirectOffset,
            &help,
            sizeof(help));
        const wchar_t* verified{};
        std::memcpy(
            &verified,
            row + kPropertyRowHelpDirectOffset,
            sizeof(verified));
        std::memcpy(
            owned_after.data(),
            row + kPropertyRowOwnedHelpOffset,
            owned_after.size());
        if (verified != help || owned_before != owned_after) return false;
        binding.installed_help = help;
        return true;
#if defined(_MSC_VER)
    } __except (out_seh = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#endif
}

bool restore_help_if_owned(
    PhysicalBinding& binding,
    unsigned long& out_seh) noexcept {
    out_seh = 0;
    if (!binding.native_row || !binding.installed_help) return true;
#if defined(_MSC_VER)
    __try {
#endif
        auto* const row = static_cast<std::byte*>(binding.native_row);
        void* current_vtable{};
        const wchar_t* current_help{};
        std::memcpy(&current_vtable, row, sizeof(current_vtable));
        std::memcpy(
            &current_help,
            row + kPropertyRowHelpDirectOffset,
            sizeof(current_help));
        if (current_vtable != binding.row_vtable ||
            current_help != binding.installed_help) {
            return true;
        }
        std::memcpy(
            row + kPropertyRowHelpDirectOffset,
            &binding.original_help,
            sizeof(binding.original_help));
        const wchar_t* verified{};
        std::memcpy(
            &verified,
            row + kPropertyRowHelpDirectOffset,
            sizeof(verified));
        return verified == binding.original_help;
#if defined(_MSC_VER)
    } __except (out_seh = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#endif
}

void restore_page_help_sources(PageBinding& binding) noexcept {
    for (std::size_t index = 0; index < binding.row_count; ++index) {
        PhysicalBinding& row = binding.rows[index];
        unsigned long seh{};
        if (!restore_help_if_owned(row, seh)) {
            log_fault(
                "help-source restoration",
                binding.page,
                row.physical_index,
                seh);
        }
    }
}

void apply_binding(
    void* page,
    const GameAddresses& addresses,
    PhysicalBinding& binding) noexcept {
    const detail::RowPresentationView view =
        detail::row_presentation_view(binding.presentation_slot);
    if (!view) return;

    if (binding.help_revision != view.revision && !binding.help_conflict &&
        !(binding.help_failed_revision == view.revision &&
          binding.help_failures >= 4u)) {
        if (binding.help_failed_revision != view.revision) {
            binding.help_failed_revision = view.revision;
            binding.help_failures = 0;
        }
        unsigned long seh{};
        if (assign_help(binding, view.help, seh)) {
            binding.help_revision = view.revision;
            binding.help_failures = 0;
        } else if (!binding.help_conflict) {
            ++binding.help_failures;
            log_fault("help-source publication", page,
                binding.physical_index, seh);
        } else {
            log_fault("help-source ownership conflict", page,
                binding.physical_index, seh);
        }
    }

    // Action-like widgets publish their registered text during every native
    // page frame. Reapply an edited row label after that pass even when the
    // client revision is unchanged. Ordinary rows retain the assigned label
    // and continue to use the cheaper revision-only path.
    const bool native_frame_republishes_label =
        binding.label_target == LabelTargetKind::family_a_row ||
        binding.label_target == LabelTargetKind::family_b_row;
    if ((binding.label_revision != view.revision ||
         native_frame_republishes_label) &&
        !(binding.label_failed_revision == view.revision &&
          binding.label_failures >= 4u)) {
        if (binding.label_failed_revision != view.revision) {
            binding.label_failed_revision = view.revision;
            binding.label_failures = 0;
        }
        unsigned long seh{};
        if (assign_label(addresses, binding, view.label, seh)) {
            binding.label_revision = view.revision;
            binding.label_failures = 0;
        } else {
            ++binding.label_failures;
            log_fault("label assignment", page,
                binding.physical_index, seh);
        }
    }

    // The right-hand text of Button, PopupChoice, and Submenu rows is also
    // native-owned and republished every frame. A non-null desired caption is
    // therefore an explicit persistent override, not a one-shot assignment.
    // PopupChoice continues to own its selection and popup list; only its
    // closed-row selected-value presentation is replaced here.
    if (view.caption && supports_control_caption(binding.row_kind) &&
        !(binding.caption_failed_revision == view.revision &&
          binding.caption_failures >= 4u)) {
        if (binding.caption_failed_revision != view.revision) {
            binding.caption_failed_revision = view.revision;
            binding.caption_failures = 0;
        }
        unsigned long seh{};
        if (assign_caption(addresses, binding, view.caption, seh)) {
            binding.caption_revision = view.revision;
            binding.caption_failures = 0;
        } else {
            ++binding.caption_failures;
            log_fault("caption assignment", page,
                binding.physical_index, seh);
        }
    }
}

bool capture_physical_binding(
    void* native_row,
    const GameAddresses& addresses,
    RowKind row_kind,
    std::size_t presentation_slot,
    std::size_t physical_index,
    PhysicalBinding& captured,
    unsigned long& out_seh) noexcept {
    out_seh = 0;
    if (!native_row || !addresses.edit_property_vtable ||
        !addresses.text_input_controller_vtable ||
        !addresses.action_button_controller_vtable ||
        !addresses.title_bridge_complete()) {
        return false;
    }
    captured = {};
    captured.row_kind = row_kind;
    captured.presentation_slot = presentation_slot;
    captured.physical_index = physical_index;
    captured.native_row = native_row;
#if defined(_MSC_VER)
    __try {
#endif
        auto* const row = static_cast<std::byte*>(native_row);
        std::memcpy(&captured.row_vtable, row,
            sizeof(captured.row_vtable));
        if (captured.row_vtable != addresses.edit_property_vtable) {
            return false;
        }
        std::memcpy(&captured.original_help,
            row + kPropertyRowHelpDirectOffset,
            sizeof(captured.original_help));
        std::memcpy(&captured.controller,
            row + kPropertyControllerOffset,
            sizeof(captured.controller));
        if (!captured.controller) return false;

        std::memcpy(&captured.controller_vtable,
            captured.controller,
            sizeof(captured.controller_vtable));
        if (!captured.controller_vtable) return false;
        void* expected_caption_accessor{};
        void* expected_action_accessor{};
        std::memcpy(&captured.caption_accessor,
            static_cast<const std::byte*>(captured.controller_vtable) +
                kControllerCaptionAccessorSlot * sizeof(void*),
            sizeof(captured.caption_accessor));
        std::memcpy(&expected_caption_accessor,
            static_cast<const std::byte*>(
                addresses.text_input_controller_vtable) +
                kControllerCaptionAccessorSlot * sizeof(void*),
            sizeof(expected_caption_accessor));
        std::memcpy(&expected_action_accessor,
            static_cast<const std::byte*>(
                addresses.action_button_controller_vtable) +
                kControllerCaptionAccessorSlot * sizeof(void*),
            sizeof(expected_action_accessor));
        if (!captured.caption_accessor) {
            return false;
        }
        const bool action_controller =
            captured.controller_vtable ==
                addresses.action_button_controller_vtable &&
            captured.caption_accessor == expected_action_accessor;
        const bool caption_controller =
            !action_controller &&
            captured.caption_accessor == expected_caption_accessor;
        const bool expected_action_controller =
            uses_action_controller(row_kind);
        if ((expected_action_controller && !action_controller) ||
            (!expected_action_controller && !caption_controller)) {
            return false;
        }
        captured.label_target = uses_widget_text_pair(row_kind)
            ? LabelTargetKind::widget_unresolved
            : LabelTargetKind::ordinary_row;

        auto* const movie_context =
            static_cast<std::byte*>(captured.controller) +
            kControllerMovieContextOffset;
        captured.row_movie_proxy = movie_context;
        void* self0{};
        void* self1{};
        void* self2{};
        std::uint32_t value_flags{};
        std::memcpy(&captured.row_movie_proxy_vtable,
            movie_context,
            sizeof(captured.row_movie_proxy_vtable));
        std::memcpy(&self0,
            movie_context + kSceneProxyFirstSelfOffset,
            sizeof(self0));
        std::memcpy(&self1,
            movie_context + kSceneProxySecondSelfOffset,
            sizeof(self1));
        std::memcpy(&self2,
            movie_context + kSceneProxyThirdSelfOffset,
            sizeof(self2));
        std::memcpy(&captured.row_movie_owner,
            movie_context + kSceneProxyOwnerOffset,
            sizeof(captured.row_movie_owner));
        std::memcpy(&value_flags,
            movie_context + kSceneProxyValueOffset +
                kScaleformValueFlagsOffset,
            sizeof(value_flags));
        const bool movie_context_valid =
            captured.row_movie_proxy_vtable &&
            self0 == movie_context && self1 == movie_context &&
            self2 == movie_context && captured.row_movie_owner &&
            (value_flags & kScaleformValueKindMask) ==
                kScaleformValueKindDisplayObject;
        return movie_context_valid;
#if defined(_MSC_VER)
    } __except (out_seh = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#endif
}

} // namespace

bool native_row_presentation_build_supported(
    const ModuleView& game) noexcept {
    const EldenRingBuildIdentity* const identity = find_elden_ring_build(
        game.timestamp(), game.image_size());
    return identity &&
        (identity->build == EldenRingBuild::v2_7_1_0 ||
         identity->build == EldenRingBuild::v2_7_2_0);
}

void begin_native_row_presentation_page(
    void* page,
    const GameAddresses& addresses) noexcept {
    void* vtable{};
    std::size_t count{};
    unsigned long seh{};
    if (!read_page_identity(page, addresses, vtable, count, seh)) return;

    std::lock_guard lock(g_mutex);
    PageBinding* binding = find_page(page);
    if (!binding) {
        const auto empty = std::find_if(
            g_pages.begin(), g_pages.end(),
            [](const PageBinding& candidate) {
                return !candidate.active;
            });
        if (empty != g_pages.end()) {
            binding = &*empty;
        } else {
            const auto oldest = std::min_element(
                g_pages.begin(), g_pages.end(),
                [](const PageBinding& left, const PageBinding& right) {
                    return left.use_serial < right.use_serial;
                });
            binding = oldest == g_pages.end() ? nullptr : &*oldest;
        }
    }
    if (!binding) return;
    if (binding->active) {
        // A physical panel can be reconstructed in place, and the bounded
        // tracker can also evict an old epoch. Restore every pointer that is
        // still demonstrably ours before replacing its ownership record.
        restore_page_help_sources(*binding);
    }
    std::uint64_t epoch = ++g_epoch;
    if (epoch == 0) epoch = ++g_epoch;
    *binding = {
        .page = page,
        .page_vtable = vtable,
        .epoch = epoch,
        .use_serial = ++g_use_serial,
        .rows = {},
        .row_count = 0,
        .active = true,
    };
}

RowPresentationMaterializationTicket
begin_native_row_presentation_materialization(
    void* page,
    const GameAddresses& addresses,
    const detail::CompiledRow& row) noexcept {
    RowPresentationMaterializationTicket ticket{};
    if (row.presentation_slot == detail::invalid_compiled_index) return ticket;

    void* vtable{};
    std::size_t count{};
    unsigned long seh{};
    if (!read_page_identity(page, addresses, vtable, count, seh)) return ticket;

    std::lock_guard lock(g_mutex);
    PageBinding* binding = find_page(page);
    if (!binding || binding->page_vtable != vtable) return ticket;
    ticket.page = page;
    ticket.presentation_slot = row.presentation_slot;
    ticket.previous_count = count;
    ticket.page_epoch = binding->epoch;
    ticket.armed = true;
    return ticket;
}

void complete_native_row_presentation_materialization(
    void* page,
    const GameAddresses& addresses,
    const detail::CompiledRow& row,
    RowPresentationMaterializationTicket ticket,
    bool constructor_succeeded) noexcept {
    if (!ticket.armed || !constructor_succeeded || page != ticket.page ||
        row.presentation_slot != ticket.presentation_slot) {
        return;
    }

    void* page_vtable{};
    std::size_t count{};
    unsigned long seh{};
    if (!read_page_identity(page, addresses, page_vtable, count, seh) ||
        count != ticket.previous_count + 1u ||
        ticket.previous_count >= kMaximumPropertyRows) {
        log_fault("constructor count delta", page,
            ticket.previous_count, seh);
        return;
    }

    auto* const native_row = static_cast<std::byte*>(page) +
        kPropertyCollectionOffset +
        ticket.previous_count * kPropertyRowStride;
    PhysicalBinding captured{};
    if (!capture_physical_binding(
            native_row,
            addresses,
            row.kind,
            row.presentation_slot,
            ticket.previous_count,
            captured,
            seh)) {
        log_fault("constructor capture", page,
            ticket.previous_count, seh);
        return;
    }

    std::lock_guard lock(g_mutex);
    PageBinding* binding = find_page(page);
    if (!binding || binding->epoch != ticket.page_epoch ||
        binding->page_vtable != page_vtable) {
        return;
    }
    const auto existing = std::find_if(
        binding->rows.begin(),
        binding->rows.begin() + binding->row_count,
        [&](const PhysicalBinding& candidate) {
            return candidate.physical_index == captured.physical_index;
        });
    if (existing == binding->rows.begin() + binding->row_count) {
        if (binding->row_count >= binding->rows.size()) {
            log_fault("binding capacity", page, captured.physical_index);
            return;
        }
        binding->rows[binding->row_count++] = captured;
    } else {
        *existing = captured;
    }
}

void apply_native_row_presentations(
    void* page,
    const GameAddresses& addresses) noexcept {
    void* page_vtable{};
    std::size_t count{};
    unsigned long seh{};
    if (!read_page_identity(page, addresses, page_vtable, count, seh)) return;

    std::lock_guard lock(g_mutex);
    PageBinding* binding = find_page(page);
    if (!binding || binding->page_vtable != page_vtable) return;
    binding->use_serial = ++g_use_serial;
    for (std::size_t index = 0; index < binding->row_count; ++index) {
        PhysicalBinding& row = binding->rows[index];
        if (row.physical_index >= count ||
            row.native_row != static_cast<std::byte*>(page) +
                kPropertyCollectionOffset +
                row.physical_index * kPropertyRowStride) {
            continue;
        }
        apply_binding(page, addresses, row);
    }
}

void retire_native_row_presentation_page(void* page) noexcept {
    if (!page) return;
    std::lock_guard lock(g_mutex);
    for (PageBinding& binding : g_pages) {
        if (binding.active && binding.page == page) {
            restore_page_help_sources(binding);
            binding = {};
        }
    }
}

void reset_native_row_presentations() noexcept {
    std::lock_guard lock(g_mutex);
    for (PageBinding& binding : g_pages) {
        if (binding.active) restore_page_help_sources(binding);
    }
    g_pages.fill({});
    g_epoch = 0;
    g_use_serial = 0;
    g_fault_logs.store(0, std::memory_order_release);
}

} // namespace erui::native
