#pragma once

namespace ble {
inline bool pairing_candidate(bool hid, bool connectable, bool attempted,
                              int rssi, int minimum_rssi) {
    return hid && connectable && !attempted && rssi >= minimum_rssi;
}
} // namespace ble
