// Pending tab transitions, the selection callback and the composite-frame detour.
#include "native_tabs_internal.hpp"

namespace native_tabs_detail {

const char* transition_kind(
    std::size_t old_physical,
    std::size_t new_physical) noexcept {
    const std::size_t count = active_physical_tab_count();
    if ((old_physical + 1) % count == new_physical) {
        return "next";
    }
    if ((old_physical + count - 1) % count ==
        new_physical) {
        return "previous";
    }
    return "direct";
}

const char* selection_source_name(TabSelectionSource source) noexcept {
    switch (source) {
    case TabSelectionSource::pointer:
        return "pointer";
    case TabSelectionSource::navigation_previous:
        return "navigation-previous";
    case TabSelectionSource::navigation_next:
        return "navigation-next";
    case TabSelectionSource::unknown:
        break;
    }
    return "unknown";
}

TabSelectionSource consume_tab_selection_source() noexcept {
    const TabSelectionSource source = g_tab_selection_source;
    g_tab_selection_source = TabSelectionSource::unknown;
    return source;
}

TabViewportChange calculate_transition(
    TabViewport& candidate,
    std::size_t old_physical,
    std::size_t new_physical,
    TabSelectionSource source) noexcept {
    static_cast<void>(old_physical);
    if (source == TabSelectionSource::navigation_next) {
        return candidate.move(
            TabDirection::next, TabBoundaryBehavior::wrap);
    }
    if (source == TabSelectionSource::navigation_previous) {
        return candidate.move(
            TabDirection::previous, TabBoundaryBehavior::wrap);
    }
    // Pointer input resolves the physical cell against the mapping that is
    // actually visible. Never infer circular next/previous from 0 <-> 7: a
    // direct mouse hover on the opposite edge is not controller wrap.
    return candidate.select_physical_aligned(new_physical);
}

bool arm_catalogue_presentation_refresh(void* top) noexcept {
    if (!g_runtime.catalogue_presentation_refresh.pending()) return false;

    InPlaceRgba8RebindTransaction& transaction =
        g_in_place_rgba8_rebind_transaction;
    if (!top || !g_runtime.active || g_runtime.top != top ||
        !g_rgba8_catalogue_ready.load(std::memory_order_acquire) ||
        g_rgba8_publication_disabled.load(std::memory_order_acquire) ||
        g_mutating_viewport || g_runtime.pending.active ||
        transaction.active || transaction.failed ||
        g_runtime.destructive_transition_failed ||
        !all_rgba8_retirement_debts_zero() ||
        !validate_top(top) ||
        !records_match_mapping(top, g_runtime.viewport)) {
        return false;
    }

    const DeferredTabPresentationRefreshPoll refresh_poll =
        g_runtime.catalogue_presentation_refresh.poll(
            g_runtime.presentation_epoch,
            g_runtime.viewport.mapping_generation(),
            g_composite_frame_sequence);
    if (refresh_poll != DeferredTabPresentationRefreshPoll::ready) {
        return false;
    }

    InjectedMovieFingerprint current_fingerprint{};
    const bool fingerprint_exact =
        sample_current_movie_fingerprint(top, current_fingerprint) &&
        same_movie_fingerprint(
            current_fingerprint, g_injected_movie_fingerprint) &&
        g_injected_movie_fingerprint.presentation_epoch ==
            g_runtime.presentation_epoch;
    if (!fingerprint_exact) return false;

    InjectedHostLayout source{};
    InjectedHostLayout target{};
    if (!snapshot_injected_host_layout(source) ||
        !derive_injected_host_layout(g_runtime.viewport, target)) {
        g_runtime.catalogue_presentation_refresh.consume();
        log_failure(
            "initial catalogue presentation refresh could not derive an exact source/target host layout; vector fallbacks remain visible");
        return false;
    }

    if (same_injected_host_layout(source, target)) {
        // Capacities eight and nine expose only stock tabs plus the overflow
        // marker on the initial window, so catalogue publication changes no
        // visible presentation and needs no display-tree mutation.
        g_runtime.catalogue_presentation_refresh.consume();
        log_line(
            "initial catalogue presentation refresh not required for the current viewport");
        return false;
    }

    if (!exact_initial_catalogue_upgrade(source, target)) {
        g_runtime.catalogue_presentation_refresh.consume();
        log_failure(
            "initial catalogue presentation refresh refused a non-vector-to-RGBA ownership topology (source={injected:0x%08X rgba:0x%08X vector:0x%08X} target={injected:0x%08X rgba:0x%08X vector:0x%08X}); vector fallbacks remain visible",
            static_cast<unsigned>(source.injected_mask),
            static_cast<unsigned>(source.rgba_mask),
            static_cast<unsigned>(source.vector_mask),
            static_cast<unsigned>(target.injected_mask),
            static_cast<unsigned>(target.rgba_mask),
            static_cast<unsigned>(target.vector_mask));
        return false;
    }

    std::uint32_t selected_physical{};
    const std::size_t selected_logical =
        g_runtime.viewport.selected_logical();
    const std::size_t expected_physical =
        g_runtime.viewport.selected_physical();
    const LogicalTab* const selected_tab =
        runtime_logical_tab(selected_logical);
    if (!selected_tab ||
        expected_physical >= active_physical_tab_count() ||
        !read_selected_physical(top, selected_physical) ||
        selected_physical != expected_physical) {
        return false;
    }

    PendingTransition refresh{};
    refresh.active = true;
    refresh.presentation_refresh = true;
    refresh.top = top;
    refresh.candidate = g_runtime.viewport;
    refresh.native_physical = selected_physical;
    refresh.desired_physical = selected_physical;
    refresh.provisional_logical = selected_logical;
    refresh.desired_logical = selected_logical;
    refresh.previous_logical = selected_logical;
    refresh.provisional_category = selected_tab->category;
    refresh.desired_category = selected_tab->category;
    refresh.requested_qpc = query_performance_ticks();
    g_runtime.pending = refresh;
    g_runtime.catalogue_presentation_refresh.consume();
    log_line(
        "initial catalogue presentation refresh armed epoch=%llu window=%zu generation=%llu sourceVector=0x%08X targetRgba=0x%08X",
        static_cast<unsigned long long>(g_runtime.presentation_epoch),
        g_runtime.viewport.window_start(),
        static_cast<unsigned long long>(
            g_runtime.viewport.mapping_generation()),
        static_cast<unsigned>(source.vector_mask),
        static_cast<unsigned>(target.rgba_mask));
    return true;
}

void abandon_pending_to_native_selection() noexcept {
    if (!g_runtime.pending.active) return;
    const std::uint32_t native_physical =
        g_runtime.pending.native_physical;
    const std::uint64_t pointer_dwell_ticket =
        g_runtime.pending.pointer_dwell_ticket;
    g_runtime.pending = {};
    fail_pointer_dwell_transition(
        pointer_dwell_ticket,
        "viewport remap was abandoned");
    const TabViewportChange fallback =
        g_runtime.viewport.select_physical(native_physical);
    if (!fallback.valid) {
        // Active native records may borrow catalogue strings. Even this
        // nominally unreachable corrupt fallback must preserve their owner.
        g_rgba8_publication_disabled.store(
            true, std::memory_order_release);
        g_runtime.destructive_transition_failed = true;
        log_failure(
            "pending-transition fallback index was invalid; runtime storage is retained and viewport mutation is suspended");
    }
}

PendingTransitionCommitResult commit_pending_transition(
    void* top) noexcept {
    PendingTransition& pending = g_runtime.pending;
    const auto freeze_destructive_transaction =
        [&pending](const char* reason) noexcept {
            if (!g_in_place_rgba8_rebind_transaction.active) return false;
            const std::uint64_t ticket = pending.pointer_dwell_ticket;
            (void)freeze_active_two_phase_rgba8_transaction(reason);
            pending = {};
            fail_pointer_dwell_transition(ticket, reason);
            return true;
        };
    if (!pending.active || pending.top != top || !validate_top(top) ||
        !records_match_mapping(top, g_runtime.viewport)) {
        (void)freeze_destructive_transaction(
            "pending transition validation or source mapping failed");
        return PendingTransitionCommitResult::failed;
    }
    if (g_in_place_rgba8_rebind_transaction.active &&
        !g_in_place_rgba8_rebind_transaction.failed &&
        g_in_place_rgba8_rebind_transaction.stage ==
            InPlaceRgba8RebindStage::waiting_retirement) {
        return PendingTransitionCommitResult::waiting;
    }

    const bool repaint_required =
        pending.presentation_refresh ||
        pending.candidate.mapping_generation() !=
            g_runtime.viewport.mapping_generation() ||
        pending.candidate.window_start() !=
            g_runtime.viewport.window_start() ||
        tab_overflow_indicators_changed(
            g_runtime.viewport, pending.candidate);
    if (pending.diagnose_wrap) {
        log_wrap_panel_state(top, "commit-before-remap",
            pending.previous_logical, pending.desired_logical);
    }

    g_mutating_viewport = true;
    if (repaint_required) {
        ResolvedTabLayers layers{};
        if (!resolve_tab_layers(top, layers)) {
            g_mutating_viewport = false;
            log_failure(
                "viewport remap aborted before mutation: dual-layer cells unavailable");
            if (freeze_destructive_transaction(
                    "dual-layer cells unavailable after phase-A clear")) {
                return PendingTransitionCommitResult::failed;
            }
            abandon_pending_to_native_selection();
            return PendingTransitionCommitResult::failed;
        }
        const bool selector_already_relocated =
            g_in_place_rgba8_rebind_transaction.active &&
            (g_in_place_rgba8_rebind_transaction.stage ==
                    InPlaceRgba8RebindStage::ready_to_repaint ||
                g_in_place_rgba8_rebind_transaction.stage ==
                    InPlaceRgba8RebindStage::repainting);
        if (!selector_already_relocated &&
            pending.desired_physical != pending.native_physical &&
            !invoke_selector_select(top, pending.desired_physical)) {
            g_mutating_viewport = false;
            destroy_tab_layers(layers);
            log_failure(
                "viewport remap aborted: selector refused physical=%u",
                static_cast<unsigned>(pending.desired_physical));
            if (freeze_destructive_transaction(
                    "selector relocation failed during transaction")) {
                return PendingTransitionCommitResult::failed;
            }
            abandon_pending_to_native_selection();
            return PendingTransitionCommitResult::failed;
        }
        if (!apply_mapping(top, pending.candidate, layers)) {
            if (freeze_destructive_transaction(
                    "two-phase Shape detach/target repaint failed")) {
                g_mutating_viewport = false;
                return PendingTransitionCommitResult::failed;
            }
            if (pending.desired_physical != pending.native_physical) {
                (void)invoke_selector_select(
                    top, pending.native_physical);
            }
            g_mutating_viewport = false;
            log_failure("viewport remap record rewrite/repaint failed");
            abandon_pending_to_native_selection();
            return PendingTransitionCommitResult::failed;
        }
        if (g_in_place_rgba8_rebind_transaction.active &&
            !g_in_place_rgba8_rebind_transaction.failed &&
            g_in_place_rgba8_rebind_transaction.stage ==
                InPlaceRgba8RebindStage::waiting_retirement) {
            g_mutating_viewport = false;
            return PendingTransitionCommitResult::waiting;
        }
    } else {
        // A content-only route transaction must leave every record and icon
        // byte untouched. The native selector already points at its target.
        if (pending.candidate.mapping_generation() !=
                g_runtime.viewport.mapping_generation() ||
            pending.candidate.window_start() !=
                g_runtime.viewport.window_start() ||
            tab_overflow_indicators_changed(
                g_runtime.viewport, pending.candidate)) {
            g_mutating_viewport = false;
            const std::uint64_t ticket = pending.pointer_dwell_ticket;
            pending = {};
            fail_pointer_dwell_transition(
                ticket, "content-only transaction changed presentation");
            return PendingTransitionCommitResult::failed;
        }
        log_line(
            "graphicsCarrier contentOnly logical=%zu->%zu window=%zu generation=%llu iconRepaint=0 recordRewrite=0",
            pending.previous_logical, pending.desired_logical,
            g_runtime.viewport.window_start(),
            static_cast<unsigned long long>(
                g_runtime.viewport.mapping_generation()));
    }

    if (pending.diagnose_wrap) {
        log_wrap_panel_state(top, "commit-after-mapping",
            pending.previous_logical, pending.desired_logical);
    }

    const bool route_transition =
        pending.previous_logical != pending.desired_logical &&
        (logical_uses_graphics_carrier(pending.previous_logical) ||
            logical_uses_graphics_carrier(pending.desired_logical));
    if (pending.provisional_category != pending.desired_category ||
        route_transition ||
        pending.previous_logical != pending.desired_logical) {
        const bool custom_route_transition =
            is_custom_logical(pending.previous_logical) ||
            is_custom_logical(pending.desired_logical);
        invoke_switch_panel(top, pending.desired_category,
            custom_route_transition
                ? PanelStateIsolationReason::corrective
                : PanelStateIsolationReason::none);
        if (!g_research_switch_succeeded) {
            const std::uint64_t ticket = pending.pointer_dwell_ticket;
            if (repaint_required) {
                // Records/icons/selector now describe the candidate. Publish
                // it before freezing further carrier transitions.
                g_runtime.viewport = pending.candidate;
                pending = {};
                g_mutating_viewport = false;
                fail_pointer_dwell_transition(
                    ticket, "carrier switch failed after viewport repaint");
                return PendingTransitionCommitResult::failed;
            }
            const std::uint32_t old_physical =
                static_cast<std::uint32_t>(
                    g_runtime.viewport.selected_physical());
            const bool restored =
                invoke_selector_select(top, old_physical);
            std::uint32_t observed{};
            const bool exact = restored &&
                read_selected_physical(top, observed) &&
                observed == old_physical;
            pending = {};
            g_mutating_viewport = false;
            fail_pointer_dwell_transition(
                ticket, "content-only carrier switch failed");
            log_failure(
                "Graphics carrier content-only failure selectorRestore={call:%u observed:%u exact:%u}; custom transitions frozen, native Back remains live",
                restored ? 1u : 0u, static_cast<unsigned>(observed),
                exact ? 1u : 0u);
            return PendingTransitionCommitResult::failed;
        }
    }

    if (!pending.presentation_refresh) {
        log_research_carrier_selection(top, "commit",
            pending.desired_logical, pending.desired_category);
    }
    if (pending.diagnose_wrap) {
        log_wrap_panel_state(top, "commit-after-switch",
            pending.previous_logical, pending.desired_logical);
    }
    if (!pending.presentation_refresh) invoke_refresh_title(top);

    const std::uint32_t event =
        g_transition_log_count.fetch_add(1, std::memory_order_relaxed) + 1;
    if (event <= kMaximumTransitionLogs) {
        log_line(
            "remap[%u] kind=%s nativePhysical=%u desiredPhysical=%u provisionalLogical=%zu desiredLogical=%zu category=%d->%d window=%zu->%zu generation=%llu",
            static_cast<unsigned>(event),
            pending.presentation_refresh
                ? "catalogue-refresh"
                : (repaint_required ? "presentation" : "content-only"),
            static_cast<unsigned>(pending.native_physical),
            static_cast<unsigned>(pending.desired_physical),
            pending.provisional_logical, pending.desired_logical,
            pending.provisional_category, pending.desired_category,
            g_runtime.viewport.window_start(),
            pending.candidate.window_start(),
            static_cast<unsigned long long>(
                pending.candidate.mapping_generation()));
    }
    const bool completed_catalogue_refresh =
        pending.presentation_refresh;
    const std::uint64_t ticket = pending.pointer_dwell_ticket;
    g_runtime.viewport = pending.candidate;
    pending = {};
    g_mutating_viewport = false;
    if (!completed_catalogue_refresh) {
        finish_pointer_dwell_transition(
            ticket, repaint_required
                ? "viewport remap committed"
                : "content-only carrier route committed");
    }
#if defined(ER_NATIVE_UI_PRODUCTION_TABS)
    if (completed_catalogue_refresh) {
        erui::detail::logf(
            erui::LogLevel::info,
            "NativeTabs initial custom-icon presentation refresh completed: physical=%zu logical=%zu",
            active_physical_tab_count(),
            active_logical_tab_count());
    }
#endif
    return PendingTransitionCommitResult::committed;
}

void __fastcall selection_callback_detour(
    void* callable,
    const std::int32_t* physical_index) noexcept {
    SelectionCallbackFn const original =
        g_original_selection.load(std::memory_order_acquire);
    if (!original) return;
    if (!g_viewport_callbacks_armed.load(std::memory_order_acquire)) {
        original(callable, physical_index);
        return;
    }
    if (g_mutating_viewport) {
        g_tab_selection_source = TabSelectionSource::unknown;
        return;
    }

    std::int32_t next_physical{-1};
    void* top{};
    const bool physical_read = guarded_read(
        physical_index, next_physical);
    const bool top_read = callable && guarded_read(
        static_cast<const std::byte*>(callable) + sizeof(void*),
        top);
    const bool belongs_to_active_runtime = top_read &&
        g_runtime.active && g_runtime.top == top;
    if (!physical_read || !top_read ||
        next_physical < 0 ||
        next_physical >=
            static_cast<std::int32_t>(active_physical_tab_count()) ||
        !g_runtime.active || g_runtime.top != top) {
        if (belongs_to_active_runtime &&
            (g_pointer_dwell.policy.active() ||
                g_pointer_dwell.timed_intent.active)) {
            reset_pointer_dwell(
                "active tab selection callback failed validation");
            g_tab_selection_source = TabSelectionSource::unknown;
        }
        original(callable, physical_index);
        return;
    }

    const TabSelectionSource selection_source =
        consume_tab_selection_source();

    // Blocking the composite input byte cannot cancel a horizontal repeat
    // that Elden Ring latched earlier in the same dispatch. Such a callback
    // can therefore arrive during the one-renderer-frame RGBA8 retirement
    // wait. It must never replace the authoritative PendingTransition or move
    // the selector away from that transaction's source/target coordinates.
    if (g_runtime.pending.active && g_runtime.pending.top == top) {
        PendingTransition& pending = g_runtime.pending;
        const InPlaceRgba8RebindTransaction& transaction =
            g_in_place_rgba8_rebind_transaction;
        const bool target_relocated = transaction.active &&
            transaction.top == top;
        const std::uint32_t anchor = tab_transition_selector_anchor(
            target_relocated
                ? TabTransitionSelectorAnchor::target
                : TabTransitionSelectorAnchor::source,
            pending.native_physical,
            pending.desired_physical);

        bool restore_call{};
        {
            ScopedBooleanTrue mutating{g_mutating_viewport};
            restore_call = invoke_selector_select(top, anchor);
        }
        std::uint32_t restored_physical{};
        const bool restore_read = read_selected_physical(
            top, restored_physical);
        const bool restored = restore_read && restored_physical == anchor;

        g_pointer_dwell.natural_intent = {};
        g_pointer_dwell.timed_intent = {};
        if ((selection_source ==
                TabSelectionSource::navigation_previous ||
             selection_source == TabSelectionSource::navigation_next) &&
            g_pointer_dwell.policy.active()) {
            reset_pointer_dwell(
                "controller repeat overlapped a pending tab transition");
        }

        const std::uint32_t event =
            g_serialized_selection_log_count.fetch_add(
                1, std::memory_order_relaxed) + 1;
        if (event <= kMaximumTransitionLogs) {
            log_line(
                "heldOverlap[%u] source=%s callbackPhysical=%d phase=%s anchor=%u restore={call:%u read:%u observed:%u exact:%u} pending={native:%u desired:%u logical:%zu->%zu window:%zu->%zu generation:%llu}; callback serialized without replacing pending transition",
                static_cast<unsigned>(event),
                selection_source_name(selection_source),
                next_physical,
                target_relocated ? "target" : "source",
                static_cast<unsigned>(anchor),
                restore_call ? 1U : 0U,
                restore_read ? 1U : 0U,
                static_cast<unsigned>(restored_physical),
                restored ? 1U : 0U,
                static_cast<unsigned>(pending.native_physical),
                static_cast<unsigned>(pending.desired_physical),
                pending.previous_logical,
                pending.desired_logical,
                g_runtime.viewport.window_start(),
                pending.candidate.window_start(),
                static_cast<unsigned long long>(
                    pending.candidate.mapping_generation()));
        }
        if (!restored) {
            const std::uint64_t pointer_dwell_ticket =
                pending.pointer_dwell_ticket;
            if (transaction.active) {
                (void)freeze_active_two_phase_rgba8_transaction(
                    "overlapping selection could not restore the in-flight selector anchor");
            }
            pending = {};
            fail_pointer_dwell_transition(
                pointer_dwell_ticket,
                "overlapping selection could not restore its selector anchor");
            log_failure(
                "held-repeat serialization could not restore selector anchor=%u (call=%u read=%u observed=%u); the overlapping callback remained suppressed",
                static_cast<unsigned>(anchor),
                restore_call ? 1U : 0U,
                restore_read ? 1U : 0U,
                static_cast<unsigned>(restored_physical));
        }
        return;
    }

    if (!records_match_mapping(top, g_runtime.viewport)) {
        if (g_pointer_dwell.policy.active() ||
            g_pointer_dwell.timed_intent.active) {
            reset_pointer_dwell(
                "active tab selection callback failed source mapping validation");
        }
        original(callable, physical_index);
        return;
    }

    if (selection_source == TabSelectionSource::navigation_previous ||
        selection_source == TabSelectionSource::navigation_next) {
        if (g_pointer_dwell.policy.active()) {
            reset_pointer_dwell("controller selection callback");
        }
    }

    const std::size_t old_physical =
        g_runtime.viewport.selected_physical();
    if (old_physical >= active_physical_tab_count()) {
        if (g_pointer_dwell.policy.active() ||
            g_pointer_dwell.timed_intent.active) {
            reset_pointer_dwell(
                "active viewport had no selected physical tab");
        }
        original(callable, physical_index);
        return;
    }

    TabViewport candidate = g_runtime.viewport;
    const std::size_t new_physical =
        static_cast<std::size_t>(next_physical);
    const PointerDwellNaturalIntent natural_intent =
        g_pointer_dwell.natural_intent;
    const PointerDwellTimedIntent timed_intent =
        g_pointer_dwell.timed_intent;
    g_pointer_dwell.natural_intent = {};
    g_pointer_dwell.timed_intent = {};
    const bool natural_dwell_transition =
        g_mouse_dwell_enabled &&
        selection_source == TabSelectionSource::pointer &&
        natural_intent.active && natural_intent.top == top &&
        natural_intent.physical == new_physical &&
        !g_pointer_dwell.policy.active() &&
        overflow_edge_for_physical(
            g_runtime.viewport, new_physical) == natural_intent.edge;
    const bool timed_dwell_transition =
        selection_source == TabSelectionSource::pointer &&
        timed_intent.active && timed_intent.top == top &&
        timed_intent.physical == new_physical &&
        g_pointer_dwell.policy.phase() ==
            TabPointerDwellPhase::awaiting_commit &&
        g_pointer_dwell.top == top;
    std::size_t provisional_logical{invalid_tab_index};
    (void)g_runtime.viewport.logical_for_physical(
        new_physical, provisional_logical);
    const TabViewportChange change = calculate_transition(
        candidate,
        old_physical,
        new_physical,
        selection_source);
    // Selecting or leaving an edge can reveal/hide its presentation-only
    // overflow marker without moving the logical viewport window. It uses the
    // same fresh-Shape transaction as a mapping change.
    const bool presentation_changed = change.valid &&
        tab_overflow_indicators_changed(g_runtime.viewport, candidate);
    const bool frozen_carrier_change = change.valid &&
        change.logical_selection_changed && g_research_failed &&
        (logical_uses_graphics_carrier(change.previous_logical) ||
            logical_uses_graphics_carrier(change.selected_logical));
    const bool freeze_mapping_change = change.valid &&
        (((change.mapping_changed || presentation_changed) &&
             g_runtime.destructive_transition_failed) ||
            frozen_carrier_change);
    if (freeze_mapping_change) {
        g_mutating_viewport = true;
        const bool restore_call = invoke_selector_select(
            top, static_cast<std::uint32_t>(old_physical));
        std::uint32_t restore_readback{};
        const bool restore_read = read_selected_physical(
            top, restore_readback);
        g_mutating_viewport = false;
        g_runtime.pending = {};
        reset_pointer_dwell(
            "a destructive RGBA8 transition previously failed");
        const bool restored = restore_call && restore_read &&
            restore_readback == old_physical;
        log_line(
            "rgba8Rebind freeze refused remap after destructive failure completedTransitions=%llu logical=%zu->%zu window=%zu->%zu selectorRestore={call:%u read:%u observed:%u expected:%zu exact:%u} publicationDisabled=%u",
            static_cast<unsigned long long>(
                g_runtime.completed_shape_replacement_transitions),
            change.previous_logical,
            change.selected_logical,
            change.previous_window_start,
            change.window_start,
            restore_call ? 1U : 0U,
            restore_read ? 1U : 0U,
            static_cast<unsigned>(restore_readback),
            old_physical,
            restored ? 1U : 0U,
            g_rgba8_publication_disabled.load(
                std::memory_order_acquire) ? 1U : 0U);
        if (restored) return;

        // The validated selector setter is the only supported way to restore
        // the old cell. If it does not also read back exactly, keep the current
        // record window frozen and let the native callback finish on the
        // selector's observed cell. Selecting that physical cell without
        // alignment cannot remap the viewport, so the highlight, page, and
        // runtime logical selection converge without another GFX mutation.
        const std::size_t corrective_physical = restore_read
            ? static_cast<std::size_t>(restore_readback)
            : (restore_call ? old_physical : new_physical);
        TabViewport corrective = g_runtime.viewport;
        const TabViewportChange corrective_change =
            corrective.select_physical(corrective_physical);
        const bool corrective_exact = corrective_change.valid &&
            !corrective_change.mapping_changed &&
            corrective_change.selected_physical == corrective_physical;
        if (corrective_exact) {
            g_runtime.viewport = corrective;
        }
        const std::int32_t corrective_index = static_cast<std::int32_t>(
            corrective_exact ? corrective_physical : new_physical);
        log_failure(
            "rgba8Rebind selector restoration was not exact; applying fail-closed native callback correction without viewport remap (restoreCall=%u restoreRead=%u observed=%u callbackPhysical=%zu correctivePhysical=%d correctiveExact=%u restartRequired=1)",
            restore_call ? 1U : 0U,
            restore_read ? 1U : 0U,
            static_cast<unsigned>(restore_readback),
            new_physical,
            corrective_index,
            corrective_exact ? 1U : 0U);
        original(callable, &corrective_index);
        return;
    }
    std::uint64_t pointer_dwell_ticket{};
    if (natural_dwell_transition && change.valid &&
        change.logical_selection_changed) {
        g_pointer_dwell.policy.begin_transition(
            natural_intent.edge,
            new_physical,
            natural_intent.x_bits,
            natural_intent.y_bits);
        if (g_pointer_dwell.policy.phase() ==
            TabPointerDwellPhase::awaiting_commit) {
            g_pointer_dwell.top = top;
            g_pointer_dwell.direction = direction_for_pointer_edge(
                natural_intent.edge);
            pointer_dwell_ticket = next_pointer_dwell_ticket();
            g_pointer_dwell.transaction_ticket = pointer_dwell_ticket;
            log_pointer_dwell(
                "begin", "native ellipsis hover selected immediately");
        }
    } else if (timed_dwell_transition && change.valid &&
        change.logical_selection_changed &&
        change.selected_logical == timed_intent.logical) {
        pointer_dwell_ticket = next_pointer_dwell_ticket();
        g_pointer_dwell.transaction_ticket = pointer_dwell_ticket;
        log_pointer_dwell(
            "accepted", "timed native callback matched target");
    } else if (timed_intent.active) {
        reset_pointer_dwell("timed native callback did not match target");
    } else if (selection_source == TabSelectionSource::pointer &&
        g_pointer_dwell.policy.active()) {
        reset_pointer_dwell("different pointer selection superseded dwell");
    }
    const bool diagnose_wrap = change.valid &&
        ((change.previous_logical == active_logical_tab_count() - 1 &&
             change.selected_logical == 0) ||
            (change.previous_logical == 0 &&
             change.selected_logical ==
                 active_logical_tab_count() - 1));

    const bool reusable_content_changed = change.valid &&
        change.logical_selection_changed &&
        (logical_uses_graphics_carrier(change.previous_logical) ||
            logical_uses_graphics_carrier(change.selected_logical));
    if (change.valid &&
        (change.mapping_changed || presentation_changed ||
            reusable_content_changed) &&
        change.selected_logical < active_logical_tab_count() &&
        change.selected_physical < active_physical_tab_count() &&
        provisional_logical < active_logical_tab_count()) {
        const LogicalTab* const provisional_tab =
            runtime_logical_tab(provisional_logical);
        const LogicalTab* const selected_tab =
            runtime_logical_tab(change.selected_logical);
        if (!provisional_tab || !selected_tab) {
            g_runtime.pending = {};
            fail_pointer_dwell_transition(
                pointer_dwell_ticket,
                "selection callback could not resolve logical catalogue entries");
            original(callable, physical_index);
            return;
        }
        g_runtime.pending = {
            true,
            diagnose_wrap,
            false,
            top,
            candidate,
            static_cast<std::uint32_t>(new_physical),
            static_cast<std::uint32_t>(change.selected_physical),
            provisional_logical,
            change.selected_logical,
            change.previous_logical,
            provisional_tab->category,
            selected_tab->category,
            pointer_dwell_ticket,
            query_performance_ticks(),
        };
    } else if (change.valid) {
        g_runtime.pending = {};
        g_runtime.viewport = candidate;
        finish_pointer_dwell_transition(
            pointer_dwell_ticket,
            "selection completed without viewport remap");
    } else {
        fail_pointer_dwell_transition(
            pointer_dwell_ticket,
            "selection callback produced an invalid transition");
    }

    const std::uint32_t event =
        g_transition_log_count.fetch_add(1, std::memory_order_relaxed) + 1;
    if (event <= kMaximumTransitionLogs) {
        log_line(
            "selection[%u] source=%s inferred=%s physical=%zu->%zu logical=%zu->%zu window=%zu->%zu mappingChanged=%u presentationChanged=%u pending=%u",
            static_cast<unsigned>(event),
            selection_source_name(selection_source),
            transition_kind(old_physical, new_physical),
            old_physical,
            new_physical,
            change.previous_logical,
            change.selected_logical,
            change.previous_window_start,
            change.window_start,
            change.mapping_changed ? 1u : 0u,
            presentation_changed ? 1u : 0u,
            g_runtime.pending.active ? 1u : 0u);
    }

    // The native callback intentionally sees the old physical mapping. If a
    // viewport shift is pending, the composite-frame boundary corrects it
    // before the selected panel receives input.
    if (diagnose_wrap) {
        log_wrap_panel_state(
            top,
            "selection-before-native",
            change.previous_logical,
            change.selected_logical);
    }
    const bool valid_native_categories = change.valid &&
        change.previous_logical < active_logical_tab_count() &&
        provisional_logical < active_logical_tab_count();
    const LogicalTab* const previous_tab = valid_native_categories
        ? runtime_logical_tab(change.previous_logical)
        : nullptr;
    const LogicalTab* const provisional_tab = valid_native_categories
        ? runtime_logical_tab(provisional_logical)
        : nullptr;
    const std::int32_t previous_category = valid_native_categories
            && previous_tab
        ? previous_tab->category
        : -1;
    const std::int32_t provisional_category = valid_native_categories
            && provisional_tab
        ? provisional_tab->category
        : -1;
    const bool true_custom_route_involved = valid_native_categories &&
        previous_tab && provisional_tab &&
        (is_custom_logical(change.previous_logical) ||
            is_custom_logical(provisional_logical) ||
            (g_runtime.pending.active &&
                is_custom_logical(g_runtime.pending.desired_logical)));
    const bool crosses_carrier_boundary = true_custom_route_involved &&
        previous_category != provisional_category;
    const bool provisional_differs_from_desired =
        true_custom_route_involved && g_runtime.pending.active &&
        g_runtime.pending.top == top &&
        g_runtime.pending.provisional_category !=
            g_runtime.pending.desired_category;
    const bool provisional_isolation_armed =
        (crosses_carrier_boundary || provisional_differs_from_desired) &&
        begin_panel_state_isolation(
            top,
            provisional_category,
            PanelStateIsolationReason::provisional);
    original(callable, physical_index);
    end_panel_state_isolation(provisional_isolation_armed);
    log_research_carrier_selection(
        top,
        "native-selection",
        provisional_logical,
        provisional_category);
    if (diagnose_wrap) {
        log_wrap_panel_state(
            top,
            "selection-after-native",
            change.previous_logical,
            change.selected_logical);
    }
}

// Runs the native composite frame with a zero input gate while
// g_blocking_composite_input is raised; the flag is restored on return.
void call_composite_frame_with_blocked_input(
    CompositeFrameFn original,
    void* composite,
    float frame_value) noexcept {
    std::uint8_t blocked_input{};
    ScopedBooleanTrue blocking{g_blocking_composite_input};
    original(composite, frame_value, &blocked_input);
}

void __fastcall composite_frame_detour(
    void* composite,
    float frame_value,
    const std::uint8_t* input_enabled) noexcept {
    CompositeFrameFn const original =
        g_original_composite_frame.load(std::memory_order_acquire);
    if (!original) return;
    if (!g_viewport_callbacks_armed.load(std::memory_order_acquire)) {
        original(composite, frame_value, input_enabled);
        return;
    }

    struct CompletedFrame {
        ~CompletedFrame() { ++g_research_completed_composite_frames; }
    } completed_frame;
    if (g_research_failed) {
        // Freeze only tab mutations. Never swallow native Back/System input.
        original(composite, frame_value, input_enabled);
        // Native teardown can run between frames or from work reached by the
        // original frame callback. Observe it after forwarding native input;
        // only its exact generation boundary may rearm a later menu epoch.
        (void)acknowledge_exact_carrier_teardown();
        return;
    }

    const std::uint64_t frame_sequence = ++g_composite_frame_sequence;

    void* const top = composite
        ? static_cast<std::byte*>(composite) - kCompositeOffset
        : nullptr;
    const bool transaction_active_at_entry =
        g_in_place_rgba8_rebind_transaction.active;
    const bool terminal_failure_at_entry =
        g_in_place_rgba8_rebind_transaction.failed &&
        g_in_place_rgba8_rebind_transaction.stage !=
            InPlaceRgba8RebindStage::none;
    bool force_blocked_input = transaction_active_at_entry;
    if (!g_mutating_viewport && top) {
        if (!ensure_runtime(top)) {
            // Quietly retry on later frames while Configuration construction
            // is still publishing its eight stock category records, unless a
            // two-phase transaction had already entered a destructive stage.
        } else {
            static_cast<void>(true) /* Graphics is the rows-only carrier */;
            (void)arm_catalogue_presentation_refresh(top);
            if (g_runtime.pending.active &&
                g_runtime.pending.top == top) {
                const bool catalogue_presentation_refresh =
                    g_runtime.pending.presentation_refresh;
                const bool diagnose_wrap =
                    g_runtime.pending.diagnose_wrap;
                const std::size_t previous_logical =
                    g_runtime.pending.previous_logical;
                const std::size_t desired_logical =
                    g_runtime.pending.desired_logical;
                const PendingTransitionCommitResult commit_result =
                    commit_pending_transition(top);
                const TabTransitionInputAction input_action =
                    tab_transition_input_action(commit_result);
                if (input_action ==
                    TabTransitionInputAction::invoke_with_blocked_input) {
                    call_composite_frame_with_blocked_input(
                        original, composite, frame_value);
                    void* const post_top = composite
                        ? static_cast<std::byte*>(composite) -
                            kCompositeOffset
                        : nullptr;
                    progress_two_phase_rgba8_retirement(
                        post_top, frame_sequence);
                    return;
                }
                if (input_action ==
                    TabTransitionInputAction::invoke_with_live_input) {
                    if (catalogue_presentation_refresh) {
                        // This internal repair was not initiated by native
                        // selection input. Keep its commit frame free of a
                        // simultaneous L1/R1 or pointer callback; ordinary
                        // live input resumes on the following frame.
                        call_composite_frame_with_blocked_input(
                            original, composite, frame_value);
                        return;
                    }
                    // The logical mapping, selector, page, title, and icon
                    // presentation are coherent now. Preserve Elden Ring's
                    // real input gate on this frame so its native held-button
                    // repeat state is not interrupted at every viewport shift.
                    // Retirement frames above remain blocked because their
                    // source/target mapping is intentionally incomplete.
                    if (diagnose_wrap) {
                        log_wrap_panel_state(
                            top,
                            "frame-before-live-call",
                            previous_logical,
                            desired_logical);
                    }
                    if (g_pointer_dwell.policy.active()) {
                        std::uint8_t enabled{};
                        const bool input_gate_read =
                            guarded_read(input_enabled, enabled);
                        const bool cancel_dwell_for_gate =
                            should_cancel_pointer_dwell_for_composite_input(
                                input_action,
                                input_gate_read,
                                enabled != 0);
                        if (cancel_dwell_for_gate) {
                            reset_pointer_dwell(
                                "composite input was disabled");
                        } else if (!input_gate_read || enabled == 0) {
                            // A native pointer selection transiently
                            // publishes a zero gate on the exact frame where
                            // the atomic tab handoff commits. Preserve the
                            // dwell for this one transition-controlled frame.
                            // If the gate remains unavailable, the ordinary
                            // path on the next frame cancels it.
                            log_pointer_dwell(
                                "gate-deferred",
                                "coherent commit deferred the transient disabled input gate");
                        }
                    }
                    original(composite, frame_value, input_enabled);
                    return;
                }
            }
        }

        InPlaceRgba8RebindTransaction& transaction =
            g_in_place_rgba8_rebind_transaction;
        if (transaction.active) {
            // An active transaction must be consumed by the pending-transition
            // branch above. Never let live native input run through an orphaned
            // phase-A clear or ready-to-repaint state.
            (void)freeze_active_two_phase_rgba8_transaction(
                "composite frame could not route the active transaction through its pending commit");
            g_runtime.pending = {};
            reset_pointer_dwell(
                "orphaned two-phase transaction was frozen");
        }
        const bool terminal_failure_now = transaction.failed &&
            transaction.stage != InPlaceRgba8RebindStage::none;
        // Block only the frame that loses or freezes the transaction. The
        // persistent proof/publication gates refuse later remaps, while normal
        // Back/System input remains usable on following frames.
        force_blocked_input = force_blocked_input ||
            (!terminal_failure_at_entry && terminal_failure_now);
    }
    if (force_blocked_input) {
        call_composite_frame_with_blocked_input(
            original, composite, frame_value);
        return;
    }
    if (g_pointer_dwell.policy.active()) {
        std::uint8_t enabled{};
        const bool input_gate_read =
            guarded_read(input_enabled, enabled);
        if (should_cancel_pointer_dwell_for_composite_input(
                TabTransitionInputAction::continue_normal_path,
                input_gate_read,
                enabled != 0)) {
            reset_pointer_dwell("composite input was disabled");
        }
    }
    original(composite, frame_value, input_enabled);

    // The pre-native-composite retained-object run still blackened the
    // renderer. Recompute the menu root after the native call, then let the
    // deferred helper reacquire and validate every AS/Movie object from that
    // post-call epoch. No pointer captured before original() crosses this
    // boundary.
    void* const post_top = composite
        ? static_cast<std::byte*>(composite) - kCompositeOffset
        : nullptr;
    if (!g_mutating_viewport && !g_runtime.pending.active) {
        (void)reconcile_rgba8_retirement_debt(
            "post-native-composite");
        const bool catalogue_ready_before =
            g_rgba8_catalogue_ready.load(std::memory_order_acquire);
        run_raw_image_lifecycle_probe(post_top);
        const bool catalogue_ready_after =
            g_rgba8_catalogue_ready.load(std::memory_order_acquire);
        if (!catalogue_ready_before && catalogue_ready_after && post_top &&
            g_runtime.active && g_runtime.top == post_top &&
            !g_runtime.destructive_transition_failed) {
            // Input may have remapped the viewport while the process-wide
            // catalogue was being constructed. Rebase the epoch request to
            // the mapping that is current at the readiness boundary; later
            // Configuration epochs schedule independently during initialize.
            const bool requested =
                !g_rgba8_publication_disabled.load(
                    std::memory_order_acquire) &&
                g_runtime.catalogue_presentation_refresh.request_if_needed(
                    g_runtime.viewport,
                    kStockLogicalTabCount,
                    g_runtime.presentation_epoch,
                    frame_sequence);
            log_line(
                "RGBA8 catalogue publication rebased epoch presentation refresh=%u epoch=%llu window=%zu generation=%llu",
                requested ? 1U : 0U,
                static_cast<unsigned long long>(
                    g_runtime.presentation_epoch),
                g_runtime.viewport.window_start(),
                static_cast<unsigned long long>(
                    g_runtime.viewport.mapping_generation()));
        }
    }
}

} // namespace native_tabs_detail
