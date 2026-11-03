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
    state_=ConnectionState::ready;return Error::none;
}
Error Session::begin_synchronization(uint64_t target,model::LinkId new_link) {
    if(!active_||role_==PeerRole::provisional_candidate||
       (state_!=ConnectionState::ready&&state_!=ConnectionState::synchronizing&&!(state_==ConnectionState::restricted&&management_)))return Error::not_ready;
    if(!(capabilities_&chunked_snapshots))return Error::unsupported_kind;
    if(!target||target<scope_.config_revision)return Error::stale_config;
    if(new_link&&new_link==scope_.link_id)return Error::identity_conflict;
    if(new_link)scope_.link_id=new_link;
    scope_.config_revision=target;management_=false;state_=ConnectionState::synchronizing;return Error::none;
}
Error Session::advance_configuration(uint64_t revision){
    if(state_!=ConnectionState::ready||!active_||role_==PeerRole::provisional_candidate)return Error::not_ready;
    if(!(capabilities_&live_configuration))return Error::unsupported_kind;
    if(revision<=scope_.config_revision)return Error::stale_config;
    scope_.config_revision=revision;return Error::none;
}
void Session::disconnect(){state_=ConnectionState::disconnected;active_=false;management_=false;capabilities_=0;}
void Session::restrict_lifecycle(){management_=false;state_=ConnectionState::restricted;}
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
    if(!e.ttl_ms||e.ttl_ms>10000||now<received||now-received>=e.ttl_ms)return Error::invalid_message;
    return permitted(e.kind,stale||synchronization_discard)?Error::none:Error::unauthorized_role;
}
bool Session::permitted(Kind kind,bool discarding)const {
    if(retired_kind(kind))return false;
    if(kind==Kind::synchronization)return active_&&role_!=PeerRole::provisional_candidate&&
        (state_==ConnectionState::ready||state_==ConnectionState::synchronizing||(state_==ConnectionState::restricted&&management_));
    if(!discarding&&state_==ConnectionState::synchronizing)return role_!=PeerRole::provisional_candidate&&(kind==Kind::device_status||kind==Kind::snapshot_request||kind==Kind::snapshot_chunk);
    if(!discarding&&state_==ConnectionState::restricted){
        if(management_){if(kind==Kind::device_status)return true;if(is_settings_kind(kind))return role_==PeerRole::coordinator?kind==Kind::settings_read:kind==Kind::settings_result;if(is_update_kind(kind))return role_==PeerRole::coordinator?kind!=Kind::update_result:kind==Kind::update_result;return kind==Kind::membership_event||kind==Kind::membership_result;}
        return role_==PeerRole::provisional_candidate?kind==Kind::enrollment_control:(kind==Kind::membership_event||kind==Kind::membership_result);
    }
    if(!active_)return false;
    if(kind==Kind::device_status)return true;
    if(kind==Kind::snapshot_request||kind==Kind::snapshot_chunk)return ((capabilities_&live_configuration)||(discarding&&state_==ConnectionState::synchronizing))&&role_!=PeerRole::provisional_candidate;
    if(is_settings_kind(kind))return role_==PeerRole::coordinator?kind!=Kind::settings_result:role_==PeerRole::actuator&&kind==Kind::settings_result;
    if(is_update_kind(kind))return role_==PeerRole::coordinator?kind!=Kind::update_result:role_==PeerRole::actuator&&kind==Kind::update_result;
    if(role_==PeerRole::coordinator)return kind==Kind::scoped_intent||kind==Kind::membership_event;
    if(role_==PeerRole::actuator)return kind==Kind::scoped_result||kind==Kind::membership_result;
    return false;
}
Error Session::admit(const Envelope& e,uint64_t received,uint64_t now){
    auto error=common(e,received,now);if(error!=Error::none)return error;
    return typed(e);
}
Error Session::typed(const Envelope& e)const{
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
        model::OwnedIntent intent;
        if(!(capabilities_&qualified_lookup)||!(capabilities_&owner_repeated_keys)||parse_owned_intent(e.payload,intent)!=Error::none)return Error::unsupported_kind;
        if(intent.kind!=model::OwnedIntentKind::key_edges&&(intent.target.port.consortium_id!=scope_.consortium_id||intent.target.port.octopus_id!=scope_.local_id))return Error::target_not_local;
    }
    if(e.kind==Kind::scoped_result&&!(capabilities_&qualified_lookup))return Error::unsupported_kind;
    if(is_settings_kind(e.kind)&&!(capabilities_&per_adapter_settings))return Error::unsupported_kind;
    if(e.kind==Kind::device_status&&!(capabilities_&hostname_metadata))return Error::unsupported_kind;
    if((e.kind==Kind::snapshot_request||e.kind==Kind::snapshot_chunk)&&!(capabilities_&chunked_snapshots))return Error::unsupported_kind;
    if(is_update_kind(e.kind)&&!(capabilities_&peer_firmware_update))return Error::unsupported_kind;
    return Error::none;
}
Error Session::discard_stale(const Envelope& e,uint64_t received,uint64_t now){
    if(state_!=ConnectionState::ready||!active_||role_==PeerRole::provisional_candidate||!(capabilities_&live_configuration))return Error::not_ready;
    auto error=common(e,received,now,true);if(error!=Error::none)return error;
    if(!valid_envelope_payload(e.kind,e.payload))return Error::invalid_message;
    error=typed(e);if(error!=Error::none)return error;
    return Error::none;
}
Error Session::discard_synchronization_frame(const Envelope& e,uint64_t received,uint64_t now){
    if(state_!=ConnectionState::synchronizing||!active_||role_==PeerRole::provisional_candidate||!(capabilities_&chunked_snapshots))return Error::not_ready;
    if(e.kind==Kind::synchronization)return Error::unsupported_kind; // The runtime must correlate control rounds explicitly.
    auto error=common(e,received,now,false,true);if(error!=Error::none)return error;
    if(!valid_envelope_payload(e.kind,e.payload))return Error::invalid_message;
    error=typed(e);if(error!=Error::none)return error;
    return Error::none;
}
Error Session::discard_lifecycle_frame(const Envelope& e,uint64_t received,uint64_t now){
    if(state_!=ConnectionState::restricted||management_||!active_||role_==PeerRole::provisional_candidate)return Error::not_ready;
    switch(e.kind){
    case Kind::device_status:case Kind::scoped_result:
    case Kind::settings_result:case Kind::update_result:break;
    default:return Error::unsupported_kind;
    }
    auto error=common(e,received,now,false,true);if(error!=Error::none)return error;
    if(!valid_envelope_payload(e.kind,e.payload))return Error::invalid_message;
    return typed(e);
}
} // namespace amity::consortium
