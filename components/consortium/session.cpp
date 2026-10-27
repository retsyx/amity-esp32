// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#include "amity/consortium/session.hpp"
#include "amity/consortium/update.hpp"
#include "amity/consortium/relay.hpp"
#include "amity/consortium/settings.hpp"
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
Error Session::synchronized(uint64_t config,bool membership,bool bindings) {
    if(state_!=ConnectionState::synchronizing||!active_||!membership||!bindings)return Error::not_ready;
    if(config!=scope_.config_revision)return Error::stale_config;
    state_=ConnectionState::ready;execution_.bind(scope_);return Error::none;
}
void Session::disconnect(){state_=ConnectionState::disconnected;active_=false;management_=false;capabilities_=0;for(auto &v:seen_)v.valid=false;next_seen_=0;execution_.invalidate();}
void Session::restrict_lifecycle(){management_=false;execution_.invalidate();state_=ConnectionState::restricted;}
void Session::restrict_management(){restrict_lifecycle();management_=true;}
Error Session::common(const Envelope &e,uint64_t received,uint64_t now)const {
    if(state_!=ConnectionState::ready&&state_!=ConnectionState::restricted&&state_!=ConnectionState::synchronizing)return Error::not_ready;
    if(e.consortium_id!=scope_.consortium_id)return Error::wrong_consortium;
    if(e.origin_octopus_id!=scope_.peer_id)return Error::identity_conflict;
    if(e.destination_octopus_id!=scope_.local_id)return Error::target_not_local;
    if(e.origin_boot_id!=scope_.peer_boot_id||e.destination_boot_id!=scope_.local_boot_id)return Error::stale_boot;
    if(e.coordinator_epoch!=scope_.epoch)return Error::stale_epoch;
    if(e.config_revision!=scope_.config_revision)return Error::stale_config;
    if(!e.message_id||!e.ttl_ms||e.ttl_ms>10000||now<received||now-received>=e.ttl_ms)return Error::invalid_message;
    return permitted(e.kind)?Error::none:Error::unauthorized_role;
}
bool Session::permitted(Kind kind)const {
    // Enrolled devices are actuators only. Retained legacy input grants and a
    // correctly authenticated controller cannot delegate home/remote input.
    if(retired_kind(kind))return false;
    if(state_==ConnectionState::synchronizing)return active_&&role_!=PeerRole::provisional_candidate&&(kind==Kind::device_status||kind==Kind::snapshot_request||kind==Kind::snapshot_chunk||
        (role_==PeerRole::coordinator&&(kind==Kind::config_prepare||kind==Kind::config_activate))||
        (role_==PeerRole::actuator&&kind==Kind::config_result));
    if(state_==ConnectionState::restricted&&management_){if(kind==Kind::heartbeat||kind==Kind::device_status||kind==Kind::execution_lease)return true;if(is_settings_kind(kind))return role_==PeerRole::coordinator?kind==Kind::settings_read:kind==Kind::settings_result;if(is_update_kind(kind))return role_==PeerRole::coordinator?kind!=Kind::update_result:kind==Kind::update_result;return kind==Kind::membership_event||kind==Kind::membership_result;}
    if(state_==ConnectionState::restricted)return role_==PeerRole::provisional_candidate?(kind==Kind::enrollment_control||kind==Kind::heartbeat):(kind==Kind::membership_event||kind==Kind::membership_result);
    if(!active_)return false;
    if(kind==Kind::heartbeat||kind==Kind::device_status)return true;
    if(is_settings_kind(kind))return role_==PeerRole::coordinator?kind!=Kind::settings_result:role_==PeerRole::actuator&&kind==Kind::settings_result;
    switch(role_) {
    case PeerRole::coordinator:return kind==Kind::relay_reserve||kind==Kind::relay_record||kind==Kind::relay_close||kind==Kind::operation||kind==Kind::hold_release||kind==Kind::binding_update||kind==Kind::membership_event||kind==Kind::snapshot_request||kind==Kind::snapshot_chunk||kind==Kind::config_prepare||kind==Kind::config_activate||kind==Kind::plan_prepare||kind==Kind::plan_barrier||kind==Kind::plan_abort||kind==Kind::scan_request||(is_update_kind(kind)&&kind!=Kind::update_result);
    case PeerRole::actuator:return kind==Kind::relay_result||kind==Kind::execution_lease||kind==Kind::binding_result||kind==Kind::membership_result||kind==Kind::snapshot_request||kind==Kind::snapshot_chunk||kind==Kind::config_result||kind==Kind::plan_result||kind==Kind::operation_result||kind==Kind::scan_result||kind==Kind::update_result;
    case PeerRole::provisional_candidate:return false;
    }
    return false;
}
Error Session::admit_checked(const Envelope &e,uint64_t received,uint64_t now,bool remember) {
    auto err=common(e,received,now);if(err!=Error::none)return err;
    for(const auto &v:seen_)if(v.valid&&v.id==e.message_id)return Error::duplicate;
    if(remember){seen_[next_seen_]={e.message_id,now,true};next_seen_=(next_seen_+1)%seen_.size();}
    return Error::none;
}
Error Session::admit(const Envelope &e,uint64_t received,uint64_t now,bool remember) {
    auto err=admit_checked(e,received,now,false);
    if(err!=Error::none)return err;
    if((e.kind==Kind::membership_event||e.kind==Kind::membership_result)&&e.ttl_ms>2000)return Error::invalid_message;
    if((e.kind==Kind::membership_event||e.kind==Kind::membership_result)&&(!e.payload.get("phase")||e.payload.get("phase")->string!="factory_reset"))return Error::unauthorized_role;
    if(e.kind==Kind::snapshot_request){const auto* kind=e.payload.get("document_kind");if(!kind||kind->type!=Json::Type::string||(kind->string!="configuration"&&kind->string!="owner_state"))return Error::unauthorized_role;}
    if(e.kind==Kind::operation){Operation op;err=parse_operation(e.payload,op);return err==Error::none?admit_operation(e,op,received,now,remember):err;}
    if(is_settings_kind(e.kind)&&!(capabilities_&per_adapter_settings))return Error::unsupported_kind;
    if(e.kind==Kind::settings_write){model::ExecutionLeaseId lease;if(!id(e.payload.get("execution_lease_id"),lease))return Error::invalid_message;if(!execution_.valid(lease,LeaseClass::control,scope_,now))return Error::lease_expired;}
    if(e.kind==Kind::device_status&&!(capabilities_&hostname_metadata))return Error::unsupported_kind;
    if((e.kind==Kind::binding_update||e.kind==Kind::binding_result)&&!(capabilities_&binding_synchronization))return Error::unsupported_kind;
    if((e.kind==Kind::membership_event||e.kind==Kind::membership_result)&&(!(capabilities_&lifecycle_journal)||!(capabilities_&factory_reset_removal)))return Error::unsupported_kind;
    if((e.kind==Kind::snapshot_request||e.kind==Kind::snapshot_chunk)&&!(capabilities_&chunked_snapshots))return Error::unsupported_kind;
    if((e.kind==Kind::config_prepare||e.kind==Kind::config_activate||e.kind==Kind::config_result)&&!(capabilities_&configuration_activation))return Error::unsupported_kind;
    if((e.kind==Kind::plan_prepare||e.kind==Kind::plan_barrier||e.kind==Kind::plan_abort||e.kind==Kind::plan_result)&&!(capabilities_&prepared_plans))return Error::unsupported_kind;
    if(e.kind==Kind::operation_result&&!(capabilities_&operation_outcomes))return Error::unsupported_kind;
    if((e.kind==Kind::scan_request||e.kind==Kind::scan_result)&&!(capabilities_&binding_synchronization))return Error::unsupported_kind;
    if(e.kind==Kind::scan_request){model::ExecutionLeaseId lease;if(!id(e.payload.get("execution_lease_id"),lease))return Error::invalid_message;if(!execution_.valid(lease,LeaseClass::control,scope_,now))return Error::lease_expired;}
    if(is_relay_kind(e.kind)&&!(capabilities_&bootstrap_relay))return Error::unsupported_kind;
    if(is_update_kind(e.kind)&&!(capabilities_&peer_firmware_update))return Error::unsupported_kind;
    if(e.kind==Kind::hold_release&&!(capabilities_&token_holds))return Error::unsupported_kind;
    return remember?admit_checked(e,received,now,true):Error::none;
}
Error Session::admit_operation(const Envelope &e,const Operation &op,uint64_t received,uint64_t now,bool remember) {
    if(e.kind!=Kind::operation)return Error::invalid_message;
    auto err=admit_checked(e,received,now,false);if(err!=Error::none)return err;
    if(!op.command_sequence)return Error::invalid_message;
    if(!(capabilities_&sequenced_operations)||!(capabilities_&owner_repeated_keys))return Error::unsupported_kind;
    if(!(capabilities_&semantic_operations)||(op.operation==SemanticOperation::key_press&&!(capabilities_&token_holds)))return Error::unsupported_kind;
    if(op.target.port.consortium_id!=scope_.consortium_id)return Error::wrong_consortium;
    if(op.target.port.octopus_id!=scope_.local_id)return Error::target_not_local;
    const auto kind=op.operation==SemanticOperation::key_press?LeaseClass::realtime_input:LeaseClass::control;
    if(!execution_.valid(op.execution_lease_id,kind,scope_,now))return Error::lease_expired;
    return remember?admit_checked(e,received,now,true):Error::none;
}
void CommandLedger::activate(model::PlanId plan,model::SessionId session){invalidate();plan_=plan;session_=session;}
void CommandLedger::invalidate(){plan_={};session_={};highwater_=0;for(auto &v:entries_)v.valid=false;}
Error CommandLedger::receive(const Operation &op,uint64_t now){
    if(!plan_||op.plan_id!=plan_||op.session_id!=session_)return Error::stale_route;
    if(!op.command_id||!op.command_sequence)return Error::invalid_message;
    for(const auto &v:entries_)if(v.valid&&v.id==op.command_id)return v.sequence==op.command_sequence?Error::duplicate:Error::transaction_conflict;
    if(op.command_sequence<=highwater_)return Error::duplicate;
    Entry *slot=nullptr;
    for(auto &v:entries_)if(!v.valid){slot=&v;break;}
    if(!slot)for(auto &v:entries_)if(v.outcome!=Outcome::received&&now>=v.at&&(!slot||v.sequence<slot->sequence))slot=&v;
    if(!slot)return Error::queue_full;
    *slot={op.command_id,Outcome::received,now,op.command_sequence,true};
    highwater_=op.command_sequence;
    return Error::none;
}
bool CommandLedger::result(model::CommandId id,Outcome outcome,uint64_t now){for(auto &v:entries_)if(v.valid&&v.id==id){if(now<v.at)return false;v.outcome=outcome;v.at=now;return true;}return false;}
std::optional<Outcome> CommandLedger::query(model::CommandId id,uint64_t now)const {for(const auto &v:entries_)if(v.valid&&v.id==id&&now>=v.at&&now-v.at<query_window_ms)return v.outcome;return {};}
} // namespace amity::consortium
