// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#include "amity/consortium/wire.hpp"
#include "amity/consortium/owned_intent.hpp"
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
bool decimal_field(const Json &v,std::string_view key) {uint64_t n;return decimal(v.get(key),n);}
bool ids(const Json &v,std::initializer_list<std::string_view> keys) {for(auto key:keys)if(!generic_id(v.get(key)))return false;return true;}
bool enrollment(const Json &v) {
    if(!ids(v,{"join_id"})||!hex(v.get("join_digest"),64))return false;
    return text_is(v.get("phase"),{"commit","ack"})&&closed(v,{"phase","join_id","join_digest","membership_revision"})&&decimal_field(v,"membership_revision");
}
bool membership(const Json &v) {
    return closed(v,{"phase"},{"result"})&&text_is(v.get("phase"),{"factory_reset"})&&(!v.get("result")||text_is(v.get("result"),{"reset_scheduled"}));
}
bool payload(Kind kind,const Json &v) {
    switch(kind) {
    case Kind::log_request:case Kind::log_result:return valid_log_payload(kind,v);
    case Kind::settings_read:case Kind::settings_write:case Kind::settings_result:return valid_settings_payload(kind,v);
    case Kind::device_status:{
        // A hostname, a Wi-Fi signal, or both.
        if(v.type!=Json::Type::object||v.object.empty()||!closed(v,{},{"hostname","wifi_rssi_negated","wifi_channel"}))return false;
        const auto* name=v.get("hostname");if(name&&(name->type!=Json::Type::string||!valid_hostname(name->string)))return false;
        const bool rssi=v.get("wifi_rssi_negated"),channel=v.get("wifi_channel");
        return rssi==channel&&(!rssi||(uint_field(v,"wifi_rssi_negated",0,127)&&uint_field(v,"wifi_channel",1,196)));
    }
    case Kind::synchronization:{model::MessageId round;std::string phase;uint64_t revision=0;return parse_synchronization(v,round,phase,revision)==Error::none;}
    case Kind::scoped_intent:{model::OwnedIntent out;return parse_owned_intent(v,out)==Error::none;}
    case Kind::scoped_result:return valid_owned_result(v);
    case Kind::enrollment_control:return enrollment(v);
    case Kind::membership_event:return membership(v)&&!v.get("result");
    case Kind::membership_result:return membership(v)&&v.get("result");
    case Kind::update_begin:case Kind::update_chunk:case Kind::update_install:case Kind::update_abort:case Kind::update_result:return valid_update(kind,v);
    }
    return false;
}
}
bool valid_envelope_payload(Kind kind,const Json& value){return payload(kind,value);}
std::string wifi_signal_json(const WifiSignalReport& signal){
    const int negated=-static_cast<int>(signal.rssi);
    return "{\"wifi_rssi_negated\":"+std::to_string(std::clamp(negated,0,127))+",\"wifi_channel\":"+std::to_string(signal.channel)+"}";
}
std::optional<WifiSignalReport> parse_wifi_signal(const Json& device_status){
    uint64_t negated=0,channel=0;
    if(!json_uint(device_status.get("wifi_rssi_negated"),negated,0,127)||!json_uint(device_status.get("wifi_channel"),channel,1,196))return std::nullopt;
    return WifiSignalReport{static_cast<int8_t>(-static_cast<int>(negated)),static_cast<uint8_t>(channel)};
}
Error parse_synchronization(const Json& value,model::MessageId& round,std::string& phase,uint64_t& revision){
    model::MessageId parsed_round;std::string parsed_phase;uint64_t parsed_revision=0;
    if(!closed(value,{"round_id","phase","config_revision"})||!id(value.get("round_id"),parsed_round)||
       !text_is(value.get("phase"),{"begin","ack","reject","advance"})||!json_string(value.get("phase"),parsed_phase,1,7)||
       !decimal(value.get("config_revision"),parsed_revision)||!parsed_revision)return Error::invalid_message;
    round=parsed_round;phase=std::move(parsed_phase);revision=parsed_revision;return Error::none;
}
std::string synchronization_json(model::MessageId round,std::string_view phase,uint64_t revision){
    if(!round||!revision||(phase!="begin"&&phase!="ack"&&phase!="reject"&&phase!="advance"))return {};
    return "{\"round_id\":\""+round.str()+"\",\"phase\":\""+std::string(phase)+"\",\"config_revision\":\""+std::to_string(revision)+"\"}";
}
const char *error_name(Error v) {
    switch(v) {
#define E(a,b) case Error::a:return b
    E(none,"OK");E(invalid_message,"INVALID_MESSAGE");E(unsupported_kind,"UNSUPPORTED_KIND");E(unauthorized_role,"UNAUTHORIZED_ROLE");E(wrong_consortium,"WRONG_CONSORTIUM");E(stale_boot,"STALE_BOOT");E(stale_config,"STALE_CONFIG");E(expired,"EXPIRED");E(target_not_local,"TARGET_NOT_LOCAL");E(not_ready,"NOT_READY");E(queue_full,"QUEUE_FULL");E(duplicate,"DUPLICATE");E(identity_conflict,"IDENTITY_CONFLICT");E(storage_failure,"STORAGE_FAILURE");E(transaction_conflict,"TRANSACTION_CONFLICT");
#undef E
    }return "INVALID_MESSAGE";
}
const char *kind_name(Kind k) {switch(k) {
#define K(a) case Kind::a:return #a
K(enrollment_control);K(membership_event);K(membership_result);
K(update_begin);K(update_chunk);K(update_install);K(update_abort);K(update_result);K(device_status);K(settings_read);K(settings_write);K(settings_result);K(scoped_intent);K(scoped_result);K(synchronization);K(log_request);K(log_result);
#undef K
}return "";}
bool decimal(const Json *v,uint64_t &out) {if(!v||v->type!=Json::Type::string)return false;const auto parsed=model::parse_uint64(v->string);if(!parsed)return false;out=*parsed;return true;}
Error parse_envelope(std::string_view body,Envelope &out,JsonError *detail) {
    if(body.substr(0,4)=="AFU1")return parse_update_chunk_envelope(body,out);
    Json v;JsonError e;if(!parse_json(body,v,e,max_frame_bytes,2048)){if(detail)*detail=e;return Error::invalid_message;}
    if(!closed(v,{"config_revision","kind","ttl_ms","payload"}))return Error::invalid_message;
    uint64_t ttl=0;Envelope x;
    if(!decimal(v.get("config_revision"),x.config_revision)||!json_uint(v.get("ttl_ms"),ttl,1,10000))return Error::invalid_message;
    bool recognized=false;for(auto kind:{Kind::enrollment_control,Kind::membership_event,Kind::membership_result,Kind::update_begin,Kind::update_chunk,Kind::update_install,Kind::update_abort,Kind::update_result,Kind::device_status,Kind::settings_read,Kind::settings_write,Kind::settings_result,Kind::scoped_intent,Kind::scoped_result,Kind::synchronization,Kind::log_request,Kind::log_result})if(text_is(v.get("kind"),{kind_name(kind)})){x.kind=kind;recognized=true;break;}
    if(!recognized)return Error::unsupported_kind;
    if((x.kind==Kind::membership_event||x.kind==Kind::membership_result)&&ttl>2000)return Error::invalid_message;
    if(x.kind==Kind::scoped_intent){model::OwnedIntent intent;if(parse_owned_intent(*v.get("payload"),intent)!=Error::none)return Error::invalid_message;x.intent=std::move(intent);}
    else if(x.kind==Kind::settings_write){DeviceSettingsWrite write;if(parse_settings_write(*v.get("payload"),write)!=Error::none)return Error::invalid_message;x.settings_write=std::move(write);}
    else if(!payload(x.kind,*v.get("payload")))return Error::invalid_message;
    x.ttl_ms=ttl;x.payload=std::move(*v.get("payload"));
    out=std::move(x);return Error::none;
}
std::string hello_json(const HelloFrame &h){
    // Firmware is the fixed lowercase ELF digest supplied by runtime. The
    // optional transaction pair never authorizes an ordinary execution session.
    if(h.firmware_identity.size()!=64||!std::all_of(h.firmware_identity.begin(),h.firmware_identity.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');}))return {};
    if(!h.supported_tentacles||h.supported_tentacles>amity::max_tentacles||h.configured_tentacles_mask>=(1u<<h.supported_tentacles))return {};
    std::string out="{\"type\":\"hello\",\"consortium_id\":\""+h.consortium_id.str()+"\",\"octopus_id\":\""+h.octopus_id.str()+"\",\"boot_id\":\""+h.boot_id.str()+"\",\"config_revision\":\""+std::to_string(h.config_revision)+"\",\"firmware_identity\":\""+h.firmware_identity+"\",\"supported_tentacles\":"+std::to_string(h.supported_tentacles)+",\"configured_tentacles_mask\":"+std::to_string(h.configured_tentacles_mask);
    if(h.pending_join_id)out+=",\"pending_join_id\":\""+h.pending_join_id->str()+"\",\"join_digest\":\""+h.join_digest+"\"";
    return out+"}";
}
Error parse_hello(std::string_view body,HelloFrame &out,JsonError *detail) {
    Json v;JsonError e;if(!parse_json(body,v,e,max_frame_bytes,2048)){if(detail)*detail=e;return Error::invalid_message;}
    if(!closed(v,{"type","consortium_id","octopus_id","boot_id","config_revision","firmware_identity","supported_tentacles","configured_tentacles_mask"},{"pending_join_id","join_digest"})||!text_is(v.get("type"),{"hello"}))return Error::invalid_message;
    HelloFrame x;uint64_t count=0,mask=0;
    if(!id(v.get("consortium_id"),x.consortium_id)||!id(v.get("octopus_id"),x.octopus_id)||!id(v.get("boot_id"),x.boot_id)||!decimal(v.get("config_revision"),x.config_revision)||!json_string(v.get("firmware_identity"),x.firmware_identity,1,96)||
       !json_uint(v.get("supported_tentacles"),count,1,amity::max_tentacles)||!json_uint(v.get("configured_tentacles_mask"),mask,0,(1u<<count)-1))return Error::invalid_message;
    if(v.get("pending_join_id")||v.get("join_digest")){model::JoinId join;if(!id(v.get("pending_join_id"),join)||!hex(v.get("join_digest"),64))return Error::invalid_message;x.pending_join_id=join;x.join_digest=v.get("join_digest")->string;}
    x.supported_tentacles=count;x.configured_tentacles_mask=mask;out=std::move(x);return Error::none;
}
} // namespace amity::consortium
