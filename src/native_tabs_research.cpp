// Research probes (BitmapData ownership/constructor probes, Graphics carrier switch).
#include "native_tabs_internal.hpp"

namespace native_tabs_detail {

#if 0
// Retained only as research history. Never compile or call this path:
// BitmapData work from the image-binding callback corrupts Scaleform's
// renderer state even when no Bitmap is attached to the display list.
ShapeAttachProbeResult probe_dynamic_bitmap_attachment(
    std::uint32_t attempt,
    void* target_value,
    void* movie_root,
    void* create_object_slot,
    ImageBindFn image_binder,
    void** resource,
    const std::array<float, 4>& rectangle) noexcept {
    ShapeAttachProbeResult result{};

    // Preserve the native ImageResource ownership contract without asking the
    // authored static Shape to accept a runtime drawing context (it cannot).
    // A short-lived dynamic Shape consumes the resource through the already
    // proven binder, but is never attached to the display list. The authored
    // GFX artwork therefore remains the visual fallback if Bitmap creation
    // fails later.
    alignas(16) std::array<std::byte, 0x38> resource_shape_wrapper{};
    bool resource_shape_constructed{};
    bool resource_shape_created{};
    unsigned long resource_shape_seh{};
    if (movie_root && create_object_slot && g_construct_scaleform_value) {
#if defined(_MSC_VER)
        __try {
#endif
            resource_shape_constructed = g_construct_scaleform_value(
                resource_shape_wrapper.data()) ==
                resource_shape_wrapper.data();
            if (resource_shape_constructed) {
                reinterpret_cast<MovieCreateObjectFn>(create_object_slot)(
                    movie_root,
                    resource_shape_wrapper.data() + 0x08,
                    "flash.display.Shape",
                    nullptr,
                    0);
                DisplayValueView resource_shape_view{};
                resource_shape_created = inspect_display_value(
                        resource_shape_wrapper.data(), resource_shape_view) &&
                    exact_as3_display_bridge(resource_shape_view) &&
                    resource_shape_view.native_display_vtable ==
                        g_game.base + kDynamicShapeVtable271Rva;
            }
#if defined(_MSC_VER)
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            resource_shape_seh =
                static_cast<unsigned long>(GetExceptionCode());
        }
#endif
    }

    bool fallback_bound{};
    unsigned long fallback_seh{};
    if (resource_shape_created) {
        result.resource_consumed = true;
#if defined(_MSC_VER)
        __try {
#endif
            fallback_bound = image_binder && image_binder(
                resource_shape_wrapper.data(), resource, rectangle.data());
#if defined(_MSC_VER)
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            fallback_seh = static_cast<unsigned long>(GetExceptionCode());
        }
#endif
    }
    result.bound = fallback_bound;
    if (resource_shape_seh == 0 && fallback_seh == 0) {
        destroy_local_scaleform_value(
            resource_shape_wrapper,
            resource_shape_constructed,
            "temporary resource Shape");
    }
    if (!resource_shape_created || !fallback_bound ||
        resource_shape_seh != 0 || fallback_seh != 0) {
        if (attempt <= kMaximumShapeAttachLogs) {
            log_line(
                "bitmapAttach[%u] resourceShape={created:%u seh:0x%08lX} fallback=%u fallbackSeh=0x%08lX consumed=%u final=0",
                static_cast<unsigned>(attempt),
                resource_shape_created ? 1U : 0U,
                resource_shape_seh,
                fallback_bound ? 1U : 0U,
                fallback_seh,
                result.resource_consumed ? 1U : 0U);
        }
        return result;
    }

    alignas(16) std::array<std::byte, 0x38> parent_wrapper{};
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

    // A repaint can reuse the pair already retained by the GFX parent. The
    // Bitmap owns the visual object; the explicit BitmapData property keeps
    // the backing pixels alive under the same symmetric cleanup boundary.
    alignas(16) std::array<std::byte, 0x38> existing_bitmap_wrapper{};
    alignas(16) std::array<std::byte, 0x38> existing_data_wrapper{};
    bool existing_bitmap_constructed{};
    bool existing_data_constructed{};
    if (exact_parent && g_get_scaleform_member) {
        const char* bitmap_property = kInjectedIconProperty;
        const char* data_property = kInjectedBitmapDataProperty;
#if defined(_MSC_VER)
        __try {
#endif
            existing_bitmap_constructed = g_get_scaleform_member(
                parent_wrapper.data(),
                existing_bitmap_wrapper.data(),
                &bitmap_property) == existing_bitmap_wrapper.data();
            existing_data_constructed = g_get_scaleform_member(
                parent_wrapper.data(),
                existing_data_wrapper.data(),
                &data_property) == existing_data_wrapper.data();
#if defined(_MSC_VER)
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            existing_bitmap_constructed = false;
            existing_data_constructed = false;
        }
#endif
    }
    DisplayValueView existing_bitmap_view{};
    ObjectValueView existing_data_view{};
    const bool reusable_bitmap = existing_bitmap_constructed &&
        existing_data_constructed &&
        inspect_display_value(
            existing_bitmap_wrapper.data(), existing_bitmap_view) &&
        exact_as3_display_bridge(existing_bitmap_view) &&
        inspect_object_value(existing_data_wrapper.data(), existing_data_view) &&
        exact_as3_object_bridge(existing_data_view);

    alignas(16) std::array<std::byte, 0x38> created_data_wrapper{};
    alignas(16) std::array<std::byte, 0x38> created_bitmap_wrapper{};
    bool created_data_constructed{};
    bool created_bitmap_constructed{};
    bool data_created{};
    bool pixels_written{};
    bool bitmap_created{};
    unsigned long create_seh{};

    ObjectValueView created_data_view{};
    DisplayValueView created_bitmap_view{};
    if (exact_parent && exact_target && !reusable_bitmap &&
        movie_root && create_object_slot && g_construct_scaleform_value) {
#if defined(_MSC_VER)
        __try {
#endif
            created_data_constructed = g_construct_scaleform_value(
                created_data_wrapper.data()) == created_data_wrapper.data();
            if (created_data_constructed) {
                const std::array<RawGfxValue, 4> arguments{
                    make_int_value(kProbeBitmapWidth),
                    make_int_value(kProbeBitmapHeight),
                    make_bool_value(true),
                    // Keep every untouched pixel transparent. A bad transform
                    // must remain a harmless overlay instead of covering the
                    // Configuration movie with an opaque test surface.
                    make_uint_value(0x00000000U),
                };
                reinterpret_cast<MovieCreateObjectFn>(create_object_slot)(
                    movie_root,
                    created_data_wrapper.data() + 0x08,
                    "flash.display.BitmapData",
                    arguments.data(),
                    static_cast<std::uint32_t>(arguments.size()));
                data_created = inspect_object_value(
                        created_data_wrapper.data(), created_data_view) &&
                    exact_as3_object_bridge(created_data_view);
            }

            // Draw a two-color X. Sixty-four calls are enough to prove
            // caller-supplied ARGB pixels without making the UI hook perform
            // a full image upload before the bridge itself is validated.
            pixels_written = data_created;
            for (std::int32_t y = 0;
                 pixels_written && y < kProbeBitmapHeight;
                 ++y) {
                std::array<RawGfxValue, 3> pixel_arguments{
                    make_int_value(y),
                    make_int_value(y),
                    make_uint_value(0xC042E8F5U),
                };
                pixels_written = reinterpret_cast<ValueInvokeFn>(
                    created_data_view.invoke_slot)(
                    created_data_view.object_interface,
                    created_data_view.object_payload,
                    nullptr,
                    "setPixel32",
                    pixel_arguments.data(),
                    pixel_arguments.size(),
                    false);
                const std::int32_t opposite =
                    kProbeBitmapWidth - 1 - y;
                if (pixels_written && opposite != y) {
                    pixel_arguments[0] = make_int_value(opposite);
                    pixel_arguments[2] = make_uint_value(0xC0FFC857U);
                    pixels_written = reinterpret_cast<ValueInvokeFn>(
                        created_data_view.invoke_slot)(
                        created_data_view.object_interface,
                        created_data_view.object_payload,
                        nullptr,
                        "setPixel32",
                        pixel_arguments.data(),
                        pixel_arguments.size(),
                        false);
                }
            }

            if (pixels_written) {
                created_bitmap_constructed = g_construct_scaleform_value(
                    created_bitmap_wrapper.data()) ==
                    created_bitmap_wrapper.data();
            }
            if (created_bitmap_constructed) {
                reinterpret_cast<MovieCreateObjectFn>(create_object_slot)(
                    movie_root,
                    created_bitmap_wrapper.data() + 0x08,
                    "flash.display.Bitmap",
                    created_data_wrapper.data() + 0x08,
                    1);
                bitmap_created = inspect_display_value(
                        created_bitmap_wrapper.data(), created_bitmap_view) &&
                    exact_as3_display_bridge(created_bitmap_view);
            }
#if defined(_MSC_VER)
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            create_seh = static_cast<unsigned long>(GetExceptionCode());
        }
#endif
    }

    void* const bitmap_wrapper = reusable_bitmap
        ? static_cast<void*>(existing_bitmap_wrapper.data())
        : static_cast<void*>(created_bitmap_wrapper.data());
    void* const data_wrapper = reusable_bitmap
        ? static_cast<void*>(existing_data_wrapper.data())
        : static_cast<void*>(created_data_wrapper.data());
    const DisplayValueView& bitmap_view = reusable_bitmap
        ? existing_bitmap_view
        : created_bitmap_view;
    const bool exact_bitmap = reusable_bitmap ||
        (create_seh == 0 && data_created && pixels_written && bitmap_created);

    bool natural_size_used{};
    bool positioned{};
    unsigned long presentation_seh{};
    if (exact_bitmap && rectangle[2] > rectangle[0] &&
        rectangle[3] > rectangle[1]) {
#if defined(_MSC_VER)
        __try {
#endif
            // The previous probe assigned ActionScript width/height before
            // attachment and produced a full-screen opaque surface. Isolate
            // Bitmap rendering first: retain its natural 32x32 dimensions and
            // use only the already-proven native x/y DisplayInfo update.
            natural_size_used = true;
            positioned = g_set_scaleform_position &&
                g_set_scaleform_position(
                    bitmap_wrapper, rectangle[0], rectangle[1]) ==
                    bitmap_wrapper;
#if defined(_MSC_VER)
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            presentation_seh =
                static_cast<unsigned long>(GetExceptionCode());
        }
#endif
    }

    bool bitmap_property_set{};
    bool data_property_set{};
    bool authored_property_set{};
    bool add_child_attempted{};
    bool attached{};
    bool authored_hidden{};
    unsigned long attach_seh{};
    if (positioned && exact_parent && exact_target) {
#if defined(_MSC_VER)
        __try {
#endif
            const auto set_member = reinterpret_cast<ValueSetMemberFn>(
                parent_view.set_member_slot);
            bitmap_property_set = set_member(
                parent_view.object_interface,
                parent_view.object_payload,
                kInjectedIconProperty,
                static_cast<const std::byte*>(bitmap_wrapper) + 0x08,
                true);
            data_property_set = bitmap_property_set && set_member(
                parent_view.object_interface,
                parent_view.object_payload,
                kInjectedBitmapDataProperty,
                static_cast<const std::byte*>(data_wrapper) + 0x08,
                true);
            authored_property_set = data_property_set && set_member(
                parent_view.object_interface,
                parent_view.object_payload,
                kAuthoredIconProperty,
                static_cast<const std::byte*>(target_value) + 0x08,
                true);
            if (authored_property_set) {
                add_child_attempted = true;
                attached = reinterpret_cast<ValueInvokeFn>(
                    parent_view.invoke_slot)(
                    parent_view.object_interface,
                    parent_view.object_payload,
                    nullptr,
                    "addChild",
                    static_cast<const std::byte*>(bitmap_wrapper) + 0x08,
                    1,
                    true);
            }
            // Keep the authored icon visible during this isolation run. The
            // transparent Bitmap is intentionally an overlay, so even an
            // unexpected transform cannot remove the known-good fallback.
            authored_hidden = false;
#if defined(_MSC_VER)
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            attach_seh = static_cast<unsigned long>(GetExceptionCode());
        }
#endif
    }
    const bool custom_attached = attach_seh == 0 && attached;

    bool rollback_detached{};
    bool rollback_hidden{};
    bool rollback_visible{};
    bool properties_cleared{};
    bool rollback_retained{};
    unsigned long rollback_seh{};
    alignas(16) std::array<std::byte, 0x38> undefined_wrapper{};
    bool undefined_constructed{};
    if (!custom_attached &&
        (bitmap_property_set || data_property_set || authored_property_set)) {
        const bool attachment_uncertain =
            attach_seh != 0 && add_child_attempted;
        // Assume a display-list reference must be retained until a normal
        // removeChild return proves otherwise. This remains true even if the
        // rollback call itself raises a native exception.
        rollback_retained = attached || attachment_uncertain;
#if defined(_MSC_VER)
        __try {
#endif
            if (attached || attachment_uncertain) {
                rollback_detached = reinterpret_cast<ValueInvokeFn>(
                    parent_view.invoke_slot)(
                    parent_view.object_interface,
                    parent_view.object_payload,
                    nullptr,
                    "removeChild",
                    static_cast<const std::byte*>(bitmap_wrapper) + 0x08,
                    1,
                    true);
            } else {
                rollback_detached = true;
            }
            if (rollback_detached) rollback_retained = false;
            if (!rollback_detached && g_set_scaleform_visible) {
                rollback_hidden = g_set_scaleform_visible(
                    bitmap_wrapper, 0) == bitmap_wrapper;
            }
            rollback_visible = !authored_hidden ||
                (g_set_scaleform_visible &&
                    g_set_scaleform_visible(target_value, 1) == target_value);
            undefined_constructed = rollback_detached &&
                g_construct_scaleform_value &&
                g_construct_scaleform_value(undefined_wrapper.data()) ==
                    undefined_wrapper.data();
            if (undefined_constructed) {
                const auto set_member = reinterpret_cast<ValueSetMemberFn>(
                    parent_view.set_member_slot);
                const bool bitmap_cleared = set_member(
                    parent_view.object_interface,
                    parent_view.object_payload,
                    kInjectedIconProperty,
                    undefined_wrapper.data() + 0x08,
                    true);
                const bool data_cleared = set_member(
                    parent_view.object_interface,
                    parent_view.object_payload,
                    kInjectedBitmapDataProperty,
                    undefined_wrapper.data() + 0x08,
                    true);
                const bool authored_cleared = set_member(
                    parent_view.object_interface,
                    parent_view.object_payload,
                    kAuthoredIconProperty,
                    undefined_wrapper.data() + 0x08,
                    true);
                properties_cleared = bitmap_cleared && data_cleared &&
                    authored_cleared;
            }
            // If removal failed or an Invoke fault made attachment state
            // uncertain, keep every parent-owned reference and track this
            // cell. A later viewport cleanup can safely retry removeChild.
            rollback_retained = !rollback_detached;
#if defined(_MSC_VER)
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            rollback_seh = static_cast<unsigned long>(GetExceptionCode());
        }
#endif
    }

    if ((custom_attached || rollback_retained) &&
        g_repaint_context.active &&
        g_repaint_context.top == g_runtime.top &&
        g_repaint_context.physical < active_physical_tab_count() &&
        g_repaint_context.layer != TabLayer::none) {
        const std::size_t bit_index = g_repaint_context.physical +
            (g_repaint_context.layer == TabLayer::back
                ? active_physical_tab_count()
                : 0);
        g_injected_icon_hosts.fetch_or(
            TabHostMask{1} << bit_index,
            std::memory_order_acq_rel);
    }

    if (attempt <= kMaximumShapeAttachLogs) {
        log_line(
            "bitmapAttach[%u] context=%u layer=%u physical=%zu fallback=%u reused=%u dataCreated=%u pixelsWritten=%u bitmapCreated=%u bitmapFlags=0x%08X bitmapNativeVtable=%p natural=%dx%d sizePropertiesWritten=0 positioned=%u properties={%u,%u,%u} attached=%u hideAttempted=0 hidden=%u custom=%u",
            static_cast<unsigned>(attempt),
            g_repaint_context.active ? 1U : 0U,
            static_cast<unsigned>(g_repaint_context.layer),
            g_repaint_context.physical,
            fallback_bound ? 1U : 0U,
            reusable_bitmap ? 1U : 0U,
            data_created ? 1U : 0U,
            pixels_written ? 1U : 0U,
            bitmap_created ? 1U : 0U,
            static_cast<unsigned>(bitmap_view.flags),
            bitmap_view.native_display_vtable,
            natural_size_used ? kProbeBitmapWidth : 0,
            natural_size_used ? kProbeBitmapHeight : 0,
            positioned ? 1U : 0U,
            bitmap_property_set ? 1U : 0U,
            data_property_set ? 1U : 0U,
            authored_property_set ? 1U : 0U,
            attached ? 1U : 0U,
            authored_hidden ? 1U : 0U,
            custom_attached ? 1U : 0U);
        log_line(
            "bitmapAttach[%u] seh={fallback:0x%08lX parent:0x%08lX create:0x%08lX presentation:0x%08lX attach:0x%08lX rollback:0x%08lX} rollback={detached:%u hidden:%u visible:%u cleared:%u retained:%u} final=%u",
            static_cast<unsigned>(attempt),
            fallback_seh,
            parent_seh,
            create_seh,
            presentation_seh,
            attach_seh,
            rollback_seh,
            rollback_detached ? 1U : 0U,
            rollback_hidden ? 1U : 0U,
            rollback_visible ? 1U : 0U,
            properties_cleared ? 1U : 0U,
            rollback_retained ? 1U : 0U,
            result.bound ? 1U : 0U);
    }

    const bool cleanup_safe = fallback_seh == 0 && parent_seh == 0 &&
        create_seh == 0 && presentation_seh == 0 && attach_seh == 0 &&
        rollback_seh == 0;
    if (cleanup_safe) {
        destroy_local_scaleform_value(
            undefined_wrapper,
            undefined_constructed,
            "bitmap rollback undefined value");
        destroy_local_scaleform_value(
            created_bitmap_wrapper,
            created_bitmap_constructed,
            "created Bitmap");
        destroy_local_scaleform_value(
            created_data_wrapper,
            created_data_constructed,
            "created BitmapData");
        destroy_local_scaleform_value(
            existing_data_wrapper,
            existing_data_constructed,
            "existing BitmapData");
        destroy_local_scaleform_value(
            existing_bitmap_wrapper,
            existing_bitmap_constructed,
            "existing Bitmap");
        destroy_local_scaleform_value(
            parent_wrapper,
            parent_constructed,
            "Bitmap parent");
    } else {
        log_failure(
            "bitmapAttach[%u] leaked temporary wrappers after a guarded native fault",
            static_cast<unsigned>(attempt));
    }
    return result;
}

#endif
bool destroy_and_free_heap_scaleform_value(
    void*& wrapper,
    bool& constructed,
    const char* label) noexcept {
    if (!wrapper) return true;
    if (constructed) {
        if (!g_destroy_scaleform_value) {
            log_failure(
                "%s cannot be released because the Scaleform destructor is unavailable",
                label ? label : "heap Scaleform value");
            return false;
        }
        unsigned long destroy_seh{};
#if defined(_MSC_VER)
        __try {
#endif
            g_destroy_scaleform_value(wrapper);
            constructed = false;
#if defined(_MSC_VER)
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            destroy_seh = static_cast<unsigned long>(GetExceptionCode());
        }
#endif
        if (destroy_seh != 0) {
            log_failure(
                "%s retained after cleanup faulted (seh=0x%08lX)",
                label ? label : "heap Scaleform value",
                destroy_seh);
            return false;
        }
    }
    if (!HeapFree(GetProcessHeap(), 0, wrapper)) {
        log_failure(
            "%s retained because HeapFree failed (error=%lu)",
            label ? label : "heap Scaleform value",
            static_cast<unsigned long>(GetLastError()));
        return false;
    }
    wrapper = nullptr;
    return true;
}

// Read-only snapshot of the VM services associated with an already validated
// Exact-build AS3 MovieRoot. The offsets come from the static boundary
// analysis shared by both audited profiles.
// No pointer is dereferenced outside guarded_read, and callers retain only the
// resulting scalar addresses for comparison on a later frame.
BitmapDataVmContextSnapshot read_bitmapdata_vm_context_snapshot(
    void* exact_movie_root) noexcept {
    BitmapDataVmContextSnapshot result{};
    void* vm{};
    result.vm_read = exact_movie_root && guarded_read(
        static_cast<const std::byte*>(exact_movie_root) + 0x88, vm);
    result.vm_address = reinterpret_cast<std::uintptr_t>(vm);
    if (!result.vm_read || !vm) return result;

    void* drawable_image_context{};
    result.drawable_image_context_read = guarded_read(
        static_cast<const std::byte*>(vm) + 0x5470,
        drawable_image_context);
    result.drawable_image_context_address =
        reinterpret_cast<std::uintptr_t>(drawable_image_context);

    void* drawable_image_provider{};
    result.drawable_image_provider_read = guarded_read(
        static_cast<const std::byte*>(vm) + 0x5478,
        drawable_image_provider);
    result.drawable_image_provider_address =
        reinterpret_cast<std::uintptr_t>(drawable_image_provider);
    return result;
}

// This property-lifecycle operation must never run from the image-binding
// callback. Its caller invokes it only after the native Configuration-panel
// frame virtual returns and after freshly validating the parent and Movie.
// It creates one transparent 2x2 BitmapData and roots it in a private property
// on that movie-owned parent. The later single-pixel experiment remains in the
// source but is disabled after it deterministically blackened the renderer.
// This creation operation deliberately performs no pixel writes,
// Bitmap construction, display-list change, explicit dispose, or property
// clear. Any uncertain native failure retains its heap wrapper for the rest of
// the process rather than risking an intrusive-list link into dead storage.
void probe_bitmapdata_movie_property_ownership(
    void* parent_wrapper,
    const DisplayValueView& parent_view,
    void* movie_root,
    void* create_object_slot,
    const DeferredBitmapDataProbe& scheduled,
    std::uint64_t frame_sequence) noexcept {
    constexpr std::int32_t diagnostic_width = 2;
    constexpr std::int32_t diagnostic_height = 2;

    void* data_wrapper = HeapAlloc(
        GetProcessHeap(), HEAP_ZERO_MEMORY, 0x40);
    void* verification_wrapper = HeapAlloc(
        GetProcessHeap(), HEAP_ZERO_MEMORY, 0x40);
    g_retained_bitmapdata_wrapper = data_wrapper;
    g_retained_bitmapdata_same_frame_wrapper = verification_wrapper;

    bool data_constructed{};
    bool data_created{};
    bool property_set{};
    bool verification_constructed{};
    bool same_frame_identity{};
    unsigned long constructor_seh{};
    unsigned long create_seh{};
    unsigned long set_member_seh{};
    unsigned long get_member_seh{};
    ObjectValueView data_view{};
    ObjectValueView verification_view{};

    if (data_wrapper && verification_wrapper && parent_wrapper &&
        exact_as3_display_bridge(parent_view) && movie_root &&
        create_object_slot && g_construct_scaleform_value &&
        g_get_scaleform_member) {
#if defined(_MSC_VER)
        __try {
#endif
            data_constructed = g_construct_scaleform_value(data_wrapper) ==
                data_wrapper;
#if defined(_MSC_VER)
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            constructor_seh = static_cast<unsigned long>(GetExceptionCode());
        }
#endif
    }

    if (data_constructed && constructor_seh == 0) {
        const std::array<RawGfxValue, 4> arguments{
            make_int_value(diagnostic_width),
            make_int_value(diagnostic_height),
            make_bool_value(true),
            make_uint_value(0x00000000U),
        };
#if defined(_MSC_VER)
        __try {
#endif
            reinterpret_cast<MovieCreateObjectFn>(create_object_slot)(
                movie_root,
                static_cast<std::byte*>(data_wrapper) + 0x08,
                "flash.display.BitmapData",
                arguments.data(),
                static_cast<std::uint32_t>(arguments.size()));
            data_created = inspect_object_value(data_wrapper, data_view) &&
                exact_as3_object_bridge(data_view);
#if defined(_MSC_VER)
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            create_seh = static_cast<unsigned long>(GetExceptionCode());
        }
#endif
    }

    if (data_created && create_seh == 0) {
#if defined(_MSC_VER)
        __try {
#endif
            property_set = reinterpret_cast<ValueSetMemberFn>(
                parent_view.set_member_slot)(
                parent_view.object_interface,
                parent_view.object_payload,
                kInjectedBitmapDataProperty,
                static_cast<const std::byte*>(data_wrapper) + 0x08,
                true);
#if defined(_MSC_VER)
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            set_member_seh = static_cast<unsigned long>(GetExceptionCode());
        }
#endif
    }

    if (property_set && set_member_seh == 0) {
        const char* property_name = kInjectedBitmapDataProperty;
#if defined(_MSC_VER)
        __try {
#endif
            void* const result = g_get_scaleform_member(
                parent_wrapper, verification_wrapper, &property_name);
            verification_constructed = result == verification_wrapper;
            same_frame_identity = verification_constructed &&
                inspect_object_value(
                    verification_wrapper, verification_view) &&
                exact_as3_object_bridge(verification_view) &&
                verification_view.object_payload == data_view.object_payload;
#if defined(_MSC_VER)
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            get_member_seh = static_cast<unsigned long>(GetExceptionCode());
        }
#endif
    }

    const bool operation_succeeded = data_created && property_set &&
        same_frame_identity && constructor_seh == 0 && create_seh == 0 &&
        set_member_seh == 0 && get_member_seh == 0;
    log_line(
        "bitmapDataProperty create phase=post-native-composite frame=%llu requested=2x2 wrapper={value:%p aligned16:%u constructed:%u created:%u payload:%p} setMember=%u sameFrame={wrapper:%p constructed:%u payload:%p identity:%u} seh={constructor:0x%08lX create:0x%08lX set:0x%08lX get:0x%08lX} operationSucceeded=%u",
        static_cast<unsigned long long>(frame_sequence),
        data_wrapper,
        data_wrapper &&
                (reinterpret_cast<std::uintptr_t>(data_wrapper) & 0xFU) == 0
            ? 1U
            : 0U,
        data_constructed ? 1U : 0U,
        data_created ? 1U : 0U,
        data_view.object_payload,
        property_set ? 1U : 0U,
        verification_wrapper,
        verification_constructed ? 1U : 0U,
        verification_view.object_payload,
        same_frame_identity ? 1U : 0U,
        constructor_seh,
        create_seh,
        set_member_seh,
        get_member_seh,
        operation_succeeded ? 1U : 0U);

    if (!operation_succeeded) {
        log_failure(
            "BitmapData movie-property operation failed closed; heap wrappers and any established parent property were retained");
        return;
    }

    const BitmapDataVmContextSnapshot vm_context =
        read_bitmapdata_vm_context_snapshot(movie_root);
    log_line(
        "bitmapDataVmContext creation frame=%llu movieRoot=%p vm={read:%u address:%p} drawableImageContext={read:%u address:%p} drawableImageProvider={read:%u address:%p} passiveOnly=1",
        static_cast<unsigned long long>(frame_sequence),
        movie_root,
        vm_context.vm_read ? 1U : 0U,
        reinterpret_cast<void*>(vm_context.vm_address),
        vm_context.drawable_image_context_read ? 1U : 0U,
        reinterpret_cast<void*>(
            vm_context.drawable_image_context_address),
        vm_context.drawable_image_provider_read ? 1U : 0U,
        reinterpret_cast<void*>(
            vm_context.drawable_image_provider_address));

    void* const expected_payload = data_view.object_payload;
    const bool verification_released =
        destroy_and_free_heap_scaleform_value(
            g_retained_bitmapdata_same_frame_wrapper,
            verification_constructed,
            "same-frame BitmapData verification wrapper");
    bool creator_released{};
    if (verification_released) {
        creator_released = destroy_and_free_heap_scaleform_value(
            g_retained_bitmapdata_wrapper,
            data_constructed,
            "created BitmapData wrapper");
    }
    if (!verification_released || !creator_released) {
        log_failure(
            "BitmapData movie-property cleanup failed closed; next-frame verification was not armed");
        return;
    }

    g_bitmapdata_property_verification = {
        BitmapDataPropertyVerificationPhase::pre_mutation_identity,
        scheduled.top,
        scheduled.layer,
        scheduled.physical,
        scheduled.expected_logical,
        scheduled.mapping_generation,
        frame_sequence,
        frame_sequence,
        scheduled.scheduling_thread,
        expected_payload,
        vm_context.vm_address,
        vm_context.drawable_image_context_address,
        vm_context.drawable_image_provider_address,
    };
    log_line(
        "bitmapDataProperty rooted payload=%p localWrappersReleased=1 phase=%s nextFrameVerificationArmed=1 propertyCleared=0 disposed=0",
        expected_payload,
        bitmapdata_property_verification_phase_name(
            g_bitmapdata_property_verification.phase));
}

void schedule_deferred_bitmapdata_constructor_probe(
    void* top,
    TabLayer layer,
    std::size_t physical) noexcept {
    std::size_t expected_logical{invalid_tab_index};
    const bool pending_mapping = g_runtime.pending.active &&
        g_runtime.pending.top == top;
    const TabViewport& expected_viewport = pending_mapping
        ? g_runtime.pending.candidate
        : g_runtime.viewport;
    if (!top || layer == TabLayer::none ||
        physical >= active_physical_tab_count() ||
        !g_runtime.active || g_runtime.top != top ||
        !expected_viewport.logical_for_physical(
            physical, expected_logical) ||
        g_bitmapdata_constructor_probe_attempted.load(
            std::memory_order_acquire) ||
        g_deferred_bitmapdata_probe.scheduled) {
        return;
    }

    g_deferred_bitmapdata_probe = DeferredBitmapDataProbe{
        true,
        top,
        layer,
        physical,
        expected_logical,
        expected_viewport.mapping_generation(),
        g_composite_frame_sequence,
        GetCurrentThreadId(),
    };
    log_line(
        "bitmapDataDeferred scheduled source=%s top=%p layer=%u physical=%zu logical=%zu generation=%llu afterFrame=%llu thread=%lu",
        pending_mapping ? "pending-candidate" : "active-viewport",
        top,
        static_cast<unsigned>(layer),
        physical,
        expected_logical,
        static_cast<unsigned long long>(
            expected_viewport.mapping_generation()),
        static_cast<unsigned long long>(g_composite_frame_sequence),
        static_cast<unsigned long>(GetCurrentThreadId()));
}

void run_deferred_bitmapdata_constructor_probe(
    void* top,
    std::uint64_t frame_sequence) noexcept {
    if (!g_deferred_bitmapdata_probe.scheduled ||
        frame_sequence <=
            g_deferred_bitmapdata_probe.scheduled_after_frame) {
        return;
    }

    if (g_bitmapdata_constructor_probe_attempted.load(
            std::memory_order_acquire)) {
        g_deferred_bitmapdata_probe = {};
        return;
    }

    const DeferredBitmapDataProbe scheduled =
        g_deferred_bitmapdata_probe;
    const std::uint32_t execution_thread = GetCurrentThreadId();
    const bool exact_epoch = top && scheduled.top == top &&
        g_runtime.active && g_runtime.top == top && validate_top(top);
    const bool exact_thread =
        scheduled.scheduling_thread == execution_thread;
    std::size_t current_logical{invalid_tab_index};
    const bool exact_mapping = exact_epoch &&
        g_runtime.viewport.mapping_generation() ==
            scheduled.mapping_generation &&
        g_runtime.viewport.logical_for_physical(
            scheduled.physical, current_logical) &&
        current_logical == scheduled.expected_logical;
    const bool quiescent = !g_mutating_viewport &&
        !g_blocking_composite_input && !g_repaint_context.active &&
        !g_resolving_icon_alias &&
        g_pending_aliased_icon.resource == nullptr &&
        !g_runtime.pending.active;
    if (!exact_epoch || !exact_thread || !exact_mapping) {
        g_deferred_bitmapdata_probe = {};
        log_line(
            "bitmapDataDeferred canceled exactEpoch=%u exactThread=%u exactMapping=%u scheduledTop=%p currentTop=%p scheduledLogical=%zu currentLogical=%zu generation=%llu->%llu scheduledThread=%lu currentThread=%lu frame=%llu",
            exact_epoch ? 1U : 0U,
            exact_thread ? 1U : 0U,
            exact_mapping ? 1U : 0U,
            scheduled.top,
            top,
            scheduled.expected_logical,
            current_logical,
            static_cast<unsigned long long>(scheduled.mapping_generation),
            static_cast<unsigned long long>(
                g_runtime.viewport.mapping_generation()),
            static_cast<unsigned long>(scheduled.scheduling_thread),
            static_cast<unsigned long>(execution_thread),
            static_cast<unsigned long long>(frame_sequence));
        return;
    }
    constexpr std::uint64_t maximum_quiescence_wait_frames = 120;
    if (!quiescent) {
        const std::uint64_t waited =
            frame_sequence - scheduled.scheduled_after_frame;
        if (waited <= maximum_quiescence_wait_frames) return;
        g_deferred_bitmapdata_probe = {};
        log_line(
            "bitmapDataDeferred canceled after waiting %llu frames for a quiescent post-composite boundary",
            static_cast<unsigned long long>(waited));
        return;
    }

    g_deferred_bitmapdata_probe = {};

    ResolvedTabProxy parent_proxy{};
    if (!resolve_injected_icon_parent(
            top, scheduled.layer, scheduled.physical, parent_proxy)) {
        log_line(
            "bitmapDataDeferred canceled because the live icon parent could not be reacquired");
        return;
    }

    void* const parent_wrapper =
        parent_proxy.storage.data() + kSceneObjProxyValueOffset;
    DisplayValueView parent_view{};
    const bool exact_parent = inspect_display_value(
            parent_wrapper, parent_view) &&
        exact_as3_display_bridge(parent_view);

    void* get_movie_slot{};
    if (exact_parent) {
        (void)guarded_read(
            static_cast<const std::byte*>(
                parent_view.object_interface_vtable) + 0x178,
            get_movie_slot);
    }

    void* public_movie{};
    unsigned long get_movie_seh{};
    const bool exact_get_movie = exact_parent &&
        get_movie_slot == g_game.base + kAs3ValueGetMovie271Rva;
    if (exact_get_movie) {
#if defined(_MSC_VER)
        __try {
#endif
            public_movie = reinterpret_cast<ValueGetMovieFn>(get_movie_slot)(
                parent_view.object_interface);
#if defined(_MSC_VER)
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            get_movie_seh = static_cast<unsigned long>(GetExceptionCode());
        }
#endif
    }

    // GetMovie is present at the expected AS3 interface slot, but Elden Ring's
    // resolved display values return null from it. Reacquire the same validated
    // owner link used by the native image presenter instead. This traversal is
    // performed from a fresh wrapper in this frame; none of these pointers is
    // retained after the synchronous probe call.
    void* owner_link{};
    void* owner_movie{};
    const bool owner_link_read = exact_parent && guarded_read(
        static_cast<const std::byte*>(parent_view.object_interface) + 0x08,
        owner_link);
    const bool owner_movie_read = owner_link_read && owner_link && guarded_read(
        static_cast<const std::byte*>(owner_link) + 0x18,
        owner_movie);
    void* const movie_root = public_movie ? public_movie : owner_movie;

    void* movie_vtable{};
    void* create_object_slot{};
    const bool movie_vtable_read = movie_root &&
        guarded_read(movie_root, movie_vtable);
    const bool create_object_read = movie_vtable_read &&
        guarded_read(
            static_cast<const std::byte*>(movie_vtable) + 0x170,
            create_object_slot);
    const bool exact_movie = get_movie_seh == 0 &&
        movie_vtable_read && create_object_read &&
        movie_vtable == g_game.base + kAs3MovieVtable271Rva &&
        create_object_slot ==
            g_game.base + kAs3MovieCreateObject271Rva;

    log_line(
        "bitmapDataDeferred execute phase=post-native-composite frame=%llu waited=%llu layer=%u physical=%zu exactParent=%u exactGetMovie=%u publicMovie=%p ownerLink={read:%u value:%p} ownerMovie={read:%u value:%p} selectedMovie=%p movieVtable=%p createObject=%p exactMovie=%u seh=0x%08lX thread=%lu",
        static_cast<unsigned long long>(frame_sequence),
        static_cast<unsigned long long>(
            frame_sequence - scheduled.scheduled_after_frame),
        static_cast<unsigned>(scheduled.layer),
        scheduled.physical,
        exact_parent ? 1U : 0U,
        exact_get_movie ? 1U : 0U,
        public_movie,
        owner_link_read ? 1U : 0U,
        owner_link,
        owner_movie_read ? 1U : 0U,
        owner_movie,
        movie_root,
        movie_vtable,
        create_object_slot,
        exact_movie ? 1U : 0U,
        get_movie_seh,
        static_cast<unsigned long>(execution_thread));

    if (exact_movie &&
        !g_bitmapdata_constructor_probe_attempted.exchange(
            true, std::memory_order_acq_rel)) {
        probe_bitmapdata_movie_property_ownership(
            parent_wrapper,
            parent_view,
            movie_root,
            create_object_slot,
            scheduled,
            frame_sequence);
    }
    destroy_proxy(parent_proxy);
}

void run_bitmapdata_property_survival_verification(
    void* top,
    std::uint64_t frame_sequence) noexcept {
    const BitmapDataPropertyVerificationPhase phase =
        g_bitmapdata_property_verification.phase;
    if (phase == BitmapDataPropertyVerificationPhase::none ||
        frame_sequence <=
            g_bitmapdata_property_verification.phase_armed_frame) {
        return;
    }

    const BitmapDataPropertyVerification pending =
        g_bitmapdata_property_verification;
    const bool known_phase =
        phase ==
            BitmapDataPropertyVerificationPhase::pre_mutation_identity ||
        phase ==
            BitmapDataPropertyVerificationPhase::post_mutation_identity;
    if (!known_phase) {
        g_bitmapdata_property_verification = {};
        log_failure(
            "BitmapData property verification discarded an unknown phase=%u",
            static_cast<unsigned>(phase));
        return;
    }
    const std::uint32_t execution_thread = GetCurrentThreadId();
    const bool exact_epoch = top && pending.top == top &&
        g_runtime.active && g_runtime.top == top && validate_top(top);
    const bool exact_thread =
        pending.creation_thread == execution_thread;
    std::size_t current_logical{invalid_tab_index};
    const bool exact_mapping = exact_epoch &&
        g_runtime.viewport.mapping_generation() ==
            pending.mapping_generation &&
        g_runtime.viewport.logical_for_physical(
            pending.physical, current_logical) &&
        current_logical == pending.expected_logical;
    const bool quiescent = !g_mutating_viewport &&
        !g_blocking_composite_input && !g_repaint_context.active &&
        !g_resolving_icon_alias &&
        g_pending_aliased_icon.resource == nullptr &&
        !g_runtime.pending.active;

    if (!exact_epoch || !exact_thread || !exact_mapping) {
        g_bitmapdata_property_verification = {};
        log_line(
            "bitmapDataProperty verification canceled phase=%s exactEpoch=%u exactThread=%u exactMapping=%u scheduledTop=%p currentTop=%p scheduledLogical=%zu currentLogical=%zu generation=%llu->%llu creationThread=%lu currentThread=%lu frame=%llu",
            bitmapdata_property_verification_phase_name(phase),
            exact_epoch ? 1U : 0U,
            exact_thread ? 1U : 0U,
            exact_mapping ? 1U : 0U,
            pending.top,
            top,
            pending.expected_logical,
            current_logical,
            static_cast<unsigned long long>(pending.mapping_generation),
            static_cast<unsigned long long>(
                g_runtime.viewport.mapping_generation()),
            static_cast<unsigned long>(pending.creation_thread),
            static_cast<unsigned long>(execution_thread),
            static_cast<unsigned long long>(frame_sequence));
        return;
    }
    constexpr std::uint64_t maximum_quiescence_wait_frames = 120;
    if (!quiescent) {
        const std::uint64_t waited =
            frame_sequence - pending.phase_armed_frame;
        if (waited <= maximum_quiescence_wait_frames) return;
        g_bitmapdata_property_verification = {};
        log_line(
            "bitmapDataProperty verification canceled phase=%s after waiting %llu frames for a quiescent post-composite boundary",
            bitmapdata_property_verification_phase_name(phase),
            static_cast<unsigned long long>(waited));
        return;
    }

    // Consume this one-shot verification before crossing any native boundary.
    // An exception or mismatch must never trigger a retry against uncertain AS
    // ownership state.
    g_bitmapdata_property_verification = {};

    ResolvedTabProxy parent_proxy{};
    if (!resolve_injected_icon_parent(
            top, pending.layer, pending.physical, parent_proxy)) {
        log_failure(
            "BitmapData property verification phase=%s could not reacquire the movie-owned parent",
            bitmapdata_property_verification_phase_name(phase));
        return;
    }

    void* const parent_wrapper =
        parent_proxy.storage.data() + kSceneObjProxyValueOffset;
    DisplayValueView parent_view{};
    const bool exact_parent = inspect_display_value(
            parent_wrapper, parent_view) &&
        exact_as3_display_bridge(parent_view);

    // Reacquire the MovieRoot exclusively through the validated owner chain.
    // This is a passive diagnostic: failed reads do not alter the ownership
    // verification result or the wrapper cleanup path below.
    void* verification_owner_link{};
    void* verification_movie_root{};
    void* verification_movie_vtable{};
    const bool verification_owner_link_read = exact_parent && guarded_read(
        static_cast<const std::byte*>(parent_view.object_interface) + 0x08,
        verification_owner_link);
    const bool verification_movie_root_read =
        verification_owner_link_read && verification_owner_link &&
        guarded_read(
            static_cast<const std::byte*>(verification_owner_link) + 0x18,
            verification_movie_root);
    const bool verification_movie_vtable_read =
        verification_movie_root_read && verification_movie_root &&
        guarded_read(verification_movie_root, verification_movie_vtable);
    const bool exact_verification_movie = verification_movie_vtable_read &&
        verification_movie_vtable ==
            g_game.base + kAs3MovieVtable271Rva;
    const BitmapDataVmContextSnapshot current_vm_context =
        exact_verification_movie
        ? read_bitmapdata_vm_context_snapshot(verification_movie_root)
        : BitmapDataVmContextSnapshot{};

    void* verification_wrapper{};
    if (!g_retained_bitmapdata_next_frame_wrapper) {
        verification_wrapper = HeapAlloc(
            GetProcessHeap(), HEAP_ZERO_MEMORY, 0x40);
        g_retained_bitmapdata_next_frame_wrapper = verification_wrapper;
    } else {
        log_failure(
            "BitmapData property verification phase=%s found an occupied retained-wrapper slot; no additional wrapper was allocated",
            bitmapdata_property_verification_phase_name(phase));
    }
    bool verification_constructed{};
    bool survived{};
    unsigned long get_member_seh{};
    ObjectValueView verification_view{};
    if (exact_parent && verification_wrapper && g_get_scaleform_member) {
        const char* property_name = kInjectedBitmapDataProperty;
#if defined(_MSC_VER)
        __try {
#endif
            void* const result = g_get_scaleform_member(
                parent_wrapper, verification_wrapper, &property_name);
            verification_constructed = result == verification_wrapper;
            survived = verification_constructed &&
                inspect_object_value(
                    verification_wrapper, verification_view) &&
                exact_as3_object_bridge(verification_view) &&
                verification_view.object_payload == pending.expected_payload;
#if defined(_MSC_VER)
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            get_member_seh = static_cast<unsigned long>(GetExceptionCode());
        }
#endif
    }

    log_line(
        "bitmapDataProperty verification phase=%s frame=%llu waitedPhase=%llu age=%llu exactParent=%u wrapper=%p constructed=%u expectedPayload=%p actualPayload=%p identity=%u seh=0x%08lX propertyCleared=0 disposed=0",
        bitmapdata_property_verification_phase_name(phase),
        static_cast<unsigned long long>(frame_sequence),
        static_cast<unsigned long long>(
            frame_sequence - pending.phase_armed_frame),
        static_cast<unsigned long long>(
            frame_sequence - pending.created_frame),
        exact_parent ? 1U : 0U,
        verification_wrapper,
        verification_constructed ? 1U : 0U,
        pending.expected_payload,
        verification_view.object_payload,
        survived ? 1U : 0U,
        get_member_seh);

    if (!survived || get_member_seh != 0) {
        log_failure(
            "BitmapData property verification phase=%s failed closed; its heap wrapper was retained",
            bitmapdata_property_verification_phase_name(phase));
        destroy_proxy(parent_proxy);
        return;
    }

    if (phase ==
        BitmapDataPropertyVerificationPhase::pre_mutation_identity) {
        if (!kEnableBitmapDataPixelMutationProbe) {
            const bool vm_changed = current_vm_context.vm_address !=
                pending.vm_address;
            const bool context_changed =
                current_vm_context.drawable_image_context_address !=
                pending.drawable_image_context_address;
            const bool provider_changed =
                current_vm_context.drawable_image_provider_address !=
                pending.drawable_image_provider_address;
            log_line(
                "bitmapDataVmContext verification frame=%llu ownerLink={read:%u address:%p} movieRoot={read:%u address:%p exact:%u} vm={read:%u initial:%p current:%p changed:%u} drawableImageContext={read:%u initial:%p current:%p changed:%u} drawableImageProvider={read:%u initial:%p current:%p changed:%u} passiveOnly=1 diagnosticFailureIsNonFatal=1",
                static_cast<unsigned long long>(frame_sequence),
                verification_owner_link_read ? 1U : 0U,
                verification_owner_link,
                verification_movie_root_read ? 1U : 0U,
                verification_movie_root,
                exact_verification_movie ? 1U : 0U,
                current_vm_context.vm_read ? 1U : 0U,
                reinterpret_cast<void*>(pending.vm_address),
                reinterpret_cast<void*>(current_vm_context.vm_address),
                vm_changed ? 1U : 0U,
                current_vm_context.drawable_image_context_read ? 1U : 0U,
                reinterpret_cast<void*>(
                    pending.drawable_image_context_address),
                reinterpret_cast<void*>(
                    current_vm_context.drawable_image_context_address),
                context_changed ? 1U : 0U,
                current_vm_context.drawable_image_provider_read ? 1U : 0U,
                reinterpret_cast<void*>(
                    pending.drawable_image_provider_address),
                reinterpret_cast<void*>(
                    current_vm_context.drawable_image_provider_address),
                provider_changed ? 1U : 0U);
            if (destroy_and_free_heap_scaleform_value(
                    g_retained_bitmapdata_next_frame_wrapper,
                    verification_constructed,
                    "ownership-only BitmapData verification wrapper")) {
                log_line(
                    "bitmapDataProperty ownershipOnlyBaselineVerified=1 verificationWrapperReleased=1 parentPropertyRetained=1 pixelMutationEnabled=0");
            }
            destroy_proxy(parent_proxy);
            return;
        }

        constexpr std::uint32_t diagnostic_argb = 0xA17BC3E5U;
        const bool mutation_guard_claimed =
            !g_bitmapdata_pixel_mutation_probe_attempted.exchange(
                true, std::memory_order_acq_rel);
        bool mutation_invoked{};
        bool mutation_succeeded{};
        unsigned long mutation_seh{};
        if (mutation_guard_claimed && verification_view.invoke_slot) {
            const std::array<RawGfxValue, 3> pixel_arguments{
                make_int_value(0),
                make_int_value(0),
                make_uint_value(diagnostic_argb),
            };
            mutation_invoked = true;
#if defined(_MSC_VER)
            __try {
#endif
                mutation_succeeded = reinterpret_cast<ValueInvokeFn>(
                    verification_view.invoke_slot)(
                    verification_view.object_interface,
                    verification_view.object_payload,
                    nullptr,
                    "setPixel32",
                    pixel_arguments.data(),
                    pixel_arguments.size(),
                    false);
#if defined(_MSC_VER)
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                mutation_seh =
                    static_cast<unsigned long>(GetExceptionCode());
            }
#endif
        }

        log_line(
            "bitmapDataProperty mutation phase=%s frame=%llu guardClaimed=%u invoked=%u method=setPixel32 arguments={x:0 y:0 argb:0x%08X} invokeResult=%u seh=0x%08lX identityBeforeMutation=1 propertyCleared=0 disposed=0",
            bitmapdata_property_verification_phase_name(phase),
            static_cast<unsigned long long>(frame_sequence),
            mutation_guard_claimed ? 1U : 0U,
            mutation_invoked ? 1U : 0U,
            static_cast<unsigned>(diagnostic_argb),
            mutation_succeeded ? 1U : 0U,
            mutation_seh);

        if (!mutation_guard_claimed || !mutation_invoked ||
            !mutation_succeeded || mutation_seh != 0) {
            log_failure(
                "BitmapData single-pixel mutation failed closed; its verified heap wrapper was retained and no post-mutation verification was armed");
            destroy_proxy(parent_proxy);
            return;
        }

        if (!destroy_and_free_heap_scaleform_value(
                g_retained_bitmapdata_next_frame_wrapper,
                verification_constructed,
                "pre-mutation BitmapData verification wrapper")) {
            log_failure(
                "BitmapData mutation succeeded but wrapper cleanup failed closed; no post-mutation verification was armed");
            destroy_proxy(parent_proxy);
            return;
        }

        BitmapDataPropertyVerification post_mutation = pending;
        post_mutation.phase =
            BitmapDataPropertyVerificationPhase::post_mutation_identity;
        post_mutation.phase_armed_frame = frame_sequence;
        g_bitmapdata_property_verification = post_mutation;
        log_line(
            "bitmapDataProperty mutationCommitted=1 verificationWrapperReleased=1 nextPhase=%s nextFrameIdentityOnlyArmed=1 expectedPayload=%p parentPropertyRetained=1",
            bitmapdata_property_verification_phase_name(
                post_mutation.phase),
            post_mutation.expected_payload);
        destroy_proxy(parent_proxy);
        return;
    }

    if (destroy_and_free_heap_scaleform_value(
            g_retained_bitmapdata_next_frame_wrapper,
            verification_constructed,
            "post-mutation BitmapData verification wrapper")) {
        log_line(
            "bitmapDataProperty postMutationSurvivalVerified=1 identityOnly=1 verificationWrapperReleased=1 parentPropertyRetained=1 pixelReadbackPerformed=0");
    }
    destroy_proxy(parent_proxy);
}

// Called synchronously by the host only after exact identity, age and native
// retirement validation. Category 5 is parking; category 8 is the carrier.
bool __cdecl switch_research_carrier_isolated(
    void* composite,
    std::int32_t category) noexcept {
    const auto original = g_original_research_switch.load(
        std::memory_order_acquire);
    if (!original || !composite || (category != 5 && category != 8) ||
        !g_research_explicit_switch || !g_runtime.pending.active ||
        static_cast<std::byte*>(g_runtime.top) + kCompositeOffset !=
            composite) return false;
    auto* const bytes = static_cast<std::byte*>(composite);
    void* source{};
    void* cached_target{};
    if (!guarded_read(bytes + kCompositeCurrentPanelOffset, source) ||
        !guarded_read(
            bytes + kCompositePanelCacheOffset +
                category * sizeof(void*),
            cached_target)) return false;
    g_state_isolation_expectation = {};
    const bool true_custom_route_involved =
        is_custom_logical(g_runtime.pending.previous_logical) ||
        is_custom_logical(g_runtime.pending.desired_logical);
    const bool native_copy_would_run = source && source != cached_target;
    const bool suppress_copy = native_copy_would_run &&
        (category == 5 || true_custom_route_involved);
    const bool armed = suppress_copy && begin_panel_state_isolation(
        g_runtime.top, category,
        PanelStateIsolationReason::corrective);
    if (suppress_copy && !armed) return false;
    original(composite, category);
    const bool copy_policy_exact = !suppress_copy ||
        (g_state_isolation_expectation.active &&
            g_state_isolation_expectation.consumed);
    end_panel_state_isolation(armed);
    void* current{};
    void* target{};
    const bool exact = copy_policy_exact &&
        guarded_read(bytes + kCompositeCurrentPanelOffset, current) &&
        guarded_read(
            bytes + kCompositePanelCacheOffset +
                category * sizeof(void*),
            target) && current && current == target;
    log_line(
        "graphicsCarrier nativeSwitch category=%d source=%p target=%p currentExact=%u stateCopyPolicy=%s customBoundary=%u frame=%llu",
        category, source, target, exact ? 1u : 0u,
        suppress_copy ? "suppressed" : "native-preserved",
        true_custom_route_involved ? 1u : 0u,
        static_cast<unsigned long long>(
            g_research_completed_composite_frames));
    return exact;
}

void __fastcall research_switch_panel_detour(
    void* composite,
    std::int32_t category) noexcept {
    const auto original = g_original_research_switch.load(
        std::memory_order_acquire);
    if (!original) return;
    if (!g_viewport_callbacks_armed.load(std::memory_order_acquire)) {
        original(composite, category);
        return;
    }
    if (g_runtime.destructive_transition_failed && !g_research_failed) {
        // The viewport lifetime guard owns no further routing. Preserve
        // Elden Ring's native switching unless an independent carrier fault
        // has made category 8 ambiguous; that stricter fault remains frozen
        // until an exact native teardown proves a recovery boundary.
        original(composite, category);
        return;
    }
    void* const top = composite
        ? static_cast<std::byte*>(composite) - kCompositeOffset
        : nullptr;
    if (g_runtime.active && g_runtime.top == top &&
        g_runtime.pending.active && !g_research_explicit_switch) {
        // The frame transaction owns the final route; suppress only the
        // provisional tab callback switch, never ordinary stock navigation.
        g_state_isolation_expectation.consumed = true;
        return;
    }
    if (category != 8) {
        original(composite, category);
        g_research_switch_succeeded = true;
        return;
    }

    const std::size_t logical = g_runtime.pending.active
        ? g_runtime.pending.desired_logical
        : g_runtime.viewport.selected_logical();
    const std::size_t physical = g_runtime.pending.active
        ? g_runtime.pending.desired_physical
        : g_runtime.viewport.selected_physical();
    if (g_research_failed || !g_runtime.active || g_runtime.top != top ||
        !logical_uses_graphics_carrier(logical) ||
        physical >= active_physical_tab_count() ||
        !g_research_explicit_switch ||
        !g_runtime.pending.active) {
        g_research_switch_succeeded = false;
        if (!g_research_failed) {
            g_research_failed = true;
            log_failure(
                "Graphics carrier switch escaped validated pending frame; custom transitions frozen");
        }
        return;
    }

    const std::size_t route = graphics_route_for_logical(logical);
    const std::uint64_t transaction_frame =
        g_research_completed_composite_frames;
    if (!g_research_prepare_carrier(
            composite, route, transaction_frame, &category,
            &switch_research_carrier_isolated)) {
        g_research_failed = true;
        g_research_switch_succeeded = false;
        log_failure(
            "Graphics carrier prepare FAILED CLOSED logical=%zu route=%zu frame=%llu",
            logical, route,
            static_cast<unsigned long long>(transaction_frame));
        return;
    }

    auto* const bytes = static_cast<std::byte*>(composite);
    const PanelSnapshot network = inspect_panel_slot(
        bytes + kCompositePanelCacheOffset + 5 * sizeof(void*));
    const PanelCarrierSnapshot network_carrier =
        inspect_panel_carrier(network);
    const bool switched = category == 8 &&
        switch_research_carrier_isolated(composite, category);
    const PanelSnapshot network_after = inspect_panel_slot(
        bytes + kCompositePanelCacheOffset + 5 * sizeof(void*));
    const PanelCarrierSnapshot network_carrier_after =
        inspect_panel_carrier(network_after);
    const bool network_exact = network.panel == network_after.panel &&
        (!network.panel ||
            (network_carrier.valid && network_carrier_after.valid &&
                network_carrier.wrapper == network_carrier_after.wrapper &&
                network_carrier.payload ==
                    network_carrier_after.payload));
    std::int32_t record_category{-1};
    const bool record_exact = guarded_read(
        record_at(top, physical) + kCategoryIdOffset,
        record_category) && record_category == category;
    if (!switched || !network_exact ||
        transaction_frame != g_research_completed_composite_frames ||
        !g_research_verify_carrier(
            composite, route, category) || !record_exact) {
        g_research_failed = true;
        g_research_switch_succeeded = false;
        log_failure(
            "Graphics carrier activation FAILED CLOSED logical=%zu route=%zu category=%d recordCategory=%d recordWrite=0",
            logical, route, category, record_category);
        return;
    }
    LogicalTab* const tab = runtime_logical_tab(logical);
    if (!tab) {
        g_research_failed = true;
        g_research_switch_succeeded = false;
        return;
    }
    tab->category = category;
    g_runtime.pending.desired_category = category;
    g_research_switch_succeeded = true;
    log_line(
        "graphicsCarrier activated logical=%zu route=%zu kind=%s category=8 frame=%llu networkPreserved=1 noInterveningFrame=1 finalCurrentCategory8=1",
        logical, route,
        route == native_graphics_route() ? "native-graphics" : "custom",
        static_cast<unsigned long long>(transaction_frame));
}

} // namespace native_tabs_detail
