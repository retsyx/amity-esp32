/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
// The CHIP ESP32 BLE manager's hook for GATT services registered beside
// CHIPoBLE. As in the SDK, the services can be configured once; the fake host
// in gatt_fakes.cpp serves the registered services as its GATT database.
#include "platform/CHIPDeviceLayer.h"
#include <vector>

struct ble_gatt_svc_def;

namespace chip::DeviceLayer::Internal {

class BLEManagerImpl {
public:
    CHIP_ERROR ConfigureExtraServices(std::vector<struct ble_gatt_svc_def> &extGattSvcs, bool afterMatterSvc);
};

BLEManagerImpl &BLEMgrImpl();

}  // namespace chip::DeviceLayer::Internal
