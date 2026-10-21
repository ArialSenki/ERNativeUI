#include "game_build_profiles.hpp"

#include "test_assertions.hpp"

#include <cstddef>

namespace {

bool same_profile(
    const erui::native::CoreUiAddressProfile& left,
    const erui::native::CoreUiAddressProfile& right) noexcept {
    return left.build == right.build &&
        left.scaleform_result_destructor_rva ==
            right.scaleform_result_destructor_rva;
}

bool same_profile(
    const erui::native::TextInputAddressProfile& left,
    const erui::native::TextInputAddressProfile& right) noexcept {
    return left.build == right.build &&
        left.row_producer_rva == right.row_producer_rva &&
        left.menu_string_constructor_rva ==
            right.menu_string_constructor_rva &&
        left.menu_string_borrowed_constructor_rva ==
            right.menu_string_borrowed_constructor_rva &&
        left.menu_string_destructor_rva ==
            right.menu_string_destructor_rva &&
        left.editor_factory_builder_rva ==
            right.editor_factory_builder_rva &&
        left.editor_factory_rva == right.editor_factory_rva;
}

bool same_profile(
    const erui::native::ColorPickerAddressProfile& left,
    const erui::native::ColorPickerAddressProfile& right) noexcept {
    return left.build == right.build &&
        left.build_menu_window_job_rva ==
            right.build_menu_window_job_rva &&
        left.submit_consume_rva == right.submit_consume_rva &&
        left.heap_provider_rva == right.heap_provider_rva &&
        left.game_allocate_rva == right.game_allocate_rva &&
        left.color_palette_constructor_rva ==
            right.color_palette_constructor_rva &&
        left.color_palette_destructor_rva ==
            right.color_palette_destructor_rva &&
        left.color_palette_populate_rva ==
            right.color_palette_populate_rva &&
        left.scene_proxy_bridge_rva == right.scene_proxy_bridge_rva &&
        left.scene_obj_proxy_destructor_rva ==
            right.scene_obj_proxy_destructor_rva &&
        left.color_control_constructor_rva ==
            right.color_control_constructor_rva &&
        left.color_picker_movie_name_rva ==
            right.color_picker_movie_name_rva &&
        left.scaleform_path_resolver_rva ==
            right.scaleform_path_resolver_rva &&
        left.game_options_action_widget_producer_rva ==
            right.game_options_action_widget_producer_rva &&
        left.scaleform_visibility_setter_rva ==
            right.scaleform_visibility_setter_rva &&
        left.scaleform_value_exists_rva ==
            right.scaleform_value_exists_rva &&
        left.scaleform_color_transform_setter_rva ==
            right.scaleform_color_transform_setter_rva;
}

bool same_profile(
    const erui::native::InputBindingsAddressProfile& left,
    const erui::native::InputBindingsAddressProfile& right) noexcept {
    return left.build == right.build &&
        left.build_key_setting_list_rva ==
            right.build_key_setting_list_rva &&
        left.construct_spacer_rva == right.construct_spacer_rva &&
        left.append_key_setting_rva == right.append_key_setting_rva &&
        left.construct_key_setting_rva ==
            right.construct_key_setting_rva &&
        left.poll_key_capture_rva == right.poll_key_capture_rva &&
        left.clear_key_setting_rva == right.clear_key_setting_rva &&
        left.refresh_key_conflict_rva == right.refresh_key_conflict_rva &&
        left.write_menu_config_rva == right.write_menu_config_rva &&
        left.write_binding_value_rva == right.write_binding_value_rva &&
        left.input_token_physical_id_rva ==
            right.input_token_physical_id_rva &&
        left.input_token_analog_rva == right.input_token_analog_rva &&
        left.get_player_input_rva == right.get_player_input_rva &&
        left.query_input_states_rva == right.query_input_states_rva &&
        left.input_manager_update_rva == right.input_manager_update_rva &&
        left.input_manager_slot_rva == right.input_manager_slot_rva &&
        left.software_keyboard_job_construct_rva ==
            right.software_keyboard_job_construct_rva &&
        left.software_keyboard_job_destroy_rva ==
            right.software_keyboard_job_destroy_rva &&
        left.text_input_dialog_construct_rva ==
            right.text_input_dialog_construct_rva &&
        left.text_input_dialog_destroy_rva ==
            right.text_input_dialog_destroy_rva &&
        left.key_config_dialog_vtable_rva ==
            right.key_config_dialog_vtable_rva &&
        left.key_setting_list_vtable_rva ==
            right.key_setting_list_vtable_rva &&
        left.key_config_vtable_rva == right.key_config_vtable_rva &&
        left.software_keyboard_job_vtable_rva ==
            right.software_keyboard_job_vtable_rva &&
        left.text_input_dialog_vtable_rva ==
            right.text_input_dialog_vtable_rva;
}

} // namespace

int main() {
    using erui::native::EldenRingBuild;

    static_assert(
        erui::native::identify_elden_ring_build(
            0x69E9C9B9, 0x5E09600) ==
        EldenRingBuild::v2_7_0_0);
    static_assert(
        erui::native::identify_elden_ring_build(
            0x6A96B418, 0x5E0DA00) ==
        EldenRingBuild::v2_7_1_0);
    static_assert(
        erui::native::identify_elden_ring_build(
            0x6A96BA95, 0x5E0DA00) ==
        EldenRingBuild::v2_7_1_1);
    static_assert(
        erui::native::identify_elden_ring_build(
            0x6AABF7E7, 0x5DFD600) ==
        EldenRingBuild::v2_7_2_0);

    // A timestamp or image-size match by itself is not accepted. This keeps
    // an accidental hybrid profile from reaching any native call boundary.
    ERUI_TEST_CHECK(
        erui::native::identify_elden_ring_build(
            0x69E9C9B9, 0x5E0DA00) ==
        EldenRingBuild::unsupported);
    ERUI_TEST_CHECK(
        erui::native::identify_elden_ring_build(
            0x6A96B418, 0x5E09600) ==
        EldenRingBuild::unsupported);
    ERUI_TEST_CHECK(
        erui::native::identify_elden_ring_build(
            0x6A96BA95, 0x5E09600) ==
        EldenRingBuild::unsupported);
    ERUI_TEST_CHECK(
        erui::native::identify_elden_ring_build(
            0x6AABF7E7, 0x5E0DA00) ==
        EldenRingBuild::unsupported);
    ERUI_TEST_CHECK(
        erui::native::identify_elden_ring_build(0, 0) ==
        EldenRingBuild::unsupported);

    ERUI_TEST_CHECK(
        erui::native::elden_ring_build_name(
            EldenRingBuild::v2_7_0_0) == "2.7.0.0");
    ERUI_TEST_CHECK(
        erui::native::elden_ring_build_name(
            EldenRingBuild::v2_7_1_0) == "2.7.1.0");
    ERUI_TEST_CHECK(
        erui::native::elden_ring_build_name(
            EldenRingBuild::v2_7_1_1) == "2.7.1.1");
    ERUI_TEST_CHECK(
        erui::native::elden_ring_build_name(
            EldenRingBuild::v2_7_2_0) == "2.7.2.0");
    ERUI_TEST_CHECK(
        erui::native::elden_ring_build_name(
            EldenRingBuild::unsupported) == "unsupported");

    const auto* const core_270 =
        erui::native::find_core_ui_address_profile(
            EldenRingBuild::v2_7_0_0);
    const auto* const core_271 =
        erui::native::find_core_ui_address_profile(
            EldenRingBuild::v2_7_1_0);
    ERUI_TEST_CHECK(core_270 && core_271);
    ERUI_TEST_CHECK(same_profile(
        *core_270,
        erui::native::CoreUiAddressProfile{
            EldenRingBuild::v2_7_0_0, 0xD81590}));
    ERUI_TEST_CHECK(same_profile(
        *core_271,
        erui::native::CoreUiAddressProfile{
            EldenRingBuild::v2_7_1_0, 0xD81600}));

    const auto* const text_270 =
        erui::native::find_text_input_address_profile(
            EldenRingBuild::v2_7_0_0);
    const auto* const text_271 =
        erui::native::find_text_input_address_profile(
            EldenRingBuild::v2_7_1_0);
    ERUI_TEST_CHECK(text_270 && text_271);
    ERUI_TEST_CHECK(same_profile(
        *text_270,
        erui::native::TextInputAddressProfile{
            EldenRingBuild::v2_7_0_0,
            0x976EF0, 0x5EE0F0, 0x6766F0, 0x1BCC60,
            0x915D70, 0x81D610}));
    ERUI_TEST_CHECK(same_profile(
        *text_271,
        erui::native::TextInputAddressProfile{
            EldenRingBuild::v2_7_1_0,
            0x976EF0, 0x5EE0F0, 0x6766F0, 0x1BCC60,
            0x915D70, 0x81D610}));

    const auto* const color_270 =
        erui::native::find_color_picker_address_profile(
            EldenRingBuild::v2_7_0_0);
    const auto* const color_271 =
        erui::native::find_color_picker_address_profile(
            EldenRingBuild::v2_7_1_0);
    ERUI_TEST_CHECK(color_270 && color_271);
    ERUI_TEST_CHECK(same_profile(
        *color_270,
        erui::native::ColorPickerAddressProfile{
            EldenRingBuild::v2_7_0_0,
            0x7AD980, 0x7AA0D0, 0x7A8120, 0x1EBBCD0,
            0x77C620, 0x77CA10, 0x77CF90, 0x7460D0,
            0xD81590, 0x8B6F90, 0x2AB8DE8, 0x74B140,
            0x86B940, 0x734190, 0x733FA0, 0xD85610}));
    ERUI_TEST_CHECK(same_profile(
        *color_271,
        erui::native::ColorPickerAddressProfile{
            EldenRingBuild::v2_7_1_0,
            0x7AD980, 0x7AA0D0, 0x7A8120, 0x1EBBD40,
            0x77C620, 0x77CA10, 0x77CF90, 0x7460D0,
            0xD81600, 0x8B6F90, 0x2AB8DE8, 0x74B140,
            0x86B940, 0x734190, 0x733FA0, 0xD85680}));

    const auto* const input_270 =
        erui::native::find_input_bindings_address_profile(
            EldenRingBuild::v2_7_0_0);
    const auto* const input_271 =
        erui::native::find_input_bindings_address_profile(
            EldenRingBuild::v2_7_1_0);
    ERUI_TEST_CHECK(input_270 && input_271);
    ERUI_TEST_CHECK(same_profile(
        *input_270,
        erui::native::InputBindingsAddressProfile{
            EldenRingBuild::v2_7_0_0,
            0x869580, 0x8696B0, 0x869FD0, 0x868000,
            0x868C60, 0x8687D0, 0x868810, 0x867C60,
            0x242960, 0x240220, 0x2403C0, 0x2413F0,
            0x2667AD0, 0x266A480, 0x4861D30, 0x81CCB0,
            0x81CE40, 0x9B9E00, 0x9B9FC0, 0x2B0DCC0,
            0x2AD6E70, 0x2AD68D0, 0x2AC5AD0, 0x2B2B908}));
    ERUI_TEST_CHECK(same_profile(
        *input_271,
        erui::native::InputBindingsAddressProfile{
            EldenRingBuild::v2_7_1_0,
            0x869580, 0x8696B0, 0x869FD0, 0x868000,
            0x868C60, 0x8687D0, 0x868810, 0x867C60,
            0x242960, 0x240220, 0x2403C0, 0x2413F0,
            0x2667B40, 0x266A4F0, 0x4861D30, 0x81CCB0,
            0x81CE40, 0x9B9E00, 0x9B9FC0, 0x2B0DCC0,
            0x2AD6E70, 0x2AD68D0, 0x2AC5AD0, 0x2B2B908}));

    // 2.7.1.1 reuses every 2.7.1.0 address: the function bodies are
    // byte-identical between the two images.
    const auto same_as_271 = [](const auto* profile, auto expected) {
        expected.build = EldenRingBuild::v2_7_1_1;
        return profile && same_profile(*profile, expected);
    };
    ERUI_TEST_CHECK(same_as_271(
        erui::native::find_core_ui_address_profile(
            EldenRingBuild::v2_7_1_1),
        *core_271));
    ERUI_TEST_CHECK(same_as_271(
        erui::native::find_text_input_address_profile(
            EldenRingBuild::v2_7_1_1),
        *text_271));
    ERUI_TEST_CHECK(same_as_271(
        erui::native::find_color_picker_address_profile(
            EldenRingBuild::v2_7_1_1),
        *color_271));
    ERUI_TEST_CHECK(same_as_271(
        erui::native::find_input_bindings_address_profile(
            EldenRingBuild::v2_7_1_1),
        *input_271));

    // Menu objects: 2.7.0.0 and 2.7.1.x share every address; 2.7.2.0 moves
    // code by +0x4B0 and .rdata by +0x20F0.
    {
        const auto* const menu_271 =
            erui::native::find_menu_object_address_profile(
                EldenRingBuild::v2_7_1_0);
        const auto* const menu_272 =
            erui::native::find_menu_object_address_profile(
                EldenRingBuild::v2_7_2_0);
        ERUI_TEST_CHECK(menu_271 && menu_272);
        for (const EldenRingBuild build :
             {EldenRingBuild::v2_7_0_0, EldenRingBuild::v2_7_1_1}) {
            const auto* const same =
                erui::native::find_menu_object_address_profile(build);
            ERUI_TEST_CHECK(same && same->build == build &&
                same->toggle_item_constructor_rva == 0x94CD40 &&
                same->option_setting_top_dialog_vtable_rva == 0x2B16B48 &&
                same->software_keyboard_job_vtable_rva == 0x2AC5AD0);
        }
        ERUI_TEST_CHECK(menu_271->toggle_item_constructor_rva == 0x94CD40 &&
            menu_271->slider_item_constructor_rva == 0x94D280 &&
            menu_271->list_select_rva == 0x739B90 &&
            menu_271->list_repaint_rva == 0x73D0D0 &&
            menu_271->toggle_index_of_rva == 0x963A60 &&
            menu_271->slider_refresh_rva == 0x74FA20 &&
            menu_271->toggle_item_vtable_rva == 0x2B14DA8 &&
            menu_271->slider_item_vtable_rva == 0x2B14D68 &&
            menu_271->option_setting_top_dialog_vtable_rva == 0x2B16B48 &&
            menu_271->option_setting_dialog_vtable_rva == 0x2B14888 &&
            menu_271->pad_setting_dialog_vtable_rva == 0x2B16DD8 &&
            menu_271->edit_property_vtable_rva == 0x2AD73D0 &&
            menu_271->text_input_controller_vtable_rva == 0x2B1B128 &&
            menu_271->software_keyboard_job_vtable_rva == 0x2AC5AD0);
        ERUI_TEST_CHECK(menu_272->toggle_item_constructor_rva == 0x94D1F0 &&
            menu_272->slider_item_constructor_rva == 0x94D730 &&
            menu_272->list_select_rva == 0x73A040 &&
            menu_272->list_repaint_rva == 0x73D580 &&
            menu_272->toggle_index_of_rva == 0x963F10 &&
            menu_272->slider_refresh_rva == 0x74FED0 &&
            menu_272->toggle_item_vtable_rva == 0x2B16E98 &&
            menu_272->slider_item_vtable_rva == 0x2B16E58 &&
            menu_272->option_setting_top_dialog_vtable_rva == 0x2B18C38 &&
            menu_272->option_setting_dialog_vtable_rva == 0x2B16978 &&
            menu_272->pad_setting_dialog_vtable_rva == 0x2B18EC8 &&
            menu_272->edit_property_vtable_rva == 0x2AD94C0 &&
            menu_272->text_input_controller_vtable_rva == 0x2B1D218 &&
            menu_272->software_keyboard_job_vtable_rva == 0x2AC7BC0);
        // The input-binding profile carries the same keyboard job vtable.
        ERUI_TEST_CHECK(
            erui::native::find_input_bindings_address_profile(
                EldenRingBuild::v2_7_2_0)->software_keyboard_job_vtable_rva ==
            menu_272->software_keyboard_job_vtable_rva);
        ERUI_TEST_CHECK(
            erui::native::find_menu_object_address_profile(
                EldenRingBuild::unsupported) == nullptr);
    }

    // 2.7.2.0: the same function bodies, moved.
    ERUI_TEST_CHECK(same_profile(
        *erui::native::find_core_ui_address_profile(
            EldenRingBuild::v2_7_2_0),
        erui::native::CoreUiAddressProfile{
            EldenRingBuild::v2_7_2_0, 0xD82CF0}));
    ERUI_TEST_CHECK(same_profile(
        *erui::native::find_text_input_address_profile(
            EldenRingBuild::v2_7_2_0),
        erui::native::TextInputAddressProfile{
            EldenRingBuild::v2_7_2_0,
            0x9773A0, 0x5EE5A0, 0x676BA0, 0x1BCC60,
            0x916220, 0x81DAC0}));
    ERUI_TEST_CHECK(same_profile(
        *erui::native::find_color_picker_address_profile(
            EldenRingBuild::v2_7_2_0),
        erui::native::ColorPickerAddressProfile{
            EldenRingBuild::v2_7_2_0,
            0x7ADE30, 0x7AA580, 0x7A85D0, 0x1EBD430,
            0x77CAD0, 0x77CEC0, 0x77D440, 0x746580,
            0xD82CF0, 0x8B7440, 0x2ABAED8, 0x74B5F0,
            0x86BDF0, 0x734640, 0x734450, 0xD86D70}));
    ERUI_TEST_CHECK(same_profile(
        *erui::native::find_input_bindings_address_profile(
            EldenRingBuild::v2_7_2_0),
        erui::native::InputBindingsAddressProfile{
            EldenRingBuild::v2_7_2_0,
            0x869A30, 0x869B60, 0x86A480, 0x8684B0,
            0x869110, 0x868C80, 0x868CC0, 0x868110,
            0x242960, 0x240220, 0x2403C0, 0x2413F0,
            0x2669270, 0x266BC20, 0x4864D50, 0x81D160,
            0x81D2F0, 0x9BA2B0, 0x9BA470, 0x2B0FDB0,
            0x2AD8F60, 0x2AD89C0, 0x2AC7BC0, 0x2B2D9F8}));

    ERUI_TEST_CHECK(
        erui::native::find_core_ui_address_profile(
            EldenRingBuild::unsupported) == nullptr);
    ERUI_TEST_CHECK(
        erui::native::find_text_input_address_profile(
            EldenRingBuild::unsupported) == nullptr);
    ERUI_TEST_CHECK(
        erui::native::find_color_picker_address_profile(
            EldenRingBuild::unsupported) == nullptr);
    ERUI_TEST_CHECK(
        erui::native::find_input_bindings_address_profile(
            EldenRingBuild::unsupported) == nullptr);

    for (std::size_t left = 0;
         left < erui::native::kSupportedEldenRingBuilds.size(); ++left) {
        const auto& identity =
            erui::native::kSupportedEldenRingBuilds[left];
        ERUI_TEST_CHECK(
            erui::native::find_elden_ring_build(
                identity.pe_timestamp, identity.image_size) == &identity);
        for (std::size_t right = left + 1;
             right < erui::native::kSupportedEldenRingBuilds.size();
             ++right) {
            const auto& other =
                erui::native::kSupportedEldenRingBuilds[right];
            ERUI_TEST_CHECK(identity.build != other.build);
            ERUI_TEST_CHECK(
                identity.pe_timestamp != other.pe_timestamp ||
                identity.image_size != other.image_size);
        }
    }

    return 0;
}
