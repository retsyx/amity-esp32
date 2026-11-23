/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
// The connectivity manager's BLE advertising switch, a strict seam. In the SDK
// it reaches BLEManagerImpl::_SetAdvertisingEnabled, which the firmware wraps
// at link time; a scenario that needs that path scripts it.
#include "lib/core/CHIPError.h"

namespace chip {
namespace DeviceLayer {

class ConnectivityManager {
public:
    CHIP_ERROR SetBLEAdvertisingEnabled(bool val);
};

ConnectivityManager &ConnectivityMgr();

}  // namespace DeviceLayer
}  // namespace chip
