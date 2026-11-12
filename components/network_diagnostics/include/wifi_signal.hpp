// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <optional>

namespace amity::network_diagnostics {
struct WifiSignal {
    bool known=false;
    int8_t rssi=0; // dBm
    uint8_t channel=0;
};
// Below this level, interactive traffic is prone to retransmission delays.
constexpr int8_t low_wifi_rssi_dbm=-70;

// The latest station sample; unknown while disconnected. Any task may read it.
WifiSignal wifi_signal();
// One background caller, about once a second. Samples every ten seconds and
// logs only a low signal.
void poll_wifi_signal();

// Logs a low signal once per minute at most when it begins, then every ten
// minutes while it stays low. A 3 dB recovery margin keeps a signal hovering
// at the threshold from starting a new episode on every sample.
class LowSignalLog {
public:
    static constexpr int8_t recovered_rssi_dbm=low_wifi_rssi_dbm+3;
    static constexpr uint32_t episode_interval_ms=60u*1000u,repeat_interval_ms=10u*60u*1000u;
    // The lowest sample since the previous log, when this sample is logged.
    std::optional<int8_t> observe(int8_t rssi,uint32_t now_ms);
private:
    bool low_=false,episode_logged_=false,logged_=false;
    uint32_t last_log_ms_=0;
    int8_t minimum_=0;
};
} // namespace amity::network_diagnostics
