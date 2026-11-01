// Native tab slot growth at Configuration construction (production only).
#include "native_tabs_internal.hpp"

namespace native_tabs_detail {

#if defined(ER_NATIVE_UI_PRODUCTION_TABS)
bool read_slot_growth_movie_fingerprint(
    const void* scene_proxy,
    SlotGrowthMovieFingerprint& fingerprint) noexcept {
    fingerprint = {};
    if (!scene_proxy) return false;

    const auto* const proxy = static_cast<const std::byte*>(scene_proxy);
    const auto* const wrapper = proxy + kSceneObjProxyValueOffset;
    void* proxy_vtable{};
    void* proxy_getter{};
    void* proxy_self0{};
    void* proxy_self1{};
    void* proxy_self2{};
    void* wrapper_vtable{};
    void* owner_link{};
    void* movie_root{};
    void* movie_vtable{};
    void* create_object{};
    DisplayValueView view{};

    if (!guarded_read(proxy, proxy_vtable) ||
        proxy_vtable != g_game.base + kSceneObjProxyVtableRva ||
        !guarded_read(
            static_cast<const std::byte*>(proxy_vtable) + sizeof(void*),
            proxy_getter) ||
        proxy_getter != g_game.base + kSceneObjProxyGetValueRva ||
        !guarded_read(proxy + 0x08, proxy_self0) ||
        !guarded_read(proxy + 0x10, proxy_self1) ||
        !guarded_read(proxy + 0x18, proxy_self2) ||
        proxy_self0 != scene_proxy || proxy_self1 != scene_proxy ||
        proxy_self2 != scene_proxy ||
        !guarded_read(proxy + 0x20, fingerprint.proxy_owner) ||
        !fingerprint.proxy_owner ||
        !guarded_read(wrapper, wrapper_vtable) ||
        wrapper_vtable != g_game.base + kSceneObjProxyValueVtableRva ||
        !inspect_display_value(wrapper, view) ||
        !exact_as3_display_bridge(view) ||
        !guarded_read(wrapper + 0x30, fingerprint.auxiliary) ||
        !guarded_read(
            static_cast<const std::byte*>(view.object_interface) + 0x08,
            owner_link) ||
        !owner_link ||
        !guarded_read(
            static_cast<const std::byte*>(owner_link) + 0x18,
            movie_root) ||
        !movie_root ||
        !guarded_read(movie_root, movie_vtable) ||
        movie_vtable != g_game.base + kAs3MovieVtable271Rva ||
        !guarded_read(
            static_cast<const std::byte*>(movie_vtable) + 0x170,
            create_object) ||
        create_object != g_game.base + kAs3MovieCreateObject271Rva) {
        fingerprint = {};
        return false;
    }

    fingerprint.object_interface = view.object_interface;
    fingerprint.flags = view.flags;
    fingerprint.object_payload = view.object_payload;
    fingerprint.owner_link = owner_link;
    fingerprint.movie_root = movie_root;
    return true;
}

bool same_slot_growth_movie(
    const SlotGrowthMovieFingerprint& left,
    const SlotGrowthMovieFingerprint& right) noexcept {
    return left.proxy_owner && left.object_interface && left.object_payload &&
        left.owner_link && left.movie_root &&
        left.proxy_owner == right.proxy_owner &&
        left.object_interface == right.object_interface &&
        left.flags == right.flags &&
        left.object_payload == right.object_payload &&
        left.auxiliary == right.auxiliary &&
        left.owner_link == right.owner_link &&
        left.movie_root == right.movie_root;
}

bool same_slot_growth_movie_lineage(
    const SlotGrowthMovieFingerprint& child,
    const SlotGrowthMovieFingerprint& root) noexcept {
    return child.object_interface && child.owner_link && child.movie_root &&
        child.object_interface == root.object_interface &&
        child.owner_link == root.owner_link &&
        child.movie_root == root.movie_root;
}

SlotGrowthCellProof probe_slot_growth_cell(
    void* movie_context,
    const char* path,
    const SlotGrowthMovieFingerprint& root,
    SlotGrowthCellStorage& storage) noexcept {
    SlotGrowthCellProof proof{};
    if (!movie_context || !path || storage.poisoned ||
        storage.proxy.constructed || !g_resolve_path || !g_value_exists ||
        !g_destroy_scaleform_value) {
        return proof;
    }

    storage.proxy.storage.fill(std::byte{});
    void* result{};
#if defined(_MSC_VER)
    __try {
#endif
        result = g_resolve_path(
            movie_context, storage.proxy.storage.data(), path);
#if defined(_MSC_VER)
    } __except (proof.seh_code = GetExceptionCode(),
               EXCEPTION_EXECUTE_HANDLER) {
        result = nullptr;
    }
#endif
    if (result != storage.proxy.storage.data() || proof.seh_code != 0) {
        // ResolvePath constructs an owning value. A mismatched return leaves
        // ownership ambiguous, so retain this process-lifetime TLS storage and
        // never attempt to destroy or reuse it.
        storage.poisoned = true;
        return proof;
    }

    storage.proxy.constructed = true;
    proof.resolver_returned_destination = true;
#if defined(_MSC_VER)
    __try {
#endif
        proof.exists = g_value_exists(storage.proxy.storage.data());
        proof.existence_call_exact = true;
#if defined(_MSC_VER)
    } __except (proof.seh_code = GetExceptionCode(),
               EXCEPTION_EXECUTE_HANDLER) {
        proof.existence_call_exact = false;
    }
#endif
    if (proof.exists && proof.seh_code == 0) {
        proof.fingerprint_exact = read_slot_growth_movie_fingerprint(
            storage.proxy.storage.data(), proof.movie);
        proof.lineage_exact = proof.fingerprint_exact &&
            same_slot_growth_movie_lineage(proof.movie, root);
    }

    proof.cleanup_exact = destroy_proxy(storage.proxy);
    if (proof.cleanup_exact) {
        storage.proxy.storage.fill(std::byte{});
    } else {
        storage.poisoned = true;
    }
    return proof;
}

bool exact_slot_growth_cell_proof(
    const SlotGrowthCellProof& proof) noexcept {
    if (!proof.resolver_returned_destination ||
        !proof.existence_call_exact || !proof.cleanup_exact ||
        proof.seh_code != 0) {
        return false;
    }
    return !proof.exists ||
        (proof.fingerprint_exact && proof.lineage_exact &&
            proof.movie.object_payload);
}

bool discover_slot_growth_target(
    void* top,
    const SlotGrowthMovieFingerprint& root,
    std::size_t configured_minimum,
    NativeTabSlotGrowthPlan& plan,
    bool& symmetric) noexcept {
    plan = {};
    symmetric = false;
    if (!top) return false;

    std::array<SlotGrowthCellProof, kSlotGrowthProbeCellCount> proofs{};
    std::size_t proof_index{};
    constexpr std::array<const char*, 2> kStripNames{
        "TabList", "BackTabList"};
    for (const char* strip : kStripNames) {
        for (std::size_t cell = 8U; cell <= 10U; ++cell) {
            char path[64]{};
            const int written = _snprintf_s(
                path,
                std::size(path),
                _TRUNCATE,
                "%s/Item_0_%zu",
                strip,
                cell);
            if (written <= 0 || proof_index >= proofs.size()) return false;
            proofs[proof_index] = probe_slot_growth_cell(
                static_cast<std::byte*>(top) + kMovieContextOffset,
                path,
                root,
                g_slot_growth_cell_storage[proof_index]);
            ++proof_index;
        }
    }

    if (proof_index != proofs.size()) return false;
    for (const SlotGrowthCellProof& proof : proofs) {
        if (!exact_slot_growth_cell_proof(proof)) return false;
    }

    const std::array<bool, 3> front{
        proofs[0].exists, proofs[1].exists, proofs[2].exists};
    const std::array<bool, 3> back{
        proofs[3].exists, proofs[4].exists, proofs[5].exists};
    symmetric = front == back;
    plan = plan_native_tab_slot_growth(front, back, configured_minimum);
    if (!plan.structure_exact()) return false;

    std::array<void*, kSlotGrowthProbeCellCount> payloads{};
    std::size_t payload_count{};
    for (const SlotGrowthCellProof& proof : proofs) {
        if (!proof.exists) continue;
        if (proof.movie.object_payload == root.object_payload) return false;
        for (std::size_t earlier = 0; earlier < payload_count; ++earlier) {
            if (payloads[earlier] == proof.movie.object_payload) return false;
        }
        payloads[payload_count++] = proof.movie.object_payload;
    }

    return plan.discovered_slot_count >= kStockLogicalTabCount &&
        plan.discovered_slot_count <= kPhysicalTabCapacity;
}

bool read_slot_growth_record(
    void* top,
    std::size_t index,
    RecordFields& fields) noexcept {
    const std::byte* const record = record_at(top, index);
    void* vtable{};
    return record && guarded_read(record, vtable) &&
        vtable == g_game.base + kCategoryRecordVtableRva &&
        guarded_read(record + kTitleOffset, fields.title) && fields.title &&
        guarded_read(record + kCategoryIdOffset, fields.category) &&
        guarded_copy(
            fields.presentation.data(),
            record + kPresentationOffset,
            fields.presentation.size());
}

bool slot_growth_records_exact(
    void* top,
    std::size_t count,
    const RecordFields* graphics_snapshot = nullptr) noexcept {
    if (!top || count < kStockLogicalTabCount ||
        count > kPhysicalTabCapacity) {
        return false;
    }
    RecordFields graphics{};
    for (std::size_t index = 0; index < count; ++index) {
        RecordFields current{};
        if (!read_slot_growth_record(top, index, current)) return false;
        const std::int32_t expected = index < kExpectedStockCategories.size()
            ? kExpectedStockCategories[index]
            : kExpectedStockCategories.back();
        if (current.category != expected) return false;
        if (index == kStockLogicalTabCount - 1U) graphics = current;
        if (index >= kStockLogicalTabCount &&
            (current.title != graphics.title ||
                current.presentation != graphics.presentation)) {
            return false;
        }
    }
    return !graphics_snapshot ||
        (graphics.title == graphics_snapshot->title &&
            graphics.category == graphics_snapshot->category &&
            graphics.presentation == graphics_snapshot->presentation);
}

bool slot_growth_geometry_supports_capacity(
    void* top,
    std::size_t required_capacity) noexcept {
    if (!top || required_capacity < kStockLogicalTabCount ||
        required_capacity > kPhysicalTabCapacity) {
        return false;
    }
    const auto* const selector =
        static_cast<const std::byte*>(top) + kSelectorOffset;
    void* selector_vtable{};
    std::int32_t horizontal{};
    std::int32_t vertical{};
    if (!guarded_read(selector, selector_vtable) ||
        selector_vtable != g_game.base + kSelectorVtableRva ||
        !guarded_read(
            selector + kSelectorHorizontalExtentOffset, horizontal) ||
        !guarded_read(
            selector + kSelectorVerticalExtentOffset, vertical) ||
        horizontal <= 0 || vertical <= 0 ||
        horizontal > 32 || vertical > 64 ||
        (horizontal != 1 && vertical != 1)) {
        return false;
    }
    // A third-party movie may author additional dormant cells beyond the
    // native ten-record ceiling. Keep them untouched and require only the
    // complete prefix needed by the capped native capacity.
    return static_cast<std::int64_t>(horizontal) * vertical >=
        static_cast<std::int64_t>(required_capacity + 1U);
}

[[noreturn]] void fail_fast_after_slot_growth(
    const char* stage,
    const SlotGrowthConstructionContext& context) noexcept {
    erui::detail::logf(
        erui::LogLevel::fatal,
        "NativeTabs slot growth lost exact state after owned append "
        "(stage=%s configuredMinimum=%zu discoveredCapacity=%zu "
        "activeTarget=%zu "
        "checkpoints=%zu); terminating before the mutated Configuration "
        "object can escape",
        stage ? stage : "unknown",
        context.configured_minimum,
        context.discovered_capacity,
        context.active_target,
        context.append_checkpoints);
    RaiseFailFastException(nullptr, nullptr, 0);
    std::terminate();
}

void append_graphics_records_at_construction_seam(
    safetyhook::Context& cpu) noexcept {
    if (!g_slot_growth_callbacks_armed.load(std::memory_order_acquire) ||
        g_inside_slot_growth_append) {
        return;
    }

    SlotGrowthConstructionContext& context = g_slot_growth_construction;
    void* const top = reinterpret_cast<void*>(cpu.rdi);
    if (!top || context.active) return;
    context = {};
    context.active = true;
    context.top = top;
    context.early_seam_seen = true;
    context.title_menu = cpu.r15 != 0;
    context.configured_minimum = g_minimum_tab_slots;
    if (context.title_menu) {
        // The same constructor also serves the title-menu options movie.
        // Physical custom tabs belong only to the in-game Configuration
        // movie, so leave that unrelated object completely untouched.
        context = {};
        return;
    }

    auto* const storage = static_cast<std::byte*>(top) +
        kCategoryStorageOffset;
    auto* const list = static_cast<std::byte*>(top) + kCategoryListOffset;
    auto* const selector = static_cast<std::byte*>(top) + kSelectorOffset;
    auto* const composite = static_cast<std::byte*>(top) + kCompositeOffset;
    auto* const controller = static_cast<std::byte*>(top) +
        kTopControllerOffset;
    std::byte* const graphics = record_at(
        top, kStockLogicalTabCount - 1U);

    void* scene_proxy{};
    void* frame_top{};
    void* top_vtable{};
    void* list_vtable{};
    void* selector_vtable{};
    void* geometry_enumerator{};
    SlotGrowthMovieFingerprint caller_movie{};
    SlotGrowthMovieFingerprint top_movie{};
    RecordFields graphics_snapshot{};
    const bool scene_proxy_read = guarded_read(
        reinterpret_cast<const void*>(cpu.rsp + 0x50), scene_proxy);
    const bool caller_movie_exact = scene_proxy_read &&
        read_slot_growth_movie_fingerprint(scene_proxy, caller_movie);
    const bool top_movie_exact = read_slot_growth_movie_fingerprint(
        static_cast<std::byte*>(top) + kMovieContextOffset, top_movie);
    const bool movie_exact = caller_movie_exact && top_movie_exact &&
        same_slot_growth_movie(caller_movie, top_movie);
    const bool frame_top_read = cpu.rbp >= 0x58 && guarded_read(
        reinterpret_cast<const void*>(cpu.rbp - 0x58), frame_top);
    const bool register_frame_exact =
        cpu.rbx == reinterpret_cast<std::uintptr_t>(storage) &&
        cpu.r13 == reinterpret_cast<std::uintptr_t>(list) &&
        cpu.r14 == reinterpret_cast<std::uintptr_t>(selector) &&
        cpu.r12 == reinterpret_cast<std::uintptr_t>(composite) &&
        cpu.rsi == reinterpret_cast<std::uintptr_t>(controller) &&
        cpu.r15 == 0 && frame_top_read && frame_top == top;
    const bool identities_exact =
        guarded_read(top, top_vtable) &&
        top_vtable == g_game.base + kTopVtableRva &&
        guarded_read(list, list_vtable) &&
        list_vtable == g_game.base + kCategoryListVtableRva &&
        guarded_read(selector, selector_vtable) &&
        selector_vtable == g_game.base + kSelectorVtableRva &&
        guarded_read(
            static_cast<const std::byte*>(selector_vtable) +
                3U * sizeof(void*),
            geometry_enumerator) &&
        geometry_enumerator ==
            g_game.base + kSelectorGeometryEnumeratorRva;
    const bool count_exact = guarded_read(
        static_cast<std::byte*>(top) + kCategoryCountOffset,
        context.count_before) &&
        context.count_before == kStockLogicalTabCount;
    const bool active_readable = guarded_read(
        selector + kSelectorActiveCountOffset,
        context.active_before);
    const bool stock_exact = slot_growth_records_exact(
        top, kStockLogicalTabCount);
    const bool snapshot_exact = read_slot_growth_record(
        top, kStockLogicalTabCount - 1U, graphics_snapshot);
    constexpr std::array<std::uint8_t, 16> kCategoryAppendEntry{
        0x40, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x48, 0xC7,
        0x44, 0x24, 0x20, 0xFE, 0xFF, 0xFF, 0xFF, 0x48};
    const bool append_pristine = entry_matches(
        kCategoryAppendRva, kCategoryAppendEntry);
    bool symmetric{};
    NativeTabSlotGrowthPlan plan{};
    const bool cells_exact = movie_exact && discover_slot_growth_target(
        top,
        caller_movie,
        context.configured_minimum,
        plan,
        symmetric);
    context.scene_proxy = scene_proxy;
    context.movie = caller_movie;
    context.discovered_capacity = plan.discovered_slot_count;
    context.active_target = native_tab_active_slot_target(
        context.discovered_capacity, active_logical_tab_count());
    context.live_cells_symmetric = symmetric;

#if defined(ER_NATIVE_UI_PRODUCTION_TABS)
    // Fork: upstream steps aside whenever NativeTweaks.dll is loaded. In this
    // fork the competing constructor path is its own category-4 tab, which is
    // installed only when Mod Settings is not a custom tab; stepping aside
    // otherwise left nine logical tabs on eight physical slots (r135 test).
    const bool competing_native_tweaks =
        erui::native::mod_settings_category_append_installed();
#else
    const bool competing_native_tweaks =
        GetModuleHandleW(L"NativeTweaks.dll") != nullptr;
#endif
    context.early_proof_exact = !competing_native_tweaks &&
        g_category_append && register_frame_exact && movie_exact &&
        identities_exact && count_exact && active_readable && stock_exact &&
        snapshot_exact && append_pristine && cells_exact;
    if (!context.early_proof_exact) {
        if (competing_native_tweaks) {
            if (context.configured_minimum > kStockLogicalTabCount) {
                log_line(
                    "WARNING: physical tab growth was left to NativeTweaks.dll because its competing constructor mutation path is loaded (configuredMinimum=%zu authoredCapacity=%zu)",
                    context.configured_minimum,
                    context.discovered_capacity);
            }
        } else {
            log_line(
                "ERROR: physical tab growth refused before mutation: exact same-construction proof failed (configuredMinimum=%zu discovered=%zu symmetric=%u)",
                context.configured_minimum,
                context.discovered_capacity,
                context.live_cells_symmetric ? 1U : 0U);
        }
        return;
    }
    if (!plan.minimum_satisfied()) {
        log_line(
            "ERROR: physical tab growth refused before mutation: current GFX exposes %zu compatible slots, below configured MinimumTabSlots=%zu",
            context.discovered_capacity,
            context.configured_minimum);
        return;
    }
    if (context.active_target < kStockLogicalTabCount ||
        context.active_target > context.discovered_capacity) {
        log_line(
            "ERROR: physical tab growth refused before mutation: invalid active target %zu for authored capacity %zu and logical count %zu",
            context.active_target,
            context.discovered_capacity,
            active_logical_tab_count());
        return;
    }
    if (context.active_target == kStockLogicalTabCount) return;

    context.mutation_attempted = true;
    for (std::size_t expected_count = kStockLogicalTabCount + 1U;
         expected_count <= context.active_target;
         ++expected_count) {
        g_inside_slot_growth_append = true;
        // The helper increments the owned-vector count before copy
        // construction. Do not catch native faults: no safe rollback exists
        // for a partially constructed counted record.
        g_category_append(storage, graphics);
        g_inside_slot_growth_append = false;

        std::uint64_t count_after{};
        std::int32_t active_after{-1};
        const bool checkpoint_exact = guarded_read(
                static_cast<std::byte*>(top) + kCategoryCountOffset,
                count_after) &&
            count_after == expected_count &&
            guarded_read(
                selector + kSelectorActiveCountOffset, active_after) &&
            active_after == context.active_before &&
            slot_growth_records_exact(
                top, expected_count, &graphics_snapshot);
        if (!checkpoint_exact) {
            fail_fast_after_slot_growth("append-checkpoint", context);
        }
        ++context.append_checkpoints;
    }
    context.mutation_committed =
        context.append_checkpoints ==
            context.active_target - kStockLogicalTabCount &&
        slot_growth_records_exact(
            top, context.active_target, &graphics_snapshot);
    if (!context.mutation_committed) {
        fail_fast_after_slot_growth("append-commit", context);
    }
}

void observe_slot_growth_post_geometry(safetyhook::Context& cpu) noexcept {
    if (!g_slot_growth_callbacks_armed.load(std::memory_order_acquire) ||
        g_inside_slot_growth_append) {
        return;
    }
    SlotGrowthConstructionContext& context = g_slot_growth_construction;
    void* const top = reinterpret_cast<void*>(cpu.rdi);
    if (!context.active || !context.mutation_attempted ||
        context.top != top || context.late_seam_seen) {
        return;
    }
    context.late_seam_seen = true;

    void* scene_proxy{};
    void* frame_top{};
    SlotGrowthMovieFingerprint caller_movie{};
    SlotGrowthMovieFingerprint top_movie{};
    std::uint64_t count{};
    std::int32_t active{-1};
    const bool exact = guarded_read(
            reinterpret_cast<const void*>(cpu.rsp + 0x50), scene_proxy) &&
        scene_proxy == context.scene_proxy &&
        read_slot_growth_movie_fingerprint(scene_proxy, caller_movie) &&
        read_slot_growth_movie_fingerprint(
            static_cast<std::byte*>(top) + kMovieContextOffset, top_movie) &&
        same_slot_growth_movie(caller_movie, context.movie) &&
        same_slot_growth_movie(top_movie, context.movie) &&
        cpu.rbp >= 0x58 && guarded_read(
            reinterpret_cast<const void*>(cpu.rbp - 0x58), frame_top) &&
        frame_top == top &&
        cpu.r13 == reinterpret_cast<std::uintptr_t>(
            static_cast<std::byte*>(top) + kCategoryListOffset) &&
        cpu.r14 == reinterpret_cast<std::uintptr_t>(
            static_cast<std::byte*>(top) + kSelectorOffset) &&
        cpu.r12 == reinterpret_cast<std::uintptr_t>(
            static_cast<std::byte*>(top) + kCompositeOffset) &&
        cpu.rsi == reinterpret_cast<std::uintptr_t>(
            static_cast<std::byte*>(top) + kTopControllerOffset) &&
        cpu.r15 == 0 &&
        guarded_read(
            static_cast<std::byte*>(top) + kCategoryCountOffset, count) &&
        count == context.active_target &&
        guarded_read(
            static_cast<std::byte*>(top) + kSelectorOffset +
                kSelectorActiveCountOffset,
            active) &&
        active == context.active_before &&
        slot_growth_records_exact(top, context.active_target) &&
        slot_growth_geometry_supports_capacity(
            top, context.discovered_capacity);
    context.late_proof_exact = exact;
}

void observe_slot_growth_post_constructor(
    safetyhook::Context& cpu) noexcept {
    if (!g_slot_growth_callbacks_armed.load(std::memory_order_acquire)) return;
    const SlotGrowthConstructionContext pending = g_slot_growth_construction;
    if (!pending.active || !pending.top) return;
    g_slot_growth_construction = {};
    if (!pending.mutation_attempted) return;

    void* top_vtable{};
    void* list_vtable{};
    void* count_getter{};
    void* selector_vtable{};
    std::uint64_t count{};
    std::int32_t active{-1};
    std::int32_t selected{-2};
    SlotGrowthMovieFingerprint caller_movie{};
    SlotGrowthMovieFingerprint top_movie{};
    const auto* const list = static_cast<const std::byte*>(pending.top) +
        kCategoryListOffset;
    const auto* const selector = static_cast<const std::byte*>(pending.top) +
        kSelectorOffset;
    const bool exact = pending.early_seam_seen && pending.late_seam_seen &&
        pending.early_proof_exact && pending.late_proof_exact &&
        pending.mutation_committed &&
        pending.append_checkpoints ==
            pending.active_target - kStockLogicalTabCount &&
        reinterpret_cast<void*>(cpu.rax) == pending.top &&
        reinterpret_cast<void*>(cpu.rdi) == pending.top &&
        read_slot_growth_movie_fingerprint(
            pending.scene_proxy, caller_movie) &&
        read_slot_growth_movie_fingerprint(
            static_cast<std::byte*>(pending.top) + kMovieContextOffset,
            top_movie) &&
        same_slot_growth_movie(caller_movie, pending.movie) &&
        same_slot_growth_movie(top_movie, pending.movie) &&
        guarded_read(pending.top, top_vtable) &&
        top_vtable == g_game.base + kTopVtableRva &&
        guarded_read(list, list_vtable) &&
        list_vtable == g_game.base + kCategoryListVtableRva &&
        guarded_read(
            static_cast<const std::byte*>(list_vtable) + sizeof(void*),
            count_getter) &&
        count_getter == g_game.base + kCategoryCountGetterRva &&
        guarded_read(selector, selector_vtable) &&
        selector_vtable == g_game.base + kSelectorVtableRva &&
        guarded_read(
            static_cast<const std::byte*>(pending.top) +
                kCategoryCountOffset,
            count) &&
        count == pending.active_target &&
        guarded_read(selector + kSelectorActiveCountOffset, active) &&
        active == static_cast<std::int32_t>(pending.active_target) &&
        guarded_read(selector + kSelectorSelectedIndexOffset, selected) &&
        selected >= -1 && selected < active &&
        slot_growth_geometry_supports_capacity(
            pending.top, pending.discovered_capacity) &&
        slot_growth_records_exact(pending.top, pending.active_target);
    if (!exact) fail_fast_after_slot_growth("post-constructor", pending);

    erui::detail::logf(
        erui::LogLevel::info,
        "NativeTabs physical tab growth committed: configuredMinimum=%zu authoredCapacity=%zu activeTarget=%zu nativeCount=%llu selectorActive=%d",
        pending.configured_minimum,
        pending.discovered_capacity,
        pending.active_target,
        static_cast<unsigned long long>(count),
        active);
}
#endif

} // namespace native_tabs_detail
