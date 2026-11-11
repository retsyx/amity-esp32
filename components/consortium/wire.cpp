// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#include "amity/consortium/wire.hpp"
#include "amity/consortium/owned_intent.hpp"
#include "amity/consortium/transfer.hpp"
#include "amity/consortium/update.hpp"
#include "amity/consortium/hostname.hpp"
#include "amity/consortium/settings.hpp"
#include "amity/consortium/logs.hpp"
#include <algorithm>

namespace amity::consortium {
namespace {
bool text_is(const Json *v, std::initializer_list<std::string_view> allowed) {return v&&v->type==Json::Type::string&&std::find(allowed.begin(),allowed.end(),v->string)!=allowed.end();}
bool hex(const Json *v,size_t length) {return v&&v->type==Json::Type::string&&v->string.size()==length&&std::all_of(v->string.begin(),v->string.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');});}
bool generic_id(const Json *v) { model::MessageId ignored;return id(v,ignored); }
bool uint_field(const Json &v,std::string_view key,uint64_t low,uint64_t high) {uint64_t n;return json_uint(v.get(key),n,low,high);}
bool bool_field(const Json &v,std::string_view key) {bool b;return json_bool(v.get(key),b);}
bool decimal_field(const Json &v,std::string_view key) {uint64_t n;return decimal(v.get(key),n);}
bool ids(const Json &v,std::initializer_list<std::string_view> keys) {for(auto key:keys)if(!generic_id(v.get(key)))return false;return true;}
bool enrollment(const Json &v) {
    if(!ids(v,{"join_id"})||!hex(v.get("join_digest"),64))return false;
    const auto *phase=v.get("phase");
    if(text_is(phase,{"challenge","prove"}))return closed(v,{"phase","join_id","join_digest","challenge_nonce"})&&generic_id(v.get("challenge_nonce"));
    if(text_is(phase,{"commit"}))return closed(v,{"phase","join_id","join_digest","membership_revision","ready"})&&decimal_field(v,"membership_revision")&&bool_field(v,"ready")&&!v.get("ready")->boolean;
    if(text_is(phase,{"ack"}))return closed(v,{"phase","join_id","join_digest","membership_revision","state"})&&decimal_field(v,"membership_revision")&&text_is(v.get("state"),{"active"});
    if(text_is(phase,{"abort"}))return closed(v,{"phase","join_id","join_digest","reason"})&&text_is(v.get("reason"),{"cancelled","incompatible","network_failed","internal_error"});
    if(!text_is(phase,{"status"})||!closed(v,{"phase","join_id","join_digest","request"},{"state","membership_revision"})||!bool_field(v,"request"))return false;
    if(v.get("request")->boolean)return !v.get("state")&&!v.get("membership_revision");
    if(!text_is(v.get("state"),{"prepared","committed","active","aborted","unknown"}))return false;
    return text_is(v.get("state"),{"committed","active"})?decimal_field(v,"membership_revision"):!v.get("membership_revision");
}
bool membership(const Json &v) {
    return closed(v,{"mutation_id","proposal_digest","phase","membership_revision","expected_boot_id"},{"result"})&&generic_id(v.get("expected_boot_id"))&&generic_id(v.get("mutation_id"))&&hex(v.get("proposal_digest"),64)&&decimal_field(v,"membership_revision")&&text_is(v.get("phase"),{"factory_reset"})&&(!v.get("result")||text_is(v.get("result"),{"reset_scheduled","rejected","unknown"}));
}
bool payload(Kind kind,const Json &v) {
    switch(kind) {
    case Kind::log_request:case Kind::log_result:return valid_log_payload(kind,v);
    case Kind::settings_read:case Kind::settings_write:case Kind::settings_result:return valid_settings_payload(kind,v);
    case Kind::device_status:return closed(v,{"hostname"})&&v.get("hostname")->type==Json::Type::string&&valid_hostname(v.get("hostname")->string);
    case Kind::synchronization:{model::MessageId round;std::string phase;uint64_t revision=0;return parse_synchronization(v,round,phase,revision)==Error::none;}
    case Kind::scoped_intent:{model::OwnedIntent out;return parse_owned_intent(v,out)==Error::none;}
    case Kind::scoped_result:return valid_owned_result(v);
    case Kind::enrollment_control:return enrollment(v);
    case Kind::membership_event:return membership(v)&&!v.get("result");
    case Kind::membership_result:return membership(v)&&v.get("result");
    case Kind::snapshot_request:{SnapshotManifest manifest;std::string phase;uint32_t next=0;return parse_snapshot_manifest(v,manifest,phase,next)==Error::none;}
    case Kind::snapshot_chunk:{
        if(!closed(v,{"transfer_id","index","total_length","payload_hash","data"})||!generic_id(v.get("transfer_id"))||!uint_field(v,"index",0,15)||!uint_field(v,"total_length",1,32768)||!hex(v.get("payload_hash"),64))return false;
        const auto *data=v.get("data");return data&&data->type==Json::Type::string&&!data->string.empty()&&data->string.size()<=4096&&data->string.size()%2==0&&hex(data,data->string.size());
    }
    case Kind::update_begin:case Kind::update_chunk:case Kind::update_install:case Kind::update_status:case Kind::update_abort:case Kind::update_result:return valid_update(kind,v);
    }
    return false;
}
}
bool valid_envelope_payload(Kind kind,const Json& value){return payload(kind,value);}
Error parse_synchronization(const Json& value,model::MessageId& round,std::string& phase,uint64_t& revision){
    model::MessageId parsed_round;std::string parsed_phase;uint64_t parsed_revision=0;
    if(!closed(value,{"round_id","phase","config_revision"})||!id(value.get("round_id"),parsed_round)||
       !text_is(value.get("phase"),{"begin","ack","reject"})||!json_string(value.get("phase"),parsed_phase,1,6)||
       !decimal(value.get("config_revision"),parsed_revision)||!parsed_revision)return Error::invalid_message;
    round=parsed_round;phase=std::move(parsed_phase);revision=parsed_revision;return Error::none;
}
std::string synchronization_json(model::MessageId round,std::string_view phase,uint64_t revision){
    if(!round||!revision||(phase!="begin"&&phase!="ack"&&phase!="reject"))return {};
    return "{\"round_id\":\""+round.str()+"\",\"phase\":\""+std::string(phase)+"\",\"config_revision\":\""+std::to_string(revision)+"\"}";
}
const char *error_name(Error v) {
    switch(v) {
#define E(a,b) case Error::a:return b
    E(none,"OK");E(invalid_message,"INVALID_MESSAGE");E(unsupported_kind,"UNSUPPORTED_CAPABILITY");E(protocol_mismatch,"PROTOCOL_MISMATCH");E(unauthorized_role,"UNAUTHORIZED_ROLE");E(wrong_consortium,"WRONG_CONSORTIUM");E(stale_boot,"STALE_BOOT");E(stale_epoch,"STALE_EPOCH");E(stale_config,"STALE_CONFIG");E(expired,"EXPIRED");E(target_not_local,"TARGET_NOT_LOCAL");E(not_ready,"NOT_READY");E(queue_full,"QUEUE_FULL");E(duplicate,"DUPLICATE");E(identity_conflict,"IDENTITY_CONFLICT");E(unknown_outcome,"UNKNOWN_OUTCOME");E(storage_failure,"STORAGE_FAILURE");E(transaction_conflict,"TRANSACTION_CONFLICT");
#undef E
    }return "INVALID_MESSAGE";
}
const char *kind_name(Kind k) {switch(k) {
#define K(a) case Kind::a:return #a
K(enrollment_control);K(membership_event);K(membership_result);
K(snapshot_request);K(snapshot_chunk);K(update_begin);K(update_chunk);K(update_install);K(update_status);K(update_abort);K(update_result);K(device_status);K(settings_read);K(settings_write);K(settings_result);K(scoped_intent);K(scoped_result);K(synchronization);K(log_request);K(log_result);
#undef K
}return "";}
bool decimal(const Json *v,uint64_t &out) {if(!v||v->type!=Json::Type::string)return false;const auto parsed=model::parse_uint64(v->string);if(!parsed)return false;out=*parsed;return true;}
Error parse_envelope(std::string_view body,Envelope &out,JsonError *detail) {
    if(body.substr(0,4)=="AFU1")return parse_update_chunk_envelope(body,out);
    Json v;JsonError e;if(!parse_json(body,v,e,max_frame_bytes,2048)){if(detail)*detail=e;return Error::invalid_message;}
    if(!closed(v,{"protocol_version","consortium_id","origin_octopus_id","origin_boot_id","destination_octopus_id","destination_boot_id","coordinator_epoch","config_revision","kind","ttl_ms","payload"}))return Error::invalid_message;
    uint64_t version=0,ttl=0;if(!json_uint(v.get("protocol_version"),version,1,1))return Error::protocol_mismatch;
    Envelope x;
    if(!id(v.get("consortium_id"),x.consortium_id)||!id(v.get("origin_octopus_id"),x.origin_octopus_id)||!id(v.get("origin_boot_id"),x.origin_boot_id)||!id(v.get("destination_octopus_id"),x.destination_octopus_id)||!id(v.get("destination_boot_id"),x.destination_boot_id)||!decimal(v.get("coordinator_epoch"),x.coordinator_epoch)||!decimal(v.get("config_revision"),x.config_revision)||!json_uint(v.get("ttl_ms"),ttl,1,10000))return Error::invalid_message;
    bool recognized=false;for(auto kind:{Kind::enrollment_control,Kind::membership_event,Kind::membership_result,Kind::snapshot_request,Kind::snapshot_chunk,Kind::update_begin,Kind::update_chunk,Kind::update_install,Kind::update_status,Kind::update_abort,Kind::update_result,Kind::device_status,Kind::settings_read,Kind::settings_write,Kind::settings_result,Kind::scoped_intent,Kind::scoped_result,Kind::synchronization,Kind::log_request,Kind::log_result})if(text_is(v.get("kind"),{kind_name(kind)})){x.kind=kind;recognized=true;break;}
    if(!recognized||retired_kind(x.kind))return Error::unsupported_kind;
    if((x.kind==Kind::membership_event||x.kind==Kind::membership_result)&&ttl>2000)return Error::invalid_message;
    if(!payload(x.kind,*v.get("payload")))return Error::invalid_message;
    x.ttl_ms=ttl;x.payload=std::move(*v.get("payload"));
    if(x.kind==Kind::scoped_intent){model::OwnedIntent op;parse_owned_intent(x.payload,op);if(op.kind!=model::OwnedIntentKind::key_edges&&(op.target.port.consortium_id!=x.consortium_id||op.target.port.octopus_id!=x.destination_octopus_id))return Error::target_not_local;}
    out=std::move(x);return Error::none;
}
std::string hello_json(const HelloFrame &h){
    // Firmware is the fixed lowercase ELF digest supplied by runtime. The
    // optional transaction pair never authorizes an ordinary execution session.
    if(h.firmware_identity.size()!=64||!std::all_of(h.firmware_identity.begin(),h.firmware_identity.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');}))return {};
    std::string out="{\"type\":\"hello\",\"protocol_min\":1,\"protocol_max\":1,\"consortium_id\":\""+h.consortium_id.str()+"\",\"octopus_id\":\""+h.octopus_id.str()+"\",\"boot_id\":\""+h.boot_id.str()+"\",\"coordinator_epoch\":\""+std::to_string(h.coordinator_epoch)+"\",\"config_revision\":\""+std::to_string(h.config_revision)+"\",\"config_hash\":\""+h.config_hash+"\",\"firmware_identity\":\""+h.firmware_identity+"\",\"required_capabilities\":"+std::to_string(h.required_capabilities)+",\"optional_capabilities\":"+std::to_string(h.optional_capabilities)+",\"frame_cap\":8192";
    if(h.pending_join_id)out+=",\"pending_join_id\":\""+h.pending_join_id->str()+"\",\"join_digest\":\""+h.join_digest+"\"";
    if(h.hardware_profile){if(!h.supported_tentacles||h.supported_tentacles>amity::max_tentacles||h.configured_tentacles_mask>=(1u<<h.supported_tentacles))return {};out+=",\"supported_tentacles\":"+std::to_string(h.supported_tentacles)+",\"configured_tentacles_mask\":"+std::to_string(h.configured_tentacles_mask);}
    return out+"}";
}
Error parse_hello(std::string_view body,HelloFrame &out,JsonError *detail) {
    Json v;JsonError e;if(!parse_json(body,v,e,max_frame_bytes,2048)){if(detail)*detail=e;return Error::invalid_message;}
    if(!closed(v,{"type","protocol_min","protocol_max","consortium_id","octopus_id","boot_id","coordinator_epoch","config_revision","config_hash","firmware_identity","required_capabilities","optional_capabilities","frame_cap"},{"pending_join_id","join_digest","supported_tentacles","configured_tentacles_mask"})||!text_is(v.get("type"),{"hello"}))return Error::invalid_message;
    HelloFrame x;uint64_t low=0,high=0,required=0,optional=0,cap=0;
    if(!json_uint(v.get("protocol_min"),low,1,255)||!json_uint(v.get("protocol_max"),high,low,255)||!id(v.get("consortium_id"),x.consortium_id)||!id(v.get("octopus_id"),x.octopus_id)||!id(v.get("boot_id"),x.boot_id)||!decimal(v.get("coordinator_epoch"),x.coordinator_epoch)||!decimal(v.get("config_revision"),x.config_revision)||!hex(v.get("config_hash"),64)||!json_string(v.get("firmware_identity"),x.firmware_identity,1,96)||!json_uint(v.get("required_capabilities"),required,0,UINT32_MAX)||(required&~known_capabilities)||!json_uint(v.get("optional_capabilities"),optional,0,UINT32_MAX)||!json_uint(v.get("frame_cap"),cap,512,max_frame_bytes))return Error::invalid_message;
    if(v.get("pending_join_id")||v.get("join_digest")){model::JoinId join;if(!id(v.get("pending_join_id"),join)||!hex(v.get("join_digest"),64))return Error::invalid_message;x.pending_join_id=join;x.join_digest=v.get("join_digest")->string;}
    if(v.get("supported_tentacles")||v.get("configured_tentacles_mask")){uint64_t count=0,mask=0;if(!json_uint(v.get("supported_tentacles"),count,1,amity::max_tentacles)||!json_uint(v.get("configured_tentacles_mask"),mask,0,(1u<<count)-1))return Error::invalid_message;x.supported_tentacles=count;x.configured_tentacles_mask=mask;x.hardware_profile=true;}
    if(low>1||high<1)return Error::protocol_mismatch;
    x.protocol_min=low;x.protocol_max=high;x.required_capabilities=required;x.optional_capabilities=optional;x.frame_cap=cap;x.config_hash=v.get("config_hash")->string;out=std::move(x);return Error::none;
}
} // namespace amity::consortium
