// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#include "amity/consortium/owned_intent.hpp"
#include <algorithm>

namespace amity::consortium {
namespace {
using K = model::OwnedIntentKind;
const char* action(K k) {
    switch(k) {
    case K::power:return "power";
    case K::source_input:return "source_input";
    case K::switch_input:return "switch_input";
    case K::announce_active_source:return "announce_active_source";
    case K::query_audio:return "query_audio";
    case K::key_down:return "key_down";
    case K::key_up:return "key_up";
    case K::context:return "context";
    case K::standby_wire:return "standby_wire";
    case K::discover:return "discover";
    }
    return "";
}
std::string quote(std::string_view s) {
    std::string out="\"";
    constexpr char hex[]="0123456789abcdef";
    for(unsigned char c:s) {
        if(c=='"'||c=='\\') {out+='\\';out+=char(c);}
        else if(c<32) {out+="\\u00";out+=hex[c>>4];out+=hex[c&15];}
        else out+=char(c);
    }
    return out+'"';
}
std::string target_json(const model::QualifiedDeviceName& n) {
    if(!n.port.valid())return {};
    return "{\"consortium_id\":\""+n.port.consortium_id.str()+"\",\"octopus_id\":\""+n.port.octopus_id.str()+"\",\"tentacle_id\":"+std::to_string(n.port.tentacle_id)+",\"side\":\""+model::side_name(n.port.side)+"\",\"osd_name\":"+quote(n.osd_name)+"}";
}
bool target(const Json& v, model::QualifiedDeviceName& n, bool allow_empty) {
    if(!closed(v,{"consortium_id","octopus_id","tentacle_id","side","osd_name"}))return false;
    uint64_t adapter=0;std::string side;
    if(!id(v.get("consortium_id"),n.port.consortium_id)||!id(v.get("octopus_id"),n.port.octopus_id)||
       !json_uint(v.get("tentacle_id"),adapter,0,amity::last_tentacle_id)||
       !json_string(v.get("side"),side,4,5)||!model::parse_side(side)||
       !json_string(v.get("osd_name"),n.osd_name,allow_empty?0:1,14))return false;
    n.port.tentacle_id=static_cast<uint8_t>(adapter);n.port.side=*model::parse_side(side);
    return n.port.valid()&&(n.osd_name.empty()?allow_empty:n.valid());
}
}
Error parse_owned_intent(const Json& v,model::OwnedIntent& out,model::ExecutionLeaseId& lease) {
    if(!closed(v,{"execution_lease_id","generation","sequence","setup_generation","target","action","arguments"})||
       !id(v.get("execution_lease_id"),lease))return Error::invalid_message;
    model::OwnedIntent x;std::string name;uint64_t setup=0;
    if(!json_uint(v.get("setup_generation"),setup,0,UINT32_MAX))return Error::invalid_message;
    x.setup_generation=static_cast<uint32_t>(setup);
    if(!decimal(v.get("generation"),x.generation)||!x.generation||!decimal(v.get("sequence"),x.sequence)||!x.sequence||
       !json_string(v.get("action"),name,1,24))return Error::invalid_message;
    bool found=false;
    for(auto k:{K::power,K::source_input,K::switch_input,K::announce_active_source,K::query_audio,K::key_down,K::key_up,K::context,K::standby_wire,K::discover})
        if(name==action(k)){x.kind=k;found=true;break;}
    const bool port_only=x.kind==K::context||x.kind==K::announce_active_source||x.kind==K::standby_wire||x.kind==K::discover;
    if(!found||!target(*v.get("target"),x.target,port_only))return Error::invalid_message;
    const auto& a=*v.get("arguments");uint64_t n=0;
    switch(x.kind) {
    case K::power:
        if(!closed(a,{"on"})||!json_bool(a.get("on"),x.on))return Error::invalid_message;
        break;
    case K::source_input:{
        if(!closed(a,{"display"}))return Error::invalid_message;
        const auto* value=a.get("display");
        if(value->type!=Json::Type::null_value){
            model::QualifiedDeviceName display;
            if(!target(*value,display,false)||display.port.consortium_id!=x.target.port.consortium_id)return Error::invalid_message;
            x.display=std::move(display);
        }
        break;
    }
    case K::switch_input:if(!closed(a,{"input"})||!json_uint(a.get("input"),n,0,255))return Error::invalid_message;x.input=static_cast<uint8_t>(n);break;
    case K::key_down:
        if(!closed(a,{"key","repeat","token"})||!json_uint(a.get("key"),n,0,255)||!json_bool(a.get("repeat"),x.repeat)||!id(a.get("token"),x.token))return Error::invalid_message;
        x.key=static_cast<uint8_t>(n);break;
    case K::key_up:
        if(!closed(a,{"key","token"})||!json_uint(a.get("key"),n,0,255)||!id(a.get("token"),x.token))return Error::invalid_message;
        x.key=static_cast<uint8_t>(n);break;
    case K::context:
        if(!json_uint(a.get("input"),n,0,255))return Error::invalid_message;
        x.input=static_cast<uint8_t>(n);
        if(!closed(a,{"selected","source_changed","source","audio","display","switch_device","input"})||!json_bool(a.get("selected"),x.selected)||!json_bool(a.get("source_changed"),x.source_changed))return Error::invalid_message;
        for(const auto& item:{std::pair<const char*,std::optional<model::QualifiedDeviceName>*>{"source",&x.source},{"audio",&x.audio},{"display",&x.display},{"switch_device",&x.switch_device}}){const auto* value=a.get(item.first);if(value->type!=Json::Type::null_value){model::QualifiedDeviceName next;if(!target(*value,next,false)||next.port.consortium_id!=x.target.port.consortium_id)return Error::invalid_message;*item.second=next;}}
        break;
    default:if(!closed(a,{}))return Error::invalid_message;break;
    }
    out=std::move(x);return Error::none;
}
std::string owned_intent_json(const model::OwnedIntent& x,model::ExecutionLeaseId lease) {
    std::string args="{}";
    switch(x.kind) {
    case K::power:args="{\"on\":"+std::string(x.on?"true":"false")+"}";break;
    case K::source_input:args="{\"display\":"+(x.display?target_json(*x.display):"null")+"}";break;
    case K::switch_input:args="{\"input\":"+std::to_string(x.input)+"}";break;
    case K::key_down:args="{\"key\":"+std::to_string(x.key)+",\"repeat\":"+(x.repeat?"true":"false")+",\"token\":\""+x.token.str()+"\"}";break;
    case K::key_up:args="{\"key\":"+std::to_string(x.key)+",\"token\":\""+x.token.str()+"\"}";break;
    case K::context:args="{\"selected\":"+std::string(x.selected?"true":"false")+",\"source_changed\":"+(x.source_changed?"true":"false")+",\"source\":"+(x.source?target_json(*x.source):"null")+",\"audio\":"+(x.audio?target_json(*x.audio):"null")+",\"display\":"+(x.display?target_json(*x.display):"null")+",\"switch_device\":"+(x.switch_device?target_json(*x.switch_device):"null")+",\"input\":"+std::to_string(x.input)+"}";break;
    default:break;
    }
    const auto json="{\"execution_lease_id\":\""+lease.str()+"\",\"generation\":\""+std::to_string(x.generation)+"\",\"sequence\":\""+std::to_string(x.sequence)+"\",\"setup_generation\":"+std::to_string(x.setup_generation)+",\"target\":"+target_json(x.target)+",\"action\":\""+action(x.kind)+"\",\"arguments\":"+args+"}";
    Json parsed;JsonError error;model::OwnedIntent verified;model::ExecutionLeaseId verified_lease;
    return parse_json(json,parsed,error,max_frame_bytes,2048)&&parse_owned_intent(parsed,verified,verified_lease)==Error::none?json:std::string{};
}
std::string owned_snapshot_json(const model::OwnedAdapterSnapshot& s) {
    std::string out="{\"owner\":\""+s.owner.str()+"\",\"boot\":\""+s.boot.str()+"\",\"adapter\":"+std::to_string(s.adapter)+",\"front_address\":"+std::to_string(s.front_address)+",\"back_address\":"+std::to_string(s.back_address)+",\"front_physical_address\":"+std::to_string(s.front_physical_address)+",\"ready\":"+(s.ready?"true":"false")+",\"discovering\":"+(s.discovering?"true":"false")+",\"discovery_generation\":"+std::to_string(s.discovery_generation)+",\"setup_generation\":"+std::to_string(s.setup_generation)+",\"setup_last_sequence\":\""+std::to_string(s.setup_last_sequence)+"\",\"setup_pending\":"+std::to_string(s.setup_pending)+",\"setup_replies_pending\":"+(s.setup_replies_pending?"true":"false")+",\"audio_status_known\":"+(s.audio_status_known?"true":"false")+",\"muted\":"+(s.muted?"true":"false")+",\"volume\":"+std::to_string(s.volume)+",\"devices\":[";
    for(const auto& d:s.devices){if(out.back()!='[')out+=',';out+="{\"side\":\""+std::string(model::side_name(d.side))+"\",\"osd_name\":"+quote(d.osd_name)+",\"logical_address\":"+std::to_string(d.logical_address)+",\"physical_address\":"+std::to_string(d.physical_address)+",\"device_type\":"+std::to_string(d.device_type)+",\"vendor_id\":"+std::to_string(d.vendor_id)+",\"resolved\":"+(d.resolved?"true":"false")+"}";}
    out+="]}";Json parsed;JsonError error;model::OwnedAdapterSnapshot checked;
    return parse_json(out,parsed,error,max_frame_bytes,2048)&&parse_owned_snapshot(parsed,checked)?out:std::string{};
}
bool parse_owned_snapshot(const Json& v,model::OwnedAdapterSnapshot& out) {
    model::OwnedAdapterSnapshot x;uint64_t n=0;
    if(!closed(v,{"owner","boot","adapter","front_address","back_address","front_physical_address","ready","discovering","discovery_generation","setup_generation","setup_last_sequence","setup_pending","setup_replies_pending","audio_status_known","muted","volume","devices"})||!id(v.get("owner"),x.owner)||!id(v.get("boot"),x.boot)||!json_uint(v.get("adapter"),n,0,amity::last_tentacle_id))return false;
    x.adapter=static_cast<uint8_t>(n);
    if(!json_uint(v.get("front_address"),n,0,15))return false;
    x.front_address=static_cast<uint8_t>(n);
    if(!json_uint(v.get("back_address"),n,0,15))return false;
    x.back_address=static_cast<uint8_t>(n);
    if(!json_uint(v.get("front_physical_address"),n,0,65535))return false;
    x.front_physical_address=static_cast<uint16_t>(n);
    if(!json_uint(v.get("discovery_generation"),n,0,UINT32_MAX))return false;
    x.discovery_generation=static_cast<uint32_t>(n);
    if(!json_bool(v.get("ready"),x.ready)||!json_bool(v.get("discovering"),x.discovering)||!json_bool(v.get("audio_status_known"),x.audio_status_known)||!json_bool(v.get("muted"),x.muted)||!json_uint(v.get("volume"),n,0,127))return false;
    x.volume=static_cast<uint8_t>(n);
    if(!json_uint(v.get("setup_generation"),n,0,UINT32_MAX))return false;
    x.setup_generation=static_cast<uint32_t>(n);
    if(!decimal(v.get("setup_last_sequence"),x.setup_last_sequence)||!json_uint(v.get("setup_pending"),n,0,64))return false;
    x.setup_pending=static_cast<uint32_t>(n);
    if(!json_bool(v.get("setup_replies_pending"),x.setup_replies_pending))return false;
    const auto* devices=v.get("devices");if(devices->type!=Json::Type::array||devices->array.size()>30)return false;
    for(const auto& value:devices->array){model::OwnedDeviceObservation d;std::string side;
        if(!closed(value,{"side","osd_name","logical_address","physical_address","device_type","vendor_id","resolved"})||!json_string(value.get("side"),side,4,5)||!model::parse_side(side)||!json_string(value.get("osd_name"),d.osd_name,0,14)||!json_uint(value.get("logical_address"),n,0,14))return false;
        d.side=*model::parse_side(side);d.logical_address=static_cast<uint8_t>(n);
        if(!json_uint(value.get("physical_address"),n,0,65535))return false;
        d.physical_address=static_cast<uint16_t>(n);
        if(!json_uint(value.get("device_type"),n,0,255))return false;
        d.device_type=static_cast<uint8_t>(n);
        if(!json_uint(value.get("vendor_id"),n,0,0xffffff)||!json_bool(value.get("resolved"),d.resolved))return false;
        d.vendor_id=static_cast<uint32_t>(n);
        if(d.resolved&&(d.osd_name.empty()||std::any_of(d.osd_name.begin(),d.osd_name.end(),[](unsigned char c){return c<32||c==127;})))return false;
        if(std::any_of(x.devices.begin(),x.devices.end(),[&](const auto& old){return old.side==d.side&&old.logical_address==d.logical_address;}))return false;
        x.devices.push_back(std::move(d));
    }
    out=std::move(x);return true;
}
bool valid_owned_result(const Json& v) {
    if(closed(v,{"snapshot"})){model::OwnedAdapterSnapshot snapshot;return parse_owned_snapshot(*v.get("snapshot"),snapshot);}
    if(closed(v,{"input_result"})){
        uint64_t generation=0,sequence=0;bool present=false;
        const auto& result=*v.get("input_result");
        return closed(result,{"generation","sequence","source_present"})&&decimal(result.get("generation"),generation)&&generation&&decimal(result.get("sequence"),sequence)&&sequence&&json_bool(result.get("source_present"),present);
    }
    return false;
}
}
