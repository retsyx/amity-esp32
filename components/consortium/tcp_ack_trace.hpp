// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <cstdint>

namespace amity::consortium {
enum class TcpAckSampleState {busy,missing,ready};
struct TcpAckSample {
    TcpAckSampleState state=TcpAckSampleState::missing;
    uint32_t buffered=0,acknowledged=0,rto_ms=0;
    uint8_t retransmissions=0;
};

// Observation only: one fixed record, never a send/ACK wait or a work queue.
// ACK time is an upper bound limited by the recorded successful-sample gap.
struct TcpAckTrace {
    static constexpr uint64_t observation_ms=10000;
    bool pending=false;
    uint32_t target=0,pending_bytes=0,initial_rto_ms=0,busy_samples=0;
    uint8_t max_nrtx=0;
    uint64_t started=0,last_sample=0,elapsed_ms=0,poll_gap_ms=0;

    const char* begin(const TcpAckSample& sample,uint64_t now,uint64_t sampled_at){
        *this={};started=last_sample=now;
        if(sample.state!=TcpAckSampleState::ready){elapsed_ms=sampled_at>=now?sampled_at-now:0;return "unavailable";}
        target=sample.buffered;initial_rto_ms=sample.rto_ms;pending=true;
        return observe(sample,sampled_at);
    }
    const char* observe(const TcpAckSample& sample,uint64_t now){
        if(!pending)return nullptr;
        if(now<started||now<last_sample){pending=false;return "clock_reversed";}
        elapsed_ms=now-started;poll_gap_ms=std::max(poll_gap_ms,now-last_sample);
        if(sample.state==TcpAckSampleState::missing){pending=false;return "closed";}
        if(sample.state==TcpAckSampleState::ready){
            last_sample=now;max_nrtx=std::max(max_nrtx,sample.retransmissions);
            // TCP's outstanding sequence span is smaller than half the space.
            // Unsigned arithmetic also handles an ACK crossing UINT32_MAX.
            if(uint32_t(sample.acknowledged-target)<0x80000000u){
                pending_bytes=0;pending=false;return "acked";
            }
            pending_bytes=target-sample.acknowledged;
        }else ++busy_samples;
        if(elapsed_ms>=observation_ms){pending=false;return "timeout";}
        return nullptr;
    }
};
}
