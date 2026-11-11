// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#include "amity/consortium/logs.hpp"
#include "amity/consortium/session.hpp"
#include <cassert>
#include <iostream>

namespace c=amity::consortium;
namespace m=amity::system;
template<class Id> Id ident(char digit){const auto value=Id::parse(std::string(32,digit));assert(value);return *value;}
c::Json parsed(const std::string& value){c::Json json;c::JsonError error;assert(c::parse_json(value,json,error));return json;}
c::LinkScope scope(c::PeerRole role){
    c::LinkScope value;value.link_id=ident<m::LinkId>('1');value.consortium_id=ident<m::ConsortiumId>('2');
    value.local_id=ident<m::OctopusId>('3');value.peer_id=ident<m::OctopusId>('4');
    value.coordinator_id=role==c::PeerRole::coordinator?value.peer_id:value.local_id;
    value.local_boot_id=ident<m::BootId>('5');value.peer_boot_id=ident<m::BootId>('6');value.epoch=1;value.config_revision=7;
    return value;
}
c::Session session(c::PeerRole role,uint32_t capabilities=c::known_capabilities,bool active=true){
    const auto link=scope(role);c::Session value;
    assert(value.authenticate(link,link.peer_id,role,active)==c::Error::none);
    c::HelloFrame hello;hello.consortium_id=link.consortium_id;hello.octopus_id=link.peer_id;hello.boot_id=link.peer_boot_id;
    hello.coordinator_epoch=1;hello.config_revision=7;hello.optional_capabilities=capabilities;
    assert(value.hello(hello,c::known_capabilities)==c::Error::none);
    if(active)assert(value.synchronized(7,true,true)==c::Error::none);
    return value;
}
c::Envelope message(const c::Session& value,c::Kind kind){
    const auto link=value.scope();c::Envelope envelope;
    envelope.consortium_id=link.consortium_id;envelope.origin_octopus_id=link.peer_id;envelope.destination_octopus_id=link.local_id;
    envelope.origin_boot_id=link.peer_boot_id;envelope.destination_boot_id=link.local_boot_id;envelope.coordinator_epoch=1;
    envelope.config_revision=7;envelope.kind=kind;envelope.ttl_ms=1000;
    const auto transfer=ident<m::MessageId>('7');
    envelope.payload=parsed(kind==c::Kind::log_request?c::log_request_json(transfer,0):c::log_result_json(transfer,0,1,"x"));
    return envelope;
}
void codec_tests(){
    const auto transfer=ident<m::MessageId>('7');
    assert(c::is_log_kind(c::Kind::log_request)&&c::is_log_kind(c::Kind::log_result)&&!c::is_log_kind(c::Kind::device_status));
    for(const auto offset:{size_t{0},c::log_capacity})for(bool close:{false,true})
        assert(c::valid_log_payload(c::Kind::log_request,parsed(c::log_request_json(transfer,offset,close))));
    assert(c::log_request_json({},0).empty()&&c::log_request_json(transfer,c::log_capacity+1).empty());
    auto request=parsed(c::log_request_json(transfer,0));
    request.get("offset")->integer=c::log_capacity+1;assert(!c::valid_log_payload(c::Kind::log_request,request));
    request=parsed(c::log_request_json(transfer,0));request.get("close")->type=c::Json::Type::integer;assert(!c::valid_log_payload(c::Kind::log_request,request));
    request=parsed(c::log_request_json(transfer,0));request.object.emplace_back("extra",c::Json{});assert(!c::valid_log_payload(c::Kind::log_request,request));

    std::string binary;for(unsigned i=0;i<256;++i)binary+=static_cast<char>(i);
    auto result=parsed(c::log_result_json(transfer,0,binary.size(),binary));
    assert(c::valid_log_payload(c::Kind::log_result,result));
    assert(result.get("data")->string.size()==512&&result.get("data")->string.substr(0,8)=="00010203"&&result.get("data")->string.substr(508)=="feff");
    assert(c::valid_log_payload(c::Kind::log_result,parsed(c::log_result_json(transfer,c::log_capacity-c::log_chunk_bytes,c::log_capacity,std::string(c::log_chunk_bytes,'x')))));
    for(const auto offset:{size_t{0},c::log_capacity})assert(c::valid_log_payload(c::Kind::log_result,parsed(c::log_result_json(transfer,offset,offset,{}))));
    for(const auto error:{"busy","unavailable","expired"})assert(c::valid_log_payload(c::Kind::log_result,parsed(c::log_result_json(transfer,17,0,{},error))));
    assert(c::log_result_json(transfer,0,1,{}).empty());
    assert(c::log_result_json(transfer,1,0,{}).empty());
    assert(c::log_result_json(transfer,c::log_capacity,c::log_capacity,"x").empty());
    assert(c::log_result_json(transfer,0,c::log_capacity+1,"x").empty());
    assert(c::log_result_json(transfer,0,c::log_capacity,std::string(c::log_chunk_bytes+1,'x')).empty());
    assert(c::log_result_json(transfer,0,0,{},"unknown").empty());
    assert(c::log_result_json(transfer,0,1,{},"busy").empty());
    assert(c::log_result_json(transfer,0,0,"x","busy").empty());
    assert(c::log_result_json({},0,0,{}).empty());
    for(const auto& malformed:std::initializer_list<std::string>{"f","FF","0g",std::string(2*c::log_chunk_bytes+2,'0')}){
        auto invalid=result;invalid.get("data")->string=malformed;assert(!c::valid_log_payload(c::Kind::log_result,invalid));
    }
    for(unsigned fault=0;fault<6;++fault){
        auto invalid=result;
        if(fault==0)invalid.get("offset")->integer=c::log_capacity+1;
        if(fault==1)invalid.get("total")->integer=c::log_capacity+1;
        if(fault==2)invalid.get("total")->integer=1;
        if(fault==3)invalid.get("data")->string.clear();
        if(fault==4)invalid.get("error")->string="busy";
        if(fault==5)invalid.get("transfer_id")->string=std::string(32,'0');
        assert(!c::valid_log_payload(c::Kind::log_result,invalid));
    }
    const auto link=scope(c::PeerRole::coordinator);
    for(const auto kind:{c::Kind::log_request,c::Kind::log_result}){
        const auto payload=kind==c::Kind::log_request?c::log_request_json(transfer,0):c::log_result_json(transfer,0,1,"x");
        const auto wire="{\"protocol_version\":1,\"consortium_id\":\""+link.consortium_id.str()+"\",\"origin_octopus_id\":\""+link.peer_id.str()+"\",\"origin_boot_id\":\""+link.peer_boot_id.str()+"\",\"destination_octopus_id\":\""+link.local_id.str()+"\",\"destination_boot_id\":\""+link.local_boot_id.str()+"\",\"coordinator_epoch\":\"1\",\"config_revision\":\"7\",\"kind\":\""+c::kind_name(kind)+"\",\"ttl_ms\":1000,\"payload\":"+payload+"}";
        c::Envelope decoded;assert(c::parse_envelope(wire,decoded)==c::Error::none&&decoded.kind==kind);
    }
}
void authorization_tests(){
    for(const auto role:{c::PeerRole::coordinator,c::PeerRole::actuator}){
        auto value=session(role);
        const auto accepted=role==c::PeerRole::coordinator?c::Kind::log_request:c::Kind::log_result;
        const auto rejected=role==c::PeerRole::coordinator?c::Kind::log_result:c::Kind::log_request;
        auto envelope=message(value,accepted);
        assert(value.admit(envelope,100,101)==c::Error::none);
        assert(value.admit(message(value,rejected),100,101)==c::Error::unauthorized_role);
        assert(value.admit(envelope,100,1100)==c::Error::invalid_message);
        for(unsigned fault=0;fault<4;++fault){
            auto invalid=envelope;
            if(fault==0)invalid.origin_boot_id=ident<m::BootId>('8');
            if(fault==1)invalid.destination_boot_id=ident<m::BootId>('8');
            if(fault==2)invalid.origin_octopus_id=ident<m::OctopusId>('8');
            if(fault==3)invalid.config_revision=8;
            assert(value.admit(invalid,100,101)!=c::Error::none);
        }
        value.restrict_management();assert(value.admit(envelope,100,101)==c::Error::none);
        assert(value.admit(message(value,rejected),100,101)==c::Error::unauthorized_role);
        auto no_cap=session(role,c::known_capabilities&~c::peer_logs);
        assert(no_cap.admit(message(no_cap,accepted),100,101)==c::Error::unsupported_kind);
        no_cap.restrict_management();assert(no_cap.admit(message(no_cap,accepted),100,101)==c::Error::unsupported_kind);
        auto passive=session(role,c::known_capabilities,false);passive.restrict_management();
        assert(passive.admit(message(passive,accepted),100,101)==c::Error::unauthorized_role);
        auto synchronizing=session(role);assert(synchronizing.begin_synchronization(7)==c::Error::none);
        assert(synchronizing.admit(envelope,100,101)==c::Error::unauthorized_role);
        assert(synchronizing.discard_synchronization_frame(envelope,100,101)==c::Error::none);
        auto retiring=session(role);retiring.restrict_lifecycle();
        assert(retiring.admit(envelope,100,101)==c::Error::unauthorized_role);
        assert(retiring.discard_lifecycle_frame(envelope,100,101)==(role==c::PeerRole::actuator?c::Error::none:c::Error::unsupported_kind));
    }
    auto provisional=session(c::PeerRole::provisional_candidate,c::known_capabilities,false);
    assert(provisional.admit(message(provisional,c::Kind::log_request),100,101)==c::Error::unauthorized_role);
}
int main(){
    codec_tests();authorization_tests();
    std::cout<<"PASS log codec bounds, binary chunks, closed payloads, authenticated roles/capabilities, management and late-frame drains\n";
}
