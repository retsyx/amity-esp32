/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
// The device layer's managers and its System Layer.
#include "platform/ConnectivityManager.h"
#include "platform/PlatformManager.h"
#include "system/SystemLayer.h"

namespace chip {
namespace DeviceLayer {

chip::System::Layer &SystemLayer();

}  // namespace DeviceLayer
}  // namespace chip
