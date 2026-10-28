// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <optional>
namespace amity::consortium {
enum class RadioMode { operational, matter_induction, remote_pairing, consortium_candidate, consortium_inviter, transition };
enum class RadioResult { granted, busy, cancelling, stale, timed_out, unavailable };
struct RadioReservation {RadioMode owner=RadioMode::operational;uint64_t generation=0,deadline_ms=0;};
class RadioArbiter {
public:
    static constexpr uint32_t enrollment_ms=180000,discovery_ms=20000,connection_ms=10000,cancel_ms=5000;
    // This scheduling core is ready for integration. Actual consortium GAP is
    // gated until encrypted admission and external Matter handoff pass.
    static constexpr bool consortium_radio_available=false;
    RadioResult request(RadioMode,uint64_t now_ms,uint32_t duration_ms,bool enforce_release_gates=true);
    void remote_reconnect_running(bool running){remote_reconnect_=running;}
    void remote_connected(bool connected){remote_connected_=connected;}
    RadioResult cleanup_complete(uint64_t generation,uint64_t now_ms);
    RadioResult release(uint64_t generation,uint64_t now_ms);
    RadioResult admit_setup_connection(uint64_t generation,uint16_t handle);
    void setup_disconnected(uint64_t generation,uint16_t handle);
    RadioResult expire(uint64_t now_ms);
    // Must be called synchronously before Matter starts advertising. The SDK's
    // post-open callback is too late, so no production adapter is supplied.
    RadioResult external_matter_will_open(uint64_t now_ms,uint32_t duration_ms,bool pase_active);
    void matter_pase_active(bool active){pase_active_=active;}
    RadioReservation reservation()const{return {owner_,generation_,deadline_};}
    bool accepts_callback(uint64_t generation)const{return generation==generation_&&owner_!=RadioMode::transition&&owner_!=RadioMode::operational;}
    bool cancellation_pending()const{return owner_==RadioMode::transition;}
    bool remote_may_reconnect()const{return owner_==RadioMode::operational&&!pase_active_;}
private:
    RadioResult transition(RadioMode,uint64_t,uint32_t);
    RadioMode owner_=RadioMode::operational,next_=RadioMode::operational;
    uint64_t generation_=0,deadline_=0,cancel_deadline_=0;
    bool remote_reconnect_=false,remote_connected_=false,pase_active_=false;
    std::optional<uint16_t> setup_handle_;
};
struct SetupEligibility {
    bool fresh_unconfigured_singleton=false;
    bool active_membership=false,prepared_join=false,operational_configuration=false,retired=false,recovery_required=false;
    bool physical_authorization_supported=false,radio_available=false;
};
bool initial_matter_eligible(const SetupEligibility &);
bool consortium_setup_eligible(const SetupEligibility &);
} // namespace amity::consortium
