#!/usr/bin/env python3
"""Run Amity functional tests on the Pi CEC harness."""

from amity_harness.runner import main, prepare_main, reset_main
from functional_tests.activity import (test_activity, test_activity_without_status_replies,
                                       test_activity_no_display, test_activity_switch_input,
                                       test_activity_without_receiver, test_activity_without_receiver_secondary,
                                       test_activity_nondefault_inputs,
                                       test_activity_manual_component,
                                       test_activity_manual_component_secondary,
                                       test_activity_same_osd_locations,
                                       test_activity_unselected_equipment_announcement,
                                       test_activity_keys, test_activity_keys_secondary,
                                       test_activity_keys_disconnected,
                                       test_activity_keys_disconnected_secondary)
from functional_tests.qualified_names import test_qualified_names
from functional_tests.missing_equipment import test_missing_equipment
from functional_tests.removal_cascade import test_removal_cascade, test_removal_replacement
from functional_tests.amity_replacement import test_amity_replacement
from functional_tests.default_physical_address import (
    test_default_physical_address_local, test_default_physical_address_local_scanned,
    test_default_physical_address_remote, test_default_physical_address_remote_scanned)
from functional_tests.ble_keyboard import (test_ble_keyboard_lifecycle, test_ble_keyboard_local,
                                         test_ble_keyboard_remote)
from functional_tests.matter import test_matter_accessory_local, test_matter_accessory_remote
from functional_tests.homekit import test_homekit_accessory_local, test_homekit_accessory_remote
from functional_tests.add_amity import test_add_amity
from functional_tests.hdmi_scan import test_hdmi_scan
from functional_tests.usb_reboot import test_usb_reboot
from functional_tests.member_reboot import test_activity_member_reboot
from functional_tests.hostname import test_hostname_local, test_hostname_consortium
from functional_tests.firmware_upgrade import test_firmware_upgrade, test_firmware_upgrade_main4
from functional_tests.source_theft import test_source_theft, test_source_theft_isolated
from functional_tests.dual_display import test_dual_display
from functional_tests.split_theater import test_split_theater_main_source, test_split_theater_member_source
from functional_tests.soak import test_soak, test_soak_main4
from functional_tests.consortium import (
    test_consortium, test_consortium_three_duts, test_consortium_four_duts,
    test_consortium_four_duts_main4)

if __name__ == "__main__":
    raise SystemExit(main({"hdmi_scan": test_hdmi_scan,
                           "usb_reboot": test_usb_reboot,
                           # Accessory slots are finite and retired by fixture deletion.
                           "matter": (test_matter_accessory_local, test_matter_accessory_remote),
                           "homekit": (test_homekit_accessory_local, test_homekit_accessory_remote),
                           "activity_member_reboot": test_activity_member_reboot,
                           "activity": (test_activity, test_activity_without_status_replies, test_activity_no_display,
                                        test_activity_without_receiver, test_activity_without_receiver_secondary,
                                        test_activity_nondefault_inputs,
                                        test_activity_switch_input,
                                        test_activity_manual_component,
                                        test_activity_manual_component_secondary,
                                        test_activity_same_osd_locations,
                                        test_activity_unselected_equipment_announcement,
                                        test_activity_keys, test_activity_keys_secondary,
                                        test_activity_keys_disconnected, test_activity_keys_disconnected_secondary),
                           "source_theft": test_source_theft, "add_amity": test_add_amity,
                           "consortium": test_consortium,
                           "consortium_three_duts": test_consortium_three_duts,
                           "consortium_four_duts": test_consortium_four_duts,
                           "hostname": (test_hostname_local, test_hostname_consortium),
                           "dual_display": test_dual_display,
                           "split_theater": (test_split_theater_main_source, test_split_theater_member_source),
                           "source_theft_isolated": test_source_theft_isolated,
                           "qualified_names": test_qualified_names,
                           "missing_equipment": test_missing_equipment,
                           "removal_cascade": test_removal_cascade,
                           "removal_replacement": test_removal_replacement,
                           "amity_replacement": test_amity_replacement,
                           "default_physical_address": (
                               test_default_physical_address_local, test_default_physical_address_local_scanned,
                               test_default_physical_address_remote, test_default_physical_address_remote_scanned),
                           "ble_keyboard": (test_ble_keyboard_lifecycle, test_ble_keyboard_local, test_ble_keyboard_remote)},
                          opt_in_tests={"soak": (test_soak, test_soak_main4),
                                        "consortium_four_duts_main4": test_consortium_four_duts_main4},
                          main_duts={"consortium_four_duts_main4": "amity-dut-4",
                                     "firmware_upgrade_main4": "amity-dut-4"},
                          scenario_mains={test_soak_main4: "amity-dut-4"},
                          firmware_variants={"firmware_upgrade_main4": test_firmware_upgrade_main4},
                          firmware_upgrade=test_firmware_upgrade, prepare=prepare_main, finish=reset_main))
