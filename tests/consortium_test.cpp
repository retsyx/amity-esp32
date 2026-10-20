// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#include "amity/consortium/json.hpp"
#include "amity/consortium/session.hpp"
#include "amity/consortium/lifecycle.hpp"
#include "amity/consortium/radio.hpp"
#include "amity/consortium/bindings.hpp"
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
void envelope_tests(const std::string &body){
    Envelope out;assert(parse_envelope(body,out)==Error::none);
    Json document;JsonError detail;assert(parse_json(body,document,detail));
    const auto* payload=document.get("payload");assert(payload&&payload->get("command_sequence"));
    const auto sequence="\"command_sequence\": \""+payload->get("command_sequence")->string+"\"";
    for(const auto* invalid:{"0","01","18446744073709551616"})assert(parse_envelope(replace(body,sequence,"\"command_sequence\": \""+std::string(invalid)+"\""),out)==Error::invalid_message);
    assert(parse_envelope(replace(body,",\n    "+sequence,""),out)==Error::invalid_message);
    assert(parse_envelope(replace(body,"\"ttl_ms\": 1000","\"ttl_ms\": 0"),out)==Error::invalid_message);
    assert(parse_envelope(replace(body,"\"config_revision\": \"7\"","\"config_revision\": \"18446744073709551616\""),out)==Error::invalid_message);
    assert(parse_envelope(replace(body,"\"config_revision\": \"7\"","\"config_revision\": 7"),out)==Error::invalid_message);
    assert(parse_envelope(replace(body,"\"config_revision\": \"7\"","\"config_revision\": \"07\""),out)==Error::invalid_message);
    assert(parse_envelope(replace(body,"\"protocol_version\": 1","\"protocol_version\": 2"),out)==Error::protocol_mismatch);
    assert(parse_envelope(replace(body,"\"kind\": \"operation\"","\"kind\": \"raw_cec\""),out)==Error::unsupported_kind);
    assert(parse_envelope(replace(body,"\"on\": true","\"on\": 1"),out)==Error::invalid_message);
    assert(parse_envelope(replace(body,"\"on\": true","\"on\": true, \"toggle\": true"),out)==Error::invalid_message);
    assert(parse_envelope(replace(body,"\"ttl_ms\": 1000","\"ttl_ms\": 1000, \"extra\": false"),out)==Error::invalid_message);
    assert(parse_envelope(replace(body,"\"ttl_ms\": 1000","\"ttl_ms\": 1000, \"ttl_ms\": 10"),out)==Error::invalid_message);
}
LinkScope scope(){return {ident<m::LinkId>(1),ident<m::ConsortiumId>(2),ident<m::OctopusId>(3),ident<m::OctopusId>(4),ident<m::OctopusId>(4),ident<m::BootId>(5),ident<m::BootId>(6),1,7};}
HelloFrame hello(const LinkScope &s){HelloFrame h;h.consortium_id=s.consortium_id;h.octopus_id=s.peer_id;h.boot_id=s.peer_boot_id;h.coordinator_epoch=s.epoch;h.config_revision=s.config_revision;h.required_capabilities=known_capabilities;return h;}
Envelope envelope(const LinkScope &s){Envelope e;e.consortium_id=s.consortium_id;e.message_id=ident<m::MessageId>(10);e.origin_octopus_id=s.peer_id;e.destination_octopus_id=s.local_id;e.origin_boot_id=s.peer_boot_id;e.destination_boot_id=s.local_boot_id;e.coordinator_epoch=s.epoch;e.config_revision=s.config_revision;e.ttl_ms=10000;e.kind=Kind::operation;return e;}
void hello_profile_tests(){
    auto value=hello(scope());value.firmware_identity=std::string(64,'a');value.config_hash=std::string(64,'b');
    HelloFrame parsed;const auto legacy=hello_json(value);assert(parse_hello(legacy,parsed)==Error::none);
    assert(!parsed.hardware_profile&&parsed.supported_tentacles==1&&parsed.configured_tentacles_mask==1);
    assert(legacy.find("supported_tentacles")==std::string::npos); // Closed legacy extended profile stays unchanged.
    value.hardware_profile=true;
    for(uint8_t count=1;count<=5;++count)for(uint8_t mask=0;mask<32;++mask){
        value.supported_tentacles=count;value.configured_tentacles_mask=mask;const auto body=hello_json(value);
        if(mask<(1u<<count)){assert(!body.empty()&&parse_hello(body,parsed)==Error::none);assert(parsed.hardware_profile&&parsed.supported_tentacles==count&&parsed.configured_tentacles_mask==mask);}
        else assert(body.empty());
    }
    value.supported_tentacles=5;value.configured_tentacles_mask=0;
    const auto all_unassigned=hello_json(value);assert(parse_hello(all_unassigned,parsed)==Error::none&&parsed.configured_tentacles_mask==0);
    for(const auto &body:{replace(all_unassigned,"\"supported_tentacles\":5", "\"supported_tentacles\":0"),replace(all_unassigned,"\"supported_tentacles\":5", "\"supported_tentacles\":6"),replace(all_unassigned,"\"supported_tentacles\":5", "\"supported_tentacles\":\"5\""),replace(all_unassigned,"\"configured_tentacles_mask\":0", "\"configured_tentacles_mask\":32"),replace(all_unassigned,"\"configured_tentacles_mask\":0", "\"configured_tentacles_mask\":true"),replace(all_unassigned,",\"supported_tentacles\":5", ""),replace(all_unassigned,",\"configured_tentacles_mask\":0", "")})assert(parse_hello(body,parsed)==Error::invalid_message);
    assert(parse_hello(replace(all_unassigned,"\"required_capabilities\":258047","\"required_capabilities\":262144"),parsed)==Error::invalid_message);
    assert(parse_hello(replace(all_unassigned,"\"optional_capabilities\":0","\"optional_capabilities\":4294967295"),parsed)==Error::none);
    Session session;auto sc=scope();assert(session.authenticate(sc,sc.peer_id,PeerRole::coordinator,true)==Error::none);
    value.supported_tentacles=1;value.configured_tentacles_mask=2;assert(session.hello(value,known_capabilities)==Error::invalid_message);
    value.supported_tentacles=5;value.configured_tentacles_mask=0;assert(session.hello(value,known_capabilities)==Error::none);
}
Operation operation(const LinkScope &s){Operation o;o.command_id=ident<m::CommandId>(11);o.command_sequence=1;o.plan_id=ident<m::PlanId>(12);o.session_id=ident<m::SessionId>(13);o.target={{s.consortium_id,s.local_id,0,m::Side::back},4};o.execution_lease_id=ident<m::ExecutionLeaseId>(14);return o;}
void session_tests(){
    auto s=scope();Session session;auto h=hello(s);auto e=envelope(s);auto op=operation(s);
    assert(session.authenticate(s,ident<m::OctopusId>(99),PeerRole::coordinator,true)==Error::identity_conflict);
    assert(session.authenticate(s,s.peer_id,PeerRole::coordinator,true)==Error::none);
    assert(session.admit(e,0,0)==Error::not_ready);
    h.required_capabilities=known_capabilities+1;assert(session.hello(h,known_capabilities)==Error::protocol_mismatch);h=hello(s);
    auto old_peer=h;old_peer.required_capabilities=known_capabilities&~sequenced_operations;old_peer.optional_capabilities=0;assert(!supports_capabilities(old_peer,semantic_operations|token_holds|sequenced_operations));assert(supports_capabilities(h,semantic_operations|token_holds|sequenced_operations));assert(session.hello(h,known_capabilities&~sequenced_operations)==Error::protocol_mismatch);
    assert(session.hello(h,known_capabilities)==Error::none);
    assert(session.synchronized(7,true,false)==Error::not_ready);assert(session.synchronized(8,true,true)==Error::stale_config);assert(session.synchronized(7,true,true)==Error::none);
    assert(session.admit_operation(e,op,100,100)==Error::lease_expired);
    assert(session.execution_leases().issue(op.execution_lease_id,LeaseClass::control,5000,100)==Error::none);
    auto unsequenced=op;unsequenced.command_sequence=0;assert(session.admit_operation(e,unsequenced,100,101,false)==Error::invalid_message);
    assert(session.admit_operation(e,op,100,101)==Error::none);assert(session.admit_operation(e,op,100,102)==Error::duplicate);
    e.message_id=ident<m::MessageId>(15);assert(session.admit_operation(e,op,5100,5100)==Error::lease_expired); // fresh TTL cannot revive delayed command
    assert(session.execution_leases().issue(ident<m::ExecutionLeaseId>(16),LeaseClass::control,5000,5200)==Error::none);op.execution_lease_id=ident<m::ExecutionLeaseId>(16);
    auto old=e;old.destination_boot_id=ident<m::BootId>(99);assert(session.admit_operation(old,op,5200,5200)==Error::stale_boot);old=e;old.coordinator_epoch=2;assert(session.admit_operation(old,op,5200,5200)==Error::stale_epoch);old=e;old.origin_octopus_id=ident<m::OctopusId>(99);assert(session.admit_operation(old,op,5200,5200)==Error::identity_conflict);
    op.operation=SemanticOperation::key_press;assert(session.admit_operation(e,op,5200,5200)==Error::lease_expired);assert(session.execution_leases().issue(ident<m::ExecutionLeaseId>(17),LeaseClass::realtime_input,500,5200)==Error::none);op.execution_lease_id=ident<m::ExecutionLeaseId>(17);assert(session.admit_operation(e,op,5200,5200,false)==Error::none);assert(session.admit_operation(e,op,5200,5700,false)==Error::lease_expired);
    for(unsigned i=20;i<23;++i)assert(session.execution_leases().issue(ident<m::ExecutionLeaseId>(i),LeaseClass::realtime_input,500,5800+i)==Error::none);assert(session.execution_leases().live(LeaseClass::realtime_input,5850)==2);assert(!session.execution_leases().valid(ident<m::ExecutionLeaseId>(20),LeaseClass::realtime_input,s,5850));
    session.disconnect();assert(session.admit(e,6000,6000)==Error::not_ready);assert(!session.execution_leases().valid(op.execution_lease_id,LeaseClass::realtime_input,s,5850));
    assert(session.authenticate(s,s.peer_id,PeerRole::coordinator,false)==Error::none);assert(session.hello(h,known_capabilities)==Error::none);assert(session.admit(e,0,0)==Error::unauthorized_role);assert(session.synchronized(7,true,true)==Error::not_ready);
    auto unknown_boot=s;unknown_boot.peer_boot_id={};assert(session.authenticate(unknown_boot,s.peer_id,PeerRole::coordinator,true)==Error::none);assert(session.hello(h,known_capabilities)==Error::none);assert(session.scope().peer_boot_id==s.peer_boot_id);
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
        assert(session.admit(e,100,100,false)!=Error::none); // Never before synchronization.
        const auto synchronized=session.synchronized(7,true,true);
        if(role==PeerRole::provisional_candidate) {assert(session.admit(e,100,100,false)!=Error::none);continue;}
        assert(synchronized==Error::none);
        assert(session.admit(e,100,100,false)==(role==PeerRole::coordinator?Error::none:Error::unauthorized_role));
        e.kind=Kind::update_result;
        assert(session.admit(e,100,100,false)==(role==PeerRole::coordinator?Error::unauthorized_role:Error::none));
        e.destination_boot_id=ident<m::BootId>(999);assert(session.admit(e,100,100,false)==Error::stale_boot);
    }
    {auto sc=scope();Session management;auto peer=hello(sc);peer.required_capabilities=23;peer.optional_capabilities=0x80000000u|1000u;assert(management.authenticate(sc,sc.peer_id,PeerRole::coordinator,true)==Error::none&&management.hello(peer,known_capabilities)==Error::none);assert(!supports_capabilities(peer,sequenced_operations));assert(management.synchronized(7,true,true)==Error::none);management.restrict_management();auto message=envelope(sc);message.kind=Kind::update_begin;assert(management.admit(message,0,0,false)==Error::none);for(auto kind:{Kind::operation,static_cast<Kind>(2),Kind::execution_lease,Kind::plan_prepare,Kind::relay_reserve}){message.kind=kind;const auto admitted=management.admit(message,0,0,false);if(kind==Kind::execution_lease)assert(admitted==Error::none);else assert(admitted!=Error::none);}assert(management.execution_leases().live(LeaseClass::control,1)==0);}
    auto sc=scope();Session legacy;auto h=hello(sc);h.required_capabilities=known_capabilities&~peer_firmware_update;
    assert(legacy.authenticate(sc,sc.peer_id,PeerRole::coordinator,true)==Error::none);
    assert(legacy.hello(h,known_capabilities&~peer_firmware_update)==Error::none&&legacy.synchronized(7,true,true)==Error::none);
    auto e=envelope(sc);e.kind=Kind::update_begin;assert(legacy.admit(e,0,0)==Error::unsupported_kind);
}
void ledger_and_hold_tests(){
    auto s=scope();auto op=operation(s);CommandLedger ledger;ledger.activate(op.plan_id,op.session_id);assert(ledger.receive(op,0)==Error::none);assert(ledger.result(op.command_id,Outcome::applied,10));assert(ledger.query(op.command_id,100)==Outcome::applied);assert(!ledger.query(op.command_id,30010));assert(ledger.receive(op,40000)==Error::duplicate);
    auto missing=op;missing.command_id=ident<m::CommandId>(999);missing.command_sequence=0;assert(ledger.receive(missing,40000)==Error::invalid_message);ledger.invalidate();assert(ledger.receive(op,40000)==Error::stale_route);
    // A long-lived plan can issue thousands of commands without forgetting the
    // sequence fence after bounded outcome-cache retirement.
    ledger.activate(op.plan_id,op.session_id);op.command_sequence=1;op.command_id=ident<m::CommandId>(1000);
    assert(ledger.receive(op,0)==Error::none);assert(ledger.result(op.command_id,Outcome::applied,1));
    auto original=op;op.command_sequence=2;assert(ledger.receive(op,2)==Error::transaction_conflict);
    for(uint64_t n=2;n<=1000;++n){op.command_sequence=n;op.command_id=ident<m::CommandId>(1000+n);const auto now=n;assert(ledger.receive(op,now)==Error::none);assert(ledger.result(op.command_id,Outcome::applied,now));}
    assert(ledger.receive(original,40000000)==Error::duplicate);assert(!ledger.query(original.command_id,40000000));
    op.command_sequence=0;assert(ledger.receive(op,40000000)==Error::invalid_message);
    ledger.activate(op.plan_id,op.session_id);
    for(uint64_t n=1;n<=CommandLedger::capacity;++n){op.command_sequence=n;op.command_id=ident<m::CommandId>(2000+n);assert(ledger.receive(op,n)==Error::none);}
    op.command_sequence=33;op.command_id=ident<m::CommandId>(2033);assert(ledger.receive(op,40000)==Error::queue_full);
    assert(ledger.result(ident<m::CommandId>(2001),Outcome::rejected,40001));assert(ledger.receive(op,40002)==Error::none);
    assert(ledger.receive(op,70001)==Error::duplicate); // Failed admission did not advance high water.
    original.command_id=ident<m::CommandId>(2001);assert(ledger.receive(original,70002)==Error::duplicate);

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
    auto wrapped=replace(power,"\"kind\": \"operation\"","\"kind\": \"enrollment_control\"");const auto payload_at=wrapped.find("\"payload\":");assert(payload_at!=std::string::npos);
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
void original_controller_tests(){
    using namespace amity::consortium::platform;
    auto local=ident<m::OctopusId>(2);Group group;assert(controller_membership_compatible(group)&&!original_controller(group,local));
    group.role=Group::coordinator;group.authority=local;group.epoch=1;assert(original_controller(group,local)&&!original_controller(group,ident<m::OctopusId>(3)));
    for(auto role:{Group::member,Group::prepared,Group::retired}){group.role=role;assert(controller_membership_compatible(group)&&!original_controller(group,local));}
    group.role=Group::coordinator;group.epoch=2;assert(!controller_membership_compatible(group)&&!original_controller(group,local));
    for(auto role:{PeerRole::coordinator,PeerRole::actuator}){
        auto sc=scope();Session session;assert(session.authenticate(sc,sc.peer_id,role,true)==Error::none);assert(session.hello(hello(sc),known_capabilities)==Error::none&&session.synchronized(7,true,true)==Error::none);
        for(unsigned restricted=0;restricted<2;++restricted){if(restricted)session.restrict_management();for(auto kind:{static_cast<Kind>(26),static_cast<Kind>(2),static_cast<Kind>(35),static_cast<Kind>(36),static_cast<Kind>(6)}){auto e=envelope(sc);e.kind=kind;assert(session.admit(e,100,100,false)==Error::unauthorized_role);}}
    }
    auto profile=hello(scope());assert(execution_profile_supported(profile,true,true,5,31));profile.required_capabilities&=~original_controller_only;assert(!execution_profile_supported(profile,true,true,1,1));profile.optional_capabilities|=original_controller_only;assert(execution_profile_supported(profile,true,true,1,1));assert(!execution_profile_supported(profile,false,true,1,1)&&!execution_profile_supported(profile,true,false,5,31));
    auto sc=scope();Session member;assert(member.authenticate(sc,sc.peer_id,PeerRole::coordinator,true)==Error::none);assert(member.hello(hello(sc),known_capabilities)==Error::none&&member.synchronized(7,true,true)==Error::none);
    JsonError detail;auto lifecycle=envelope(sc);lifecycle.kind=Kind::membership_event;lifecycle.ttl_ms=2000;assert(parse_json("{\"phase\":\"handover_commit\"}",lifecycle.payload,detail));assert(member.admit(lifecycle,0,0,false)==Error::unauthorized_role);lifecycle.payload.get("phase")->string="factory_reset";assert(member.admit(lifecycle,0,0,false)==Error::none);
    lifecycle.kind=Kind::snapshot_request;assert(parse_json("{\"document_kind\":\"handover\"}",lifecycle.payload,detail));assert(member.admit(lifecycle,0,0,false)==Error::unauthorized_role);

}
void binding_tests(){
    auto s=scope();m::QualifiedDeviceName name{{s.consortium_id,s.local_id,0,m::Side::back},"Player"};m::ComponentBinding b;b.binding_id=ident<m::BindingId>(1);b.component_id=ident<m::ComponentId>(2);b.capability=m::Capability::power;b.device_name=name;b.endpoint={name.port,4};b.owner_octopus_id=s.local_id;b.domain_id=ident<m::DomainId>(3);b.binding_revision=1;b.domain_revision=1;b.status=m::BindingStatus::verified;b.physical_address_evidence_ref=ident<m::EvidenceId>(8);auto nav=b;nav.binding_id=ident<m::BindingId>(4);nav.capability=m::Capability::navigation;unsigned fenced=0;BindingSynchronizer sync(s.local_id,[&](m::BindingId,uint64_t){++fenced;});assert(sync.restore({b,nav})==Error::none);ResolutionEvidence observation{name,s.local_boot_id,10,8,true};assert(sync.observe(observation)==Error::none&&fenced==2);assert(sync.bindings()[0].status==m::BindingStatus::quarantined);
    BindingResolution resolution{ident<m::ResolutionId>(5),name,s.local_boot_id,10,{}};for(auto item:{b,nav}){item.endpoint.logical_address=8;item.binding_revision=2;item.domain_revision=2;item.status=m::BindingStatus::tentative;resolution.bindings.push_back({1,item});}
    auto partial=resolution;partial.bindings.pop_back();assert(sync.install(partial,true)==Error::stale_binding);assert(sync.bindings()[0].binding_revision==1);assert(sync.install(resolution,false)==Error::not_ready);auto stale=resolution;stale.observation_sequence=9;assert(sync.install(stale,true)==Error::stale_binding);assert(sync.install(resolution,true)==Error::none);assert(sync.install(resolution,true)==Error::none);auto conflict=resolution;conflict.bindings[0].binding.endpoint.logical_address=9;assert(sync.install(conflict,true)==Error::transaction_conflict);
    auto op=operation(s);op.binding_id=b.binding_id;op.component_id=b.component_id;op.domain_id=b.domain_id;op.target=b.endpoint;op.binding_revision=1;op.domain_revision=1;assert(!sync.target_current(op));op.target.logical_address=8;op.binding_revision=2;op.domain_revision=2;assert(sync.target_current(op));op.target.port.tentacle_id=1;assert(!sync.target_current(op));
}
void radio_and_gesture_tests(){
    RadioArbiter radio;assert(radio.request(RadioMode::consortium_candidate,0,180000)==RadioResult::unavailable);radio.remote_reconnect_running(true);assert(radio.request(RadioMode::consortium_inviter,0,180000,false)==RadioResult::cancelling);auto r=radio.reservation();assert(!radio.accepts_callback(r.generation-1));assert(radio.request(RadioMode::remote_pairing,1,10000)==RadioResult::busy);assert(radio.cleanup_complete(r.generation-1,2)==RadioResult::stale);assert(radio.cleanup_complete(r.generation,100)==RadioResult::granted);assert(radio.admit_setup_connection(r.generation,1)==RadioResult::granted);assert(radio.admit_setup_connection(r.generation,2)==RadioResult::busy);assert(radio.external_matter_will_open(200,60000,false)==RadioResult::cancelling);assert(!radio.accepts_callback(r.generation));r=radio.reservation();assert(radio.cleanup_complete(r.generation,300)==RadioResult::granted);radio.matter_pase_active(true);assert(radio.release(r.generation,400)==RadioResult::busy);radio.matter_pase_active(false);assert(radio.release(r.generation,500)==RadioResult::cancelling);r=radio.reservation();assert(radio.cleanup_complete(r.generation,501)==RadioResult::granted&&radio.remote_may_reconnect());
    SetupEligibility eligible;eligible.fresh_unconfigured_singleton=eligible.physical_authorization_supported=eligible.radio_available=eligible.bootstrap_ready=true;assert(initial_matter_eligible(eligible)&&consortium_setup_eligible(eligible));eligible.prepared_join=true;assert(!initial_matter_eligible(eligible)&&!consortium_setup_eligible(eligible));
    using G=device_settings::SetupGesture;using Button=device_settings::SetupGestureButton;
    auto gesture=[](uint64_t duration){Button b;assert(b.update(false,0)==G::none);assert(b.update(false,50)==G::none);assert(b.update(true,100)==G::none);assert(b.update(true,150)==G::none);assert(b.update(false,150+duration)==G::none);return b.update(false,200+duration);};
    assert(gesture(2999)==G::initial_matter);assert(gesture(3000)==G::consortium);assert(gesture(9999)==G::consortium);assert(gesture(10000)==G::factory_reset);
    Button held;assert(held.update(false,0)==G::none);assert(held.update(false,50)==G::none);assert(held.update(true,100)==G::none);assert(held.update(true,150)==G::none);assert(held.update(true,3150)==G::none);assert(held.update(true,10150)==G::factory_reset);assert(held.update(false,10200)==G::none);assert(held.update(false,10250)==G::none);
    Button bootheld;assert(bootheld.update(true,0)==G::none);assert(bootheld.update(true,50)==G::none);assert(bootheld.update(true,20000)==G::none);assert(bootheld.update(false,20001)==G::none);assert(bootheld.update(false,20051)==G::none);
}
void bootstrap_tests(){
    BootstrapRecords records;auto rid=ident<m::MessageId>(1);records.connect(5);assert(records.fragment(4,rid,8,0,"abcd")==BootstrapRecords::Result::stale);assert(records.fragment(5,rid,4097,0,"abcd")==BootstrapRecords::Result::invalid);assert(records.fragment(5,rid,8,0,"abcd")==BootstrapRecords::Result::incomplete);assert(records.fragment(5,rid,8,1,"X")==BootstrapRecords::Result::conflicting);assert(records.fragment(5,rid,8,0,"abcd")==BootstrapRecords::Result::incomplete);assert(records.fragment(5,rid,8,4,"efgh")==BootstrapRecords::Result::complete);CipherCounter counter;assert(counter.consume()==1);assert(records.finish(rid,"ciphertext"));assert(records.fragment(5,rid,8,0,"abcdefgh")==BootstrapRecords::Result::duplicate);assert(records.cached_response()=="ciphertext");assert(counter.consume()==2); // duplicate consumed no counter
    records.disconnect();records.connect(6);assert(records.fragment(5,rid,8,0,"abcdefgh")==BootstrapRecords::Result::stale);CipherCounter end(UINT32_MAX);assert(!end.consume());EnrollmentWindow window;assert(!window.open(false,0));assert(window.open(true,0));for(int i=0;i<5;++i){assert(window.admit(i));assert(!window.admit(i));window.failed_proof();}assert(!window.available(100));assert(window.open(true,100));assert(!window.available(180100));
}
int main(int argc,char **argv){assert(argc==2);std::string root=argv[1];const auto power=read(root+"/amity-octopus-consortium/examples/operation-ensure-power.json");json_tests();frame_tests(power);envelope_tests(power);for(auto name:{"operation-activate-proxy.json","operation-press-key.json"}){Envelope e;assert(parse_envelope(read(root+"/amity-octopus-consortium/examples/"+name),e)==Error::none);}hello_profile_tests();session_tests();update_tests();ledger_and_hold_tests();lifecycle_tests();source_abort_tests(power);original_controller_tests();binding_tests();radio_and_gesture_tests();bootstrap_tests();std::cout<<"consortium protocol/lifecycle fault tests passed\n";}
