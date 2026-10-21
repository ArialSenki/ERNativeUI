#pragma once

#include "game_build.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace erui::native {

// Build-locked native interfaces are kept here so profile selection can be
// tested independently of the injected runtime. Each backend still validates
// selected function entries and applies its existing local checks to data,
// vtables, and live objects before use.
struct CoreUiAddressProfile {
    EldenRingBuild build{};
    std::uintptr_t scaleform_result_destructor_rva{};
};

// 2.7.1.1 entries repeat 2.7.1.0: every function body at these RVAs is
// byte-identical between the two images (offline comparison). 2.7.2.0 entries
// point at the same function bodies, moved (masked byte comparison against
// 2.7.1.0; data RVAs from their vtable contents and code references).
inline constexpr std::array<CoreUiAddressProfile, 4>
    kCoreUiAddressProfiles{{
        {
            EldenRingBuild::v2_7_0_0,
            0xD81590,
        },
        {
            EldenRingBuild::v2_7_1_0,
            0xD81600,
        },
        {
            EldenRingBuild::v2_7_1_1,
            0xD81600,
        },
        {
            EldenRingBuild::v2_7_2_0,
            0xD82CF0,
        },
    }};

struct TextInputAddressProfile {
    EldenRingBuild build{};
    std::uintptr_t row_producer_rva{};
    std::uintptr_t menu_string_constructor_rva{};
    std::uintptr_t menu_string_borrowed_constructor_rva{};
    std::uintptr_t menu_string_destructor_rva{};
    std::uintptr_t editor_factory_builder_rva{};
    std::uintptr_t editor_factory_rva{};
};

inline constexpr std::array<TextInputAddressProfile, 4>
    kTextInputAddressProfiles{{
        {
            EldenRingBuild::v2_7_0_0,
            0x976EF0,
            0x5EE0F0,
            0x6766F0,
            0x1BCC60,
            0x915D70,
            0x81D610,
        },
        {
            EldenRingBuild::v2_7_1_0,
            0x976EF0,
            0x5EE0F0,
            0x6766F0,
            0x1BCC60,
            0x915D70,
            0x81D610,
        },
        {
            EldenRingBuild::v2_7_1_1,
            0x976EF0,
            0x5EE0F0,
            0x6766F0,
            0x1BCC60,
            0x915D70,
            0x81D610,
        },
        {
            EldenRingBuild::v2_7_2_0,
            0x9773A0,
            0x5EE5A0,
            0x676BA0,
            0x1BCC60,
            0x916220,
            0x81DAC0,
        },
    }};

struct ColorPickerAddressProfile {
    EldenRingBuild build{};
    std::uintptr_t build_menu_window_job_rva{};
    std::uintptr_t submit_consume_rva{};
    std::uintptr_t heap_provider_rva{};
    std::uintptr_t game_allocate_rva{};
    std::uintptr_t color_palette_constructor_rva{};
    std::uintptr_t color_palette_destructor_rva{};
    std::uintptr_t color_palette_populate_rva{};
    std::uintptr_t scene_proxy_bridge_rva{};
    std::uintptr_t scene_obj_proxy_destructor_rva{};
    std::uintptr_t color_control_constructor_rva{};
    std::uintptr_t color_picker_movie_name_rva{};
    std::uintptr_t scaleform_path_resolver_rva{};
    std::uintptr_t game_options_action_widget_producer_rva{};
    std::uintptr_t scaleform_visibility_setter_rva{};
    std::uintptr_t scaleform_value_exists_rva{};
    std::uintptr_t scaleform_color_transform_setter_rva{};
};

inline constexpr std::array<ColorPickerAddressProfile, 4>
    kColorPickerAddressProfiles{{
        {
            EldenRingBuild::v2_7_0_0,
            0x7AD980,
            0x7AA0D0,
            0x7A8120,
            0x1EBBCD0,
            0x77C620,
            0x77CA10,
            0x77CF90,
            0x7460D0,
            0xD81590,
            0x8B6F90,
            0x2AB8DE8,
            0x74B140,
            0x86B940,
            0x734190,
            0x733FA0,
            0xD85610,
        },
        {
            EldenRingBuild::v2_7_1_0,
            0x7AD980,
            0x7AA0D0,
            0x7A8120,
            0x1EBBD40,
            0x77C620,
            0x77CA10,
            0x77CF90,
            0x7460D0,
            0xD81600,
            0x8B6F90,
            0x2AB8DE8,
            0x74B140,
            0x86B940,
            0x734190,
            0x733FA0,
            0xD85680,
        },
        {
            EldenRingBuild::v2_7_1_1,
            0x7AD980,
            0x7AA0D0,
            0x7A8120,
            0x1EBBD40,
            0x77C620,
            0x77CA10,
            0x77CF90,
            0x7460D0,
            0xD81600,
            0x8B6F90,
            0x2AB8DE8,
            0x74B140,
            0x86B940,
            0x734190,
            0x733FA0,
            0xD85680,
        },
        {
            EldenRingBuild::v2_7_2_0,
            0x7ADE30,
            0x7AA580,
            0x7A85D0,
            0x1EBD430,
            0x77CAD0,
            0x77CEC0,
            0x77D440,
            0x746580,
            0xD82CF0,
            0x8B7440,
            0x2ABAED8,
            0x74B5F0,
            0x86BDF0,
            0x734640,
            0x734450,
            0xD86D70,
        },
    }};

struct InputBindingsAddressProfile {
    EldenRingBuild build{};
    std::uintptr_t build_key_setting_list_rva{};
    std::uintptr_t construct_spacer_rva{};
    std::uintptr_t append_key_setting_rva{};
    std::uintptr_t construct_key_setting_rva{};
    std::uintptr_t poll_key_capture_rva{};
    std::uintptr_t clear_key_setting_rva{};
    std::uintptr_t refresh_key_conflict_rva{};
    std::uintptr_t write_menu_config_rva{};
    std::uintptr_t write_binding_value_rva{};
    std::uintptr_t input_token_physical_id_rva{};
    std::uintptr_t input_token_analog_rva{};
    std::uintptr_t get_player_input_rva{};
    std::uintptr_t query_input_states_rva{};
    std::uintptr_t input_manager_update_rva{};
    std::uintptr_t input_manager_slot_rva{};
    std::uintptr_t software_keyboard_job_construct_rva{};
    std::uintptr_t software_keyboard_job_destroy_rva{};
    std::uintptr_t text_input_dialog_construct_rva{};
    std::uintptr_t text_input_dialog_destroy_rva{};
    std::uintptr_t key_config_dialog_vtable_rva{};
    std::uintptr_t key_setting_list_vtable_rva{};
    std::uintptr_t key_config_vtable_rva{};
    std::uintptr_t software_keyboard_job_vtable_rva{};
    std::uintptr_t text_input_dialog_vtable_rva{};
};

inline constexpr std::array<InputBindingsAddressProfile, 4>
    kInputBindingsAddressProfiles{{
        {
            EldenRingBuild::v2_7_0_0,
            0x869580,
            0x8696B0,
            0x869FD0,
            0x868000,
            0x868C60,
            0x8687D0,
            0x868810,
            0x867C60,
            0x242960,
            0x240220,
            0x2403C0,
            0x2413F0,
            0x2667AD0,
            0x266A480,
            0x4861D30,
            0x81CCB0,
            0x81CE40,
            0x9B9E00,
            0x9B9FC0,
            0x2B0DCC0,
            0x2AD6E70,
            0x2AD68D0,
            0x2AC5AD0,
            0x2B2B908,
        },
        {
            EldenRingBuild::v2_7_1_0,
            0x869580,
            0x8696B0,
            0x869FD0,
            0x868000,
            0x868C60,
            0x8687D0,
            0x868810,
            0x867C60,
            0x242960,
            0x240220,
            0x2403C0,
            0x2413F0,
            0x2667B40,
            0x266A4F0,
            0x4861D30,
            0x81CCB0,
            0x81CE40,
            0x9B9E00,
            0x9B9FC0,
            0x2B0DCC0,
            0x2AD6E70,
            0x2AD68D0,
            0x2AC5AD0,
            0x2B2B908,
        },
        {
            EldenRingBuild::v2_7_1_1,
            0x869580,
            0x8696B0,
            0x869FD0,
            0x868000,
            0x868C60,
            0x8687D0,
            0x868810,
            0x867C60,
            0x242960,
            0x240220,
            0x2403C0,
            0x2413F0,
            0x2667B40,
            0x266A4F0,
            0x4861D30,
            0x81CCB0,
            0x81CE40,
            0x9B9E00,
            0x9B9FC0,
            0x2B0DCC0,
            0x2AD6E70,
            0x2AD68D0,
            0x2AC5AD0,
            0x2B2B908,
        },
        {
            EldenRingBuild::v2_7_2_0,
            0x869A30,
            0x869B60,
            0x86A480,
            0x8684B0,
            0x869110,
            0x868C80,
            0x868CC0,
            0x868110,
            0x242960,
            0x240220,
            0x2403C0,
            0x2413F0,
            0x2669270,
            0x266BC20,
            0x4864D50,
            0x81D160,
            0x81D2F0,
            0x9BA2B0,
            0x9BA470,
            0x2B0FDB0,
            0x2AD8F60,
            0x2AD89C0,
            0x2AC7BC0,
            0x2B2D9F8,
        },
    }};

// Option-menu controls and dialogs the host recognises by vtable, plus the
// live value refresh entry points. 2.7.0.0 and 2.7.1.x share every address;
// 2.7.2.0 moves the code (+0x4B0) and .rdata (+0x20F0) with identical
// function bodies and vtable contents.
struct MenuObjectAddressProfile {
    EldenRingBuild build{};
    std::uintptr_t toggle_item_constructor_rva{};
    std::uintptr_t slider_item_constructor_rva{};
    std::uintptr_t list_select_rva{};
    std::uintptr_t list_repaint_rva{};
    std::uintptr_t toggle_index_of_rva{};
    std::uintptr_t slider_refresh_rva{};
    std::uintptr_t toggle_item_vtable_rva{};
    std::uintptr_t slider_item_vtable_rva{};
    std::uintptr_t option_setting_top_dialog_vtable_rva{};
    std::uintptr_t option_setting_dialog_vtable_rva{};
    std::uintptr_t pad_setting_dialog_vtable_rva{};
    std::uintptr_t edit_property_vtable_rva{};
    std::uintptr_t text_input_controller_vtable_rva{};
    std::uintptr_t software_keyboard_job_vtable_rva{};
};

inline constexpr MenuObjectAddressProfile kMenuObjects271{
    EldenRingBuild::v2_7_1_0,
    0x94CD40, 0x94D280, 0x739B90, 0x73D0D0, 0x963A60, 0x74FA20,
    0x2B14DA8, 0x2B14D68, 0x2B16B48, 0x2B14888, 0x2B16DD8, 0x2AD73D0,
    0x2B1B128, 0x2AC5AD0,
};

[[nodiscard]] constexpr MenuObjectAddressProfile with_build(
    MenuObjectAddressProfile profile, EldenRingBuild build) noexcept {
    profile.build = build;
    return profile;
}

inline constexpr std::array<MenuObjectAddressProfile, 4>
    kMenuObjectAddressProfiles{{
        with_build(kMenuObjects271, EldenRingBuild::v2_7_0_0),
        kMenuObjects271,
        with_build(kMenuObjects271, EldenRingBuild::v2_7_1_1),
        {
            EldenRingBuild::v2_7_2_0,
            0x94D1F0, 0x94D730, 0x73A040, 0x73D580, 0x963F10, 0x74FED0,
            0x2B16E98, 0x2B16E58, 0x2B18C38, 0x2B16978, 0x2B18EC8, 0x2AD94C0,
            0x2B1D218, 0x2AC7BC0,
        },
    }};

template <typename Profile, std::size_t Count>
[[nodiscard]] constexpr const Profile* find_address_profile(
    const std::array<Profile, Count>& profiles,
    EldenRingBuild build) noexcept {
    for (const Profile& profile : profiles) {
        if (profile.build == build) return &profile;
    }
    return nullptr;
}

[[nodiscard]] constexpr const TextInputAddressProfile*
find_text_input_address_profile(EldenRingBuild build) noexcept {
    return find_address_profile(kTextInputAddressProfiles, build);
}

[[nodiscard]] constexpr const CoreUiAddressProfile*
find_core_ui_address_profile(EldenRingBuild build) noexcept {
    return find_address_profile(kCoreUiAddressProfiles, build);
}

[[nodiscard]] constexpr const ColorPickerAddressProfile*
find_color_picker_address_profile(EldenRingBuild build) noexcept {
    return find_address_profile(kColorPickerAddressProfiles, build);
}

[[nodiscard]] constexpr const InputBindingsAddressProfile*
find_input_bindings_address_profile(EldenRingBuild build) noexcept {
    return find_address_profile(kInputBindingsAddressProfiles, build);
}

[[nodiscard]] constexpr const MenuObjectAddressProfile*
find_menu_object_address_profile(EldenRingBuild build) noexcept {
    return find_address_profile(kMenuObjectAddressProfiles, build);
}

} // namespace erui::native
