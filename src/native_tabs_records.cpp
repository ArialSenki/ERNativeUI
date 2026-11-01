// Tab records, viewport mapping, panel state isolation and runtime initialization.
#include "native_tabs_internal.hpp"

namespace native_tabs_detail {

std::byte* record_at(void* top, std::size_t physical) noexcept {
    if (!top || physical >= kPhysicalTabCapacity) return nullptr;
    return static_cast<std::byte*>(top) + kCategoryStorageOffset +
        physical * kCategoryRecordSize;
}

bool read_live_tab_geometry(
    void* top,
    LiveTabGeometry& geometry) noexcept {
    geometry = {};
    if (!top) return false;
    std::uint64_t record_count{};
    std::int32_t active{-1};
    std::int32_t selected{-1};
    std::int32_t horizontal{-1};
    std::int32_t vertical{-1};
    const auto* const selector =
        static_cast<const std::byte*>(top) + kSelectorOffset;
    void* selector_vtable{};
    if (!guarded_read(selector, selector_vtable) ||
        selector_vtable != g_game.base + kSelectorVtableRva ||
        !guarded_read(
            static_cast<const std::byte*>(top) + kCategoryCountOffset,
            record_count) ||
        !guarded_read(selector + kSelectorActiveCountOffset, active) ||
        !guarded_read(selector + kSelectorSelectedIndexOffset, selected) ||
        !guarded_read(
            selector + kSelectorHorizontalExtentOffset, horizontal) ||
        !guarded_read(selector + kSelectorVerticalExtentOffset, vertical)) {
        return false;
    }
    erui::detail::NativeTabLiveGeometry validated{};
    if (!erui::detail::validate_native_tab_live_geometry(
            record_count,
            active,
            selected,
            horizontal,
            vertical,
            validated)) {
        return false;
    }
    geometry.active = validated.active_slot_count;
    geometry.authored = validated.authored_cell_count;
    geometry.selected = validated.selected_slot;
    return true;
}

bool extra_records_are_graphics_carriers(
    void* top,
    std::size_t active) noexcept {
    if (!top || active < kStockLogicalTabCount ||
        active > kPhysicalTabCapacity) {
        return false;
    }
    if (active == kStockLogicalTabCount) return true;

    std::int32_t graphics_category{-1};
    const wchar_t* graphics_title{};
    std::array<std::byte, kPresentationSize> graphics_presentation{};
    const std::byte* const graphics = record_at(
        top, kStockLogicalTabCount - 1U);
    if (!guarded_read(graphics + kTitleOffset, graphics_title) ||
        !graphics_title ||
        !guarded_read(graphics + kCategoryIdOffset, graphics_category) ||
        graphics_category != kExpectedStockCategories.back() ||
        !guarded_copy(
            graphics_presentation.data(),
            graphics + kPresentationOffset,
            graphics_presentation.size())) {
        return false;
    }

    std::array<wchar_t, kTitleCapacity> expected_title{};
    if (!copy_title(graphics_title, expected_title)) return false;
    for (std::size_t physical = kStockLogicalTabCount;
         physical < active;
         ++physical) {
        const std::byte* const record = record_at(top, physical);
        const wchar_t* title{};
        std::int32_t category{-1};
        std::array<std::byte, kPresentationSize> presentation{};
        std::array<wchar_t, kTitleCapacity> title_copy{};
        if (!guarded_read(record + kTitleOffset, title) || !title ||
            !copy_title(title, title_copy) || title_copy != expected_title ||
            !guarded_read(record + kCategoryIdOffset, category) ||
            category != graphics_category ||
            !guarded_copy(
                presentation.data(),
                record + kPresentationOffset,
                presentation.size()) ||
            presentation != graphics_presentation) {
            return false;
        }
    }
    return true;
}

bool validate_top(void* top) noexcept {
    if (!top) return false;
    void* vtable{};
    LiveTabGeometry geometry{};
    if (!guarded_read(top, vtable) ||
        vtable != g_game.base + kTopVtableRva ||
        !read_live_tab_geometry(top, geometry)) {
        return false;
    }

    for (std::size_t physical = 0; physical < geometry.active; ++physical) {
        void* record_vtable{};
        if (!guarded_read(record_at(top, physical), record_vtable) ||
            record_vtable != g_game.base + kCategoryRecordVtableRva) {
            return false;
        }
    }
#if defined(ER_NATIVE_UI_PRODUCTION_TABS)
    if (!g_native_tab_runtime) return false;
    const std::size_t epoch_count =
        g_native_tab_runtime->active_physical_slot_count();
    // The transformed Scaleform movie and its selector layout are immutable
    // for this process. A different D0 while the prior epoch is still bound is
    // therefore an identity/lifecycle mismatch, never an in-place resize.
    if (epoch_count != 0U) return epoch_count == geometry.active;
    if (!extra_records_are_graphics_carriers(top, geometry.active)) {
        log_failure(
            "live Configuration exposes %zu active tabs, but records beyond the eight stock routes are not exact Graphics carriers; custom-tab mutation remains disabled for this movie epoch",
            geometry.active);
        return false;
    }
#else
    if (geometry.active != kStockLogicalTabCount) return false;
#endif
    return true;
}

bool copy_title(
    const wchar_t* source,
    std::array<wchar_t, kTitleCapacity>& destination) noexcept {
    destination.fill(L'\0');
    if (!source) return false;
#if defined(_MSC_VER)
    __try {
#endif
        for (std::size_t index = 0; index + 1 < destination.size(); ++index) {
            destination[index] = source[index];
            if (source[index] == L'\0') return index != 0;
        }
        destination.back() = L'\0';
        return false;
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        destination.fill(L'\0');
        return false;
    }
#endif
}

bool make_synthetic_icon_descriptor(
    std::array<std::byte, kPresentationSize>& descriptor,
    std::int32_t icon_id) noexcept {
    if (!g_make_icon_descriptor) return false;
    descriptor.fill(std::byte{});

    void* result{};
#if defined(_MSC_VER)
    __try {
#endif
        result = g_make_icon_descriptor(
            descriptor.data(), icon_id);
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        result = nullptr;
    }
#endif
    std::uint8_t resource_family{};
    std::int32_t resource_id{-1};
    std::memcpy(
        &resource_family,
        descriptor.data() + 0x38,
        sizeof(resource_family));
    std::memcpy(
        &resource_id,
        descriptor.data() + 0x3C,
        sizeof(resource_id));
    return result == descriptor.data() && resource_family == 0 &&
        resource_id == icon_id;
}

bool snapshot_record_fields(
    void* top,
    std::array<RecordFields, kPhysicalTabCapacity>& fields) noexcept {
    for (std::size_t physical = 0;
         physical < active_physical_tab_count();
         ++physical) {
        const std::byte* const record = record_at(top, physical);
        if (!guarded_read(record + kTitleOffset, fields[physical].title) ||
            !guarded_read(
                record + kCategoryIdOffset,
                fields[physical].category) ||
            !guarded_copy(
                fields[physical].presentation.data(),
                record + kPresentationOffset,
                kPresentationSize)) {
            return false;
        }
    }
    return true;
}

bool write_record_fields(
    void* top,
    const std::array<RecordFields, kPhysicalTabCapacity>& fields) noexcept {
    for (std::size_t physical = 0;
         physical < active_physical_tab_count();
         ++physical) {
        std::byte* const record = record_at(top, physical);
        if (!guarded_copy(
                record + kTitleOffset,
                &fields[physical].title,
                sizeof(fields[physical].title)) ||
            !guarded_copy(
                record + kCategoryIdOffset,
                &fields[physical].category,
                sizeof(fields[physical].category)) ||
            !guarded_copy(
                record + kPresentationOffset,
                fields[physical].presentation.data(),
                kPresentationSize)) {
            return false;
        }
    }
    return true;
}

bool build_mapping_fields(
    const TabViewport& viewport,
    std::array<RecordFields, kPhysicalTabCapacity>& fields) noexcept {
    if (!viewport.initialized() ||
        viewport.visible_slot_count() != active_physical_tab_count()) {
        return false;
    }
    for (std::size_t physical = 0;
         physical < active_physical_tab_count();
         ++physical) {
        std::size_t logical{};
        if (!viewport.logical_for_physical(physical, logical) ||
            logical >= active_logical_tab_count()) {
            return false;
        }
        const LogicalTab* const tab = runtime_logical_tab(logical);
        if (!tab) return false;
        fields[physical].title = tab->title.data();
        fields[physical].category = tab->category;
        fields[physical].presentation = tab->presentation;
        if (tab_overflow_indicator(viewport, physical) !=
            TabOverflowIndicator::none) {
            fields[physical].presentation =
                g_runtime.overflow_presentation;
        }
    }
    return true;
}

bool records_match_mapping(void* top, const TabViewport& viewport) noexcept {
    std::array<RecordFields, kPhysicalTabCapacity> expected{};
    std::array<RecordFields, kPhysicalTabCapacity> actual{};
    if (!build_mapping_fields(viewport, expected) ||
        !snapshot_record_fields(top, actual)) {
        return false;
    }
    for (std::size_t physical = 0;
         physical < active_physical_tab_count();
         ++physical) {
        if (actual[physical].title != expected[physical].title ||
            actual[physical].category != expected[physical].category ||
            actual[physical].presentation != expected[physical].presentation) {
            return false;
        }
    }
    return true;
}

const char* stock_epoch_match_name(
    erui::detail::StockTabEpochMatch match) noexcept {
    using erui::detail::StockTabEpochMatch;
    switch (match) {
    case StockTabEpochMatch::exact: return "exact";
    case StockTabEpochMatch::missing_title_pointer:
        return "missing-title-pointer";
    case StockTabEpochMatch::borrowed_title_pointer:
        return "borrowed-title-pointer";
    case StockTabEpochMatch::incomplete_title:
        return "incomplete-title";
    case StockTabEpochMatch::category_mismatch:
        return "category-mismatch";
    case StockTabEpochMatch::title_mismatch: return "title-mismatch";
    case StockTabEpochMatch::presentation_mismatch:
        return "presentation-mismatch";
    }
    return "unknown";
}

bool exact_stock_layout_republished(void* top) noexcept {
    static_assert(
        kStockLogicalTabCount == erui::detail::tab_record_epoch_slot_count);
    static_assert(
        kTitleCapacity == erui::detail::tab_record_epoch_title_capacity);
    static_assert(
        kPresentationSize ==
            erui::detail::tab_record_epoch_presentation_size);

    std::array<RecordFields, kPhysicalTabCapacity> actual{};
    if (!snapshot_record_fields(top, actual) ||
        !extra_records_are_graphics_carriers(
            top, active_physical_tab_count())) {
        return false;
    }

    std::array<
        erui::detail::TabRecordObservation,
        kStockLogicalTabCount> observations{};
    std::array<
        erui::detail::StockTabRecordFingerprint,
        kStockLogicalTabCount> stock{};
#if defined(ER_NATIVE_UI_PRODUCTION_TABS)
    if (!g_logical_tab_catalogue ||
        g_logical_tab_catalogue->tabs.size() !=
            active_logical_tab_count()) {
        return false;
    }
#else
    std::array<std::uintptr_t, kLogicalTabCount> owned_title_storage{};
    for (std::size_t logical = 0;
         logical < active_logical_tab_count();
         ++logical) {
        const LogicalTab* const tab = runtime_logical_tab(logical);
        if (!tab) return false;
        owned_title_storage[logical] = reinterpret_cast<std::uintptr_t>(
            tab->title.data());
    }
    const std::span<const std::uintptr_t> owned_titles(
        owned_title_storage);
#endif
    for (std::size_t physical = 0;
         physical < kStockLogicalTabCount;
         ++physical) {
        observations[physical].source_title =
            reinterpret_cast<std::uintptr_t>(actual[physical].title);
        observations[physical].title_complete = copy_title(
            actual[physical].title, observations[physical].title);
        observations[physical].category = actual[physical].category;
        observations[physical].presentation =
            actual[physical].presentation;

        const LogicalTab* const tab = runtime_logical_tab(physical);
        if (!tab) return false;
        stock[physical].title = tab->title;
        stock[physical].category = kExpectedStockCategories[physical];
        stock[physical].presentation = tab->presentation;
    }

#if defined(ER_NATIVE_UI_PRODUCTION_TABS)
    const erui::detail::StockTabEpochResult result =
        erui::detail::classify_stock_tab_epoch_with_predicate(
            observations,
            stock,
            [](std::uintptr_t candidate) noexcept {
                for (const LogicalTab& tab :
                     g_logical_tab_catalogue->tabs) {
                    if (candidate == reinterpret_cast<std::uintptr_t>(
                            tab.title.data())) {
                        return true;
                    }
                }
                return false;
            });
#else
    const erui::detail::StockTabEpochResult result =
        erui::detail::classify_stock_tab_epoch(
            observations, stock, owned_titles);
#endif
    if (result.exact()) return true;

    const std::uint32_t mismatch =
        g_record_mismatch_log_count.fetch_add(
            1, std::memory_order_relaxed) + 1;
    if (mismatch <= kMaximumRecordMismatchLogs &&
        result.slot < kStockLogicalTabCount) {
        const std::size_t physical = result.slot;
        std::uint32_t actual_token{};
        std::int32_t actual_resource{-1};
        std::uint32_t expected_token{};
        std::int32_t expected_resource{-1};
        std::memcpy(
            &actual_token,
            observations[physical].presentation.data(),
            sizeof(actual_token));
        std::memcpy(
            &actual_resource,
            observations[physical].presentation.data() + 0x3C,
            sizeof(actual_resource));
        std::memcpy(
            &expected_token,
            stock[physical].presentation.data(),
            sizeof(expected_token));
        std::memcpy(
            &expected_resource,
            stock[physical].presentation.data() + 0x3C,
            sizeof(expected_resource));
        log_line(
            "WARNING: same-address record mismatch is not an exact stock reset: reason=%s physical=%zu titleComplete=%u actualTitle=%ls expectedTitle=%ls category=%d/%d presentationMatch=%u iconToken=%u/%u resourceId=%d/%d",
            stock_epoch_match_name(result.match),
            physical,
            observations[physical].title_complete ? 1u : 0u,
            observations[physical].title_complete
                ? observations[physical].title.data()
                : L"<unreadable-or-truncated>",
            stock[physical].title.data(),
            observations[physical].category,
            stock[physical].category,
            observations[physical].presentation ==
                    stock[physical].presentation
                ? 1u : 0u,
            static_cast<unsigned>(actual_token),
            static_cast<unsigned>(expected_token),
            actual_resource,
            expected_resource);
    }
    return false;
}

bool destroy_proxy(ResolvedTabProxy& proxy) noexcept {
    if (!proxy.constructed) return true;
    if (!g_destroy_scaleform_value) return false;
#if defined(_MSC_VER)
    __try {
#endif
        g_destroy_scaleform_value(
            proxy.storage.data() + kSceneObjProxyValueOffset);
        proxy.constructed = false;
        return true;
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        log_failure(
            "Scaleform proxy cleanup faulted (seh=0x%08lX)",
            static_cast<unsigned long>(GetExceptionCode()));
        return false;
    }
#endif
}

bool destroy_proxies(
    std::array<ResolvedTabProxy, kPhysicalTabCapacity>& proxies) noexcept {
    bool exact{true};
    for (std::size_t physical = 0;
         physical < active_physical_tab_count();
         ++physical) {
        exact = destroy_proxy(proxies[physical]) && exact;
    }
    return exact;
}

bool destroy_tab_layers(ResolvedTabLayers& layers) noexcept {
    const bool front_exact = destroy_proxies(layers.tab_list);
    const bool back_exact = destroy_proxies(layers.back_tab_list);
    return front_exact && back_exact;
}

// Shared by resolve_tab_cell and resolve_injected_icon_parent: resolves `path`
// in the menu movie into `proxy`. On failure it logs `failure_format` (a
// literal with one %s for the path; it is also the failure-budget site key)
// and destroys the proxy.
bool resolve_proxy_at_path(
    void* top,
    const char* path,
    ResolvedTabProxy& proxy,
    const char* failure_format) noexcept {
    bool resolved{};
#if defined(_MSC_VER)
    __try {
#endif
        void* const movie_context =
            static_cast<std::byte*>(top) + kMovieContextOffset;
        void* const result = g_resolve_path(
            movie_context, proxy.storage.data(), path);
        proxy.constructed = result == proxy.storage.data();
        resolved = proxy.constructed &&
            g_value_exists(proxy.storage.data());
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        resolved = false;
    }
#endif
    if (!resolved) {
        log_failure(failure_format, path);
        destroy_proxy(proxy);
    }
    return resolved;
}

bool resolve_tab_cell(
    void* top,
    const char* layer_name,
    std::size_t physical,
    ResolvedTabProxy& proxy) noexcept {
    if (!top || !layer_name || physical >= active_physical_tab_count() ||
        !g_resolve_path || !g_value_exists || !g_destroy_scaleform_value) {
        return false;
    }

    char path[64]{};
    const int written = _snprintf_s(
        path,
        std::size(path),
        _TRUNCATE,
        "%s/Item_0_%zu",
        layer_name,
        physical);
    if (written <= 0) return false;

    return resolve_proxy_at_path(
        top, path, proxy, "could not resolve existing tab cell '%s'");
}

bool resolve_tab_layer(
    void* top,
    const char* layer_name,
    std::array<ResolvedTabProxy, kPhysicalTabCapacity>& proxies) noexcept {
    if (!top || !layer_name || !g_resolve_path || !g_value_exists ||
        !g_destroy_scaleform_value) {
        return false;
    }
    for (std::size_t physical = 0;
         physical < active_physical_tab_count();
         ++physical) {
        if (!resolve_tab_cell(
                top, layer_name, physical, proxies[physical])) {
            destroy_proxies(proxies);
            return false;
        }
    }
    return true;
}

bool resolve_tab_layers(
    void* top,
    ResolvedTabLayers& layers) noexcept {
    // TabList and BackTabList are coincident presentation layers. Resolve the
    // complete pair before any record mutation so a missing cell cannot leave
    // one layer showing a different logical window from the other.
    if (!resolve_tab_layer(top, "TabList", layers.tab_list)) {
        log_failure("could not resolve the complete TabList layer");
        destroy_tab_layers(layers);
        return false;
    }
    if (!resolve_tab_layer(top, "BackTabList", layers.back_tab_list)) {
        log_failure("could not resolve the complete BackTabList layer");
        destroy_tab_layers(layers);
        return false;
    }
    return true;
}

bool resolve_injected_icon_parent(
    void* top,
    TabLayer layer,
    std::size_t physical,
    ResolvedTabProxy& proxy) noexcept {
    if (!top || physical >= active_physical_tab_count() ||
        (layer != TabLayer::front && layer != TabLayer::back) ||
        !g_resolve_path || !g_value_exists || !g_destroy_scaleform_value) {
        return false;
    }

    char path[96]{};
    const char* const format = layer == TabLayer::front
        ? "TabList/Item_0_%zu/Cursor/ItemIcon"
        : "BackTabList/Item_0_%zu/ItemIcon";
    const int written = _snprintf_s(
        path, std::size(path), _TRUNCATE, format, physical);
    if (written <= 0) return false;

    return resolve_proxy_at_path(
        top, path, proxy, "could not reacquire injected-icon parent '%s'");
}

bool sample_current_movie_fingerprint(
    void* top,
    InjectedMovieFingerprint& fingerprint) noexcept {
    fingerprint = {};
    void* const storage = HeapAlloc(
        GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(ResolvedTabProxy));
    auto* const proxy = storage
        ? ::new (storage) ResolvedTabProxy{}
        : nullptr;
    if (!proxy) {
        log_failure(
            "movie fingerprint sampling could not allocate a stable proxy");
        return false;
    }

    bool exact{};
    if (resolve_injected_icon_parent(
            top, TabLayer::front, 0, *proxy)) {
        void* const wrapper =
            proxy->storage.data() + kSceneObjProxyValueOffset;
        DisplayValueView view{};
        void* owner_link{};
        void* movie_root{};
        exact = inspect_display_value(wrapper, view) &&
            exact_as3_display_bridge(view) &&
            guarded_read(
                static_cast<const std::byte*>(view.object_interface) + 0x08,
                owner_link) && owner_link &&
            guarded_read(
                static_cast<const std::byte*>(owner_link) + 0x18,
                movie_root) && movie_root;
        if (exact) {
            fingerprint.object_interface = view.object_interface;
            fingerprint.owner_link = owner_link;
            fingerprint.movie_root = movie_root;
            fingerprint.presentation_epoch = g_runtime.presentation_epoch;
        }
    }

    const bool proxy_clean = destroy_proxy(*proxy);
    if (proxy_clean) {
        proxy->~ResolvedTabProxy();
        HeapFree(GetProcessHeap(), 0, storage);
    } else {
        log_failure(
            "movie fingerprint sampling retained its stable proxy after cleanup uncertainty");
    }
    return exact && proxy_clean;
}

bool same_movie_fingerprint(
    const InjectedMovieFingerprint& left,
    const InjectedMovieFingerprint& right) noexcept {
    return left.object_interface && left.owner_link && left.movie_root &&
        left.object_interface == right.object_interface &&
        left.owner_link == right.owner_link &&
        left.movie_root == right.movie_root;
}

bool repaint_tab_cell(
    void* top,
    const char* layer_name,
    std::size_t physical,
    const TabViewport& viewport,
    ResolvedTabProxy& proxy) noexcept {
    if (!top || !layer_name ||
        physical >= active_physical_tab_count() ||
        !g_present_tab || !proxy.constructed) {
        destroy_proxy(proxy);
        return false;
    }

    bool presented{};
    bool present_faulted{};
    const RepaintContext previous_context = g_repaint_context;
    TabLayer layer{TabLayer::none};
    if (std::strcmp(layer_name, "TabList") == 0) {
        layer = TabLayer::front;
    } else if (std::strcmp(layer_name, "BackTabList") == 0) {
        layer = TabLayer::back;
    }
    std::size_t logical{invalid_tab_index};
    (void)viewport.logical_for_physical(physical, logical);
    g_repaint_context = {true, top, layer, physical, logical};
#if defined(_MSC_VER)
    __try {
#endif
        void* top_reference = top;
        g_present_tab(
            &top_reference,
            proxy.storage.data(),
            static_cast<std::uint32_t>(physical));
        presented = true;
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        present_faulted = true;
        log_failure(
            "%s repaint faulted at physical=%zu (seh=0x%08lX)",
            layer_name,
            physical,
            static_cast<unsigned long>(GetExceptionCode()));
    }
#endif
    if (presented) {
        const bool retirement_exact =
            reconcile_rgba8_retirement_debt("post-present");
        if (!retirement_exact ||
            !rgba8_references_match_tracked_hosts("post-present")) {
            if (g_in_place_rgba8_rebind_transaction.active) {
                g_in_place_rgba8_rebind_transaction.failed = true;
            } else {
                g_rgba8_publication_disabled.store(
                    true, std::memory_order_release);
            }
            log_failure(
                "post-present RGBA8 reference audit failed; active rebind transaction is frozen or later raster publication is disabled");
        }
        log_rgba8_lifecycle_snapshot("post-present", top);
    } else if (present_faulted) {
        // The presenter may have consumed either the lookup reference or the
        // binder-local reference before faulting. No normal return boundary
        // exists from which to infer ownership, so permanently stop raster
        // publication for this process and retain every existing ledger bit.
        g_rgba8_publication_disabled.store(
            true, std::memory_order_release);
        g_pending_aliased_icon = {};
        g_resolving_icon_alias = false;
        const bool refs_exact = rgba8_references_match_tracked_hosts(
            "present-fault");
        log_rgba8_lifecycle_snapshot("present-fault", top);
        log_failure(
            "%s repaint ownership became uncertain after presenter SEH (physical=%zu logical=%zu refsExact=%u); later RGBA8 publication is disabled",
            layer_name,
            physical,
            logical,
            refs_exact ? 1U : 0U);
    }
    g_repaint_context = previous_context;
    // PresentTab consumes the embedded Scaleform value. On an exception the
    // value's ownership is uncertain; deliberately leak this research-only
    // reference rather than risk a double release.
    proxy.constructed = false;
    return presented;
}

bool repaint_tab_layer(
    void* top,
    const char* layer_name,
    const TabViewport& viewport,
    std::array<ResolvedTabProxy, kPhysicalTabCapacity>& proxies) noexcept {
    if (!top || !layer_name || !g_present_tab) {
        destroy_proxies(proxies);
        return false;
    }
    for (std::size_t physical = 0;
         physical < active_physical_tab_count();
         ++physical) {
        if (g_in_place_rgba8_rebind_transaction.active &&
            g_in_place_rgba8_rebind_transaction.failed) {
            destroy_proxies(proxies);
            return false;
        }
        if (!repaint_tab_cell(
                top, layer_name, physical, viewport, proxies[physical])) {
            destroy_proxies(proxies);
            return false;
        }
        // A correlated image-bind failure can occur after the native
        // presenter has consumed this cell successfully. Stop before touching
        // the next cell: continuing would turn a fail-closed research result
        // into additional unproven display-tree mutations.
        if (g_in_place_rgba8_rebind_transaction.active &&
            g_in_place_rgba8_rebind_transaction.failed) {
            destroy_proxies(proxies);
            return false;
        }
    }
    return true;
}

bool repaint_tab_layers(
    void* top,
    const TabViewport& viewport,
    ResolvedTabLayers& layers) noexcept {
    if (!repaint_tab_layer(
            top, "TabList", viewport, layers.tab_list)) {
        destroy_proxies(layers.back_tab_list);
        return false;
    }
    if (!repaint_tab_layer(
            top, "BackTabList", viewport, layers.back_tab_list)) {
        return false;
    }
    return true;
}

bool invoke_selector_select(
    void* top,
    std::uint32_t physical) noexcept {
    if (!top || !g_selector_select ||
        physical >= active_physical_tab_count()) {
        return false;
    }
    bool selected{};
#if defined(_MSC_VER)
    __try {
#endif
        selected = g_selector_select(
            static_cast<std::byte*>(top) + kSelectorOffset,
            physical,
            1);
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        selected = false;
        log_failure(
            "selector relocation faulted (seh=0x%08lX)",
            static_cast<unsigned long>(GetExceptionCode()));
    }
#endif
    return selected;
}
const char* panel_state_isolation_reason_name(
    PanelStateIsolationReason reason) noexcept;

constexpr std::size_t kNativeGraphicsLogical = 7;
[[nodiscard]] std::size_t native_graphics_route() noexcept {
    return active_custom_tab_count();
}

bool logical_uses_graphics_carrier(
    std::size_t logical) noexcept {
    return logical == kNativeGraphicsLogical ||
        (logical >= kStockLogicalTabCount &&
            logical < active_logical_tab_count());
}

bool is_custom_logical(std::size_t logical) noexcept {
    return logical >= kStockLogicalTabCount &&
        logical < active_logical_tab_count();
}

std::size_t graphics_route_for_logical(
    std::size_t logical) noexcept {
    return logical == kNativeGraphicsLogical
        ? native_graphics_route()
        : logical - kStockLogicalTabCount;
}

void invoke_switch_panel(
    void* top,
    std::int32_t category,
    PanelStateIsolationReason isolation_reason) noexcept {
    if (!top || !g_switch_panel) return;
    const bool isolation_armed = begin_panel_state_isolation(
        top, category, isolation_reason);
#if defined(_MSC_VER)
    __try {
#endif
        g_research_explicit_switch = true;
        g_research_switch_succeeded = false;
        g_switch_panel(
            static_cast<std::byte*>(top) + kCompositeOffset,
            category);
        g_research_explicit_switch = false;
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_research_explicit_switch = false;
        g_research_failed = true;
        log_failure(
            "panel correction faulted for category=%d (seh=0x%08lX)",
            category,
            static_cast<unsigned long>(GetExceptionCode()));
    }
#endif
    end_panel_state_isolation(isolation_armed);
}

void invoke_refresh_title(void* top) noexcept {
    if (!top || !g_refresh_title) return;
#if defined(_MSC_VER)
    __try {
#endif
        g_refresh_title(top);
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        log_failure(
            "title refresh faulted (seh=0x%08lX)",
            static_cast<unsigned long>(GetExceptionCode()));
    }
#endif
}

bool restore_previous_mapping(
    void* top,
    const std::array<RecordFields, kPhysicalTabCapacity>& previous,
    bool allow_vector_only_epoch_during_retirement = false) noexcept {
    const InjectedIconsClearResult clear_result = clear_injected_icons(
        top, allow_vector_only_epoch_during_retirement);
    const bool overlays_cleared =
        allow_vector_only_epoch_during_retirement
        ? accept_vector_only_epoch_clear_result(
              top, clear_result, "initial-mapping-rollback")
        : clear_result == InjectedIconsClearResult::cleared;
    const bool records_restored = overlays_cleared &&
        write_record_fields(top, previous);
    ResolvedTabLayers rollback_layers{};
    const bool visuals_restored = records_restored &&
        resolve_tab_layers(top, rollback_layers) &&
        repaint_tab_layers(top, g_runtime.viewport, rollback_layers);
    if (!overlays_cleared || !records_restored || !visuals_restored) {
        destroy_tab_layers(rollback_layers);
        log_failure(
            "record/visual rollback after slot remapping could not be completed (overlays=%u records=%u visuals=%u)",
            overlays_cleared ? 1u : 0u,
            records_restored ? 1u : 0u,
            visuals_restored ? 1u : 0u);
        return false;
    }
    return true;
}

[[nodiscard]] bool derive_injected_host_layout(
    const TabViewport& viewport,
    InjectedHostLayout& output) noexcept {
    output = {};
    if (!viewport.initialized() ||
        viewport.visible_slot_count() != active_physical_tab_count()) {
        return false;
    }

    for (std::size_t physical = 0;
         physical < active_physical_tab_count();
         ++physical) {
        std::size_t logical{invalid_tab_index};
        if (!viewport.logical_for_physical(physical, logical) ||
            logical >= active_logical_tab_count()) {
            return false;
        }

        bool vector = tab_overflow_indicator(viewport, physical) !=
            TabOverflowIndicator::none;
        std::size_t asset{};
        if (!vector && logical >= kStockLogicalTabCount) {
            const std::size_t custom = logical - kStockLogicalTabCount;
            if (custom >= active_custom_tab_count()) {
                return false;
            }
            if (catalogue_icon_has_effective_raster(custom)) {
                // Zero remains the empty-host sentinel. The encoded value is
                // size_t so every public logical-tab index is representable.
                asset = custom + 1U;
            } else {
                vector = true;
            }
        }

        for (std::size_t layer = 0; layer < 2; ++layer) {
            const std::size_t bit =
                physical + layer * active_physical_tab_count();
            const TabHostMask mask = TabHostMask{1} << bit;
            if (vector) {
                output.vector_mask |= mask;
                output.injected_mask |= mask;
            } else if (asset != 0) {
                output.rgba_mask |= mask;
                output.injected_mask |= mask;
                output.assets[bit] = asset;
            }
        }
    }
    return output.injected_mask ==
            (output.rgba_mask | output.vector_mask) &&
        (output.rgba_mask & output.vector_mask) == 0;
}

// Source ownership must come from the committed display-tree ledger, not by
// re-deriving the old viewport with the current publication gate.  Native
// RGBA8 resources are created after the initial tab-strip paint; readiness can
// therefore change while that old viewport is still showing vector fallbacks.
// Reclassifying those already-painted hosts as raster would make the first
// real remap disagree with the Shapes that actually exist.
[[nodiscard]] bool snapshot_injected_host_layout(
    InjectedHostLayout& output) noexcept {
    output = {};
    output.injected_mask = g_injected_icon_hosts.load(
        std::memory_order_acquire);
    output.rgba_mask = g_rgba8_icon_hosts.load(
        std::memory_order_acquire);
    if ((output.rgba_mask & ~output.injected_mask) != 0) return false;
    output.vector_mask = output.injected_mask & ~output.rgba_mask;

    for (std::size_t bit = 0; bit < active_tab_host_count(); ++bit) {
        const TabHostMask mask = TabHostMask{1} << bit;
        const std::size_t encoded = g_rgba8_host_assets[bit].load(
            std::memory_order_acquire);
        if ((output.rgba_mask & mask) == 0) {
            if (encoded != 0) return false;
            continue;
        }
        if (encoded == 0) return false;
        const std::size_t asset = static_cast<std::size_t>(encoded - 1U);
        if (asset >= active_custom_tab_count() ||
            !catalogue_icon_has_retained_raster(asset)) {
            return false;
        }
        output.assets[bit] = encoded;
    }
    return output.injected_mask ==
            (output.rgba_mask | output.vector_mask) &&
        (output.rgba_mask & output.vector_mask) == 0;
}

[[nodiscard]] bool same_injected_host_layout(
    const InjectedHostLayout& left,
    const InjectedHostLayout& right) noexcept {
    if (left.injected_mask != right.injected_mask ||
        left.rgba_mask != right.rgba_mask ||
        left.vector_mask != right.vector_mask) {
        return false;
    }
    for (std::size_t bit = 0; bit < active_tab_host_count(); ++bit) {
        if (left.assets[bit] != right.assets[bit]) return false;
    }
    return true;
}

[[nodiscard]] bool exact_initial_catalogue_upgrade(
    const InjectedHostLayout& source,
    const InjectedHostLayout& target) noexcept {
    const bool topology_exact = source.rgba_mask == 0 &&
        target.rgba_mask != 0 &&
        source.injected_mask == target.injected_mask &&
        source.vector_mask ==
            (target.vector_mask | target.rgba_mask) &&
        (target.vector_mask & target.rgba_mask) == 0;
    if (!topology_exact) return false;

    for (std::size_t bit = 0; bit < active_tab_host_count(); ++bit) {
        const TabHostMask mask = TabHostMask{1} << bit;
        if (source.assets[bit] != 0 ||
            (((target.rgba_mask & mask) != 0) !=
                (target.assets[bit] != 0))) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool pending_requests_catalogue_presentation_refresh(
    void* top,
    const TabViewport& viewport) noexcept {
    const PendingTransition& pending = g_runtime.pending;
    return pending.active && pending.presentation_refresh &&
        pending.top == top && g_runtime.active && g_runtime.top == top &&
        pending.candidate.mapping_generation() ==
            viewport.mapping_generation() &&
        pending.candidate.window_start() == viewport.window_start() &&
        pending.candidate.selected_logical() ==
            viewport.selected_logical();
}

[[nodiscard]] bool shape_replacement_plan(
    void* top,
    const TabViewport& viewport,
    Rgba8ReplacementPlan& output) noexcept {
    const bool catalogue_presentation_refresh =
        pending_requests_catalogue_presentation_refresh(top, viewport);
    const bool transition_required = g_runtime.active &&
        g_runtime.top == top &&
        (catalogue_presentation_refresh ||
            tab_overflow_indicators_changed(g_runtime.viewport, viewport) ||
            viewport.mapping_generation() !=
                g_runtime.viewport.mapping_generation());
    if (!transition_required ||
        g_runtime.completed_shape_replacement_transitions ==
            std::numeric_limits<std::uint64_t>::max()) {
        return false;
    }

    InjectedHostLayout source{};
    InjectedHostLayout target{};
    if (!snapshot_injected_host_layout(source) ||
        !derive_injected_host_layout(viewport, target)) {
        return false;
    }

    if (catalogue_presentation_refresh) {
        // This deferred first-epoch repair is deliberately narrower than a
        // generic same-mapping repaint. Before catalogue publication every
        // injected host is a vector Shape. Afterwards, only visible custom
        // hosts may become RGBA8; overflow markers remain vectors. Refuse any
        // other ownership topology before touching the display tree.
        if (!exact_initial_catalogue_upgrade(source, target)) return false;
    }

    output = {};
    output.transition_sequence =
        g_runtime.completed_shape_replacement_transitions + 1U;
    output.source_window = g_runtime.viewport.window_start();
    output.target_window = viewport.window_start();
    output.source_injected_mask = source.injected_mask;
    output.source_rgba_mask = source.rgba_mask;
    output.source_vector_mask = source.vector_mask;
    output.target_injected_mask = target.injected_mask;
    output.target_rgba_mask = target.rgba_mask;
    output.target_vector_mask = target.vector_mask;
    // Preserve a Shape only when the same physical host keeps the exact same
    // presentation kind and, for raster content, the exact immutable asset.
    // Everything else is staged in the inactive property bank before its
    // source bank is retired.
    for (std::size_t bit = 0; bit < active_tab_host_count(); ++bit) {
        const TabHostMask mask = TabHostMask{1} << bit;
        const bool same_vector = (source.vector_mask & mask) != 0 &&
            (target.vector_mask & mask) != 0;
        const bool same_raster = (source.rgba_mask & mask) != 0 &&
            (target.rgba_mask & mask) != 0 &&
            source.assets[bit] != 0 &&
            source.assets[bit] == target.assets[bit];
        if (same_vector || same_raster) {
            output.preserved_host_mask |= mask;
        }
    }
    output.raster_detach_mask =
        source.rgba_mask & ~output.preserved_host_mask;
    output.vector_detach_mask =
        source.vector_mask & ~output.preserved_host_mask;
    output.expected_detach_mask =
        source.injected_mask & ~output.preserved_host_mask;
    output.retired_injected_mask = output.preserved_host_mask;
    output.fresh_host_mask =
        target.injected_mask & ~output.preserved_host_mask;
    output.audit_mask = active_tab_host_mask();
    output.source_assets = source.assets;
    output.target_assets = target.assets;
    return true;
}

bool apply_mapping(
    void* top,
    const TabViewport& viewport,
    ResolvedTabLayers& layers,
    bool allow_transparent_initial_raster_placeholder) noexcept {
    std::array<RecordFields, kPhysicalTabCapacity> previous{};
    std::array<RecordFields, kPhysicalTabCapacity> desired{};
    if (!snapshot_record_fields(top, previous) ||
        !build_mapping_fields(viewport, desired)) {
        destroy_tab_layers(layers);
        return false;
    }

    Rgba8ReplacementPlan replacement_plan{};
    const bool transition_required = g_runtime.active &&
        g_runtime.top == top &&
        (pending_requests_catalogue_presentation_refresh(top, viewport) ||
            tab_overflow_indicators_changed(g_runtime.viewport, viewport) ||
            viewport.mapping_generation() !=
                g_runtime.viewport.mapping_generation());
    const bool shape_replacement_transition =
        shape_replacement_plan(top, viewport, replacement_plan);
    if (transition_required && !shape_replacement_transition) {
        destroy_tab_layers(layers);
        log_failure(
            "viewport mutation required a fresh-Shape transaction, but no exact transition plan could be derived; no record or display mutation was attempted");
        return false;
    }
    if (shape_replacement_transition) {
        InPlaceRgba8RebindTransaction& transaction =
            g_in_place_rgba8_rebind_transaction;
        if (!transaction.active) {
            const std::uint64_t phase_a_started_qpc =
                query_performance_ticks();
            const TabHostMask injected = g_injected_icon_hosts.load(
                std::memory_order_acquire);
            const TabHostMask rgba = g_rgba8_icon_hosts.load(
                std::memory_order_acquire);
            bool assets_exact = true;
            for (std::size_t bit = 0;
                 bit < active_tab_host_count();
                 ++bit) {
                const std::size_t expected =
                    replacement_plan.source_assets[bit];
                assets_exact = assets_exact &&
                    g_rgba8_host_assets[bit].load(
                        std::memory_order_acquire) == expected;
            }
            bool debts_zero = all_rgba8_retirement_debts_zero();
            InjectedMovieFingerprint current_fingerprint{};
            const bool fingerprint_sampled = sample_current_movie_fingerprint(
                top, current_fingerprint);
            const bool fingerprint_unset =
                !g_injected_movie_fingerprint.object_interface &&
                !g_injected_movie_fingerprint.owner_link &&
                !g_injected_movie_fingerprint.movie_root;
            if (fingerprint_sampled && fingerprint_unset &&
                replacement_plan.source_injected_mask == 0) {
                g_injected_movie_fingerprint = current_fingerprint;
            }
            const bool fingerprint_exact = fingerprint_sampled &&
                same_movie_fingerprint(
                    current_fingerprint,
                    g_injected_movie_fingerprint);
            const bool refs_exact =
                reconcile_rgba8_retirement_debt(
                    "shape-replacement-source-preflight") &&
                rgba8_references_match_tracked_hosts(
                    "shape-replacement-source-preflight");
            debts_zero = all_rgba8_retirement_debts_zero();
            const bool staging_empty =
                g_staged_injected_icon_hosts.load(
                    std::memory_order_acquire) == 0 &&
                g_staged_rgba8_icon_hosts.load(
                    std::memory_order_acquire) == 0;
            bool staged_assets_empty = true;
            for (std::size_t bit = 0;
                 bit < active_tab_host_count();
                 ++bit) {
                staged_assets_empty = staged_assets_empty &&
                    g_staged_rgba8_host_assets[bit].load(
                        std::memory_order_acquire) == 0;
            }
            const bool publication_state_compatible =
                !g_rgba8_publication_disabled.load(
                    std::memory_order_acquire) ||
                (replacement_plan.source_rgba_mask |
                    replacement_plan.target_rgba_mask) == 0;
            const bool source_ledger_exact =
                injected == replacement_plan.source_injected_mask &&
                rgba == replacement_plan.source_rgba_mask && assets_exact &&
                staging_empty && staged_assets_empty &&
                fingerprint_exact && refs_exact &&
                publication_state_compatible;
            TabHostMask property_state_mask{};
            if (source_ledger_exact) {
                const auto preflight = [&property_state_mask, top](
                    TabLayer layer,
                    std::size_t physical,
                    InPlaceRebindHostExpectation expectation,
                    std::size_t expected_asset = 0) noexcept {
                    const std::size_t bit = physical +
                        (layer == TabLayer::back
                            ? active_physical_tab_count()
                            : 0);
                    const std::uint8_t active_bank =
                        active_icon_property_bank(bit);
                    const bool exact = preflight_in_place_rgba8_rebind_host(
                            top,
                            layer,
                            physical,
                            expectation,
                            expected_asset,
                            active_bank) &&
                        preflight_in_place_rgba8_rebind_host(
                            top,
                            layer,
                            physical,
                            InPlaceRebindHostExpectation::untracked,
                            0,
                            static_cast<std::uint8_t>(active_bank ^ 1U),
                            false);
                    if (exact) {
                        property_state_mask |= TabHostMask{1} << bit;
                    }
                };
                for (std::size_t bit = 0;
                     bit < active_tab_host_count();
                     ++bit) {
                    const TabHostMask mask = TabHostMask{1} << bit;
                    if ((replacement_plan.audit_mask & mask) == 0) continue;
                    const TabLayer layer = bit < active_physical_tab_count()
                        ? TabLayer::front
                        : TabLayer::back;
                    const std::size_t physical =
                        bit % active_physical_tab_count();
                    if ((replacement_plan.source_rgba_mask & mask) != 0) {
                        preflight(
                            layer,
                            physical,
                            InPlaceRebindHostExpectation::rgba8_shape,
                            replacement_plan.source_assets[bit]);
                    } else if ((replacement_plan.source_vector_mask & mask) !=
                        0) {
                        preflight(
                            layer,
                            physical,
                            InPlaceRebindHostExpectation::vector_shape);
                    } else {
                        preflight(
                            layer,
                            physical,
                            InPlaceRebindHostExpectation::untracked);
                    }
                }
            }
            const bool property_states_exact =
                property_state_mask == replacement_plan.audit_mask;
            if (!source_ledger_exact || !property_states_exact) {
                destroy_tab_layers(layers);
                log_failure(
                    "generic RGBA8 replacement transition %llu window %zu->%zu source preflight failed; no record or display mutation was attempted (injected=0x%04X expected=0x%04X rgba=0x%04X expected=0x%04X assets=%u stagingEmpty=%u debtsZero=%u fingerprint=%u refs=%u sourceLedger=%u propertyStates=0x%04X/0x%04X)",
                    static_cast<unsigned long long>(
                        replacement_plan.transition_sequence),
                    replacement_plan.source_window,
                    replacement_plan.target_window,
                    static_cast<unsigned>(injected),
                    static_cast<unsigned>(replacement_plan.source_injected_mask),
                    static_cast<unsigned>(rgba),
                    static_cast<unsigned>(replacement_plan.source_rgba_mask),
                    assets_exact ? 1U : 0U,
                    (staging_empty && staged_assets_empty) ? 1U : 0U,
                    debts_zero ? 1U : 0U,
                    fingerprint_exact ? 1U : 0U,
                    refs_exact ? 1U : 0U,
                    source_ledger_exact ? 1U : 0U,
                    static_cast<unsigned>(property_state_mask),
                    static_cast<unsigned>(replacement_plan.audit_mask));
                return false;
            }

            transaction = {};
            transaction.active = true;
            transaction.transition_sequence =
                replacement_plan.transition_sequence;
            transaction.stage = InPlaceRgba8RebindStage::clearing;
            transaction.top = top;
            transaction.presentation_epoch =
                g_runtime.presentation_epoch;
            transaction.source_generation =
                g_runtime.viewport.mapping_generation();
            transaction.target_generation = viewport.mapping_generation();
            transaction.source_window =
                g_runtime.viewport.window_start();
            transaction.target_window = viewport.window_start();
            transaction.clear_frame = g_composite_frame_sequence;
            transaction.requested_qpc = g_runtime.pending.requested_qpc != 0
                ? g_runtime.pending.requested_qpc
                : phase_a_started_qpc;
            transaction.phase_a_started_qpc = phase_a_started_qpc;
            transaction.expected_clear_mask =
                replacement_plan.expected_detach_mask;
            transaction.expected_raster_clear_mask =
                replacement_plan.raster_detach_mask;
            transaction.expected_vector_clear_mask =
                replacement_plan.vector_detach_mask;
            transaction.source_injected_mask =
                replacement_plan.source_injected_mask;
            transaction.source_rgba_mask = replacement_plan.source_rgba_mask;
            transaction.retired_injected_mask =
                replacement_plan.retired_injected_mask;
            transaction.target_injected_mask =
                replacement_plan.target_injected_mask;
            transaction.target_rgba_mask = replacement_plan.target_rgba_mask;
            transaction.audit_mask = replacement_plan.audit_mask;
            transaction.expected_host_mask =
                replacement_plan.fresh_host_mask;
            transaction.expected_rgba_presentation_mask =
                replacement_plan.target_rgba_mask;
            transaction.expected_vector_host_mask =
                replacement_plan.target_vector_mask &
                replacement_plan.fresh_host_mask;
            transaction.preserved_host_mask =
                replacement_plan.preserved_host_mask;
            transaction.source_assets = replacement_plan.source_assets;
            transaction.target_assets = replacement_plan.target_assets;

            // Debt from an earlier, already-committed bank may outlive its
            // display object for a renderer frame.  Wait before staging a new
            // overlap, but leave every current source Shape attached and
            // visible throughout that wait.
            if (!debts_zero) {
                transaction.stage =
                    InPlaceRgba8RebindStage::waiting_retirement;
                if (!destroy_tab_layers(layers)) {
                    transaction.failed = true;
                    return false;
                }
                log_line(
                    "rgba8Handoff transition=%llu waiting for prior renderer debt with source banks still visible",
                    static_cast<unsigned long long>(
                        transaction.transition_sequence));
                return true;
            }

            // No display object has been detached.  The target repaint will
            // attach every changed target into the inactive bank, above the
            // still-visible source, before source retirement begins.
            transaction.detach_completed_qpc =
                query_performance_ticks();
            transaction.retirement_completed_qpc =
                transaction.detach_completed_qpc;
            transaction.stage = InPlaceRgba8RebindStage::ready_to_repaint;
            log_line(
                "rgba8Handoff transition=%llu source preserved; atomic inactive-bank target repaint armed",
                static_cast<unsigned long long>(
                    transaction.transition_sequence));
        }

        const bool matching_ready_transaction = transaction.active &&
            !transaction.failed &&
            transaction.stage ==
                InPlaceRgba8RebindStage::ready_to_repaint &&
            transaction.top == top &&
            transaction.presentation_epoch ==
                g_runtime.presentation_epoch &&
            transaction.source_generation ==
                g_runtime.viewport.mapping_generation() &&
            transaction.target_generation == viewport.mapping_generation() &&
            transaction.source_window ==
                g_runtime.viewport.window_start() &&
            transaction.target_window == viewport.window_start() &&
            transaction.completed_clear_mask == 0;
        const TabHostMask injected = g_injected_icon_hosts.load(
            std::memory_order_acquire);
        const TabHostMask rgba = g_rgba8_icon_hosts.load(
            std::memory_order_acquire);
        bool source_assets_exact = true;
        bool staged_assets_empty = true;
        for (std::size_t bit = 0; bit < active_tab_host_count(); ++bit) {
            source_assets_exact = source_assets_exact &&
                g_rgba8_host_assets[bit].load(
                    std::memory_order_acquire) ==
                    transaction.source_assets[bit];
            staged_assets_empty = staged_assets_empty &&
                g_staged_rgba8_host_assets[bit].load(
                    std::memory_order_acquire) == 0;
        }
        const bool debts_zero = all_rgba8_retirement_debts_zero();
        InjectedMovieFingerprint current_fingerprint{};
        const bool fingerprint_exact =
            sample_current_movie_fingerprint(top, current_fingerprint) &&
            same_movie_fingerprint(
                current_fingerprint,
                g_injected_movie_fingerprint);
        const bool refs_exact = debts_zero &&
            rgba8_references_match_tracked_hosts(
                "shape-replacement-ready");
        const bool publication_state_compatible =
            !g_rgba8_publication_disabled.load(
                std::memory_order_acquire) ||
            (transaction.source_rgba_mask |
                transaction.target_rgba_mask) == 0;
        const bool ready_ledger_exact = matching_ready_transaction &&
            injected == transaction.source_injected_mask &&
            rgba == transaction.source_rgba_mask && source_assets_exact &&
            g_staged_injected_icon_hosts.load(
                std::memory_order_acquire) == 0 &&
            g_staged_rgba8_icon_hosts.load(
                std::memory_order_acquire) == 0 && staged_assets_empty &&
            fingerprint_exact && refs_exact &&
            publication_state_compatible;
        TabHostMask property_state_mask{};
        if (ready_ledger_exact) {
            const auto preflight = [&property_state_mask, top](
                TabLayer layer,
                std::size_t physical,
                InPlaceRebindHostExpectation expectation,
                std::size_t expected_asset = 0) noexcept {
                const std::size_t bit = physical +
                    (layer == TabLayer::back
                        ? active_physical_tab_count()
                        : 0);
                const std::uint8_t active_bank =
                    active_icon_property_bank(bit);
                const bool exact = preflight_in_place_rgba8_rebind_host(
                        top,
                        layer,
                        physical,
                        expectation,
                        expected_asset,
                        active_bank) &&
                    preflight_in_place_rgba8_rebind_host(
                        top,
                        layer,
                        physical,
                        InPlaceRebindHostExpectation::untracked,
                        0,
                        static_cast<std::uint8_t>(active_bank ^ 1U),
                        false);
                if (exact) {
                    property_state_mask |= TabHostMask{1} << bit;
                }
            };
            for (std::size_t bit = 0;
                 bit < active_tab_host_count();
                 ++bit) {
                const TabHostMask mask = TabHostMask{1} << bit;
                if ((transaction.audit_mask & mask) == 0) continue;
                const TabLayer layer = bit < active_physical_tab_count()
                    ? TabLayer::front
                    : TabLayer::back;
                const std::size_t physical =
                    bit % active_physical_tab_count();
                if ((transaction.source_rgba_mask & mask) != 0) {
                    preflight(
                        layer,
                        physical,
                        InPlaceRebindHostExpectation::rgba8_shape,
                        transaction.source_assets[bit]);
                } else if ((transaction.source_injected_mask & mask) != 0) {
                    preflight(
                        layer,
                        physical,
                        InPlaceRebindHostExpectation::vector_shape);
                } else {
                    preflight(
                        layer,
                        physical,
                        InPlaceRebindHostExpectation::untracked);
                }
            }
        }
        if (!ready_ledger_exact ||
            property_state_mask != transaction.audit_mask) {
            transaction.failed = true;
            g_rgba8_publication_disabled.store(
                true, std::memory_order_release);
            g_runtime.destructive_transition_failed = true;
            destroy_tab_layers(layers);
            log_failure(
                "atomic RGBA8 handoff preflight failed (ready:%u injected:0x%04X rgba:0x%04X sourceAssets:%u stagedEmpty:%u debtsZero:%u fingerprint:%u refs:%u properties:0x%04X/0x%04X)",
                matching_ready_transaction ? 1U : 0U,
                static_cast<unsigned>(injected),
                static_cast<unsigned>(rgba),
                source_assets_exact ? 1U : 0U,
                staged_assets_empty ? 1U : 0U,
                debts_zero ? 1U : 0U,
                fingerprint_exact ? 1U : 0U,
                refs_exact ? 1U : 0U,
                static_cast<unsigned>(property_state_mask),
                static_cast<unsigned>(transaction.audit_mask));
            return false;
        }

        transaction.repaint_started_qpc = query_performance_ticks();
        transaction.stage = InPlaceRgba8RebindStage::repainting;
        if (!write_record_fields(top, desired)) {
            transaction.failed = true;
            g_rgba8_publication_disabled.store(
                true, std::memory_order_release);
            g_runtime.destructive_transition_failed = true;
            destroy_tab_layers(layers);
            log_failure(
                "fresh-Shape RGBA8 replacement could not publish its target records after retirement; later remaps are frozen");
            return false;
        }

        const bool repainted = repaint_tab_layers(top, viewport, layers);
        // At this point every changed custom target is attached in the
        // inactive bank and is above the still-visible source Shape.  Prove
        // the complete staged set before removing a single source child.
        const TabHostMask expected_staged_rgba =
            transaction.target_rgba_mask & transaction.expected_host_mask;
        bool staged_assets_exact = true;
        for (std::size_t bit = 0; bit < active_tab_host_count(); ++bit) {
            const TabHostMask mask = TabHostMask{1} << bit;
            const std::size_t expected =
                (expected_staged_rgba & mask) != 0
                ? transaction.target_assets[bit]
                : 0;
            staged_assets_exact = staged_assets_exact &&
                g_staged_rgba8_host_assets[bit].load(
                    std::memory_order_acquire) == expected;
        }
        const bool staged_exact = repainted && !transaction.failed &&
            g_staged_injected_icon_hosts.load(
                std::memory_order_acquire) ==
                transaction.expected_host_mask &&
            g_staged_rgba8_icon_hosts.load(
                std::memory_order_acquire) == expected_staged_rgba &&
            staged_assets_exact &&
            transaction.completed_host_mask ==
                transaction.expected_host_mask &&
            transaction.completed_rgba_presentation_mask ==
                transaction.expected_rgba_presentation_mask &&
            transaction.completed_vector_host_mask ==
                transaction.expected_vector_host_mask;

        bool handoff_exact = staged_exact;
        for (std::size_t bit = 0;
             bit < active_tab_host_count() && handoff_exact;
             ++bit) {
            const TabHostMask mask = TabHostMask{1} << bit;
            if ((transaction.expected_vector_clear_mask & mask) == 0) {
                continue;
            }
            const TabLayer layer = bit < active_physical_tab_count()
                ? TabLayer::front
                : TabLayer::back;
            const bool target_is_injected =
                (transaction.target_injected_mask & mask) != 0;
            handoff_exact = detach_vector_host_for_replacement(
                top,
                layer,
                bit % active_physical_tab_count(),
                transaction,
                !target_is_injected);
        }
        for (std::size_t bit = 0;
             bit < active_tab_host_count() && handoff_exact;
             ++bit) {
            const TabHostMask mask = TabHostMask{1} << bit;
            if ((transaction.expected_raster_clear_mask & mask) == 0) {
                continue;
            }
            const TabLayer layer = bit < active_physical_tab_count()
                ? TabLayer::front
                : TabLayer::back;
            const bool target_is_injected =
                (transaction.target_injected_mask & mask) != 0;
            handoff_exact = detach_rgba8_host_for_replacement(
                top,
                layer,
                bit % active_physical_tab_count(),
                transaction,
                !target_is_injected);
        }

        // Publish inactive banks only after all old banks were removed.  The
        // bank bit and ownership ledgers change together inside this callback;
        // no renderer boundary can observe an authored-icon-only state.
        if (handoff_exact && transaction.completed_clear_mask ==
                transaction.expected_clear_mask) {
            TabHostMask bank_b = g_icon_bank_b_hosts.load(
                std::memory_order_acquire);
            for (std::size_t bit = 0;
                 bit < active_tab_host_count();
                 ++bit) {
                const TabHostMask mask = TabHostMask{1} << bit;
                if ((transaction.expected_host_mask & mask) == 0) continue;
                const std::uint8_t old_bank = active_icon_property_bank(bit);
                if ((old_bank ^ 1U) == kIconPropertyBankB) {
                    bank_b |= mask;
                } else {
                    bank_b &= ~mask;
                }
                g_injected_icon_hosts.fetch_or(
                    mask, std::memory_order_acq_rel);
                if ((expected_staged_rgba & mask) != 0) {
                    g_rgba8_icon_hosts.fetch_or(
                        mask, std::memory_order_acq_rel);
                    g_rgba8_host_assets[bit].store(
                        transaction.target_assets[bit],
                        std::memory_order_release);
                }
                g_staged_injected_icon_hosts.fetch_and(
                    ~mask,
                    std::memory_order_acq_rel);
                g_staged_rgba8_icon_hosts.fetch_and(
                    ~mask,
                    std::memory_order_acq_rel);
                g_staged_rgba8_host_assets[bit].store(
                    0, std::memory_order_release);
            }
            g_icon_bank_b_hosts.store(bank_b, std::memory_order_release);
        } else {
            handoff_exact = false;
        }
        transaction.detach_completed_qpc = query_performance_ticks();

        TabHostMask postflight_property_mask{};
        if (handoff_exact) {
            const auto postflight = [&](
                TabLayer layer,
                std::size_t physical,
                InPlaceRebindHostExpectation expectation,
                std::size_t expected_asset = 0) noexcept {
                const std::size_t bit = physical +
                    (layer == TabLayer::back
                        ? active_physical_tab_count()
                        : 0);
                const std::uint8_t active_bank =
                    active_icon_property_bank(bit);
                const bool exact = preflight_in_place_rgba8_rebind_host(
                        top,
                        layer,
                        physical,
                        expectation,
                        expected_asset,
                        active_bank) &&
                    preflight_in_place_rgba8_rebind_host(
                        top,
                        layer,
                        physical,
                        InPlaceRebindHostExpectation::untracked,
                        0,
                        static_cast<std::uint8_t>(active_bank ^ 1U),
                        false);
                if (exact) {
                    postflight_property_mask |= TabHostMask{1} << bit;
                }
            };
            for (std::size_t bit = 0;
                 bit < active_tab_host_count();
                 ++bit) {
                const TabHostMask mask = TabHostMask{1} << bit;
                if ((transaction.audit_mask & mask) == 0) continue;
                const TabLayer layer = bit < active_physical_tab_count()
                    ? TabLayer::front
                    : TabLayer::back;
                const std::size_t physical =
                    bit % active_physical_tab_count();
                if ((transaction.target_rgba_mask & mask) != 0) {
                    postflight(
                        layer,
                        physical,
                        InPlaceRebindHostExpectation::rgba8_shape,
                        transaction.target_assets[bit]);
                } else if ((transaction.target_injected_mask & mask) != 0) {
                    postflight(
                        layer,
                        physical,
                        InPlaceRebindHostExpectation::vector_shape);
                } else {
                    postflight(
                        layer,
                        physical,
                        InPlaceRebindHostExpectation::untracked);
                }
            }
        }
        const InPlaceRgba8RebindTransaction completed =
            g_in_place_rgba8_rebind_transaction;
        g_in_place_rgba8_rebind_transaction = {};
        const TabHostMask target_injected =
            g_injected_icon_hosts.load(std::memory_order_acquire);
        const TabHostMask target_rgba = g_rgba8_icon_hosts.load(
            std::memory_order_acquire);
        bool target_assets_exact = true;
        for (std::size_t bit = 0;
             bit < active_tab_host_count();
             ++bit) {
            const std::size_t expected = completed.target_assets[bit];
            target_assets_exact = target_assets_exact &&
                g_rgba8_host_assets[bit].load(std::memory_order_acquire) ==
                    expected;
        }
        const bool final_refs_exact =
            rgba8_references_match_tracked_hosts(
                "shape-replacement-final");
        const bool transition_complete = handoff_exact && !completed.failed &&
            completed.transition_sequence ==
                g_runtime.completed_shape_replacement_transitions + 1U &&
            completed.completed_clear_mask ==
                completed.expected_clear_mask &&
            completed.completed_host_mask ==
                completed.expected_host_mask &&
            completed.completed_rgba_presentation_mask ==
                completed.expected_rgba_presentation_mask &&
            completed.completed_vector_host_mask ==
                completed.expected_vector_host_mask &&
            target_injected == completed.target_injected_mask &&
            target_rgba == completed.target_rgba_mask &&
            target_assets_exact && final_refs_exact &&
            postflight_property_mask ==
                completed.audit_mask;
        if (transition_complete) {
            g_runtime.completed_shape_replacement_transitions =
                completed.transition_sequence;
            if (completed.target_injected_mask == 0) {
                g_injected_movie_fingerprint = {};
            }
        } else {
            // A destructive detach or target repaint may already have changed
            // display-tree ownership. Preserve the target records/ledgers and
            // reject every later remap rather than attempting an unsafe undo.
            g_runtime.destructive_transition_failed = true;
        }
        const std::uint64_t transition_completed_qpc =
            query_performance_ticks();
        const bool timing_qpc_valid = completed.requested_qpc != 0 &&
            completed.phase_a_started_qpc >= completed.requested_qpc &&
            completed.retirement_completed_qpc >=
                completed.phase_a_started_qpc &&
            completed.repaint_started_qpc >=
                completed.retirement_completed_qpc &&
            completed.detach_completed_qpc >=
                completed.repaint_started_qpc &&
            transition_completed_qpc >= completed.detach_completed_qpc;
        const std::uint64_t transition_microseconds =
            elapsed_qpc_microseconds(
                completed.requested_qpc, transition_completed_qpc);
        const std::uint64_t queued_microseconds = elapsed_qpc_microseconds(
            completed.requested_qpc, completed.phase_a_started_qpc);
        const std::uint64_t phase_a_microseconds = elapsed_qpc_microseconds(
            completed.phase_a_started_qpc,
            completed.retirement_completed_qpc);
        const std::uint64_t retirement_microseconds =
            elapsed_qpc_microseconds(
                completed.detach_completed_qpc,
                transition_completed_qpc);
        const std::uint64_t ready_wait_microseconds =
            elapsed_qpc_microseconds(
                completed.retirement_completed_qpc,
                completed.repaint_started_qpc);
        const std::uint64_t repaint_microseconds = elapsed_qpc_microseconds(
            completed.repaint_started_qpc, completed.detach_completed_qpc);
        log_line(
            "STRESS transition sequence=%llu source={window:%zu generation:%llu injected:0x%04X rgba:0x%04X} target={window:%zu generation:%llu injected:0x%04X/0x%04X rgba:0x%04X/0x%04X} proof={detach:0x%04X/0x%04X fresh:0x%04X/0x%04X assets:%u refs:%u properties:0x%04X/0x%04X complete:%u frozen:%u} timingUs={valid:%u total:%llu queued:%llu phaseA:%llu retirement:%llu readyWait:%llu repaint:%llu frames:%llu}",
            static_cast<unsigned long long>(
                completed.transition_sequence),
            completed.source_window,
            static_cast<unsigned long long>(completed.source_generation),
            static_cast<unsigned>(completed.source_injected_mask),
            static_cast<unsigned>(completed.source_rgba_mask),
            completed.target_window,
            static_cast<unsigned long long>(completed.target_generation),
            static_cast<unsigned>(target_injected),
            static_cast<unsigned>(completed.target_injected_mask),
            static_cast<unsigned>(target_rgba),
            static_cast<unsigned>(completed.target_rgba_mask),
            static_cast<unsigned>(completed.completed_clear_mask),
            static_cast<unsigned>(completed.expected_clear_mask),
            static_cast<unsigned>(completed.completed_host_mask),
            static_cast<unsigned>(completed.expected_host_mask),
            target_assets_exact ? 1U : 0U,
            final_refs_exact ? 1U : 0U,
            static_cast<unsigned>(postflight_property_mask),
            static_cast<unsigned>(completed.audit_mask),
            transition_complete ? 1U : 0U,
            g_runtime.destructive_transition_failed ? 1U : 0U,
            timing_qpc_valid ? 1U : 0U,
            static_cast<unsigned long long>(transition_microseconds),
            static_cast<unsigned long long>(queued_microseconds),
            static_cast<unsigned long long>(phase_a_microseconds),
            static_cast<unsigned long long>(retirement_microseconds),
            static_cast<unsigned long long>(ready_wait_microseconds),
            static_cast<unsigned long long>(repaint_microseconds),
            static_cast<unsigned long long>(
                completed.retirement_wait_frames));
        if (!transition_complete) {
            g_rgba8_publication_disabled.store(
                true, std::memory_order_release);
            log_failure(
                "fresh-Shape RGBA8 replacement did not satisfy its final transition gate; target mapping was retained, later viewport remaps are frozen, and later RGBA8 publication is disabled");
        } else {
            log_line(
                "rgba8Replace transition %llu completed; arbitrary forward, reverse, presentation-only, and wrap remaps remain armed",
                static_cast<unsigned long long>(
                    completed.transition_sequence));
        }
        return true;
    }

    // Script-created children survive native timeline repainting. Remove every
    // tracked child while its current parent mapping is still valid, then let
    // the desired repaint recreate only the custom icons it actually needs.
    const InjectedIconsClearResult initial_clear_result =
        clear_injected_icons(
            top, allow_transparent_initial_raster_placeholder);
    const bool initial_clear_accepted =
        allow_transparent_initial_raster_placeholder
        ? accept_vector_only_epoch_clear_result(
              top, initial_clear_result, "initial-mapping")
        : initial_clear_result == InjectedIconsClearResult::cleared;
    if (!initial_clear_accepted) {
        destroy_tab_layers(layers);
        log_failure(
            "slot remapping aborted because injected icons could not be cleared");
        return false;
    }

    if (!write_record_fields(top, desired)) {
        destroy_tab_layers(layers);
        if (!restore_previous_mapping(
                top,
                previous,
                allow_transparent_initial_raster_placeholder)) {
            // write_record_fields() is deliberately guarded per field and
            // can therefore fail after publishing a prefix. If the rollback
            // also fails, some live records may still borrow title pointers
            // from g_runtime.catalogue. Freeze mutation and retain that
            // storage rather than letting initialization zero it.
            g_rgba8_publication_disabled.store(
                true, std::memory_order_release);
            g_runtime.destructive_transition_failed = true;
            log_failure(
                "record publication and rollback both failed; live tab-record storage is retained and later remaps are frozen");
        }
        return false;
    }
    const bool repainted = [&]() noexcept {
        if (!allow_transparent_initial_raster_placeholder) {
            return repaint_tab_layers(top, viewport, layers);
        }
        ScopedBooleanTrue initial_target_repaint{g_initial_target_repaint};
        return repaint_tab_layers(top, viewport, layers);
    }();
    if (!repainted) {
        // Some desired-pass cells may already have consumed their temporary
        // proxies. Re-resolve a complete pair of layers for rollback so native
        // records and visible cells agree.
        if (!restore_previous_mapping(
                top,
                previous,
                allow_transparent_initial_raster_placeholder)) {
            g_rgba8_publication_disabled.store(
                true, std::memory_order_release);
            g_runtime.destructive_transition_failed = true;
            log_failure(
                "tab repaint and rollback both failed; live tab-record storage is retained and later remaps are frozen");
        }
        return false;
    }
    return true;
}

bool read_selected_physical(void* top, std::uint32_t& selected) noexcept {
    std::int32_t value{-1};
    if (!top || !guarded_read(
            static_cast<const std::byte*>(top) + kSelectorOffset +
                kSelectorSelectedIndexOffset,
            value) ||
        value < 0 || value >=
            static_cast<std::int32_t>(active_physical_tab_count())) {
        return false;
    }
    selected = static_cast<std::uint32_t>(value);
    return true;
}

PanelSnapshot inspect_panel_slot(const void* slot) noexcept {
    PanelSnapshot snapshot{};
    snapshot.pointer_read = guarded_read(slot, snapshot.panel);
    if (!snapshot.pointer_read || !snapshot.panel) return snapshot;

    snapshot.vtable_read = guarded_read(snapshot.panel, snapshot.vtable);
    snapshot.option_setting = snapshot.vtable_read &&
        snapshot.vtable == g_game.base + kOptionSettingPanelVtableRva;
    // Do not walk deep offsets on an unknown panel type. The pointer and
    // vtable are enough to diagnose a foreign cached object safely.
    if (!snapshot.option_setting) return snapshot;

    snapshot.fields_read = guarded_read(
        static_cast<const std::byte*>(snapshot.panel) +
            kPanelVisualCapacityOffset,
        snapshot.visual_capacity) &&
        guarded_read(
            static_cast<const std::byte*>(snapshot.panel) +
                kPanelNativeRowCountOffset,
            snapshot.native_row_count);
    return snapshot;
}

PanelCarrierSnapshot inspect_panel_carrier(
    const PanelSnapshot& panel) noexcept {
    PanelCarrierSnapshot snapshot{};
    if (!panel.option_setting || !panel.panel || !g_game.base ||
        g_game.size == 0) {
        return snapshot;
    }

    auto* const proxy = static_cast<std::byte*>(panel.panel) + 0x1200;
    if (!guarded_read(proxy, snapshot.proxy_vtable) ||
        snapshot.proxy_vtable !=
            g_game.base + kSceneObjProxyVtableRva ||
        !guarded_read(
            static_cast<const std::byte*>(snapshot.proxy_vtable) +
                sizeof(void*),
            snapshot.getter) ||
        snapshot.getter != g_game.base + kSceneObjProxyGetValueRva) {
        return snapshot;
    }

    using CarrierGetterFn = void*(__fastcall*)(void*);
#if defined(_MSC_VER)
    __try {
#endif
        snapshot.wrapper = reinterpret_cast<CarrierGetterFn>(snapshot.getter)(
            proxy);
        if (snapshot.wrapper && guarded_read(
                static_cast<const std::byte*>(snapshot.wrapper) + 0x20,
                snapshot.wrapper_flags) &&
            guarded_read(
                static_cast<const std::byte*>(snapshot.wrapper) + 0x28,
                snapshot.payload) &&
            snapshot.payload) {
            snapshot.valid = true;
        }
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        snapshot = {};
    }
#endif
    return snapshot;
}

void log_research_carrier_selection(
    void* top,
    const char* stage,
    std::size_t logical,
    std::int32_t category) noexcept {
    if (!top || !stage || !g_runtime.active || g_runtime.top != top ||
        !is_research_custom_category(category)) {
        return;
    }
    const std::uint32_t event =
        g_carrier_selection_log_count.fetch_add(
            1, std::memory_order_relaxed) + 1;
    if (event > kMaximumCarrierSelectionLogs) return;

    auto* const composite = static_cast<std::byte*>(top) +
        kCompositeOffset;
    const PanelSnapshot current = inspect_panel_slot(
        composite + kCompositeCurrentPanelOffset);
    const PanelSnapshot cache4 = inspect_panel_slot(
        composite + kCompositePanelCacheOffset + 4 * sizeof(void*));
    const PanelSnapshot cache8 = inspect_panel_slot(
        composite + kCompositePanelCacheOffset + 8 * sizeof(void*));
    const PanelCarrierSnapshot current_carrier =
        inspect_panel_carrier(current);
    const PanelCarrierSnapshot carrier4 = inspect_panel_carrier(cache4);
    const PanelCarrierSnapshot carrier8 = inspect_panel_carrier(cache8);
    const bool both_present = carrier4.valid && carrier8.valid;
    const bool distinct = both_present && cache4.panel != cache8.panel &&
        carrier4.wrapper != carrier8.wrapper &&
        carrier4.payload != carrier8.payload;
    const bool current_exact = current_carrier.valid && carrier4.valid &&
        current.panel == cache4.panel &&
        current_carrier.wrapper == carrier4.wrapper &&
        current_carrier.payload == carrier4.payload;

    log_line(
        "carrierSelection[%u] stage=%s logical=%zu category=%d current=%p/%p/%p custom4=%p/%p/%p{cap:%u rows:%llu} stockGraphics8=%p/%p/%p{cap:%u rows:%llu} bothPresent=%u distinct={panel:%u wrapper:%u payload:%u exact:%u} currentExact=%u",
        static_cast<unsigned>(event),
        stage,
        logical,
        category,
        current.panel,
        current_carrier.wrapper,
        current_carrier.payload,
        cache4.panel,
        carrier4.wrapper,
        carrier4.payload,
        static_cast<unsigned>(cache4.visual_capacity),
        static_cast<unsigned long long>(cache4.native_row_count),
        cache8.panel,
        carrier8.wrapper,
        carrier8.payload,
        static_cast<unsigned>(cache8.visual_capacity),
        static_cast<unsigned long long>(cache8.native_row_count),
        both_present ? 1U : 0U,
        both_present && cache4.panel != cache8.panel ? 1U : 0U,
        both_present && carrier4.wrapper != carrier8.wrapper ? 1U : 0U,
        both_present && carrier4.payload != carrier8.payload ? 1U : 0U,
        distinct ? 1U : 0U,
        current_exact ? 1U : 0U);
}

bool resolve_panel_child(
    void* panel,
    const char* path,
    ResolvedTabProxy& proxy) noexcept {
    if (!panel || !path || !*path || !g_resolve_path ||
        !g_value_exists || !g_destroy_scaleform_value) {
        return false;
    }

    bool resolved{};
#if defined(_MSC_VER)
    __try {
#endif
        void* const source = static_cast<std::byte*>(panel) + 0x1200;
        void* const result = g_resolve_path(
            source, proxy.storage.data(), path);
        proxy.constructed = result == proxy.storage.data();
        resolved = proxy.constructed &&
            g_value_exists(proxy.storage.data());
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        resolved = false;
    }
#endif
    return resolved;
}

bool read_display_num_children(
    void* carrier_wrapper,
    std::uint32_t& count) noexcept {
    count = 0;
    if (!carrier_wrapper || !g_get_scaleform_member ||
        !g_destroy_scaleform_value) {
        return false;
    }

    alignas(16) std::array<std::byte, 0x38> value{};
    bool constructed{};
    bool valid{};
#if defined(_MSC_VER)
    __try {
#endif
        const char* name = "numChildren";
        constructed = g_get_scaleform_member(
            carrier_wrapper, value.data(), &name) == value.data();
        std::uint32_t flags{};
        std::uint32_t encoded{};
        valid = constructed &&
            guarded_read(value.data() + 0x20, flags) &&
            (flags & 0x8Fu) == 0x03u &&
            guarded_read(value.data() + 0x28, encoded);
        if (valid) count = encoded;
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        valid = false;
    }
#endif
    destroy_local_scaleform_value(value, constructed, "numChildren");
    return valid;
}

bool get_display_child_at(
    const DisplayValueView& carrier,
    std::uint32_t index,
    std::array<std::byte, 0x38>& wrapper,
    bool& constructed,
    DisplayValueView& child) noexcept {
    constructed = false;
    child = {};
    if (!exact_as3_display_bridge(carrier) ||
        !g_construct_scaleform_value || !g_destroy_scaleform_value) {
        return false;
    }

    bool invoked{};
#if defined(_MSC_VER)
    __try {
#endif
        constructed = g_construct_scaleform_value(wrapper.data()) ==
            wrapper.data();
        if (constructed) {
            const RawGfxValue argument = make_int_value(
                static_cast<std::int32_t>(index));
            invoked = reinterpret_cast<ValueInvokeFn>(carrier.invoke_slot)(
                carrier.object_interface,
                carrier.object_payload,
                wrapper.data() + 0x08,
                "getChildAt",
                &argument,
                1,
                true);
        }
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        invoked = false;
    }
#endif
    return invoked && inspect_display_value(wrapper.data(), child) &&
        exact_as3_display_bridge(child);
}

void preserve_active_aliased_carrier_visibility(
    safetyhook::Context& context) noexcept {
    if (!g_runtime.active || !g_runtime.top || !g_game.base) return;

    auto* const composite = reinterpret_cast<std::byte*>(context.rsi);
    auto* const expected_composite =
        static_cast<std::byte*>(g_runtime.top) + kCompositeOffset;
    if (!composite || composite != expected_composite) return;

    // The native loop has just compared this candidate panel with the active
    // panel and placed the desired visibility in DL. Never alter an existing
    // native show decision, and never touch any pair other than the two exact
    // wrappers used by Game Options (cache 0) and the research tab (cache 4).
    const bool native_visible = (context.rdx & 0xFFu) != 0;
    if (native_visible) return;

    void* active{};
    if (!guarded_read(
            composite + kCompositeCurrentPanelOffset,
            active) ||
        !active) {
        return;
    }

    const PanelSnapshot cache0 = inspect_panel_slot(
        composite + kCompositePanelCacheOffset + 0 * sizeof(void*));
    const PanelSnapshot cache4 = inspect_panel_slot(
        composite + kCompositePanelCacheOffset + 4 * sizeof(void*));
    void* const candidate = reinterpret_cast<void*>(context.rcx);
    const void* const candidate_slot =
        reinterpret_cast<const void*>(context.rbx);
    const void* const cache0_slot =
        composite + kCompositePanelCacheOffset + 0 * sizeof(void*);
    const void* const cache4_slot =
        composite + kCompositePanelCacheOffset + 4 * sizeof(void*);
    const std::uintptr_t candidate_category = context.rdi;
    if (!cache0.option_setting || !cache4.option_setting ||
        !cache0.panel || !cache4.panel ||
        cache0.panel == cache4.panel || !candidate) {
        return;
    }

    const bool exact_alias_pair =
        (active == cache0.panel && candidate == cache4.panel &&
            candidate_slot == cache4_slot && candidate_category == 4) ||
        (active == cache4.panel && candidate == cache0.panel &&
            candidate_slot == cache0_slot && candidate_category == 0);
    if (!exact_alias_pair) return;

    // Different C++ panel wrappers are allowed to alias only when their
    // validated SceneObj proxies resolve to the exact same live Scaleform
    // payload. If any part of that proof changes, preserve native behavior.
    const PanelCarrierSnapshot cache0_carrier =
        inspect_panel_carrier(cache0);
    const PanelCarrierSnapshot cache4_carrier =
        inspect_panel_carrier(cache4);
    if (!cache0_carrier.valid || !cache4_carrier.valid ||
        (cache0_carrier.wrapper_flags & 0x8Fu) < 2u ||
        (cache4_carrier.wrapper_flags & 0x8Fu) < 2u ||
        cache0_carrier.payload != cache4_carrier.payload) {
        return;
    }

    context.rdx = (context.rdx & ~std::uintptr_t{0xFF}) |
        std::uintptr_t{1};

    const std::uint32_t event =
        g_alias_visibility_log_count.fetch_add(
            1, std::memory_order_relaxed) + 1;
    if (event <= kMaximumAliasVisibilityLogs) {
        log_line(
            "aliasVisibility[%u] active=%p candidate=%p activeCategory=%u candidateCategory=%u payload=%p native=0 final=1",
            static_cast<unsigned>(event),
            active,
            candidate,
            active == cache0.panel ? 0u : 4u,
            candidate == cache0.panel ? 0u : 4u,
            cache0_carrier.payload);
    }
}

const char* panel_state_isolation_reason_name(
    PanelStateIsolationReason reason) noexcept {
    switch (reason) {
    case PanelStateIsolationReason::provisional:
        return "provisional";
    case PanelStateIsolationReason::corrective:
        return "corrective";
    case PanelStateIsolationReason::none:
        break;
    }
    return "none";
}

bool begin_panel_state_isolation(
    void* top,
    std::int32_t target_category,
    PanelStateIsolationReason reason) noexcept {
    if (!top || reason == PanelStateIsolationReason::none ||
        target_category < 0 || target_category >= 10 ||
        !g_runtime.active || g_runtime.top != top ||
        g_state_isolation_expectation.active) {
        return false;
    }

    auto* const composite =
        static_cast<std::byte*>(top) + kCompositeOffset;
    void* source{};
    if (!guarded_read(
            composite + kCompositeCurrentPanelOffset,
            source) ||
        !source) {
        return false;
    }
    void* cached_target{};
    if (!guarded_read(composite + kCompositePanelCacheOffset +
            static_cast<std::size_t>(target_category) * sizeof(void*), cached_target) ||
        cached_target == source) {
        return false;
    }

    g_state_isolation_expectation = {
        true,
        false,
        composite,
        source,
        target_category,
        reason,
    };
    return true;
}

void end_panel_state_isolation(bool armed) noexcept {
    if (!armed) return;
    if (g_state_isolation_expectation.active &&
        !g_state_isolation_expectation.consumed) {
        log_failure(
            "expected %s panel-state isolation did not reach the validated native copy seam",
            panel_state_isolation_reason_name(
                g_state_isolation_expectation.reason));
    }
    g_state_isolation_expectation = {};
}

void isolate_panel_state_copy(safetyhook::Context& context) noexcept {
    if (!g_viewport_callbacks_armed.load(std::memory_order_acquire)) return;
    if (!g_runtime.active || !g_runtime.top || !g_game.base) return;

    auto* const composite = reinterpret_cast<std::byte*>(context.rsi);
    void* const source = reinterpret_cast<void*>(context.rbx);
    void* const target = reinterpret_cast<void*>(context.rax);
    auto* const expected_composite =
        static_cast<std::byte*>(g_runtime.top) + kCompositeOffset;
    if (!composite || composite != expected_composite || !source ||
        !target || source == target) {
        return;
    }

    void* current{};
    if (!guarded_read(
            composite + kCompositeCurrentPanelOffset,
            current) ||
        current != target) {
        return;
    }

    std::int32_t source_category{-1};
    std::int32_t target_category{-1};
    PanelSnapshot source_snapshot{};
    PanelSnapshot target_snapshot{};
    for (std::int32_t category = 0; category < 10; ++category) {
        const void* const slot = composite + kCompositePanelCacheOffset +
            static_cast<std::size_t>(category) * sizeof(void*);
        const PanelSnapshot snapshot = inspect_panel_slot(slot);
        if (snapshot.panel == source && source_category < 0) {
            source_category = category;
            source_snapshot = snapshot;
        }
        if (snapshot.panel == target && target_category < 0) {
            target_category = category;
            target_snapshot = snapshot;
        }
    }
    if (source_category < 0 || target_category < 0 ||
        !source_snapshot.option_setting || !source_snapshot.fields_read ||
        !target_snapshot.option_setting || !target_snapshot.fields_read) {
        return;
    }

    PanelStateIsolationExpectation& expectation =
        g_state_isolation_expectation;
    const bool expected_transition = expectation.active &&
        expectation.composite == composite &&
        expectation.source == source &&
        expectation.target_category == target_category;
    if (!expected_transition) return;

    expectation.consumed = true;
    context.rip = reinterpret_cast<std::uintptr_t>(g_game.base) +
        kSwitchPanelStateCopyResumeRva;

    const std::uint32_t event =
        g_state_isolation_log_count.fetch_add(
            1, std::memory_order_relaxed) + 1;
    if (event <= kMaximumStateIsolationLogs) {
        log_line(
            "panelStateIsolation[%u] reason=%s sourceCategory=%d targetCategory=%d source=%p target=%p skipped=eldenring.exe+0x%llX..+0x%llX",
            static_cast<unsigned>(event),
            panel_state_isolation_reason_name(expectation.reason),
            source_category,
            target_category,
            source,
            target,
            static_cast<unsigned long long>(kSwitchPanelStateCopyRva),
            static_cast<unsigned long long>(
                kSwitchPanelStateCopyResumeRva));
    }
}

void log_wrap_panel_state(
    void* top,
    const char* stage,
    std::size_t previous_logical,
    std::size_t desired_logical) noexcept {
    if (!top || !stage) return;
    const std::uint32_t line =
        g_wrap_diagnostic_log_count.fetch_add(
            1, std::memory_order_relaxed) + 1;
    if (line > kMaximumWrapDiagnosticLogs) return;

    const auto* const composite =
        static_cast<const std::byte*>(top) + kCompositeOffset;
    const PanelSnapshot current = inspect_panel_slot(
        composite + kCompositeCurrentPanelOffset);
    const PanelSnapshot cache0 = inspect_panel_slot(
        composite + kCompositePanelCacheOffset + 0 * sizeof(void*));
    const PanelSnapshot cache4 = inspect_panel_slot(
        composite + kCompositePanelCacheOffset + 4 * sizeof(void*));
    const PanelSnapshot cache5 = inspect_panel_slot(
        composite + kCompositePanelCacheOffset + 5 * sizeof(void*));
    const PanelSnapshot cache8 = inspect_panel_slot(
        composite + kCompositePanelCacheOffset + 8 * sizeof(void*));
    const PanelCarrierSnapshot current_carrier =
        inspect_panel_carrier(current);
    const PanelCarrierSnapshot cache0_carrier =
        inspect_panel_carrier(cache0);
    const PanelCarrierSnapshot cache4_carrier =
        inspect_panel_carrier(cache4);

    std::uint32_t selected{};
    const bool selector_read = read_selected_physical(top, selected);
    std::int32_t selected_category{-1};
    const bool category_read = selector_read && guarded_read(
        record_at(top, selected) + kCategoryIdOffset,
        selected_category);

    log_line(
        "wrapPanel[%u] logical=%zu->%zu stage=%s selector=%s%u selectedCategory=%s%d current=%p eq={0:%u 4:%u 5:%u 8:%u} currentPanel={vtable:%p option:%u fields:%u cap:%u rows:%llu} cache0=%p{vtable:%p option:%u fields:%u cap:%u rows:%llu} cache4=%p{vtable:%p option:%u fields:%u cap:%u rows:%llu} cache5=%p{option:%u fields:%u rows:%llu} cache8=%p{option:%u fields:%u rows:%llu} carriers={current:%p>%p/0x%X/%u cache0:%p>%p/0x%X/%u cache4:%p>%p/0x%X/%u same04:%u}",
        static_cast<unsigned>(line),
        previous_logical,
        desired_logical,
        stage,
        selector_read ? "" : "?",
        static_cast<unsigned>(selected),
        category_read ? "" : "?",
        selected_category,
        current.panel,
        current.panel && current.panel == cache0.panel ? 1u : 0u,
        current.panel && current.panel == cache4.panel ? 1u : 0u,
        current.panel && current.panel == cache5.panel ? 1u : 0u,
        current.panel && current.panel == cache8.panel ? 1u : 0u,
        current.vtable,
        current.option_setting ? 1u : 0u,
        current.fields_read ? 1u : 0u,
        static_cast<unsigned>(current.visual_capacity),
        static_cast<unsigned long long>(current.native_row_count),
        cache0.panel,
        cache0.vtable,
        cache0.option_setting ? 1u : 0u,
        cache0.fields_read ? 1u : 0u,
        static_cast<unsigned>(cache0.visual_capacity),
        static_cast<unsigned long long>(cache0.native_row_count),
        cache4.panel,
        cache4.vtable,
        cache4.option_setting ? 1u : 0u,
        cache4.fields_read ? 1u : 0u,
        static_cast<unsigned>(cache4.visual_capacity),
        static_cast<unsigned long long>(cache4.native_row_count),
        cache5.panel,
        cache5.option_setting ? 1u : 0u,
        cache5.fields_read ? 1u : 0u,
        static_cast<unsigned long long>(cache5.native_row_count),
        cache8.panel,
        cache8.option_setting ? 1u : 0u,
        cache8.fields_read ? 1u : 0u,
        static_cast<unsigned long long>(cache8.native_row_count),
        current_carrier.wrapper,
        current_carrier.payload,
        static_cast<unsigned>(current_carrier.wrapper_flags),
        current_carrier.valid ? 1u : 0u,
        cache0_carrier.wrapper,
        cache0_carrier.payload,
        static_cast<unsigned>(cache0_carrier.wrapper_flags),
        cache0_carrier.valid ? 1u : 0u,
        cache4_carrier.wrapper,
        cache4_carrier.payload,
        static_cast<unsigned>(cache4_carrier.wrapper_flags),
        cache4_carrier.valid ? 1u : 0u,
        cache0_carrier.valid && cache4_carrier.valid &&
                cache0_carrier.payload == cache4_carrier.payload
            ? 1u
            : 0u);
}

bool restore_preserved_stock_records(void* top) noexcept {
    if (!top || !g_preserved_stock_catalogue_ready) return false;
    std::array<RecordFields, kPhysicalTabCapacity> stock{};
    for (std::size_t physical = 0;
         physical < kStockLogicalTabCount;
         ++physical) {
        const LogicalTab& logical =
            g_preserved_stock_catalogue[physical];
        if (logical.category != kExpectedStockCategories[physical] ||
            logical.title[0] == L'\0') {
            return false;
        }
        stock[physical].title = logical.title.data();
        stock[physical].category = logical.category;
        stock[physical].presentation = logical.presentation;
    }
    const LogicalTab& graphics =
        g_preserved_stock_catalogue[kStockLogicalTabCount - 1U];
    for (std::size_t physical = kStockLogicalTabCount;
         physical < active_physical_tab_count();
         ++physical) {
        stock[physical].title = graphics.title.data();
        stock[physical].category = graphics.category;
        stock[physical].presentation = graphics.presentation;
    }
    return write_record_fields(top, stock);
}

void reset_runtime() noexcept {
    (void)freeze_active_two_phase_rgba8_transaction(
        "runtime reset while transaction was active");
    reset_pointer_dwell(nullptr);
    // A scheduled probe owns no AS value, but its menu coordinates are valid
    // only for the current Configuration epoch.
    g_deferred_bitmapdata_probe = {};
    // The BitmapData property itself is movie-owned and is intentionally left
    // alone. Only discard native verification coordinates when that movie
    // epoch ends; never dereference or clear the remembered payload here.
    g_bitmapdata_property_verification = {};
    g_in_place_rgba8_rebind_transaction = {};
    g_runtime = {};
    g_mutating_viewport = false;
    g_tab_selection_source = TabSelectionSource::unknown;
    g_pointer_cell_sample_trace = {};
    g_blocking_composite_input = false;
    g_state_isolation_expectation = {};
#if defined(ER_NATIVE_UI_PRODUCTION_TABS)
    if (g_native_tab_runtime &&
        g_injected_icon_hosts.load(std::memory_order_acquire) == 0U &&
        g_staged_injected_icon_hosts.load(std::memory_order_acquire) == 0U &&
        all_rgba8_retirement_debts_zero()) {
        g_native_tab_runtime->reset_epoch_tracking();
    }
#endif
}

bool acknowledge_exact_carrier_teardown() noexcept {
#if defined(ER_NATIVE_UI_PRODUCTION_TABS)
    const std::uint64_t carrier_teardown_generation =
        erui::native::native_tab_graphics_carrier_teardown_generation();
    if (carrier_teardown_generation ==
            g_research_carrier_teardown_generation) {
        return false;
    }
    const bool recovered = g_research_failed;
    g_research_carrier_teardown_generation = carrier_teardown_generation;
    g_research_failed = false;
    if (recovered) {
        log_line(
            "Graphics carrier fault cleared after exact native teardown generation=%llu; the new Configuration epoch may construct native Graphics again",
            static_cast<unsigned long long>(
                carrier_teardown_generation));
    }
    return recovered;
#else
    return false;
#endif
}

bool initialize_runtime(
    void* top,
    bool use_preserved_stock = false) noexcept {
    if (!validate_top(top)) return false;
    LiveTabGeometry live_geometry{};
    if (!read_live_tab_geometry(top, live_geometry)) {
        return false;
    }
#if defined(ER_NATIVE_UI_PRODUCTION_TABS)
    if (!g_native_tab_runtime) return false;
    bool owns_provisional_physical_epoch{};
    const std::size_t epoch_count =
        g_native_tab_runtime->active_physical_slot_count();
    if (epoch_count == 0U) {
        if (!g_native_tab_runtime->begin_physical_epoch(
                live_geometry.active)) {
            return false;
        }
        owns_provisional_physical_epoch = true;
    } else if (epoch_count != live_geometry.active) {
        return false;
    }

    // Binding D0 to the consolidated runtime is provisional until RuntimeState
    // is published below. All returns before that boundary are observational or
    // operate only on process-owned catalogue resources, so this guard may end
    // the span exactly. Once published, reset_runtime() remains the sole owner
    // of epoch teardown because records and display objects may borrow it.
    struct ProvisionalPhysicalEpoch final {
        erui::detail::NativeTabRuntimeState* runtime{};
        bool owned{};
        bool published{};

        ~ProvisionalPhysicalEpoch() noexcept {
            if (runtime && owned && !published) {
                runtime->reset_epoch_tracking();
            }
        }
    } provisional_epoch{
        g_native_tab_runtime.get(),
        owns_provisional_physical_epoch,
        false,
    };
#endif
    if (live_geometry.active != active_physical_tab_count()) return false;
    log_line(
        "Configuration tab geometry validated top=%p active=%zu authored=%zu selected=%zu stockLogical=%zu nativeRecordCapacity=%zu",
        top,
        live_geometry.active,
        live_geometry.authored,
        live_geometry.selected,
        kStockLogicalTabCount,
        kPhysicalTabCapacity);
    (void)acknowledge_exact_carrier_teardown();
    g_pointer_dwell.policy = TabPointerDwell(g_pointer_dwell_timing);
    const std::uint64_t initialization_started_qpc =
        query_performance_ticks();
    if ((g_injected_icon_hosts.load(std::memory_order_acquire) |
            g_staged_injected_icon_hosts.load(
                std::memory_order_acquire)) != 0) {
        log_line(
            "Viewport initialization found a surviving injected-icon ledger; reconciling it before publishing a new catalogue");
        const InjectedIconsClearResult clear_result =
            clear_injected_icons(top, true);
        if (!accept_vector_only_epoch_clear_result(
                top, clear_result, "runtime-initialization")) {
            if (clear_result == InjectedIconsClearResult::failed) {
                log_failure(
                    "Viewport initialization refused to overwrite a surviving injected-icon ledger");
            }
            return false;
        }
    } else {
        g_injected_movie_fingerprint = {};
    }

    if (use_preserved_stock && !g_preserved_stock_catalogue_ready) {
        log_failure(
            "Viewport initialization requested a preserved stock catalogue before one was captured");
        return false;
    }

    std::array<RecordFields, kPhysicalTabCapacity> original{};
    if (!use_preserved_stock && !snapshot_record_fields(top, original)) {
        return false;
    }

    RuntimeState next{};
    next.top = top;
    next.presentation_epoch =
        g_presentation_epoch_counter.fetch_add(
            1, std::memory_order_relaxed) + 1;
    const auto initialization_tab = [&next](
        std::size_t logical) noexcept -> LogicalTab* {
#if defined(ER_NATIVE_UI_PRODUCTION_TABS)
        (void)next;
        return runtime_logical_tab(logical);
#else
        return logical < next.catalogue.size()
            ? &next.catalogue[logical]
            : nullptr;
#endif
    };
    if (active_logical_tab_count() < kStockLogicalTabCount ||
        active_logical_tab_count() < active_physical_tab_count()) {
        return false;
    }
#if defined(ER_NATIVE_UI_PRODUCTION_TABS)
    if (!g_logical_tab_catalogue ||
        g_logical_tab_catalogue->tabs.size() !=
            active_logical_tab_count()) {
        log_failure("pinned logical tab catalogue is unavailable");
        return false;
    }
#endif
    for (std::size_t logical = 0;
         logical < kStockLogicalTabCount;
         ++logical) {
        LogicalTab* const destination = initialization_tab(logical);
        if (!destination) return false;
        if (use_preserved_stock) {
            const LogicalTab& stock = g_preserved_stock_catalogue[logical];
            if (stock.category != kExpectedStockCategories[logical] ||
                !copy_title(
                    stock.title.data(), destination->title)) {
                return false;
            }
            destination->category = stock.category;
            destination->presentation = stock.presentation;
        } else {
            if (original[logical].category !=
                    kExpectedStockCategories[logical] ||
                !copy_title(
                    original[logical].title,
                    destination->title)) {
                return false;
            }
            destination->category = original[logical].category;
            destination->presentation = original[logical].presentation;
        }
    }
    if (!use_preserved_stock) {
        for (std::size_t logical = 0;
             logical < kStockLogicalTabCount;
             ++logical) {
            const LogicalTab* const source = initialization_tab(logical);
            if (!source) return false;
            g_preserved_stock_catalogue[logical] = *source;
        }
        g_preserved_stock_catalogue_ready = true;
    }

    for (std::size_t icon = 0;
         icon < active_custom_tab_count();
         ++icon) {
        LogicalTab* const tab = initialization_tab(
            kStockLogicalTabCount + icon);
        if (!tab) return false;
        if (!make_synthetic_icon_descriptor(
                tab->presentation, kRgba8IconIds[icon])) {
            log_failure(
                "could not construct RGBA8 icon descriptor %zu (id=%d)",
                icon,
                kRgba8IconIds[icon]);
            return false;
        }
    }
    if (!make_synthetic_icon_descriptor(
            next.overflow_presentation, kOverflowIconId)) {
        log_failure(
            "could not construct the overflow ellipsis icon descriptor");
        return false;
    }
    for (std::size_t logical = kStockLogicalTabCount;
         logical < active_logical_tab_count();
         ++logical) {
        const std::size_t extra = logical - kStockLogicalTabCount;
        LogicalTab* const destination = initialization_tab(logical);
        if (!destination) return false;
#if defined(ER_NATIVE_UI_PRODUCTION_TABS)
        const auto custom = g_native_tab_catalogue
            ? g_native_tab_catalogue->custom_entries()
            : std::span<const erui::detail::NativeTabCatalogueEntry>{};
        erui::detail::RuntimeState& host_runtime =
            erui::detail::runtime_state();
        const wchar_t* const title =
            extra < custom.size() && host_runtime.menu
            ? host_runtime.menu->texts.lookup(
                  static_cast<int>(custom[extra].title_id))
            : nullptr;
        if (!copy_title(title, destination->title)) {
            log_failure(
                "custom-tab title %zu is unavailable or exceeds %zu UTF-16 code units",
                extra,
                destination->title.size() - 1U);
            return false;
        }
#else
        const int title_length = swprintf_s(
            destination->title.data(),
            destination->title.size(),
            L"[Probe] Client Page %02zu",
            extra + 1);
        if (title_length <= 0 ||
            static_cast<std::size_t>(title_length) >=
                destination->title.size()) {
            log_failure("could not generate a stable custom-tab title");
            return false;
        }
#endif
        // Every custom tab resolves through the category-8 Graphics carrier.
        destination->category = 8;
    }

    std::uint32_t selected{};
    if (!read_selected_physical(top, selected) ||
        !next.viewport.initialize(
            active_logical_tab_count(),
            active_physical_tab_count(),
            g_navigation_margin,
            0)) {
        return false;
    }
    const TabViewportChange initial_selection =
        next.viewport.select_physical(selected);
    if (!initial_selection.valid) return false;

    // Publish the catalogue before build_mapping_fields() reads it. No hook
    // can interleave here because all menu callbacks execute on this UI thread.
    g_runtime = next;
#if defined(ER_NATIVE_UI_PRODUCTION_TABS)
    provisional_epoch.published = true;
#endif
    ResolvedTabLayers layers{};
    if (!resolve_tab_layers(top, layers)) {
        reset_runtime();
        return false;
    }
    if (!apply_mapping(top, g_runtime.viewport, layers, true)) {
        if (g_runtime.destructive_transition_failed) {
            // A partial initial write followed by an unprovable rollback can
            // leave native records borrowing this catalogue. Mark the epoch
            // active solely as a lifetime guard: ensure_runtime() will detect
            // the non-exact mapping, suspend mutation, and keep these strings
            // alive for as long as the Configuration instance can use them.
            g_runtime.active = true;
            g_runtime.pending = {};
            reset_pointer_dwell(
                "initial tab publication failed destructively");
            log_failure(
                "initial tab publication could not be rolled back exactly; catalogue storage is pinned and viewport mutation is suspended");
            return false;
        }
        reset_runtime();
        return false;
    }
    g_runtime.active = true;

    // Raster resources are process-owned, while every Configuration movie
    // has a fresh display tree. RuntimeState remains inactive until the
    // mapping is complete, so initial publication always uses the proven
    // vector-Shape ownership route. When an exact raster survives from an
    // earlier epoch that Shape is transparent; otherwise it remains the
    // visible failure-safe cross. Schedule one ownership-checked presentation
    // pass for every new movie epoch to perform the normal raster handoff.
    const bool presentation_refresh_requested =
        !g_rgba8_publication_disabled.load(std::memory_order_acquire) &&
        g_runtime.catalogue_presentation_refresh.request_if_needed(
            g_runtime.viewport,
            kStockLogicalTabCount,
            g_runtime.presentation_epoch,
            g_composite_frame_sequence);
    log_line(
        "Configuration epoch deferred initial presentation refresh=%u epoch=%llu window=%zu generation=%llu catalogueReady=%u",
        presentation_refresh_requested ? 1U : 0U,
        static_cast<unsigned long long>(
            g_runtime.presentation_epoch),
        g_runtime.viewport.window_start(),
        static_cast<unsigned long long>(
            g_runtime.viewport.mapping_generation()),
        g_rgba8_catalogue_ready.load(std::memory_order_acquire)
            ? 1U
            : 0U);
    invoke_refresh_title(top);

#if defined(ER_NATIVE_UI_PRODUCTION_TABS)
    // Keep one lightweight, release-log proof per Configuration epoch. The
    // per-record and transition traces below remain diagnostics-only, but this
    // line is enough to establish which live selector geometry was accepted
    // when investigating a player report.
    erui::detail::logf(
        erui::LogLevel::info,
        "NativeTabs Configuration geometry accepted: active=%zu authored=%zu logical=%zu selected=%zu",
        live_geometry.active,
        live_geometry.authored,
        active_logical_tab_count(),
        live_geometry.selected);
#endif

    const std::uint64_t initialization_microseconds =
        elapsed_qpc_microseconds(
            initialization_started_qpc, query_performance_ticks());
    log_line(
        "STRESS viewport initialized epoch=%llu top=%p logical=%zu physical=%zu margin=%zu selectedLogical=%zu selectedPhysical=%zu window=%zu timingUs=%llu",
        static_cast<unsigned long long>(g_runtime.presentation_epoch),
        top,
        g_runtime.viewport.logical_count(),
        g_runtime.viewport.physical_capacity(),
        g_runtime.viewport.navigation_margin(),
        g_runtime.viewport.selected_logical(),
        g_runtime.viewport.selected_physical(),
        g_runtime.viewport.window_start(),
        static_cast<unsigned long long>(initialization_microseconds));
    for (std::size_t logical = 0;
         logical < active_logical_tab_count();
         ++logical) {
        const LogicalTab* const tab = runtime_logical_tab(logical);
        if (!tab) return false;
        std::uint32_t token{};
        std::int32_t resource_id{-1};
        std::memcpy(
            &token,
            tab->presentation.data(),
            sizeof(token));
        std::memcpy(
            &resource_id,
            tab->presentation.data() + 0x3C,
            sizeof(resource_id));
        log_line(
            "catalogue[%zu] category=%d iconToken=%u resourceId=%d title=%ls",
            logical,
            tab->category,
            static_cast<unsigned>(token),
            resource_id,
            tab->title.data());
    }
    log_rgba8_lifecycle_snapshot("epoch-initialized", top);
    return true;
}

bool ensure_runtime(void* top) noexcept {
    if (!validate_top(top)) {
        if (freeze_active_two_phase_rgba8_transaction(
                "Configuration top validation failed")) {
            g_runtime.pending = {};
            reset_pointer_dwell(
                "Configuration top validation failed during transaction");
        }
        return false;
    }
    if (g_runtime.active) {
        if (g_runtime.destructive_transition_failed) {
            // The active flag can be a lifetime guard after an unprovable
            // initial rollback. Keep all borrowed catalogue storage alive but
            // perform no further viewport reads, writes, or navigation.
            return false;
        }
        if (g_runtime.top == top) {
            if (records_match_mapping(top, g_runtime.viewport)) {
                const bool fingerprint_known =
                    g_injected_movie_fingerprint.object_interface &&
                    g_injected_movie_fingerprint.owner_link &&
                    g_injected_movie_fingerprint.movie_root;
                if (!fingerprint_known) return true;

                InjectedMovieFingerprint current_fingerprint{};
                // r151: PerfLog timing of this per-frame safety sample.
#if defined(ER_NATIVE_UI_PRODUCTION_TABS)
                const bool perf = erui::native::hook_perf_enabled();
                const std::uint64_t perf_start =
                    perf ? erui::native::hook_perf_ticks() : 0;
#endif
                const bool sampled = sample_current_movie_fingerprint(
                    top, current_fingerprint);
#if defined(ER_NATIVE_UI_PRODUCTION_TABS)
                if (perf) {
                    erui::native::record_tab_fingerprint_perf(
                        erui::native::hook_perf_ticks() - perf_start);
                }
#endif
                if (!sampled) {
                    if (freeze_active_two_phase_rgba8_transaction(
                            "same-top movie fingerprint could not be sampled")) {
                        g_runtime.pending = {};
                        reset_pointer_dwell(
                            "movie fingerprint sampling failed during transaction");
                    }
                    log_failure(
                        "same-address Configuration movie fingerprint could not be sampled; viewport fast return refused");
                    return false;
                }
                if (same_movie_fingerprint(
                        g_injected_movie_fingerprint,
                        current_fingerprint)) {
                    return true;
                }

                // Records can survive while Scaleform recreates its movie at
                // the same native Configuration address. Reconcile the old
                // ledger against the newly sampled parent before any lookup
                // or bind callback mutates that new presentation tree.
                log_line(
                    "same-address Configuration records retained across a movie change; reconciling old fingerprint={interface:%p owner:%p root:%p} current={interface:%p owner:%p root:%p}",
                    g_injected_movie_fingerprint.object_interface,
                    g_injected_movie_fingerprint.owner_link,
                    g_injected_movie_fingerprint.movie_root,
                    current_fingerprint.object_interface,
                    current_fingerprint.owner_link,
                    current_fingerprint.movie_root);
                (void)freeze_active_two_phase_rgba8_transaction(
                    "same-address movie fingerprint changed");
                g_runtime.pending = {};
                reset_pointer_dwell("same-address movie epoch changed");
                const InjectedIconsClearResult clear_result =
                    clear_injected_icons(top, true);
                if (!accept_vector_only_epoch_clear_result(
                        top,
                        clear_result,
                        "same-address-movie-change")) {
                    if (clear_result == InjectedIconsClearResult::failed) {
                        log_failure(
                            "same-address movie change could not reconcile the retained icon ledger; viewport remains suspended");
                    }
                    return false;
                }
                if (!restore_preserved_stock_records(top)) {
                    log_failure(
                        "same-address movie change could not restore the process-stable stock records before reinitialization");
                    return false;
                }
                log_line(
                    "same-address movie change restored all eight stock records from the preserved pristine catalogue before reinitialization");
                reset_runtime();
                return initialize_runtime(top, true);
            }
            if (exact_stock_layout_republished(top)) {
                // Elden Ring can reuse the same Configuration allocation for
                // a later menu epoch. It first republishes all eight exact
                // stock records, including native-owned title pointers. Only
                // that complete fingerprint is accepted as a safe lifetime
                // boundary; arbitrary same-address mutations still fail
                // closed below.
                (void)freeze_active_two_phase_rgba8_transaction(
                    "stock layout was republished during the transaction");
                g_runtime.pending = {};
                reset_pointer_dwell("configuration epoch reset");
                const InjectedIconsClearResult clear_result =
                    clear_injected_icons(top, true);
                if (!accept_vector_only_epoch_clear_result(
                        top,
                        clear_result,
                        "same-address-stock-reset")) {
                    if (clear_result == InjectedIconsClearResult::failed) {
                        log_failure(
                            "same-address stock reset could not clear tracked custom icons; viewport remains suspended");
                    }
                    return false;
                }
                log_line(
                    "Exact stock tab layout republished at the same Configuration address; starting a new viewport epoch (top=%p)",
                    top);
                reset_runtime();
                return initialize_runtime(top);
            }
            // Live records borrow title pointers from catalogue. Keep that
            // storage intact if another component rewrites this same menu;
            // zeroing it here would invalidate the strings still in use.
            (void)freeze_active_two_phase_rgba8_transaction(
                "same-top viewport records no longer match the source mapping");
            g_runtime.pending = {};
            reset_pointer_dwell("viewport mapping invalidated");
            log_failure(
                "same Configuration instance no longer matches the viewport mapping; mutation is suspended");
            return false;
        }
        log_line(
            "Configuration instance changed; rebuilding viewport state (oldTop=%p newTop=%p)",
            g_runtime.top,
            top);

        (void)freeze_active_two_phase_rgba8_transaction(
            "Configuration instance changed during the transaction");

        // A new Configuration wrapper does not by itself prove whether Elden
        // Ring retained or recreated the tab-strip display tree. Reacquire
        // every tracked parent through the current, already-validated top. If
        // our two reserved properties still contain the exact objects, remove
        // them. If both are exactly Undefined, the fresh movie already owns a
        // clean parent and this proven epoch boundary may discard the stale
        // bit. Every partial or unexpected state still fails closed.
        const InjectedIconsClearResult clear_result =
            clear_injected_icons(top, true);
        if (!accept_vector_only_epoch_clear_result(
                top, clear_result, "configuration-instance-change")) {
            if (clear_result == InjectedIconsClearResult::failed) {
                log_failure(
                    "Configuration instance changed but retained tab-strip icons could not be cleared through the new top; viewport remains suspended");
            }
            return false;
        }
        if (!restore_preserved_stock_records(top)) {
            log_failure(
                "Configuration instance changed but its records could not be restored from the process-stable stock catalogue");
            return false;
        }
        log_line(
            "Reconciled retained or already-absent tab-strip icons and restored pristine stock records through the new Configuration top before rebuilding viewport state");
        reset_runtime();
        return initialize_runtime(top, true);
    }
    return initialize_runtime(top);
}

} // namespace native_tabs_detail
