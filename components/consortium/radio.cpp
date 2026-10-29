// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#include "amity/consortium/radio.hpp"
namespace amity::consortium {
RadioResult RadioArbiter::transition(RadioMode next,uint64_t now,uint32_t duration){
    if(now>UINT64_MAX-duration||now>UINT64_MAX-cancel_ms||generation_==UINT64_MAX)return RadioResult::unavailable;
    ++generation_;owner_=RadioMode::transition;next_=next;deadline_=now+duration;cancel_deadline_=now+cancel_ms;return RadioResult::cancelling;
}
RadioResult RadioArbiter::request(RadioMode requested,uint64_t now,uint32_t duration,bool gates){
    const uint32_t maximum=requested==RadioMode::matter_induction?900000:enrollment_ms;
    if(requested==RadioMode::operational||requested==RadioMode::transition||!duration||duration>maximum||now>UINT64_MAX-duration)return RadioResult::unavailable;
    if(gates&&(requested==RadioMode::consortium_candidate||requested==RadioMode::consortium_inviter)&&!consortium_radio_available)return RadioResult::unavailable;
    if(owner_!=RadioMode::operational||pase_active_)return RadioResult::busy;
    if(remote_reconnect_)return transition(requested,now,duration);
    if(generation_==UINT64_MAX)return RadioResult::unavailable;
    owner_=requested;++generation_;deadline_=now+duration;return RadioResult::granted;
}
RadioResult RadioArbiter::cleanup_complete(uint64_t generation,uint64_t now){
    if(generation!=generation_||owner_!=RadioMode::transition)return RadioResult::stale;
    const bool expired=now>=cancel_deadline_||now>=deadline_;
    // Cleanup is now proven even if its caller's bounded wait has expired.
    // Consume it without reviving the expired setup reservation, so the radio
    // worker can resume reconnect and a later request can reserve fresh work.
    remote_reconnect_=false;setup_handle_.reset();owner_=expired?RadioMode::operational:next_;
    if(owner_==RadioMode::operational){next_=RadioMode::operational;deadline_=cancel_deadline_=0;}
    return expired?RadioResult::timed_out:RadioResult::granted;
}
RadioResult RadioArbiter::release(uint64_t generation,uint64_t now){
    if(generation!=generation_||owner_==RadioMode::operational)return RadioResult::stale;
    if(owner_==RadioMode::matter_induction&&pase_active_)return RadioResult::busy;
    return transition(RadioMode::operational,now,cancel_ms+1);
}
RadioResult RadioArbiter::admit_setup_connection(uint64_t generation,uint16_t handle){
    if(!accepts_callback(generation))return RadioResult::stale;
    if(setup_handle_)return *setup_handle_==handle?RadioResult::granted:RadioResult::busy;
    // One remote plus one setup leaves the third configured slot for cleanup.
    setup_handle_=handle;return RadioResult::granted;
}
void RadioArbiter::setup_disconnected(uint64_t generation,uint16_t handle){if(generation==generation_&&setup_handle_&&*setup_handle_==handle)setup_handle_.reset();}
RadioResult RadioArbiter::expire(uint64_t now){
    if(owner_==RadioMode::operational)return RadioResult::granted;
    if(owner_==RadioMode::transition)return now>=cancel_deadline_?RadioResult::timed_out:RadioResult::cancelling;
    if(now<deadline_)return RadioResult::granted;
    if(owner_==RadioMode::matter_induction&&pase_active_)return RadioResult::busy;
    return release(generation_,now);
}
RadioResult RadioArbiter::external_matter_will_open(uint64_t now,uint32_t duration,bool active){
    pase_active_=active;
    if(owner_==RadioMode::matter_induction)return RadioResult::granted;
    if(owner_==RadioMode::transition)return RadioResult::busy;
    if(!duration||duration>900000)return RadioResult::unavailable;
    if(owner_==RadioMode::operational&&!remote_reconnect_)return request(RadioMode::matter_induction,now,duration,false);
    return transition(RadioMode::matter_induction,now,duration);
}
bool consortium_setup_eligible(const SetupEligibility &e){return !e.active_membership&&!e.prepared_join&&!e.retired&&!e.recovery_required&&e.physical_authorization_supported&&e.radio_available;}
} // namespace amity::consortium
