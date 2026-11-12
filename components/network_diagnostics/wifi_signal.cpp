// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#include "wifi_signal.hpp"
#include <algorithm>
#include <atomic>
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"

namespace amity::network_diagnostics {
namespace {
constexpr uint32_t known_bit=1u<<16,sample_interval_ms=10000;
// One word, so readers never combine fields from different samples.
DRAM_ATTR std::atomic<uint32_t> signal_word{0};
}

std::optional<int8_t> LowSignalLog::observe(int8_t rssi,uint32_t now_ms){
    if(rssi>=recovered_rssi_dbm||(!low_&&rssi>=low_wifi_rssi_dbm)){low_=false;return std::nullopt;}
    if(!low_){low_=true;episode_logged_=false;minimum_=rssi;}
    else minimum_=std::min(minimum_,rssi);
    if(logged_&&now_ms-last_log_ms_<(episode_logged_?repeat_interval_ms:episode_interval_ms))return std::nullopt;
    const auto lowest=minimum_;
    logged_=episode_logged_=true;last_log_ms_=now_ms;minimum_=0;
    return lowest;
}

WifiSignal wifi_signal(){
    const auto word=signal_word.load(std::memory_order_relaxed);
    WifiSignal signal;
    signal.known=word&known_bit;
    if(signal.known){signal.rssi=static_cast<int8_t>(static_cast<uint8_t>(word>>8));signal.channel=static_cast<uint8_t>(word);}
    return signal;
}

void poll_wifi_signal(){
    static bool sampled=false;
    static uint32_t next_ms=0;
    static LowSignalLog low;
    const auto now=static_cast<uint32_t>(esp_timer_get_time()/1000);
    if(sampled&&static_cast<int32_t>(now-next_ms)<0)return;
    sampled=true;next_ms=now+sample_interval_ms;
    wifi_ap_record_t access_point{};
    if(esp_wifi_sta_get_ap_info(&access_point)!=ESP_OK){signal_word.store(0,std::memory_order_relaxed);return;}
    signal_word.store(known_bit|static_cast<uint32_t>(static_cast<uint8_t>(access_point.rssi))<<8|access_point.primary,std::memory_order_relaxed);
    if(const auto lowest=low.observe(access_point.rssi,now))
        ESP_LOGW("network_diag","Wi-Fi signal low rssi=%d dBm min=%d dBm channel=%u",
                 static_cast<int>(access_point.rssi),static_cast<int>(*lowest),static_cast<unsigned>(access_point.primary));
}
} // namespace amity::network_diagnostics
