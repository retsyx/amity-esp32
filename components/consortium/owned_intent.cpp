// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#include "amity/consortium/owned_intent.hpp"
#include <algorithm>

namespace amity::consortium {
namespace {
using K = model::OwnedIntentKind;
const char* action(K k) {
    switch(k) {
    case K::key_edges:return "key_edges";
    case K::context:return "context";
    case K::discover:return "discover";
    case K::activity:return "activity";
    default:break;
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
bool port_action(K k){return k==K::context||k==K::discover||k==K::activity;}
bool parse_action(const Json* value,K& out){
    std::string name;if(!json_string(value,name,1,24))return false;
    for(auto k:{K::key_edges,K::context,K::discover,K::activity})
        if(name==action(k)){out=k;return true;}
    return false;
}
bool parse_context(const Json& v,model::OwnedIntent& x,bool activity=false){
    uint64_t input=0;
    if(!(activity?closed(v,{"selected","source_changed","source","audio","display","switch_device","input","repair","reselect"}):closed(v,{"selected","source_changed","source","audio","display","switch_device","input"}))||
       !json_bool(v.get("selected"),x.selected)||!json_bool(v.get("source_changed"),x.source_changed)||
       !json_uint(v.get("input"),input,0,255))return false;
    if(activity&&(!json_bool(v.get("repair"),x.repair)||!json_bool(v.get("reselect"),x.reselect)))return false;
    x.input=static_cast<uint8_t>(input);
    for(const auto& item:{std::pair<const char*,std::optional<model::QualifiedDeviceName>*>{"source",&x.source},{"audio",&x.audio},{"display",&x.display},{"switch_device",&x.switch_device}}){
        const auto* value=v.get(item.first);if(value->type==Json::Type::null_value)continue;
        model::QualifiedDeviceName next;if(!target(*value,next,false)||next.port.consortium_id!=x.target.port.consortium_id)return false;
        *item.second=std::move(next);
    }
    return true;
}
std::string context_json(const model::OwnedIntent& x,bool activity=false){
    return "{\"selected\":"+std::string(x.selected?"true":"false")+",\"source_changed\":"+(x.source_changed?"true":"false")+",\"source\":"+(x.source?target_json(*x.source):"null")+",\"audio\":"+(x.audio?target_json(*x.audio):"null")+",\"display\":"+(x.display?target_json(*x.display):"null")+",\"switch_device\":"+(x.switch_device?target_json(*x.switch_device):"null")+",\"input\":"+std::to_string(x.input)+(activity?",\"repair\":"+std::string(x.repair?"true":"false")+",\"reselect\":"+(x.reselect?"true":"false"):"")+"}";
}
}
Error parse_owned_intent(const Json& v,model::OwnedIntent& out) {
    model::OwnedIntent x;
    if(!parse_action(v.get("action"),x.kind))return Error::invalid_message;
    if(!(x.kind==K::key_edges?closed(v,{"generation","action","arguments"}):closed(v,{"generation","target","action","arguments"}))||
       !decimal(v.get("generation"),x.generation)||!x.generation)return Error::invalid_message;
    if(x.kind!=K::key_edges&&!target(*v.get("target"),x.target,port_action(x.kind)))return Error::invalid_message;
    const auto& a=*v.get("arguments");uint64_t n=0;
    switch(x.kind) {
    case K::key_edges:{
        if(!closed(a,{"edges"}))return Error::invalid_message;
        const auto* edges=a.get("edges");
        if(edges->type!=Json::Type::array||edges->array.empty()||edges->array.size()>2)return Error::invalid_message;
        for(const auto& edge:edges->array){
            auto& decoded=x.edges[x.edge_count];
            if(!closed(edge,{"key","down"})||!json_uint(edge.get("key"),n,0,255)||!json_bool(edge.get("down"),decoded.down))return Error::invalid_message;
            decoded.key=static_cast<uint8_t>(n);++x.edge_count;
        }
        if(x.edge_count==2&&(x.edges[0].key!=x.edges[1].key||!x.edges[0].down||x.edges[1].down))return Error::invalid_message;
        break;
    }
    case K::activity:
    case K::context:
        if(!parse_context(a,x,x.kind==K::activity))return Error::invalid_message;
        break;
    default:if(!closed(a,{}))return Error::invalid_message;break;
    }
    out=std::move(x);return Error::none;
}
std::string owned_intent_json(const model::OwnedIntent& x) {
    std::string args="{}";
    switch(x.kind) {
    case K::key_edges:
        if(!x.edge_count||x.edge_count>2)return {};
        args="{\"edges\":[";
        for(uint8_t index=0;index<x.edge_count;++index){if(index)args+=',';args+="{\"key\":"+std::to_string(x.edges[index].key)+",\"down\":"+(x.edges[index].down?"true":"false")+"}";}
        args+="]}";break;
    case K::context:args=context_json(x);break;
    case K::activity:args=context_json(x,true);break;
    case K::discover:break;
    default:return {};
    }
    return "{\"generation\":\""+std::to_string(x.generation)+"\""+(x.kind==K::key_edges?std::string{}:",\"target\":"+target_json(x.target))+",\"action\":\""+action(x.kind)+"\",\"arguments\":"+args+"}";
}
std::string owned_snapshot_json(const model::OwnedAdapterSnapshot& s) {
    std::string out="{\"owner\":\""+s.owner.str()+"\",\"boot\":\""+s.boot.str()+"\",\"adapter\":"+std::to_string(s.adapter)+",\"front_address\":"+std::to_string(s.front_address)+",\"back_address\":"+std::to_string(s.back_address)+",\"front_physical_address\":"+std::to_string(s.front_physical_address)+",\"ready\":"+(s.ready?"true":"false")+",\"discovering\":"+(s.discovering?"true":"false")+",\"discovery_generation\":"+std::to_string(s.discovery_generation)+",\"audio_status_known\":"+(s.audio_status_known?"true":"false")+",\"muted\":"+(s.muted?"true":"false")+",\"volume\":"+std::to_string(s.volume)+",\"devices\":[";
    for(const auto& d:s.devices){if(out.back()!='[')out+=',';out+="{\"side\":\""+std::string(model::side_name(d.side))+"\",\"osd_name\":"+quote(d.osd_name)+",\"logical_address\":"+std::to_string(d.logical_address)+",\"physical_address\":"+std::to_string(d.physical_address)+",\"device_type\":"+std::to_string(d.device_type)+",\"vendor_id\":"+std::to_string(d.vendor_id)+",\"resolved\":"+(d.resolved?"true":"false")+"}";}
    return out+"]}";
}
bool parse_owned_snapshot(const Json& v,model::OwnedAdapterSnapshot& out) {
    model::OwnedAdapterSnapshot x;uint64_t n=0;
    if(!closed(v,{"owner","boot","adapter","front_address","back_address","front_physical_address","ready","discovering","discovery_generation","audio_status_known","muted","volume","devices"})||!id(v.get("owner"),x.owner)||!id(v.get("boot"),x.boot)||!json_uint(v.get("adapter"),n,0,amity::last_tentacle_id))return false;
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
    return false;
}
}
