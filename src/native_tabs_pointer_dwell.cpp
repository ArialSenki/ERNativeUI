// Pointer dwell (hover-to-scroll) on the overflow tabs.
#include "native_tabs_internal.hpp"

namespace native_tabs_detail {

const char* pointer_dwell_phase_name(
    TabPointerDwellPhase phase) noexcept {
    switch (phase) {
    case TabPointerDwellPhase::idle:
        return "idle";
    case TabPointerDwellPhase::awaiting_commit:
        return "awaiting-commit";
    case TabPointerDwellPhase::cooldown:
        return "cooldown";
    case TabPointerDwellPhase::repeat_due:
        return "repeat-due";
    }
    return "unknown";
}

const char* pointer_dwell_edge_name(
    TabPointerDwellEdge edge) noexcept {
    switch (edge) {
    case TabPointerDwellEdge::previous:
        return "previous";
    case TabPointerDwellEdge::next:
        return "next";
    case TabPointerDwellEdge::none:
        break;
    }
    return "none";
}

void log_pointer_dwell(
    const char* event,
    const char* detail) noexcept {
    // r146: ask log_line's filter first so production without trace logging
    // does not spend the record budget on lines that are dropped anyway.
    if (!retain_stress_log_format("pointerDwell[")) return;
    const std::uint32_t record =
        g_pointer_dwell_log_count.fetch_add(
            1, std::memory_order_relaxed) + 1;
    if (record > kMaximumPointerDwellLogs) return;
    log_line(
        "pointerDwell[%u] event=%s phase=%s edge=%s physical=%zu selectedLogical=%zu window=%zu delay=%llu deadline=%llu repeatCommits=%llu ticket=%llu %s",
        static_cast<unsigned>(record),
        event ? event : "unknown",
        pointer_dwell_phase_name(g_pointer_dwell.policy.phase()),
        pointer_dwell_edge_name(g_pointer_dwell.policy.edge()),
        g_pointer_dwell.policy.physical_slot(),
        g_runtime.viewport.initialized()
            ? g_runtime.viewport.selected_logical()
            : invalid_tab_index,
        g_runtime.viewport.initialized()
            ? g_runtime.viewport.window_start()
            : 0,
        static_cast<unsigned long long>(
            g_pointer_dwell.policy.current_delay_ms()),
        static_cast<unsigned long long>(
            g_pointer_dwell.policy.deadline_ms()),
        static_cast<unsigned long long>(
            g_pointer_dwell.policy.automatic_repeats_committed()),
        static_cast<unsigned long long>(
            g_pointer_dwell.transaction_ticket),
        detail ? detail : "");
}

void clear_pointer_dwell_intents() noexcept {
    g_pointer_dwell.natural_intent = {};
    g_pointer_dwell.timed_intent = {};
}

void reset_pointer_dwell(
    const char* reason) noexcept {
    const bool was_active = g_pointer_dwell.policy.active() ||
        g_pointer_dwell.timed_intent.active;
    if (was_active && reason) log_pointer_dwell("cancel", reason);
    g_pointer_dwell.policy.reset();
    g_pointer_dwell.top = nullptr;
    g_pointer_dwell.direction = TabDirection::next;
    g_pointer_dwell.transaction_ticket = 0;
    clear_pointer_dwell_intents();
}

std::uint64_t next_pointer_dwell_ticket() noexcept {
    ++g_pointer_dwell.ticket_counter;
    if (g_pointer_dwell.ticket_counter == 0) {
        ++g_pointer_dwell.ticket_counter;
    }
    return g_pointer_dwell.ticket_counter;
}

TabPointerDwellEdge overflow_edge_for_physical(
    const TabViewport& viewport,
    std::size_t physical) noexcept {
    switch (tab_overflow_indicator(viewport, physical)) {
    case TabOverflowIndicator::previous:
        return TabPointerDwellEdge::previous;
    case TabOverflowIndicator::next:
        return TabPointerDwellEdge::next;
    case TabOverflowIndicator::none:
        break;
    }
    return TabPointerDwellEdge::none;
}

TabDirection direction_for_pointer_edge(
    TabPointerDwellEdge edge) noexcept {
    return edge == TabPointerDwellEdge::previous
        ? TabDirection::previous
        : TabDirection::next;
}

bool pointer_dwell_has_directional_target(
    const TabViewport& viewport,
    TabDirection direction,
    std::size_t physical,
    std::size_t* target_output = nullptr) noexcept {
    if (!viewport.initialized()) return false;
    std::size_t target{};
    if (!viewport.logical_for_physical(physical, target)) return false;
    const std::size_t selected = viewport.selected_logical();
    const bool valid = direction == TabDirection::next
        ? selected < viewport.logical_count() - 1 &&
            target == selected + 1
        : selected != 0 && target + 1 == selected;
    if (valid && target_output) *target_output = target;
    return valid;
}

bool pointer_dwell_adapter_valid() noexcept {
    return g_pointer_dwell.policy.active() &&
        g_pointer_dwell.top &&
        g_runtime.active &&
        g_runtime.top == g_pointer_dwell.top &&
        g_get_pointer_position &&
        g_grid_hit_test &&
        g_native_pointer_select;
}

void finish_pointer_dwell_transition(
    std::uint64_t ticket,
    const char* detail) noexcept {
    if (!ticket ||
        ticket != g_pointer_dwell.transaction_ticket ||
        !pointer_dwell_adapter_valid()) {
        if (g_pointer_dwell.policy.active()) {
            reset_pointer_dwell("transaction ticket mismatch");
        }
        return;
    }

    g_pointer_dwell.transaction_ticket = 0;
    const bool has_more = pointer_dwell_has_directional_target(
        g_runtime.viewport,
        g_pointer_dwell.direction,
        g_pointer_dwell.policy.physical_slot());
    g_pointer_dwell.policy.transition_committed(
        GetTickCount64(), has_more);
    if (g_pointer_dwell.policy.active()) {
        log_pointer_dwell("commit", detail);
    } else {
        log_pointer_dwell("endpoint", detail);
        g_pointer_dwell.top = nullptr;
        clear_pointer_dwell_intents();
    }
}

void fail_pointer_dwell_transition(
    std::uint64_t ticket,
    const char* reason) noexcept {
    if (ticket &&
        ticket == g_pointer_dwell.transaction_ticket) {
        g_pointer_dwell.policy.transition_failed();
        g_pointer_dwell.transaction_ticket = 0;
        log_pointer_dwell("failed", reason);
        g_pointer_dwell.top = nullptr;
        clear_pointer_dwell_intents();
    }
}

bool is_active_tab_selector(std::uintptr_t candidate) noexcept {
    if (!candidate || !g_runtime.active || !g_runtime.top) return false;
    return candidate == reinterpret_cast<std::uintptr_t>(
        static_cast<std::byte*>(g_runtime.top) + kSelectorOffset);
}

std::uint64_t elapsed_ms(
    std::uint64_t now,
    std::uint64_t then) noexcept {
    return now >= then ? now - then : 0;
}

float pointer_coordinate(std::uint32_t bits) noexcept {
    float value{};
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

void log_pointer_poll_summary(
    const PointerCellSampleTrace& trace,
    std::uint64_t now,
    std::int32_t selected) noexcept {
    // r146: same filter-before-budget order as log_pointer_dwell.
    if (!retain_stress_log_format("pointerPoll[")) return;
    const std::uint32_t record =
        g_pointer_sample_log_count.fetch_add(
            1, std::memory_order_relaxed) + 1;
    if (record > kMaximumPointerSampleLogs) return;

    log_line(
        "pointerPoll[%u] event=summary hit=%d x=%.3f y=%.3f selected=%d frames=%u polls=%u natural=%u fallback=%u unavailable=%u moves=%u hitChanges=%u comparisons=%u coordMismatch=%u hitMismatch=%u faults=%u lastSeh=0x%08lX stationaryMs=%llu",
        static_cast<unsigned>(record),
        trace.last_hit,
        static_cast<double>(pointer_coordinate(trace.last_x_bits)),
        static_cast<double>(pointer_coordinate(trace.last_y_bits)),
        selected,
        static_cast<unsigned>(trace.dispatch_frames),
        static_cast<unsigned>(trace.successful_polls),
        static_cast<unsigned>(trace.natural_frames),
        static_cast<unsigned>(trace.fallback_frames),
        static_cast<unsigned>(trace.unavailable_frames),
        static_cast<unsigned>(trace.coordinate_changes),
        static_cast<unsigned>(trace.hit_changes),
        static_cast<unsigned>(trace.comparison_frames),
        static_cast<unsigned>(trace.coordinate_mismatches),
        static_cast<unsigned>(trace.hit_mismatches),
        static_cast<unsigned>(trace.poll_faults),
        trace.last_poll_seh,
        static_cast<unsigned long long>(
            trace.initialized
                ? elapsed_ms(now, trace.last_position_change_ms)
                : 0));
}

void reset_pointer_poll_interval(
    PointerCellSampleTrace& trace,
    std::uint64_t now) noexcept {
    trace.last_report_ms = now;
    trace.dispatch_frames = 0;
    trace.successful_polls = 0;
    trace.natural_frames = 0;
    trace.fallback_frames = 0;
    trace.unavailable_frames = 0;
    trace.coordinate_changes = 0;
    trace.hit_changes = 0;
    trace.comparison_frames = 0;
    trace.coordinate_mismatches = 0;
    trace.hit_mismatches = 0;
    trace.poll_faults = 0;
    trace.last_poll_seh = 0;
}

bool poll_native_pointer(
    void* selector,
    const std::uint8_t* input_gate,
    NativePointerSample& sample) noexcept {
    sample = {};
    if (!selector || !input_gate || !g_get_pointer_position ||
        !g_grid_hit_test) {
        return false;
    }

    std::uint8_t input_enabled{};
    if (!guarded_read(input_gate, input_enabled) || input_enabled == 0) {
        return false;
    }

    PointerPosition position{};
    PointerPosition returned_position{};
    bool completed{};
#if defined(_MSC_VER)
    __try {
#endif
        PointerPosition* const returned =
            g_get_pointer_position(input_gate, &position);
        if (returned == &position) {
            returned_position = position;
            sample.hit = g_grid_hit_test(selector, &position);
            completed = true;
        }
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        sample.seh_code = static_cast<unsigned long>(GetExceptionCode());
    }
#endif

    if (!completed) return false;
    std::memcpy(&sample.x_bits, &returned_position.x, sizeof(sample.x_bits));
    std::memcpy(&sample.y_bits, &returned_position.y, sizeof(sample.y_bits));
    return true;
}

void observe_pointer_cell_sample(
    safetyhook::Context& context) noexcept {
    if (!g_viewport_callbacks_armed.load(std::memory_order_acquire)) return;
    if (!is_active_tab_selector(context.rsi)) return;

    std::uint32_t x_bits{};
    std::uint32_t y_bits{};
    if (!guarded_read(
            reinterpret_cast<const void*>(context.rsp + 0x20),
            x_bits) ||
        !guarded_read(
            reinterpret_cast<const void*>(context.rsp + 0x24),
            y_bits)) {
        ++g_pointer_cell_sample_trace.poll_faults;
        return;
    }

    PointerCellSampleTrace& trace = g_pointer_cell_sample_trace;
    trace.seen_this_dispatch = true;
    trace.natural_hit = static_cast<std::int32_t>(
        static_cast<std::uint32_t>(context.rax));
    trace.natural_x_bits = x_bits;
    trace.natural_y_bits = y_bits;

    const std::int32_t natural_hit = trace.natural_hit;
    const std::uint64_t now = GetTickCount64();
    if (g_pointer_dwell.policy.active()) {
        if (!pointer_dwell_adapter_valid() ||
            !pointer_dwell_has_directional_target(
                g_runtime.viewport,
                g_pointer_dwell.direction,
                g_pointer_dwell.policy.physical_slot())) {
            reset_pointer_dwell("edge no longer has a directional target");
            return;
        }

        const auto update = g_pointer_dwell.policy.observe_natural(
            natural_hit, x_bits, y_bits, now);
        if (update.cancelled) {
            g_pointer_dwell.top = nullptr;
            g_pointer_dwell.transaction_ticket = 0;
            clear_pointer_dwell_intents();
            log_pointer_dwell("leave", "natural hit left the armed edge");
            // Continue through the idle classification below. If the pointer
            // moved directly onto the opposite ellipsis, this same native
            // hover remains immediate and starts a fresh dwell transaction.
        } else {
            if (update.deadline_restarted) {
                log_pointer_dwell("motion", "natural sample restarted delay");
            }
            if (update.suppress_natural_selection) {
                // At the pointer-cell seam, EAX == -1 takes the native
                // no-hit branch and
                // returns false from pointer processing. That both prevents a
                // repaint-triggered selection and leaves L1/R1 free to run
                // later in this same input frame.
                context.rax =
                    (context.rax & ~std::uintptr_t{0xFFFFFFFFu}) |
                    std::uintptr_t{0xFFFFFFFFu};
            }
            return;
        }
    }

    g_pointer_dwell.natural_intent = {};
    if (g_mutating_viewport || g_runtime.pending.active ||
        !g_get_pointer_position || !g_grid_hit_test ||
        !g_native_pointer_select || natural_hit < 0) {
        if (g_runtime.pending.active && natural_hit >= 0) {
            context.rax =
                (context.rax & ~std::uintptr_t{0xFFFFFFFFu}) |
                std::uintptr_t{0xFFFFFFFFu};
        }
        return;
    }

    const std::size_t physical = static_cast<std::size_t>(natural_hit);
    const TabPointerDwellEdge edge = overflow_edge_for_physical(
        g_runtime.viewport, physical);
    if (edge == TabPointerDwellEdge::none) return;
    g_pointer_dwell.natural_intent = {
        true,
        g_runtime.top,
        edge,
        static_cast<std::uint32_t>(physical),
        x_bits,
        y_bits,
    };
}

void observe_native_pointer_poll(
    safetyhook::Context& context) noexcept {
    // r146: with no armed dwell the poll only feeds the pointerPoll summary
    // (dwell is armed from observe_pointer_cell_sample's natural intent, and
    // the trace fields have no other reader). Skip the per-frame native
    // pointer and grid hit-test calls when that summary would be dropped.
    if (!g_pointer_dwell.policy.active() &&
        !retain_stress_log_format("pointerPoll[")) {
        return;
    }
    PointerCellSampleTrace& trace = g_pointer_cell_sample_trace;
    ++trace.dispatch_frames;
    if (trace.seen_this_dispatch) ++trace.natural_frames;

    const std::uint64_t now = GetTickCount64();

    NativePointerSample sample{};
    const bool runtime_safe = !g_mutating_viewport &&
        !g_runtime.pending.active;
    const bool available = runtime_safe && poll_native_pointer(
        reinterpret_cast<void*>(context.rdi),
        reinterpret_cast<const std::uint8_t*>(context.rsi),
        sample);
    if (!available) {
        ++trace.unavailable_frames;
        if (sample.seh_code != 0) {
            ++trace.poll_faults;
            trace.last_poll_seh = sample.seh_code;
        }
    } else {
        ++trace.successful_polls;
        if (trace.seen_this_dispatch) {
            ++trace.comparison_frames;
            if (trace.natural_x_bits != sample.x_bits ||
                trace.natural_y_bits != sample.y_bits) {
                ++trace.coordinate_mismatches;
            }
            if (trace.natural_hit != sample.hit) {
                ++trace.hit_mismatches;
            }
        } else {
            ++trace.fallback_frames;
        }

        if (!trace.initialized) {
            trace.initialized = true;
            trace.last_hit = sample.hit;
            trace.last_x_bits = sample.x_bits;
            trace.last_y_bits = sample.y_bits;
            trace.last_position_change_ms = now;
            trace.last_report_ms = now;
        } else {
            const bool coordinates_changed =
                trace.last_x_bits != sample.x_bits ||
                trace.last_y_bits != sample.y_bits;
            if (coordinates_changed) {
                ++trace.coordinate_changes;
                trace.last_position_change_ms = now;
            }
            if (trace.last_hit != sample.hit) ++trace.hit_changes;
            trace.last_hit = sample.hit;
            trace.last_x_bits = sample.x_bits;
            trace.last_y_bits = sample.y_bits;
        }
    }

    if (g_pointer_dwell.policy.active()) {
        if (!pointer_dwell_adapter_valid()) {
            reset_pointer_dwell("runtime identity changed");
        } else if (available) {
            if (!pointer_dwell_has_directional_target(
                    g_runtime.viewport,
                    g_pointer_dwell.direction,
                    g_pointer_dwell.policy.physical_slot())) {
                reset_pointer_dwell(
                    "edge no longer has a directional target");
            } else {
                const TabPointerDwellPhase previous_phase =
                    g_pointer_dwell.policy.phase();
                const auto update = g_pointer_dwell.policy.observe_poll(
                    sample.hit,
                    sample.x_bits,
                    sample.y_bits,
                    now);
                if (update.cancelled) {
                    g_pointer_dwell.top = nullptr;
                    g_pointer_dwell.transaction_ticket = 0;
                    clear_pointer_dwell_intents();
                    log_pointer_dwell(
                        "leave", "native poll left the armed edge");
                } else {
                    if (update.deadline_restarted) {
                        log_pointer_dwell(
                            "motion", "native poll restarted delay");
                    }
                    if (previous_phase != TabPointerDwellPhase::repeat_due &&
                        update.repeat_due) {
                        log_pointer_dwell(
                            "due", "stationary delay elapsed");
                    }
                }
            }
        } else if (!g_blocking_composite_input &&
            !g_runtime.pending.active && !g_mutating_viewport) {
            reset_pointer_dwell("native pointer poll became unavailable");
        }
    }

    if (trace.last_report_ms == 0) trace.last_report_ms = now;
    if (elapsed_ms(now, trace.last_report_ms) >= kPointerPollReportMs) {
        std::uint32_t selected{};
        const bool selected_read = guarded_read(
            reinterpret_cast<const void*>(
                context.rdi + kSelectorSelectedIndexOffset),
            selected);
        log_pointer_poll_summary(
            trace,
            now,
            selected_read ? static_cast<std::int32_t>(selected) : -1);
        reset_pointer_poll_interval(trace, now);
    }
}

void observe_input_dispatch_frame_begin(
    safetyhook::Context& context) noexcept {
    if (!g_viewport_callbacks_armed.load(std::memory_order_acquire)) return;
    if (is_active_tab_selector(context.rdi)) {
        // A timed request is published immediately before A394. The ordinary
        // native change path must consume it in this same dispatch. Reaching
        // another frame with the marker intact means no matching callback was
        // delivered; fail closed instead of suppressing the edge forever.
        if (g_pointer_dwell.timed_intent.active) {
            reset_pointer_dwell(
                "timed selection produced no callback before next dispatch");
        }
        g_tab_selection_source = TabSelectionSource::unknown;
        g_pointer_cell_sample_trace.seen_this_dispatch = false;
        g_pointer_dwell.natural_intent = {};
    }
}

void observe_pointer_dispatch_result(
    safetyhook::Context& context) noexcept {
    if (!g_viewport_callbacks_armed.load(std::memory_order_acquire)) return;
    if (!is_active_tab_selector(context.rdi)) return;

    // Reuse Elden Ring's own cursor conversion and GridControl hit test at a
    // stable post-dispatch seam. This remains observation-only: the returned
    // hit is compared with B880 when that natural path ran, otherwise it only
    // proves that stationary per-frame samples are available.
    observe_native_pointer_poll(context);

    // This seam runs immediately after the pointer helper. Do not overwrite a
    // stronger marker if another validated path was observed in this frame.
    // Otherwise a changed selection returned by the helper is the pointer
    // path. R12D is the selected index saved at this frame's entry seam.
    std::uint32_t selected{};
    const bool selection_changed =
        (context.rax & 0xFFu) != 0u &&
        guarded_read(
            reinterpret_cast<const void*>(
                context.rdi + kSelectorSelectedIndexOffset),
            selected) &&
        selected != static_cast<std::uint32_t>(context.r12);
    if (selection_changed &&
        g_tab_selection_source == TabSelectionSource::unknown) {
        g_tab_selection_source = TabSelectionSource::pointer;
    }
}

void observe_horizontal_navigation_commit(
    safetyhook::Context& context) noexcept {
    if (!g_viewport_callbacks_armed.load(std::memory_order_acquire)) return;
    if (!is_active_tab_selector(context.rdi)) return;

    const auto delta_x = static_cast<std::int32_t>(
        static_cast<std::uint32_t>(context.rbx));
    const auto delta_y = static_cast<std::int32_t>(
        static_cast<std::uint32_t>(context.rbx >> 32u));
    if (delta_y != 0 || (delta_x != -1 && delta_x != 1)) return;

    if (g_pointer_dwell.policy.active()) {
        log_pointer_dwell(
            "controller", "native horizontal navigation took priority");
        g_pointer_dwell.policy.controller_navigation();
        g_pointer_dwell.top = nullptr;
        g_pointer_dwell.transaction_ticket = 0;
        clear_pointer_dwell_intents();
    }

    g_tab_selection_source = delta_x < 0
        ? TabSelectionSource::navigation_previous
        : TabSelectionSource::navigation_next;
}

bool call_native_pointer_select(
    void* selector,
    std::uint32_t physical) noexcept {
    if (!selector || !g_native_pointer_select ||
        physical >= active_physical_tab_count()) {
        return false;
    }
    bool completed{};
#if defined(_MSC_VER)
    __try {
#endif
        static_cast<void>(g_native_pointer_select(selector, physical));
        completed = true;
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        log_failure(
            "timed pointer cell selection faulted (seh=0x%08lX)",
            static_cast<unsigned long>(GetExceptionCode()));
    }
#endif
    return completed;
}

void dispatch_due_pointer_dwell(
    safetyhook::Context& context) noexcept {
    if (!g_viewport_callbacks_armed.load(std::memory_order_acquire)) return;
    if (!is_active_tab_selector(context.rdi) ||
        !g_pointer_dwell.policy.repeat_due()) {
        return;
    }

    if (g_tab_selection_source ==
            TabSelectionSource::navigation_previous ||
        g_tab_selection_source == TabSelectionSource::navigation_next) {
        reset_pointer_dwell("controller navigation reached dispatch");
        return;
    }
    if (g_tab_selection_source != TabSelectionSource::unknown ||
        !pointer_dwell_adapter_valid() || g_mutating_viewport ||
        g_runtime.pending.active ||
        !records_match_mapping(g_runtime.top, g_runtime.viewport)) {
        reset_pointer_dwell("timed dispatch safety gate failed");
        return;
    }

    std::uint32_t selected{};
    if (!guarded_read(
            reinterpret_cast<const void*>(
                context.rdi + kSelectorSelectedIndexOffset),
            selected) ||
        selected != static_cast<std::uint32_t>(context.r12)) {
        reset_pointer_dwell("another native input changed selection");
        return;
    }

    const std::size_t edge_physical =
        g_pointer_dwell.policy.physical_slot();
    std::size_t target_logical{invalid_tab_index};
    if (edge_physical >= active_physical_tab_count() ||
        !pointer_dwell_has_directional_target(
            g_runtime.viewport,
            g_pointer_dwell.direction,
            edge_physical,
            &target_logical)) {
        reset_pointer_dwell("timed edge reached catalogue endpoint");
        return;
    }

    if (!call_native_pointer_select(
            reinterpret_cast<void*>(context.rdi),
            static_cast<std::uint32_t>(edge_physical))) {
        reset_pointer_dwell("native timed selection call failed");
        return;
    }

    std::uint32_t updated{};
    if (!guarded_read(
            reinterpret_cast<const void*>(
                context.rdi + kSelectorSelectedIndexOffset),
            updated) ||
        updated != edge_physical || updated == selected) {
        reset_pointer_dwell("native timed selection was rejected");
        return;
    }

    g_pointer_dwell.policy.repeat_dispatched();
    g_pointer_dwell.timed_intent = {
        true,
        g_runtime.top,
        updated,
        target_logical,
    };
    g_tab_selection_source = TabSelectionSource::pointer;
    log_pointer_dwell(
        "dispatch", "native cell setter accepted timed repeat");
}

} // namespace native_tabs_detail
