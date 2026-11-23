// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#include "setup_timing.hpp"
#include "esp_log.h"
namespace amity::consortium::platform {
void emit_setup_timing(const char *area,const SetupTimingEntry& entry,model::OctopusId peer,model::LinkId link,model::MessageId scan){
    if(scan){
        ESP_LOGI("setup_timing","area=%s stage=%s at_ms=%llu elapsed_ms=%llu result=%s scan=%s peer=%s code=%d",area,entry.stage,
            static_cast<unsigned long long>(entry.at_ms),static_cast<unsigned long long>(entry.elapsed_ms),entry.result,scan.str().c_str(),peer.str().c_str(),entry.code);
        return;
    }
    ESP_LOGI("setup_timing","area=%s stage=%s at_ms=%llu elapsed_ms=%llu result=%s peer=%s link=%s count=%u call_ms=%llu wait_ms=%llu code=%d metric=%s",area,entry.stage,
        static_cast<unsigned long long>(entry.at_ms),static_cast<unsigned long long>(entry.elapsed_ms),entry.result,peer.str().c_str(),link.str().c_str(),unsigned(entry.count),
        static_cast<unsigned long long>(entry.call_ms),static_cast<unsigned long long>(entry.wait_ms),entry.code,entry.summed?"sum":"span");
}
} // namespace
