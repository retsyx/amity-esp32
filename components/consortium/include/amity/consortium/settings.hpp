// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "amity/consortium/wire.hpp"
namespace amity::consortium {
bool is_settings_kind(Kind);
bool valid_settings_snapshot(const Json&);
bool valid_settings_payload(Kind,const Json&);
Error parse_device_settings_write(const Json&,DeviceSettingsWrite&);
// A whole settings_write payload: request_id and settings.
Error parse_settings_write(const Json&,DeviceSettingsWrite&);
std::string device_settings_write_json(const DeviceSettingsWrite&);
// Small bounded JSON renderer for the validated settings vocabulary only.
std::string settings_json(const Json&);
}
