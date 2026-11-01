// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#include "amity/consortium/session.hpp"
#include "amity/consortium/update.hpp"
#include "amity/consortium/settings.hpp"
#include "amity/consortium/owned_intent.hpp"
namespace amity::consortium {
Error Session::authenticate(const LinkScope &scope,model::OctopusId peer,PeerRole role,bool active) {
    disconnect();
    if(!scope.link_id||!scope.consortium_id||!scope.local_id||!scope.peer_id||!scope.local_boot_id||!scope.coordinator_id||peer!=scope.peer_id)return Error::identity_conflict;
    if(role==PeerRole::coordinator&&peer!=scope.coordinator_id)return Error::unauthorized_role;
    if(scope.epoch!=1)return Error::stale_epoch;
    scope_=scope;role_=role;active_=active;state_=ConnectionState::negotiating;return Error::none;
}
Error Session::hello(const HelloFrame &h,uint32_t supported) {
    if(state_!=ConnectionState::negotiating)return Error::not_ready;
    if(h.octopus_id!=scope_.peer_id)return Error::identity_conflict;
    if(h.consortium_id!=scope_.consortium_id)return Error::wrong_consortium;
    if(!h.boot_id)return Error::stale_boot;
    if(scope_.peer_boot_id&&h.boot_id!=scope_.peer_boot_id)return Error::stale_boot;
    scope_.peer_boot_id=h.boot_id;
    if(h.coordinator_epoch!=scope_.epoch)return Error::stale_epoch;
    if(!h.supported_tentacles||h.supported_tentacles>amity::max_tentacles||h.configured_tentacles_mask>=(1u<<h.supported_tentacles))return Error::invalid_message;
    if(h.protocol_min>1||h.protocol_max<1||(h.required_capabilities&~supported)||((h.required_capabilities|supported)&~known_capabilities))return Error::protocol_mismatch;
    capabilities_=(h.required_capabilities|h.optional_capabilities)&supported;
    state_=active_?ConnectionState::synchronizing:ConnectionState::restricted;return Error::none;
}
Error Session::synchronized(uint64_t config,bool membership,bool owner_metadata) {
    if(state_!=ConnectionState::synchronizing||!active_||!membership||!owner_metadata)return Error::not_ready;
    if(config!=scope_.config_revision)return Error::stale_config;
    state_=ConnectionState::ready;execution_.bind(scope_);return Error::none;
}
Error Session::begin_synchronization(uint64_t target,model::LinkId new_link) {
    if(!active_||role_==PeerRole::provisional_candidate||
       (state_!=ConnectionState::ready&&state_!=ConnectionState::synchronizing&&!(state_==ConnectionState::restricted&&management_)))return Error::not_ready;
    if(!(capabilities_&chunked_snapshots))return Error::unsupported_kind;
    if(!target||target<scope_.config_revision)return Error::stale_config;
    if(new_link&&new_link==scope_.link_id)return Error::identity_conflict;
    if(new_link)scope_.link_id=new_link;
    scope_.config_revision=target;execution_.invalidate();management_=false;state_=ConnectionState::synchronizing;return Error::none;
}
Error Session::advance_configuration(uint64_t revision){
    if(state_!=ConnectionState::ready||!active_||role_==PeerRole::provisional_candidate)return Error::not_ready;
    if(!(capabilities_&live_configuration))return Error::unsupported_kind;
    if(revision<=scope_.config_revision)return Error::stale_config;
    scope_.config_revision=revision;execution_.bind(scope_);return Error::none;
}
void Session::disconnect(){state_=ConnectionState::disconnected;active_=false;management_=false;capabilities_=0;for(auto &v:seen_)v.valid=false;next_seen_=0;execution_.invalidate();}
void Session::restrict_lifecycle(){management_=false;execution_.invalidate();state_=ConnectionState::restricted;}
void Session::restrict_management(){restrict_lifecycle();management_=true;}
Error Session::common(const Envelope &e,uint64_t received,uint64_t now,bool stale,bool synchronization_discard)const {
    if(state_!=ConnectionState::ready&&state_!=ConnectionState::restricted&&state_!=ConnectionState::synchronizing)return Error::not_ready;
    if(e.consortium_id!=scope_.consortium_id)return Error::wrong_consortium;
    if(e.origin_octopus_id!=scope_.peer_id)return Error::identity_conflict;
    if(e.destination_octopus_id!=scope_.local_id)return Error::target_not_local;
    if(e.origin_boot_id!=scope_.peer_boot_id||e.destination_boot_id!=scope_.local_boot_id)return Error::stale_boot;
    if(e.coordinator_epoch!=scope_.epoch)return Error::stale_epoch;
    // Synchronization barriers reconcile only the current authenticated stream.
    // The runtime correlates their exact round/target; ordinary work retains
    // the exact configuration fence even if a previous exchange partly applied.
    if(synchronization_discard){if(!e.config_revision||e.config_revision>scope_.config_revision)return Error::stale_config;}
    else if(e.kind==Kind::synchronization&&!stale){if(!e.config_revision)return Error::stale_config;}
    else if(stale?(!e.config_revision||e.config_revision>=scope_.config_revision):e.config_revision!=scope_.config_revision)return Error::stale_config;
    if(!e.message_id||!e.ttl_ms||e.ttl_ms>10000||now<received||now-received>=e.ttl_ms)return Error::invalid_message;
    return permitted(e.kind,stale||synchronization_discard)?Error::none:Error::unauthorized_role;
}
bool Session::permitted(Kind kind,bool discarding)const {
    if(retired_kind(kind))return false;
    if(kind==Kind::synchronization)return active_&&role_!=PeerRole::provisional_candidate&&
        (state_==ConnectionState::ready||state_==ConnectionState::synchronizing||(state_==ConnectionState::restricted&&management_));
    if(!discarding&&state_==ConnectionState::synchronizing)return role_!=PeerRole::provisional_candidate&&(kind==Kind::heartbeat||kind==Kind::device_status||kind==Kind::snapshot_request||kind==Kind::snapshot_chunk);
    if(!discarding&&state_==ConnectionState::restricted){
        if(management_){if(kind==Kind::heartbeat||kind==Kind::device_status||kind==Kind::execution_lease)return true;if(is_settings_kind(kind))return role_==PeerRole::coordinator?kind==Kind::settings_read:kind==Kind::settings_result;if(is_update_kind(kind))return role_==PeerRole::coordinator?kind!=Kind::update_result:kind==Kind::update_result;return kind==Kind::membership_event||kind==Kind::membership_result;}
        return role_==PeerRole::provisional_candidate?(kind==Kind::enrollment_control||kind==Kind::heartbeat):(kind==Kind::membership_event||kind==Kind::membership_result);
    }
    if(!active_)return false;
    if(kind==Kind::heartbeat||kind==Kind::device_status)return true;
    if(kind==Kind::snapshot_request||kind==Kind::snapshot_chunk)return ((capabilities_&live_configuration)||(discarding&&state_==ConnectionState::synchronizing))&&role_!=PeerRole::provisional_candidate;
    if(is_settings_kind(kind))return role_==PeerRole::coordinator?kind!=Kind::settings_result:role_==PeerRole::actuator&&kind==Kind::settings_result;
    if(is_update_kind(kind))return role_==PeerRole::coordinator?kind!=Kind::update_result:role_==PeerRole::actuator&&kind==Kind::update_result;
    if(role_==PeerRole::coordinator)return kind==Kind::scoped_intent||kind==Kind::membership_event;
    if(role_==PeerRole::actuator)return kind==Kind::scoped_result||kind==Kind::execution_lease||kind==Kind::membership_result;
    return false;
}
Error Session::admit_checked(const Envelope& e,uint64_t received,uint64_t now,bool remember){
    auto error=common(e,received,now);if(error!=Error::none)return error;
    return replay(e,now,remember);
}
Error Session::replay(const Envelope& e,uint64_t now,bool remember){
    for(const auto& entry:seen_)if(entry.valid&&entry.id==e.message_id)return Error::duplicate;
    if(remember){seen_[next_seen_]={e.message_id,now,true};next_seen_=(next_seen_+1)%seen_.size();}
    return Error::none;
}
Error Session::admit(const Envelope& e,uint64_t received,uint64_t now,bool remember){
    auto error=admit_checked(e,received,now,false);if(error!=Error::none)return error;
    error=typed(e,now,true);if(error!=Error::none)return error;
    return remember?replay(e,now,true):Error::none;
}
Error Session::typed(const Envelope& e,uint64_t now,bool leases)const{
    if(e.kind==Kind::synchronization){
        if(!(capabilities_&chunked_snapshots))return Error::unsupported_kind;
        if(!valid_envelope_payload(e.kind,e.payload))return Error::invalid_message;
        const bool begin=e.payload.get("phase")->string=="begin";
        if(begin!=(role_==PeerRole::coordinator))return Error::unauthorized_role;
        uint64_t target=0;decimal(e.payload.get("config_revision"),target);
        if(begin&&target<scope_.config_revision)return Error::stale_config;
    }
    if((e.kind==Kind::membership_event||e.kind==Kind::membership_result)&&(!(capabilities_&lifecycle_journal)||!(capabilities_&factory_reset_removal)||e.ttl_ms>2000))return Error::unsupported_kind;
    if(e.kind==Kind::scoped_intent){
        model::OwnedIntent intent;model::ExecutionLeaseId lease;
        if(!(capabilities_&qualified_lookup)||!(capabilities_&owner_repeated_keys)||parse_owned_intent(e.payload,intent,lease)!=Error::none)return Error::unsupported_kind;
        if(intent.target.port.consortium_id!=scope_.consortium_id||intent.target.port.octopus_id!=scope_.local_id)return Error::target_not_local;
        // Up is exact-token cleanup, independent of positive-work authority.
        if(leases&&intent.kind!=model::OwnedIntentKind::key_up){const auto cls=intent.kind==model::OwnedIntentKind::key_down?LeaseClass::realtime_input:LeaseClass::control;if(!execution_.valid(lease,cls,scope_,now))return Error::lease_expired;}
    }
    if(e.kind==Kind::scoped_result&&!(capabilities_&qualified_lookup))return Error::unsupported_kind;
    if(is_settings_kind(e.kind)&&!(capabilities_&per_adapter_settings))return Error::unsupported_kind;
    if(e.kind==Kind::settings_write){model::ExecutionLeaseId lease;if(!id(e.payload.get("execution_lease_id"),lease)||(leases&&!execution_.valid(lease,LeaseClass::control,scope_,now)))return Error::lease_expired;}
    if(e.kind==Kind::device_status&&!(capabilities_&hostname_metadata))return Error::unsupported_kind;
    if((e.kind==Kind::snapshot_request||e.kind==Kind::snapshot_chunk)&&!(capabilities_&chunked_snapshots))return Error::unsupported_kind;
    if(is_update_kind(e.kind)&&!(capabilities_&peer_firmware_update))return Error::unsupported_kind;
    return Error::none;
}
Error Session::discard_stale(const Envelope& e,uint64_t received,uint64_t now,bool remember){
    if(state_!=ConnectionState::ready||!active_||role_==PeerRole::provisional_candidate||!(capabilities_&live_configuration))return Error::not_ready;
    auto error=common(e,received,now,true);if(error!=Error::none)return error;
    if(!valid_envelope_payload(e.kind,e.payload))return Error::invalid_message;
    error=typed(e,now,false);if(error!=Error::none)return error;
    if(e.kind==Kind::execution_lease){model::LinkId link;if(!id(e.payload.get("link_id"),link)||link!=scope_.link_id)return Error::identity_conflict;}
    return replay(e,now,remember);
}
Error Session::discard_synchronization_frame(const Envelope& e,uint64_t received,uint64_t now,bool remember){
    if(state_!=ConnectionState::synchronizing||!active_||role_==PeerRole::provisional_candidate||!(capabilities_&chunked_snapshots))return Error::not_ready;
    if(e.kind==Kind::synchronization)return Error::unsupported_kind; // The runtime must correlate control rounds explicitly.
    auto error=common(e,received,now,false,true);if(error!=Error::none)return error;
    if(!valid_envelope_payload(e.kind,e.payload))return Error::invalid_message;
    error=typed(e,now,false);if(error!=Error::none)return error;
    if(e.kind==Kind::execution_lease){model::LinkId link;if(!id(e.payload.get("link_id"),link)||link!=scope_.link_id)return Error::identity_conflict;}
    return replay(e,now,remember);
}
} // namespace amity::consortium
