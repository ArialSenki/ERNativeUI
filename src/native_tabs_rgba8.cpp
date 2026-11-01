// RGBA8 icon catalogue lifecycle, retirement, in-place rebinds and the texture-lookup detour.
#include "native_tabs_internal.hpp"

namespace native_tabs_detail {

bool guarded_add_ref_scaleform_image_resource(
    ScaleformImageResourceAddRefFn add_ref,
    void* resource,
    unsigned long& seh_code) noexcept;

Rgba8ReferenceSnapshot snapshot_rgba8_references(
    std::size_t asset_index) noexcept {
    Rgba8ReferenceSnapshot snapshot{};
    if (asset_index >= active_custom_tab_count()) return snapshot;
    void* const resource = g_retained_rgba8_resources[asset_index].load(
        std::memory_order_acquire);
    void* const image = g_retained_rgba8_images[asset_index].load(
        std::memory_order_acquire);
    snapshot.resource_read = resource && guarded_read(
        static_cast<const std::byte*>(resource) + 0x08,
        snapshot.resource);
    snapshot.raw_read = image && guarded_read(
        static_cast<const std::byte*>(image) + 0x08,
        snapshot.raw);
    return snapshot;
}

bool reconcile_rgba8_retirement_debt(
    const char* phase) noexcept {
    if (!g_rgba8_catalogue_ready.load(std::memory_order_acquire)) {
        return true;
    }

    const auto reconcile_asset = [phase](
        std::size_t asset,
        bool& removed_from_worklist) noexcept {
        removed_from_worklist = false;
        const std::uint32_t active =
            count_active_rgba8_asset_hosts(asset);
        const std::uint32_t prior_debt = rgba8_retirement_debt(asset);
        if (!catalogue_icon_requires_raster(asset)) {
            if (active != 0 || prior_debt != 0) {
                log_failure(
                    "RGBA8 retirement reconciliation phase=%s found ownership for non-raster asset=%zu active=%u debt=%u",
                    phase ? phase : "unknown",
                    asset,
                    static_cast<unsigned>(active),
                    static_cast<unsigned>(prior_debt));
                return false;
            }
            return true;
        }

        const Rgba8ReferenceSnapshot refs =
            snapshot_rgba8_references(asset);
        const auto retirement = plan_rgba8_retirement_reconciliation(
            active,
            prior_debt,
            refs.raw,
            kRgba8RetirementLimits);
        if (!refs.resource_read || !refs.raw_read || refs.resource != 1U ||
            !retirement.accepted()) {
            log_failure(
                "RGBA8 retirement reconciliation phase=%s asset=%zu refs={resourceRead:%u resource:%u rawRead:%u raw:%u allowed:%u..%u} active=%u debt=%u policy=%u",
                phase ? phase : "unknown",
                asset,
                refs.resource_read ? 1U : 0U,
                static_cast<unsigned>(refs.resource),
                refs.raw_read ? 1U : 0U,
                static_cast<unsigned>(refs.raw),
                static_cast<unsigned>(retirement.minimum_raw),
                static_cast<unsigned>(retirement.maximum_raw),
                static_cast<unsigned>(active),
                static_cast<unsigned>(prior_debt),
                static_cast<unsigned>(retirement.status));
            return false;
        }
        const std::uint32_t observed_debt =
            retirement.retirement_debt_after;
        if (observed_debt < prior_debt) {
            if (!set_rgba8_retirement_debt(asset, observed_debt)) {
                log_failure(
                    "RGBA8 retirement reconciliation phase=%s could not update asset=%zu debt=%u->%u",
                    phase ? phase : "unknown",
                    asset,
                    static_cast<unsigned>(prior_debt),
                    static_cast<unsigned>(observed_debt));
                return false;
            }
            removed_from_worklist = observed_debt == 0U;
            log_line(
                "rgba8Retirement reconcile phase=%s asset=%zu active=%u raw=%u debt=%u->%u",
                phase ? phase : "unknown",
                asset,
                static_cast<unsigned>(active),
                static_cast<unsigned>(refs.raw),
                static_cast<unsigned>(prior_debt),
                static_cast<unsigned>(observed_debt));
        }
        return true;
    };

    bool exact{true};
#if defined(ER_NATIVE_UI_PRODUCTION_TABS)
    // A frame can reference at most twenty active and twenty staged logical
    // assets. Audit that bounded visible set together with the sparse deferred
    // retirement list, so correctness follows live work rather than the API
    // ceiling. Each asset is sampled once because the renderer may decrement
    // its RawImage reference asynchronously between two reads.
    std::array<std::size_t, kTabHostCapacity * 2U> visible_assets{};
    std::array<bool, kTabHostCapacity * 2U> visible_assets_audited{};
    std::size_t visible_asset_count{};
    const auto remember_visible_asset = [
        &visible_assets,
        &visible_asset_count,
        &exact,
        phase](std::size_t encoded) noexcept {
        if (encoded == 0U) return;
        const std::size_t asset = encoded - 1U;
        if (asset >= active_custom_tab_count() ||
            !catalogue_icon_requires_raster(asset)) {
            exact = false;
            log_failure(
                "RGBA8 retirement reconciliation phase=%s found invalid visible asset encoding=%zu",
                phase ? phase : "unknown",
                encoded);
            return;
        }
        for (std::size_t entry = 0;
             entry < visible_asset_count;
             ++entry) {
            if (visible_assets[entry] == asset) return;
        }
        if (visible_asset_count >= visible_assets.size()) {
            exact = false;
            log_failure(
                "RGBA8 retirement reconciliation phase=%s exceeded the bounded visible asset set",
                phase ? phase : "unknown");
            return;
        }
        visible_assets[visible_asset_count++] = asset;
    };
    for (std::size_t host = 0; host < active_tab_host_count(); ++host) {
        remember_visible_asset(g_rgba8_host_assets[host].load(
            std::memory_order_acquire));
        remember_visible_asset(g_staged_rgba8_host_assets[host].load(
            std::memory_order_acquire));
    }

    // The sparse list is render-thread-owned; clearing an entry swaps another
    // index into the same cursor, so that cursor is deliberately revisited.
    std::size_t cursor{};
    while (g_native_tab_runtime &&
           cursor < g_native_tab_runtime->retiring_tab_indices().size()) {
        const std::size_t asset =
            g_native_tab_runtime->retiring_tab_indices()[cursor];
        bool removed{};
        exact = reconcile_asset(asset, removed) && exact;
        for (std::size_t entry = 0;
             entry < visible_asset_count;
             ++entry) {
            if (visible_assets[entry] == asset) {
                visible_assets_audited[entry] = true;
                break;
            }
        }
        if (!removed) ++cursor;
    }
    for (std::size_t entry = 0;
         entry < visible_asset_count;
         ++entry) {
        if (visible_assets_audited[entry]) continue;
        bool removed{};
        exact = reconcile_asset(visible_assets[entry], removed) && exact;
    }
#else
    for (std::size_t asset = 0;
         asset < active_custom_tab_count();
         ++asset) {
        bool removed{};
        exact = reconcile_asset(asset, removed) && exact;
    }
#endif
    return exact;
}

bool rgba8_resources_at_root_baseline() noexcept {
    if (!g_rgba8_catalogue_ready.load(std::memory_order_acquire)) {
        return false;
    }

    // This predicate is used only at an epoch boundary after the current
    // display object reports a different Movie fingerprint. The old Movie has
    // already torn down its Shapes by then, so the live-host ledger still
    // describes the previous epoch and must not participate in this audit.
    // Prove the literal process-owned roots first; only after every asset is at
    // 1/1 may stale renderer-retirement debt be discarded and the caller
    // clear the old host ledger.
    for (std::size_t icon = 0;
         icon < active_custom_tab_count();
         ++icon) {
        if (!catalogue_icon_requires_raster(icon)) {
            const std::uint32_t debt =
                g_rgba8_retirement_debt[icon].load(
                    std::memory_order_acquire);
            if (debt != 0) {
                log_failure(
                    "RGBA8 epoch root-baseline audit found debt for non-raster asset=%zu debt=%u",
                    icon,
                    static_cast<unsigned>(debt));
                return false;
            }
            continue;
        }
        void* const resource = g_retained_rgba8_resources[icon].load(
            std::memory_order_acquire);
        void* const image = g_retained_rgba8_images[icon].load(
            std::memory_order_acquire);
        std::uint32_t resource_ref{};
        std::uint32_t raw_ref{};
        if (!resource || !image ||
            !guarded_read(
                static_cast<const std::byte*>(resource) + 0x08,
                resource_ref) ||
            !guarded_read(
                static_cast<const std::byte*>(image) + 0x08,
                raw_ref) ||
            resource_ref != 1U || raw_ref != 1U) {
            log_failure(
                "RGBA8 epoch root-baseline audit failed asset=%zu resource=%p ref=%u image=%p rawRef=%u",
                icon,
                resource,
                static_cast<unsigned>(resource_ref),
                image,
                static_cast<unsigned>(raw_ref));
            return false;
        }
    }
    clear_all_rgba8_retirement_debts();
    return true;
}

bool rgba8_references_match_tracked_hosts(
    const char* phase,
    std::size_t override_host,
    std::size_t override_asset,
    bool override_staged) noexcept {
    const TabHostMask rgba_hosts = g_rgba8_icon_hosts.load(
        std::memory_order_acquire);
    const TabHostMask staged_rgba_hosts =
        g_staged_rgba8_icon_hosts.load(std::memory_order_acquire);
    if (!g_rgba8_catalogue_ready.load(std::memory_order_acquire)) {
        const bool empty = rgba_hosts == 0 && staged_rgba_hosts == 0;
        if (!empty) {
            log_failure(
                "RGBA8 ref audit phase=%s found tracked hosts before the all-ready catalogue publication",
                phase ? phase : "unknown");
        }
        return empty;
    }

    // At most twenty front/back physical hosts can contribute references.
    // Store those sparse counts by logical index instead of allocating a
    // maximum-sized parallel array.
    std::array<std::pair<std::size_t, std::uint32_t>, kTabHostCapacity * 2U>
        expected_hosts{};
    std::size_t expected_host_entry_count{};
    bool expected_host_overflow{};
    const auto increment_expected_host = [&expected_hosts,
                                          &expected_host_entry_count,
                                          &expected_host_overflow](
        std::size_t custom) noexcept {
        for (std::size_t entry = 0;
             entry < expected_host_entry_count;
             ++entry) {
            if (expected_hosts[entry].first == custom) {
                ++expected_hosts[entry].second;
                return;
            }
        }
        if (expected_host_entry_count < expected_hosts.size()) {
            expected_hosts[expected_host_entry_count++] = {custom, 1U};
        } else {
            expected_host_overflow = true;
        }
    };
    const auto expected_host_count = [&expected_hosts,
                                      &expected_host_entry_count](
        std::size_t custom) noexcept -> std::uint32_t {
        for (std::size_t entry = 0;
             entry < expected_host_entry_count;
             ++entry) {
            if (expected_hosts[entry].first == custom) {
                return expected_hosts[entry].second;
            }
        }
        return 0U;
    };
    bool map_exact{true};
    const auto encoded_asset_is_current = [](std::size_t encoded) noexcept {
        if (encoded == 0) return true;
        const std::size_t asset = static_cast<std::size_t>(encoded - 1U);
        return asset < active_custom_tab_count() &&
            catalogue_icon_requires_raster(asset);
    };
    for (std::size_t host = 0; host < active_tab_host_count(); ++host) {
        const TabHostMask mask = TabHostMask{1} << host;
        const std::size_t encoded = host == override_host &&
                !override_staged
            ? override_asset
            : g_rgba8_host_assets[host].load(std::memory_order_acquire);
        const bool bit_set = host == override_host && !override_staged
            ? encoded != 0
            : (rgba_hosts & mask) != 0;
        if ((encoded != 0) != bit_set ||
            !encoded_asset_is_current(encoded)) {
            map_exact = false;
            continue;
        }
        if (encoded != 0) increment_expected_host(encoded - 1U);

        const std::size_t staged_encoded = host == override_host &&
                override_staged
            ? override_asset
            : g_staged_rgba8_host_assets[host].load(
                std::memory_order_acquire);
        const bool staged_bit_set = host == override_host && override_staged
            ? staged_encoded != 0
            : (staged_rgba_hosts & mask) != 0;
        if ((staged_encoded != 0) != staged_bit_set ||
            !encoded_asset_is_current(staged_encoded)) {
            map_exact = false;
            continue;
        }
        if (staged_encoded != 0) {
            increment_expected_host(staged_encoded - 1U);
        }
    }

    const auto audit_icon = [phase, &map_exact, &expected_host_count](
        std::size_t icon) noexcept {
        const std::uint32_t tracked_hosts = expected_host_count(icon);
        const std::uint32_t retirement_debt = rgba8_retirement_debt(icon);
        if (!catalogue_icon_requires_raster(icon)) {
            const bool entry_exact = tracked_hosts == 0 &&
                retirement_debt == 0;
            if (!entry_exact) {
                log_failure(
                    "RGBA8 ref audit phase=%s found tracked ownership for non-raster asset=%zu trackedHosts=%u retirementDebt=%u mapExact=%u",
                    phase ? phase : "unknown",
                    icon,
                    static_cast<unsigned>(tracked_hosts),
                    static_cast<unsigned>(retirement_debt),
                    map_exact ? 1U : 0U);
            }
            return entry_exact;
        }
        void* const resource = g_retained_rgba8_resources[icon].load(
            std::memory_order_acquire);
        void* const image = g_retained_rgba8_images[icon].load(
            std::memory_order_acquire);
        const std::uint64_t checksum =
            g_retained_rgba8_checksums[icon].load(
                std::memory_order_acquire);
        std::uint32_t resource_ref{};
        std::uint32_t raw_ref{};
        const bool resource_read = resource && guarded_read(
            static_cast<const std::byte*>(resource) + 0x08,
            resource_ref);
        const bool raw_read = image && guarded_read(
            static_cast<const std::byte*>(image) + 0x08,
            raw_ref);
        const std::uint32_t expected_raw =
            1U + tracked_hosts + retirement_debt;
        const bool entry_exact =
            g_retained_rgba8_ready[icon].load(
                std::memory_order_acquire) &&
            resource && image &&
            resource_read && raw_read && resource_ref == 1U &&
            raw_ref == expected_raw;
        if (!entry_exact) {
            log_failure(
                "RGBA8 ref audit phase=%s asset=%zu id=%d checksum=0x%016llX refs={resourceRead:%u resource:%u expected:1 rawRead:%u raw:%u expected:%u} trackedHosts=%u retirementDebt=%u mapExact=%u",
                phase ? phase : "unknown",
                icon,
                kRgba8IconIds[icon],
                static_cast<unsigned long long>(checksum),
                resource_read ? 1U : 0U,
                static_cast<unsigned>(resource_ref),
                raw_read ? 1U : 0U,
                static_cast<unsigned>(raw_ref),
                static_cast<unsigned>(expected_raw),
                static_cast<unsigned>(tracked_hosts),
                static_cast<unsigned>(retirement_debt),
                map_exact ? 1U : 0U);
        }
        return entry_exact;
    };

    if (expected_host_overflow) {
        map_exact = false;
        log_failure(
            "RGBA8 ref audit phase=%s exceeded the bounded active/staged host identity set",
            phase ? phase : "unknown");
    }
    bool exact = map_exact;
#if defined(ER_NATIVE_UI_PRODUCTION_TABS)
    // A transition can affect only the active physical hosts plus resources
    // that still carry deferred renderer references. Immutable inactive roots
    // were proven at catalogue publication and are rechecked at epoch reset;
    // scanning every registered tab on every repaint would make navigation
    // scale with the public API ceiling instead of visible work.
    for (std::size_t entry = 0;
         entry < expected_host_entry_count;
         ++entry) {
        exact = audit_icon(expected_hosts[entry].first) && exact;
    }
    if (g_native_tab_runtime) {
        for (const std::size_t icon :
             g_native_tab_runtime->retiring_tab_indices()) {
            if (expected_host_count(icon) == 0U) {
                exact = audit_icon(icon) && exact;
            }
        }
    }
#else
    for (std::size_t icon = 0;
         icon < active_custom_tab_count();
         ++icon) {
        exact = audit_icon(icon) && exact;
    }
#endif
    if (!map_exact) {
        log_failure(
            "RGBA8 ref audit phase=%s found a host-bit/resource-index ledger mismatch (activeBits=0x%04X stagedBits=0x%04X overrideHost=%zu overrideAsset=%u overrideStaged=%u)",
            phase ? phase : "unknown",
            static_cast<unsigned>(rgba_hosts),
            static_cast<unsigned>(staged_rgba_hosts),
            override_host,
            static_cast<unsigned>(override_asset),
            override_staged ? 1U : 0U);
    }
    return exact;
}

void log_rgba8_lifecycle_snapshot(
    const char* phase,
    void* top) noexcept {
    // The full snapshot walks every retained asset and was invaluable while
    // proving ownership. In the 50-tab timing fixture it would perform that
    // diagnostic walk after every painted cell, so omit it from successful
    // stress runs. Transaction pre/postflight reference gates remain active.
#if defined(ER_NATIVE_UI_PRODUCTION_TABS)
    if (!erui::detail::trace_logging()) return;
#else
    if (kStressQuietSuccessLogging) return;
#endif

    constexpr std::uint32_t kMaximumLifecycleLogs = 512;
    const std::uint32_t event = g_rgba8_lifecycle_log_count.fetch_add(
        1, std::memory_order_relaxed) + 1;
    if (event > kMaximumLifecycleLogs) return;

    const TabHostMask hosts = g_injected_icon_hosts.load(
        std::memory_order_acquire);
    const TabHostMask rgba_hosts = g_rgba8_icon_hosts.load(
        std::memory_order_acquire);
    const TabHostMask staged_hosts =
        g_staged_injected_icon_hosts.load(std::memory_order_acquire);
    const TabHostMask staged_rgba_hosts =
        g_staged_rgba8_icon_hosts.load(std::memory_order_acquire);
    log_line(
        "rgba8Lifecycle[%u] phase=%s epoch=%llu top=%p catalogueReady=%u fingerprint={epoch:%llu interface:%p ownerLink:%p movieRoot:%p} hosts={activeBits:0x%04X activeLive:%u activeRgbaBits:0x%04X activeRgbaLive:%u stagedBits:0x%04X stagedLive:%u stagedRgbaBits:0x%04X stagedRgbaLive:%u} rgbaEvents={attach:%llu detach:%llu}",
        static_cast<unsigned>(event),
        phase ? phase : "unknown",
        static_cast<unsigned long long>(g_runtime.presentation_epoch),
        top,
        g_rgba8_catalogue_ready.load(std::memory_order_acquire) ? 1U : 0U,
        static_cast<unsigned long long>(
            g_injected_movie_fingerprint.presentation_epoch),
        g_injected_movie_fingerprint.object_interface,
        g_injected_movie_fingerprint.owner_link,
        g_injected_movie_fingerprint.movie_root,
        static_cast<unsigned>(hosts),
        static_cast<unsigned>(count_tracked_icon_hosts(hosts)),
        static_cast<unsigned>(rgba_hosts),
        static_cast<unsigned>(count_tracked_icon_hosts(rgba_hosts)),
        static_cast<unsigned>(staged_hosts),
        static_cast<unsigned>(count_tracked_icon_hosts(staged_hosts)),
        static_cast<unsigned>(staged_rgba_hosts),
        static_cast<unsigned>(count_tracked_icon_hosts(staged_rgba_hosts)),
        static_cast<unsigned long long>(
            g_rgba8_successful_attach_count.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            g_rgba8_successful_detach_count.load(std::memory_order_relaxed)));

    for (std::size_t host = 0; host < active_tab_host_count(); ++host) {
        const std::size_t encoded = g_rgba8_host_assets[host].load(
            std::memory_order_acquire);
        if (encoded == 0 || encoded > active_custom_tab_count()) continue;
        const std::size_t icon = encoded - 1U;
        log_line(
            "rgba8Lifecycle[%u] host={layer:%s physical:%zu} asset=%zu id=%d checksum=0x%016llX",
            static_cast<unsigned>(event),
            host < active_physical_tab_count() ? "front" : "back",
            host % active_physical_tab_count(),
            icon,
            kRgba8IconIds[icon],
            static_cast<unsigned long long>(
                g_retained_rgba8_checksums[icon].load(
                    std::memory_order_acquire)));
    }

    for (std::size_t icon = 0;
         icon < active_custom_tab_count();
         ++icon) {
        if (!catalogue_icon_requires_raster(icon)) continue;
        const std::uint32_t live_asset_hosts =
            count_active_rgba8_asset_hosts(icon);
        void* const resource = g_retained_rgba8_resources[icon].load(
            std::memory_order_acquire);
        void* const image = g_retained_rgba8_images[icon].load(
            std::memory_order_acquire);
        const std::uint64_t checksum =
            g_retained_rgba8_checksums[icon].load(
                std::memory_order_acquire);
        const std::uint32_t retirement_debt =
            g_rgba8_retirement_debt[icon].load(
                std::memory_order_acquire);
        std::uint32_t resource_ref{};
        std::uint32_t raw_ref{};
        // Passive, opaque RawImage scalar samples. Static analysis suggests
        // +0x10 participates in renderer realization, but this probe does not
        // treat that interpretation (or any sampled value) as a safety gate.
        void* image_opaque_10{};
        void* image_auxiliary_18{};
        void* image_auxiliary_20{};
        const bool resource_ref_read = resource && guarded_read(
            static_cast<const std::byte*>(resource) + 0x08,
            resource_ref);
        const bool raw_ref_read = image && guarded_read(
            static_cast<const std::byte*>(image) + 0x08,
            raw_ref);
        const bool renderer_fields_read = image &&
            guarded_read(
                static_cast<const std::byte*>(image) + 0x10,
                image_opaque_10) &&
            guarded_read(
                static_cast<const std::byte*>(image) + 0x18,
                image_auxiliary_18) &&
            guarded_read(
                static_cast<const std::byte*>(image) + 0x20,
                image_auxiliary_20);
        log_line(
            "rgba8Lifecycle[%u] asset=%zu id=%d sourceAsset=%zu resource=%p image=%p checksum=0x%016llX liveHosts=%u retirementDebt=%u refs={resourceRead:%u resource:%u rootExpected:1 rawRead:%u raw:%u rawExpected:%u} imageState={read:%u opaque10:%p opaque18:%p opaque20:%p}",
            static_cast<unsigned>(event),
            icon,
            kRgba8IconIds[icon],
            icon,
            resource,
            image,
            static_cast<unsigned long long>(checksum),
            static_cast<unsigned>(live_asset_hosts),
            static_cast<unsigned>(retirement_debt),
            resource_ref_read ? 1U : 0U,
            static_cast<unsigned>(resource_ref),
            raw_ref_read ? 1U : 0U,
            static_cast<unsigned>(raw_ref),
            static_cast<unsigned>(
                1U + live_asset_hosts + retirement_debt),
            renderer_fields_read ? 1U : 0U,
            image_opaque_10,
            image_auxiliary_18,
            image_auxiliary_20);
    }
    if (event == kMaximumLifecycleLogs) {
        log_line("Further RGBA8 lifecycle snapshots are suppressed");
    }
}

void** __fastcall texture_lookup_detour(
    void* repository,
    void** out_resource,
    const wchar_t* resource_name) noexcept {
    TextureLookupFn const original =
        g_original_texture_lookup.load(std::memory_order_acquire);
    if (!original) {
        if (out_resource) *out_resource = nullptr;
        return out_resource;
    }
    if (!g_viewport_callbacks_armed.load(std::memory_order_acquire)) {
        return original(repository, out_resource, resource_name);
    }

    VectorIconKind kind{VectorIconKind::none};
    const wchar_t* synthetic_resource{};
    std::size_t rgba8_index{invalid_tab_index};
    if (parse_rgba8_icon_resource_name(resource_name, rgba8_index)) {
        kind = VectorIconKind::cross;
        synthetic_resource =
            kRgba8IconResourceNames[rgba8_index].data();
    }
    if (exact_resource_name(resource_name, kOverflowIconResource)) {
        kind = VectorIconKind::ellipsis;
        synthetic_resource = kOverflowIconResource;
    }
    if (g_resolving_icon_alias || kind == VectorIconKind::none) {
        return original(repository, out_resource, resource_name);
    }
    g_pending_aliased_icon = {};

    // The retained RGBA8 resource follows the same ownership contract as a
    // repository hit: publish exactly one owned lookup reference. The native
    // presenter creates and consumes its own binder-local reference, then
    // releases this lookup reference after the bind returns. Our permanent
    // root therefore remains at refcount 1 between presentations.
    const bool exact_repaint_coordinate = g_repaint_context.active &&
        g_repaint_context.top &&
        g_repaint_context.top == g_runtime.top &&
        g_repaint_context.layer != TabLayer::none &&
        g_repaint_context.physical < active_physical_tab_count();
    const bool exact_repaint_context = exact_repaint_coordinate &&
        g_runtime.active;
    const std::size_t logical = g_repaint_context.logical;
    const bool logical_resolved = exact_repaint_context &&
        logical < active_logical_tab_count();
    const bool exact_catalogue_icon =
        rgba8_index < active_custom_tab_count() &&
        logical_resolved && logical == kStockLogicalTabCount + rgba8_index;
    const bool effective_raster = exact_catalogue_icon &&
        catalogue_icon_has_effective_raster(rgba8_index);
    const bool exact_initial_logical_raster =
        kind == VectorIconKind::cross && exact_repaint_coordinate &&
        rgba8_index < active_custom_tab_count() &&
        logical < active_logical_tab_count() &&
        logical == kStockLogicalTabCount + rgba8_index &&
        catalogue_icon_requires_raster(rgba8_index);
    const bool transparent_vector_placeholder =
        should_use_transparent_initial_raster_placeholder({
            g_initial_target_repaint,
            g_runtime.active,
            g_repaint_context.top == g_runtime.top,
            exact_repaint_coordinate,
            exact_initial_logical_raster,
            exact_initial_logical_raster &&
                catalogue_icon_has_effective_raster(rgba8_index),
        });
    void* const retained_resource = rgba8_index < active_custom_tab_count()
        ? g_retained_rgba8_resources[rgba8_index].load(
              std::memory_order_acquire)
        : nullptr;
    void* const retained_image = rgba8_index < active_custom_tab_count()
        ? g_retained_rgba8_images[rgba8_index].load(
              std::memory_order_acquire)
        : nullptr;
    const std::uint64_t retained_checksum =
        rgba8_index < active_custom_tab_count()
        ? g_retained_rgba8_checksums[rgba8_index].load(
              std::memory_order_acquire)
        : 0;
    if (kind == VectorIconKind::cross && effective_raster &&
        retained_resource && retained_image && out_resource &&
        !g_rgba8_publication_disabled.load(std::memory_order_acquire)) {
        void* resource_vtable{};
        std::uint32_t resource_ref_before{};
        void* exposed_image{};
        void* delegate_vtable{};
        std::uint32_t delegate_ref{};
        void* delegated_image{};
        std::uint32_t resource_kind{};
        void* image_vtable{};
        std::uint32_t image_ref_before{};
        void* prior_output{};
        const bool fields_read =
            guarded_read(retained_resource, resource_vtable) &&
            guarded_read(
                static_cast<const std::byte*>(retained_resource) + 0x08,
                resource_ref_before) &&
            guarded_read(
                static_cast<const std::byte*>(retained_resource) + 0x18,
                exposed_image) &&
            guarded_read(
                static_cast<const std::byte*>(retained_resource) + 0x20,
                delegate_vtable) &&
            guarded_read(
                static_cast<const std::byte*>(retained_resource) + 0x28,
                delegate_ref) &&
            guarded_read(
                static_cast<const std::byte*>(retained_resource) + 0x48,
                delegated_image) &&
            guarded_read(
                static_cast<const std::byte*>(retained_resource) + 0x60,
                resource_kind) &&
            guarded_read(retained_image, image_vtable) &&
            guarded_read(
                static_cast<const std::byte*>(retained_image) + 0x08,
                image_ref_before) &&
            guarded_read(out_resource, prior_output);
        // The native lookup treats this as a pure output parameter and writes
        // it unconditionally on every path. The presenter intentionally does
        // not initialize its stack slot, so prior_output is diagnostic only.
        const bool exact_resource = fields_read &&
            g_native_tabs_build_profile &&
            resource_vtable ==
                g_game.base + kScaleformImageResourceVtable271Rva &&
            exposed_image == retained_image &&
            delegate_vtable ==
                g_game.base + kScaleformImageDelegateVtable271Rva &&
            delegate_ref == 1U && delegated_image == nullptr &&
            resource_kind == 1U &&
            image_vtable == g_game.base + kRawImageVtableRva &&
            resource_ref_before == 1U && image_ref_before >= 1U;

        unsigned long add_ref_seh{};
        bool add_ref_completed{};
        std::uint32_t resource_ref_after{};
        bool ref_after_read{};
        bool output_written{};
        bool output_verified{};
        if (exact_resource) {
            add_ref_completed =
                guarded_add_ref_scaleform_image_resource(
                    reinterpret_cast<ScaleformImageResourceAddRefFn>(
                        g_game.base +
                        kScaleformImageResourceAddRef271Rva),
                    retained_resource,
                    add_ref_seh);
            ref_after_read = add_ref_completed && guarded_read(
                static_cast<const std::byte*>(retained_resource) + 0x08,
                resource_ref_after);
            const bool exact_add_ref = ref_after_read &&
                resource_ref_after == resource_ref_before + 1U;
            if (exact_add_ref) {
                output_written = guarded_copy(
                    out_resource,
                    &retained_resource,
                    sizeof(retained_resource));
                void* published{};
                output_verified = output_written &&
                    guarded_read(out_resource, published) &&
                    published == retained_resource;
            }
        }

        const std::uint32_t event =
            g_rgba8_lookup_log_count.fetch_add(
                1, std::memory_order_relaxed) + 1;
        log_line(
            "rgba8Lookup[%u] epoch=%llu icon=%zu logical=%zu checksum=0x%016llX context={layer:%u physical:%zu} resource=%p image=%p fieldsRead=%u exact=%u layout={delegate:%p delegateRef:%u delegated:%p kind:%u priorOutputIgnored:%p} addRef=%u seh=0x%08lX refs={before:%u afterRead:%u after:%u} imageRef=%u output={written:%u verified:%u}",
            static_cast<unsigned>(event),
            static_cast<unsigned long long>(
                g_runtime.presentation_epoch),
            rgba8_index,
            logical,
            static_cast<unsigned long long>(retained_checksum),
            static_cast<unsigned>(g_repaint_context.layer),
            g_repaint_context.physical,
            retained_resource,
            retained_image,
            fields_read ? 1U : 0U,
            exact_resource ? 1U : 0U,
            delegate_vtable,
            static_cast<unsigned>(delegate_ref),
            delegated_image,
            static_cast<unsigned>(resource_kind),
            prior_output,
            add_ref_completed ? 1U : 0U,
            add_ref_seh,
            static_cast<unsigned>(resource_ref_before),
            ref_after_read ? 1U : 0U,
            static_cast<unsigned>(resource_ref_after),
            static_cast<unsigned>(image_ref_before),
            output_written ? 1U : 0U,
            output_verified ? 1U : 0U);

        // A successful guarded write is the ownership handoff. The readback is
        // diagnostic only; never Release a reference after its pointer has
        // become caller-visible, even if that diagnostic read unexpectedly
        // fails.
        if (output_written) {
            g_pending_aliased_icon = {
                retained_resource,
                kind,
                g_repaint_context,
                true,
                false,
                rgba8_index,
                logical,
                retained_checksum,
                g_runtime.presentation_epoch,
            };
            return out_resource;
        }

        // AddRef succeeded but publication did not. Balance only the reference
        // owned by this failed lookup attempt; the process root stays alive.
        if (add_ref_completed) {
            unsigned long rollback_seh{};
            const bool rolled_back =
                guarded_release_scaleform_image_resource(
                    reinterpret_cast<ScaleformImageResourceReleaseFn>(
                        g_game.base +
                        kScaleformImageResourceRelease271Rva),
                    retained_resource,
                    rollback_seh);
            log_line(
                "rgba8Lookup[%u] rollback completed=%u seh=0x%08lX",
                static_cast<unsigned>(event),
                rolled_back ? 1U : 0U,
                rollback_seh);
        }
        g_rgba8_publication_disabled.store(true, std::memory_order_release);
        log_failure(
            "direct RGBA8 lookup failed closed; later custom icons use the proven vector fallback");
    }

    // Call the SafetyHook trampoline with a different, known-native name. The
    // guard also makes this safe if the lookup implementation gains an
    // indirect recursive path during later experiments.
    g_resolving_icon_alias = true;
    void** const result = original(
        repository, out_resource, kAliasedNativeIconResource);
    g_resolving_icon_alias = false;

    void* resolved{};
    (void)guarded_read(out_resource, resolved);
    if (resolved) {
        g_pending_aliased_icon = {
            resolved,
            kind,
            g_repaint_context,
            false,
            transparent_vector_placeholder,
            rgba8_index,
            logical,
            retained_checksum,
            g_runtime.presentation_epoch,
        };
    }
    const std::uint32_t event =
        g_alias_log_count.fetch_add(1, std::memory_order_relaxed) + 1;
    if (event <= kMaximumAliasLogs) {
        log_line(
            "iconAlias[%u] epoch=%llu kind=%u rgbaIcon=%zu logical=%zu exactCatalogue=%u transparentPlaceholder=%u context={active:%u layer:%u physical:%zu} %ls -> %ls resource=%p",
            static_cast<unsigned>(event),
            static_cast<unsigned long long>(
                g_runtime.presentation_epoch),
            static_cast<unsigned>(kind),
            rgba8_index,
            logical,
            exact_catalogue_icon ? 1U : 0U,
            transparent_vector_placeholder ? 1U : 0U,
            g_repaint_context.active ? 1U : 0U,
            static_cast<unsigned>(g_repaint_context.layer),
            g_repaint_context.physical,
            synthetic_resource,
            kAliasedNativeIconResource,
            resolved);
    }
    if (event == kMaximumAliasLogs) {
        log_line("Further synthetic-icon alias events are suppressed");
    }
    return result;
}

bool guarded_create_raw_image(
    RawImageCreateFn create,
    const RawImageSize& size,
    void*& image,
    unsigned long& seh_code) noexcept {
    image = nullptr;
    seh_code = 0;
    if (!create) return false;
#if defined(_MSC_VER)
    __try {
#endif
        image = create(
            1U, // Scaleform::Render::Image_R8G8B8A8
            1U, // one actual top-level mip level
            &size,
            0U, // immutable system-memory image
            nullptr,
            nullptr);
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        seh_code = static_cast<unsigned long>(GetExceptionCode());
        image = nullptr;
    }
#endif
    return image != nullptr && seh_code == 0;
}

bool guarded_get_raw_image_data(
    RawImageGetDataFn get_data,
    void* image,
    RawImageData& data,
    bool& returned,
    unsigned long& seh_code) noexcept {
    returned = false;
    seh_code = 0;
    if (!get_data || !image) return false;
#if defined(_MSC_VER)
    __try {
#endif
        returned = get_data(image, &data);
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        seh_code = static_cast<unsigned long>(GetExceptionCode());
        returned = false;
    }
#endif
    return seh_code == 0;
}

bool guarded_clear_raw_image_data(
    ImageDataClearFn clear,
    RawImageData& data,
    unsigned long& seh_code) noexcept {
    seh_code = 0;
    if (!clear) return false;
#if defined(_MSC_VER)
    __try {
#endif
        clear(&data);
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        seh_code = static_cast<unsigned long>(GetExceptionCode());
    }
#endif
    return seh_code == 0;
}

bool guarded_release_raw_image(
    RawImageReleaseFn release,
    void* image,
    unsigned long& seh_code) noexcept {
    seh_code = 0;
    if (!release || !image) return false;
#if defined(_MSC_VER)
    __try {
#endif
        release(image);
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        seh_code = static_cast<unsigned long>(GetExceptionCode());
    }
#endif
    return seh_code == 0;
}

bool guarded_allocate_scaleform_image_resource(
    NativeHeapAllocFn allocate,
    void* heap,
    void*& storage,
    unsigned long& seh_code) noexcept {
    storage = nullptr;
    seh_code = 0;
    if (!allocate || !heap) return false;
#if defined(_MSC_VER)
    __try {
#endif
        storage = allocate(
            heap,
            kScaleformImageResourceSize,
            nullptr);
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        seh_code = static_cast<unsigned long>(GetExceptionCode());
        storage = nullptr;
    }
#endif
    return storage != nullptr && seh_code == 0;
}

bool guarded_construct_scaleform_image_resource(
    ScaleformImageResourceCtorFn construct,
    void* storage,
    void* image,
    const wchar_t* name,
    void*& result,
    unsigned long& seh_code) noexcept {
    result = nullptr;
    seh_code = 0;
    if (!construct || !storage || !image || !name) return false;
#if defined(_MSC_VER)
    __try {
#endif
        result = construct(
            storage,
            name,
            image,
            1U);
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        seh_code = static_cast<unsigned long>(GetExceptionCode());
        result = nullptr;
    }
#endif
    return result == storage && seh_code == 0;
}

bool guarded_add_ref_scaleform_image_resource(
    ScaleformImageResourceAddRefFn add_ref,
    void* resource,
    unsigned long& seh_code) noexcept {
    seh_code = 0;
    if (!add_ref || !resource) return false;
#if defined(_MSC_VER)
    __try {
#endif
        add_ref(resource);
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        seh_code = static_cast<unsigned long>(GetExceptionCode());
    }
#endif
    return seh_code == 0;
}

bool guarded_release_scaleform_image_resource(
    ScaleformImageResourceReleaseFn release,
    void* resource,
    unsigned long& seh_code) noexcept {
    seh_code = 0;
    if (!release || !resource) return false;
#if defined(_MSC_VER)
    __try {
#endif
        release(resource);
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        seh_code = static_cast<unsigned long>(GetExceptionCode());
    }
#endif
    return seh_code == 0;
}

void run_raw_image_lifecycle_probe_one(
    void* top,
    std::size_t icon_index,
    RetainedRgba8Resource& retained) noexcept {
    retained = {};
    if (!kEnableRawImageLifecycleProbe || !top || !g_runtime.active ||
        g_runtime.top != top || !validate_top(top) ||
        icon_index >= active_custom_tab_count() ||
        !catalogue_icon_requires_raster(icon_index)) {
        return;
    }

    constexpr std::array<std::uint8_t, 20> kRawImageCreateEntry{
        0x89, 0x54, 0x24, 0x10, 0x89, 0x4C, 0x24, 0x08,
        0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x57,
        0x48, 0x83, 0xEC, 0x40};
    constexpr std::array<std::uint8_t, 26> kRawImageGetDataEntry{
        0x48, 0x83, 0xEC, 0x28, 0x48, 0x8B, 0xC2, 0x48,
        0x8D, 0x51, 0x28, 0x48, 0x8B, 0xC8, 0xE8, 0x1D,
        0xBF, 0xFF, 0xFF, 0xB0, 0x01, 0x48, 0x83, 0xC4,
        0x28, 0xC3};
    constexpr std::array<std::uint8_t, 20> kImageDataClearEntry{
        0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C,
        0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57,
        0x48, 0x83, 0xEC, 0x20};
    constexpr std::array<std::uint8_t, 16> kRawImageReleaseEntry{
        0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B,
        0xD9, 0x83, 0xCA, 0xFF, 0x48, 0x83, 0xC1, 0x08};

    if (!g_native_tabs_build_profile) return;
    const std::uintptr_t create_rva = kRawImageCreate271Rva;
    const std::uintptr_t get_data_rva = kRawImageGetData271Rva;
    const std::uintptr_t clear_rva = kImageDataClear271Rva;
    const std::uintptr_t expected_release_rva = kRawImageRelease271Rva;
    const bool exact_interfaces =
        entry_matches(create_rva, kRawImageCreateEntry) &&
        entry_matches(get_data_rva, kRawImageGetDataEntry) &&
        entry_matches(clear_rva, kImageDataClearEntry) &&
        entry_matches(expected_release_rva, kRawImageReleaseEntry) &&
        image_contains(kRawImageVtableRva, 0x18) &&
        image_contains(expected_release_rva, kRawImageReleaseEntry.size());
    log_line(
        "rawImageLifecycle begin icon=%zu id=%d thread=%lu profile=%s interfacesExact=%u exposed=0 bound=0",
        icon_index,
        kRgba8IconIds[icon_index],
        static_cast<unsigned long>(GetCurrentThreadId()),
        tabs_build().version,
        exact_interfaces ? 1U : 0U);
    if (!exact_interfaces) {
        log_failure(
            "RawImage lifecycle interfaces differ from the validated executable; no native call was made");
        return;
    }

    RawImageCreateFn const create = reinterpret_cast<RawImageCreateFn>(
        g_game.base + create_rva);
    RawImageGetDataFn const get_data =
        reinterpret_cast<RawImageGetDataFn>(g_game.base + get_data_rva);
    ImageDataClearFn const clear = reinterpret_cast<ImageDataClearFn>(
        g_game.base + clear_rva);
    constexpr RawImageSize kSize{
        static_cast<std::uint32_t>(kProbeBitmapWidth),
        static_cast<std::uint32_t>(kProbeBitmapHeight)};

    void* image{};
    unsigned long create_seh{};
    const bool created = guarded_create_raw_image(
        create, kSize, image, create_seh);
    if (!created) {
        log_failure(
            "RawImage::Create failed closed (image=%p seh=0x%08lX); nothing was exposed",
            image,
            create_seh);
        return;
    }

    void* vtable{};
    std::uint32_t refcount{};
    void* release_slot{};
    const bool object_fields_read =
        guarded_read(image, vtable) &&
        guarded_read(
            static_cast<const std::byte*>(image) + 0x08,
            refcount) &&
        vtable &&
        guarded_read(
            static_cast<const std::byte*>(vtable) + 0x10,
            release_slot);
    const bool exact_object = object_fields_read &&
        vtable == g_game.base + kRawImageVtableRva &&
        release_slot == g_game.base + expected_release_rva &&
        refcount == 1U;
    log_line(
        "rawImageLifecycle object image=%p reads=%u exact=%u vtable=%p rva=0x%llX ref=%u release=%p rva=0x%llX createSeh=0x%08lX",
        image,
        object_fields_read ? 1U : 0U,
        exact_object ? 1U : 0U,
        vtable,
        static_cast<unsigned long long>(
            locate_game_pointer(vtable).rva),
        static_cast<unsigned>(refcount),
        release_slot,
        static_cast<unsigned long long>(
            locate_game_pointer(release_slot).rva),
        create_seh);
    if (!exact_object) {
        log_failure(
            "RawImage object identity was unexpected; the unexposed allocation is intentionally retained rather than calling an unproven release slot");
        return;
    }

    RawImageData view{};
    view.raw_plane_count = 1;
    view.planes = &view.plane0;
    bool get_data_returned{};
    unsigned long get_data_seh{};
    const bool get_data_call_completed = guarded_get_raw_image_data(
        get_data,
        image,
        view,
        get_data_returned,
        get_data_seh);

    RawImagePlane plane{};
    bool plane_read{};
    if (get_data_call_completed && get_data_returned && view.planes) {
        plane_read = guarded_read(view.planes, plane);
    }
    constexpr std::uintptr_t kRowBytes =
        static_cast<std::uintptr_t>(kProbeBitmapWidth) * 4U;
    const bool plane_size_multiplication_safe = plane.pitch == 0 ||
        static_cast<std::uintptr_t>(kProbeBitmapHeight) <=
            (std::numeric_limits<std::uintptr_t>::max)() / plane.pitch;
    const std::uintptr_t required_data_size =
        plane_size_multiplication_safe
        ? plane.pitch * static_cast<std::uintptr_t>(kProbeBitmapHeight)
        : 0;
    const std::uintptr_t data_address =
        reinterpret_cast<std::uintptr_t>(plane.data);
    const bool plane_address_addition_safe = plane.data != nullptr &&
        required_data_size <=
            (std::numeric_limits<std::uintptr_t>::max)() - data_address;
    const bool exact_data = get_data_call_completed &&
        get_data_returned && get_data_seh == 0 &&
        view.format == 1U && view.use == 0U &&
        view.flags == 0U && view.level_count == 1U &&
        view.raw_plane_count == 1U &&
        view.planes == &view.plane0 && view.palette == nullptr &&
        plane_read &&
        plane.width == kSize.width && plane.height == kSize.height &&
        plane.pitch >= kRowBytes && plane_size_multiplication_safe &&
        plane.data_size >= required_data_size &&
        plane_address_addition_safe;
    log_line(
        "rawImageLifecycle data call=%u returned=%u seh=0x%08lX exact=%u format=%u use=0x%08X flags=0x%02X levels=%u planes=%u planeTable=%p inlinePlane=%p palette=%p planeRead=%u plane={width:%u height:%u pitch:%llu size:%llu data:%p}",
        get_data_call_completed ? 1U : 0U,
        get_data_returned ? 1U : 0U,
        get_data_seh,
        exact_data ? 1U : 0U,
        static_cast<unsigned>(view.format),
        static_cast<unsigned>(view.use),
        static_cast<unsigned>(view.flags),
        static_cast<unsigned>(view.level_count),
        static_cast<unsigned>(view.raw_plane_count),
        view.planes,
        static_cast<void*>(&view.plane0),
        view.palette,
        plane_read ? 1U : 0U,
        static_cast<unsigned>(plane.width),
        static_cast<unsigned>(plane.height),
        static_cast<unsigned long long>(plane.pitch),
        static_cast<unsigned long long>(plane.data_size),
        plane.data);

    constexpr std::size_t kProbeRowBytes =
        static_cast<std::size_t>(kProbeBitmapWidth) * 4U;
    std::array<std::byte, kProbeRowBytes> expected_row{};
    std::array<std::byte, kProbeRowBytes> observed_row{};
    bool pixels_written = exact_data;
    bool pixels_verified = exact_data;
    std::uint64_t expected_checksum = 1469598103934665603ULL;
    std::uint64_t observed_checksum = 1469598103934665603ULL;
    for (std::uint32_t y = 0;
         exact_data && y < kSize.height;
         ++y) {
        for (std::uint32_t x = 0; x < kSize.width; ++x) {
            const Rgba8Pixel pixel = rgba8_probe_pixel(icon_index, x, y);
            const std::size_t offset = static_cast<std::size_t>(x) * 4U;
            expected_row[offset + 0] = static_cast<std::byte>(pixel.red);
            expected_row[offset + 1] = static_cast<std::byte>(pixel.green);
            expected_row[offset + 2] = static_cast<std::byte>(pixel.blue);
            expected_row[offset + 3] = static_cast<std::byte>(pixel.alpha);
        }
        for (const std::byte value : expected_row) {
            expected_checksum ^= static_cast<std::uint8_t>(value);
            expected_checksum *= 1099511628211ULL;
        }
        std::byte* const destination = plane.data +
            static_cast<std::uintptr_t>(y) * plane.pitch;
        if (!guarded_copy(
                destination, expected_row.data(), expected_row.size())) {
            pixels_written = false;
            pixels_verified = false;
            break;
        }
        observed_row.fill(std::byte{});
        if (!guarded_copy(
                observed_row.data(), destination, observed_row.size())) {
            pixels_verified = false;
            break;
        }
        if (observed_row != expected_row) {
            pixels_verified = false;
            break;
        }
        for (const std::byte value : observed_row) {
            observed_checksum ^= static_cast<std::uint8_t>(value);
            observed_checksum *= 1099511628211ULL;
        }
    }
    pixels_verified = pixels_verified &&
        expected_checksum == observed_checksum;
    log_line(
        "rawImageLifecycle pixels icon=%zu id=%d written=%u verified=%u rows=%u rowBytes=%zu checksum={expected:0x%016llX observed:0x%016llX} alpha=diagnostic-ramp exposed=0 bound=0",
        icon_index,
        kRgba8IconIds[icon_index],
        pixels_written ? 1U : 0U,
        pixels_verified ? 1U : 0U,
        static_cast<unsigned>(kSize.height),
        kProbeRowBytes,
        static_cast<unsigned long long>(expected_checksum),
        static_cast<unsigned long long>(observed_checksum));

    bool view_cleared{};
    unsigned long clear_seh{};
    if (get_data_call_completed) {
        view_cleared = guarded_clear_raw_image_data(
            clear, view, clear_seh);
    }

    bool image_released{};
    unsigned long release_seh{};
    bool resource_probe_attempted{};
    bool resource_probe_completed{};
    bool visual_resource_retained{};
    bool uncertain_objects_retained{};
    if (kEnableNativeImageResourceLifecycleProbe &&
        exact_data && pixels_written && pixels_verified && view_cleared) {
        resource_probe_attempted = true;

        constexpr std::array<std::uint8_t, 6> kRawImageGetTypeEntry{
            0xB8, 0x02, 0x00, 0x00, 0x00, 0xC3};
        constexpr std::array<std::uint8_t, 33>
            kScaleformImageResourceCtorEntry{
                0x48, 0x89, 0x4C, 0x24, 0x08, 0x57, 0x48, 0x83,
                0xEC, 0x30, 0x48, 0xC7, 0x44, 0x24, 0x20, 0xFE,
                0xFF, 0xFF, 0xFF, 0x48, 0x89, 0x5C, 0x24, 0x48,
                0x49, 0x8B, 0xC0, 0x48, 0x8B, 0xDA, 0x48, 0x8B,
                0xF9};
        constexpr std::array<std::uint8_t, 14>
            kScaleformImageResourceAddRefEntry{
                0x48, 0x83, 0xC1, 0x08, 0xBA, 0x01, 0x00,
                0x00, 0x00, 0xE9, 0x32, 0x50, 0x00, 0x00};
        constexpr std::array<std::uint8_t, 16>
            kScaleformImageResourceReleaseEntry{
                0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B,
                0xD9, 0x83, 0xCA, 0xFF, 0x48, 0x83, 0xC1, 0x08};
        constexpr std::array<std::uint8_t, 25>
            kScaleformImageResourceDeletingDestructorEntry{
                0x40, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x48, 0xC7,
                0x44, 0x24, 0x20, 0xFE, 0xFF, 0xFF, 0xFF, 0x48,
                0x89, 0x5C, 0x24, 0x40, 0x8B, 0xFA, 0x48, 0x8B,
                0xD9};
        // The RIP displacement necessarily changes when the global-heap data
        // slot moves. Validate the instruction semantically against the
        // profile-selected slot, then validate its unchanged allocation tail.
        constexpr std::array<std::uint8_t, 14>
            kScaleformImageResourceAllocationTail{
                0x48, 0x8B, 0x01, 0x45, 0x33, 0xC0, 0xBA,
                0x98, 0x00, 0x00, 0x00, 0xFF, 0x50, 0x50};

        void* raw_type_slot{};
        void* resource_deleting_destructor{};
        const bool static_fields_read =
            guarded_read(
                static_cast<const std::byte*>(vtable) + 0x18,
                raw_type_slot) &&
            guarded_read(
                g_game.base + kScaleformImageResourceVtable271Rva,
                resource_deleting_destructor);
        std::uintptr_t allocation_heap_rva{};
        const bool exact_allocation_call =
            resolve_rip_relative_data_target(
                kScaleformImageResourceAllocationCall271Rva,
                allocation_heap_rva) &&
            allocation_heap_rva ==
                static_cast<std::uintptr_t>(kScaleformGlobalHeap271Rva) &&
            entry_matches(
                static_cast<std::uintptr_t>(
                    kScaleformImageResourceAllocationCall271Rva) + 7U,
                kScaleformImageResourceAllocationTail);
        const bool exact_resource_interfaces = static_fields_read &&
            raw_type_slot == g_game.base + kRawImageGetType271Rva &&
            resource_deleting_destructor ==
                g_game.base +
                    kScaleformImageResourceDeletingDestructor271Rva &&
            entry_matches(
                kRawImageGetType271Rva,
                kRawImageGetTypeEntry) &&
            entry_matches(
                kScaleformImageResourceCtor271Rva,
                kScaleformImageResourceCtorEntry) &&
            entry_matches(
                kScaleformImageResourceAddRef271Rva,
                kScaleformImageResourceAddRefEntry) &&
            entry_matches(
                kScaleformImageResourceRelease271Rva,
                kScaleformImageResourceReleaseEntry) &&
            entry_matches(
                kScaleformImageResourceDeletingDestructor271Rva,
                kScaleformImageResourceDeletingDestructorEntry) &&
            exact_allocation_call &&
            image_contains(kScaleformImageResourceVtable271Rva, 0x08) &&
            image_contains(kScaleformImageDelegateVtable271Rva, 0x08) &&
            image_contains(kScaleformGlobalHeap271Rva, sizeof(void*));
        log_line(
            "nativeImageResourceLifecycle begin thread=%lu profile=%s interfacesExact=%u raw=%p rawType=%p/0x%llX deletingDestructor=%p/0x%llX exposed=0 bound=0",
            static_cast<unsigned long>(GetCurrentThreadId()),
            tabs_build().version,
            exact_resource_interfaces ? 1U : 0U,
            image,
            raw_type_slot,
            static_cast<unsigned long long>(
                locate_game_pointer(raw_type_slot).rva),
            resource_deleting_destructor,
            static_cast<unsigned long long>(
                locate_game_pointer(resource_deleting_destructor).rva));

        if (exact_resource_interfaces) {
            void* heap{};
            void* heap_vtable{};
            void* allocation_slot{};
            const bool heap_fields_read =
                guarded_read(
                    g_game.base + kScaleformGlobalHeap271Rva,
                    heap) &&
                guarded_read(heap, heap_vtable) &&
                guarded_read(
                    static_cast<const std::byte*>(heap_vtable) + 0x50,
                    allocation_slot);
            const GamePointerLocation allocation_location =
                locate_game_pointer(allocation_slot);
            const bool exact_heap = heap_fields_read && heap &&
                heap_vtable && allocation_slot &&
                allocation_location.in_image;
            log_line(
                "nativeImageResourceLifecycle heap reads=%u exact=%u heap=%p vtable=%p allocate=%p/0x%llX size=0x%zX arg3=null",
                heap_fields_read ? 1U : 0U,
                exact_heap ? 1U : 0U,
                heap,
                heap_vtable,
                allocation_slot,
                static_cast<unsigned long long>(allocation_location.rva),
                kScaleformImageResourceSize);

            if (exact_heap) {
                void* storage{};
                unsigned long allocation_seh{};
                const bool allocated =
                    guarded_allocate_scaleform_image_resource(
                        reinterpret_cast<NativeHeapAllocFn>(allocation_slot),
                        heap,
                        storage,
                        allocation_seh);
                log_line(
                    "nativeImageResourceLifecycle allocation completed=%u storage=%p seh=0x%08lX",
                    allocated ? 1U : 0U,
                    storage,
                    allocation_seh);

                if (allocated) {
                    void* constructed_resource{};
                    unsigned long constructor_seh{};
                    const bool constructed =
                        guarded_construct_scaleform_image_resource(
                            reinterpret_cast<
                                ScaleformImageResourceCtorFn>(
                                    g_game.base +
                                    kScaleformImageResourceCtor271Rva),
                            storage,
                            image,
                            kRawImageResourceProbeNames[icon_index].data(),
                            constructed_resource,
                            constructor_seh);

                    void* resource_vtable{};
                    std::uint32_t resource_refcount{};
                    void* exposed_image{};
                    void* delegate_vtable{};
                    std::uint32_t delegate_refcount{};
                    void* delegated_image{};
                    std::uint32_t resource_kind{};
                    std::uint32_t raw_refcount_after_constructor{};
                    const bool resource_fields_read = constructed &&
                        guarded_read(
                            constructed_resource,
                            resource_vtable) &&
                        guarded_read(
                            static_cast<const std::byte*>(
                                constructed_resource) + 0x08,
                            resource_refcount) &&
                        guarded_read(
                            static_cast<const std::byte*>(
                                constructed_resource) + 0x18,
                            exposed_image) &&
                        guarded_read(
                            static_cast<const std::byte*>(
                                constructed_resource) + 0x20,
                            delegate_vtable) &&
                        guarded_read(
                            static_cast<const std::byte*>(
                                constructed_resource) + 0x28,
                            delegate_refcount) &&
                        guarded_read(
                            static_cast<const std::byte*>(
                                constructed_resource) + 0x48,
                            delegated_image) &&
                        guarded_read(
                            static_cast<const std::byte*>(
                                constructed_resource) + 0x60,
                            resource_kind) &&
                        guarded_read(
                            static_cast<const std::byte*>(image) + 0x08,
                            raw_refcount_after_constructor);
                    const bool exact_resource = resource_fields_read &&
                        constructed_resource == storage &&
                        resource_vtable ==
                            g_game.base +
                                kScaleformImageResourceVtable271Rva &&
                        resource_refcount == 1U &&
                        exposed_image == image &&
                        delegate_vtable ==
                            g_game.base +
                                kScaleformImageDelegateVtable271Rva &&
                        delegate_refcount == 1U &&
                        delegated_image == nullptr &&
                        resource_kind == 1U &&
                        raw_refcount_after_constructor == 2U;
                    log_line(
                        "nativeImageResourceLifecycle constructor completed=%u result=%p storage=%p seh=0x%08lX fieldsRead=%u exact=%u resource={vtable:%p/0x%llX ref:%u exposed:%p delegateVtable:%p/0x%llX delegateRef:%u delegated:%p kind:%u} rawRef=%u exposedToUi=0 bound=0",
                        constructed ? 1U : 0U,
                        constructed_resource,
                        storage,
                        constructor_seh,
                        resource_fields_read ? 1U : 0U,
                        exact_resource ? 1U : 0U,
                        resource_vtable,
                        static_cast<unsigned long long>(
                            locate_game_pointer(resource_vtable).rva),
                        static_cast<unsigned>(resource_refcount),
                        exposed_image,
                        delegate_vtable,
                        static_cast<unsigned long long>(
                            locate_game_pointer(delegate_vtable).rva),
                        static_cast<unsigned>(delegate_refcount),
                        delegated_image,
                        static_cast<unsigned>(resource_kind),
                        static_cast<unsigned>(
                            raw_refcount_after_constructor));

                    if (exact_resource) {
                        unsigned long creator_release_seh{};
                        const bool creator_released =
                            guarded_release_raw_image(
                                reinterpret_cast<RawImageReleaseFn>(
                                    release_slot),
                                image,
                                creator_release_seh);
                        std::uint32_t raw_refcount_after_creator_release{};
                        const bool remaining_ref_read = creator_released &&
                            guarded_read(
                                static_cast<const std::byte*>(image) +
                                    0x08,
                                raw_refcount_after_creator_release);
                        const bool ownership_handoff_exact =
                            remaining_ref_read &&
                            raw_refcount_after_creator_release == 1U;
                        log_line(
                            "nativeImageResourceLifecycle ownership creatorReleased=%u seh=0x%08lX remainingRefRead=%u rawRef=%u exact=%u",
                            creator_released ? 1U : 0U,
                            creator_release_seh,
                            remaining_ref_read ? 1U : 0U,
                            static_cast<unsigned>(
                                raw_refcount_after_creator_release),
                            ownership_handoff_exact ? 1U : 0U);

                        if (ownership_handoff_exact) {
                            // Stage this exact root. The outer catalogue gate
                            // publishes none of the resources until every
                            // requested entry has passed this same
                            // ownership handoff.
                            retained.image = image;
                            retained.resource = constructed_resource;
                            retained.checksum = observed_checksum;
                            retained.ready = true;
                            visual_resource_retained = true;
                            resource_probe_completed = true;
                            log_line(
                                "rgba8Resource staged icon=%zu id=%d resource=%p image=%p resourceRef=1 rawRef=1 checksum=0x%016llX intentionalProcessLifetime=1 exposed=0 bound=0",
                                icon_index,
                                kRgba8IconIds[icon_index],
                                constructed_resource,
                                image,
                                static_cast<unsigned long long>(
                                    observed_checksum));
                        } else {
                            uncertain_objects_retained = true;
                        }
                    } else {
                        // Construction may have partially transferred
                        // ownership even if it faulted or a postcondition did
                        // not match. Retain both bounded one-shot allocations
                        // rather than guessing which destructor is safe.
                        uncertain_objects_retained = true;
                    }
                }
            }
        }
    }

    // If the wrapper experiment never obtained storage, RawImage ownership is
    // still exactly the already-proven single creator reference. Preserve the
    // original cleanup path. An uncertain constructed object deliberately
    // blocks this fallback to avoid a double Release.
    if (get_data_call_completed && view_cleared &&
        !image_released && !visual_resource_retained &&
        !uncertain_objects_retained) {
        image_released = guarded_release_raw_image(
            reinterpret_cast<RawImageReleaseFn>(release_slot),
            image,
            release_seh);
    }
    const bool completed = exact_data && pixels_written &&
        pixels_verified && view_cleared &&
        (image_released || visual_resource_retained) &&
        (!resource_probe_attempted || resource_probe_completed);
    log_line(
        "rawImageLifecycle cleanup viewCleared=%u clearSeh=0x%08lX imageReleased=%u fallbackReleaseSeh=0x%08lX resourceProbe={attempted:%u completed:%u} visualCacheRetained=%u uncertainRetained=%u completed=%u exposed=0 bound=0",
        view_cleared ? 1U : 0U,
        clear_seh,
        image_released ? 1U : 0U,
        release_seh,
        resource_probe_attempted ? 1U : 0U,
        resource_probe_completed ? 1U : 0U,
        visual_resource_retained ? 1U : 0U,
        uncertain_objects_retained ? 1U : 0U,
        completed ? 1U : 0U);
    if (!completed) {
        log_failure(
            "RawImage/ImageResource lifecycle gate did not complete; renderer publication remains disabled");
    }
}

void run_raw_image_lifecycle_probe(void* top) noexcept {
    if (!kEnableRawImageLifecycleProbe || !top || !g_runtime.active ||
        g_runtime.top != top || !validate_top(top)) {
        return;
    }
    if (g_raw_image_lifecycle_probe_attempted.exchange(
            true, std::memory_order_acq_rel)) {
        return;
    }

    const std::size_t custom_count = active_custom_tab_count();
    // These two vectors are private construction-transaction scratch, not
    // another runtime catalogue. Their exact registered size lets us retain
    // the all-or-nothing publication gate without imposing the public ceiling
    // on every process. All persistent state is owned by tabs[index].
    std::vector<std::uint64_t> expected_checksums;
    std::vector<RetainedRgba8Resource> staged;
    try {
        expected_checksums.resize(custom_count);
        staged.resize(custom_count);
    } catch (...) {
        g_rgba8_catalogue_ready.store(false, std::memory_order_release);
        g_rgba8_publication_disabled.store(true, std::memory_order_release);
        log_failure(
            "RGBA8 catalogue could not allocate an exact-sized %zu-tab construction transaction; raster publication remains disabled",
            custom_count);
        return;
    }

    const std::uint64_t catalogue_started_qpc = query_performance_ticks();
    std::uint64_t slowest_icon_microseconds{};
    std::size_t slowest_icon{};
    std::size_t ready_count{};
    std::size_t raster_count{};
    bool procedural_catalogue_valid{true};
    for (std::size_t icon = 0;
         icon < custom_count && procedural_catalogue_valid;
         ++icon) {
        if (!catalogue_icon_requires_raster(icon)) continue;
        expected_checksums[icon] = rgba8_probe_icon_checksum(icon);
#if !defined(ER_NATIVE_UI_PRODUCTION_TABS)
        procedural_catalogue_valid = expected_checksums[icon] != 0;
#endif
        ++raster_count;
    }
#if !defined(ER_NATIVE_UI_PRODUCTION_TABS)
    procedural_catalogue_valid = procedural_catalogue_valid &&
        rgba8_probe_catalogue_is_unique_and_alpha_profiled(
            expected_checksums);
#endif
    bool all_ready{procedural_catalogue_valid};
    for (std::size_t icon = 0;
         icon < custom_count;
         ++icon) {
        if (!catalogue_icon_requires_raster(icon)) continue;
        const std::uint64_t icon_started_qpc = query_performance_ticks();
        run_raw_image_lifecycle_probe_one(top, icon, staged[icon]);
        const std::uint64_t icon_microseconds = elapsed_qpc_microseconds(
            icon_started_qpc, query_performance_ticks());
        if (icon_microseconds > slowest_icon_microseconds) {
            slowest_icon_microseconds = icon_microseconds;
            slowest_icon = icon;
        }
        const bool entry_ready = staged[icon].ready &&
            staged[icon].image && staged[icon].resource &&
            staged[icon].checksum == expected_checksums[icon];
        all_ready = all_ready && entry_ready;
        if (entry_ready) {
            ++ready_count;
        }
    }

    bool identities_unique{true};
    for (std::size_t icon = 0;
         icon < custom_count;
         ++icon) {
        if (!catalogue_icon_requires_raster(icon)) continue;
        for (std::size_t other = 0; other < icon; ++other) {
            if (!catalogue_icon_requires_raster(other)) continue;
            identities_unique = identities_unique &&
                staged[icon].image != staged[other].image &&
                staged[icon].resource != staged[other].resource;
        }
    }
    all_ready = all_ready && identities_unique;
    std::uint64_t catalogue_microseconds = elapsed_qpc_microseconds(
        catalogue_started_qpc, query_performance_ticks());
    std::uint64_t average_icon_microseconds = raster_count == 0
        ? 0
        : catalogue_microseconds / raster_count;

    if (!all_ready) {
        // Successfully staged entries retain their exact process-lifetime root
        // rather than guessing teardown after a sibling failed. None becomes
        // visible to the lookup detour, so the whole catalogue falls back to
        // the proven vector icon as one atomic feature gate.
        g_rgba8_catalogue_ready.store(false, std::memory_order_release);
        g_rgba8_publication_disabled.store(true, std::memory_order_release);
        for (std::size_t icon = 0;
             icon < custom_count;
             ++icon) {
            if (!catalogue_icon_requires_raster(icon)) continue;
            log_line(
                "rgba8Catalogue stage icon=%zu id=%d ready=%u resource=%p image=%p checksum=0x%016llX published=0",
                icon,
                kRgba8IconIds[icon],
                staged[icon].ready ? 1U : 0U,
                staged[icon].resource,
                staged[icon].image,
                static_cast<unsigned long long>(staged[icon].checksum));
        }
        log_failure(
            "RGBA8 catalogue did not pass its all-ready/identity gate (procedural=%u ready=%zu/%zu unique=%u timingUs={total:%llu average:%llu slowest:%llu icon:%zu}); no custom raster resource was published",
            procedural_catalogue_valid ? 1U : 0U,
            ready_count,
            raster_count,
            identities_unique ? 1U : 0U,
            static_cast<unsigned long long>(catalogue_microseconds),
            static_cast<unsigned long long>(average_icon_microseconds),
            static_cast<unsigned long long>(slowest_icon_microseconds),
            slowest_icon);
        return;
    }

    // Publish every identity first, then release one all-ready flag. No
    // arbitrary presentation reader can observe a partially published table.
    for (std::size_t icon = 0;
         icon < custom_count;
         ++icon) {
        if (!catalogue_icon_requires_raster(icon)) continue;
        publish_retained_rgba8(icon, staged[icon]);
        log_line(
            "rgba8Catalogue publish slot=%zu id=%d resource=%p image=%p resourceRef=1 rawRef=1 checksum=0x%016llX identity=unique",
            icon,
            kRgba8IconIds[icon],
            staged[icon].resource,
            staged[icon].image,
            static_cast<unsigned long long>(staged[icon].checksum));
    }
    g_rgba8_catalogue_ready.store(true, std::memory_order_release);
    catalogue_microseconds = elapsed_qpc_microseconds(
        catalogue_started_qpc, query_performance_ticks());
    average_icon_microseconds = raster_count == 0
        ? 0
        : catalogue_microseconds / raster_count;
    log_line(
        "STRESS catalogue ready count=%zu ready=%zu alpha=straight-vs-premultiplied-ramp publication=atomic identity=one-to-one unique=1 timingUs={total:%llu average:%llu slowest:%llu icon:%zu}",
        raster_count,
        ready_count,
        static_cast<unsigned long long>(catalogue_microseconds),
        static_cast<unsigned long long>(average_icon_microseconds),
        static_cast<unsigned long long>(slowest_icon_microseconds),
        slowest_icon);
    log_rgba8_lifecycle_snapshot("catalogue-published", top);
}

constexpr bool is_undefined_gfx_value(std::uint32_t flags) noexcept;

bool preflight_in_place_rgba8_rebind_host(
    void* top,
    TabLayer layer,
    std::size_t physical,
    InPlaceRebindHostExpectation expectation,
    std::size_t expected_asset,
    std::uint8_t property_bank,
    bool enforce_ledger) noexcept {
    const std::size_t bit = physical +
        (layer == TabLayer::back ? active_physical_tab_count() : 0);
    const bool expects_tracked =
        expectation != InPlaceRebindHostExpectation::untracked;
    const bool expects_rgba =
        expectation == InPlaceRebindHostExpectation::rgba8_shape;
    if (!top || physical >= active_physical_tab_count() ||
        (layer != TabLayer::front && layer != TabLayer::back) ||
        bit >= g_rgba8_host_assets.size() ||
        (expects_rgba &&
            (expected_asset == 0 ||
                expected_asset > active_custom_tab_count())) ||
        (!expects_rgba && expected_asset != 0)) {
        return false;
    }

    const TabHostMask mask = TabHostMask{1} << bit;
    if (property_bank > kIconPropertyBankB) {
        property_bank = active_icon_property_bank(bit);
    }
    const TabHostMask injected = g_injected_icon_hosts.load(
        std::memory_order_acquire);
    const TabHostMask rgba = g_rgba8_icon_hosts.load(
        std::memory_order_acquire);
    const std::size_t asset = g_rgba8_host_assets[bit].load(
        std::memory_order_acquire);
    const bool ledger_exact = !enforce_ledger ||
        ((((injected & mask) != 0) == expects_tracked) &&
            (((rgba & mask) != 0) == expects_rgba) &&
            asset == expected_asset);

    void* const storage = HeapAlloc(
        GetProcessHeap(), HEAP_ZERO_MEMORY,
        sizeof(InPlaceRebindPreflightArena));
    auto* const arena = storage
        ? ::new (storage) InPlaceRebindPreflightArena{}
        : nullptr;
    if (!arena) return false;

    bool injected_constructed{};
    bool authored_constructed{};
    bool fingerprint_exact{};
    bool properties_exact{};
    bool injected_flags_read{};
    bool authored_flags_read{};
    std::uint32_t injected_flags{};
    std::uint32_t authored_flags{};
    unsigned long seh_code{};
    if (ledger_exact && resolve_injected_icon_parent(
            top, layer, physical, arena->parent)) {
        void* const parent_wrapper =
            arena->parent.storage.data() + kSceneObjProxyValueOffset;
        DisplayValueView parent_view{};
        const bool parent_exact = inspect_display_value(
                parent_wrapper, parent_view) &&
            exact_as3_display_bridge(parent_view);
        void* owner_link{};
        void* movie_root{};
        fingerprint_exact = parent_exact && guarded_read(
                static_cast<const std::byte*>(parent_view.object_interface) +
                    0x08,
                owner_link) && owner_link && guarded_read(
                static_cast<const std::byte*>(owner_link) + 0x18,
                movie_root) && movie_root &&
            g_injected_movie_fingerprint.presentation_epoch ==
                g_runtime.presentation_epoch &&
            parent_view.object_interface ==
                g_injected_movie_fingerprint.object_interface &&
            owner_link == g_injected_movie_fingerprint.owner_link &&
            movie_root == g_injected_movie_fingerprint.movie_root;
        if (fingerprint_exact && g_get_scaleform_member) {
            const char* injected_property =
                injected_icon_property(property_bank);
            const char* authored_property =
                authored_icon_property(property_bank);
#if defined(_MSC_VER)
            __try {
#endif
                injected_constructed = g_get_scaleform_member(
                    parent_wrapper,
                    arena->injected.value.data(),
                    &injected_property) == arena->injected.value.data();
                authored_constructed = g_get_scaleform_member(
                    parent_wrapper,
                    arena->authored.value.data(),
                    &authored_property) == arena->authored.value.data();
#if defined(_MSC_VER)
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                seh_code = static_cast<unsigned long>(GetExceptionCode());
            }
#endif
        }
        if (seh_code == 0 && injected_constructed &&
            authored_constructed) {
            injected_flags_read = guarded_read(
                arena->injected.value.data() + 0x20, injected_flags);
            authored_flags_read = guarded_read(
                arena->authored.value.data() + 0x20, authored_flags);
            if (!expects_tracked) {
                properties_exact = injected_flags_read &&
                    authored_flags_read &&
                    is_undefined_gfx_value(injected_flags) &&
                    is_undefined_gfx_value(authored_flags);
            } else {
                DisplayValueView injected_view{};
                DisplayValueView authored_view{};
                properties_exact = inspect_display_value(
                        arena->injected.value.data(), injected_view) &&
                    inspect_display_value(
                        arena->authored.value.data(), authored_view) &&
                    exact_as3_display_bridge(injected_view) &&
                    exact_as3_display_bridge(authored_view) &&
                    injected_view.object_interface ==
                        parent_view.object_interface &&
                    authored_view.object_interface ==
                        parent_view.object_interface &&
                    injected_view.native_display_vtable ==
                        g_game.base + kDynamicShapeVtable271Rva &&
                    validated_authored_icon_target_vtable(
                        authored_view.native_display_vtable) &&
                    injected_view.object_payload !=
                        authored_view.object_payload;
            }
        }
    }

    bool cleanup_exact = seh_code == 0;
    if (cleanup_exact) {
        cleanup_exact = destroy_local_scaleform_value(
            arena->injected.value,
            injected_constructed,
            "in-place rebind preflight Shape") && cleanup_exact;
        cleanup_exact = destroy_local_scaleform_value(
            arena->authored.value,
            authored_constructed,
            "in-place rebind preflight authored icon") && cleanup_exact;
        cleanup_exact = destroy_proxy(arena->parent) && cleanup_exact;
    }
    const bool exact = ledger_exact && fingerprint_exact &&
        properties_exact && cleanup_exact;
    const char* const expectation_name = !expects_tracked
        ? "untracked"
        : (expects_rgba ? "rgba8" : "vector");
    log_line(
        "rgba8Rebind host preflight layer=%u physical=%zu expectation=%s asset=%u ledger=%u fingerprint=%u properties=%u flags={injected:%u/0x%08X authored:%u/0x%08X} cleanup=%u seh=0x%08lX result=%u",
        static_cast<unsigned>(layer),
        physical,
        expectation_name,
        static_cast<unsigned>(asset),
        ledger_exact ? 1U : 0U,
        fingerprint_exact ? 1U : 0U,
        properties_exact ? 1U : 0U,
        injected_flags_read ? 1U : 0U,
        static_cast<unsigned>(injected_flags),
        authored_flags_read ? 1U : 0U,
        static_cast<unsigned>(authored_flags),
        cleanup_exact ? 1U : 0U,
        seh_code,
        exact ? 1U : 0U);
    if (cleanup_exact) {
        arena->~InPlaceRebindPreflightArena();
        HeapFree(GetProcessHeap(), 0, arena);
    } else {
        log_failure(
            "fresh-Shape RGBA8 replacement preflight retained its stable wrapper arena after cleanup uncertainty");
    }
    return exact;
}

bool clear_in_place_rgba8_rebind_host(
    void* top,
    TabLayer layer,
    std::size_t physical,
    InPlaceRgba8RebindTransaction& transaction) noexcept {
    const std::size_t bit = physical +
        (layer == TabLayer::back ? active_physical_tab_count() : 0);
    if (!top || physical >= active_physical_tab_count() ||
        (layer != TabLayer::front && layer != TabLayer::back) ||
        bit >= g_rgba8_host_assets.size()) {
        return false;
    }

    const TabHostMask mask = TabHostMask{1} << bit;
    if ((transaction.expected_raster_clear_mask & mask) == 0 ||
        (transaction.completed_clear_mask & mask) != 0) {
        return false;
    }

    const bool reconciled_before = reconcile_rgba8_retirement_debt(
        "two-phase-clear-before");
    const TabHostMask injected = g_injected_icon_hosts.load(
        std::memory_order_acquire);
    const TabHostMask rgba = g_rgba8_icon_hosts.load(
        std::memory_order_acquire);
    const std::size_t encoded_asset = g_rgba8_host_assets[bit].load(
        std::memory_order_acquire);
    const std::uint32_t active_before =
        count_active_rgba8_asset_hosts(0U);
    const std::uint32_t debt_before =
        g_rgba8_retirement_debt[0].load(std::memory_order_acquire);
    const Rgba8ReferenceSnapshot refs_before =
        snapshot_rgba8_references(0U);
    const std::uint32_t raw_expected_before =
        1U + active_before + debt_before;
    const bool ownership_before_exact = reconciled_before &&
        (injected & mask) != 0 && (rgba & mask) != 0 &&
        encoded_asset == 1U && active_before > 0U &&
        debt_before < kMaximumRgba8RetirementDebt &&
        refs_before.resource_read && refs_before.resource == 1U &&
        refs_before.raw_read && refs_before.raw == raw_expected_before;

    void* const arena_storage = HeapAlloc(
        GetProcessHeap(), HEAP_ZERO_MEMORY,
        sizeof(InPlaceRebindClearArena));
    auto* const arena = arena_storage
        ? ::new (arena_storage) InPlaceRebindClearArena{}
        : nullptr;
    if (!arena) return false;

    bool injected_constructed{};
    bool authored_constructed{};
    bool graphics_constructed{};
    bool fingerprint_exact{};
    bool shape_exact{};
    bool graphics_exact{};
    bool graphics_cleared{};
    unsigned long seh_code{};
    if (ownership_before_exact && resolve_injected_icon_parent(
            top, layer, physical, arena->parent)) {
        void* const parent_wrapper =
            arena->parent.storage.data() + kSceneObjProxyValueOffset;
        DisplayValueView parent_view{};
        const bool parent_exact = inspect_display_value(
                parent_wrapper, parent_view) &&
            exact_as3_display_bridge(parent_view);
        void* owner_link{};
        void* movie_root{};
        fingerprint_exact = parent_exact && guarded_read(
                static_cast<const std::byte*>(parent_view.object_interface) +
                    0x08,
                owner_link) && owner_link && guarded_read(
                static_cast<const std::byte*>(owner_link) + 0x18,
                movie_root) && movie_root &&
            g_injected_movie_fingerprint.presentation_epoch ==
                g_runtime.presentation_epoch &&
            parent_view.object_interface ==
                g_injected_movie_fingerprint.object_interface &&
            owner_link == g_injected_movie_fingerprint.owner_link &&
            movie_root == g_injected_movie_fingerprint.movie_root;
        if (fingerprint_exact && g_get_scaleform_member) {
            const char* injected_property = kInjectedIconProperty;
            const char* authored_property = kAuthoredIconProperty;
#if defined(_MSC_VER)
            __try {
#endif
                injected_constructed = g_get_scaleform_member(
                    parent_wrapper,
                    arena->injected.value.data(),
                    &injected_property) == arena->injected.value.data();
                authored_constructed = g_get_scaleform_member(
                    parent_wrapper,
                    arena->authored.value.data(),
                    &authored_property) == arena->authored.value.data();
#if defined(_MSC_VER)
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                seh_code = static_cast<unsigned long>(GetExceptionCode());
            }
#endif
        }

        DisplayValueView injected_view{};
        DisplayValueView authored_view{};
        shape_exact = seh_code == 0 && injected_constructed &&
            authored_constructed && inspect_display_value(
                arena->injected.value.data(), injected_view) &&
            inspect_display_value(
                arena->authored.value.data(), authored_view) &&
            exact_as3_display_bridge(injected_view) &&
            exact_as3_display_bridge(authored_view) &&
            injected_view.object_interface == parent_view.object_interface &&
            authored_view.object_interface == parent_view.object_interface &&
            injected_view.native_display_vtable ==
                g_game.base + kDynamicShapeVtable271Rva &&
            validated_authored_icon_target_vtable(
                authored_view.native_display_vtable) &&
            injected_view.object_payload != authored_view.object_payload;
        if (shape_exact && g_get_scaleform_member) {
            const char* graphics_property = "graphics";
#if defined(_MSC_VER)
            __try {
#endif
                graphics_constructed = g_get_scaleform_member(
                    arena->injected.value.data(),
                    arena->graphics.value.data(),
                    &graphics_property) == arena->graphics.value.data();
#if defined(_MSC_VER)
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                seh_code = static_cast<unsigned long>(GetExceptionCode());
            }
#endif
        }
        ObjectValueView graphics_view{};
        graphics_exact = seh_code == 0 && graphics_constructed &&
            inspect_object_value(
                arena->graphics.value.data(), graphics_view) &&
            exact_as3_object_bridge(graphics_view);
        if (graphics_exact) {
            graphics_cleared = invoke_object_method(
                graphics_view, "clear", nullptr, 0, seh_code);
        }
    }

    bool cleanup_exact = seh_code == 0;
    if (cleanup_exact) {
        cleanup_exact = destroy_local_scaleform_value(
            arena->graphics.value,
            graphics_constructed,
            "two-phase rebind clear Graphics") && cleanup_exact;
        cleanup_exact = destroy_local_scaleform_value(
            arena->authored.value,
            authored_constructed,
            "two-phase rebind clear authored icon") && cleanup_exact;
        cleanup_exact = destroy_local_scaleform_value(
            arena->injected.value,
            injected_constructed,
            "two-phase rebind clear Shape") && cleanup_exact;
        cleanup_exact = destroy_proxy(arena->parent) && cleanup_exact;
    }

    const Rgba8ReferenceSnapshot refs_after =
        snapshot_rgba8_references(0);
    const std::uint32_t active_after = active_before > 0U
        ? active_before - 1U
        : 0U;
    const std::uint32_t minimum_raw_after = 1U + active_after;
    const std::uint32_t maximum_debt_after = debt_before + 1U;
    const std::uint32_t maximum_raw_after =
        minimum_raw_after + maximum_debt_after;
    const bool clear_transition_exact = ownership_before_exact &&
        fingerprint_exact && shape_exact && graphics_exact &&
        graphics_cleared && cleanup_exact && seh_code == 0 &&
        refs_after.resource_read && refs_after.resource == 1U &&
        refs_after.raw_read && refs_after.raw >= minimum_raw_after &&
        refs_after.raw <= maximum_raw_after;

    bool ledger_exact{};
    std::uint32_t recorded_debt = debt_before;
    if (clear_transition_exact) {
        recorded_debt = refs_after.raw - minimum_raw_after;
        g_rgba8_icon_hosts.fetch_and(
            ~mask,
            std::memory_order_acq_rel);
        g_rgba8_host_assets[bit].store(0, std::memory_order_release);
        g_rgba8_retirement_debt[0].store(
            recorded_debt, std::memory_order_release);
        g_rgba8_successful_detach_count.fetch_add(
            1, std::memory_order_relaxed);
        transaction.completed_clear_mask |= mask;
        ledger_exact = reconcile_rgba8_retirement_debt(
            "two-phase-clear-after");
        recorded_debt = g_rgba8_retirement_debt[0].load(
            std::memory_order_acquire);
    }

    log_line(
        "rgba8Rebind clear layer=%u physical=%zu mask=0x%04X ownership=%u fingerprint=%u shape=%u graphics={exact:%u cleared:%u} refs={resource:%u/%u raw:%u/%u->%u/%u expectedBefore:%u allowedAfter:%u..%u} ledger={active:%u->%u debt:%u->%u completed:0x%04X/0x%04X exact:%u} cleanup=%u seh=0x%08lX",
        static_cast<unsigned>(layer),
        physical,
        static_cast<unsigned>(mask),
        ownership_before_exact ? 1U : 0U,
        fingerprint_exact ? 1U : 0U,
        shape_exact ? 1U : 0U,
        graphics_exact ? 1U : 0U,
        graphics_cleared ? 1U : 0U,
        refs_before.resource_read ? 1U : 0U,
        static_cast<unsigned>(refs_before.resource),
        refs_before.raw_read ? 1U : 0U,
        static_cast<unsigned>(refs_before.raw),
        refs_after.raw_read ? 1U : 0U,
        static_cast<unsigned>(refs_after.raw),
        static_cast<unsigned>(raw_expected_before),
        static_cast<unsigned>(minimum_raw_after),
        static_cast<unsigned>(maximum_raw_after),
        static_cast<unsigned>(active_before),
        static_cast<unsigned>(active_after),
        static_cast<unsigned>(debt_before),
        static_cast<unsigned>(recorded_debt),
        static_cast<unsigned>(transaction.completed_clear_mask),
        static_cast<unsigned>(transaction.expected_clear_mask),
        ledger_exact ? 1U : 0U,
        cleanup_exact ? 1U : 0U,
        seh_code);

    if (cleanup_exact) {
        arena->~InPlaceRebindClearArena();
        HeapFree(GetProcessHeap(), 0, arena);
    } else {
        log_failure(
            "two-phase RGBA8 clear retained its stable wrapper arena after cleanup uncertainty");
    }
    return clear_transition_exact && ledger_exact;
}

enum class InjectedIconDetachResult : std::uint8_t {
    failed,
    removed,
    already_absent,
    different_movie,
};

constexpr bool is_undefined_gfx_value(std::uint32_t flags) noexcept {
    return (flags & 0x8FU) == 0;
}

static_assert(is_undefined_gfx_value(0));
static_assert(!is_undefined_gfx_value(0x08));
static_assert(!is_undefined_gfx_value(0x0A));

InjectedIconDetachResult clear_injected_icon_host(
    void* top,
    TabLayer layer,
    std::size_t physical,
    std::uint8_t property_bank = 0xFFU,
    bool restore_authored = true) noexcept {
    const std::size_t host_bit = physical +
        (layer == TabLayer::back ? active_physical_tab_count() : 0);
    if (property_bank > kIconPropertyBankB) {
        property_bank = active_icon_property_bank(host_bit);
    }
    const char* const injected_property_name =
        injected_icon_property(property_bank);
    const char* const authored_property_name =
        authored_icon_property(property_bank);
    void* const arena_storage = HeapAlloc(
        GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(InjectedIconDetachArena));
    auto* const arena = arena_storage
        ? ::new (arena_storage) InjectedIconDetachArena{}
        : nullptr;
    if (!arena) {
        log_failure("iconDetach could not allocate stable wrapper arena");
        return InjectedIconDetachResult::failed;
    }

    if (!resolve_injected_icon_parent(
            top, layer, physical, arena->parent)) {
        const bool parent_clean = destroy_proxy(arena->parent);
        if (parent_clean) {
            arena->~InjectedIconDetachArena();
            HeapFree(GetProcessHeap(), 0, arena_storage);
        }
        return InjectedIconDetachResult::failed;
    }

    void* const parent_wrapper =
        arena->parent.storage.data() + kSceneObjProxyValueOffset;
    DisplayValueView parent_view{};
    const bool exact_parent = inspect_display_value(
            parent_wrapper, parent_view) &&
        exact_as3_display_bridge(parent_view);
    void* parent_owner_link{};
    void* parent_movie_root{};
    const bool fingerprint_read = exact_parent &&
        guarded_read(
            static_cast<const std::byte*>(parent_view.object_interface) +
                0x08,
            parent_owner_link) &&
        parent_owner_link &&
        guarded_read(
            static_cast<const std::byte*>(parent_owner_link) + 0x18,
            parent_movie_root) &&
        parent_movie_root;
    const bool fingerprint_known =
        g_injected_movie_fingerprint.object_interface &&
        g_injected_movie_fingerprint.owner_link &&
        g_injected_movie_fingerprint.movie_root;
    const bool same_movie = fingerprint_read && fingerprint_known &&
        parent_view.object_interface ==
            g_injected_movie_fingerprint.object_interface &&
        parent_owner_link == g_injected_movie_fingerprint.owner_link &&
        parent_movie_root == g_injected_movie_fingerprint.movie_root;

    auto& injected_wrapper = arena->injected.value;
    auto& authored_wrapper = arena->authored.value;
    auto& graphics_wrapper = arena->graphics.value;
    auto& undefined_wrapper = arena->undefined.value;
    auto& verify_injected_wrapper = arena->verify_injected.value;
    auto& verify_authored_wrapper = arena->verify_authored.value;
    bool injected_constructed{};
    bool authored_constructed{};
    bool graphics_constructed{};
    bool graphics_exact{};
    bool graphics_cleared{};
    bool undefined_constructed{};
    bool verify_injected_constructed{};
    bool verify_authored_constructed{};
    bool injected_flags_read{};
    bool authored_flags_read{};
    std::uint32_t injected_flags{};
    std::uint32_t authored_flags{};
    bool already_absent{};
    bool removed{};
    bool hidden_fallback{};
    bool authored_visible{};
    bool icon_property_cleared{};
    bool authored_property_cleared{};
    bool property_clear_verified{};
    unsigned long graphics_seh{};
    unsigned long seh_code{};

    if (same_movie && g_get_scaleform_member) {
        const char* icon_property = injected_property_name;
        const char* authored_property = authored_property_name;
#if defined(_MSC_VER)
        __try {
#endif
            injected_constructed = g_get_scaleform_member(
                parent_wrapper,
                injected_wrapper.data(),
                &icon_property) == injected_wrapper.data();
            authored_constructed = g_get_scaleform_member(
                parent_wrapper,
                authored_wrapper.data(),
                &authored_property) == authored_wrapper.data();

            injected_flags_read = injected_constructed && guarded_read(
                injected_wrapper.data() + 0x20, injected_flags);
            authored_flags_read = authored_constructed && guarded_read(
                authored_wrapper.data() + 0x20, authored_flags);
            already_absent = injected_flags_read && authored_flags_read &&
                is_undefined_gfx_value(injected_flags) &&
                is_undefined_gfx_value(authored_flags);

            DisplayValueView injected_view{};
            DisplayValueView authored_view{};
            const bool exact_injected = injected_constructed &&
                inspect_display_value(
                    injected_wrapper.data(), injected_view) &&
                exact_as3_display_bridge(injected_view);
            const bool exact_authored = authored_constructed &&
                inspect_display_value(
                    authored_wrapper.data(), authored_view) &&
                exact_as3_display_bridge(authored_view);
            const bool exact_shape_icon = exact_injected &&
                injected_view.native_display_vtable ==
                    g_game.base + kDynamicShapeVtable271Rva;

            if (exact_shape_icon && exact_authored &&
                g_construct_scaleform_value && g_set_scaleform_visible) {
                if (g_get_scaleform_member) {
                    const char* graphics_property = "graphics";
#if defined(_MSC_VER)
                    __try {
#endif
                        graphics_constructed = g_get_scaleform_member(
                            injected_wrapper.data(),
                            graphics_wrapper.data(),
                            &graphics_property) == graphics_wrapper.data();
#if defined(_MSC_VER)
                    } __except (EXCEPTION_EXECUTE_HANDLER) {
                        graphics_seh = static_cast<unsigned long>(
                            GetExceptionCode());
                    }
#endif
                }
                ObjectValueView graphics_view{};
                graphics_exact = graphics_seh == 0 &&
                    graphics_constructed &&
                    inspect_object_value(
                        graphics_wrapper.data(), graphics_view) &&
                    exact_as3_object_bridge(graphics_view);
                if (graphics_exact) {
                    graphics_cleared = invoke_object_method(
                        graphics_view,
                        "clear",
                        nullptr,
                        0,
                        graphics_seh);
                }

                // A detached AS3 Shape remains alive until Scaleform's tracing
                // collector runs. Clear its Graphics command list first so a
                // bitmap fill releases its RawImage reference synchronously;
                // only then may the host ledger be checked and transferred.
                // Restoring the authored icon is an independent visual
                // fail-safe. Even when Graphics.clear() is unavailable, make
                // the native icon visible before hiding the injected Shape;
                // only exact Graphics cleanup permits ownership detachment.
                authored_visible = !restore_authored ||
                    g_set_scaleform_visible(
                        authored_wrapper.data(), 1) == authored_wrapper.data();
                if (graphics_cleared && graphics_seh == 0 &&
                    authored_visible) {
                    removed = reinterpret_cast<ValueInvokeFn>(
                        parent_view.invoke_slot)(
                        parent_view.object_interface,
                        parent_view.object_payload,
                        nullptr,
                        "removeChild",
                        injected_wrapper.data() + 0x08,
                        1,
                        true);
                }
                if (!removed) {
                    hidden_fallback = g_set_scaleform_visible(
                        injected_wrapper.data(), 0) == injected_wrapper.data();
                }

                // Keep parent-owned references when removeChild fails.
                // Hiding is only a visual fail-safe; it is not ownership
                // cleanup, and retaining the references lets a later remap
                // retry the exact removal without accumulating orphan icons.
                if (removed) {
                    undefined_constructed = g_construct_scaleform_value(
                        undefined_wrapper.data()) == undefined_wrapper.data();
                }
                if (removed && undefined_constructed) {
                    const auto set_member =
                        reinterpret_cast<ValueSetMemberFn>(
                            parent_view.set_member_slot);
                    icon_property_cleared = set_member(
                        parent_view.object_interface,
                        parent_view.object_payload,
                        injected_property_name,
                        undefined_wrapper.data() + 0x08,
                        true);
                    authored_property_cleared = set_member(
                        parent_view.object_interface,
                        parent_view.object_payload,
                        authored_property_name,
                        undefined_wrapper.data() + 0x08,
                        true);
                    if (icon_property_cleared &&
                        authored_property_cleared) {
                        verify_injected_constructed =
                            g_get_scaleform_member(
                                parent_wrapper,
                                verify_injected_wrapper.data(),
                                &icon_property) ==
                            verify_injected_wrapper.data();
                        verify_authored_constructed =
                            g_get_scaleform_member(
                                parent_wrapper,
                                verify_authored_wrapper.data(),
                                &authored_property) ==
                            verify_authored_wrapper.data();
                        std::uint32_t verify_injected_flags{};
                        std::uint32_t verify_authored_flags{};
                        property_clear_verified =
                            verify_injected_constructed &&
                            verify_authored_constructed &&
                            guarded_read(
                                verify_injected_wrapper.data() + 0x20,
                                verify_injected_flags) &&
                            guarded_read(
                                verify_authored_wrapper.data() + 0x20,
                                verify_authored_flags) &&
                            is_undefined_gfx_value(
                                verify_injected_flags) &&
                            is_undefined_gfx_value(
                                verify_authored_flags);
                    }
                }
            }
#if defined(_MSC_VER)
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            seh_code = static_cast<unsigned long>(GetExceptionCode());
        }
#endif
    }

    // Destroy every managed wrapper while its heap arena remains stable. Any
    // native fault retains the arena and all ledger bits for a later exact
    // epoch cleanup rather than leaving an ObjectInterface link into dead
    // stack storage or claiming an ownership transition that did not finish.
    bool wrappers_clean = seh_code == 0 && graphics_seh == 0;
    if (wrappers_clean) {
        wrappers_clean = destroy_local_scaleform_value(
            verify_authored_wrapper,
            verify_authored_constructed,
            "detach verify authored") && wrappers_clean;
        wrappers_clean = destroy_local_scaleform_value(
            verify_injected_wrapper,
            verify_injected_constructed,
            "detach verify injected") && wrappers_clean;
        wrappers_clean = destroy_local_scaleform_value(
            undefined_wrapper,
            undefined_constructed,
            "detach undefined value") && wrappers_clean;
        wrappers_clean = destroy_local_scaleform_value(
            authored_wrapper,
            authored_constructed,
            "detach authored icon") && wrappers_clean;
        wrappers_clean = destroy_local_scaleform_value(
            graphics_wrapper,
            graphics_constructed,
            "detach Shape graphics") && wrappers_clean;
        wrappers_clean = destroy_local_scaleform_value(
            injected_wrapper,
            injected_constructed,
            "detach Shape") && wrappers_clean;
    }
    const bool proxy_clean = wrappers_clean && destroy_proxy(arena->parent);
    const bool cleanup_clean = wrappers_clean && proxy_clean;

    const bool exact_removed = same_movie && graphics_exact &&
        graphics_cleared && graphics_seh == 0 && removed && authored_visible &&
        icon_property_cleared && authored_property_cleared &&
        property_clear_verified && seh_code == 0 && cleanup_clean;
    const bool exact_absent = same_movie && already_absent &&
        seh_code == 0 && cleanup_clean;
    const bool exact_different_movie = fingerprint_read &&
        fingerprint_known && !same_movie && cleanup_clean;
    const InjectedIconDetachResult result = exact_removed
        ? InjectedIconDetachResult::removed
        : (exact_absent
            ? InjectedIconDetachResult::already_absent
            : (exact_different_movie
                ? InjectedIconDetachResult::different_movie
                : InjectedIconDetachResult::failed));
    log_line(
        "iconDetach epoch=%llu layer=%u physical=%zu exactParent=%u fingerprint={read:%u known:%u same:%u interface:%p/%p owner:%p/%p root:%p/%p} propertiesConstructed={%u,%u} propertyFlags={read:%u,0x%08X;read:%u,0x%08X} graphics={constructed:%u exact:%u clear:%u seh:0x%08lX} alreadyAbsent=%u removed=%u hiddenFallback=%u authoredVisible=%u propertyCleared={%u,%u verified:%u} cleanup={wrappers:%u proxy:%u} seh=0x%08lX result=%u",
        static_cast<unsigned long long>(g_runtime.presentation_epoch),
        static_cast<unsigned>(layer),
        physical,
        exact_parent ? 1U : 0U,
        fingerprint_read ? 1U : 0U,
        fingerprint_known ? 1U : 0U,
        same_movie ? 1U : 0U,
        parent_view.object_interface,
        g_injected_movie_fingerprint.object_interface,
        parent_owner_link,
        g_injected_movie_fingerprint.owner_link,
        parent_movie_root,
        g_injected_movie_fingerprint.movie_root,
        injected_constructed ? 1U : 0U,
        authored_constructed ? 1U : 0U,
        injected_flags_read ? 1U : 0U,
        static_cast<unsigned>(injected_flags),
        authored_flags_read ? 1U : 0U,
        static_cast<unsigned>(authored_flags),
        graphics_constructed ? 1U : 0U,
        graphics_exact ? 1U : 0U,
        graphics_cleared ? 1U : 0U,
        graphics_seh,
        already_absent ? 1U : 0U,
        removed ? 1U : 0U,
        hidden_fallback ? 1U : 0U,
        authored_visible ? 1U : 0U,
        icon_property_cleared ? 1U : 0U,
        authored_property_cleared ? 1U : 0U,
        property_clear_verified ? 1U : 0U,
        wrappers_clean ? 1U : 0U,
        proxy_clean ? 1U : 0U,
        seh_code,
        static_cast<unsigned>(result));

    if (cleanup_clean) {
        arena->~InjectedIconDetachArena();
        HeapFree(GetProcessHeap(), 0, arena_storage);
    } else {
        log_failure(
            "iconDetach retained stable heap arena after guarded cleanup uncertainty");
    }
    return result;
}

// Retire one raster host without retaining its Shape. The generic per-cell
// detach helper performs the exact same-Movie Graphics.clear(), removeChild(),
// authored-icon restore, property-to-Undefined readback, and wrapper teardown
// already proven by ordinary viewport cleanup. This wrapper adds the
// renderer-debt accounting required when the RawImage reference retires on a
// later frame instead of synchronously during Graphics.clear().
bool detach_rgba8_host_for_replacement(
    void* top,
    TabLayer layer,
    std::size_t physical,
    InPlaceRgba8RebindTransaction& transaction,
    bool restore_authored) noexcept {
    const std::size_t bit = physical +
        (layer == TabLayer::back ? active_physical_tab_count() : 0);
    if (!top || physical >= active_physical_tab_count() ||
        (layer != TabLayer::front && layer != TabLayer::back) ||
        bit >= g_rgba8_host_assets.size()) {
        return false;
    }

    const TabHostMask mask = TabHostMask{1} << bit;
    if ((transaction.expected_raster_clear_mask & mask) == 0 ||
        (transaction.completed_clear_mask & mask) != 0) {
        return false;
    }

    const bool reconciled_before = reconcile_rgba8_retirement_debt(
        "shape-replacement-detach-before");
    const TabHostMask injected_before = g_injected_icon_hosts.load(
        std::memory_order_acquire);
    const TabHostMask rgba_before = g_rgba8_icon_hosts.load(
        std::memory_order_acquire);
    const std::size_t encoded_asset_before =
        g_rgba8_host_assets[bit].load(std::memory_order_acquire);
    const bool asset_index_exact = encoded_asset_before > 0U &&
        encoded_asset_before <= active_custom_tab_count();
    const std::size_t asset_index = asset_index_exact
        ? static_cast<std::size_t>(encoded_asset_before - 1U)
        : invalid_tab_index;
    const std::uint32_t active_before = asset_index_exact
        ? count_active_rgba8_asset_hosts(asset_index)
        : 0U;
    const std::uint32_t debt_before = asset_index_exact
        ? g_rgba8_retirement_debt[asset_index].load(
            std::memory_order_acquire)
        : 0U;
    const Rgba8ReferenceSnapshot refs_before = asset_index_exact
        ? snapshot_rgba8_references(asset_index)
        : Rgba8ReferenceSnapshot{};
    const auto source_retirement =
        plan_rgba8_retirement_reconciliation(
            active_before,
            debt_before,
            refs_before.raw,
            kRgba8RetirementLimits);
    const bool ownership_before_exact = reconciled_before &&
        (injected_before & mask) != 0 && (rgba_before & mask) != 0 &&
        asset_index_exact && active_before > 0U &&
        debt_before < kMaximumRgba8RetirementDebt &&
        refs_before.resource_read && refs_before.resource == 1U &&
        refs_before.raw_read && source_retirement.accepted() &&
        source_retirement.retirement_debt_after == debt_before;

    const InjectedIconDetachResult detach_result = ownership_before_exact
        ? clear_injected_icon_host(
            top,
            layer,
            physical,
            active_icon_property_bank(bit),
            restore_authored)
        : InjectedIconDetachResult::failed;
    const Rgba8ReferenceSnapshot refs_after =
        asset_index_exact
        ? snapshot_rgba8_references(asset_index)
        : Rgba8ReferenceSnapshot{};
    const Rgba8HostRemovalPlan retirement =
        plan_rgba8_host_removal(
            active_before,
            debt_before,
            refs_before.raw,
            refs_after.raw,
            kRgba8RetirementLimits);
    const bool detach_transition_exact = ownership_before_exact &&
        detach_result == InjectedIconDetachResult::removed &&
        refs_after.resource_read && refs_after.resource == 1U &&
        refs_after.raw_read && retirement.accepted();

    std::uint32_t recorded_debt = debt_before;
    bool ledger_exact{};
    if (detach_transition_exact) {
        recorded_debt = retirement.retirement_debt_after;
        const bool debt_published = set_rgba8_retirement_debt(
            asset_index, recorded_debt);
        if (debt_published) {
            g_injected_icon_hosts.fetch_and(
                ~mask,
                std::memory_order_acq_rel);
            g_rgba8_icon_hosts.fetch_and(
                ~mask,
                std::memory_order_acq_rel);
            g_rgba8_host_assets[bit].store(0, std::memory_order_release);

            ledger_exact =
                (g_injected_icon_hosts.load(
                     std::memory_order_acquire) & mask) == 0 &&
                (g_rgba8_icon_hosts.load(
                     std::memory_order_acquire) & mask) == 0 &&
                g_rgba8_host_assets[bit].load(
                    std::memory_order_acquire) == 0 &&
                rgba8_retirement_debt(asset_index) == recorded_debt;
        }
        if (ledger_exact) {
            g_rgba8_successful_detach_count.fetch_add(
                1, std::memory_order_relaxed);
            transaction.completed_clear_mask =
                transaction.completed_clear_mask | mask;
        }
    }

    log_line(
        "rgba8Replace detach layer=%u physical=%zu mask=0x%04X asset=%u ownership=%u result=%u refs={resource:%u/%u raw:%u/%u->%u/%u expectedBefore:%u allowedAfter:%u..%u} ledger={active:%u->%u debt:%u->%u injected:0x%04X rgba:0x%04X assetAfter:%u completed:0x%04X/0x%04X exact:%u}",
        static_cast<unsigned>(layer),
        physical,
        static_cast<unsigned>(mask),
        static_cast<unsigned>(encoded_asset_before),
        ownership_before_exact ? 1U : 0U,
        static_cast<unsigned>(detach_result),
        refs_before.resource_read ? 1U : 0U,
        static_cast<unsigned>(refs_before.resource),
        refs_before.raw_read ? 1U : 0U,
        static_cast<unsigned>(refs_before.raw),
        refs_after.raw_read ? 1U : 0U,
        static_cast<unsigned>(refs_after.raw),
        static_cast<unsigned>(source_retirement.maximum_raw),
        static_cast<unsigned>(retirement.minimum_raw_after),
        static_cast<unsigned>(retirement.maximum_raw_after),
        static_cast<unsigned>(active_before),
        static_cast<unsigned>(retirement.active_hosts_after),
        static_cast<unsigned>(debt_before),
        static_cast<unsigned>(recorded_debt),
        static_cast<unsigned>(
            g_injected_icon_hosts.load(std::memory_order_acquire)),
        static_cast<unsigned>(
            g_rgba8_icon_hosts.load(std::memory_order_acquire)),
        static_cast<unsigned>(
            g_rgba8_host_assets[bit].load(std::memory_order_acquire)),
        static_cast<unsigned>(transaction.completed_clear_mask),
        static_cast<unsigned>(transaction.expected_clear_mask),
        ledger_exact ? 1U : 0U);
    return detach_transition_exact && ledger_exact;
}

// Retire one vector-only injected Shape. Unlike a raster host it owns no
// RawImage reference, so exact removal can update the injected-host ledger
// immediately and does not create renderer-retirement debt.
bool detach_vector_host_for_replacement(
    void* top,
    TabLayer layer,
    std::size_t physical,
    InPlaceRgba8RebindTransaction& transaction,
    bool restore_authored) noexcept {
    const std::size_t bit = physical +
        (layer == TabLayer::back ? active_physical_tab_count() : 0);
    if (!top || physical >= active_physical_tab_count() ||
        (layer != TabLayer::front && layer != TabLayer::back) ||
        bit >= g_rgba8_host_assets.size()) {
        return false;
    }

    const TabHostMask mask = TabHostMask{1} << bit;
    if ((transaction.expected_vector_clear_mask & mask) == 0 ||
        (transaction.completed_clear_mask & mask) != 0) {
        return false;
    }

    const TabHostMask injected_before = g_injected_icon_hosts.load(
        std::memory_order_acquire);
    const TabHostMask rgba_before = g_rgba8_icon_hosts.load(
        std::memory_order_acquire);
    const std::size_t asset_before = g_rgba8_host_assets[bit].load(
        std::memory_order_acquire);
    const bool refs_before_exact =
        rgba8_references_match_tracked_hosts(
            "shape-replacement-vector-detach-before");
    const bool ownership_before_exact =
        (injected_before & mask) != 0 && (rgba_before & mask) == 0 &&
        asset_before == 0 && refs_before_exact &&
        preflight_in_place_rgba8_rebind_host(
            top, layer, physical,
            InPlaceRebindHostExpectation::vector_shape);
    const InjectedIconDetachResult detach_result = ownership_before_exact
        ? clear_injected_icon_host(
            top,
            layer,
            physical,
            active_icon_property_bank(bit),
            restore_authored)
        : InjectedIconDetachResult::failed;

    bool ledger_exact{};
    if (detach_result == InjectedIconDetachResult::removed) {
        g_injected_icon_hosts.fetch_and(
            ~mask,
            std::memory_order_acq_rel);
        ledger_exact =
            (g_injected_icon_hosts.load(std::memory_order_acquire) & mask) ==
                0 &&
            (g_rgba8_icon_hosts.load(std::memory_order_acquire) & mask) == 0 &&
            g_rgba8_host_assets[bit].load(std::memory_order_acquire) == 0 &&
            rgba8_references_match_tracked_hosts(
                "shape-replacement-vector-detach-after");
        if (ledger_exact) {
            transaction.completed_clear_mask =
                transaction.completed_clear_mask | mask;
        }
    }

    log_line(
        "rgba8Replace vectorDetach layer=%u physical=%zu mask=0x%04X ownership=%u result=%u ledger={injected:0x%04X rgba:0x%04X asset:%u completed:0x%04X/0x%04X exact:%u}",
        static_cast<unsigned>(layer),
        physical,
        static_cast<unsigned>(mask),
        ownership_before_exact ? 1U : 0U,
        static_cast<unsigned>(detach_result),
        static_cast<unsigned>(
            g_injected_icon_hosts.load(std::memory_order_acquire)),
        static_cast<unsigned>(
            g_rgba8_icon_hosts.load(std::memory_order_acquire)),
        static_cast<unsigned>(
            g_rgba8_host_assets[bit].load(std::memory_order_acquire)),
        static_cast<unsigned>(transaction.completed_clear_mask),
        static_cast<unsigned>(transaction.expected_clear_mask),
        ledger_exact ? 1U : 0U);
    return ownership_before_exact &&
        detach_result == InjectedIconDetachResult::removed && ledger_exact;
}

InjectedIconsClearResult clear_injected_icons(
    void* top,
    bool allow_already_absent) noexcept {
    const TabHostMask active_tracked = g_injected_icon_hosts.load(
        std::memory_order_acquire);
    const TabHostMask staged_tracked =
        g_staged_injected_icon_hosts.load(std::memory_order_acquire);
    if ((active_tracked | staged_tracked) == 0) {
        log_rgba8_lifecycle_snapshot("clear-no-hosts", top);
        if (!reconcile_rgba8_retirement_debt(
                "epoch-clear-no-hosts")) {
            return InjectedIconsClearResult::failed;
        }
        if (!all_rgba8_retirement_debts_zero()) {
            return allow_already_absent
                ? InjectedIconsClearResult::waiting_for_renderer_retirement
                : InjectedIconsClearResult::failed;
        }
        g_injected_movie_fingerprint = {};
        g_icon_bank_b_hosts.store(0, std::memory_order_release);
        return InjectedIconsClearResult::cleared;
    }

    bool all_cleared{true};
    const auto clear_bank = [&](bool staged) noexcept {
        const TabHostMask tracked = staged
            ? g_staged_injected_icon_hosts.load(std::memory_order_acquire)
            : g_injected_icon_hosts.load(std::memory_order_acquire);
        for (std::size_t bit = 0; bit < active_tab_host_count(); ++bit) {
            const TabHostMask mask = TabHostMask{1} << bit;
            if ((tracked & mask) == 0) continue;

            const TabLayer layer = bit < active_physical_tab_count()
                ? TabLayer::front
                : TabLayer::back;
            const std::size_t physical = bit % active_physical_tab_count();
            const TabHostMask rgba_bits = staged
                ? g_staged_rgba8_icon_hosts.load(std::memory_order_acquire)
                : g_rgba8_icon_hosts.load(std::memory_order_acquire);
            const bool was_rgba = (rgba_bits & mask) != 0;
            const std::size_t prior_asset = staged
                ? g_staged_rgba8_host_assets[bit].load(
                    std::memory_order_acquire)
                : g_rgba8_host_assets[bit].load(
                    std::memory_order_acquire);
            const bool asset_index_exact = was_rgba && prior_asset > 0U &&
                prior_asset <= active_custom_tab_count() &&
                catalogue_icon_requires_raster(prior_asset - 1U);
            const std::size_t asset_index = asset_index_exact
                ? prior_asset - 1U
                : invalid_tab_index;
            const std::uint32_t active_before = asset_index_exact
                ? count_active_rgba8_asset_hosts(asset_index)
                : 0U;
            std::uint32_t debt_before = asset_index_exact
                ? rgba8_retirement_debt(asset_index)
                : 0U;
            const Rgba8ReferenceSnapshot refs_before =
                asset_index_exact
                ? snapshot_rgba8_references(asset_index)
                : Rgba8ReferenceSnapshot{};
            bool epoch_source_exact = allow_already_absent &&
                asset_index_exact && refs_before.resource_read &&
                refs_before.resource == 1U && refs_before.raw_read;
            if (epoch_source_exact) {
                const auto reconciliation =
                    plan_rgba8_retirement_reconciliation(
                        active_before,
                        debt_before,
                        refs_before.raw,
                        kRgba8RetirementLimits);
                epoch_source_exact = reconciliation.accepted();
                if (epoch_source_exact &&
                    reconciliation.retirement_debt_after != debt_before) {
                    epoch_source_exact = set_rgba8_retirement_debt(
                        asset_index,
                        reconciliation.retirement_debt_after);
                    if (epoch_source_exact) {
                        debt_before =
                            reconciliation.retirement_debt_after;
                    }
                }
            }
            const std::uint8_t property_bank = staged
                ? static_cast<std::uint8_t>(
                    active_icon_property_bank(bit) ^ 1U)
                : active_icon_property_bank(bit);
            const InjectedIconDetachResult result =
                clear_injected_icon_host(
                    top, layer, physical, property_bank);
            const Rgba8ReferenceSnapshot refs_after =
                asset_index_exact &&
                    result == InjectedIconDetachResult::removed
                ? snapshot_rgba8_references(asset_index)
                : Rgba8ReferenceSnapshot{};
            const Rgba8HostRemovalPlan epoch_retirement =
                plan_rgba8_host_removal(
                    active_before,
                    debt_before,
                    refs_before.raw,
                    refs_after.raw,
                    kRgba8RetirementLimits);
            const bool epoch_removed_ref_proven =
                allow_already_absent && was_rgba && epoch_source_exact &&
                result == InjectedIconDetachResult::removed &&
                refs_after.resource_read && refs_after.resource == 1U &&
                refs_after.raw_read && epoch_retirement.accepted() &&
                set_rgba8_retirement_debt(
                    asset_index,
                    epoch_retirement.retirement_debt_after);
            const bool removed_ref_proven =
                result == InjectedIconDetachResult::removed &&
                (!was_rgba ||
                    (allow_already_absent
                        ? epoch_removed_ref_proven
                        : rgba8_references_match_tracked_hosts(
                            staged
                                ? "post-remove-staged"
                                : "post-remove",
                            bit,
                            0,
                            staged)));
            const bool absent_boundary_released =
                allow_already_absent &&
                result == InjectedIconDetachResult::already_absent &&
                (!was_rgba || rgba8_resources_at_root_baseline());
            const TabHostMask all_rgba_bits =
                g_rgba8_icon_hosts.load(std::memory_order_acquire) |
                g_staged_rgba8_icon_hosts.load(std::memory_order_acquire);
            const bool movie_boundary_released =
                allow_already_absent &&
                result == InjectedIconDetachResult::different_movie &&
                (!was_rgba || all_rgba_bits == 0 ||
                    rgba8_resources_at_root_baseline());
            if (removed_ref_proven || absent_boundary_released ||
                movie_boundary_released) {
                if (staged) {
                    g_staged_injected_icon_hosts.fetch_and(
                        ~mask,
                        std::memory_order_acq_rel);
                    g_staged_rgba8_icon_hosts.fetch_and(
                        ~mask,
                        std::memory_order_acq_rel);
                    g_staged_rgba8_host_assets[bit].store(
                        0, std::memory_order_release);
                } else {
                    g_injected_icon_hosts.fetch_and(
                        ~mask,
                        std::memory_order_acq_rel);
                    g_rgba8_icon_hosts.fetch_and(
                        ~mask,
                        std::memory_order_acq_rel);
                    g_rgba8_host_assets[bit].store(
                        0, std::memory_order_release);
                }
                const TabHostMask injected_after = staged
                    ? g_staged_injected_icon_hosts.load(
                        std::memory_order_acquire)
                    : g_injected_icon_hosts.load(
                        std::memory_order_acquire);
                const TabHostMask rgba_after = staged
                    ? g_staged_rgba8_icon_hosts.load(
                        std::memory_order_acquire)
                    : g_rgba8_icon_hosts.load(
                        std::memory_order_acquire);
                const std::size_t asset_after = staged
                    ? g_staged_rgba8_host_assets[bit].load(
                        std::memory_order_acquire)
                    : g_rgba8_host_assets[bit].load(
                        std::memory_order_acquire);
                const bool ledger_exact =
                    (injected_after & mask) == 0 &&
                    (rgba_after & mask) == 0 && asset_after == 0 &&
                    (!epoch_removed_ref_proven ||
                        rgba8_retirement_debt(asset_index) ==
                            epoch_retirement.retirement_debt_after);
                if (was_rgba && ledger_exact) {
                    g_rgba8_successful_detach_count.fetch_add(
                        1, std::memory_order_relaxed);
                }
                log_line(
                    "iconDetach ownershipCleared=%u bank=%s layer=%u physical=%zu rgba=%u asset=%u result=%u refsProven=%u retirement={active:%u->%u debt:%u->%u policy:%u}",
                    ledger_exact ? 1U : 0U,
                    staged ? "staged" : "active",
                    static_cast<unsigned>(layer),
                    physical,
                    was_rgba ? 1U : 0U,
                    static_cast<unsigned>(prior_asset),
                    static_cast<unsigned>(result),
                    (!was_rgba || removed_ref_proven ||
                        absent_boundary_released || movie_boundary_released)
                        ? 1U
                        : 0U,
                    static_cast<unsigned>(active_before),
                    static_cast<unsigned>(
                        epoch_retirement.active_hosts_after),
                    static_cast<unsigned>(debt_before),
                    static_cast<unsigned>(
                        epoch_retirement.retirement_debt_after),
                    static_cast<unsigned>(epoch_retirement.status));
                if (!ledger_exact) {
                    log_failure(
                        "iconDetach ownership proof succeeded but the host/debt ledger did not publish exactly (bank=%s layer=%u physical=%zu rgba=%u)",
                        staged ? "staged" : "active",
                        static_cast<unsigned>(layer),
                        physical,
                        was_rgba ? 1U : 0U);
                    all_cleared = false;
                }
                if (absent_boundary_released && was_rgba) {
                    log_line(
                        "iconDetach accepted already-absent RGBA8 host at an epoch boundary only after every resource/raw image returned to root refcount 1 (bank=%s layer=%u physical=%zu)",
                        staged ? "staged" : "active",
                        static_cast<unsigned>(layer),
                        physical);
                }
                if (movie_boundary_released) {
                    log_line(
                        "iconDetach accepted different-movie epoch boundary after exact ownership proof (bank=%s layer=%u physical=%zu rgba=%u)",
                        staged ? "staged" : "active",
                        static_cast<unsigned>(layer),
                        physical,
                        was_rgba ? 1U : 0U);
                }
            } else {
                if (result == InjectedIconDetachResult::removed && was_rgba) {
                    log_failure(
                        "iconDetach removed a tracked RGBA8 Shape but its RawImage retirement transition was not exact; ownership ledger retained (bank=%s layer=%u physical=%zu policy=%u)",
                        staged ? "staged" : "active",
                        static_cast<unsigned>(layer),
                        physical,
                        static_cast<unsigned>(epoch_retirement.status));
                } else if (result ==
                    InjectedIconDetachResult::already_absent) {
                    log_failure(
                        "iconDetach found an already-absent tracked icon without both an allowed Configuration epoch boundary and RGBA root-refcount proof (bank=%s layer=%u physical=%zu rgba=%u)",
                        staged ? "staged" : "active",
                        static_cast<unsigned>(layer),
                        physical,
                        was_rgba ? 1U : 0U);
                }
                all_cleared = false;
            }
        }
    };

    // A failed atomic handoff may leave a partial inactive bank above the
    // intact source bank. Retire that staged bank first, then the active bank.
    clear_bank(true);
    clear_bank(false);
    const bool host_ledgers_empty =
        g_injected_icon_hosts.load(std::memory_order_acquire) == 0 &&
        g_staged_injected_icon_hosts.load(std::memory_order_acquire) == 0;
    const bool retirement_exact = all_cleared && host_ledgers_empty &&
        reconcile_rgba8_retirement_debt("epoch-clear-post-remove");
    const bool retirement_complete = retirement_exact &&
        all_rgba8_retirement_debts_zero();
    const InjectedIconsClearResult clear_result = !all_cleared ||
            !host_ledgers_empty || !retirement_exact
        ? InjectedIconsClearResult::failed
        : (retirement_complete
            ? InjectedIconsClearResult::cleared
            : (allow_already_absent
                ? InjectedIconsClearResult::waiting_for_renderer_retirement
                : InjectedIconsClearResult::failed));
    log_rgba8_lifecycle_snapshot(
        clear_result == InjectedIconsClearResult::cleared
            ? "post-clear"
            : (clear_result ==
                    InjectedIconsClearResult::waiting_for_renderer_retirement
                ? "post-clear-waiting-retirement"
                : "post-clear-failed"),
        top);
    if (clear_result == InjectedIconsClearResult::cleared) {
        g_injected_movie_fingerprint = {};
        g_icon_bank_b_hosts.store(0, std::memory_order_release);
    }
    return clear_result;
}

// Scaleform may publish the next Configuration movie while renderer-side
// RawImage references from the preceding movie are still retiring.  The old
// Shapes have already been detached at this point, so exact retirement debt
// is the only ownership allowed to cross that movie boundary.  A new movie may
// present vector-only placeholders during that short interval; attaching a
// retained RGBA resource remains blocked by arm_catalogue_presentation_refresh
// until every debt reaches zero.
[[nodiscard]] bool accept_vector_only_epoch_clear_result(
    void* top,
    InjectedIconsClearResult clear_result,
    const char* phase) noexcept {
    if (clear_result == InjectedIconsClearResult::cleared) return true;

    LiveTabGeometry geometry{};
    const bool geometry_read = read_live_tab_geometry(top, geometry);
    bool active_assets_empty = true;
    bool staged_assets_empty = true;
    if (g_rgba8_host_assets.size() !=
        g_staged_rgba8_host_assets.size()) {
        active_assets_empty = false;
        staged_assets_empty = false;
    } else {
        for (std::size_t bit = 0;
             bit < g_rgba8_host_assets.size();
             ++bit) {
            active_assets_empty = active_assets_empty &&
                g_rgba8_host_assets[bit].load(
                    std::memory_order_acquire) == 0U;
            staged_assets_empty = staged_assets_empty &&
                g_staged_rgba8_host_assets[bit].load(
                    std::memory_order_acquire) == 0U;
        }
    }

    const InPlaceRgba8RebindTransaction& transaction =
        g_in_place_rgba8_rebind_transaction;
    const Rgba8RapidEpochHandoffState proof{
        .exact_waiting_for_renderer_retirement =
            clear_result == InjectedIconsClearResult::
                waiting_for_renderer_retirement,
        .physical_geometry_matches = geometry_read &&
            geometry.active == active_physical_tab_count(),
        .active_injected_hosts_empty =
            g_injected_icon_hosts.load(std::memory_order_acquire) == 0U,
        .staged_injected_hosts_empty =
            g_staged_injected_icon_hosts.load(
                std::memory_order_acquire) == 0U,
        .active_rgba_hosts_empty =
            g_rgba8_icon_hosts.load(std::memory_order_acquire) == 0U,
        .staged_rgba_hosts_empty =
            g_staged_rgba8_icon_hosts.load(
                std::memory_order_acquire) == 0U,
        .active_asset_ledger_empty = active_assets_empty,
        .staged_asset_ledger_empty = staged_assets_empty,
        .transaction_active = transaction.active,
        .transaction_failed = transaction.failed,
        .destructive_runtime_failure =
            g_runtime.destructive_transition_failed,
    };
    if (!can_begin_vector_only_epoch_during_rgba8_retirement(proof)) {
        return false;
    }

    // No display object remains associated with the old movie fingerprint or
    // either property bank.  Retirement debt deliberately remains untouched:
    // it is the exact renderer ownership that the deferred RGBA handoff waits
    // to observe reaching zero.
    g_injected_movie_fingerprint = {};
    g_icon_bank_b_hosts.store(0, std::memory_order_release);
    log_line(
        "RGBA8 rapid epoch handoff accepted phase=%s active=%zu vectorOnly=1 retirementPending=1",
        phase ? phase : "unspecified",
        geometry.active);
    return true;
}

bool freeze_active_two_phase_rgba8_transaction(
    const char* reason) noexcept {
    InPlaceRgba8RebindTransaction& transaction =
        g_in_place_rgba8_rebind_transaction;
    if (!transaction.active) return false;

    const InPlaceRgba8RebindStage stage = transaction.stage;
    const TabHostMask completed_clear =
        transaction.completed_clear_mask;
    transaction.failed = true;
    transaction.active = false;
    g_rgba8_publication_disabled.store(true, std::memory_order_release);
    g_runtime.destructive_transition_failed = true;
    log_failure(
        "two-phase RGBA8 transaction frozen after exact state was lost (stage=%u cleared=0x%04X reason=%s); ownership ledgers are retained and no rollback or manual release is attempted",
        static_cast<unsigned>(stage),
        static_cast<unsigned>(completed_clear),
        reason ? reason : "unspecified");
    return true;
}

void progress_two_phase_rgba8_retirement(
    void* top,
    std::uint64_t frame_sequence) noexcept {
    InPlaceRgba8RebindTransaction& transaction =
        g_in_place_rgba8_rebind_transaction;
    if (!transaction.active || transaction.failed ||
        transaction.stage !=
            InPlaceRgba8RebindStage::waiting_retirement) {
        return;
    }

    std::uint32_t selected_physical{};
    const bool selected_exact = read_selected_physical(
            top, selected_physical) &&
        g_runtime.pending.active &&
        selected_physical == g_runtime.pending.desired_physical;
    const bool coordinates_exact = top && transaction.top == top &&
        g_runtime.active && g_runtime.top == top &&
        transaction.presentation_epoch == g_runtime.presentation_epoch &&
        transaction.source_generation ==
            g_runtime.viewport.mapping_generation() &&
        transaction.source_window == g_runtime.viewport.window_start() &&
        records_match_mapping(top, g_runtime.viewport) &&
        g_runtime.pending.active && g_runtime.pending.top == top &&
        transaction.target_generation ==
            g_runtime.pending.candidate.mapping_generation() &&
        transaction.target_window ==
            g_runtime.pending.candidate.window_start() &&
        transaction.completed_clear_mask == 0 && selected_exact;
    InjectedMovieFingerprint current_fingerprint{};
    const bool fingerprint_exact = coordinates_exact &&
        sample_current_movie_fingerprint(top, current_fingerprint) &&
        same_movie_fingerprint(
            current_fingerprint,
            g_injected_movie_fingerprint);
    const bool reconciled = coordinates_exact && fingerprint_exact &&
        reconcile_rgba8_retirement_debt(
            "two-phase-wait-post-native");
    const TabHostMask injected = g_injected_icon_hosts.load(
        std::memory_order_acquire);
    const TabHostMask rgba = g_rgba8_icon_hosts.load(
        std::memory_order_acquire);
    bool source_assets_exact = true;
    bool staged_assets_empty = true;
    for (std::size_t bit = 0; bit < active_tab_host_count(); ++bit) {
        source_assets_exact = source_assets_exact &&
            g_rgba8_host_assets[bit].load(std::memory_order_acquire) ==
                transaction.source_assets[bit];
        staged_assets_empty = staged_assets_empty &&
            g_staged_rgba8_host_assets[bit].load(
                std::memory_order_acquire) == 0;
    }
    // Reconciliation is also the reference audit for this wait frame: it
    // samples every visible or retiring RawImage once, accepts only the bounded
    // [1 + active, 1 + active + debt] range, and updates debt from that same
    // observation. A second refcount read here could race the renderer's next
    // asynchronous decrement and falsely fail an otherwise valid retirement.
    const bool refs_exact = reconciled;
    const bool ledger_exact = refs_exact &&
        injected == transaction.source_injected_mask &&
        rgba == transaction.source_rgba_mask && source_assets_exact &&
        g_staged_injected_icon_hosts.load(std::memory_order_acquire) == 0 &&
        g_staged_rgba8_icon_hosts.load(std::memory_order_acquire) == 0 &&
        staged_assets_empty;
    const bool debts_zero = all_rgba8_retirement_debts_zero();
    const bool crossed_native_frame =
        frame_sequence > transaction.clear_frame;
    const bool timed_out = frame_sequence >= transaction.clear_frame &&
        frame_sequence - transaction.clear_frame >=
            kInPlaceRebindRetirementTimeoutFrames;

    if (coordinates_exact && fingerprint_exact && ledger_exact &&
        debts_zero && crossed_native_frame) {
        transaction.retirement_completed_qpc =
            query_performance_ticks();
        transaction.retirement_wait_frames =
            frame_sequence - transaction.clear_frame;
        transaction.stage =
            InPlaceRgba8RebindStage::ready_to_repaint;
        log_line(
            "rgba8Handoff transition=%llu prior renderer debt retired frame=%llu waitedFrames=%llu source banks stayed visible; atomic target repaint armed",
            static_cast<unsigned long long>(
                transaction.transition_sequence),
            static_cast<unsigned long long>(frame_sequence),
            static_cast<unsigned long long>(
                frame_sequence - transaction.clear_frame));
        return;
    }

    if (coordinates_exact && fingerprint_exact && ledger_exact &&
        !timed_out) {
        return;
    }

    const std::uint64_t pointer_dwell_ticket =
        g_runtime.pending.pointer_dwell_ticket;
    transaction.failed = true;
    transaction.active = false;
    g_rgba8_publication_disabled.store(
        true, std::memory_order_release);
    g_runtime.destructive_transition_failed = true;
    g_runtime.pending = {};
    fail_pointer_dwell_transition(
        pointer_dwell_ticket,
        timed_out
            ? "two-phase RGBA8 renderer retirement timed out"
            : "two-phase RGBA8 renderer retirement lost exact state");
    log_failure(
        "atomic RGBA8 handoff debt wait failed closed (frame=%llu clearFrame=%llu timeout=%u coordinates=%u selected=%u/%u fingerprint=%u reconciled=%u ledger=%u debtsZero=%u injected=0x%04X rgba=0x%04X sourceAssets=%u stagedEmpty=%u); source Shapes remained visible",
        static_cast<unsigned long long>(frame_sequence),
        static_cast<unsigned long long>(transaction.clear_frame),
        timed_out ? 1U : 0U,
        coordinates_exact ? 1U : 0U,
        static_cast<unsigned>(selected_physical),
        selected_exact ? 1U : 0U,
        fingerprint_exact ? 1U : 0U,
        reconciled ? 1U : 0U,
        ledger_exact ? 1U : 0U,
        debts_zero ? 1U : 0U,
        static_cast<unsigned>(injected),
        static_cast<unsigned>(rgba),
        source_assets_exact ? 1U : 0U,
        staged_assets_empty ? 1U : 0U);
}

} // namespace native_tabs_detail
