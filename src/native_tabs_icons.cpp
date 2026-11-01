// Vector and RGBA8 icon shape attachment and the image-bind detour.
#include "native_tabs_internal.hpp"

namespace native_tabs_detail {

Rgba8ShapeAttachProbeResult probe_dynamic_rgba8_shape_attachment(
    std::uint32_t attempt,
    void* target_value,
    void* movie_root,
    void* create_object_slot,
    ImageBindFn image_binder,
    void** resource,
    const std::array<float, 4>& rectangle,
    bool require_reusable_shape,
    bool skip_same_asset_bind,
    bool clear_before_bind) noexcept {
    Rgba8ShapeAttachProbeResult result{};
    const std::uint8_t property_bank = repaint_icon_property_bank();
    const char* const injected_property =
        injected_icon_property(property_bank);
    const char* const authored_property =
        authored_icon_property(property_bank);
    // Only the fixed, forward-proven Graphics canvas is used for custom RGBA
    // content; retain the authored rectangle parameter for call-site symmetry.
    (void)rectangle;

    void* resource_value{};
    const bool resource_read = guarded_read(resource, resource_value) &&
        resource_value;
    if (resource_read) {
        result.reference_before_read = guarded_read(
            static_cast<const std::byte*>(resource_value) + 0x08,
            result.reference_before);
    }
    const bool exact_incoming_reference =
        result.reference_before_read && result.reference_before == 3U;

    void* const arena_storage = HeapAlloc(
        GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(Rgba8ShapeWrapperArena));
    auto* const wrappers = arena_storage
        ? ::new (arena_storage) Rgba8ShapeWrapperArena{}
        : nullptr;
    if (!wrappers) {
        // Without stable wrapper storage there is no safe Shape probe. Balance
        // only a positively identified binder-local reference; on any other
        // count, leave ownership untouched and fail closed.
        if (resource_read && exact_incoming_reference) {
            result.resource_consumed =
                guarded_release_scaleform_image_resource(
                    reinterpret_cast<ScaleformImageResourceReleaseFn>(
                        g_game.base +
                        kScaleformImageResourceRelease271Rva),
                    resource_value,
                    result.consume_seh);
            result.reference_after_read = guarded_read(
                static_cast<const std::byte*>(resource_value) + 0x08,
                result.reference_after);
            result.exact_reference_transition =
                result.resource_consumed && result.reference_after_read &&
                result.reference_after == 2U;
        }
        log_failure(
            "rgba8Shape[%u] wrapper arena allocation failed; consumed=%u seh=0x%08lX refs=%u/%u->%u/%u exact=%u",
            static_cast<unsigned>(attempt),
            result.resource_consumed ? 1U : 0U,
            result.consume_seh,
            result.reference_before_read ? 1U : 0U,
            static_cast<unsigned>(result.reference_before),
            result.reference_after_read ? 1U : 0U,
            static_cast<unsigned>(result.reference_after),
            result.exact_reference_transition ? 1U : 0U);
        return result;
    }

    // Resolve the same ActionScript parent that owns the already-proven vector
    // overlay. The authored timeline Shape cannot accept an image fill on this
    // movie (its native drawing-context slot is null), so the RGBA resource is
    // never forwarded to that object.
    auto& parent_wrapper = wrappers->parent.value;
    const char* parent_name = "parent";
    bool parent_constructed{};
    unsigned long parent_seh{};
    if (target_value && g_get_scaleform_member) {
#if defined(_MSC_VER)
        __try {
#endif
            parent_constructed = g_get_scaleform_member(
                target_value, parent_wrapper.data(), &parent_name) ==
                parent_wrapper.data();
#if defined(_MSC_VER)
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            parent_seh = static_cast<unsigned long>(GetExceptionCode());
        }
#endif
    }

    DisplayValueView parent_view{};
    DisplayValueView target_view{};
    const bool exact_parent = parent_seh == 0 && parent_constructed &&
        inspect_display_value(parent_wrapper.data(), parent_view) &&
        exact_as3_display_bridge(parent_view);
    const bool exact_target = inspect_display_value(target_value, target_view) &&
        exact_as3_display_bridge(target_view) &&
        validated_authored_icon_target_vtable(
            target_view.native_display_vtable);

    auto& existing_wrapper = wrappers->existing.value;
    bool existing_constructed{};
    unsigned long existing_get_seh{};
    if (exact_parent && g_get_scaleform_member) {
        const char* property_name = injected_property;
#if defined(_MSC_VER)
        __try {
#endif
            existing_constructed = g_get_scaleform_member(
                parent_wrapper.data(), existing_wrapper.data(),
                &property_name) == existing_wrapper.data();
#if defined(_MSC_VER)
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            existing_get_seh =
                static_cast<unsigned long>(GetExceptionCode());
        }
#endif
    }

    bool tracked_cell{};
    if (g_repaint_context.active &&
        g_repaint_context.top == g_runtime.top &&
        g_repaint_context.physical < active_physical_tab_count() &&
        g_repaint_context.layer != TabLayer::none) {
        const std::size_t bit_index = g_repaint_context.physical +
            (g_repaint_context.layer == TabLayer::back
                ? active_physical_tab_count()
                : 0);
        const AtomicTabHostMask& host_ledger =
            repaint_stages_fresh_host(bit_index)
            ? g_staged_injected_icon_hosts
            : g_injected_icon_hosts;
        tracked_cell = (host_ledger.load(std::memory_order_acquire) &
            (TabHostMask{1} << bit_index)) != 0;
    }
    DisplayValueView existing_view{};
    const bool reusable_shape = existing_get_seh == 0 && tracked_cell &&
        existing_constructed &&
        inspect_display_value(existing_wrapper.data(), existing_view) &&
        exact_as3_display_bridge(existing_view) &&
        existing_view.native_display_vtable ==
            g_game.base + kDynamicShapeVtable271Rva;

    auto& created_wrapper = wrappers->created.value;
    bool created_constructed{};
    void* constructor_result{};
    unsigned long constructor_seh{};
    if (exact_parent && exact_target && !reusable_shape &&
        !require_reusable_shape &&
        g_construct_scaleform_value) {
#if defined(_MSC_VER)
        __try {
#endif
            constructor_result =
                g_construct_scaleform_value(created_wrapper.data());
            created_constructed = true;
#if defined(_MSC_VER)
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            constructor_seh =
                static_cast<unsigned long>(GetExceptionCode());
        }
#endif
    }

    unsigned long create_seh{};
    if (created_constructed && movie_root && create_object_slot) {
#if defined(_MSC_VER)
        __try {
#endif
            reinterpret_cast<MovieCreateObjectFn>(create_object_slot)(
                movie_root,
                created_wrapper.data() + 0x08,
                "flash.display.Shape",
                nullptr,
                0);
#if defined(_MSC_VER)
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            create_seh = static_cast<unsigned long>(GetExceptionCode());
        }
#endif
    }

    DisplayValueView created_view{};
    const bool created_shape = created_constructed && create_seh == 0 &&
        constructor_result == created_wrapper.data() &&
        inspect_display_value(created_wrapper.data(), created_view) &&
        exact_as3_display_bridge(created_view) &&
        created_view.native_display_vtable ==
            g_game.base + kDynamicShapeVtable271Rva;
    void* const shape_wrapper = reusable_shape
        ? static_cast<void*>(existing_wrapper.data())
        : static_cast<void*>(created_wrapper.data());
    const DisplayValueView& shape_view = reusable_shape
        ? existing_view
        : created_view;
    const bool exact_dynamic_shape = reusable_shape || created_shape;
    result.reused_existing_shape = reusable_shape;
    result.created_new_shape = created_shape;
    result.pre_bind_graphics_clear_required = clear_before_bind;

    // Scaleform's image binder succeeds when called a second time on the same
    // Shape, but runtime proof showed that its cached drawing command can then
    // render as an untextured white quad. Clear the existing Graphics command
    // list before the one allowed asset-0 -> asset-1 rebind. Graphics.clear()
    // is already proven by the detach path and also releases the old RawImage
    // fill synchronously when the renderer permits it.
    auto& graphics_wrapper = wrappers->graphics.value;
    bool graphics_constructed{};
    unsigned long graphics_get_seh{};
    ObjectValueView graphics_view{};
    bool exact_graphics{};
    Rgba8ReferenceSnapshot pre_clear_old_refs{};
    Rgba8ReferenceSnapshot post_clear_old_refs{};
    if (result.pre_bind_graphics_clear_required && reusable_shape &&
        g_get_scaleform_member) {
        const char* graphics_name = "graphics";
#if defined(_MSC_VER)
        __try {
#endif
            graphics_constructed = g_get_scaleform_member(
                shape_wrapper,
                graphics_wrapper.data(),
                &graphics_name) == graphics_wrapper.data();
#if defined(_MSC_VER)
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            graphics_get_seh =
                static_cast<unsigned long>(GetExceptionCode());
        }
#endif
        exact_graphics = graphics_get_seh == 0 && graphics_constructed &&
            inspect_object_value(graphics_wrapper.data(), graphics_view) &&
            exact_as3_object_bridge(graphics_view);
        if (exact_graphics) {
            pre_clear_old_refs = snapshot_rgba8_references(0);
            result.pre_bind_old_raw_before_read =
                pre_clear_old_refs.raw_read;
            result.pre_bind_old_raw_before = pre_clear_old_refs.raw;
            result.pre_bind_graphics_clear_attempted = true;
            result.pre_bind_graphics_cleared = invoke_object_method(
                graphics_view,
                "clear",
                nullptr,
                0,
                result.graphics_clear_seh);
            post_clear_old_refs = snapshot_rgba8_references(0);
            result.pre_bind_old_raw_after_read = post_clear_old_refs.raw_read;
            result.pre_bind_old_raw_after = post_clear_old_refs.raw;
            result.pre_bind_old_raw_released_exact =
                pre_clear_old_refs.resource_read &&
                pre_clear_old_refs.resource == 1U &&
                pre_clear_old_refs.raw_read &&
                post_clear_old_refs.resource_read &&
                post_clear_old_refs.resource == 1U &&
                post_clear_old_refs.raw_read &&
                pre_clear_old_refs.raw > 0U &&
                post_clear_old_refs.raw == pre_clear_old_refs.raw - 1U;
        }
    }
    const bool pre_bind_graphics_ready =
        !result.pre_bind_graphics_clear_required ||
        (result.pre_bind_graphics_clear_attempted &&
            result.pre_bind_graphics_cleared &&
            result.graphics_clear_seh == 0 &&
            result.pre_bind_old_raw_released_exact);
    if (attempt <= kMaximumShapeAttachLogs &&
        result.pre_bind_graphics_clear_required) {
        log_line(
            "rgba8Shape[%u] preBindClear={required:1 graphicsConstructed:%u graphicsExact:%u attempted:%u cleared:%u seh:0x%08lX oldResource:%u/%u->%u/%u oldRaw:%u/%u->%u/%u releasedExact:%u binderAllowed:%u}",
            static_cast<unsigned>(attempt),
            graphics_constructed ? 1U : 0U,
            exact_graphics ? 1U : 0U,
            result.pre_bind_graphics_clear_attempted ? 1U : 0U,
            result.pre_bind_graphics_cleared ? 1U : 0U,
            result.graphics_clear_seh,
            pre_clear_old_refs.resource_read ? 1U : 0U,
            static_cast<unsigned>(pre_clear_old_refs.resource),
            post_clear_old_refs.resource_read ? 1U : 0U,
            static_cast<unsigned>(post_clear_old_refs.resource),
            result.pre_bind_old_raw_before_read ? 1U : 0U,
            static_cast<unsigned>(result.pre_bind_old_raw_before),
            result.pre_bind_old_raw_after_read ? 1U : 0U,
            static_cast<unsigned>(result.pre_bind_old_raw_after),
            result.pre_bind_old_raw_released_exact ? 1U : 0U,
            pre_bind_graphics_ready ? 1U : 0U);
    }
    constexpr std::array<float, 4> kFixedCanvasRectangle{
        static_cast<float>(kCrossCanvasLeft),
        static_cast<float>(kCrossCanvasTop),
        static_cast<float>(kCrossCanvasLeft + kCrossCanvasWidth),
        static_cast<float>(kCrossCanvasTop + kCrossCanvasHeight),
    };

    // The presenter owns one binder-local reference in addition to the lookup
    // reference. Consume that reference exactly once on every branch. The
    // normal path gives it to the unchanged image binder on a dynamic Shape;
    // a pre-bind validation failure balances it directly with Release. Never
    // retry the resource on the authored target after either operation.
    bool release_completed{};
    if (skip_same_asset_bind && reusable_shape && resource_read &&
        exact_incoming_reference) {
        release_completed = guarded_release_scaleform_image_resource(
            reinterpret_cast<ScaleformImageResourceReleaseFn>(
                g_game.base + kScaleformImageResourceRelease271Rva),
            resource_value,
            result.consume_seh);
        result.resource_consumed = release_completed;
        result.same_asset_bind_skipped = release_completed;
    } else if (exact_dynamic_shape && pre_bind_graphics_ready && image_binder &&
        resource_read && exact_incoming_reference) {
        result.resource_consumed = true;
        result.raster_bind_attempted = true;
#if defined(_MSC_VER)
        __try {
#endif
            result.raster_bound = image_binder(
                shape_wrapper, resource, kFixedCanvasRectangle.data());
#if defined(_MSC_VER)
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            result.consume_seh =
                static_cast<unsigned long>(GetExceptionCode());
            result.raster_bound = false;
        }
#endif
    } else if (resource_read && exact_incoming_reference) {
        release_completed = guarded_release_scaleform_image_resource(
            reinterpret_cast<ScaleformImageResourceReleaseFn>(
                g_game.base + kScaleformImageResourceRelease271Rva),
            resource_value,
            result.consume_seh);
        result.resource_consumed = release_completed;
    }

    if (resource_read) {
        result.reference_after_read = guarded_read(
            static_cast<const std::byte*>(resource_value) + 0x08,
            result.reference_after);
    }
    result.exact_reference_transition =
        result.reference_before_read && result.reference_after_read &&
        result.reference_before == 3U && result.reference_after == 2U;
    const bool raster_ready =
        ((result.raster_bind_attempted && result.raster_bound) ||
            result.same_asset_bind_skipped) &&
        result.consume_seh == 0 && result.exact_reference_transition;

    // A false return from the dynamic-Shape binder still has a proven 3 -> 2
    // ownership transition. In that narrow case, paint the known-good vector
    // cross into this same Shape so the active front/back layer never flashes
    // the unrelated Graphics icon while publication is disabled for later
    // repaints. No second bind or resource operation is performed.
    const bool can_paint_fallback = result.raster_bind_attempted &&
        !result.raster_bound && result.consume_seh == 0 &&
        result.exact_reference_transition && exact_dynamic_shape;
    if (can_paint_fallback && !graphics_constructed &&
        g_get_scaleform_member) {
        const char* graphics_name = "graphics";
#if defined(_MSC_VER)
        __try {
#endif
            graphics_constructed = g_get_scaleform_member(
                shape_wrapper,
                graphics_wrapper.data(),
                &graphics_name) == graphics_wrapper.data();
#if defined(_MSC_VER)
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            graphics_get_seh =
                static_cast<unsigned long>(GetExceptionCode());
        }
#endif
    }
    if (can_paint_fallback && !exact_graphics) {
        exact_graphics = graphics_get_seh == 0 && graphics_constructed &&
            inspect_object_value(graphics_wrapper.data(), graphics_view) &&
            exact_as3_object_bridge(graphics_view);
    }
    constexpr double local_width = kCrossCanvasWidth;
    constexpr double local_height = kCrossCanvasHeight;
    constexpr bool sane_extents = local_width > 0.0 &&
        local_width <= 256.0 && local_height > 0.0 &&
        local_height <= 256.0;
    unsigned long paint_seh{};
    std::uint32_t painted_rectangles{};
    if (exact_graphics && sane_extents && can_paint_fallback) {
        if (draw_graphics_rectangle(
                graphics_view,
                0x42E8F5U,
                0.92,
                local_width * 0.42,
                local_height * 0.08,
                local_width * 0.16,
                local_height * 0.84,
                paint_seh)) {
            ++painted_rectangles;
        }
        if (painted_rectangles == 1 && draw_graphics_rectangle(
                graphics_view,
                0xFFC857U,
                0.92,
                local_width * 0.12,
                local_height * 0.42,
                local_width * 0.76,
                local_height * 0.16,
                paint_seh)) {
            ++painted_rectangles;
        }
        if (painted_rectangles == 2 && draw_graphics_rectangle(
                graphics_view,
                0xD95CFFU,
                0.96,
                local_width * 0.36,
                local_height * 0.36,
                local_width * 0.28,
                local_height * 0.28,
                paint_seh)) {
            ++painted_rectangles;
        }
    }
    const bool vector_fallback_painted =
        painted_rectangles == 3 && paint_seh == 0;
    const bool drawable_shape = exact_dynamic_shape &&
        (raster_ready || vector_fallback_painted);

    bool icon_property_set{};
    bool authored_property_set{};
    unsigned long property_set_seh{};
    if (exact_parent && exact_target && drawable_shape) {
#if defined(_MSC_VER)
        __try {
#endif
            const auto set_member = reinterpret_cast<ValueSetMemberFn>(
                parent_view.set_member_slot);
            icon_property_set = set_member(
                parent_view.object_interface,
                parent_view.object_payload,
                injected_property,
                static_cast<const std::byte*>(shape_wrapper) + 0x08,
                true);
            authored_property_set = icon_property_set && set_member(
                parent_view.object_interface,
                parent_view.object_payload,
                authored_property,
                static_cast<const std::byte*>(target_value) + 0x08,
                true);
#if defined(_MSC_VER)
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            property_set_seh =
                static_cast<unsigned long>(GetExceptionCode());
        }
#endif
    }

    auto& verified_icon_wrapper = wrappers->verified_icon.value;
    auto& verified_authored_wrapper = wrappers->verified_authored.value;
    bool verified_icon_constructed{};
    bool verified_authored_constructed{};
    unsigned long property_verify_seh{};
    if (icon_property_set && authored_property_set &&
        g_get_scaleform_member) {
        const char* icon_property = injected_property;
#if defined(_MSC_VER)
        __try {
#endif
            verified_icon_constructed = g_get_scaleform_member(
                parent_wrapper.data(),
                verified_icon_wrapper.data(),
                &icon_property) == verified_icon_wrapper.data();
            verified_authored_constructed = g_get_scaleform_member(
                parent_wrapper.data(),
                verified_authored_wrapper.data(),
                &authored_property) == verified_authored_wrapper.data();
#if defined(_MSC_VER)
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            property_verify_seh =
                static_cast<unsigned long>(GetExceptionCode());
        }
#endif
    }
    DisplayValueView verified_icon_view{};
    DisplayValueView verified_authored_view{};
    const bool properties_verified = property_verify_seh == 0 &&
        verified_icon_constructed && verified_authored_constructed &&
        inspect_display_value(
            verified_icon_wrapper.data(), verified_icon_view) &&
        inspect_display_value(
            verified_authored_wrapper.data(), verified_authored_view) &&
        exact_as3_display_bridge(verified_icon_view) &&
        exact_as3_display_bridge(verified_authored_view) &&
        verified_icon_view.object_payload == shape_view.object_payload &&
        verified_icon_view.native_display_vtable ==
            g_game.base + kDynamicShapeVtable271Rva &&
        verified_authored_view.object_payload == target_view.object_payload;

    // The native binder emits a zero-based image quad and derives only scale
    // from the supplied rectangle's width/height. Both raster and immediate
    // vector fallback therefore use the fixed, forward-proven Graphics canvas
    // and its matching DisplayInfo translation. This deliberately ignores the
    // remapped authored icon's varying reverse-wrap bounds.
    constexpr float position_x = static_cast<float>(kCrossCanvasLeft);
    constexpr float position_y = static_cast<float>(kCrossCanvasTop);
    void* position_result{};
    unsigned long position_seh{};
    if (properties_verified && drawable_shape && g_set_scaleform_position) {
#if defined(_MSC_VER)
        __try {
#endif
            position_result = g_set_scaleform_position(
                shape_wrapper, position_x, position_y);
#if defined(_MSC_VER)
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            position_seh = static_cast<unsigned long>(GetExceptionCode());
        }
#endif
    }
    const bool positioned = position_seh == 0 &&
        position_result == shape_wrapper;

    bool shape_visible{};
    unsigned long visible_seh{};
    if (positioned && g_set_scaleform_visible) {
#if defined(_MSC_VER)
        __try {
#endif
            shape_visible = g_set_scaleform_visible(shape_wrapper, 1) ==
                shape_wrapper;
#if defined(_MSC_VER)
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            visible_seh = static_cast<unsigned long>(GetExceptionCode());
        }
#endif
    }

    bool add_child_attempted{};
    bool attached{};
    unsigned long attach_seh{};
    if (shape_visible) {
#if defined(_MSC_VER)
        __try {
#endif
            add_child_attempted = true;
            attached = reinterpret_cast<ValueInvokeFn>(
                parent_view.invoke_slot)(
                parent_view.object_interface,
                parent_view.object_payload,
                nullptr,
                "addChild",
                static_cast<const std::byte*>(shape_wrapper) + 0x08,
                1,
                true);
#if defined(_MSC_VER)
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            attach_seh = static_cast<unsigned long>(GetExceptionCode());
        }
#endif
    }

    bool authored_hidden{};
    unsigned long hide_seh{};
    if (attached && g_set_scaleform_visible) {
#if defined(_MSC_VER)
        __try {
#endif
            authored_hidden = g_set_scaleform_visible(target_value, 0) ==
                target_value;
#if defined(_MSC_VER)
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            hide_seh = static_cast<unsigned long>(GetExceptionCode());
        }
#endif
    }
    result.custom_visible = drawable_shape && positioned && shape_visible &&
        attached && authored_hidden;
    result.vector_fallback_visible =
        vector_fallback_painted && result.custom_visible;
    result.bound = result.custom_visible;
    const bool attachment_uncertain =
        attach_seh != 0 && add_child_attempted;

    bool rollback_detached{};
    bool rollback_hidden{};
    bool rollback_visible{};
    bool properties_cleared{};
    bool rollback_retained{};
    unsigned long rollback_seh{};
    auto& undefined_wrapper = wrappers->undefined.value;
    bool undefined_constructed{};
    if (!result.custom_visible &&
        (icon_property_set || authored_property_set)) {
        // A faulting Invoke may have attached the child before raising. Keep
        // the parent-owned properties and tracked-host bit until removeChild
        // returns normally; never infer detachment from a missing return value.
        rollback_retained = attached || reusable_shape ||
            attachment_uncertain;
#if defined(_MSC_VER)
        __try {
#endif
            if (attached || reusable_shape || attachment_uncertain) {
                rollback_detached = reinterpret_cast<ValueInvokeFn>(
                    parent_view.invoke_slot)(
                    parent_view.object_interface,
                    parent_view.object_payload,
                    nullptr,
                    "removeChild",
                    static_cast<const std::byte*>(shape_wrapper) + 0x08,
                    1,
                    true);
            } else {
                rollback_detached = true;
            }
            if (rollback_detached) rollback_retained = false;
            if (!rollback_detached && g_set_scaleform_visible) {
                rollback_hidden = g_set_scaleform_visible(
                    shape_wrapper, 0) == shape_wrapper;
            }
            rollback_visible = !authored_hidden ||
                (g_set_scaleform_visible &&
                    g_set_scaleform_visible(target_value, 1) == target_value);
            if (rollback_detached && g_construct_scaleform_value &&
                g_construct_scaleform_value(undefined_wrapper.data()) ==
                    undefined_wrapper.data()) {
                undefined_constructed = true;
                const auto set_member = reinterpret_cast<ValueSetMemberFn>(
                    parent_view.set_member_slot);
                const bool icon_cleared = set_member(
                    parent_view.object_interface,
                    parent_view.object_payload,
                    injected_property,
                    undefined_wrapper.data() + 0x08,
                    true);
                const bool authored_cleared = set_member(
                    parent_view.object_interface,
                    parent_view.object_payload,
                    authored_property,
                    undefined_wrapper.data() + 0x08,
                    true);
                properties_cleared = icon_cleared && authored_cleared;
            }
            if (!rollback_detached) rollback_retained = true;
            // removeChild proves only display-list detachment. Either private
            // parent property may still own the Shape (and therefore its
            // RawImage fill) when a SetMember fails or when its success has
            // not been independently read back as Undefined. Retain the host
            // ledger conservatively so a later exact cleanup/epoch baseline
            // can account for that ownership; never infer release here.
            if (icon_property_set || authored_property_set) {
                rollback_retained = true;
            }
#if defined(_MSC_VER)
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            rollback_seh =
                static_cast<unsigned long>(GetExceptionCode());
        }
#endif
    }

    if ((result.custom_visible || rollback_retained) &&
        g_repaint_context.active &&
        g_repaint_context.top == g_runtime.top &&
        g_repaint_context.physical < active_physical_tab_count() &&
        g_repaint_context.layer != TabLayer::none) {
        const std::size_t bit_index = g_repaint_context.physical +
            (g_repaint_context.layer == TabLayer::back
                ? active_physical_tab_count()
                : 0);
        const TabHostMask mask = TabHostMask{1} << bit_index;
        AtomicTabHostMask& host_ledger = repaint_stages_fresh_host(bit_index)
            ? g_staged_injected_icon_hosts
            : g_injected_icon_hosts;
        const TabHostMask prior = host_ledger.fetch_or(
            mask, std::memory_order_acq_rel);
        (void)prior;
        result.host_retained = true;
    }

    if (attempt <= kMaximumShapeAttachLogs) {
        log_line(
            "rgba8Shape[%u] context={layer:%u physical:%zu} parent=%u target=%u tracked=%u reused=%u requiredReuse=%u created=%u resource={read:%u consumed:%u bindAttempted:%u rasterBound:%u sameAssetSkipped:%u release:%u seh:0x%08lX refs:%u/%u->%u/%u exact:%u}",
            static_cast<unsigned>(attempt),
            static_cast<unsigned>(g_repaint_context.layer),
            g_repaint_context.physical,
            exact_parent ? 1U : 0U,
            exact_target ? 1U : 0U,
            tracked_cell ? 1U : 0U,
            reusable_shape ? 1U : 0U,
            require_reusable_shape ? 1U : 0U,
            created_shape ? 1U : 0U,
            resource_read ? 1U : 0U,
            result.resource_consumed ? 1U : 0U,
            result.raster_bind_attempted ? 1U : 0U,
            result.raster_bound ? 1U : 0U,
            result.same_asset_bind_skipped ? 1U : 0U,
            release_completed ? 1U : 0U,
            result.consume_seh,
            result.reference_before_read ? 1U : 0U,
            static_cast<unsigned>(result.reference_before),
            result.reference_after_read ? 1U : 0U,
            static_cast<unsigned>(result.reference_after),
            result.exact_reference_transition ? 1U : 0U);
        log_line(
            "rgba8Shape[%u] fallback={eligible:%u graphics:%u rects:%u visible:%u seh:0x%08lX} properties={set:%u/%u verified:%u} presentation={positioned:%u at:%.3f,%.3f visible:%u addAttempted:%u attached:%u attachUncertain:%u authoredHidden:%u customVisible:%u} rollback={detached:%u hidden:%u visible:%u cleared:%u retained:%u seh:0x%08lX}",
            static_cast<unsigned>(attempt),
            can_paint_fallback ? 1U : 0U,
            exact_graphics ? 1U : 0U,
            static_cast<unsigned>(painted_rectangles),
            result.vector_fallback_visible ? 1U : 0U,
            paint_seh,
            icon_property_set ? 1U : 0U,
            authored_property_set ? 1U : 0U,
            properties_verified ? 1U : 0U,
            positioned ? 1U : 0U,
            static_cast<double>(position_x),
            static_cast<double>(position_y),
            shape_visible ? 1U : 0U,
            add_child_attempted ? 1U : 0U,
            attached ? 1U : 0U,
            attachment_uncertain ? 1U : 0U,
            authored_hidden ? 1U : 0U,
            result.custom_visible ? 1U : 0U,
            rollback_detached ? 1U : 0U,
            rollback_hidden ? 1U : 0U,
            rollback_visible ? 1U : 0U,
            properties_cleared ? 1U : 0U,
            rollback_retained ? 1U : 0U,
            rollback_seh);
    }

    const bool cleanup_safe = parent_seh == 0 && existing_get_seh == 0 &&
        constructor_seh == 0 && create_seh == 0 &&
        result.consume_seh == 0 && graphics_get_seh == 0 &&
        result.graphics_clear_seh == 0 && paint_seh == 0 &&
        property_set_seh == 0 && property_verify_seh == 0 &&
        position_seh == 0 && visible_seh == 0 && attach_seh == 0 &&
        hide_seh == 0 && rollback_seh == 0;
    bool wrapper_destruction_exact = cleanup_safe;
    if (cleanup_safe) {
        wrapper_destruction_exact = destroy_local_scaleform_value(
            undefined_wrapper,
            undefined_constructed,
            "RGBA8 rollback undefined value") &&
            wrapper_destruction_exact;
        wrapper_destruction_exact = destroy_local_scaleform_value(
            verified_authored_wrapper,
            verified_authored_constructed,
            "RGBA8 verified authored icon") &&
            wrapper_destruction_exact;
        wrapper_destruction_exact = destroy_local_scaleform_value(
            verified_icon_wrapper,
            verified_icon_constructed,
            "RGBA8 verified injected icon") &&
            wrapper_destruction_exact;
        wrapper_destruction_exact = destroy_local_scaleform_value(
            graphics_wrapper,
            graphics_constructed,
            "RGBA8 fallback graphics") &&
            wrapper_destruction_exact;
        wrapper_destruction_exact = destroy_local_scaleform_value(
            created_wrapper,
            created_constructed,
            "RGBA8 created Shape") &&
            wrapper_destruction_exact;
        wrapper_destruction_exact = destroy_local_scaleform_value(
            existing_wrapper,
            existing_constructed,
            "RGBA8 existing Shape") &&
            wrapper_destruction_exact;
        wrapper_destruction_exact = destroy_local_scaleform_value(
            parent_wrapper,
            parent_constructed,
            "RGBA8 Shape parent") &&
            wrapper_destruction_exact;
    }
    const bool wrappers_released = !undefined_constructed &&
        !verified_authored_constructed && !verified_icon_constructed &&
        !graphics_constructed && !created_constructed &&
        !existing_constructed && !parent_constructed;
    result.cleanup_exact = cleanup_safe && wrapper_destruction_exact &&
        wrappers_released;
    if (result.cleanup_exact) {
        wrappers->~Rgba8ShapeWrapperArena();
        (void)HeapFree(GetProcessHeap(), 0, wrappers);
    } else {
        log_failure(
            "rgba8Shape[%u] retained heap wrapper arena after guarded fault/cleanup uncertainty (cleanupSafe=%u wrappersReleased=%u)",
            static_cast<unsigned>(attempt),
            cleanup_safe ? 1U : 0U,
            wrappers_released ? 1U : 0U);
    }
    if (attempt <= kMaximumShapeAttachLogs) {
        log_line(
            "rgba8Shape[%u] cleanup={preconditions:%u destructors:%u wrappersReleased:%u exact:%u}",
            static_cast<unsigned>(attempt),
            cleanup_safe ? 1U : 0U,
            wrapper_destruction_exact ? 1U : 0U,
            wrappers_released ? 1U : 0U,
            result.cleanup_exact ? 1U : 0U);
    }
    return result;
}

ShapeAttachProbeResult probe_dynamic_shape_attachment(
    std::uint32_t attempt,
    VectorIconKind icon_kind,
    bool transparent_cross,
    void* target_value,
    void* movie_root,
    void* create_object_slot,
    ImageBindFn image_binder,
    void** resource,
    const std::array<float, 4>& rectangle) noexcept {
    ShapeAttachProbeResult result{};
    const std::uint8_t property_bank = repaint_icon_property_bank();
    const char* const injected_property =
        injected_icon_property(property_bank);
    const char* const authored_property =
        authored_icon_property(property_bank);

    // The aliased native resource belongs to this binder call. Consume it on
    // the authored target first so every failure below can safely leave the
    // normal Graphics icon visible. The custom vector Shape is an overlay; it
    // never owns or forwards this native renderer resource.
    bool fallback_bound{};
    unsigned long bind_seh{};
    if (image_binder && resource) {
        result.resource_consumed = true;
#if defined(_MSC_VER)
        __try {
#endif
            fallback_bound = image_binder(
                target_value, resource, rectangle.data());
#if defined(_MSC_VER)
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            bind_seh = static_cast<unsigned long>(GetExceptionCode());
        }
#endif
    }
    result.bound = fallback_bound;
    if (!result.resource_consumed || bind_seh != 0) {
        if (attempt <= kMaximumShapeAttachLogs) {
            log_line(
                "shapeAttach[%u] kind=%u fallbackBound=%u resourceConsumed=%u bindSeh=0x%08lX customSkipped=1",
                static_cast<unsigned>(attempt),
                static_cast<unsigned>(icon_kind),
                fallback_bound ? 1U : 0U,
                result.resource_consumed ? 1U : 0U,
                bind_seh);
        }
        return result;
    }

    // Resolve target.parent through the game's tracked wrapper. All persistent
    // ownership stays inside that ActionScript parent: no GFx wrapper or raw
    // movie object is retained after this synchronous repaint call.
    alignas(16) std::array<std::byte, 0x38> parent_wrapper{};
    const char* parent_name = "parent";
    void* parent_result{};
    bool parent_constructed{};
    unsigned long parent_seh{};
    if (target_value && g_get_scaleform_member) {
#if defined(_MSC_VER)
        __try {
#endif
            parent_result = g_get_scaleform_member(
                target_value, parent_wrapper.data(), &parent_name);
            parent_constructed = parent_result == parent_wrapper.data();
#if defined(_MSC_VER)
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            parent_seh = static_cast<unsigned long>(GetExceptionCode());
        }
#endif
    }

    DisplayValueView parent_view{};
    const bool exact_parent = parent_seh == 0 && parent_constructed &&
        inspect_display_value(parent_wrapper.data(), parent_view) &&
        exact_as3_display_bridge(parent_view);

    DisplayValueView target_view{};
    const bool exact_target = inspect_display_value(
            target_value, target_view) &&
        exact_as3_display_bridge(target_view) &&
        validated_authored_icon_target_vtable(
            target_view.native_display_vtable);

    // Reuse a child already owned through the reserved parent property if the
    // same cell is repainted before a viewport remap. GetMember constructs its
    // destination, including the undefined-value case.
    alignas(16) std::array<std::byte, 0x38> existing_wrapper{};
    bool existing_constructed{};
    void* existing_result{};
    if (exact_parent && g_get_scaleform_member) {
        const char* property_name = injected_property;
#if defined(_MSC_VER)
        __try {
#endif
            existing_result = g_get_scaleform_member(
                parent_wrapper.data(), existing_wrapper.data(), &property_name);
            existing_constructed =
                existing_result == existing_wrapper.data();
#if defined(_MSC_VER)
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            existing_result = nullptr;
        }
#endif
    }
    DisplayValueView existing_view{};
    bool tracked_cell{};
    if (g_repaint_context.active &&
        g_repaint_context.top == g_runtime.top &&
        g_repaint_context.physical < active_physical_tab_count() &&
        g_repaint_context.layer != TabLayer::none) {
        const std::size_t bit_index = g_repaint_context.physical +
            (g_repaint_context.layer == TabLayer::back
                ? active_physical_tab_count()
                : 0);
        const AtomicTabHostMask& host_ledger =
            repaint_stages_fresh_host(bit_index)
            ? g_staged_injected_icon_hosts
            : g_injected_icon_hosts;
        tracked_cell = (host_ledger.load(std::memory_order_acquire) &
            (TabHostMask{1} << bit_index)) != 0;
    }
    // A failed parent-property clear can leave a dormant reference after the
    // Shape was successfully detached. Reuse only objects that the runtime
    // still tracks as attached to this exact cell; otherwise create a fresh
    // Shape and overwrite the stale property.
    const bool reusable_shape = tracked_cell && existing_constructed &&
        inspect_display_value(existing_wrapper.data(), existing_view) &&
        exact_as3_display_bridge(existing_view) &&
        existing_view.native_display_vtable ==
            g_game.base + kDynamicShapeVtable271Rva;

    // A CSScaleformValue is a 0x38-byte tracked wrapper whose public
    // GFx::Value begins at +0x08. Create a Shape only when the parent does not
    // already own our validated child.
    alignas(16) std::array<std::byte, 0x38> created_wrapper{};
    bool wrapper_constructed{};
    void* constructor_result{};
    unsigned long constructor_seh{};
    if (exact_parent && exact_target && !reusable_shape &&
        g_construct_scaleform_value) {
#if defined(_MSC_VER)
        __try {
#endif
            constructor_result =
                g_construct_scaleform_value(created_wrapper.data());
            wrapper_constructed = true;
#if defined(_MSC_VER)
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            constructor_seh =
                static_cast<unsigned long>(GetExceptionCode());
        }
#endif
    }

    unsigned long create_seh{};
    if (wrapper_constructed && movie_root && create_object_slot) {
#if defined(_MSC_VER)
        __try {
#endif
            reinterpret_cast<MovieCreateObjectFn>(create_object_slot)(
                movie_root,
                created_wrapper.data() + 0x08,
                "flash.display.Shape",
                nullptr,
                0);
#if defined(_MSC_VER)
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            create_seh = static_cast<unsigned long>(GetExceptionCode());
        }
#endif
    }

    DisplayValueView created_view{};
    const bool created_shape = wrapper_constructed && create_seh == 0 &&
        constructor_result == created_wrapper.data() &&
        inspect_display_value(created_wrapper.data(), created_view) &&
        exact_as3_display_bridge(created_view) &&
        created_view.native_display_vtable ==
            g_game.base + kDynamicShapeVtable271Rva;
    void* const shape_wrapper = reusable_shape
        ? static_cast<void*>(existing_wrapper.data())
        : static_cast<void*>(created_wrapper.data());
    const DisplayValueView& shape_view = reusable_shape
        ? existing_view
        : created_view;
    const bool exact_dynamic_shape = reusable_shape || created_shape;
    result.reused_existing_shape = reusable_shape;
    result.created_new_shape = created_shape;

    // Paint only a newly-created Shape. A reused Shape is already retained by
    // the GFX parent and already contains this vector probe. Calling the public
    // ActionScript Graphics methods is important: they perform Scaleform's
    // own twip conversion and display invalidation without creating a renderer
    // texture or entering the private DrawingContext ABI.
    alignas(16) std::array<std::byte, 0x38> graphics_wrapper{};
    bool graphics_constructed{};
    void* graphics_result{};
    unsigned long graphics_get_seh{};
    if (created_shape && g_get_scaleform_member) {
        const char* graphics_name = "graphics";
#if defined(_MSC_VER)
        __try {
#endif
            graphics_result = g_get_scaleform_member(
                created_wrapper.data(),
                graphics_wrapper.data(),
                &graphics_name);
            graphics_constructed =
                graphics_result == graphics_wrapper.data();
#if defined(_MSC_VER)
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            graphics_get_seh =
                static_cast<unsigned long>(GetExceptionCode());
        }
#endif
    }

    ObjectValueView graphics_view{};
    const bool exact_graphics = graphics_get_seh == 0 &&
        graphics_constructed &&
        inspect_object_value(graphics_wrapper.data(), graphics_view) &&
        exact_as3_object_bridge(graphics_view);
    const double local_width =
        static_cast<double>(rectangle[2] - rectangle[0]);
    const double local_height =
        static_cast<double>(rectangle[3] - rectangle[1]);
    const bool sane_extents = local_width > 0.0 && local_width <= 256.0 &&
        local_height > 0.0 && local_height <= 256.0;
    const double overlay_left = icon_kind == VectorIconKind::cross
        ? kCrossCanvasLeft
        : static_cast<double>(rectangle[0]);
    const double overlay_top = icon_kind == VectorIconKind::cross
        ? kCrossCanvasTop
        : static_cast<double>(rectangle[1]);
    const double overlay_width = icon_kind == VectorIconKind::cross
        ? kCrossCanvasWidth
        : local_width;
    const double overlay_height = icon_kind == VectorIconKind::cross
        ? kCrossCanvasHeight
        : local_height;

    unsigned long paint_seh{};
    std::uint32_t painted_rectangles{};
    const double primary_cross_alpha = transparent_cross ? 0.0 : 0.92;
    const double center_cross_alpha = transparent_cross ? 0.0 : 0.96;
    if (exact_graphics && sane_extents &&
        icon_kind == VectorIconKind::cross) {
        if (draw_graphics_rectangle(
                graphics_view,
                0x42E8F5U,
                primary_cross_alpha,
                overlay_width * 0.42,
                overlay_height * 0.08,
                overlay_width * 0.16,
                overlay_height * 0.84,
                paint_seh)) {
            ++painted_rectangles;
        }
        if (painted_rectangles == 1 && draw_graphics_rectangle(
                graphics_view,
                0xFFC857U,
                primary_cross_alpha,
                overlay_width * 0.12,
                overlay_height * 0.42,
                overlay_width * 0.76,
                overlay_height * 0.16,
                paint_seh)) {
            ++painted_rectangles;
        }
        if (painted_rectangles == 2 && draw_graphics_rectangle(
                graphics_view,
                0xD95CFFU,
                center_cross_alpha,
                overlay_width * 0.36,
                overlay_height * 0.36,
                overlay_width * 0.28,
                overlay_height * 0.28,
                paint_seh)) {
            ++painted_rectangles;
        }
    } else if (exact_graphics && sane_extents &&
        icon_kind == VectorIconKind::ellipsis) {
        const double dot_size =
            (std::min)(local_width, local_height) * 0.11;
        const double dot_y = local_height * 0.50 - dot_size * 0.50;
        constexpr std::array<double, 3> kDotCenters{0.25, 0.50, 0.75};
        for (double center : kDotCenters) {
            if (!draw_graphics_rectangle(
                    graphics_view,
                    0xC8BE9FU,
                    0.96,
                    local_width * center - dot_size * 0.50,
                    dot_y,
                    dot_size,
                    dot_size,
                    paint_seh)) {
                break;
            }
            ++painted_rectangles;
        }
    }
    const bool painted = reusable_shape ||
        (created_shape && painted_rectangles == 3 && paint_seh == 0);
    const bool exact_drawable_shape = exact_dynamic_shape && painted;

    bool icon_property_set{};
    bool authored_property_set{};
    unsigned long property_set_seh{};
    if (exact_parent && exact_target && exact_drawable_shape) {
#if defined(_MSC_VER)
        __try {
#endif
            const auto set_member = reinterpret_cast<ValueSetMemberFn>(
                parent_view.set_member_slot);
            icon_property_set = set_member(
                parent_view.object_interface,
                parent_view.object_payload,
                injected_property,
                static_cast<const std::byte*>(shape_wrapper) + 0x08,
                true);
            authored_property_set = icon_property_set && set_member(
                parent_view.object_interface,
                parent_view.object_payload,
                authored_property,
                static_cast<const std::byte*>(target_value) + 0x08,
                true);
#if defined(_MSC_VER)
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            property_set_seh =
                static_cast<unsigned long>(GetExceptionCode());
        }
#endif
    }

    alignas(16) std::array<std::byte, 0x38> verified_icon_wrapper{};
    alignas(16) std::array<std::byte, 0x38> verified_authored_wrapper{};
    bool verified_icon_constructed{};
    bool verified_authored_constructed{};
    unsigned long property_verify_seh{};
    if (icon_property_set && authored_property_set &&
        g_get_scaleform_member) {
        const char* icon_property = injected_property;
#if defined(_MSC_VER)
        __try {
#endif
            verified_icon_constructed = g_get_scaleform_member(
                parent_wrapper.data(),
                verified_icon_wrapper.data(),
                &icon_property) == verified_icon_wrapper.data();
            verified_authored_constructed = g_get_scaleform_member(
                parent_wrapper.data(),
                verified_authored_wrapper.data(),
                &authored_property) == verified_authored_wrapper.data();
#if defined(_MSC_VER)
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            property_verify_seh =
                static_cast<unsigned long>(GetExceptionCode());
        }
#endif
    }
    DisplayValueView verified_icon_view{};
    DisplayValueView verified_authored_view{};
    const bool properties_verified = property_verify_seh == 0 &&
        verified_icon_constructed && verified_authored_constructed &&
        inspect_display_value(
            verified_icon_wrapper.data(), verified_icon_view) &&
        inspect_display_value(
            verified_authored_wrapper.data(), verified_authored_view) &&
        exact_as3_display_bridge(verified_icon_view) &&
        exact_as3_display_bridge(verified_authored_view) &&
        verified_icon_view.object_payload == shape_view.object_payload &&
        verified_icon_view.native_display_vtable ==
            g_game.base + kDynamicShapeVtable271Rva &&
        verified_authored_view.object_payload == target_view.object_payload;

    void* position_result{};
    unsigned long position_seh{};
    if (properties_verified && painted && g_set_scaleform_position) {
#if defined(_MSC_VER)
        __try {
#endif
            position_result = g_set_scaleform_position(
                shape_wrapper,
                static_cast<float>(overlay_left),
                static_cast<float>(overlay_top));
#if defined(_MSC_VER)
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            position_seh =
                static_cast<unsigned long>(GetExceptionCode());
        }
#endif
    }
    const bool positioned = position_seh == 0 &&
        position_result == shape_wrapper;

    bool shape_visible{};
    unsigned long shape_visible_seh{};
    if (painted && positioned && g_set_scaleform_visible) {
#if defined(_MSC_VER)
        __try {
#endif
            shape_visible = g_set_scaleform_visible(shape_wrapper, 1) ==
                shape_wrapper;
#if defined(_MSC_VER)
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            shape_visible_seh =
                static_cast<unsigned long>(GetExceptionCode());
        }
#endif
    }

    bool attached{};
    unsigned long attach_seh{};
    if (painted && positioned && shape_visible) {
#if defined(_MSC_VER)
        __try {
#endif
            attached = reinterpret_cast<ValueInvokeFn>(
                parent_view.invoke_slot)(
                parent_view.object_interface,
                parent_view.object_payload,
                nullptr,
                "addChild",
                static_cast<const std::byte*>(shape_wrapper) + 0x08,
                1,
                true);
#if defined(_MSC_VER)
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            attach_seh = static_cast<unsigned long>(GetExceptionCode());
        }
#endif
    }

    bool authored_hidden{};
    unsigned long hide_seh{};
    if (painted && attached && g_set_scaleform_visible) {
#if defined(_MSC_VER)
        __try {
#endif
            authored_hidden = g_set_scaleform_visible(target_value, 0) ==
                target_value;
#if defined(_MSC_VER)
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            hide_seh = static_cast<unsigned long>(GetExceptionCode());
        }
#endif
    }

    const bool custom_visible =
        painted && positioned && shape_visible && attached && authored_hidden;
    result.custom_visible = custom_visible;
    result.bound = fallback_bound || custom_visible;

    // If any post-registration step failed, detach the child if necessary,
    // restore the native icon, and clear both parent-owned references. A fresh
    // undefined tracked value is the safest SetMember source for clearing.
    bool rollback_detached{};
    bool rollback_hidden{};
    bool rollback_visible{};
    bool properties_cleared{};
    bool rollback_retained{};
    unsigned long rollback_seh{};
    alignas(16) std::array<std::byte, 0x38> undefined_wrapper{};
    bool undefined_constructed{};
    if (!custom_visible && (icon_property_set || authored_property_set)) {
#if defined(_MSC_VER)
        __try {
#endif
            if (attached || reusable_shape) {
                rollback_detached = reinterpret_cast<ValueInvokeFn>(
                    parent_view.invoke_slot)(
                    parent_view.object_interface,
                    parent_view.object_payload,
                    nullptr,
                    "removeChild",
                    static_cast<const std::byte*>(shape_wrapper) + 0x08,
                    1,
                    true);
            } else {
                rollback_detached = true;
            }
            if (!rollback_detached && g_set_scaleform_visible) {
                rollback_hidden = g_set_scaleform_visible(
                    shape_wrapper, 0) == shape_wrapper;
            }
            rollback_visible = !authored_hidden ||
                (g_set_scaleform_visible &&
                    g_set_scaleform_visible(target_value, 1) == target_value);
            if (rollback_detached && g_construct_scaleform_value &&
                g_construct_scaleform_value(undefined_wrapper.data()) ==
                    undefined_wrapper.data()) {
                undefined_constructed = true;
                const auto set_member = reinterpret_cast<ValueSetMemberFn>(
                    parent_view.set_member_slot);
                const bool icon_cleared = set_member(
                    parent_view.object_interface,
                    parent_view.object_payload,
                    injected_property,
                    undefined_wrapper.data() + 0x08,
                    true);
                const bool authored_cleared = set_member(
                    parent_view.object_interface,
                    parent_view.object_payload,
                    authored_property,
                    undefined_wrapper.data() + 0x08,
                    true);
                properties_cleared = icon_cleared && authored_cleared;
            }
            rollback_retained = !rollback_detached && rollback_hidden;
#if defined(_MSC_VER)
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            rollback_seh =
                static_cast<unsigned long>(GetExceptionCode());
        }
#endif
    }

    if ((custom_visible || rollback_retained) && g_repaint_context.active &&
        g_repaint_context.top == g_runtime.top &&
        g_repaint_context.physical < active_physical_tab_count() &&
        g_repaint_context.layer != TabLayer::none) {
        const std::size_t bit_index = g_repaint_context.physical +
            (g_repaint_context.layer == TabLayer::back
                ? active_physical_tab_count()
                : 0);
        const TabHostMask mask = TabHostMask{1} << bit_index;
        AtomicTabHostMask& host_ledger = repaint_stages_fresh_host(bit_index)
            ? g_staged_injected_icon_hosts
            : g_injected_icon_hosts;
        host_ledger.fetch_or(mask, std::memory_order_acq_rel);
    }

    if (attempt <= kMaximumShapeAttachLogs) {
        log_line(
            "shapeAttach[%u] kind=%u context=%u layer=%u physical=%zu transparentPlaceholder=%u exactParent=%u exactTarget=%u reused=%u ctor=%u ctorSeh=0x%08lX createSeh=0x%08lX exactDynamic=%u graphics={constructed:%u exact:%u seh:0x%08lX} sourceRect={%.3f,%.3f,%.3f,%.3f sane:%u} overlayRect={%.3f,%.3f,%.3f,%.3f} painted={rects:%u final:%u seh:0x%08lX}",
            static_cast<unsigned>(attempt),
            static_cast<unsigned>(icon_kind),
            g_repaint_context.active ? 1U : 0U,
            static_cast<unsigned>(g_repaint_context.layer),
            g_repaint_context.physical,
            transparent_cross ? 1U : 0U,
            exact_parent ? 1U : 0U,
            exact_target ? 1U : 0U,
            reusable_shape ? 1U : 0U,
            wrapper_constructed ? 1U : 0U,
            constructor_seh,
            create_seh,
            exact_dynamic_shape ? 1U : 0U,
            graphics_constructed ? 1U : 0U,
            exact_graphics ? 1U : 0U,
            graphics_get_seh,
            static_cast<double>(rectangle[0]),
            static_cast<double>(rectangle[1]),
            static_cast<double>(rectangle[2]),
            static_cast<double>(rectangle[3]),
            sane_extents ? 1U : 0U,
            overlay_left,
            overlay_top,
            overlay_left + overlay_width,
            overlay_top + overlay_height,
            static_cast<unsigned>(painted_rectangles),
            painted ? 1U : 0U,
            paint_seh);
        log_line(
            "shapeAttach[%u] exactDrawable=%u propertySet={%u,%u} propertySetSeh=0x%08lX propertyVerified=%u propertyVerifySeh=0x%08lX",
            static_cast<unsigned>(attempt),
            exact_drawable_shape ? 1U : 0U,
            icon_property_set ? 1U : 0U,
            authored_property_set ? 1U : 0U,
            property_set_seh,
            properties_verified ? 1U : 0U,
            property_verify_seh);
        log_line(
            "shapeAttach[%u] positioned=%u position={%.3f,%.3f} positionSeh=0x%08lX shapeVisible=%u shapeVisibleSeh=0x%08lX attached=%u attachSeh=0x%08lX resourceConsumed=%u fallbackBound=%u customVisible=%u bindSeh=0x%08lX authoredHidden=%u hideSeh=0x%08lX rollback={detached:%u hidden:%u visible:%u cleared:%u retained:%u seh:0x%08lX}",
            static_cast<unsigned>(attempt),
            positioned ? 1U : 0U,
            overlay_left,
            overlay_top,
            position_seh,
            shape_visible ? 1U : 0U,
            shape_visible_seh,
            attached ? 1U : 0U,
            attach_seh,
            result.resource_consumed ? 1U : 0U,
            fallback_bound ? 1U : 0U,
            custom_visible ? 1U : 0U,
            bind_seh,
            authored_hidden ? 1U : 0U,
            hide_seh,
            rollback_detached ? 1U : 0U,
            rollback_hidden ? 1U : 0U,
            rollback_visible ? 1U : 0U,
            properties_cleared ? 1U : 0U,
            rollback_retained ? 1U : 0U,
            rollback_seh);
    }

    const bool cleanup_safe = parent_seh == 0 && constructor_seh == 0 &&
        create_seh == 0 && graphics_get_seh == 0 && paint_seh == 0 &&
        property_set_seh == 0 && property_verify_seh == 0 && bind_seh == 0 &&
        position_seh == 0 && shape_visible_seh == 0 && attach_seh == 0 &&
        hide_seh == 0 && rollback_seh == 0;
    bool wrapper_destruction_exact = cleanup_safe;
    if (cleanup_safe) {
        wrapper_destruction_exact = destroy_local_scaleform_value(
            undefined_wrapper,
            undefined_constructed,
            "undefined property value") && wrapper_destruction_exact;
        wrapper_destruction_exact = destroy_local_scaleform_value(
            verified_authored_wrapper,
            verified_authored_constructed,
            "verified authored icon") && wrapper_destruction_exact;
        wrapper_destruction_exact = destroy_local_scaleform_value(
            verified_icon_wrapper,
            verified_icon_constructed,
            "verified injected icon") && wrapper_destruction_exact;
        wrapper_destruction_exact = destroy_local_scaleform_value(
            graphics_wrapper, graphics_constructed, "Shape graphics") &&
            wrapper_destruction_exact;
        wrapper_destruction_exact = destroy_local_scaleform_value(
            created_wrapper, wrapper_constructed, "created Shape") &&
            wrapper_destruction_exact;
        wrapper_destruction_exact = destroy_local_scaleform_value(
            existing_wrapper, existing_constructed, "existing Shape") &&
            wrapper_destruction_exact;
        wrapper_destruction_exact = destroy_local_scaleform_value(
            parent_wrapper, parent_constructed, "Shape parent") &&
            wrapper_destruction_exact;
    } else {
        log_failure(
            "shapeAttach[%u] leaked temporary wrappers after a guarded native fault",
            static_cast<unsigned>(attempt));
    }
    result.cleanup_exact = wrapper_destruction_exact &&
        !undefined_constructed && !verified_authored_constructed &&
        !verified_icon_constructed && !graphics_constructed &&
        !wrapper_constructed && !existing_constructed &&
        !parent_constructed;
    return result;
}

bool __fastcall image_bind_detour(
    void* scaleform_value,
    void** resource,
    const float* rectangle) noexcept {
    ImageBindFn const original =
        g_original_image_bind.load(std::memory_order_acquire);
    if (!original) return false;
    if (!g_viewport_callbacks_armed.load(std::memory_order_acquire)) {
        return original(scaleform_value, resource, rectangle);
    }

    void* resource_value{};
    (void)guarded_read(resource, resource_value);

    // Treat the marker as immediate: the validated presenter performs this
    // bind directly after its successful texture lookup. Consuming it at the
    // next bind prevents a stale pointer from correlating with unrelated UI.
    const PendingAliasedIcon pending = std::exchange(
        g_pending_aliased_icon, PendingAliasedIcon{});
    const bool correlated = pending.resource != nullptr &&
        pending.kind != VectorIconKind::none &&
        resource_value == pending.resource &&
        pending.presentation_epoch == g_runtime.presentation_epoch &&
        pending.context.active && g_repaint_context.active &&
        pending.context.top == g_repaint_context.top &&
        pending.context.layer == g_repaint_context.layer &&
        pending.context.physical == g_repaint_context.physical &&
        pending.context.logical == g_repaint_context.logical;
    if (!correlated) {
        return original(scaleform_value, resource, rectangle);
    }

    std::uint32_t target_flags{};
    void* target_object_interface{};
    void* target_object_interface_vtable{};
    void* target_get_member_slot{};
    void* target_invoke_slot{};
    void* target_get_movie_slot{};
    void* target_interface_owner_link{};
    void* target_movie_root{};
    void* target_movie_root_vtable{};
    void* target_movie_root_create_object_slot{};
    void* target_vm{};
    void* target_object{};
    void* native_display_object{};
    void* native_display_vtable{};
    void* resource_image{};
    std::array<float, 4> rectangle_values{};
    const auto* const target_bytes =
        static_cast<const std::byte*>(scaleform_value);
    const bool object_interface_read = target_bytes &&
        guarded_read(target_bytes + 0x18, target_object_interface);
    const bool object_interface_vtable_read = target_object_interface &&
        guarded_read(
            target_object_interface,
            target_object_interface_vtable);
    const auto* const object_interface_vtable_bytes =
        static_cast<const std::byte*>(target_object_interface_vtable);
    const bool get_member_slot_read = object_interface_vtable_bytes &&
        guarded_read(
            object_interface_vtable_bytes + 0x20,
            target_get_member_slot);
    const bool invoke_slot_read = object_interface_vtable_bytes &&
        guarded_read(
            object_interface_vtable_bytes + 0x30,
            target_invoke_slot);
    const bool get_movie_slot_read = object_interface_vtable_bytes &&
        guarded_read(
            object_interface_vtable_bytes + 0x178,
            target_get_movie_slot);
    const auto* const object_interface_bytes =
        static_cast<const std::byte*>(target_object_interface);
    const bool interface_owner_link_read = object_interface_bytes &&
        guarded_read(
            object_interface_bytes + 0x08,
            target_interface_owner_link);
    const auto* const interface_owner_link_bytes =
        static_cast<const std::byte*>(target_interface_owner_link);
    const bool movie_root_read = interface_owner_link_bytes &&
        guarded_read(
            interface_owner_link_bytes + 0x18,
            target_movie_root);
    const bool movie_root_vtable_read = target_movie_root &&
        guarded_read(target_movie_root, target_movie_root_vtable);
    const auto* const movie_root_vtable_bytes =
        static_cast<const std::byte*>(target_movie_root_vtable);
    const bool movie_root_create_object_slot_read =
        movie_root_vtable_bytes &&
        guarded_read(
            movie_root_vtable_bytes + 0x170,
            target_movie_root_create_object_slot);
    const auto* const movie_root_bytes =
        static_cast<const std::byte*>(target_movie_root);
    const bool vm_read = movie_root_bytes &&
        guarded_read(movie_root_bytes + 0x88, target_vm);
    const bool flags_read = target_bytes &&
        guarded_read(target_bytes + 0x20, target_flags);
    const bool object_read = target_bytes &&
        guarded_read(target_bytes + 0x28, target_object);
    const auto* const object_bytes =
        static_cast<const std::byte*>(target_object);
    const bool native_object_read = object_bytes &&
        guarded_read(object_bytes + 0x88, native_display_object);
    const bool native_vtable_read = native_display_object &&
        guarded_read(native_display_object, native_display_vtable);
    const auto* const resource_bytes =
        static_cast<const std::byte*>(resource_value);
    const bool resource_image_read = resource_bytes &&
        guarded_read(resource_bytes + 0x18, resource_image);
    const bool rectangle_read = rectangle && guarded_copy(
        rectangle_values.data(), rectangle, sizeof(rectangle_values));
    const bool exact_pending_asset = !pending.direct_rgba8 ||
        (pending.rgba8_index < active_custom_tab_count() &&
            resource_value ==
                g_retained_rgba8_resources[pending.rgba8_index].load(
                    std::memory_order_acquire) &&
            resource_image_read && resource_image ==
                g_retained_rgba8_images[pending.rgba8_index].load(
                    std::memory_order_acquire) &&
            g_retained_rgba8_ready[pending.rgba8_index].load(
                std::memory_order_acquire) &&
            pending.checksum ==
                g_retained_rgba8_checksums[pending.rgba8_index].load(
                    std::memory_order_acquire));

    bool fingerprint_consistent{};
    if (object_interface_read &&
        interface_owner_link_read && movie_root_read) {
        const bool unset =
            g_injected_movie_fingerprint.object_interface == nullptr &&
            g_injected_movie_fingerprint.owner_link == nullptr &&
            g_injected_movie_fingerprint.movie_root == nullptr;
        if (unset) {
            g_injected_movie_fingerprint.object_interface =
                target_object_interface;
            g_injected_movie_fingerprint.owner_link =
                target_interface_owner_link;
            g_injected_movie_fingerprint.movie_root = target_movie_root;
            g_injected_movie_fingerprint.presentation_epoch =
                g_runtime.presentation_epoch;
        }
        fingerprint_consistent =
            g_injected_movie_fingerprint.object_interface ==
                target_object_interface &&
            g_injected_movie_fingerprint.owner_link ==
                target_interface_owner_link &&
            g_injected_movie_fingerprint.movie_root == target_movie_root &&
            g_injected_movie_fingerprint.presentation_epoch ==
                g_runtime.presentation_epoch;
    }

    // The lookup reference still owns this known-native resource and the
    // original binder below will consume it. Observe scalar fields only while
    // that ownership is unquestionably live.
    if (resource_image_read) {
        log_native_image_resource_snapshot(resource_value, resource_image);
    }

    const std::uint32_t event =
        g_image_bind_log_count.fetch_add(1, std::memory_order_relaxed) + 1;
    if (event <= kMaximumImageBindLogs) {
        log_line(
            "iconBind[%u] epoch=%llu icon=%zu logical=%zu checksum=0x%016llX fingerprintConsistent=%u interfaceRead=%u interface=%p interfaceVtableRead=%u interfaceVtable=%p getMemberRead=%u getMember=%p invokeRead=%u invoke=%p getMovieRead=%u getMovie=%p ownerLinkRead=%u ownerLink=%p movieRootRead=%u movieRoot=%p movieRootVtableRead=%u movieRootVtable=%p rootCreateObjectRead=%u rootCreateObject=%p vmRead=%u vm=%p",
            static_cast<unsigned>(event),
            static_cast<unsigned long long>(pending.presentation_epoch),
            pending.rgba8_index,
            pending.logical,
            static_cast<unsigned long long>(pending.checksum),
            fingerprint_consistent ? 1U : 0U,
            object_interface_read ? 1U : 0U,
            target_object_interface,
            object_interface_vtable_read ? 1U : 0U,
            target_object_interface_vtable,
            get_member_slot_read ? 1U : 0U,
            target_get_member_slot,
            invoke_slot_read ? 1U : 0U,
            target_invoke_slot,
            get_movie_slot_read ? 1U : 0U,
            target_get_movie_slot,
            interface_owner_link_read ? 1U : 0U,
            target_interface_owner_link,
            movie_root_read ? 1U : 0U,
            target_movie_root,
            movie_root_vtable_read ? 1U : 0U,
            target_movie_root_vtable,
            movie_root_create_object_slot_read ? 1U : 0U,
            target_movie_root_create_object_slot,
            vm_read ? 1U : 0U,
            target_vm);
        log_line(
            "iconBind[%u] pre target=%p flagsRead=%u flags=0x%08X objectRead=%u object=%p nativeObjectRead=%u nativeObject=%p nativeVtableRead=%u nativeVtable=%p resourceArg=%p resource=%p imageRead=%u image=%p rectRead=%u rect={%.3f,%.3f,%.3f,%.3f}",
            static_cast<unsigned>(event),
            scaleform_value,
            flags_read ? 1U : 0U,
            static_cast<unsigned>(target_flags),
            object_read ? 1U : 0U,
            target_object,
            native_object_read ? 1U : 0U,
            native_display_object,
            native_vtable_read ? 1U : 0U,
            native_display_vtable,
            static_cast<void*>(resource),
            resource_value,
            resource_image_read ? 1U : 0U,
            resource_image,
            rectangle_read ? 1U : 0U,
            static_cast<double>(rectangle_values[0]),
            static_cast<double>(rectangle_values[1]),
            static_cast<double>(rectangle_values[2]),
            static_cast<double>(rectangle_values[3]));
    }

    // Validate the recovered ObjectInterface::Invoke ABI with an inherited,
    // side-effect-free method before any later probe creates a display object.
    // This is deliberately limited to the exact bridge selected above.
    if (!g_value_invoke_probe_attempted.exchange(
            true, std::memory_order_relaxed)) {
        const bool exact_get_movie =
            g_native_tabs_build_profile &&
            target_object_interface_vtable ==
                g_game.base + kAs3ValueInterfaceVtable271Rva &&
            target_get_movie_slot ==
                g_game.base + kAs3ValueGetMovie271Rva;
        void* public_movie{};
        void* public_movie_vtable{};
        void* public_movie_slot_170{};
        unsigned long get_movie_seh{};
        if (exact_get_movie) {
#if defined(_MSC_VER)
            __try {
#endif
                public_movie = reinterpret_cast<ValueGetMovieFn>(
                    target_get_movie_slot)(target_object_interface);
#if defined(_MSC_VER)
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                get_movie_seh =
                    static_cast<unsigned long>(GetExceptionCode());
            }
#endif
        }
        const bool public_movie_vtable_read = public_movie &&
            guarded_read(public_movie, public_movie_vtable);
        const auto* const public_movie_vtable_bytes =
            static_cast<const std::byte*>(public_movie_vtable);
        const bool public_movie_slot_170_read =
            public_movie_vtable_bytes && guarded_read(
                public_movie_vtable_bytes + 0x170,
                public_movie_slot_170);
        log_line(
            "valueGetMovie validation exact=%u movie=%p movieVtableRead=%u movieVtable=%p slot170Read=%u slot170=%p seh=0x%08lX",
            exact_get_movie ? 1U : 0U,
            public_movie,
            public_movie_vtable_read ? 1U : 0U,
            public_movie_vtable,
            public_movie_slot_170_read ? 1U : 0U,
            public_movie_slot_170,
            get_movie_seh);

        const bool exact_interface =
            g_native_tabs_build_profile &&
            target_object_interface_vtable ==
                g_game.base + kAs3ValueInterfaceVtable271Rva &&
            target_invoke_slot == g_game.base + kAs3ValueInvoke271Rva &&
            target_movie_root_vtable ==
                g_game.base + kAs3MovieVtable271Rva &&
            target_movie_root_create_object_slot ==
                g_game.base + kAs3MovieCreateObject271Rva &&
            validated_authored_icon_target_vtable(
                native_display_vtable) &&
            (target_flags & 0x8FU) == 0x0AU && target_object;
        bool invoked{};
        unsigned long seh_code{};
        if (exact_interface) {
#if defined(_MSC_VER)
            __try {
#endif
                invoked = reinterpret_cast<ValueInvokeFn>(
                    target_invoke_slot)(
                    target_object_interface,
                    target_object,
                    nullptr,
                    "toString",
                    nullptr,
                    0,
                    true);
#if defined(_MSC_VER)
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                seh_code =
                    static_cast<unsigned long>(GetExceptionCode());
            }
#endif
        }
        log_line(
            "valueInvoke validation exact=%u result=%u seh=0x%08lX method=toString",
            exact_interface ? 1U : 0U,
            invoked ? 1U : 0U,
            seh_code);
        if (exact_interface && invoked && seh_code == 0) {
            g_value_invoke_probe_validated.store(
                true, std::memory_order_release);
        }
    }

    const bool exact_attachment_bridge =
        g_native_tabs_build_profile &&
        g_repaint_context.active &&
        g_repaint_context.top == g_runtime.top &&
        g_repaint_context.layer != TabLayer::none &&
        g_repaint_context.physical < active_physical_tab_count() &&
        g_value_invoke_probe_validated.load(std::memory_order_acquire) &&
        target_object_interface_vtable ==
            g_game.base + kAs3ValueInterfaceVtable271Rva &&
        target_get_member_slot ==
            g_game.base + kAs3ValueGetMember271Rva &&
        target_invoke_slot == g_game.base + kAs3ValueInvoke271Rva &&
        target_movie_root_vtable ==
            g_game.base + kAs3MovieVtable271Rva &&
        target_movie_root_create_object_slot ==
            g_game.base + kAs3MovieCreateObject271Rva &&
        validated_authored_icon_target_vtable(
            native_display_vtable) &&
        flags_read && (target_flags & 0x8FU) == 0x0AU && target_object &&
        rectangle_read && g_construct_scaleform_value &&
        g_get_scaleform_member && g_set_scaleform_position &&
        g_set_scaleform_visible && g_destroy_scaleform_value;

    if (pending.direct_rgba8) {
        const std::uint32_t attach_attempt =
            g_shape_attach_attempt_count.fetch_add(
                1, std::memory_order_relaxed) + 1;
        const bool exact_host_coordinate =
            g_repaint_context.layer != TabLayer::none &&
            g_repaint_context.physical < active_physical_tab_count() &&
            pending.rgba8_index < active_custom_tab_count();
        const std::size_t host_bit = exact_host_coordinate
            ? g_repaint_context.physical +
                (g_repaint_context.layer == TabLayer::back
                    ? active_physical_tab_count()
                    : 0)
            : invalid_tab_index;
        const TabHostMask host_mask = exact_host_coordinate
            ? TabHostMask{1} << host_bit
            : 0;
        const std::size_t desired_asset = exact_host_coordinate
            ? pending.rgba8_index + 1U
            : 0;
        const bool staging_fresh_host = exact_host_coordinate &&
            repaint_stages_fresh_host(host_bit);
        const TabHostMask prior_rgba_hosts = staging_fresh_host
            ? g_staged_rgba8_icon_hosts.load(std::memory_order_acquire)
            : g_rgba8_icon_hosts.load(std::memory_order_acquire);
        const TabHostMask prior_injected_hosts = staging_fresh_host
            ? g_staged_injected_icon_hosts.load(std::memory_order_acquire)
            : g_injected_icon_hosts.load(std::memory_order_acquire);
        const std::size_t prior_asset = exact_host_coordinate
            ? (staging_fresh_host
                ? g_staged_rgba8_host_assets[host_bit].load(
                    std::memory_order_acquire)
                : g_rgba8_host_assets[host_bit].load(
                    std::memory_order_acquire))
            : 0;
        const bool prior_host_consistent = exact_host_coordinate &&
            (((prior_rgba_hosts & host_mask) != 0) ==
                (prior_asset != 0));
        const InPlaceRgba8RebindTransaction& rebind_transaction =
            g_in_place_rgba8_rebind_transaction;
        const bool matching_rebind_transaction =
            rebind_transaction.active &&
            g_runtime.pending.active &&
            rebind_transaction.top == g_repaint_context.top &&
            rebind_transaction.top == g_runtime.top &&
            rebind_transaction.presentation_epoch ==
                g_runtime.presentation_epoch &&
            rebind_transaction.source_window ==
                g_runtime.viewport.window_start() &&
            rebind_transaction.target_window ==
                g_runtime.pending.candidate.window_start() &&
            g_runtime.pending.top == g_runtime.top &&
            g_runtime.pending.candidate.mapping_generation() ==
                rebind_transaction.target_generation;
        const bool exact_rebind_transaction =
            matching_rebind_transaction && !rebind_transaction.failed &&
            rebind_transaction.stage ==
                InPlaceRgba8RebindStage::repainting;
        const bool exact_shape_replacement =
            exact_rebind_transaction && exact_host_coordinate &&
            (rebind_transaction.expected_host_mask & host_mask) != 0 &&
            staging_fresh_host &&
            rebind_transaction.target_assets[host_bit] == desired_asset &&
            (prior_injected_hosts & host_mask) == 0 &&
            (prior_rgba_hosts & host_mask) == 0 && prior_asset == 0U &&
            desired_asset > 0U &&
            desired_asset <= active_custom_tab_count() &&
            pending.logical ==
                kStockLogicalTabCount + pending.rgba8_index;
        const bool exact_same_asset_repaint =
            exact_rebind_transaction && exact_host_coordinate &&
            !staging_fresh_host &&
            prior_asset != 0 && prior_asset == desired_asset;
        const bool host_transition_compatible = prior_host_consistent &&
            (prior_asset == 0 || prior_asset == desired_asset ||
                exact_shape_replacement);
        if (!host_transition_compatible) {
            if (g_in_place_rgba8_rebind_transaction.active) {
                g_in_place_rgba8_rebind_transaction.failed = true;
            } else {
                g_rgba8_publication_disabled.store(
                    true, std::memory_order_release);
            }
            log_failure(
                "RGBA8 bind refused before Shape mutation because host ownership was incompatible (layer=%u physical=%zu priorAsset=%u desiredAsset=%u rgbaBits=0x%04X)",
                static_cast<unsigned>(g_repaint_context.layer),
                g_repaint_context.physical,
                static_cast<unsigned>(prior_asset),
                static_cast<unsigned>(desired_asset),
                static_cast<unsigned>(prior_rgba_hosts));
        }

        Rgba8ReferenceSnapshot fresh_refs_before{};
        bool rebind_pre_refs_exact{true};
        if (exact_shape_replacement) {
            const std::size_t fresh_asset_index = desired_asset - 1U;
            fresh_refs_before =
                snapshot_rgba8_references(fresh_asset_index);
            const std::uint32_t fresh_expected =
                1U + count_active_rgba8_asset_hosts(fresh_asset_index) +
                g_rgba8_retirement_debt[fresh_asset_index].load(
                    std::memory_order_acquire);
            rebind_pre_refs_exact = fresh_refs_before.resource_read &&
                fresh_refs_before.resource == 3U &&
                fresh_refs_before.raw_read &&
                fresh_refs_before.raw == fresh_expected &&
                g_rgba8_retirement_debt[fresh_asset_index].load(
                    std::memory_order_acquire) == 0U;
            log_line(
                "rgba8Replace pre transition=%llu layer=%u physical=%zu freshAsset=%u refs={resource:%u/%u raw:%u/%u expected:%u} exact=%u",
                static_cast<unsigned long long>(
                    rebind_transaction.transition_sequence),
                static_cast<unsigned>(g_repaint_context.layer),
                g_repaint_context.physical,
                static_cast<unsigned>(desired_asset),
                fresh_refs_before.resource_read ? 1U : 0U,
                static_cast<unsigned>(fresh_refs_before.resource),
                fresh_refs_before.raw_read ? 1U : 0U,
                static_cast<unsigned>(fresh_refs_before.raw),
                static_cast<unsigned>(fresh_expected),
                rebind_pre_refs_exact ? 1U : 0U);
            if (!rebind_pre_refs_exact) {
                g_in_place_rgba8_rebind_transaction.failed = true;
            }
        }
        Rgba8ShapeAttachProbeResult attachment{};
        if (exact_attachment_bridge && fingerprint_consistent &&
            exact_pending_asset && host_transition_compatible &&
            rebind_pre_refs_exact) {
            attachment = probe_dynamic_rgba8_shape_attachment(
                attach_attempt,
                scaleform_value,
                target_movie_root,
                target_movie_root_create_object_slot,
                original,
                resource,
                rectangle_values,
                exact_same_asset_repaint,
                exact_same_asset_repaint,
                false);
        } else {
            // The presenter has already created a binder-local reference. If
            // the exact dynamic-Shape bridge is unavailable, balance that one
            // reference directly; forwarding this custom resource to the
            // authored static Shape is the disproven route.
            attachment.reference_before_read = guarded_read(
                static_cast<const std::byte*>(resource_value) + 0x08,
                attachment.reference_before);
            const bool exact_incoming_reference =
                attachment.reference_before_read &&
                attachment.reference_before == 3U;
            if (exact_incoming_reference) {
                attachment.resource_consumed =
                    guarded_release_scaleform_image_resource(
                        reinterpret_cast<ScaleformImageResourceReleaseFn>(
                            g_game.base +
                            kScaleformImageResourceRelease271Rva),
                        resource_value,
                        attachment.consume_seh);
                attachment.reference_after_read = guarded_read(
                    static_cast<const std::byte*>(resource_value) + 0x08,
                    attachment.reference_after);
            }
            attachment.exact_reference_transition =
                attachment.resource_consumed &&
                attachment.reference_before_read &&
                attachment.reference_after_read &&
                attachment.reference_before == 3U &&
                attachment.reference_after == 2U;
        }

        Rgba8ReferenceSnapshot fresh_refs_after{};
        bool exact_rebind_refs{};
        if (exact_shape_replacement && attachment.resource_consumed &&
            attachment.raster_bind_attempted && attachment.raster_bound &&
            attachment.created_new_shape &&
            !attachment.reused_existing_shape && attachment.cleanup_exact &&
            attachment.consume_seh == 0) {
            const std::size_t fresh_asset_index = desired_asset - 1U;
            fresh_refs_after =
                snapshot_rgba8_references(fresh_asset_index);
            exact_rebind_refs = fresh_refs_after.resource_read &&
                fresh_refs_after.resource == 2U &&
                fresh_refs_after.raw_read && fresh_refs_before.raw_read &&
                fresh_refs_before.raw <
                    std::numeric_limits<std::uint32_t>::max() &&
                fresh_refs_after.raw == fresh_refs_before.raw + 1U &&
                !attachment.pre_bind_graphics_clear_required &&
                !attachment.pre_bind_graphics_clear_attempted &&
                attachment.graphics_clear_seh == 0;
            log_line(
                "rgba8Replace attach transition=%llu layer=%u physical=%zu freshAsset=%u created=%u reused=%u binder={attempted:%u bound:%u seh:0x%08lX} refs={resource:%u/%u raw:%u/%u before:%u} debt=%u exact=%u",
                static_cast<unsigned long long>(
                    rebind_transaction.transition_sequence),
                static_cast<unsigned>(g_repaint_context.layer),
                g_repaint_context.physical,
                static_cast<unsigned>(desired_asset),
                attachment.created_new_shape ? 1U : 0U,
                attachment.reused_existing_shape ? 1U : 0U,
                attachment.raster_bind_attempted ? 1U : 0U,
                attachment.raster_bound ? 1U : 0U,
                attachment.consume_seh,
                fresh_refs_after.resource_read ? 1U : 0U,
                static_cast<unsigned>(fresh_refs_after.resource),
                fresh_refs_after.raw_read ? 1U : 0U,
                static_cast<unsigned>(fresh_refs_after.raw),
                static_cast<unsigned>(fresh_refs_before.raw),
                static_cast<unsigned>(
                    g_rgba8_retirement_debt[fresh_asset_index].load(
                        std::memory_order_acquire)),
                exact_rebind_refs ? 1U : 0U);
            if (!exact_rebind_refs) {
                g_in_place_rgba8_rebind_transaction.failed = true;
            }
        }

        const bool presentation_bind_exact = attachment.raster_bound ||
            attachment.same_asset_bind_skipped;
        bool host_asset_recorded{};
        if (attachment.host_retained && presentation_bind_exact &&
            pending.rgba8_index < active_custom_tab_count() &&
            g_repaint_context.layer != TabLayer::none &&
            g_repaint_context.physical < active_physical_tab_count()) {
            const std::size_t bit = host_bit;
            const TabHostMask mask = host_mask;
            const std::size_t recorded_prior_asset =
                staging_fresh_host
                ? g_staged_rgba8_host_assets[bit].load(
                    std::memory_order_acquire)
                : g_rgba8_host_assets[bit].load(
                    std::memory_order_acquire);
            AtomicTabHostMask& rgba_host_ledger = staging_fresh_host
                ? g_staged_rgba8_icon_hosts
                : g_rgba8_icon_hosts;
            auto& asset_ledger = staging_fresh_host
                ? g_staged_rgba8_host_assets
                : g_rgba8_host_assets;
            const TabHostMask prior_hosts =
                rgba_host_ledger.load(std::memory_order_acquire);
            const bool prior_consistent =
                ((prior_hosts & mask) != 0) ==
                (recorded_prior_asset != 0);
            const bool compatible_asset = recorded_prior_asset == 0 ||
                recorded_prior_asset == desired_asset ||
                (exact_shape_replacement && exact_rebind_refs &&
                    recorded_prior_asset == 0U);
            if (prior_consistent && compatible_asset) {
                const TabHostMask prior = rgba_host_ledger.fetch_or(
                    mask, std::memory_order_acq_rel);
                asset_ledger[bit].store(
                    desired_asset, std::memory_order_release);
                host_asset_recorded = true;
                if (exact_shape_replacement && exact_rebind_refs) {
                    g_in_place_rgba8_rebind_transaction.completed_host_mask =
                        g_in_place_rgba8_rebind_transaction
                            .completed_host_mask | mask;
                }
                if ((prior & mask) == 0) {
                    g_rgba8_successful_attach_count.fetch_add(
                        1, std::memory_order_relaxed);
                }
            } else {
                if (g_in_place_rgba8_rebind_transaction.active) {
                    g_in_place_rgba8_rebind_transaction.failed = true;
                } else {
                    g_rgba8_publication_disabled.store(
                        true, std::memory_order_release);
                }
                log_failure(
                    "RGBA8 host asset transition refused without a prior detach proof (layer=%u physical=%zu priorAsset=%u desiredAsset=%u priorBits=0x%04X)",
                    static_cast<unsigned>(g_repaint_context.layer),
                    g_repaint_context.physical,
                    static_cast<unsigned>(recorded_prior_asset),
                    static_cast<unsigned>(desired_asset),
                    static_cast<unsigned>(prior_hosts));
            }
        } else if (attachment.host_retained &&
            g_repaint_context.layer != TabLayer::none &&
            g_repaint_context.physical < active_physical_tab_count()) {
            const std::size_t bit = g_repaint_context.physical +
                (g_repaint_context.layer == TabLayer::back
                    ? active_physical_tab_count()
                    : 0);
            const TabHostMask mask = TabHostMask{1} << bit;
            const TabHostMask prior = g_rgba8_icon_hosts.load(
                std::memory_order_acquire);
            if ((prior & mask) != 0) {
                // A vector fallback can repaint the same Shape, but it is not
                // proof that Scaleform released the previous raster fill.
                // Preserve the old asset association until exact Shape
                // removal or an epoch/root-refcount proof clears it.
                log_line(
                    "RGBA8 fallback preserved retained asset ownership pending detach proof (layer=%u physical=%zu asset=%u)",
                    static_cast<unsigned>(g_repaint_context.layer),
                    g_repaint_context.physical,
                    static_cast<unsigned>(
                        g_rgba8_host_assets[bit].load(
                            std::memory_order_acquire)));
            }
        }

        const bool exact_dynamic_raster = exact_attachment_bridge &&
            fingerprint_consistent &&
            exact_pending_asset &&
            host_transition_compatible &&
            pending.rgba8_index < active_custom_tab_count() &&
            pending.logical ==
                kStockLogicalTabCount + pending.rgba8_index &&
            attachment.resource_consumed &&
            ((attachment.raster_bind_attempted &&
                 attachment.raster_bound) ||
                attachment.same_asset_bind_skipped) &&
            host_asset_recorded &&
            (!exact_shape_replacement || exact_rebind_refs) &&
            (!exact_shape_replacement ||
                (!attachment.pre_bind_graphics_clear_required &&
                    !attachment.pre_bind_graphics_clear_attempted &&
                    attachment.graphics_clear_seh == 0)) &&
            (!exact_same_asset_repaint ||
                attachment.reused_existing_shape) &&
            attachment.exact_reference_transition &&
            attachment.cleanup_exact &&
            attachment.custom_visible && attachment.consume_seh == 0;
        const bool expected_transaction_rgba_cell =
            matching_rebind_transaction && exact_host_coordinate &&
            (rebind_transaction.expected_rgba_presentation_mask &
                host_mask) != 0;
        if (expected_transaction_rgba_cell) {
            if (exact_dynamic_raster && !rebind_transaction.failed) {
                g_in_place_rgba8_rebind_transaction
                    .completed_rgba_presentation_mask =
                    g_in_place_rgba8_rebind_transaction
                        .completed_rgba_presentation_mask | host_mask;
            } else {
                g_in_place_rgba8_rebind_transaction.failed = true;
            }
            log_line(
                "rgba8Replace presentation layer=%u physical=%zu asset=%u exact=%u completed=0x%04X/0x%04X",
                static_cast<unsigned>(g_repaint_context.layer),
                g_repaint_context.physical,
                static_cast<unsigned>(desired_asset),
                exact_dynamic_raster ? 1U : 0U,
                static_cast<unsigned>(
                    g_in_place_rgba8_rebind_transaction
                        .completed_rgba_presentation_mask),
                static_cast<unsigned>(
                    rebind_transaction.expected_rgba_presentation_mask));
        }
        const std::uint32_t rgba_event =
            g_rgba8_bind_log_count.fetch_add(
                1, std::memory_order_relaxed) + 1;
        log_line(
            "rgba8Bind[%u] epoch=%llu icon=%zu logical=%zu checksum=0x%016llX context={layer:%u physical:%zu} fingerprintConsistent=%u exactAsset=%u correlated=1 resource=%p image=%p rectRead=%u bridge=%u consumed=%u bindAttempted=%u sameAssetSkipped=%u created=%u reused=%u freshReplacement=%u replacementRefs=%u preClear={required:%u attempted:%u cleared:%u oldReleased:%u seh:0x%08lX} refs={beforeRead:%u before:%u afterRead:%u after:%u exact:%u} rasterBound=%u customVisible=%u vectorFallback=%u cleanupExact=%u result=%u seh=0x%08lX authoredTarget=0 dynamicShape=1",
            static_cast<unsigned>(rgba_event),
            static_cast<unsigned long long>(pending.presentation_epoch),
            pending.rgba8_index,
            pending.logical,
            static_cast<unsigned long long>(pending.checksum),
            static_cast<unsigned>(g_repaint_context.layer),
            g_repaint_context.physical,
            fingerprint_consistent ? 1U : 0U,
            exact_pending_asset ? 1U : 0U,
            resource_value,
            resource_image,
            rectangle_read ? 1U : 0U,
            exact_attachment_bridge ? 1U : 0U,
            attachment.resource_consumed ? 1U : 0U,
            attachment.raster_bind_attempted ? 1U : 0U,
            attachment.same_asset_bind_skipped ? 1U : 0U,
            attachment.created_new_shape ? 1U : 0U,
            attachment.reused_existing_shape ? 1U : 0U,
            exact_shape_replacement ? 1U : 0U,
            exact_rebind_refs ? 1U : 0U,
            attachment.pre_bind_graphics_clear_required ? 1U : 0U,
            attachment.pre_bind_graphics_clear_attempted ? 1U : 0U,
            attachment.pre_bind_graphics_cleared ? 1U : 0U,
            attachment.pre_bind_old_raw_released_exact ? 1U : 0U,
            attachment.graphics_clear_seh,
            attachment.reference_before_read ? 1U : 0U,
            static_cast<unsigned>(attachment.reference_before),
            attachment.reference_after_read ? 1U : 0U,
            static_cast<unsigned>(attachment.reference_after),
            attachment.exact_reference_transition ? 1U : 0U,
            attachment.raster_bound ? 1U : 0U,
            attachment.custom_visible ? 1U : 0U,
            attachment.vector_fallback_visible ? 1U : 0U,
            attachment.cleanup_exact ? 1U : 0U,
            attachment.bound ? 1U : 0U,
            attachment.consume_seh);
        if (!exact_dynamic_raster) {
            if (g_in_place_rgba8_rebind_transaction.active) {
                g_in_place_rgba8_rebind_transaction.failed = true;
            } else {
                g_rgba8_publication_disabled.store(
                    true, std::memory_order_release);
            }
            log_failure(
                "dynamic-Shape RGBA8 bind failed its exact presentation/ownership gate; later custom icons use the proven vector fallback");
        }
        if (event <= kMaximumImageBindLogs) {
            log_line(
                "iconBind[%u] result=%u target=%p resource=%p route=dynamic-shape-rgba8 fallback=%u",
                static_cast<unsigned>(event),
                attachment.bound ? 1U : 0U,
                scaleform_value,
                resource_value,
                attachment.vector_fallback_visible ? 1U : 0U);
        }
        return attachment.bound;
    }

    const std::uint32_t attach_attempt =
        g_shape_attach_attempt_count.fetch_add(1, std::memory_order_relaxed) +
        1;
    ShapeAttachProbeResult attachment{};
    if (exact_attachment_bridge) {
        if (pending.kind == VectorIconKind::cross &&
            kEnableBitmapDataOwnershipProbe) {
            // Capture only stable logical coordinates here. The later frame
            // reacquires a fresh parent wrapper and derives its current Movie;
            // no raw Scaleform object or movie pointer crosses frame boundaries.
            schedule_deferred_bitmapdata_constructor_probe(
                g_repaint_context.top,
                g_repaint_context.layer,
                g_repaint_context.physical);
        }
        // BitmapData creation is deliberately excluded here. Runtime tests
        // proved that even constructor-only BitmapData work performed from
        // this image-binding callback can poison Scaleform presentation for
        // the whole game. Keep this path on the proven native Shape binder.
        attachment = probe_dynamic_shape_attachment(
            attach_attempt,
            pending.kind,
            pending.transparent_vector_placeholder,
            scaleform_value,
            target_movie_root,
            target_movie_root_create_object_slot,
            original,
            resource,
            rectangle_values);
    } else if (attach_attempt <= kMaximumShapeAttachLogs) {
        log_line(
            "shapeAttach[%u] kind=%u skipped exactBridge=%u context=%u layer=%u physical=%zu invokeValidated=%u targetVtable=%p targetSupported=%u rectRead=%u ctorReady=%u memberReady=%u positionReady=%u visibleReady=%u dtorReady=%u",
            static_cast<unsigned>(attach_attempt),
            static_cast<unsigned>(pending.kind),
            exact_attachment_bridge ? 1U : 0U,
            g_repaint_context.active ? 1U : 0U,
            static_cast<unsigned>(g_repaint_context.layer),
            g_repaint_context.physical,
            g_value_invoke_probe_validated.load(
                std::memory_order_acquire) ? 1U : 0U,
            native_display_vtable,
            validated_authored_icon_target_vtable(
                native_display_vtable) ? 1U : 0U,
            rectangle_read ? 1U : 0U,
            g_construct_scaleform_value ? 1U : 0U,
            g_get_scaleform_member ? 1U : 0U,
            g_set_scaleform_position ? 1U : 0U,
            g_set_scaleform_visible ? 1U : 0U,
            g_destroy_scaleform_value ? 1U : 0U);
    }

    const bool bound = attachment.resource_consumed
        ? attachment.bound
        : original(scaleform_value, resource, rectangle);
    const std::size_t vector_host_bit =
        g_repaint_context.layer != TabLayer::none &&
            g_repaint_context.physical < active_physical_tab_count()
        ? g_repaint_context.physical +
            (g_repaint_context.layer == TabLayer::back
                ? active_physical_tab_count()
                : 0)
        : invalid_tab_index;
    const TabHostMask vector_host_mask =
        vector_host_bit < active_tab_host_count()
        ? TabHostMask{1} << vector_host_bit
        : 0;
    const InPlaceRgba8RebindTransaction& vector_transaction =
        g_in_place_rgba8_rebind_transaction;
    const bool matching_vector_transaction = vector_transaction.active &&
        g_runtime.pending.active &&
        vector_transaction.top == g_repaint_context.top &&
        vector_transaction.top == g_runtime.top &&
        vector_transaction.presentation_epoch ==
            g_runtime.presentation_epoch &&
        vector_transaction.source_window ==
            g_runtime.viewport.window_start() &&
        vector_transaction.target_window ==
            g_runtime.pending.candidate.window_start() &&
        g_runtime.pending.top == g_runtime.top &&
        g_runtime.pending.candidate.mapping_generation() ==
            vector_transaction.target_generation;
    const bool expected_fresh_vector =
        matching_vector_transaction &&
        !vector_transaction.failed &&
        vector_transaction.stage == InPlaceRgba8RebindStage::repainting &&
        (vector_transaction.expected_vector_host_mask &
            vector_host_mask) != 0;
    if (expected_fresh_vector) {
        const bool exact_vector_creation =
            (pending.kind == VectorIconKind::ellipsis ||
                pending.kind == VectorIconKind::cross) &&
            attachment.resource_consumed &&
            attachment.created_new_shape &&
            !attachment.reused_existing_shape &&
            attachment.custom_visible && attachment.cleanup_exact && bound;
        if (exact_vector_creation) {
            g_in_place_rgba8_rebind_transaction
                .completed_vector_host_mask =
                g_in_place_rgba8_rebind_transaction
                    .completed_vector_host_mask | vector_host_mask;
            g_in_place_rgba8_rebind_transaction.completed_host_mask =
                g_in_place_rgba8_rebind_transaction
                    .completed_host_mask | vector_host_mask;
        } else {
            g_in_place_rgba8_rebind_transaction.failed = true;
        }
        log_line(
            "rgba8Rebind fresh-vector layer=%u physical=%zu created=%u reused=%u visible=%u cleanup=%u bound=%u exact=%u completed=0x%04X/0x%04X",
            static_cast<unsigned>(g_repaint_context.layer),
            g_repaint_context.physical,
            attachment.created_new_shape ? 1U : 0U,
            attachment.reused_existing_shape ? 1U : 0U,
            attachment.custom_visible ? 1U : 0U,
            attachment.cleanup_exact ? 1U : 0U,
            bound ? 1U : 0U,
            exact_vector_creation ? 1U : 0U,
            static_cast<unsigned>(
                g_in_place_rgba8_rebind_transaction
                    .completed_vector_host_mask),
            static_cast<unsigned>(
                vector_transaction.expected_vector_host_mask));
    }
    if (bound && g_repaint_context.layer != TabLayer::none &&
        g_repaint_context.physical < active_physical_tab_count()) {
        const std::size_t bit = g_repaint_context.physical +
            (g_repaint_context.layer == TabLayer::back
                ? active_physical_tab_count()
                : 0);
        const TabHostMask mask = TabHostMask{1} << bit;
        const TabHostMask prior = g_rgba8_icon_hosts.load(
            std::memory_order_acquire);
        if ((prior & mask) != 0) {
            // The vector presenter may reuse our dynamic Shape. A successful
            // paint does not prove its old image fill released a RawImage
            // reference, so keep the RGBA ownership ledger until exact
            // removeChild/property cleanup (or a movie-boundary ref proof).
            log_line(
                "vector bind preserved RGBA8 ownership pending detach proof (layer=%u physical=%zu asset=%u)",
                static_cast<unsigned>(g_repaint_context.layer),
                g_repaint_context.physical,
                static_cast<unsigned>(
                    g_rgba8_host_assets[bit].load(
                        std::memory_order_acquire)));
        }
    }
    if (event <= kMaximumImageBindLogs) {
        log_line(
            "iconBind[%u] result=%u target=%p resource=%p",
            static_cast<unsigned>(event),
            bound ? 1U : 0U,
            scaleform_value,
            resource_value);
    }
    if (event == kMaximumImageBindLogs) {
        log_line("Further synthetic-icon bind events are suppressed");
    }
    return bound;
}

} // namespace native_tabs_detail
