/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
// The ESP32 Wi-Fi commissioning driver. Its members are strict seams.
#include "esp_wifi.h"
#include "platform/NetworkCommissioning.h"

namespace chip {
namespace DeviceLayer {
namespace NetworkCommissioning {

class ESPWiFiDriver final {
public:
    using ConnectCallback = Internal::WirelessDriver::ConnectCallback;

    NetworkIterator *GetNetworks();
    CHIP_ERROR CommitConfiguration();
    CHIP_ERROR RevertConfiguration();
    Status RemoveNetwork(ByteSpan networkId, MutableCharSpan &outDebugText, uint8_t &outNetworkIndex);
    void ConnectNetwork(ByteSpan networkId, ConnectCallback *callback);
    Status AddOrUpdateNetwork(ByteSpan ssid, ByteSpan credentials, MutableCharSpan &outDebugText,
                              uint8_t &outNetworkIndex);

    static ESPWiFiDriver &GetInstance() {
        static ESPWiFiDriver instance;
        return instance;
    }
};

}  // namespace NetworkCommissioning
}  // namespace DeviceLayer
}  // namespace chip
