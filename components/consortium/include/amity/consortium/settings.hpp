// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "amity/consortium/wire.hpp"
namespace amity::consortium {
struct AdapterSettingChange {uint8_t tentacle_id=0;std::string default_front_physical_address;};
struct DeviceSettingsWrite {model::BootId expected_boot;uint64_t expected_revision=0;std::vector<AdapterSettingChange> changes;};
bool is_settings_kind(Kind);
bool valid_settings_snapshot(const Json&);
bool valid_settings_payload(Kind,const Json&);
Error parse_device_settings_write(const Json&,DeviceSettingsWrite&);
std::string device_settings_write_json(const DeviceSettingsWrite&);
// Small bounded JSON renderer for the validated settings vocabulary only.
std::string settings_json(const Json&);
}
