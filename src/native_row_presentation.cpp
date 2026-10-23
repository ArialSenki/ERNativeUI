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
constexpr char kCaptionPath[] = "Caption/Text_0";

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
    const wchar_t* original_help{};
    const wchar_t* installed_help{};
    std::uint64_t label_revision{};
    std::uint64_t help_revision{};
    std::uint64_t label_failed_revision{};
    std::uint64_t help_failed_revision{};
    std::uint8_t label_failures{};
    std::uint8_t help_failures{};
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

bool resolve_label_target(
    const GameAddresses& addresses,
    const PhysicalBinding& binding,
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
        return addresses.scaleform_path_resolver(
            movie_context, result, kCaptionPath) == result;
#if defined(_MSC_VER)
    } __except (out_seh = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#endif
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

bool assign_label(
    const GameAddresses& addresses,
    const PhysicalBinding& binding,
    const wchar_t* label,
    unsigned long& out_seh) noexcept {
    out_seh = 0;
    if (!binding.controller || !binding.controller_vtable ||
        !binding.row_movie_proxy || !label || !*label ||
        !addresses.title_bridge_complete()) {
        return false;
    }

    alignas(16) std::byte result[scaleform_path_result_size]{};
    if (!resolve_label_target(addresses, binding, result, out_seh)) {
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

    if (binding.label_revision != view.revision &&
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
}

bool capture_physical_binding(
    void* native_row,
    const GameAddresses& addresses,
    std::size_t presentation_slot,
    std::size_t physical_index,
    PhysicalBinding& captured,
    unsigned long& out_seh) noexcept {
    out_seh = 0;
    if (!native_row || !addresses.edit_property_vtable ||
        !addresses.text_input_controller_vtable) {
        return false;
    }
    captured = {};
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
        std::memcpy(&captured.caption_accessor,
            static_cast<const std::byte*>(captured.controller_vtable) +
                kControllerCaptionAccessorSlot * sizeof(void*),
            sizeof(captured.caption_accessor));
        std::memcpy(&expected_caption_accessor,
            static_cast<const std::byte*>(
                addresses.text_input_controller_vtable) +
                kControllerCaptionAccessorSlot * sizeof(void*),
            sizeof(expected_caption_accessor));
        if (!captured.caption_accessor ||
            captured.caption_accessor != expected_caption_accessor) {
            return false;
        }

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
        return captured.row_movie_proxy_vtable &&
            self0 == movie_context && self1 == movie_context &&
            self2 == movie_context && captured.row_movie_owner &&
            (value_flags & kScaleformValueKindMask) ==
                kScaleformValueKindDisplayObject;
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
