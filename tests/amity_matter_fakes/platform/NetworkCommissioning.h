/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
// The network commissioning driver types Amity uses. The status values are
// checked against the SDK by sdk_parity/network_commissioning.def.
#include "lib/core/CHIPError.h"
#include "lib/support/Span.h"
#include <cstddef>
#include <cstdint>

namespace chip {
namespace app {
namespace Clusters {
namespace NetworkCommissioning {

enum class NetworkCommissioningStatusEnum : uint8_t {
    kSuccess = 0x00,
    kOutOfRange = 0x01,
    kBoundsExceeded = 0x02,
    kNetworkIDNotFound = 0x03,
    kDuplicateNetworkID = 0x04,
    kNetworkNotFound = 0x05,
    kRegulatoryError = 0x06,
    kAuthFailure = 0x07,
    kUnsupportedSecurity = 0x08,
    kOtherConnectionFailure = 0x09,
    kIPV6Failed = 0x0A,
    kIPBindFailed = 0x0B,
    kUnknownError = 0x0C,
};

}  // namespace NetworkCommissioning
}  // namespace Clusters
}  // namespace app

namespace DeviceLayer {
namespace NetworkCommissioning {

inline constexpr size_t kMaxNetworkIDLen = 32;

template <typename T> class Iterator {
public:
    virtual ~Iterator() = default;
    virtual size_t Count() = 0;
    virtual bool Next(T &item) = 0;
    virtual void Release() = 0;

protected:
    Iterator() = default;
};

struct Network {
    uint8_t networkID[kMaxNetworkIDLen];
    uint8_t networkIDLen = 0;
    bool connected = false;
};

using NetworkIterator = Iterator<Network>;
using Status = app::Clusters::NetworkCommissioning::NetworkCommissioningStatusEnum;

namespace Internal {

class WirelessDriver {
public:
    class ConnectCallback {
    public:
        virtual void OnResult(Status commissioningError, CharSpan debugText, int32_t connectStatus) = 0;
        virtual ~ConnectCallback() = default;
    };
};

}  // namespace Internal
}  // namespace NetworkCommissioning
}  // namespace DeviceLayer
}  // namespace chip
