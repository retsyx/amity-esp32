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
    value.local_boot_id=ident<m::BootId>('5');value.peer_boot_id=ident<m::BootId>('6');value.config_revision=7;
    return value;
}
c::Session session(c::PeerRole role,bool active=true){
    const auto link=scope(role);c::Session value;
    assert(value.authenticate(link,link.peer_id,role,active)==c::Error::none);
    c::HelloFrame hello;hello.consortium_id=link.consortium_id;hello.octopus_id=link.peer_id;hello.boot_id=link.peer_boot_id;
    hello.config_revision=7;
    assert(value.hello(hello)==c::Error::none);
    if(active)assert(value.synchronized(7,true,true)==c::Error::none);
    return value;
}
c::LogChunk chunk(uint32_t offset,uint32_t total,std::string bytes){c::LogChunk value;value.transfer=ident<m::MessageId>('7');value.offset=offset;value.total=total;value.bytes=std::move(bytes);return value;}
c::Envelope frame(const c::LogChunk& value){
    c::Envelope envelope;envelope.config_revision=7;envelope.kind=c::Kind::log_result;envelope.ttl_ms=1000;
    const auto bytes=c::log_chunk_envelope(envelope,c::encode_log_chunk(value));assert(!bytes.empty());
    c::Envelope decoded;assert(c::parse_envelope(bytes,decoded)==c::Error::none);return decoded;
}
c::Envelope message(const c::Session&,c::Kind kind){
    if(kind==c::Kind::log_result)return frame(chunk(0,1,"x"));
    c::Envelope envelope;envelope.config_revision=7;envelope.kind=kind;envelope.ttl_ms=1000;
    envelope.payload=parsed(c::log_request_json(ident<m::MessageId>('7')));
    return envelope;
}
void codec_tests(){
    const auto transfer=ident<m::MessageId>('7');
    assert(c::is_log_kind(c::Kind::log_request)&&c::is_log_kind(c::Kind::log_result)&&!c::is_log_kind(c::Kind::device_status));
    assert(c::is_bulk_kind(c::Kind::log_result)&&!c::is_bulk_kind(c::Kind::log_request)&&c::is_bulk_kind(c::Kind::update_chunk));
    assert(c::valid_envelope_payload(c::Kind::log_request,parsed(c::log_request_json(transfer)))&&c::log_request_json({}).empty());
    auto request=parsed(c::log_request_json(transfer));request.object.emplace_back("offset",parsed("0"));assert(!c::valid_envelope_payload(c::Kind::log_request,request));
    assert(!c::valid_envelope_payload(c::Kind::log_request,parsed("{\"transfer_id\":\""+std::string(32,'0')+"\"}")));
    // Log bytes have no JSON form; old request/result vocabulary is rejected.
    assert(!c::valid_envelope_payload(c::Kind::log_result,parsed("{\"transfer_id\":\""+transfer.str()+"\",\"offset\":0,\"total\":1,\"data\":\"78\",\"error\":\"\"}")));

    std::string binary;for(unsigned i=0;i<256;++i)binary+=static_cast<char>(i);
    auto decoded=frame(chunk(0,binary.size(),binary));
    assert(decoded.kind==c::Kind::log_result&&decoded.config_revision==7&&decoded.ttl_ms==1000&&decoded.log_chunk&&decoded.log_chunk->bytes==binary&&decoded.log_chunk->total==256&&!decoded.log_chunk->unavailable);
    const std::string full(c::log_chunk_bytes,'x');
    assert(frame(chunk(c::log_capacity-c::log_chunk_bytes,c::log_capacity,full)).log_chunk->bytes==full);
    for(const auto offset:{uint32_t{0},uint32_t(c::log_capacity)})assert(frame(chunk(offset,offset,{})).log_chunk->bytes.empty());
    auto unavailable=chunk(0,0,{});unavailable.unavailable=true;assert(frame(unavailable).log_chunk->unavailable);
    for(const auto& invalid:{chunk(0,1,{}),chunk(1,0,{}),chunk(c::log_capacity,c::log_capacity,"x"),chunk(0,c::log_capacity+1,"x"),
            chunk(0,c::log_capacity,std::string(c::log_chunk_bytes+1,'x')),chunk(2,3,"xx")})assert(c::encode_log_chunk(invalid).empty());
    auto anonymous=chunk(0,1,"x");anonymous.transfer={};assert(c::encode_log_chunk(anonymous).empty());
    unavailable.total=1;assert(c::encode_log_chunk(unavailable).empty());
    const auto encoded=c::encode_log_chunk(chunk(0,4,"abcd"));c::LogChunk out;
    assert(encoded.size()==29&&c::decode_log_chunk(encoded,out)&&out.bytes=="abcd");
    auto bad=encoded;bad[24]=2;assert(!c::decode_log_chunk(bad,out));
    bad=encoded;bad[23]=1;assert(!c::decode_log_chunk(bad,out)); // Bytes beyond the stated total.
    assert(!c::decode_log_chunk(encoded.substr(0,24),out));
    c::Envelope envelope;envelope.config_revision=7;envelope.kind=c::Kind::log_result;envelope.ttl_ms=1000;
    auto wire=c::log_chunk_envelope(envelope,encoded);c::Envelope parsed_frame;
    for(const auto ttl:{0u,10001u}){auto changed=envelope;changed.ttl_ms=ttl;assert(c::log_chunk_envelope(changed,encoded).empty());}
    auto expired=wire;expired[12]=expired[13]=expired[14]=expired[15]=0;assert(c::parse_envelope(expired,parsed_frame)==c::Error::invalid_message);
    envelope.kind=c::Kind::log_request;assert(c::log_chunk_envelope(envelope,encoded).empty());
    assert(c::parse_envelope(wire.substr(0,40),parsed_frame)==c::Error::invalid_message); // A chunk header cut short.
    const auto json_request=std::string("{\"config_revision\":\"7\",\"kind\":\"log_request\",\"ttl_ms\":1000,\"payload\":")+c::log_request_json(transfer)+"}";
    assert(c::parse_envelope(json_request,parsed_frame)==c::Error::none&&parsed_frame.kind==c::Kind::log_request);
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
        auto invalid=envelope;invalid.config_revision=8;assert(value.admit(invalid,100,101)!=c::Error::none);
        if(accepted==c::Kind::log_result){auto missing=envelope;missing.log_chunk.reset();assert(value.admit(missing,100,101)==c::Error::invalid_message);}
        value.restrict_management();assert(value.admit(envelope,100,101)==c::Error::none);
        assert(value.admit(message(value,rejected),100,101)==c::Error::unauthorized_role);
        auto passive=session(role,false);passive.restrict_management();
        assert(passive.admit(message(passive,accepted),100,101)==c::Error::unauthorized_role);
        auto synchronizing=session(role);assert(synchronizing.begin_synchronization(7)==c::Error::none);
        assert(synchronizing.admit(envelope,100,101)==c::Error::unauthorized_role);
        assert(synchronizing.discard_synchronization_frame(envelope,100,101)==c::Error::none);
        auto retiring=session(role);retiring.restrict_lifecycle();
        assert(retiring.admit(envelope,100,101)==c::Error::unauthorized_role);
        assert(retiring.discard_lifecycle_frame(envelope,100,101)==(role==c::PeerRole::actuator?c::Error::none:c::Error::unsupported_kind));
    }
    auto provisional=session(c::PeerRole::provisional_candidate,false);
    assert(provisional.admit(message(provisional,c::Kind::log_request),100,101)==c::Error::unauthorized_role);
}
int main(){
    codec_tests();authorization_tests();
    std::cout<<"PASS log request and binary chunk codec bounds, bulk class, authenticated roles, management and late-frame drains\n";
}
