// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#include "amity/consortium/json.hpp"
#include "amity/consortium/session.hpp"
#include "amity/consortium/settings.hpp"
#include "amity/consortium/lifecycle.hpp"
#include "amity/consortium/radio.hpp"
#include "amity/consortium/owned_intent.hpp"
#include "amity/consortium/bootstrap.hpp"
#include "amity/consortium/update.hpp"
#include "setup_gesture.hpp"
#include "../components/consortium/candidate_enrollment.hpp"
#include "../components/consortium/controller_policy.hpp"
#include "../components/consortium/enrollment_abort.hpp"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
using namespace amity::consortium;
namespace m=amity::system;
template<class T>T ident(unsigned n){char text[33];std::snprintf(text,sizeof(text),"%032x",n);return *T::parse(text);}
std::string read(const std::string &p){std::ifstream f(p);assert(f);std::stringstream s;s<<f.rdbuf();return s.str();}
std::string replace(std::string s,std::string_view before,std::string_view after){auto p=s.find(before);assert(p!=std::string::npos);s.replace(p,before.size(),after);return s;}
void json_tests(){
    Json out;JsonError err;
    for(const auto &s:{"{}","[]","{\"a\":[1,true,false,null,\"text\"]}","{\"a\":\"\\ud83d\\udc19\"}","18446744073709551615"})assert(parse_json(s,out,err));
    for(const auto &s:{"", "{\"a\":1,\"a\":2}","{\"a\":1,\"\\u0061\":2}","{\"a\":\"\\u0000\"}","{\"a\":\"\\ud800\"}","{\"a\":\"\\udfff\"}","{} false","01","-1","1.5","1e2","18446744073709551616","[1,]","{\"a\":1,}"})assert(!parse_json(s,out,err));
    assert(!parse_json(std::string("{\"a\":\"")+char(0xc0)+char(0x80)+"\"}",out,err));
    assert(!parse_json(std::string("{\"a\":\"")+char(0xed)+char(0xa0)+char(0x80)+"\"}",out,err));
    assert(!parse_json(std::string("{}\0",3),out,err));
    assert(!parse_json(std::string(14,'[')+"1"+std::string(14,']'),out,err));
    std::string many="[0";for(int i=0;i<256;++i)many+=",0";many+="]";assert(!parse_json(many,out,err));
}
void frame_tests(std::string_view body){
    const auto bytes=frame(body);assert(!bytes.empty());
    for(size_t split=0;split<=bytes.size();++split){FrameReader reader;size_t used=0;auto r=reader.feed(bytes.data(),split,used,100);assert(used==split);assert(r==(split==bytes.size()?FrameReader::Result::frame:FrameReader::Result::incomplete));if(split<bytes.size()){r=reader.feed(bytes.data()+split,bytes.size()-split,used,101);assert(r==FrameReader::Result::frame);}assert(reader.body()==body);}
    FrameReader reader;size_t used;uint8_t zero[]={0,0,0,0};assert(reader.feed(zero,4,used,0)==FrameReader::Result::invalid_length);assert(reader.feed(zero,4,used,1)==FrameReader::Result::failed);
    reader.reset();uint8_t big[]={0,0,32,1};assert(reader.feed(big,4,used,0)==FrameReader::Result::invalid_length);
    reader.reset();assert(reader.feed(bytes.data(),1,used,0)==FrameReader::Result::incomplete);assert(reader.check_deadline(2000)==FrameReader::Result::timed_out);
    reader.reset();for(uint64_t i=0;i<10;++i)assert(reader.feed(bytes.data()+i,1,used,i*1000)==FrameReader::Result::incomplete);assert(reader.check_deadline(10000)==FrameReader::Result::timed_out);
    reader.reset();auto two=bytes;two.insert(two.end(),bytes.begin(),bytes.end());assert(reader.feed(two.data(),two.size(),used,0)==FrameReader::Result::frame);assert(used==bytes.size());reader.next_frame();assert(reader.feed(two.data()+used,two.size()-used,used,1)==FrameReader::Result::frame);
}

LinkScope scope(){return {ident<m::LinkId>(1),ident<m::ConsortiumId>(2),ident<m::OctopusId>(3),ident<m::OctopusId>(4),ident<m::OctopusId>(4),ident<m::BootId>(5),ident<m::BootId>(6),1,7};}
HelloFrame hello(const LinkScope& s){HelloFrame h;h.consortium_id=s.consortium_id;h.octopus_id=s.peer_id;h.boot_id=s.peer_boot_id;h.coordinator_epoch=s.epoch;h.config_revision=s.config_revision;h.required_capabilities=known_capabilities;return h;}
Envelope envelope(const LinkScope& s){Envelope e;e.consortium_id=s.consortium_id;e.origin_octopus_id=s.peer_id;e.destination_octopus_id=s.local_id;e.origin_boot_id=s.peer_boot_id;e.destination_boot_id=s.local_boot_id;e.coordinator_epoch=s.epoch;e.config_revision=s.config_revision;e.ttl_ms=10000;e.kind=Kind::heartbeat;return e;}
std::string packet(const LinkScope& s,Kind kind,const std::string& payload){return "{\"protocol_version\":1,\"consortium_id\":\""+s.consortium_id.str()+"\",\"origin_octopus_id\":\""+s.peer_id.str()+"\",\"origin_boot_id\":\""+s.peer_boot_id.str()+"\",\"destination_octopus_id\":\""+s.local_id.str()+"\",\"destination_boot_id\":\""+s.local_boot_id.str()+"\",\"coordinator_epoch\":\"1\",\"config_revision\":\"7\",\"kind\":\""+kind_name(kind)+"\",\"ttl_ms\":1000,\"payload\":"+payload+"}";}
m::OwnedIntent action(const LinkScope& s){m::OwnedIntent x;x.kind=m::OwnedIntentKind::activity;x.target={{s.consortium_id,s.local_id,0,m::Side::back},"Player"};x.generation=1;x.selected=true;x.source=x.target;return x;}
void envelope_tests(const std::string& body){
    Envelope out;assert(parse_envelope(body,out)==Error::none);m::OwnedIntent decoded;
    const auto* arguments=out.payload.get("arguments");
    assert(arguments&&closed(*arguments,{"selected","source_changed","source","audio","display","switch_device","input","repair"}));
    assert(parse_owned_intent(out.payload,decoded)==Error::none&&decoded.kind==m::OwnedIntentKind::activity&&decoded.selected&&!decoded.repair);
    assert(parse_envelope(replace(body,"\"repair\":false","\"repair\":true"),out)==Error::none);
    assert(parse_owned_intent(out.payload,decoded)==Error::none&&decoded.repair);
    auto input=action(scope());input.display=m::QualifiedDeviceName{{scope().consortium_id,scope().peer_id,1,m::Side::front},"TV"};
    const auto selected=packet(scope(),Kind::scoped_intent,owned_intent_json(input));Envelope routed;
    assert(parse_envelope(selected,routed)==Error::none&&parse_owned_intent(routed.payload,decoded)==Error::none&&decoded.display==input.display);
    input.display->port.consortium_id=ident<m::ConsortiumId>(99);assert(owned_intent_json(input).empty());
    auto key=action(scope());key.kind=m::OwnedIntentKind::key_edges;key.edge_count=1;key.edges[0]={4,true};
    Json key_payload;JsonError key_error;assert(parse_json(owned_intent_json(key),key_payload,key_error));
    assert(closed(key_payload,{"generation","action","arguments"})&&closed(*key_payload.get("arguments"),{"edges"}));
    assert(parse_owned_intent(key_payload,decoded)==Error::none&&decoded.edge_count==1&&decoded.edges[0].key==4&&decoded.edges[0].down);
    key.edges[0].down=false;assert(parse_json(owned_intent_json(key),key_payload,key_error));
    assert(closed(key_payload,{"generation","action","arguments"})&&closed(*key_payload.get("arguments"),{"edges"}));
    assert(parse_owned_intent(key_payload,decoded)==Error::none&&decoded.edge_count==1&&decoded.edges[0].key==4&&!decoded.edges[0].down);
    assert(parse_envelope(replace(body,"\"repair\":false","\"obsolete_repair\":false"),out)==Error::invalid_message);
    assert(parse_envelope(replace(body,"\"ttl_ms\":1000","\"ttl_ms\":0"),out)==Error::invalid_message);
    assert(parse_envelope(replace(body,"\"config_revision\":\"7\"","\"config_revision\":7"),out)==Error::invalid_message);
    assert(parse_envelope(replace(body,"\"config_revision\":\"7\"","\"config_revision\":\"07\""),out)==Error::invalid_message);
    assert(parse_envelope(replace(body,"\"protocol_version\":1","\"protocol_version\":2"),out)==Error::protocol_mismatch);
    assert(parse_envelope(replace(body,"\"kind\":\"scoped_intent\"","\"kind\":\"raw_cec\""),out)==Error::unsupported_kind);
    assert(parse_envelope(replace(body,"\"repair\":false","\"repair\":1"),out)==Error::invalid_message);
    assert(parse_envelope(replace(body,"\"repair\":false","\"repair\":false,\"toggle\":true"),out)==Error::invalid_message);
    assert(parse_envelope(replace(body,"\"ttl_ms\":1000","\"ttl_ms\":1000,\"extra\":false"),out)==Error::invalid_message);
    assert(parse_envelope(replace(body,"\"ttl_ms\":1000","\"ttl_ms\":1000,\"ttl_ms\":10"),out)==Error::invalid_message);
}
void session_tests(){
    const auto s=scope();auto h=hello(s);Session session;auto e=envelope(s);e.kind=Kind::scoped_intent;auto intent=action(s);JsonError detail;assert(parse_json(owned_intent_json(intent),e.payload,detail));
    assert(session.authenticate(s,ident<m::OctopusId>(99),PeerRole::coordinator,true)==Error::identity_conflict);assert(session.authenticate(s,s.peer_id,PeerRole::coordinator,true)==Error::none);assert(session.admit(e,0,0)==Error::not_ready);
    h.required_capabilities|=0x80000000u;assert(session.hello(h,known_capabilities)==Error::protocol_mismatch);h=hello(s);assert(session.hello(h,known_capabilities)==Error::none);assert(session.synchronized(7,true,false)==Error::not_ready);assert(session.synchronized(8,true,true)==Error::stale_config);assert(session.synchronized(7,true,true)==Error::none);
    assert(session.admit(e,100,101)==Error::none);assert(session.admit(e,100,102)==Error::none);
    // An authenticated current original controller needs no renewed grant,
    // including after an idle interval beyond the previous grant period.
    assert(session.admit(e,5100,5100)==Error::none);
    assert(session.admit(e,5100,5000)==Error::invalid_message&&session.admit(e,5100,15100)==Error::invalid_message);
    for(unsigned n=0;n<6;++n){auto bad=e;switch(n){case 0:bad.origin_boot_id=ident<m::BootId>(99);break;case 1:bad.destination_boot_id=ident<m::BootId>(99);break;case 2:bad.coordinator_epoch=2;break;case 3:bad.origin_octopus_id=ident<m::OctopusId>(99);break;case 4:bad.destination_octopus_id=ident<m::OctopusId>(99);break;case 5:bad.config_revision=8;break;}assert(session.admit(bad,5200,5200)!=Error::none);}
    intent.kind=m::OwnedIntentKind::key_edges;intent.edge_count=1;intent.edges[0]={4,true};assert(parse_json(owned_intent_json(intent),e.payload,detail));assert(session.admit(e,5200,5200)==Error::none);
    assert(session.admit(e,5200,15200)==Error::invalid_message);
    intent.edges[0].down=false;assert(parse_json(owned_intent_json(intent),e.payload,detail));assert(session.admit(e,5700,5700)==Error::none);
    session.disconnect();assert(session.admit(e,6000,6000)==Error::not_ready);
    assert(session.authenticate(s,s.peer_id,PeerRole::coordinator,false)==Error::none&&session.hello(h,known_capabilities)==Error::none);assert(session.admit(e,0,0)==Error::unauthorized_role);
    auto unknown=s;unknown.peer_boot_id={};assert(session.authenticate(unknown,s.peer_id,PeerRole::coordinator,true)==Error::none&&session.hello(h,known_capabilities)==Error::none&&session.scope().peer_boot_id==s.peer_boot_id);
}
void owner_activity_selection_tests(){
    auto request=action(scope());request.target.osd_name.clear();Json value;JsonError detail;m::OwnedIntent decoded;
    for(bool selected:{false,true})for(bool repair:{false,true}){
        request.selected=selected;request.repair=repair;request.activity_step=true;
        assert(parse_json(owned_intent_json(request),value,detail)&&parse_owned_intent(value,decoded)==Error::none);
        assert(decoded.selected==selected&&decoded.repair==repair&&decoded.source==request.source&&!decoded.activity_step);
        assert(!value.get("activity_step"));
    }
    value.object.emplace_back("activity_step",Json{});assert(parse_owned_intent(value,decoded)==Error::invalid_message);
    for(auto kind:{m::OwnedIntentKind::power,m::OwnedIntentKind::source_input,m::OwnedIntentKind::switch_input,m::OwnedIntentKind::announce_active_source,m::OwnedIntentKind::query_audio,m::OwnedIntentKind::standby_wire}){
        request.kind=kind;assert(owned_intent_json(request).empty());
    }
    Json retired;assert(parse_json("{\"action_result\":{}}",retired,detail)&&!valid_owned_result(retired));
}
void raw_key_edge_tests(){
    auto key=action(scope());key.kind=m::OwnedIntentKind::key_edges;key.edge_count=2;key.edges={{{0,true},{0,false}}};
    Json value;JsonError detail;m::OwnedIntent decoded;
    assert(parse_json(owned_intent_json(key),value,detail)&&parse_owned_intent(value,decoded)==Error::none);
    assert(decoded.edge_count==2&&decoded.edges[0].key==0&&decoded.edges[0].down&&!decoded.edges[1].down&&!decoded.target.port.octopus_id);
    assert(closed(value,{"generation","action","arguments"}));
    auto bad=value;bad.object.emplace_back("target",Json{});assert(parse_owned_intent(bad,decoded)==Error::invalid_message);
    for(const auto& edges:{std::string("[]"),std::string("[{\"key\":0,\"down\":true},{\"key\":1,\"down\":false}]"),
        std::string("[{\"key\":0,\"down\":false},{\"key\":0,\"down\":true}]"),std::string("[{\"key\":0,\"down\":true},{\"key\":0,\"down\":true}]"),
        std::string("[{\"key\":256,\"down\":true}]"),std::string("[{\"key\":0,\"down\":1}]"),std::string("[{\"key\":0,\"down\":true,\"repeat\":true}]"),
        std::string("[{\"key\":0,\"down\":true},{\"key\":0,\"down\":false},{\"key\":0,\"down\":false}]")}){
        assert(parse_json("{\"generation\":\"1\",\"action\":\"key_edges\",\"arguments\":{\"edges\":"+edges+"}}",bad,detail));
        assert(parse_owned_intent(bad,decoded)==Error::invalid_message);
    }
    for(const auto* old:{"key_down","key_up"}){
        assert(parse_json("{\"generation\":\"1\",\"action\":\""+std::string(old)+"\",\"arguments\":{\"key\":0}}",bad,detail));
        assert(parse_owned_intent(bad,decoded)==Error::invalid_message);
    }
}
void synchronization_tests(){
    const auto original=scope();JsonError detail;
    const auto payload=[&](std::string_view phase,uint64_t revision){return "{\"round_id\":\""+ident<m::MessageId>(120).str()+"\",\"phase\":\""+std::string(phase)+"\",\"config_revision\":\""+std::to_string(revision)+"\"}";};
    const auto encoded=packet(original,Kind::synchronization,payload("begin",8));Envelope begin;
    assert(static_cast<unsigned>(Kind::synchronization)==43&&!retired_kind(Kind::synchronization));
    assert(parse_envelope(encoded,begin)==Error::none&&begin.kind==Kind::synchronization);
    model::MessageId round;std::string phase;uint64_t target=0;
    assert(parse_synchronization(begin.payload,round,phase,target)==Error::none&&round==ident<m::MessageId>(120)&&phase=="begin"&&target==8);
    assert(synchronization_json(round,phase,target)==payload("begin",8));
    assert(synchronization_json({},phase,target).empty()&&synchronization_json(round,"retry",target).empty()&&synchronization_json(round,phase,0).empty());
    for(const auto phase:{"begin","ack","reject"})assert(parse_envelope(packet(original,Kind::synchronization,payload(phase,8)),begin)==Error::none);
    for(const auto invalid:{"0","08","18446744073709551616"})assert(parse_envelope(packet(original,Kind::synchronization,replace(payload("begin",8),"\"8\"","\""+std::string(invalid)+"\"")),begin)==Error::invalid_message);
    assert(parse_envelope(packet(original,Kind::synchronization,payload("retry",8)),begin)==Error::invalid_message);
    assert(parse_envelope(replace(encoded,"\"phase\":\"begin\"","\"phase\":\"begin\",\"extra\":true"),begin)==Error::invalid_message);
    assert(parse_envelope(replace(encoded,ident<m::MessageId>(120).str(),std::string(32,'0')),begin)==Error::invalid_message);
    assert(parse_envelope(encoded,begin)==Error::none);
    const auto start=[&](Session& value,PeerRole role=PeerRole::coordinator,uint32_t capabilities=known_capabilities,bool active=true){
        auto sc=original;if(role==PeerRole::actuator)sc.coordinator_id=sc.local_id;
        auto h=hello(sc);h.required_capabilities=capabilities;
        assert(value.authenticate(sc,sc.peer_id,role,active)==Error::none&&value.hello(h,known_capabilities)==Error::none);
    };
    Session member;start(member);
    assert(member.state()==ConnectionState::synchronizing&&member.admit(begin,100,100)==Error::none);
    for(const auto revision:{6u,8u,99u}){auto barrier=begin;barrier.config_revision=revision;assert(member.admit(barrier,100,100)==Error::none);}
    auto ordinary=envelope(original);ordinary.config_revision=8;assert(member.admit(ordinary,100,100)==Error::stale_config);
    for(unsigned field=0;field<9;++field){auto bad=begin;switch(field){
        case 0:bad.consortium_id=ident<m::ConsortiumId>(99);break;
        case 1:bad.origin_octopus_id=ident<m::OctopusId>(99);break;
        case 2:bad.destination_octopus_id=ident<m::OctopusId>(99);break;
        case 3:bad.origin_boot_id=ident<m::BootId>(99);break;
        case 4:bad.destination_boot_id=ident<m::BootId>(99);break;
        case 5:bad.coordinator_epoch=2;break;
        case 6:bad.ttl_ms=0;break;
        case 7:bad.ttl_ms=10001;break;
        case 8:bad.config_revision=0;break;
    }assert(member.admit(bad,100,100)!=Error::none);}
    assert(member.admit(begin,100,99)==Error::invalid_message&&member.admit(begin,100,1100)==Error::invalid_message);
    assert(member.admit(begin,100,100)==Error::none);
    assert(member.synchronized(7,true,true)==Error::none);
    assert(member.begin_synchronization(8)==Error::none);
    auto expected=original;expected.config_revision=8;
    assert(member.scope()==expected&&member.state()==ConnectionState::synchronizing);
    assert(member.begin_synchronization(8)==Error::none); // Same-revision retry keeps the authenticated stream.
    assert(member.begin_synchronization(7)==Error::stale_config&&member.begin_synchronization(0)==Error::stale_config&&member.scope()==expected);
    assert(member.admit(begin,100,101)==Error::none);
    auto stale_work=envelope(original);stale_work.kind=Kind::scoped_intent;
    assert(parse_json(owned_intent_json(action(original)),stale_work.payload,detail));
    assert(member.admit(stale_work,100,101)==Error::stale_config);
    assert(member.discard_synchronization_frame(stale_work,100,101)==Error::none&&member.discard_synchronization_frame(stale_work,100,101)==Error::none);
    assert(member.state()==ConnectionState::synchronizing&&member.scope()==expected);
    assert(member.discard_stale(stale_work,100,101)==Error::not_ready);
    auto current_work=stale_work;current_work.config_revision=8;
    assert(member.admit(current_work,100,101)==Error::unauthorized_role&&member.discard_synchronization_frame(current_work,100,101)==Error::none);
    assert(member.admit(current_work,100,101)==Error::unauthorized_role); // Discard never changes operational admission.
    assert(member.discard_synchronization_frame(begin,100,101)==Error::unsupported_kind);
    assert(member.discard_synchronization_frame(stale_work,100,99)==Error::invalid_message&&member.discard_synchronization_frame(stale_work,100,10100)==Error::invalid_message);
    for(const auto revision:{0u,9u}){auto invalid=stale_work;invalid.config_revision=revision;assert(member.discard_synchronization_frame(invalid,100,101)==Error::stale_config);}
    auto malformed=stale_work;malformed.payload.object.emplace_back("extra",Json{});assert(member.discard_synchronization_frame(malformed,100,101)==Error::invalid_message);
    for(unsigned field=0;field<6;++field){auto bad=stale_work;switch(field){
        case 0:bad.origin_boot_id=ident<m::BootId>(99);break;
        case 1:bad.destination_boot_id=ident<m::BootId>(99);break;
        case 2:bad.origin_octopus_id=ident<m::OctopusId>(99);break;
        case 3:bad.destination_octopus_id=ident<m::OctopusId>(99);break;
        case 4:bad.consortium_id=ident<m::ConsortiumId>(99);break;
        case 5:bad.coordinator_epoch=2;break;
    }assert(member.discard_synchronization_frame(bad,100,101)!=Error::none);}
    auto rollback=begin;assert(parse_json(payload("begin",7),rollback.payload,detail));assert(member.admit(rollback,100,101)==Error::stale_config);
    auto positive=envelope(expected);positive.kind=Kind::scoped_intent;assert(parse_json(owned_intent_json(action(expected)),positive.payload,detail));
    assert(member.admit(positive,100,101)==Error::unauthorized_role);
    auto heartbeat=envelope(expected);assert(member.admit(heartbeat,100,101)==Error::none);
    assert(member.synchronized(7,true,true)==Error::stale_config&&member.synchronized(8,true,true)==Error::none);
    assert(member.admit(positive,100,101)==Error::none);
    assert(member.discard_synchronization_frame(stale_work,100,101)==Error::not_ready);
    member.restrict_management();assert(member.begin_synchronization(8)==Error::none&&member.synchronized(8,true,true)==Error::none);
    member.restrict_lifecycle();assert(member.begin_synchronization(8)==Error::not_ready&&member.admit(begin,100,101)==Error::unauthorized_role);
    Session no_chunks;start(no_chunks,PeerRole::coordinator,known_capabilities&~chunked_snapshots);assert(no_chunks.begin_synchronization(8)==Error::unsupported_kind&&no_chunks.admit(begin,100,100)==Error::unsupported_kind);
    Session management;start(management,PeerRole::coordinator,known_capabilities&~live_configuration);assert(management.synchronized(7,true,true)==Error::none);management.restrict_management();assert(management.admit(begin,100,100)==Error::none&&management.begin_synchronization(8)==Error::none);
    assert(management.discard_synchronization_frame(stale_work,100,101)==Error::none);
    Session controller;start(controller,PeerRole::actuator);assert(controller.begin_synchronization(8)==Error::none);
    for(const auto role:{PeerRole::coordinator,PeerRole::actuator}){
        Session value;start(value,role);
        for(const auto phase:{"begin","ack","reject"}){auto message=begin;assert(parse_json(payload(phase,8),message.payload,detail));
            assert(value.admit(message,100,100)==((std::string_view(phase)=="begin")== (role==PeerRole::coordinator)?Error::none:Error::unauthorized_role));
        }
    }
    for(const auto role:{PeerRole::coordinator,PeerRole::provisional_candidate}){Session passive;start(passive,role,known_capabilities,false);assert(passive.begin_synchronization(8)==Error::not_ready&&passive.admit(begin,100,100)==Error::unauthorized_role);}
    Session negotiating;assert(negotiating.authenticate(original,original.peer_id,PeerRole::coordinator,true)==Error::none&&negotiating.begin_synchronization(8)==Error::not_ready);
}
void logical_session_restart_tests(){
    const auto original=scope();Session session;JsonError detail;
    assert(session.authenticate(original,original.peer_id,PeerRole::coordinator,true)==Error::none);
    assert(session.hello(hello(original),known_capabilities)==Error::none&&session.synchronized(7,true,true)==Error::none);
    auto remembered=envelope(original);
    assert(session.admit(remembered,100,100)==Error::none);
    const auto logical_link=ident<m::LinkId>(143);
    assert(session.begin_synchronization(7,logical_link)==Error::none);
    auto expected=original;expected.link_id=logical_link;
    assert(session.scope()==expected&&session.state()==ConnectionState::synchronizing);
    assert(session.admit(remembered,100,101)==Error::none); // Ordinary heartbeats remain admissible during synchronization.
    auto positive=envelope(expected);positive.kind=Kind::scoped_intent;
    assert(parse_json(owned_intent_json(action(expected)),positive.payload,detail));
    assert(session.admit(positive,100,101)==Error::unauthorized_role);
    assert(session.synchronized(7,true,true)==Error::none&&session.admit(positive,100,101)==Error::none);
    auto foreign=positive;foreign.origin_boot_id=ident<m::BootId>(99);assert(session.admit(foreign,100,102)==Error::stale_boot);
    foreign=positive;foreign.config_revision=6;assert(session.admit(foreign,100,102)==Error::stale_config);
    assert(session.begin_synchronization(7,logical_link)==Error::identity_conflict&&session.scope()==expected&&session.state()==ConnectionState::ready);
    assert(session.begin_synchronization(6,ident<m::LinkId>(145))==Error::stale_config&&session.scope()==expected);
    assert(session.begin_synchronization(8,ident<m::LinkId>(145))==Error::none);
    expected.link_id=ident<m::LinkId>(145);expected.config_revision=8;
    assert(session.scope()==expected&&session.state()==ConnectionState::synchronizing);
}
void original_controller_tests(){
    using namespace amity::consortium::platform;const auto local=ident<m::OctopusId>(2);Group group;assert(controller_membership_compatible(group)&&!original_controller(group,local));group.role=Group::coordinator;group.authority=local;group.epoch=1;assert(original_controller(group,local)&&!original_controller(group,ident<m::OctopusId>(3)));
    for(auto role:{Group::member,Group::prepared,Group::retired}){group.role=role;assert(controller_membership_compatible(group)&&!original_controller(group,local));}group.role=Group::coordinator;group.epoch=2;assert(!controller_membership_compatible(group));
    for(auto role:{PeerRole::coordinator,PeerRole::actuator}){const auto sc=scope();Session session;assert(session.authenticate(sc,sc.peer_id,role,true)==Error::none&&session.hello(hello(sc),known_capabilities)==Error::none&&session.synchronized(7,true,true)==Error::none);for(unsigned restricted=0;restricted<2;++restricted){if(restricted)session.restrict_management();for(unsigned number:{1u,2u,3u,4u,5u,6u,12u,13u,18u,19u,20u,21u,22u,23u,24u,25u,26u,33u,34u,35u,36u}){auto e=envelope(sc);e.kind=static_cast<Kind>(number);assert(retired_kind(e.kind)&&session.admit(e,100,100)==Error::unauthorized_role);}}}
    auto profile=hello(scope());assert(execution_profile_supported(profile,true,true,4,15));profile.required_capabilities&=~qualified_lookup;assert(!execution_profile_supported(profile,true,true,1,1));profile.optional_capabilities|=qualified_lookup;assert(execution_profile_supported(profile,true,true,1,1));assert(!execution_profile_supported(profile,false,true,1,1)&&!execution_profile_supported(profile,true,false,4,15));
    profile.required_capabilities&=~live_configuration;assert(!execution_profile_supported(profile,true,true,4,15));profile.optional_capabilities|=live_configuration;assert(execution_profile_supported(profile,true,true,4,15));
}
void live_configuration_tests(){
    const auto old=scope();Session session;JsonError detail;
    const auto start=[&](Session& value,PeerRole role=PeerRole::coordinator,uint32_t caps=known_capabilities){
        auto h=hello(old);h.required_capabilities=caps;
        assert(value.authenticate(old,old.peer_id,role,true)==Error::none&&value.hello(h,known_capabilities)==Error::none&&value.synchronized(old.config_revision,true,true)==Error::none);
    };
    start(session);
    auto heartbeat=envelope(old);assert(parse_json("{\"sequence\":\"1\",\"observation_cursor\":\"0\",\"ready\":true}",heartbeat.payload,detail));
    assert(session.admit(heartbeat,100,100)==Error::none);
    for(auto invalid:{0u,6u,7u})assert(session.advance_configuration(invalid)==Error::stale_config);
    assert(session.advance_configuration(8)==Error::none);
    auto current=old;current.config_revision=8;assert(session.scope()==current&&session.state()==ConnectionState::ready);
    assert(session.discard_stale(heartbeat,100,101)==Error::none); // Obsolete observations are discarded without executing them.
    heartbeat.config_revision=8;assert(session.admit(heartbeat,100,101)==Error::none);
    auto intent=envelope(current);intent.kind=Kind::scoped_intent;
    assert(parse_json(owned_intent_json(action(current)),intent.payload,detail));
    assert(session.admit(intent,101,101)==Error::none); // Current synchronized authority is sufficient.
    assert(parse_json(owned_intent_json(action(current)),intent.payload,detail));assert(session.admit(intent,101,102)==Error::none);
    auto stale=envelope(old);stale.payload=heartbeat.payload;
    assert(session.admit(stale,100,102)==Error::stale_config);
    assert(session.discard_stale(stale,100,102)==Error::none&&session.discard_stale(stale,100,102)==Error::none);
    assert(session.discard_stale(stale,100,103)==Error::none);
    stale.config_revision=8;assert(session.admit(stale,100,103)==Error::none);

    for(auto invalid:{0u,8u,9u}){stale.config_revision=invalid;assert(session.discard_stale(stale,100,103)==Error::stale_config);}
    stale.config_revision=7;
    for(unsigned field=0;field<8;++field){auto bad=stale;switch(field){
        case 0:bad.consortium_id=ident<m::ConsortiumId>(99);break;
        case 1:bad.origin_octopus_id=ident<m::OctopusId>(99);break;
        case 2:bad.destination_octopus_id=ident<m::OctopusId>(99);break;
        case 3:bad.origin_boot_id=ident<m::BootId>(99);break;
        case 4:bad.destination_boot_id=ident<m::BootId>(99);break;
        case 5:bad.coordinator_epoch=2;break;
        case 6:bad.ttl_ms=0;break;
        case 7:bad.ttl_ms=10001;break;
    }assert(session.discard_stale(bad,100,103)!=Error::none);}
    assert(session.discard_stale(stale,100,99)==Error::invalid_message&&session.discard_stale(stale,100,10100)==Error::invalid_message);
    auto malformed=stale;malformed.payload.object.emplace_back("unknown",Json{});assert(session.discard_stale(malformed,100,103)==Error::invalid_message);
    auto foreign=envelope(old);foreign.kind=Kind::scoped_intent;auto target=action(old);target.target.port.octopus_id=old.peer_id;
    assert(parse_json(owned_intent_json(target),foreign.payload,detail));assert(session.discard_stale(foreign,100,103)==Error::target_not_local);
    target=action(old);assert(parse_json(owned_intent_json(target),foreign.payload,detail));
    assert(session.discard_stale(foreign,100,103)==Error::none); // Discarding obsolete work grants no execution authority.
    auto snapshot=envelope(current);snapshot.kind=Kind::snapshot_request;
    assert(session.admit(snapshot,100,104)==Error::none);snapshot.kind=Kind::snapshot_chunk;assert(session.admit(snapshot,100,104)==Error::none);
    Session missing;start(missing,PeerRole::coordinator,known_capabilities&~live_configuration);
    assert(missing.advance_configuration(8)==Error::unsupported_kind&&missing.admit(snapshot,100,104)==Error::stale_config);
    snapshot.config_revision=7;assert(missing.admit(snapshot,100,104)==Error::unauthorized_role);
    missing.restrict_management();auto update=envelope(old);update.kind=Kind::update_begin;assert(missing.admit(update,100,104)==Error::none);
    assert(missing.advance_configuration(8)==Error::not_ready); // Older peers remain update-capable, never operational.
    Session no_chunks;start(no_chunks,PeerRole::coordinator,known_capabilities&~chunked_snapshots);assert(no_chunks.admit(snapshot,100,104)==Error::unsupported_kind);
    Session actuator;start(actuator,PeerRole::actuator);assert(actuator.advance_configuration(8)==Error::none);
    auto wrong_role=foreign;assert(actuator.discard_stale(wrong_role,100,104)==Error::unauthorized_role);
    Session no_hostname;start(no_hostname,PeerRole::actuator,known_capabilities&~hostname_metadata);assert(no_hostname.advance_configuration(8)==Error::none);
    auto name=envelope(old);name.kind=Kind::device_status;assert(parse_json("{\"hostname\":\"member\"}",name.payload,detail));assert(no_hostname.discard_stale(name,100,104)==Error::unsupported_kind);
    session.restrict_lifecycle();assert(session.advance_configuration(9)==Error::not_ready&&session.discard_stale(stale,100,104)==Error::not_ready);
    Session negotiating;assert(negotiating.authenticate(old,old.peer_id,PeerRole::coordinator,true)==Error::none);assert(negotiating.advance_configuration(8)==Error::not_ready);
}
void hello_profile_tests(){
    auto value=hello(scope());value.firmware_identity=std::string(64,'a');value.config_hash=std::string(64,'b');
    HelloFrame parsed;const auto legacy=hello_json(value);assert(parse_hello(legacy,parsed)==Error::none);
    assert(!parsed.hardware_profile&&parsed.supported_tentacles==1&&parsed.configured_tentacles_mask==1);
    assert(legacy.find("supported_tentacles")==std::string::npos); // Closed legacy extended profile stays unchanged.
    value.hardware_profile=true;
    for(uint8_t count=1;count<=4;++count)for(uint8_t mask=0;mask<32;++mask){
        value.supported_tentacles=count;value.configured_tentacles_mask=mask;const auto body=hello_json(value);
        if(mask<(1u<<count)){assert(!body.empty()&&parse_hello(body,parsed)==Error::none);assert(parsed.hardware_profile&&parsed.supported_tentacles==count&&parsed.configured_tentacles_mask==mask);}
        else assert(body.empty());
    }
    value.supported_tentacles=4;value.configured_tentacles_mask=0;
    const auto all_unassigned=hello_json(value);assert(parse_hello(all_unassigned,parsed)==Error::none&&parsed.configured_tentacles_mask==0);
    for(const auto &body:{replace(all_unassigned,"\"supported_tentacles\":4", "\"supported_tentacles\":0"),replace(all_unassigned,"\"supported_tentacles\":4", "\"supported_tentacles\":5"),replace(all_unassigned,"\"supported_tentacles\":4", "\"supported_tentacles\":\"4\""),replace(all_unassigned,"\"configured_tentacles_mask\":0", "\"configured_tentacles_mask\":16"),replace(all_unassigned,"\"configured_tentacles_mask\":0", "\"configured_tentacles_mask\":true"),replace(all_unassigned,",\"supported_tentacles\":4", ""),replace(all_unassigned,",\"configured_tentacles_mask\":0", "")})assert(parse_hello(body,parsed)==Error::invalid_message);
    assert(parse_hello(replace(all_unassigned,"\"required_capabilities\":"+std::to_string(known_capabilities),"\"required_capabilities\":2147483648"),parsed)==Error::invalid_message);
    assert(parse_hello(replace(all_unassigned,"\"optional_capabilities\":0","\"optional_capabilities\":4294967295"),parsed)==Error::none);
    Session session;auto sc=scope();assert(session.authenticate(sc,sc.peer_id,PeerRole::coordinator,true)==Error::none);
    value.supported_tentacles=1;value.configured_tentacles_mask=2;assert(session.hello(value,known_capabilities)==Error::invalid_message);
    value.supported_tentacles=5;value.configured_tentacles_mask=0;assert(hello_json(value).empty()&&session.hello(value,known_capabilities)==Error::invalid_message);
    value.supported_tentacles=4;value.configured_tentacles_mask=0;assert(session.hello(value,known_capabilities)==Error::none);
}
void adapter_index_tests(){
    Json value;JsonError detail;const auto s=scope();
    m::QualifiedDeviceName name{{s.consortium_id,s.local_id,3,m::Side::back},"Player"};
    m::QualifiedDeviceName parsed_name;m::FullyQualifiedCecAddress parsed_address;
    assert(parse_json(m::name_json(name),value,detail)&&parse_name(value,parsed_name));
    value.get("tentacle_id")->integer=4;assert(!parse_name(value,parsed_name));
    assert(parse_json(m::address_json({name.port,4}),value,detail)&&parse_address(value,parsed_address));
    value.get("tentacle_id")->integer=4;assert(!parse_address(value,parsed_address));
    DeviceSettingsWrite write,parsed_write;write.expected_boot=s.local_boot_id;write.expected_revision=1;write.changes.push_back({3,"2.0.0.0"});
    assert(parse_json(device_settings_write_json(write),value,detail)&&parse_device_settings_write(value,parsed_write)==Error::none);
    value.get("changes")->array[0].get("tentacle_id")->integer=4;assert(parse_device_settings_write(value,parsed_write)==Error::invalid_message);
    const auto snapshot="{\"boot_id\":\""+s.local_boot_id.str()+"\",\"settings_revision\":\"1\",\"adapters\":[{\"tentacle_id\":3,\"name\":\"HDMI adapter 4\",\"configured\":true,\"default_front_physical_address\":\"2.0.0.0\",\"observed_front_physical_address\":null,\"effective_front_physical_address\":null}]}";
    assert(parse_json(snapshot,value,detail)&&valid_settings_snapshot(value));
    value.get("adapters")->array[0].get("tentacle_id")->integer=4;assert(!valid_settings_snapshot(value));
}
void update_tests() {
    const std::string base="\"request_id\":\""+ident<m::MessageId>(40).str()+"\",\"release_id\":\"0.1.56-dev\",\"image_sha256\":\""+std::string(64,'a')+"\"";
    const auto chunk="{"+base+",\"offset\":\"0\",\"data_hex\":\"0001\",\"chunk_sha256\":\""+std::string(64,'b')+"\"}";
    const auto valid=[&](Kind kind,const std::string& body){Json j;JsonError e;return parse_json(body,j,e)&&valid_update(kind,j);};
    assert(valid(Kind::update_chunk,chunk));
    assert(!valid(Kind::update_chunk,replace(chunk,"\"offset\":\"0\"","\"offset\":\"00\"")));
    assert(!valid(Kind::update_chunk,replace(chunk,"\"data_hex\":\"0001\"","\"data_hex\":\"001\"")));
    assert(!valid(Kind::update_chunk,replace(chunk,"0001",std::string(4098,'a'))));
    assert(!valid(Kind::update_chunk,replace(chunk,"0.1.56-dev","../image")));
    assert(!valid(Kind::update_chunk,replace(chunk,"\"offset\":\"0\"","\"offset\":0")));
    assert(!valid(Kind::update_chunk,replace(chunk,"\"offset\":\"0\"","\"offset\":\"0\",\"force\":true")));
    const auto begin="{"+base+",\"canonical_manifest\":\"{}\",\"signature_hex\":\"\"}";
    assert(valid(Kind::update_begin,begin));
    assert(valid(Kind::update_begin,begin.substr(0,begin.size()-1)+",\"validate_only\":true}"));
    assert(!valid(Kind::update_begin,begin.substr(0,begin.size()-1)+",\"validate_only\":1}")); // Only the compiled actuator verifier may allow unsigned development.
    assert(!valid(Kind::update_begin,replace(begin,"\"signature_hex\":\"\"","\"signature_hex\":\"ff\"")));
    const auto result="{"+base+",\"offset\":\"2048\",\"stage\":\"progress\",\"error\":\"\"}";
    assert(valid(Kind::update_result,result));
    assert(!valid(Kind::update_result,replace(result,"\"error\":\"\"","\"error\":\"arbitrary text\"")));
    for(auto role:{PeerRole::coordinator,PeerRole::actuator,PeerRole::provisional_candidate}) {
        auto sc=scope();Session session;auto h=hello(sc);assert(session.authenticate(sc,sc.peer_id,role,true)==Error::none);
        assert(session.hello(h,known_capabilities)==Error::none);
        auto e=envelope(sc);e.kind=Kind::update_begin;
        assert(session.admit(e,100,100)!=Error::none); // Never before synchronization.
        const auto synchronized=session.synchronized(7,true,true);
        if(role==PeerRole::provisional_candidate) {assert(session.admit(e,100,100)!=Error::none);continue;}
        assert(synchronized==Error::none);
        assert(session.admit(e,100,100)==(role==PeerRole::coordinator?Error::none:Error::unauthorized_role));
        e.kind=Kind::update_result;
        assert(session.admit(e,100,100)==(role==PeerRole::coordinator?Error::unauthorized_role:Error::none));
        e.destination_boot_id=ident<m::BootId>(999);assert(session.admit(e,100,100)==Error::stale_boot);
    }
    {auto sc=scope();Session management;auto peer=hello(sc);peer.required_capabilities=peer_firmware_update|lifecycle_journal;peer.optional_capabilities=0;
     assert(management.authenticate(sc,sc.peer_id,PeerRole::coordinator,true)==Error::none&&management.hello(peer,known_capabilities)==Error::none&&management.synchronized(7,true,true)==Error::none);management.restrict_management();auto message=envelope(sc);message.kind=Kind::update_begin;assert(management.admit(message,0,0)==Error::none);
     for(auto kind:{Kind::scoped_intent,static_cast<Kind>(2),static_cast<Kind>(18),static_cast<Kind>(7)}){message.kind=kind;assert(management.admit(message,0,0)!=Error::none);}}

    auto sc=scope();Session legacy;auto h=hello(sc);h.required_capabilities=known_capabilities&~peer_firmware_update;
    assert(legacy.authenticate(sc,sc.peer_id,PeerRole::coordinator,true)==Error::none);
    assert(legacy.hello(h,known_capabilities&~peer_firmware_update)==Error::none&&legacy.synchronized(7,true,true)==Error::none);
    auto e=envelope(sc);e.kind=Kind::update_begin;assert(legacy.admit(e,0,0)==Error::unsupported_kind);
}
MembershipRecord prepared(){MembershipRecord r;r.state=MembershipState::prepared;r.consortium_id=ident<m::ConsortiumId>(1);r.coordinator_id=ident<m::OctopusId>(2);r.epoch=1;r.join_id=ident<m::JoinId>(3);r.join_digest=std::string(64,'a');return r;}
void lifecycle_tests(){
    MembershipRecord disk;bool writes=true;unsigned count=0;auto save=[&](const MembershipRecord &r){++count;if(!writes)return false;disk=r;return true;};MembershipJournal journal({},save);auto p=prepared();writes=false;assert(journal.prepare_authenticated(p)==Error::storage_failure);assert(disk.state==MembershipState::unclaimed);writes=true;assert(journal.prepare_authenticated(p)==Error::none);assert(journal.prepare_authenticated(p)==Error::none);auto changed=p;changed.coordinator_id=ident<m::OctopusId>(9);assert(journal.prepare_authenticated(changed)==Error::transaction_conflict);assert(!journal.permits_execution());
    assert(journal.commit(p.join_id,p.join_digest,1)==Error::not_ready);auto link=ident<m::LinkId>(4);auto nonce=ident<m::MessageId>(5);assert(journal.challenge(p.join_id,p.join_digest,link,nonce)==Error::none);assert(journal.prove_lan(p.join_id,p.join_digest,ident<m::LinkId>(9),nonce)==Error::transaction_conflict);assert(journal.prove_lan(p.join_id,p.join_digest,link,nonce)==Error::none);
    MembershipJournal rebooted(disk,save);assert(rebooted.commit(p.join_id,p.join_digest,1)==Error::not_ready); // old TLS proof does not survive crash
    writes=false;assert(journal.commit(p.join_id,p.join_digest,1)==Error::storage_failure);assert(disk.state==MembershipState::prepared);writes=true;assert(journal.commit(p.join_id,p.join_digest,1)==Error::none);auto commit_writes=count;assert(journal.commit(p.join_id,p.join_digest,1)==Error::none);assert(count==commit_writes);assert(journal.abort(p.join_id,p.join_digest)==Error::not_ready);assert(journal.candidate_active(p.join_id,p.join_digest,1)==Error::none);assert(!journal.permits_execution());assert(journal.ready_after_sync(p.join_id,p.join_digest,1)==Error::none);assert(journal.permits_execution());MembershipJournal active_restart(disk,save);assert(!active_restart.permits_execution());
    auto historic=disk;historic.epoch=2;MembershipJournal incompatible(historic,save);assert(incompatible.record().state==MembershipState::recovery_required&&!incompatible.permits_execution());
    auto mutation=ident<m::MutationId>(12);assert(journal.retire(mutation,std::string(64,'d'),2)==Error::none);assert(journal.retire(mutation,std::string(64,'d'),2)==Error::none);MembershipJournal retired(disk,save);assert(retired.record().state==MembershipState::retired&&!retired.permits_execution());assert(retired.prepare_authenticated(p)==Error::transaction_conflict);
    MembershipRecord corrupt=p;corrupt.coordinator_id={};MembershipJournal invalid(corrupt,save);assert(invalid.record().state==MembershipState::recovery_required);
    // Cancellation is a durable passive decision, never an implicit return to
    // the candidate's former independent singleton authority.
    MembershipRecord cancelled_disk=p;bool cancel_writes=false;unsigned cancel_count=0;
    auto cancel_save=[&](const MembershipRecord& r){++cancel_count;if(!cancel_writes)return false;cancelled_disk=r;return true;};
    MembershipJournal cancelling(p,cancel_save);assert(cancelling.abort(p.join_id,p.join_digest)==Error::storage_failure);assert(cancelled_disk.state==MembershipState::prepared);
    assert(cancelling.abort(ident<m::JoinId>(99),p.join_digest)==Error::transaction_conflict);cancel_writes=true;assert(cancelling.abort(p.join_id,p.join_digest)==Error::none);
    MembershipJournal cancelled_boot(cancelled_disk,cancel_save);assert(cancelled_boot.record().state==MembershipState::retired&&!cancelled_boot.permits_execution());assert(cancelled_boot.record().join_id==p.join_id&&cancelled_boot.record().join_digest==p.join_digest);
    const auto cancelled_count=cancel_count;assert(cancelled_boot.abort(p.join_id,p.join_digest)==Error::none&&cancel_count==cancelled_count);assert(cancelled_boot.prepare_authenticated(p)==Error::transaction_conflict);assert(cancelled_boot.commit(p.join_id,p.join_digest,1)==Error::not_ready);
    amity::consortium::platform::Group candidate,next;candidate.role=amity::consortium::platform::Group::prepared;candidate.consortium=p.consortium_id;candidate.authority=p.coordinator_id;candidate.epoch=1;candidate.revision=1;candidate.join_id=p.join_id;candidate.join_digest=p.join_digest;candidate.join_revision=1;candidate.joining_id=ident<m::OctopusId>(8);candidate.peers.push_back({p.coordinator_id,"enrolled-leaf",std::string(64,'a'),false});
    using amity::consortium::platform::candidate_join_state;using amity::consortium::platform::abort_prepared_candidate;
    assert(std::string_view(candidate_join_state(candidate,p.join_id,p.join_digest))=="prepared");assert(std::string_view(candidate_join_state(candidate,p.join_id,std::string(64,'b')))=="unknown");assert(!abort_prepared_candidate(candidate,ident<m::JoinId>(99),p.join_digest,next));
    assert(abort_prepared_candidate(candidate,p.join_id,p.join_digest,next));assert(next.role==amity::consortium::platform::Group::retired&&next.mutation_id&&next.join_id==p.join_id&&!next.peers[0].active);assert(std::string_view(candidate_join_state(next,p.join_id,p.join_digest))=="aborted");
    auto active_candidate=candidate;active_candidate.role=amity::consortium::platform::Group::member;assert(!abort_prepared_candidate(active_candidate,p.join_id,p.join_digest,next));assert(std::string_view(candidate_join_state(active_candidate,p.join_id,p.join_digest))=="active");
    MembershipRoster roster([](const auto &,uint64_t){return true;});MemberCredential member{ident<m::OctopusId>(1),"DER",std::string(64,'a'),false};assert(roster.add(member,0)==Error::none);assert(roster.lookup(member.id));auto impostor=member;impostor.spki_sha256=std::string(64,'b');assert(roster.add(impostor,1)==Error::identity_conflict);assert(roster.revoke(member.id,1)==Error::none);assert(!roster.lookup(member.id));
    VerifiedLeafEvidence evidence{member.leaf_der,member.spki_sha256,member.id,certificate_expired|certificate_future,true,true,true};assert(OperationalCredentialPolicy::accepts(member,evidence));evidence.flags|=certificate_other_failure;assert(!OperationalCredentialPolicy::accepts(member,evidence));evidence.flags=0;evidence.leaf_der="OTHER";assert(!OperationalCredentialPolicy::accepts(member,evidence));
}
void source_abort_tests(const std::string& power){
    using namespace amity::consortium::platform;
    auto s=scope();Group group;group.consortium=s.consortium_id;group.authority=s.coordinator_id;group.epoch=s.epoch;
    EnrollmentAbort pending{{s.peer_id,"exact removed leaf",std::string(64,'a'),false},s.consortium_id,s.coordinator_id,s.epoch,ident<m::JoinId>(77),std::string(64,'b')};
    assert(pending.matches(group,s.peer_id));assert(!pending.matches(group,ident<m::OctopusId>(90)));
    auto changed=group;++changed.epoch;assert(!pending.matches(changed,s.peer_id));changed=group;changed.authority=ident<m::OctopusId>(91);assert(!pending.matches(changed,s.peer_id));
    std::string encoded;auto json=[&](std::string_view phase,std::string_view extra){Json value;JsonError error;encoded="{\"phase\":\""+std::string(phase)+"\",\"join_id\":\""+pending.join.str()+"\",\"join_digest\":\""+pending.digest+"\""+std::string(extra)+"}";assert(parse_json(encoded,value,error));return value;};
    auto status=json("status",",\"request\":true");assert(pending.may_send(status));auto abort=json("abort",",\"reason\":\"cancelled\"");assert(pending.may_send(abort));
    auto forbidden=abort;forbidden.object.emplace_back("ready",Json{});assert(!pending.may_send(forbidden));assert(!pending.may_send(json("abort",",\"reason\":\"force\"")));
    assert(!pending.may_send(json("commit",",\"membership_revision\":\"2\",\"ready\":false")));assert(!pending.may_send(json("challenge",",\"challenge_nonce\":\""+ident<m::MessageId>(1).str()+"\"")));
    auto wrapped=replace(power,"\"kind\":\"scoped_intent\"","\"kind\":\"enrollment_control\"");const auto payload_at=wrapped.find("\"payload\":");assert(payload_at!=std::string::npos);
    auto frame=[&](){return wrapped.substr(0,payload_at)+"\"payload\":"+encoded+"}";};
    for(const auto& state:{std::string("prepared"),std::string("committed"),std::string("active"),std::string("aborted"),std::string("unknown")}){
        auto response=json("status",",\"request\":false,\"state\":\""+state+"\""+((state=="committed"||state=="active")?",\"membership_revision\":\"2\"":""));
        assert(!pending.may_send(response));Envelope parsed;assert(parse_envelope(frame(),parsed)==Error::none);
        const auto result=enrollment_abort_status(pending,parsed.payload);assert(result!=EnrollmentAbortStatus::invalid&&result!=EnrollmentAbortStatus::ignore);
        if(state=="committed"||state=="active")assert(result!=EnrollmentAbortStatus::prepared); // committed decisions never authorize rollback.
        encoded.insert(encoded.size()-1,",\"unauthorized\":null");assert(parse_envelope(frame(),parsed)==Error::invalid_message);
    }
    assert(enrollment_abort_status(pending,status)==EnrollmentAbortStatus::invalid);
    auto prove=json("prove",",\"challenge_nonce\":\""+ident<m::MessageId>(1).str()+"\"");assert(enrollment_abort_status(pending,prove)==EnrollmentAbortStatus::ignore);
    auto other=pending;other.join=ident<m::JoinId>(78);assert(!other.may_send(abort)&&enrollment_abort_status(other,prove)==EnrollmentAbortStatus::invalid);
}
void radio_and_gesture_tests(){
    RadioArbiter radio;assert(radio.request(RadioMode::consortium_candidate,0,180000)==RadioResult::unavailable);radio.remote_reconnect_running(true);assert(radio.request(RadioMode::consortium_inviter,0,180000,false)==RadioResult::cancelling);auto r=radio.reservation();assert(!radio.accepts_callback(r.generation-1));assert(radio.request(RadioMode::remote_pairing,1,10000)==RadioResult::busy);assert(radio.cleanup_complete(r.generation-1,2)==RadioResult::stale);assert(radio.cleanup_complete(r.generation,100)==RadioResult::granted);assert(radio.admit_setup_connection(r.generation,1)==RadioResult::granted);assert(radio.admit_setup_connection(r.generation,2)==RadioResult::busy);assert(radio.external_matter_will_open(200,60000,false)==RadioResult::cancelling);assert(!radio.accepts_callback(r.generation));r=radio.reservation();assert(radio.cleanup_complete(r.generation,300)==RadioResult::granted);radio.matter_pase_active(true);assert(radio.release(r.generation,400)==RadioResult::busy);radio.matter_pase_active(false);assert(radio.release(r.generation,500)==RadioResult::cancelling);r=radio.reservation();assert(radio.cleanup_complete(r.generation,501)==RadioResult::granted&&radio.remote_may_reconnect());
    SetupEligibility eligible;eligible.physical_authorization_supported=eligible.radio_available=true;assert(consortium_setup_eligible(eligible));eligible.prepared_join=true;assert(!consortium_setup_eligible(eligible));
    using G=device_settings::SetupGesture;using Button=device_settings::SetupGestureButton;
    auto gesture=[](uint64_t duration){Button b;assert(b.update(false,0)==G::none);assert(b.update(false,50)==G::none);assert(b.update(true,100)==G::none);assert(b.update(true,150)==G::none);assert(b.update(false,100+duration)==G::none);return b.update(false,150+duration);};
    assert(gesture(2999)==G::initial_matter);assert(gesture(3000)==G::consortium);assert(gesture(9999)==G::consortium);
    // Add and reset share the accepted physical press edge. Release debounce
    // cannot promote a sub-ten-second hold or extend Add beyond ten seconds.
    assert(gesture(10000)==G::factory_reset);assert(gesture(10049)==G::factory_reset);assert(gesture(10050)==G::factory_reset);
    Button held;assert(held.update(false,0)==G::none);assert(held.update(false,50)==G::none);assert(held.update(true,100)==G::none);assert(held.update(true,150)==G::none);assert(held.update(true,3150)==G::none);assert(held.update(true,10099)==G::none);assert(held.next_check_ms(10099)==1);assert(held.update(true,10100)==G::factory_reset);assert(held.update(true,10150)==G::none);assert(held.update(false,10200)==G::none);assert(held.update(false,10250)==G::none);
    Button delayed;assert(delayed.update(false,0)==G::none);assert(delayed.update(false,50)==G::none);assert(delayed.update(true,100)==G::none);assert(delayed.update(true,250)==G::none);assert(delayed.next_check_ms(250)==9850);assert(delayed.update(true,10100)==G::factory_reset);
    Button bootheld;assert(bootheld.update(true,0)==G::none);assert(bootheld.update(true,50)==G::none);assert(bootheld.update(true,20000)==G::none);assert(bootheld.update(false,20001)==G::none);assert(bootheld.update(false,20051)==G::none);
}
void bootstrap_tests(){
    BootstrapRecords records;auto rid=ident<m::MessageId>(1);records.connect(5);assert(records.fragment(4,rid,8,0,"abcd")==BootstrapRecords::Result::stale);assert(records.fragment(5,rid,4097,0,"abcd")==BootstrapRecords::Result::invalid);assert(records.fragment(5,rid,8,0,"abcd")==BootstrapRecords::Result::incomplete);assert(records.fragment(5,rid,8,1,"X")==BootstrapRecords::Result::conflicting);assert(records.fragment(5,rid,8,0,"abcd")==BootstrapRecords::Result::incomplete);assert(records.fragment(5,rid,8,4,"efgh")==BootstrapRecords::Result::complete);CipherCounter counter;assert(counter.consume()==1);assert(records.finish(rid,"ciphertext"));assert(records.fragment(5,rid,8,0,"abcdefgh")==BootstrapRecords::Result::duplicate);assert(records.cached_response()=="ciphertext");assert(counter.consume()==2); // duplicate consumed no counter
    records.disconnect();records.connect(6);assert(records.fragment(5,rid,8,0,"abcdefgh")==BootstrapRecords::Result::stale);CipherCounter end(UINT32_MAX);assert(!end.consume());EnrollmentWindow window;assert(!window.open(false,0));assert(window.open(true,0));for(int i=0;i<5;++i){assert(window.admit(i));assert(!window.admit(i));window.failed_proof();}assert(!window.available(100));assert(window.open(true,100));assert(!window.available(180100));
}

int main(int argc,char** argv){assert(argc==2);(void)argv;const auto power=packet(scope(),Kind::scoped_intent,owned_intent_json(action(scope())));json_tests();frame_tests(power);envelope_tests(power);hello_profile_tests();adapter_index_tests();session_tests();owner_activity_selection_tests();raw_key_edge_tests();synchronization_tests();logical_session_restart_tests();live_configuration_tests();update_tests();lifecycle_tests();source_abort_tests(power);original_controller_tests();radio_and_gesture_tests();bootstrap_tests();std::cout<<"PASS consortium scoped-intent protocol, same-stream synchronization barriers and exact authority/boots/config/message-expiry fences, lifecycle/storage faults, update authorization, bootstrap/radio/gestures and all retired graph kinds\n";}
