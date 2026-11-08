// Game image validation, hook install/remove and the public erui::native entry points.
#include "native_tabs_internal.hpp"

namespace native_tabs_detail {

bool initialize_log() noexcept {
    std::array<wchar_t, 32768> module_path{};
    const DWORD length = GetModuleFileNameW(
        g_module, module_path.data(), static_cast<DWORD>(module_path.size()));
    if (length == 0 || static_cast<std::size_t>(length) >= module_path.size()) {
        return false;
    }

    wchar_t* const separator = wcsrchr(module_path.data(), L'\\');
    if (!separator) return false;
    *(separator + 1) = L'\0';
    constexpr wchar_t kLogName[] = L"ERNativeUITabViewportProbe.log";
    const std::size_t prefix_length = wcslen(module_path.data());
    if (prefix_length + _countof(kLogName) > module_path.size()) return false;
    wcscat_s(module_path.data(), module_path.size(), kLogName);

    g_log_file = CreateFileW(
        module_path.data(),
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    return g_log_file != INVALID_HANDLE_VALUE;
}

bool inspect_game_image(GameImage& image) noexcept {
    image = {};
    auto* const base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
    if (!base) return false;

#if defined(_MSC_VER)
    __try {
#endif
        const auto* const dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 ||
            dos->e_lfanew > 0x100000) {
            return false;
        }
        const auto* const nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
            base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE ||
            nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
            return false;
        }
        image.base = base;
        image.size = nt->OptionalHeader.SizeOfImage;
        image.timestamp = nt->FileHeader.TimeDateStamp;
        return image.size >= 0x1000;
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        image = {};
        return false;
    }
#endif
}

bool supported_game_image(const GameImage& image) noexcept {
    return find_native_tabs_build_profile(image.timestamp, image.size) !=
        nullptr;
}

bool image_contains(std::uintptr_t rva, std::size_t size) noexcept {
    return g_game.base && rva < g_game.size && size <= g_game.size - rva;
}

bool resolve_rip_relative_data_target(
    std::uintptr_t instruction_rva,
    std::uintptr_t& target_rva) noexcept {
    target_rva = 0;
    constexpr std::size_t kInstructionSize = 7;
    if (!image_contains(instruction_rva, kInstructionSize)) return false;

    std::array<std::uint8_t, 3> opcode{};
    std::int32_t displacement{};
    if (!guarded_copy(
            opcode.data(),
            g_game.base + instruction_rva,
            opcode.size()) ||
        opcode != std::array<std::uint8_t, 3>{0x48, 0x8B, 0x0D} ||
        !guarded_read(
            g_game.base + instruction_rva + opcode.size(),
            displacement)) {
        return false;
    }

    const std::int64_t target =
        static_cast<std::int64_t>(instruction_rva) +
        static_cast<std::int64_t>(kInstructionSize) + displacement;
    if (target < 0 ||
        static_cast<std::uint64_t>(target) >= g_game.size) {
        return false;
    }
    target_rva = static_cast<std::uintptr_t>(target);
    return true;
}

bool resolve_relative_call_target(
    std::uintptr_t call_rva,
    std::uintptr_t& target_rva) noexcept {
    target_rva = 0;
    if (!image_contains(call_rva, 5)) return false;

    std::uint8_t opcode{};
    std::int32_t displacement{};
    if (!guarded_read(g_game.base + call_rva, opcode) || opcode != 0xE8 ||
        !guarded_read(g_game.base + call_rva + 1, displacement)) {
        return false;
    }
    const std::int64_t target = static_cast<std::int64_t>(call_rva) + 5 +
        static_cast<std::int64_t>(displacement);
    if (target < 0 ||
        static_cast<std::uint64_t>(target) >= g_game.size) {
        return false;
    }
    target_rva = static_cast<std::uintptr_t>(target);
    return true;
}

bool validate_official_host() noexcept {
#if defined(ER_NATIVE_UI_PRODUCTION_TABS)
    g_research_prepare_carrier =
        &erui::native::prepare_native_tab_graphics_carrier;
    g_research_verify_carrier =
        &erui::native::verify_native_tab_graphics_carrier;
    if (erui::native::native_tab_graphics_carrier_version() != 2) {
        log_line("ERROR: native Graphics carrier V2 bridge unavailable");
        return false;
    }
    return true;
#else
    std::array<wchar_t, 32768> host_path{};
    HMODULE const host = GetModuleHandleW(L"ERNativeUI.dll");
    const DWORD length = host
        ? GetModuleFileNameW(
              host,
              host_path.data(),
              static_cast<DWORD>(host_path.size()))
        : 0;
    if (length == 0 || static_cast<std::size_t>(length) >= host_path.size()) {
        log_line("ERROR: loaded ERNativeUI host path unavailable");
        return false;
    }

    std::array<char, 32768> utf8_path{};
    const int converted = WideCharToMultiByte(
        CP_UTF8,
        0,
        host_path.data(),
        static_cast<int>(length),
        utf8_path.data(),
        static_cast<int>(utf8_path.size() - 1),
        nullptr,
        nullptr);
    if (converted <= 0) {
        log_line("ERROR: loaded ERNativeUI host path conversion failed");
        return false;
    }
    utf8_path[static_cast<std::size_t>(converted)] = '\0';
    log_line("Loaded ERNativeUI host path=%s", utf8_path.data());

    constexpr wchar_t kExpectedSuffix[] =
        L"\\DemoConfigMenu\\ERNativeUI.dll";
    const std::size_t path_length = static_cast<std::size_t>(length);
    const std::size_t suffix_length = _countof(kExpectedSuffix) - 1;
    if (path_length < suffix_length ||
        _wcsicmp(
            host_path.data() + path_length - suffix_length,
            kExpectedSuffix) != 0) {
        log_line(
            "ERROR: viewport probe requires the validated official ERNativeUI host under DemoConfigMenu; no hooks installed");
        return false;
    }

    using ResearchMarkerFn = std::uint32_t(__cdecl*)();
    ResearchMarkerFn const marker = reinterpret_cast<ResearchMarkerFn>(
        GetProcAddress(host, kTabContentResearchMarkerExport));
    if (!marker || marker() != kTabContentResearchMarker) {
        log_line(
            "ERROR: viewport content proof requires the exact research host marker; no hooks installed");
        return false;
    }
    log_line("Validated ERNativeUI tab-content research host marker");
    g_research_prepare_carrier = reinterpret_cast<ResearchPrepareCarrierFn>(
        GetProcAddress(host, "ERUI_ResearchPrepareGraphicsCarrierV2"));
    g_research_verify_carrier = reinterpret_cast<ResearchVerifyCarrierFn>(
        GetProcAddress(host, "ERUI_ResearchVerifyGraphicsCarrierV2"));
    ResearchCarrierVersionFn const carrier_version =
        reinterpret_cast<ResearchCarrierVersionFn>(GetProcAddress(
            host, "ERUI_ResearchGraphicsCarrierVersionV2"));
    if (!g_research_prepare_carrier || !g_research_verify_carrier ||
        !carrier_version || carrier_version() != 2) {
        log_line("ERROR: exact Graphics carrier V2 exports missing; no hooks installed");
        return false;
    }
    log_line("Validated Graphics carrier research protocol V2");
    return true;
#endif
}

template <typename Function>
bool prepare_hook(
    std::uintptr_t rva,
    void* detour,
    SafetyHookInline& hook,
    std::atomic<Function>& original,
    const char* name) noexcept {
    auto result = SafetyHookInline::create(
        g_game.base + rva,
        detour,
        SafetyHookInline::StartDisabled);
    if (!result) {
        log_line(
            "ERROR: could not prepare %s hook at RVA 0x%llX (SafetyHook error type %u)",
            name,
            static_cast<unsigned long long>(rva),
            static_cast<unsigned>(result.error().type));
        return false;
    }
    hook = std::move(*result);
    original.store(hook.original<Function>(), std::memory_order_release);
    return true;
}

bool enable_hook(SafetyHookInline& hook, const char* name) noexcept {
    auto result = hook.enable();
    if (!result) {
        log_line(
            "ERROR: could not enable %s hook (SafetyHook error type %u)",
            name,
            static_cast<unsigned>(result.error().type));
        return false;
    }
    return true;
}

bool enable_hook(SafetyHookMid& hook, const char* name) noexcept {
    auto result = hook.enable();
    if (!result) {
        log_line(
            "ERROR: could not enable %s hook (SafetyHook error type %u)",
            name,
            static_cast<unsigned>(result.error().type));
        return false;
    }
    return true;
}

void reset_hooks() noexcept {
    if (g_viewport_hooks_exposed.load(std::memory_order_acquire)) {
        // No quiescence protocol exists for game-thread callbacks. Once the
        // first enable has been attempted, even an apparently disabled hook
        // may have an in-flight trampoline. The pinned host retains the full
        // prepared set instead of invalidating executable storage.
        return;
    }
    g_viewport_callbacks_armed.store(false, std::memory_order_release);
#if defined(ER_NATIVE_UI_PRODUCTION_TABS)
    g_slot_growth_callbacks_armed.store(false, std::memory_order_release);
    g_slot_growth_early_hook.reset();
    g_slot_growth_late_hook.reset();
    g_slot_growth_post_constructor_hook.reset();
    g_category_append = nullptr;
    g_slot_growth_construction = {};
#endif
    g_research_switch_hook.reset();
    g_original_research_switch.store(nullptr, std::memory_order_release);
    g_composite_frame_hook.reset();
    g_selection_hook.reset();
    g_horizontal_navigation_source_hook.reset();
    g_selection_dispatch_hook.reset();
    g_pointer_cell_sample_hook.reset();
    g_pointer_dispatch_source_hook.reset();
    g_input_dispatch_frame_begin_hook.reset();
    g_panel_alias_visibility_hook.reset();
    g_panel_state_copy_hook.reset();
    g_texture_lookup_hook.reset();
    g_image_bind_hook.reset();
    g_original_composite_frame.store(nullptr, std::memory_order_release);
    g_original_selection.store(nullptr, std::memory_order_release);
    g_original_texture_lookup.store(nullptr, std::memory_order_release);
    g_original_image_bind.store(nullptr, std::memory_order_release);
    g_get_pointer_position = nullptr;
    g_grid_hit_test = nullptr;
    g_native_pointer_select = nullptr;
    g_mutating_viewport = false;
    g_blocking_composite_input = false;
    g_tab_selection_source = TabSelectionSource::unknown;
    g_pointer_cell_sample_trace = {};
    reset_pointer_dwell(nullptr);
    g_state_isolation_expectation = {};
}

bool install_hooks_and_interfaces(
    bool install_hooks = true,
    const erui::native::PreservedSharedMenuAddresses*
        preserved_shared = nullptr) noexcept {
    (void)preserved_shared;
    constexpr std::array<std::uint8_t, 16> kSelectionEntry{
        0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x8B, 0x12,
        0x48, 0x8B, 0xD9, 0x48, 0x8B, 0x49, 0x08, 0x48};
    constexpr std::array<std::uint8_t, 16> kCompositeFrameEntry{
        0x48, 0x8B, 0x89, 0xB8, 0x00, 0x00, 0x00, 0x48,
        0x85, 0xC9, 0x74, 0x07, 0x48, 0x8B, 0x01, 0x48};
    constexpr std::array<std::uint8_t, 16> kSelectorSelectEntry{
        0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C,
        0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48};
    constexpr std::array<std::uint8_t, 10>
        kInputDispatchFrameBeginBytes{
            0x44, 0x8B, 0xA7, 0xD4, 0x00, 0x00, 0x00,
            0x45, 0x33, 0xFF};
    constexpr std::array<std::uint8_t, 15>
        kPointerDispatchResultBytes{
            0x84, 0xC0,
            0x0F, 0x85, 0xC5, 0x01, 0x00, 0x00,
            0x4C, 0x8D, 0xB7, 0x60, 0x07, 0x00, 0x00};
    constexpr std::array<std::uint8_t, 18>
        kHorizontalNavigationCommitBytes{
            0x8B, 0x97, 0xD4, 0x00, 0x00, 0x00,
            0x4C, 0x8D, 0x45, 0x40,
            0x48, 0x8B, 0xCF,
            0xE8, 0xA6, 0x1B, 0x00, 0x00};
    constexpr std::array<std::uint8_t, 7>
        kSelectionDispatchBytes{
            0x44, 0x39, 0xA7, 0xD4, 0x00, 0x00, 0x00};
    constexpr std::array<std::uint8_t, 17>
        kPointerCellSampleBytes{
            0x83, 0xF8, 0xFF,
            0x74, 0x59,
            0x8B, 0xD0,
            0x48, 0x8B, 0xCE,
            0xE8, 0xD1, 0x11, 0x00, 0x00,
            0xB0, 0x01};
    constexpr std::array<std::uint8_t, 21>
        kGetPointerPosition271Entry{
            0x48, 0x89, 0x5C, 0x24, 0x08,
            0x57,
            0x48, 0x83, 0xEC, 0x30,
            0x48, 0x8B, 0xDA,
            0x48, 0x8B, 0xF9,
            0xE8, 0x4B, 0x05, 0x00, 0x00};
    constexpr std::array<std::uint8_t, 23>
        kGridHitTest271Entry{
            0x48, 0x8B, 0xC4,
            0x56,
            0x57,
            0x41, 0x56,
            0x48, 0x81, 0xEC, 0x90, 0x00, 0x00, 0x00,
            0x48, 0xC7, 0x44, 0x24, 0x20,
            0xFE, 0xFF, 0xFF, 0xFF};
    constexpr std::array<std::uint8_t, 21>
        kNativePointerSelect271Entry{
            0x48, 0x89, 0x5C, 0x24, 0x18,
            0x89, 0x54, 0x24, 0x10,
            0x57,
            0x48, 0x83, 0xEC, 0x20,
            0x44, 0x8B, 0x99, 0xDC, 0x00, 0x00, 0x00};
    constexpr std::array<std::uint8_t, 16> kResolvePathEntry{
        0x4C, 0x89, 0x44, 0x24, 0x18, 0x4C, 0x89, 0x4C,
        0x24, 0x20, 0x55, 0x53, 0x56, 0x57, 0x41, 0x56};
    constexpr std::array<std::uint8_t, 16> kValueExistsEntry{
        0x48, 0x83, 0xEC, 0x28, 0x48, 0x8B, 0x01, 0xFF,
        0x10, 0xF6, 0x40, 0x20, 0x8F, 0x0F, 0x95, 0xC0};
    constexpr std::array<std::uint8_t, 16> kPresentTabEntry{
        0x48, 0x8B, 0xC4, 0x48, 0x89, 0x50, 0x10, 0x57,
        0x48, 0x81, 0xEC, 0x90, 0x00, 0x00, 0x00, 0x48};
    constexpr std::array<std::uint8_t, 16> kMakeIconDescriptorEntry{
        0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83,
        0xEC, 0x20, 0x8B, 0xFA, 0x48, 0x8B, 0xD9, 0x85};
    constexpr std::array<std::uint8_t, 16> kTextureLookupEntry{
        0x40, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55,
        0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0x6C, 0x24};
    // Both supported builds have this exact entry. Together with the stable
    // three-argument callsite below, it validates the bool-returning image
    // binding implementation before the research detour is installed.
    constexpr std::array<std::uint8_t, 32> kImageBindEntry{
        0x48, 0x8B, 0xC4, 0x48, 0x89, 0x50, 0x10, 0x56,
        0x57, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57, 0x48,
        0x83, 0xEC, 0x60, 0x48, 0xC7, 0x40, 0x98, 0xFE,
        0xFF, 0xFF, 0xFF, 0x48, 0x89, 0x58, 0x08, 0x48};
    constexpr std::array<std::uint8_t, 12> kImageBindArguments{
        0x4C, 0x8B, 0xC0, 0x48, 0x8D, 0x54, 0x24, 0x28,
        0x48, 0x8D, 0x4D, 0x28};
    constexpr std::array<std::uint8_t, 2> kImageBindReturnsTrue{
        0xB0, 0x01};
    constexpr std::array<std::uint8_t, 2> kImageBindReturnsFalse{
        0x32, 0xC0};
    constexpr std::array<std::uint8_t, 16> kRefreshTitleEntry{
        0x40, 0x57, 0x48, 0x81, 0xEC, 0x90, 0x00, 0x00,
        0x00, 0x48, 0xC7, 0x44, 0x24, 0x20, 0xFE, 0xFF};
    constexpr std::array<std::uint8_t, 16> kSwitchPanelEntry{
        0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74,
        0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48};
    constexpr std::array<std::uint8_t, 49>
        kSwitchPanelStateCopyBytes{
            0x48, 0x85, 0xC0, 0x74, 0x3A,
            0x48, 0x85, 0xDB, 0x74, 0x27,
            0x48, 0x8D, 0x93, 0x38, 0x1B, 0x00, 0x00,
            0x48, 0x8D, 0x88, 0x38, 0x1B, 0x00, 0x00,
            0xE8, 0x11, 0xF9, 0xFF, 0xFF,
            0x48, 0x8B, 0x8E, 0xB8, 0x00, 0x00, 0x00,
            0x0F, 0xB6, 0x83, 0xE3, 0x1C, 0x00, 0x00,
            0x88, 0x81, 0xE3, 0x1C, 0x00, 0x00};
    constexpr std::array<std::uint8_t, 16>
        kSwitchPanelStateCopyResumeBytes{
            0x48, 0x8B, 0x8E, 0xB8, 0x00, 0x00, 0x00, 0x33,
            0xD2, 0xE8, 0x5F, 0xA3, 0x03, 0x00, 0x33, 0xFF};
    constexpr std::array<std::uint8_t, 41>
        kSwitchPanelVisibilityBytes{
            0x48, 0x8B, 0x0B,
            0x48, 0x85, 0xC9,
            0x74, 0x16,
            0x48, 0x39, 0x8E, 0xB8, 0x00, 0x00, 0x00,
            0x0F, 0x94, 0xC2,
            0x48, 0x81, 0xC1, 0x00, 0x12, 0x00, 0x00,
            0xE8, 0xFB, 0x77, 0xDF, 0xFF,
            0xFF, 0xC7,
            0x48, 0x83, 0xC3, 0x08,
            0x83, 0xFF, 0x0A,
            0x72, 0xD7};
    constexpr std::array<std::uint8_t, 17> kConstructor271Entry{
        0x48, 0x89, 0x4C, 0x24, 0x08, 0x48, 0x83, 0xEC,
        0x18, 0x48, 0xC7, 0x04, 0x24, 0xFE, 0xFF, 0xFF,
        0xFF};
    constexpr std::array<std::uint8_t, 16> kGetMember271Entry{
        0x48, 0x89, 0x54, 0x24, 0x10, 0x53, 0x48, 0x83,
        0xEC, 0x40, 0x48, 0xC7, 0x44, 0x24, 0x38, 0xFE};
    constexpr std::array<std::uint8_t, 16> kSetPosition271Entry{
        0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x55,
        0x48, 0x8D, 0x68, 0xC8, 0x48, 0x81, 0xEC, 0x30};
    constexpr std::array<std::uint8_t, 16> kSetVisible271Entry{
        0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x7C,
        0x24, 0x18, 0x55, 0x48, 0x8D, 0x6C, 0x24, 0xF0};
    constexpr std::array<std::uint8_t, 16> kDestructor271Entry{
        0x48, 0x89, 0x4C, 0x24, 0x08, 0x53, 0x48, 0x83,
        0xEC, 0x30, 0x48, 0xC7, 0x44, 0x24, 0x20, 0xFE};
#if defined(ER_NATIVE_UI_PRODUCTION_TABS)
    constexpr std::array<std::uint8_t, 16> kTopConstructorEntry{
        0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41,
        0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0xAC};
    constexpr std::array<std::uint8_t, 16> kEarlySlotGrowth271{
        0x4C, 0x8D, 0x05, 0xB2, 0x8F, 0x17, 0x02, 0x48,
        0x8D, 0x55, 0x50, 0x48, 0x8B, 0x4C, 0x24, 0x50};
    constexpr std::array<std::uint8_t, 16> kEarlySlotGrowth272{
        0x4C, 0x8D, 0x05, 0xF2, 0xAB, 0x17, 0x02, 0x48,
        0x8D, 0x55, 0x50, 0x48, 0x8B, 0x4C, 0x24, 0x50};
    constexpr std::array<std::uint8_t, 16> kFirstCountPublication{
        0x49, 0x8B, 0x45, 0x00, 0x49, 0x8B, 0xCD, 0xFF,
        0x50, 0x08, 0x8B, 0xD0, 0x48, 0x8D, 0x4B, 0x18};
    constexpr std::array<std::uint8_t, 16> kSelectorGeometryDispatch{
        0x49, 0x8B, 0x06, 0x49, 0x8B, 0xCE, 0xFF, 0x50,
        0x18, 0x0F, 0xBF, 0x9D, 0x74, 0x01, 0x00, 0x00};
    constexpr std::array<std::uint8_t, 16> kPostGeometryObservation{
        0x0F, 0xBF, 0x9D, 0x74, 0x01, 0x00, 0x00, 0x4C,
        0x89, 0x6C, 0x24, 0x40, 0x49, 0x8D, 0x8E, 0xE0};
    constexpr std::array<std::uint8_t, 17> kCountPropagationDispatch{
        0x45, 0x33, 0xC9, 0x44, 0x8B, 0xC3, 0x49, 0x8B,
        0xD5, 0x49, 0x8B, 0xCE, 0xE8, 0xD4, 0xF7, 0xFF,
        0xFF};
    constexpr std::array<std::uint8_t, 16> kPostConstructorObservation{
        0x48, 0x8B, 0xF8, 0xEB, 0x03, 0x48, 0x8B, 0xFB,
        0xF6, 0xC3, 0x01, 0x74, 0x11, 0x83, 0xE3, 0xFE};
    constexpr std::array<std::uint8_t, 16> kCategoryAppendEntry{
        0x40, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x48, 0xC7,
        0x44, 0x24, 0x20, 0xFE, 0xFF, 0xFF, 0xFF, 0x48};
    constexpr std::array<std::uint8_t, 7> kCategoryCountGetterEntry{
        0x8B, 0x81, 0x60, 0x05, 0x00, 0x00, 0xC3};
    constexpr std::array<std::uint8_t, 16> kSelectorCountSetterEntry{
        0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83,
        0xEC, 0x20, 0x8B, 0xFA, 0x48, 0x8B, 0xD9, 0x85};
    constexpr std::array<std::uint8_t, 20> kGeometryEnumeratorEntry{
        0x48, 0x8B, 0xC4, 0x55, 0x56, 0x57, 0x41, 0x54,
        0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D,
        0xA8, 0xE8, 0xFE, 0xFF};
#endif

    if (!g_native_tabs_build_profile) {
        log_line(
            "ERROR: no exact native-tab build profile was selected; no hooks installed");
        return false;
    }
    const std::uintptr_t destructor_rva = kScaleformDestructor271Rva;
    const std::uintptr_t expected_texture_lookup_rva =
        kTextureLookup271Rva;
    const std::uintptr_t expected_image_bind_rva = kImageBind271Rva;
    std::uintptr_t texture_lookup_rva{};
    if (!resolve_relative_call_target(
            kTextureLookupCallRva,
            texture_lookup_rva) ||
        texture_lookup_rva != expected_texture_lookup_rva) {
        log_line(
            "ERROR: texture lookup call target at RVA 0x%llX was 0x%llX; expected 0x%llX",
            static_cast<unsigned long long>(kTextureLookupCallRva),
            static_cast<unsigned long long>(texture_lookup_rva),
            static_cast<unsigned long long>(
                expected_texture_lookup_rva));
        return false;
    }
    std::uintptr_t image_bind_rva{};
    if (!resolve_relative_call_target(kImageBindCallRva, image_bind_rva) ||
        image_bind_rva != expected_image_bind_rva) {
        log_line(
            "ERROR: image bind call target at RVA 0x%llX was 0x%llX; expected 0x%llX",
            static_cast<unsigned long long>(kImageBindCallRva),
            static_cast<unsigned long long>(image_bind_rva),
            static_cast<unsigned long long>(expected_image_bind_rva));
        return false;
    }
    const bool destructor_entry_matches =
        entry_matches(destructor_rva, kDestructor271Entry);
    const bool constructor_entry_matches =
        entry_matches(kScaleformConstructor271Rva, kConstructor271Entry);
    const bool shape_helper_entries_match =
        entry_matches(kScaleformGetMember271Rva, kGetMember271Entry) &&
            entry_matches(
                kScaleformSetPosition271Rva, kSetPosition271Entry) &&
            entry_matches(
                kScaleformSetVisible271Rva, kSetVisible271Entry);
    const bool native_pointer_poll_helpers_match =
        entry_matches(
            kGetPointerPositionRva,
            kGetPointerPosition271Entry) &&
        entry_matches(kGridHitTestRva, kGridHitTest271Entry) &&
        entry_matches(
            kNativePointerSelectRva,
            kNativePointerSelect271Entry);
#if defined(ER_NATIVE_UI_PRODUCTION_TABS)
    const bool early_slot_growth_matches =
        tabs_build().timestamp == kNativeTabsBuildProfiles[0].timestamp
        ? entry_matches(kEarlySlotGrowthRva, kEarlySlotGrowth271)
        : entry_matches(kEarlySlotGrowthRva, kEarlySlotGrowth272);
#endif
    // Fork r156: every check carries a name so a failure says which interface
    // differs (Nexus/NPC Test Arena: the log only said "one or more").
    struct NamedInterfaceCheck {
        const char* name;
        bool matches;
    };
#if defined(ER_NATIVE_UI_PRODUCTION_TABS)
    const NamedInterfaceCheck slot_growth_checks[]{
        {"topConstructor",
            entry_matches(kTopConstructorRva, kTopConstructorEntry)},
        {"earlySlotGrowth", early_slot_growth_matches},
        {"firstCountPublication",
            entry_matches(
                kFirstCountPublicationRva, kFirstCountPublication)},
        {"selectorGeometryDispatch",
            entry_matches(
                kSelectorGeometryDispatchRva, kSelectorGeometryDispatch)},
        {"postGeometryObservation",
            entry_matches(
                kPostGeometryObservationRva, kPostGeometryObservation)},
        {"countPropagationDispatch",
            entry_matches(
                kCountPropagationDispatchRva, kCountPropagationDispatch)},
        {"postConstructorObservation",
            entry_matches(
                kPostConstructorObservationRva,
                kPostConstructorObservation)},
        {"categoryAppend",
            entry_matches(kCategoryAppendRva, kCategoryAppendEntry)},
        {"categoryCountGetter",
            entry_matches(
                kCategoryCountGetterRva, kCategoryCountGetterEntry)},
        {"selectorSetActiveCount",
            entry_matches(
                kSelectorSetActiveCountRva, kSelectorCountSetterEntry)},
        {"selectorGeometryEnumerator",
            entry_matches(
                kSelectorGeometryEnumeratorRva, kGeometryEnumeratorEntry)},
    };
#endif

    const bool pristine_resolve_path_entry_matches =
        entry_matches(kResolvePathRva, kResolvePathEntry);
    bool host_owned_resolve_path_bridge_matches{};
#if defined(ER_NATIVE_UI_PRODUCTION_TABS)
    // Startup preflight validates the original entry before registration is
    // opened. Production installation happens after the ordinary menu hooks,
    // whose title bridge legitimately patches this exact same function. Only
    // accept that post-preflight mutation when the menu-hook owner confirms
    // its live bridge is installed at this precise address; an unknown patch
    // must still fail closed.
    host_owned_resolve_path_bridge_matches = install_hooks &&
        erui::native::native_scaleform_path_bridge_installed_at(
            reinterpret_cast<const void*>(
                g_game.base + kResolvePathRva));
#endif
    const bool resolve_path_interface_matches =
        pristine_resolve_path_entry_matches ||
        host_owned_resolve_path_bridge_matches;

    const bool pristine_selector_select_entry_matches =
        entry_matches(kSelectorSelectRva, kSelectorSelectEntry);
    bool approved_selector_select_entry_matches{};
#if defined(ER_NATIVE_UI_PRODUCTION_TABS)
    if (install_hooks && preserved_shared) {
        erui::native::ModuleView live_game{};
        approved_selector_select_entry_matches =
            live_game.initialize(
                reinterpret_cast<HMODULE>(g_game.base)) &&
            erui::native::revalidate_preserved_shared_menu_entry(
                live_game,
                *preserved_shared,
                erui::native::PreservedSharedMenuEntry::selector_select);
    }
#endif
    const bool selector_select_interface_matches =
        pristine_selector_select_entry_matches ||
        approved_selector_select_entry_matches;

    if (!native_pointer_poll_helpers_match) {
        log_line(
            "ERROR: native pointer polling helpers differ from the validated Elden Ring %s executable; no hooks installed",
            tabs_build().version);
        return false;
    }

    const NamedInterfaceCheck interface_checks[]{
        {"selectionCallback",
            entry_matches(kSelectionCallbackRva, kSelectionEntry)},
        {"compositeFrame",
            entry_matches(kCompositeFrameRva, kCompositeFrameEntry)},
        {"selectorSelect", selector_select_interface_matches},
        {"inputDispatchFrameBegin",
            entry_matches(
                kInputDispatchFrameBeginRva,
                kInputDispatchFrameBeginBytes)},
        {"pointerDispatchResult",
            entry_matches(
                kPointerDispatchResultRva,
                kPointerDispatchResultBytes)},
        {"horizontalNavigationCommit",
            entry_matches(
                kHorizontalNavigationCommitRva,
                kHorizontalNavigationCommitBytes)},
        {"selectionDispatch",
            entry_matches(
                kSelectionDispatchRva,
                kSelectionDispatchBytes)},
        {"pointerCellSample",
            entry_matches(
                kPointerCellSampleRva,
                kPointerCellSampleBytes)},
        {"switchPanel", entry_matches(kSwitchPanelRva, kSwitchPanelEntry)},
        {"switchPanelStateCopy",
            entry_matches(
                tabs_build().switch_panel_state_copy_signature,
                kSwitchPanelStateCopyBytes)},
        {"switchPanelStateCopyResume",
            entry_matches(
                kSwitchPanelStateCopyResumeRva,
                kSwitchPanelStateCopyResumeBytes)},
        {"switchPanelVisibility",
            entry_matches(
                tabs_build().switch_panel_visibility_signature,
                kSwitchPanelVisibilityBytes)},
        {"resolvePath", resolve_path_interface_matches},
        {"valueExists", entry_matches(kValueExistsRva, kValueExistsEntry)},
        {"presentTab", entry_matches(kPresentTabRva, kPresentTabEntry)},
        {"makeIconDescriptor",
            entry_matches(
                kMakeIconDescriptorRva,
                kMakeIconDescriptorEntry)},
        {"textureLookup",
            entry_matches(texture_lookup_rva, kTextureLookupEntry)},
        {"imageBind", entry_matches(image_bind_rva, kImageBindEntry)},
        {"imageBindArguments",
            entry_matches(
                kImageBindCallRva - kImageBindArguments.size(),
                kImageBindArguments)},
        {"imageBindReturnsTrue",
            entry_matches(
                image_bind_rva + 0x282,
                kImageBindReturnsTrue)},
        {"imageBindReturnsFalse",
            entry_matches(
                image_bind_rva + 0x293,
                kImageBindReturnsFalse)},
        {"refreshTitle",
            entry_matches(kRefreshTitleRva, kRefreshTitleEntry)},
        {"scaleformConstructor", constructor_entry_matches},
        {"scaleformShapeHelpers", shape_helper_entries_match},
        {"scaleformDestructor", destructor_entry_matches},
    };
    std::array<char, 1024> failed_checks{};
    std::size_t failed_length = 0;
    const auto collect_failures =
        [&](const NamedInterfaceCheck* first, std::size_t count) noexcept {
            for (std::size_t index = 0; index < count; ++index) {
                if (first[index].matches) continue;
                const int written = _snprintf_s(
                    failed_checks.data() + failed_length,
                    failed_checks.size() - failed_length,
                    _TRUNCATE,
                    "%s%s",
                    failed_length ? "," : "",
                    first[index].name);
                if (written < 0) {
                    failed_length = failed_checks.size() - 1U;
                    return;
                }
                failed_length += static_cast<std::size_t>(written);
            }
        };
    collect_failures(interface_checks, std::size(interface_checks));
#if defined(ER_NATIVE_UI_PRODUCTION_TABS)
    collect_failures(slot_growth_checks, std::size(slot_growth_checks));
#endif
    if (failed_length != 0) {
        log_line(
            "ERROR: viewport interfaces differ from the validated executable: %s",
            failed_checks.data());
        log_line(
            "ERROR: one or more viewport interfaces differ from the validated executable; no hooks installed (resolvePath={pristine:%u hostOwned:%u accepted:%u} selectorSelect={pristine:%u solidApproved:%u accepted:%u})",
            pristine_resolve_path_entry_matches ? 1U : 0U,
            host_owned_resolve_path_bridge_matches ? 1U : 0U,
            resolve_path_interface_matches ? 1U : 0U,
            pristine_selector_select_entry_matches ? 1U : 0U,
            approved_selector_select_entry_matches ? 1U : 0U,
            selector_select_interface_matches ? 1U : 0U);
        return false;
    }

    g_selector_select = reinterpret_cast<SelectorSelectFn>(
        g_game.base + kSelectorSelectRva);
    g_get_pointer_position = native_pointer_poll_helpers_match
        ? reinterpret_cast<GetPointerPositionFn>(
              g_game.base + kGetPointerPositionRva)
        : nullptr;
    g_grid_hit_test = native_pointer_poll_helpers_match
        ? reinterpret_cast<GridHitTestFn>(g_game.base + kGridHitTestRva)
        : nullptr;
    g_native_pointer_select = native_pointer_poll_helpers_match
        ? reinterpret_cast<NativePointerSelectFn>(
              g_game.base + kNativePointerSelectRva)
        : nullptr;
    g_switch_panel = reinterpret_cast<SwitchPanelFn>(
        g_game.base + kSwitchPanelRva);
    g_refresh_title = reinterpret_cast<RefreshTitleFn>(
        g_game.base + kRefreshTitleRva);
    g_resolve_path = reinterpret_cast<ResolvePathFn>(
        g_game.base + kResolvePathRva);
    g_value_exists = reinterpret_cast<ValueExistsFn>(
        g_game.base + kValueExistsRva);
    g_present_tab = reinterpret_cast<PresentTabFn>(
        g_game.base + kPresentTabRva);
    g_make_icon_descriptor = reinterpret_cast<MakeIconDescriptorFn>(
        g_game.base + kMakeIconDescriptorRva);
    g_construct_scaleform_value = reinterpret_cast<ScaleformConstructorFn>(
        g_game.base + kScaleformConstructor271Rva);
    g_get_scaleform_member = reinterpret_cast<ScaleformGetMemberFn>(
        g_game.base + kScaleformGetMember271Rva);
    g_set_scaleform_position = reinterpret_cast<ScaleformSetPositionFn>(
        g_game.base + kScaleformSetPosition271Rva);
    g_set_scaleform_visible = reinterpret_cast<ScaleformSetVisibleFn>(
        g_game.base + kScaleformSetVisible271Rva);
    g_destroy_scaleform_value = reinterpret_cast<ScaleformDestructorFn>(
        g_game.base + destructor_rva);
#if defined(ER_NATIVE_UI_PRODUCTION_TABS)
    g_category_append = reinterpret_cast<CategoryAppendFn>(
        g_game.base + kCategoryAppendRva);
#endif

    if (!install_hooks) return true;

    auto state_copy_hook = SafetyHookMid::create(
        g_game.base + kSwitchPanelStateCopyRva,
        &isolate_panel_state_copy,
        SafetyHookMid::StartDisabled);
    if (!state_copy_hook) {
        log_line(
            "ERROR: could not prepare panel-state isolation hook at RVA 0x%llX (SafetyHook error type %u)",
            static_cast<unsigned long long>(kSwitchPanelStateCopyRva),
            static_cast<unsigned>(state_copy_hook.error().type));
        return false;
    }
    g_panel_state_copy_hook = std::move(*state_copy_hook);

    auto frame_begin_source_hook = SafetyHookMid::create(
        g_game.base + kInputDispatchFrameBeginRva,
        &observe_input_dispatch_frame_begin,
        SafetyHookMid::StartDisabled);
    if (!frame_begin_source_hook) {
        log_line(
            "ERROR: could not prepare input-dispatch frame-begin hook at RVA 0x%llX (SafetyHook error type %u)",
            static_cast<unsigned long long>(
                kInputDispatchFrameBeginRva),
            static_cast<unsigned>(frame_begin_source_hook.error().type));
        reset_hooks();
        return false;
    }
    g_input_dispatch_frame_begin_hook =
        std::move(*frame_begin_source_hook);

    auto pointer_source_hook = SafetyHookMid::create(
        g_game.base + kPointerDispatchResultRva,
        &observe_pointer_dispatch_result,
        SafetyHookMid::StartDisabled);
    if (!pointer_source_hook) {
        log_line(
            "ERROR: could not prepare pointer-source hook at RVA 0x%llX (SafetyHook error type %u)",
            static_cast<unsigned long long>(
                kPointerDispatchResultRva),
            static_cast<unsigned>(pointer_source_hook.error().type));
        reset_hooks();
        return false;
    }
    g_pointer_dispatch_source_hook = std::move(*pointer_source_hook);

    auto horizontal_navigation_source_hook = SafetyHookMid::create(
        g_game.base + kHorizontalNavigationCommitRva,
        &observe_horizontal_navigation_commit,
        SafetyHookMid::StartDisabled);
    if (!horizontal_navigation_source_hook) {
        log_line(
            "ERROR: could not prepare horizontal-navigation source hook at RVA 0x%llX (SafetyHook error type %u)",
            static_cast<unsigned long long>(
                kHorizontalNavigationCommitRva),
            static_cast<unsigned>(
                horizontal_navigation_source_hook.error().type));
        reset_hooks();
        return false;
    }
    g_horizontal_navigation_source_hook =
        std::move(*horizontal_navigation_source_hook);

    auto selection_dispatch_hook = SafetyHookMid::create(
        g_game.base + kSelectionDispatchRva,
        &dispatch_due_pointer_dwell,
        SafetyHookMid::StartDisabled);
    if (!selection_dispatch_hook) {
        log_line(
            "ERROR: could not prepare selection-dispatch hook at RVA 0x%llX (SafetyHook error type %u)",
            static_cast<unsigned long long>(kSelectionDispatchRva),
            static_cast<unsigned>(selection_dispatch_hook.error().type));
        reset_hooks();
        return false;
    }
    g_selection_dispatch_hook = std::move(*selection_dispatch_hook);

    auto pointer_cell_sample_hook = SafetyHookMid::create(
        g_game.base + kPointerCellSampleRva,
        &observe_pointer_cell_sample,
        SafetyHookMid::StartDisabled);
    if (!pointer_cell_sample_hook) {
        log_line(
            "ERROR: could not prepare pointer-cell sample hook at RVA 0x%llX (SafetyHook error type %u)",
            static_cast<unsigned long long>(kPointerCellSampleRva),
            static_cast<unsigned>(
                pointer_cell_sample_hook.error().type));
        reset_hooks();
        return false;
    }
    g_pointer_cell_sample_hook =
        std::move(*pointer_cell_sample_hook);

#if defined(ER_NATIVE_UI_PRODUCTION_TABS)
    auto slot_growth_early_hook = SafetyHookMid::create(
        g_game.base + kEarlySlotGrowthRva,
        &append_graphics_records_at_construction_seam,
        SafetyHookMid::StartDisabled);
    if (!slot_growth_early_hook) {
        log_line(
            "ERROR: could not prepare physical-tab growth hook (SafetyHook error type %u)",
            static_cast<unsigned>(slot_growth_early_hook.error().type));
        reset_hooks();
        return false;
    }
    auto slot_growth_late_hook = SafetyHookMid::create(
        g_game.base + kPostGeometryObservationRva,
        &observe_slot_growth_post_geometry,
        SafetyHookMid::StartDisabled);
    if (!slot_growth_late_hook) {
        log_line(
            "ERROR: could not prepare physical-tab post-geometry proof hook (SafetyHook error type %u)",
            static_cast<unsigned>(slot_growth_late_hook.error().type));
        reset_hooks();
        return false;
    }
    auto slot_growth_post_constructor_hook = SafetyHookMid::create(
        g_game.base + kPostConstructorObservationRva,
        &observe_slot_growth_post_constructor,
        SafetyHookMid::StartDisabled);
    if (!slot_growth_post_constructor_hook) {
        log_line(
            "ERROR: could not prepare physical-tab post-constructor proof hook (SafetyHook error type %u)",
            static_cast<unsigned>(
                slot_growth_post_constructor_hook.error().type));
        reset_hooks();
        return false;
    }
    g_slot_growth_early_hook = std::move(*slot_growth_early_hook);
    g_slot_growth_late_hook = std::move(*slot_growth_late_hook);
    g_slot_growth_post_constructor_hook =
        std::move(*slot_growth_post_constructor_hook);
#endif

    if (!prepare_hook(
            kSwitchPanelRva,
            reinterpret_cast<void*>(&research_switch_panel_detour),
            g_research_switch_hook,
            g_original_research_switch,
            "reusable carrier switch") ||
        !prepare_hook(
            texture_lookup_rva,
            reinterpret_cast<void*>(&texture_lookup_detour),
            g_texture_lookup_hook,
            g_original_texture_lookup,
            "Scaleform texture lookup") ||
        !prepare_hook(
            image_bind_rva,
            reinterpret_cast<void*>(&image_bind_detour),
            g_image_bind_hook,
            g_original_image_bind,
            "Scaleform image bind") ||
        !prepare_hook(
            kSelectionCallbackRva,
            reinterpret_cast<void*>(&selection_callback_detour),
            g_selection_hook,
            g_original_selection,
            "tab selection callback") ||
        !prepare_hook(
            kCompositeFrameRva,
            reinterpret_cast<void*>(&composite_frame_detour),
            g_composite_frame_hook,
            g_original_composite_frame,
            "composite panel frame")) {
        reset_hooks();
        return false;
    }
    // Arm panel-state isolation before a viewport callback can route category
    // 4, then arm the image observer before resource aliasing can publish a
    // marker.
    // From the first enable attempt onward the hook objects and catalogue are
    // process-lifetime. Until every target is enabled, callbacks are gated to
    // exact native pass-through/no-op behavior.
#if defined(ER_NATIVE_UI_PRODUCTION_TABS)
    // Hook preparation can allocate and suspend threads. Close that timing
    // window with one last exact selector-prefix and indirect-target check
    // while every prepared hook is still disabled and locally reversible.
    bool selector_still_approved =
        entry_matches(kSelectorSelectRva, kSelectorSelectEntry);
    if (preserved_shared && !selector_still_approved) {
        erui::native::ModuleView live_game{};
        selector_still_approved = live_game.initialize(
            reinterpret_cast<HMODULE>(g_game.base)) &&
            erui::native::revalidate_preserved_shared_menu_entry(
                live_game,
                *preserved_shared,
                erui::native::PreservedSharedMenuEntry::selector_select);
    }
    if (!selector_still_approved) {
        log_line(
            "ERROR: Selector::select changed after interface validation and before viewport hook exposure; no hooks installed");
        reset_hooks();
        return false;
    }
#endif
    g_viewport_callbacks_armed.store(false, std::memory_order_release);
#if defined(ER_NATIVE_UI_PRODUCTION_TABS)
    g_slot_growth_callbacks_armed.store(false, std::memory_order_release);
#endif
    g_viewport_hooks_exposed.store(true, std::memory_order_release);

#if defined(ER_NATIVE_UI_PRODUCTION_TABS)
    // Publish the post-constructor observer first. Until all three hooks are
    // visible and the callback gate is armed, every callback is a native
    // no-op. This ordering ensures an attempted append can never escape
    // without the final ownership/count proof.
    if (!enable_hook(
            g_slot_growth_post_constructor_hook,
            "physical-tab post-constructor proof") ||
        !enable_hook(
            g_slot_growth_late_hook,
            "physical-tab post-geometry proof") ||
        !enable_hook(
            g_slot_growth_early_hook,
            "physical-tab growth")) {
        return false;
    }
#endif

    const auto state_copy_enabled = g_panel_state_copy_hook.enable();
    if (!state_copy_enabled) {
        log_line(
            "ERROR: could not enable panel-state isolation hook (SafetyHook error type %u)",
            static_cast<unsigned>(state_copy_enabled.error().type));
        return false;
    }
    if (!enable_hook(
            g_input_dispatch_frame_begin_hook,
            "tab input-dispatch frame-begin") ||
        !enable_hook(
            g_pointer_dispatch_source_hook,
            "tab pointer-source") ||
        !enable_hook(
            g_horizontal_navigation_source_hook,
            "tab horizontal-navigation-source") ||
        !enable_hook(
            g_selection_dispatch_hook,
            "tab selection-dispatch") ||
        !enable_hook(
            g_pointer_cell_sample_hook,
            "tab pointer-cell sample") ||
        !enable_hook(g_research_switch_hook, "reusable carrier switch") ||
        !enable_hook(g_image_bind_hook, "Scaleform image bind") ||
        !enable_hook(g_texture_lookup_hook, "Scaleform texture lookup") ||
        !enable_hook(g_selection_hook, "tab selection callback") ||
        !enable_hook(g_composite_frame_hook, "composite panel frame")) {
        return false;
    }

    // Make the complete viewport operational before native record growth can
    // run. A Configuration construction racing this publication may safely
    // retain the stock eight records, but it can never expose appended records
    // to an unarmed viewport.
    g_viewport_callbacks_armed.store(true, std::memory_order_release);
#if defined(ER_NATIVE_UI_PRODUCTION_TABS)
    g_slot_growth_callbacks_armed.store(true, std::memory_order_release);
#endif

    log_line(
        "STRESS startup hooks installed: protocol=GraphicsCarrierV2 selection=eldenring.exe+0x%llX source={frame:+0x%llX pointer:+0x%llX horizontal:+0x%llX dispatch:+0x%llX cellSample:+0x%llX} nativePoll={status:%s position:+0x%llX hit:+0x%llX select:+0x%llX} compositeFrame=+0x%llX panelStateIsolation=+0x%llX->+0x%llX rowsCarrier=category8/selector11 parking=category5 textureLookup=+0x%llX imageBind=+0x%llX cross=%ls overflow=%ls alias=%ls; logical=%zu physical=%zu margin=%zu dwell={initial:%llums repeat:%llums decay:%llums minimum:%llums} imageProbe={nativeResource:dynamic-shape-rgba8 rawImageLifecycle:%s nativeResourceLifecycle:%s}",
        static_cast<unsigned long long>(kSelectionCallbackRva),
        static_cast<unsigned long long>(kInputDispatchFrameBeginRva),
        static_cast<unsigned long long>(kPointerDispatchResultRva),
        static_cast<unsigned long long>(
            kHorizontalNavigationCommitRva),
        static_cast<unsigned long long>(kSelectionDispatchRva),
        static_cast<unsigned long long>(kPointerCellSampleRva),
        native_pointer_poll_helpers_match ? "enabled" : "unavailable",
        static_cast<unsigned long long>(kGetPointerPositionRva),
        static_cast<unsigned long long>(kGridHitTestRva),
        static_cast<unsigned long long>(kNativePointerSelectRva),
        static_cast<unsigned long long>(kCompositeFrameRva),
        static_cast<unsigned long long>(kSwitchPanelStateCopyRva),
        static_cast<unsigned long long>(
            kSwitchPanelStateCopyResumeRva),
        static_cast<unsigned long long>(texture_lookup_rva),
        static_cast<unsigned long long>(image_bind_rva),
        kCrossIconResource,
        kOverflowIconResource,
        kAliasedNativeIconResource,
        active_logical_tab_count(),
        active_physical_tab_count(),
        g_navigation_margin,
        static_cast<unsigned long long>(
            g_pointer_dwell_timing.initial_delay_ms),
        static_cast<unsigned long long>(
            g_pointer_dwell_timing.repeat_delay_ms),
        static_cast<unsigned long long>(
            g_pointer_dwell_timing.repeat_decay_ms),
        static_cast<unsigned long long>(
            g_pointer_dwell_timing.minimum_repeat_delay_ms),
        kEnableRawImageLifecycleProbe ? "enabled" : "disabled",
        kEnableNativeImageResourceLifecycleProbe ? "enabled" : "disabled");
    return true;
}

#if !defined(ER_NATIVE_UI_PRODUCTION_TABS)
DWORD WINAPI run_probe(void*) noexcept {
    if (!initialize_log()) return 1;
    const std::uint64_t bootstrap_started_qpc = query_performance_ticks();
    log_line("STRESS ============================================================");
    log_line("STRESS startup configuration-tab viewport mutation probe");

    HMODULE pinned{};
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(g_module),
            &pinned)) {
        log_line("ERROR: probe DLL could not be pinned; no hooks installed");
        return 1;
    }

    if (!inspect_game_image(g_game)) {
        log_line("ERROR: could not inspect the main executable image");
        return 1;
    }
    log_line(
        "STRESS startup gameImage base=%p timestamp=0x%08X size=0x%zX",
        static_cast<void*>(g_game.base),
        static_cast<unsigned>(g_game.timestamp),
        g_game.size);
    g_native_tabs_build_profile = find_native_tabs_build_profile(
        g_game.timestamp, g_game.size);
    if (!g_native_tabs_build_profile) {
        log_line(
            "ERROR: unsupported Elden Ring executable identity; expected an exact 2.7.1.0 or 2.7.2.0 image");
        return 1;
    }
    if (!validate_official_host()) return 1;
    const bool installed = install_hooks_and_interfaces();
    log_line(
        "STRESS bootstrap complete=%u timingUs=%llu scope=game-validation-plus-hook-install",
        installed ? 1U : 0U,
        static_cast<unsigned long long>(elapsed_qpc_microseconds(
            bootstrap_started_qpc, query_performance_ticks())));
    return installed ? 0 : 1;
}
#endif

} // namespace native_tabs_detail

// The public entry points used to see the anonymous namespace implicitly.
using namespace native_tabs_detail;

#if defined(ER_NATIVE_UI_PRODUCTION_TABS)
namespace erui::native {

NativeTabsPreflightStatus preflight_native_tabs() noexcept {
    g_native_tabs_build_profile = nullptr;
    GameImage image{};
    if (!inspect_game_image(image)) {
        return NativeTabsPreflightStatus::interface_mismatch;
    }
    const NativeTabsBuildProfile* const profile =
        find_native_tabs_build_profile(image.timestamp, image.size);
    // Fork: one line per step, so a startup crash shows how far it got.
    detail::logf(LogLevel::info,
        "Tab preflight: image base=%p size=0x%zX timestamp=0x%08X profile=%s",
        static_cast<void*>(image.base), image.size,
        static_cast<unsigned>(image.timestamp), profile ? "found" : "none");
    if (!profile) {
        return NativeTabsPreflightStatus::unsupported_game;
    }

    g_native_tabs_build_profile = profile;
    g_game = image;
    GameAddresses carrier_addresses{};
    carrier_addresses.game_image_base = image.base;
    carrier_addresses.game_image_size = image.size;
    carrier_addresses.graphics_panel_materializer =
        reinterpret_cast<BuiltinPanelMaterializerFn>(
            image.base + profile->graphics_panel_materializer);
    const bool carrier = preflight_native_tab_carrier(carrier_addresses);
    detail::logf(LogLevel::info, "Tab preflight: Graphics carrier %s",
        carrier ? "matches" : "does not match");
    if (!carrier || !validate_official_host()) {
        return NativeTabsPreflightStatus::interface_mismatch;
    }
    detail::logf(LogLevel::info, "Tab preflight: checking the tab interfaces");
    if (!install_hooks_and_interfaces(false)) {
        return NativeTabsPreflightStatus::interface_mismatch;
    }
    return NativeTabsPreflightStatus::available;
}

bool install_native_tabs(
    const detail::NativeTabCatalogue& catalogue,
    const PreservedSharedMenuAddresses* preserved_shared) noexcept {
    if (g_native_tabs_installed.load(std::memory_order_acquire) ||
        catalogue.custom_count() == 0 ||
        catalogue.custom_count() > ERUI_CUSTOM_TAB_MAX_COUNT ||
        catalogue.entries().size() !=
            kStockLogicalTabCount + catalogue.custom_count()) {
        return false;
    }
    // The immutable-image preflight ran before provider registration opened.
    // At this point install_native_menu_hooks() has deliberately patched the
    // carrier entry points, so comparing them with the original executable
    // bytes again would reject our own healthy hooks. Validate the installed
    // carrier state instead; install_hooks_and_interfaces(true) below still
    // validates every viewport-specific executable seam before patching it.
    if (!native_tab_graphics_carrier_installed() ||
        !validate_official_host()) {
        detail::logf(
            LogLevel::error,
            "NativeTabs requires the already-installed validated Graphics carrier");
        return false;
    }

    detail::RuntimeState& host_runtime = detail::runtime_state();
    if (!host_runtime.menu) return false;
    const auto custom = catalogue.custom_entries();
    const std::size_t configured_minimum =
        catalogue.navigation_settings().minimum_tab_slots;
    if (configured_minimum < kStockLogicalTabCount ||
        configured_minimum > kPhysicalTabCapacity) {
        detail::logf(
            LogLevel::error,
            "NativeTabs rejected MinimumTabSlots=%zu; native physical capacity is %zu..%zu",
            configured_minimum,
            kStockLogicalTabCount,
            kPhysicalTabCapacity);
        return false;
    }
    std::unique_ptr<detail::NativeTabRuntimeState> runtime_candidate =
        detail::NativeTabRuntimeState::create(custom.size());
    if (!runtime_candidate) {
        detail::logf(
            LogLevel::error,
            "NativeTabs could not allocate %zu exact-sized runtime entries",
            custom.size());
        return false;
    }
    std::unique_ptr<PinnedLogicalTabCatalogue> logical_candidate{};
    try {
        logical_candidate = std::make_unique<PinnedLogicalTabCatalogue>();
        logical_candidate->tabs.resize(catalogue.entries().size());
    } catch (...) {
        detail::logf(
            LogLevel::error,
            "NativeTabs could not allocate the exact-sized logical catalogue");
        return false;
    }
    const auto entries = catalogue.entries();
    for (std::size_t logical = 0; logical < entries.size(); ++logical) {
        logical_candidate->tabs[logical].source = &entries[logical];
    }
    for (std::size_t index = 0; index < custom.size(); ++index) {
        const wchar_t* const title = host_runtime.menu->texts.lookup(
            static_cast<int>(custom[index].title_id));
        if (!title || title[0] == L'\0' ||
            wcsnlen_s(title, kTitleCapacity) >= kTitleCapacity) {
            detail::logf(
                LogLevel::error,
                "NativeTabs rejected custom tab %zu: title is unavailable or exceeds %zu UTF-16 code units",
                index,
                kTitleCapacity - 1U);
            return false;
        }
        LogicalTab& logical =
            logical_candidate->tabs[kStockLogicalTabCount + index];
        if (!copy_title(title, logical.title)) return false;
        logical.category = 8;
        logical.custom_runtime = runtime_candidate->tab(index);
        if (!logical.custom_runtime) return false;
        auto& icon_runtime = logical.custom_runtime->icon();
        icon_runtime.resource_id = kFirstRgba8IconId +
            static_cast<std::int32_t>(index);
        make_fixed_decimal_name(
            icon_runtime.resource_name,
            L"MENU_ItemIcon_",
            static_cast<std::uint32_t>(icon_runtime.resource_id),
            5U);
        make_fixed_decimal_name(
            icon_runtime.raw_resource_name,
            L"ERNativeUI.TabProbe.RawRGBA8.",
            static_cast<std::uint32_t>(index + 1U),
            4U);
        const auto& icon = custom[index].icon;
        if (icon.source == TabIconSource::fallback) {
            if (!icon.rgba8_pixels.empty()) return false;
            continue;
        }
        if (icon.source != TabIconSource::rgba8 ||
            icon.rgba8_pixels.size() !=
                ERUI_TAB_ICON_RGBA8_BYTE_COUNT) {
            return false;
        }
        icon_runtime.raster_requested = true;
    }
    g_native_tab_catalogue = &catalogue;
    g_native_tab_runtime = std::move(runtime_candidate);
    g_logical_tab_catalogue = std::move(logical_candidate);
    // The configuration catalogue is immutable after registration freezes.
    // Constructor callbacks read only this process-lifetime scalar floor;
    // the exact live movie may safely raise the constructor-local target to a
    // larger compatible strip, up to the hard native capacity of ten.
    g_minimum_tab_slots = configured_minimum;
    g_navigation_margin = catalogue.navigation_settings().navigation_margin;
    g_mouse_dwell_enabled =
        catalogue.navigation_settings().mouse_dwell_enabled;
    g_pointer_dwell_timing =
        catalogue.navigation_settings().mouse_dwell_timing;
    if (!install_hooks_and_interfaces(true, preserved_shared)) {
        if (g_viewport_hooks_exposed.load(std::memory_order_acquire)) {
            // Once the first enable was attempted, an enabled/in-flight
            // trampoline may exist even when SafetyHook reported failure.
            // Publish the lifetime gate so fail_install() retains the complete
            // pinned runtime and the unarmed callbacks remain native no-ops.
            g_native_tabs_installed.store(true, std::memory_order_release);
            detail::logf(
                LogLevel::error,
                "NativeTabs hook installation failed after viewport-hook exposure; the partial unarmed pinned runtime is retained for process safety");
            return false;
        }
        // Hook creation/validation failed before any target was exposed, so
        // ordinary local rollback is still race-free.
        reset_hooks();
        reset_runtime();
        g_native_tab_catalogue = nullptr;
        g_native_tab_runtime.reset();
        g_logical_tab_catalogue.reset();
        return false;
    }
    g_native_tabs_installed.store(true, std::memory_order_release);
    detail::logf(
        LogLevel::info,
        "NativeTabs installed: logical=%zu custom=%zu minimumPhysicalSlots=%zu hardPhysicalCapacity=%zu margin=%zu mouseDwell=%u",
        active_logical_tab_count(),
        active_custom_tab_count(),
        g_minimum_tab_slots,
        kPhysicalTabCapacity,
        g_navigation_margin,
        g_mouse_dwell_enabled ? 1U : 0U);
    return true;
}

bool remove_native_tabs() noexcept {
    if (!native_tab_graphics_carrier_exposed() &&
        !g_viewport_hooks_exposed.load(std::memory_order_acquire) &&
        !g_native_tabs_installed.load(std::memory_order_acquire)) {
        return true;
    }
    // There is deliberately no load/exchange teardown window here. A game
    // callback can enter immediately after the hooks are enabled and does not
    // take a host-side uninstall lock. Refuse the whole runtime uninstall so
    // catalogue storage, Shape resources, and hook trampolines remain valid.
    detail::logf(
        LogLevel::warning,
        "NativeTabs hot removal was refused after carrier/viewport hook exposure; the pinned API 1.3 runtime and catalogue remain process-lifetime");
    return false;
}

bool native_tabs_installed() noexcept {
    return g_native_tabs_installed.load(std::memory_order_acquire);
}

} // namespace erui::native
#else
BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, void*) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        DisableThreadLibraryCalls(module);
        HANDLE thread = CreateThread(nullptr, 0, &run_probe, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
    }
    return TRUE;
}
#endif
