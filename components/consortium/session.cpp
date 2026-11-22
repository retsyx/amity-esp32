// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#include "amity/consortium/session.hpp"
#include "amity/consortium/update.hpp"
#include "amity/consortium/settings.hpp"
#include "amity/consortium/logs.hpp"
namespace amity::consortium {
Error Session::authenticate(const LinkScope &scope,model::OctopusId peer,PeerRole role,bool active) {
    disconnect();
    if(!scope.link_id||!scope.consortium_id||!scope.local_id||!scope.peer_id||!scope.local_boot_id||!scope.coordinator_id||peer!=scope.peer_id)return Error::identity_conflict;
    if(role==PeerRole::coordinator&&peer!=scope.coordinator_id)return Error::unauthorized_role;
    scope_=scope;role_=role;active_=active;state_=ConnectionState::negotiating;return Error::none;
}
Error Session::hello(const HelloFrame &h) {
    if(state_!=ConnectionState::negotiating)return Error::not_ready;
    if(h.octopus_id!=scope_.peer_id)return Error::identity_conflict;
    if(h.consortium_id!=scope_.consortium_id)return Error::wrong_consortium;
    if(!h.boot_id)return Error::stale_boot;
    if(scope_.peer_boot_id&&h.boot_id!=scope_.peer_boot_id)return Error::stale_boot;
    scope_.peer_boot_id=h.boot_id;
    if(!h.supported_tentacles||h.supported_tentacles>amity::max_tentacles||h.configured_tentacles_mask>=(1u<<h.supported_tentacles))return Error::invalid_message;
    state_=active_?ConnectionState::synchronizing:ConnectionState::restricted;return Error::none;
}
Error Session::synchronized(uint64_t config,bool membership,bool owner_metadata) {
    if(state_!=ConnectionState::synchronizing||!active_||!membership||!owner_metadata)return Error::not_ready;
    if(config!=scope_.config_revision)return Error::stale_config;
    state_=ConnectionState::ready;return Error::none;
}
Error Session::begin_synchronization(uint64_t target,model::LinkId new_link) {
    if(!active_||role_==PeerRole::provisional_candidate||
       (state_!=ConnectionState::ready&&state_!=ConnectionState::synchronizing))return Error::not_ready;
    if(!target||target<scope_.config_revision)return Error::stale_config;
    if(new_link&&new_link==scope_.link_id)return Error::identity_conflict;
    if(new_link)scope_.link_id=new_link;
    scope_.config_revision=target;management_=false;state_=ConnectionState::synchronizing;return Error::none;
}
Error Session::advance_configuration(uint64_t revision){
    if(state_!=ConnectionState::ready||!active_||role_==PeerRole::provisional_candidate)return Error::not_ready;
    if(revision<=scope_.config_revision)return Error::stale_config;
    scope_.config_revision=revision;return Error::none;
}
void Session::disconnect(){state_=ConnectionState::disconnected;active_=false;management_=false;}
void Session::restrict_lifecycle(){management_=false;state_=ConnectionState::restricted;}
void Session::restrict_management(){restrict_lifecycle();management_=true;}
Error Session::common(const Envelope &e,uint64_t received,uint64_t now,bool stale,bool synchronization_discard)const {
    if(state_!=ConnectionState::ready&&state_!=ConnectionState::restricted&&state_!=ConnectionState::synchronizing)return Error::not_ready;
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
    if(kind==Kind::synchronization)return active_&&role_!=PeerRole::provisional_candidate&&
        (state_==ConnectionState::ready||state_==ConnectionState::synchronizing);
    if(!discarding&&state_==ConnectionState::synchronizing)return role_!=PeerRole::provisional_candidate&&kind==Kind::device_status;
    if(!discarding&&state_==ConnectionState::restricted){
        if(management_){if(!active_||role_==PeerRole::provisional_candidate)return false;if(is_log_kind(kind))return role_==PeerRole::coordinator?kind==Kind::log_request:kind==Kind::log_result;if(is_update_kind(kind))return role_==PeerRole::coordinator?kind!=Kind::update_result:kind==Kind::update_result;return role_==PeerRole::coordinator?kind==Kind::membership_event:kind==Kind::membership_result;}
        return role_==PeerRole::provisional_candidate?kind==Kind::enrollment_control:(kind==Kind::membership_event||kind==Kind::membership_result);
    }
    if(!active_)return false;
    if(kind==Kind::device_status)return true;
    if(is_log_kind(kind))return role_==PeerRole::coordinator?kind==Kind::log_request:role_==PeerRole::actuator&&kind==Kind::log_result;
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
        if(!valid_envelope_payload(e.kind,e.payload))return Error::invalid_message;
        // The coordinator starts rounds and advances the configuration; the member answers.
        const auto& phase=e.payload.get("phase")->string;
        const bool begin=phase=="begin",advance=phase=="advance";
        if((begin||advance)!=(role_==PeerRole::coordinator))return Error::unauthorized_role;
        uint64_t target=0;decimal(e.payload.get("config_revision"),target);
        if((begin&&target<scope_.config_revision)||(advance&&target<=scope_.config_revision))return Error::stale_config;
    }
    // Payloads were validated when parsed; only scope-dependent rules remain.
    if(e.kind==Kind::scoped_intent){
        if(!e.intent)return Error::invalid_message;
        const auto& intent=*e.intent;
        if(intent.kind!=model::OwnedIntentKind::key_edges&&(intent.target.port.consortium_id!=scope_.consortium_id||intent.target.port.octopus_id!=scope_.local_id))return Error::target_not_local;
    }
    if(e.kind==Kind::update_chunk&&!e.update_chunk)return Error::invalid_message;
    if(e.kind==Kind::log_result&&!e.log_chunk)return Error::invalid_message;
    return Error::none;
}
Error Session::discard_stale(const Envelope& e,uint64_t received,uint64_t now){
    if(state_!=ConnectionState::ready||!active_||role_==PeerRole::provisional_candidate)return Error::not_ready;
    const auto error=common(e,received,now,true);if(error!=Error::none)return error;
    return typed(e);
}
Error Session::discard_synchronization_frame(const Envelope& e,uint64_t received,uint64_t now){
    if(state_!=ConnectionState::synchronizing||!active_||role_==PeerRole::provisional_candidate)return Error::not_ready;
    if(e.kind==Kind::synchronization)return Error::unsupported_kind; // The runtime must correlate control rounds explicitly.
    const auto error=common(e,received,now,false,true);if(error!=Error::none)return error;
    return typed(e);
}
Error Session::discard_lifecycle_frame(const Envelope& e,uint64_t received,uint64_t now){
    if(state_!=ConnectionState::restricted||management_||!active_||role_==PeerRole::provisional_candidate)return Error::not_ready;
    switch(e.kind){
    case Kind::device_status:case Kind::scoped_result:
    case Kind::settings_result:case Kind::update_result:case Kind::log_result:break;
    default:return Error::unsupported_kind;
    }
    const auto error=common(e,received,now,false,true);if(error!=Error::none)return error;
    return typed(e);
}
} // namespace amity::consortium
