// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "amity/system/model.hpp"
namespace amity::system {
// Owner-local discovery cache reports. Runtime addresses and availability are
// neither persisted nor used as additional execution authority.
struct CecObservation {
    FullyQualifiedCecAddress endpoint;
    std::optional<std::string> osd_name;
    BootId owner_boot_id;
    uint64_t local_sequence = 0;
    uint16_t physical_address = 0xffff;
    ScanId scan_id;
    bool stale = false;
    uint8_t device_type = 0xff;
    uint32_t vendor_id = 0;
};
}
