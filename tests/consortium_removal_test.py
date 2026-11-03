#!/usr/bin/env python3
"""Run actual removal admission, dispatch, one-shot permit and result cache with real Session/codecs."""
from pathlib import Path
import argparse,os,shlex,subprocess,tempfile
from consortium_gatt_callback_test import function
ROOT=Path(__file__).resolve().parents[1]
PREFIX=r'''
#include "amity/consortium/runtime.hpp"
#include "amity/consortium/owned_intent.hpp"
#include "amity/consortium/settings.hpp"
#include "controller_policy.hpp"
#include "enrollment_abort.hpp"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <sys/socket.h>
namespace amity::consortium {
using namespace platform;
uint64_t clock_ms=100;uint64_t now_ms(){return clock_ms;}
template<class T>T ident(unsigned n){char value[33];snprintf(value,sizeof(value),"%032x",n);return *T::parse(value);}
struct FakeTls{bool server=false;int socket=-1;model::OctopusId authenticated;std::vector<Peer> allowed;std::vector<std::string> frames;size_t next=0;bool receive(std::string& out,uint32_t timeout){assert(timeout<=2000);if(next==frames.size()){clock_ms+=timeout;return false;}out=frames[next++];return true;}};
struct Active{Session session;model::OctopusId peer;std::shared_ptr<FakeTls> tls=std::make_shared<FakeTls>();std::mutex mutex;std::deque<int> queue;bool cancelled=false,retiring=false,ready=true,management_ready=false,refreshing=false,context_ready=false,context_requested=false;std::atomic_bool restart_requested{false},execution_fenced{false};RuntimePeerStatus status;std::optional<EnrollmentAbort> enrollment_abort;model::MessageId removal_request;uint64_t removal_until=0;std::string retirement_payload;};
std::mutex lock;Group group;Identity identity;RuntimeStatus status;RuntimeLifecycleHooks lifecycle;RuntimeControlHooks controls;model::BootId boot;uint64_t configuration=1;std::vector<std::shared_ptr<Active>> links;
// Diagnostics are independently exercised by runtime transport tests. This
// fixture executes the completion and real roster-removal decisions directly.
struct LinkTrace{const char* phase="";bool retained=false;bool fail(const char*){return false;}};
struct DeferredLinkTrace{void capture(const Active&,const LinkTrace&,uint64_t){}};
unsigned group_saves=0,quiesces=0,markers=0,restarts=0,revocations=0,hash_delay=0;bool disk_ok=true,send_ok=true;unsigned failure=0;std::vector<std::string> events,sent;std::vector<uint32_t> sent_timeouts;
void gatt_client_disconnect(){}
esp_err_t save_group(const Group&){++group_saves;return disk_ok?ESP_OK:ESP_FAIL;}
bool current_peer_credential(const Active& active,const Group& value){for(const auto& peer:value.peers)if(peer.id==active.peer&&peer.id==active.tls->authenticated)for(const auto& allowed:active.tls->allowed)if(peer.id==allowed.id&&peer.der==allowed.der&&peer.pin==allowed.pin)return true;return false;}
// The cryptographic primitive is injected, not the production scope/digest
// construction. Production SHA-256 has separate known-vector tests.
std::string sha256(std::string_view text){clock_ms+=hash_delay;uint64_t h=1469598103934665603ull;for(unsigned char c:text){h^=c;h*=1099511628211ull;}char out[65];snprintf(out,sizeof(out),"%016llx%016llx%016llx%016llx",(unsigned long long)h,(unsigned long long)h,(unsigned long long)h,(unsigned long long)h);return out;}
std::string settings_quote(std::string_view text){return "\""+std::string(text)+"\"";}
Json parsed(std::string_view text){Json value;JsonError detail;assert(parse_json(text,value,detail));return value;}
bool send(Active&,Kind kind,std::string_view payload,uint32_t timeout){assert(timeout<=2000);assert(kind==Kind::membership_event||kind==Kind::membership_result);sent.emplace_back(payload);sent_timeouts.push_back(timeout);events.push_back(kind==Kind::membership_result?"response_attempt":"request_attempt");return send_ok;}
}
'''
SUITE=r'''
namespace amity::consortium {
void reset(bool main){
 clock_ms=100;configuration=1;controls={};group_saves=quiesces=markers=restarts=revocations=hash_delay=0;disk_ok=send_ok=true;failure=0;events.clear();sent.clear();sent_timeouts.clear();links.clear();removal_operations.clear();removal_authorization={};identity={};group={};status={};boot=ident<model::BootId>(6);identity.id=ident<model::OctopusId>(4);group.role=main?Group::coordinator:Group::member;group.consortium=ident<model::ConsortiumId>(2);group.authority=main?identity.id:ident<model::OctopusId>(3);group.epoch=1;group.revision=1;status.authority_ready=main;status.coordinator=main;status.member=!main;group.peers.push_back({ident<model::OctopusId>(3),"leaf","pin",true});
 lifecycle.retire=[]{++quiesces;events.push_back("quiescent");if(failure==1)return ESP_FAIL;if(failure==2)clock_ms+=2000;if(failure==3)links.front()->cancelled=true;if(failure==4)++configuration;return ESP_OK;};
 lifecycle.revoked=[](model::OctopusId){++revocations;};
 lifecycle.factory_reset=[](model::OctopusId issuer){assert(quiesces&&runtime_removed_device_reset_authorized(issuer));events.push_back("marker");if(failure==5)return ESP_FAIL;++markers;return ESP_OK;};
 lifecycle.after_factory_reset=[]{assert(markers&&events.back()=="response_attempt");++restarts;events.push_back("restart");};
}
std::shared_ptr<Active> connection(bool receiver,bool management=false,uint32_t caps=known_capabilities){
 auto active=std::make_shared<Active>();active->peer=ident<model::OctopusId>(3);active->tls->server=!receiver;active->tls->authenticated=active->peer;active->tls->allowed=group.peers;
 LinkScope sc{ident<model::LinkId>(5),group.consortium,identity.id,active->peer,group.authority,boot,ident<model::BootId>(7),1,1};
 assert(active->session.authenticate(sc,sc.peer_id,receiver?PeerRole::coordinator:PeerRole::actuator,true)==Error::none);HelloFrame hello;hello.consortium_id=sc.consortium_id;hello.octopus_id=sc.peer_id;hello.boot_id=sc.peer_boot_id;hello.coordinator_epoch=1;hello.config_revision=1;hello.required_capabilities=caps;
 assert(active->session.hello(hello,known_capabilities)==Error::none&&active->session.synchronized(1,true,true)==Error::none);if(management)active->session.restrict_management();
 active->ready=!management;active->management_ready=management;active->status.id=active->peer;active->status.boot_id=sc.peer_boot_id;active->status.config_revision=1;active->status.negotiated_capabilities=caps;active->status.ready=!management;active->status.management_ready=management;links.push_back(active);return active;
}
Envelope message(const std::shared_ptr<Active>& active,unsigned id=20){const auto& sc=active->session.scope();Envelope e;e.consortium_id=sc.consortium_id;e.origin_octopus_id=sc.peer_id;e.destination_octopus_id=sc.local_id;e.origin_boot_id=sc.peer_boot_id;e.destination_boot_id=sc.local_boot_id;e.coordinator_epoch=1;e.config_revision=1;e.kind=Kind::membership_event;e.ttl_ms=2000;auto mutation=ident<model::MutationId>(id);const auto digest=retirement_hash(sc,mutation,2);e.payload=parsed("{\"mutation_id\":\""+mutation.str()+"\",\"proposal_digest\":\""+digest+"\",\"phase\":\"factory_reset\",\"membership_revision\":\"2\",\"expected_boot_id\":\""+boot.str()+"\"}");return e;}
bool dispatch(const std::shared_ptr<Active>& active,const Envelope& e){const auto received=clock_ms;return active->session.admit(e,received,received)==Error::none&&retire_member(*active,e,received);}
void codec_tests(){
 const auto base=parsed(R"({"protocol_version":1,"consortium_id":"11111111111111111111111111111111","origin_octopus_id":"33333333333333333333333333333333","origin_boot_id":"44444444444444444444444444444444","destination_octopus_id":"55555555555555555555555555555555","destination_boot_id":"66666666666666666666666666666666","coordinator_epoch":"1","config_revision":"1","kind":"membership_event","ttl_ms":2000,"payload":{"mutation_id":"99999999999999999999999999999999","proposal_digest":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","phase":"factory_reset","membership_revision":"2","expected_boot_id":"66666666666666666666666666666666"}})");
 unsigned cases=0,accepted=0;
 auto check_body=[&](const std::string& body,bool expected){
  Envelope envelope;const bool actual=parse_envelope(body,envelope)==Error::none;
  if(actual!=expected)std::cerr<<"Removal codec expected "<<expected<<" for "<<body<<'\n';
  assert(actual==expected);++cases;if(actual)++accepted;
 };
 auto check=[&](const Json& value,bool expected){check_body(settings_json(value),expected);};
 auto remove=[](Json& value,std::string_view field){value.object.erase(std::remove_if(value.object.begin(),value.object.end(),[&](const auto& item){return item.first==field;}),value.object.end());};
 struct ResultCase{const char* value;bool event,result;};
 const ResultCase results[]={{nullptr,true,false},{"reset_scheduled",false,true},{"rejected",false,true},{"unknown",false,true},{"retired",false,false},{"",false,false}};
 struct TtlCase{unsigned value;bool accepted;};
 const TtlCase ttls[]={{0,false},{1,true},{2000,true},{2001,false},{10000,false}};
 for(bool response:{false,true})for(const auto& result:results)for(const auto& ttl:ttls){
  auto value=base;value.get("kind")->string=response?"membership_result":"membership_event";value.get("ttl_ms")->integer=ttl.value;
  if(result.value)value.get("payload")->object.emplace_back("result",parsed('"'+std::string(result.value)+'"'));
  check(value,(response?result.result:result.event)&&ttl.accepted);
 }
 // These exact malformed payload values used to run only when an optional
 // JSON Schema dependency was installed. They always exercise the C++ parser.
 for(const auto* field:{"mutation_id","proposal_digest","phase","membership_revision","expected_boot_id"}){
  for(const auto* replacement:{"null","\"\"","true","0","\"00000000000000000000000000000000\"","\"01\"","\"18446744073709551616\""}){
   auto value=base;*value.get("payload")->get(field)=parsed(replacement);check(value,false);
  }
  auto value=base;remove(*value.get("payload"),field);check(value,false);
 }
 for(const auto* phase:{"retire","handover_commit"}){auto value=base;value.get("payload")->get("phase")->string=phase;check(value,false);}
 {auto value=base;value.get("payload")->object.emplace_back("extra",parsed("true"));check(value,false);}
 for(const auto* revision:{"0","1","18446744073709551615"}){auto value=base;value.get("payload")->get("membership_revision")->string=revision;check(value,true);}
 for(const auto& digest:std::vector<std::string>{std::string(63,'a'),std::string(65,'a'),std::string(64,'A'),std::string(64,'g')}){auto value=base;value.get("payload")->get("proposal_digest")->string=digest;check(value,false);}
 {auto value=base;value.get("payload")->get("proposal_digest")->string=std::string(64,'0');check(value,true);}
 // Closed envelope shape and typed identity/counter boundaries are checked
 // separately from Session's authenticated scope and removal authorization.
 for(const auto& entry:base.object){auto value=base;remove(value,entry.first);check(value,false);}
 {auto value=base;value.object.emplace_back("extra",parsed("true"));check(value,false);}
 {auto value=base;value.object.emplace_back("ttl_ms",parsed("2000"));check(value,false);}
 {auto value=base;value.get("payload")->object.emplace_back("phase",parsed("\"factory_reset\""));check(value,false);}
 for(const auto* field:{"consortium_id","origin_octopus_id","origin_boot_id","destination_octopus_id","destination_boot_id"}){
  for(const auto& id:std::vector<std::string>{std::string(32,'0'),std::string(31,'1'),std::string(33,'1'),std::string(32,'A'),std::string(32,'g')}){
   auto value=base;value.get(field)->string=id;check(value,false);
  }
 }
 for(const auto* field:{"coordinator_epoch","config_revision"}){
  for(const auto* replacement:{"null","true","0","\"\"","\"01\"","\"-1\"","\"18446744073709551616\""}){
   auto value=base;*value.get(field)=parsed(replacement);check(value,false);
  }
  for(const auto* revision:{"0","18446744073709551615"}){auto value=base;value.get(field)->string=revision;check(value,true);}
 }
 for(const auto* replacement:{"null","true","\"1\"","0","2"}){auto value=base;*value.get("protocol_version")=parsed(replacement);check(value,false);}
 for(const auto* replacement:{"null","true","\"1\"","10001"}){auto value=base;*value.get("ttl_ms")=parsed(replacement);check(value,false);}
 {auto body=settings_json(base);const auto at=body.find("\"ttl_ms\":2000");assert(at!=std::string::npos);body.replace(at,13,"\"ttl_ms\":-1");check_body(body,false);}
 {auto value=base;value.get("kind")->string="unknown";check(value,false);}
 {auto value=base;*value.get("payload")=parsed("null");check(value,false);}
 std::cout<<"PASS mandatory removal envelope/payload codec cases: "<<cases<<" total, "<<accepted<<" accepted, TTL1/2000 limits, exact result values, closed/unique fields and malformed identity/digest/revision boundaries\n";
}
void receiver_tests(){
 for(bool management:{false,true}){reset(false);auto active=connection(true,management);assert(dispatch(active,message(active))&&group.role==Group::retired&&markers==1&&restarts==1);assert(events==std::vector<std::string>({"quiescent","marker","response_attempt","restart"}));assert(parsed(sent.back()).get("result")->string=="reset_scheduled");assert(!runtime_removed_device_reset_authorized(active->peer));assert(!dispatch(active,message(active,21))&&markers==1);}
 for(unsigned fault=0;fault<14;++fault){reset(false);auto active=connection(true);auto e=message(active);switch(fault){case 0:e.origin_boot_id=ident<model::BootId>(99);break;case 1:e.destination_boot_id=ident<model::BootId>(99);break;case 2:e.origin_octopus_id=ident<model::OctopusId>(99);break;case 3:e.destination_octopus_id=ident<model::OctopusId>(99);break;case 4:e.consortium_id=ident<model::ConsortiumId>(99);break;case 5:++e.config_revision;break;case 6:++e.coordinator_epoch;break;case 7:e.payload.get("expected_boot_id")->string=ident<model::BootId>(99).str();break;case 8:e.payload.get("membership_revision")->string="1";break;case 9:e.payload.get("proposal_digest")->string=std::string(64,'a');break;case 10:e.payload.get("phase")->string="retire";break;case 11:active->tls->authenticated=ident<model::OctopusId>(99);break;case 12:e.ttl_ms=2001;break;case 13:configuration=2;break;}assert(!dispatch(active,e)&&!markers&&!restarts);}
 for(unsigned fault=1;fault<=5;++fault){reset(false);auto active=connection(true);failure=fault;assert(!dispatch(active,message(active))&&!markers&&!restarts&&group.role==Group::retired);}
 reset(false);auto active=connection(true);disk_ok=false;assert(!dispatch(active,message(active))&&group.role==Group::member&&!quiesces&&!markers);
 reset(false);active=connection(true);send_ok=false;assert(!dispatch(active,message(active))&&markers==1&&restarts==1); // LostACK still finishes explicitly authorized durable reset.
 reset(true);active=connection(false);assert(!dispatch(active,message(active))&&!group_saves&&!markers); // Main can never be reset by a member.
 reset(false);active=connection(true,false,known_capabilities&~factory_reset_removal);assert(!dispatch(active,message(active))&&!markers);
 reset(false);active=connection(true);auto a=active->session.scope(),b=a;std::swap(b.local_id,b.peer_id);std::swap(b.local_boot_id,b.peer_boot_id);assert(retirement_hash(a,ident<model::MutationId>(9),2)==retirement_hash(b,ident<model::MutationId>(9),2));b.peer_boot_id=ident<model::BootId>(99);assert(retirement_hash(a,ident<model::MutationId>(9),2)!=retirement_hash(b,ident<model::MutationId>(9),2));
 std::cout<<"PASS actual target removal: exact scope/boots/revision/digest/capability, main denial, ready+management, quiescence before durable marker, originaldeadline/revocation, ACK-loss restart, no replay\n";
}
void source_tests(){
 const auto request=ident<model::MessageId>(30),other=ident<model::MessageId>(31);const auto peer=ident<model::OctopusId>(3);
 for(bool management:{false,true}){reset(true);auto active=connection(false,management);assert(runtime_remove_octopus(peer,request,1,active->status.boot_id)==ESP_OK&&group.peers.empty()&&revocations==1&&active->retiring);assert(parsed(runtime_removal_operation(peer,request)).get("stage")->string=="admitted");assert(runtime_remove_octopus(peer,request,1,active->status.boot_id)==ESP_OK&&group_saves==1);assert(runtime_remove_octopus(identity.id,request,1,{})==ESP_ERR_INVALID_ARG);const auto original=active->retirement_payload;assert(parsed(original).get("expected_boot_id")->string==active->status.boot_id.str());clock_ms+=2000;assert(!finish_retirement(*active)&&sent.empty());assert(parsed(runtime_removal_operation(peer,request)).get("reset")->string=="unconfirmed");}
 reset(true);assert(runtime_remove_octopus(peer,request,1,{})==ESP_OK&&group.peers.empty());assert(parsed(runtime_removal_operation(peer,request)).get("reset")->string=="offline");assert(links.empty()&&sent.empty());
 reset(true);auto active=connection(false,false,known_capabilities&~factory_reset_removal);assert(runtime_remove_octopus(peer,request,1,{})==ESP_ERR_NOT_SUPPORTED&&group.peers.size()==1&&!group_saves);
 reset(true);active=connection(false);assert(runtime_remove_octopus(peer,request,2,{})==ESP_ERR_INVALID_STATE&&runtime_remove_octopus(peer,request,1,ident<model::BootId>(99))==ESP_ERR_INVALID_STATE&&!group_saves);
 reset(true);active=connection(false);active->refreshing=true;assert(runtime_remove_octopus(peer,request,1,active->status.boot_id)==ESP_ERR_INVALID_STATE&&!group_saves&&group.peers.size()==1&&!active->retiring);
 active->refreshing=false;configuration=2;assert(runtime_remove_octopus(peer,request,1,active->status.boot_id)==ESP_ERR_INVALID_STATE&&!group_saves&&group.peers.size()==1);
 reset(true);active=connection(false);disk_ok=false;assert(runtime_remove_octopus(peer,request,1,{})==ESP_FAIL&&group.peers.size()==1&&!active->retiring);
 reset(true);active=connection(false);active->cancelled=true;assert(runtime_remove_octopus(peer,request,1,{})==ESP_OK&&!active->retiring&&parsed(runtime_removal_operation(peer,request)).get("reset")->string=="offline");
 reset(false);assert(runtime_remove_octopus(peer,request,1,{})==ESP_ERR_INVALID_STATE&&!group_saves);
 reset(true);assert(runtime_remove_octopus(identity.id,request,1,{})==ESP_ERR_INVALID_STATE&&!group_saves);
 reset(true);group.join_id=ident<model::JoinId>(9);group.joining_id=peer;group.join_digest=std::string(64,'a');active=connection(false);assert(runtime_remove_octopus(peer,request,1,{})==ESP_OK&&active->enrollment_abort&&!active->retiring&&parsed(runtime_removal_operation(peer,request)).get("reset")->string=="unconfirmed");
 reset(true);for(unsigned n=0;n<24;++n){group.peers={{peer,"leaf","pin",true}};assert(runtime_remove_octopus(peer,ident<model::MessageId>(100+n),group.revision,{})==ESP_OK);assert(removal_operations.size()<=8);}assert(parsed(runtime_removal_operation(peer,other)).get("reset")->string=="unconfirmed");
 std::cout<<"PASS actual main removal: atomic revision+reviewedBoot CAS, durable localdrop, unsupportedpeer preflight, offline noqueue, duplicate noop, candidate abort distinct, bounded8terminal/evictedunknown\n";
}
void empty_context_refresh_allows_reviewed_removal(){
 reset(true);auto active=connection(false);active->refreshing=true;
 std::function<void(model::OwnedContextResult)> completed;unsigned requests=0;
 controls.ready=[&](auto owner,uint64_t revision,auto done){assert(owner==active->peer&&revision==1);++requests;completed=std::move(done);return true;};
 assert(initialize_context(*active)&&active->context_requested&&!active->context_ready&&active->refreshing&&requests==1);
 const auto request=ident<model::MessageId>(95);
 assert(runtime_remove_octopus(active->peer,request,1,active->status.boot_id)==ESP_ERR_INVALID_STATE&&group_saves==0&&group.peers.size()==1);
 // The real HubService test proves an empty selector set produces published
 // with no outgoing message. Consume that completion through actual runtime.
 completed(model::OwnedContextResult::published);
 assert(!active->refreshing&&!active->context_ready&&active->context_requested&&!active->cancelled&&active->queue.empty());
 assert(initialize_context(*active)&&requests==1); // No repeated empty capture.
 assert(runtime_remove_octopus(active->peer,request,1,active->status.boot_id)==ESP_OK&&group.peers.empty()&&active->retiring&&revocations==1);
 assert(parsed(runtime_removal_operation(active->peer,request)).get("stage")->string=="admitted");
 std::cout<<"PASS empty owner context completes refresh without false input readiness, repeated captures or blocking exact reviewed removal\n";
}
void ack_tests(){
 for(bool publish_cleanup:{false,true}){
 reset(true);auto active=connection(false);const auto request=ident<model::MessageId>(80);assert(runtime_remove_octopus(active->peer,request,1,{})==ESP_OK);auto response=parsed(active->retirement_payload);response.object.emplace_back("result",parsed("\"reset_scheduled\""));const auto sc=active->session.scope();Envelope e;e.kind=Kind::membership_result;e.consortium_id=sc.consortium_id;e.origin_octopus_id=sc.peer_id;e.destination_octopus_id=sc.local_id;e.origin_boot_id=sc.peer_boot_id;e.destination_boot_id=sc.local_boot_id;e.coordinator_epoch=1;e.config_revision=1;e.ttl_ms=2000;std::string payload=active->retirement_payload;payload.pop_back();payload+=",\"result\":\"reset_scheduled\"}";
 // The real Remove transaction revokes membership in before_commit, then
 // publishes its saved-config deletion before this worker necessarily runs.
 if(publish_cleanup){configuration=2;clock_ms+=100;}
 active->tls->frames.push_back(envelope_json(e,payload));assert(finish_retirement(*active));assert(parsed(runtime_removal_operation(active->peer,request)).get("reset")->string=="scheduled");assert(!markers&&!restarts);
 assert(active->session.scope().config_revision==1&&sent.front()==active->retirement_payload&&sent_timeouts.front()==(publish_cleanup?1900u:2000u)); // Never restamp original scope/payload/deadline.
 }
 std::cout<<"PASS exact authenticated durable-marker ACK updates only its removal operation; admission never claims completed reset\n";
}
std::string inflight_payload(const LinkScope& scope,Kind kind){
 if(kind==Kind::heartbeat)return "{\"sequence\":\"1\",\"observation_cursor\":\"0\",\"ready\":true}";
 if(kind==Kind::device_status)return "{\"hostname\":\"member\"}";
 if(kind==Kind::scoped_result){model::OwnedAdapterSnapshot report;report.owner=scope.peer_id;report.boot=scope.peer_boot_id;report.ready=true;const auto bytes=owned_snapshot_json(report);assert(!bytes.empty());return "{\"snapshot\":"+bytes+"}";}
 if(kind==Kind::settings_result)return "{\"request_id\":\""+ident<model::MessageId>(90).str()+"\",\"operation\":\"read\",\"stage\":\"failed\",\"error\":\"NOT_READY\",\"snapshot\":null,\"rebooting\":false}";
 assert(kind==Kind::update_result);return "{\"request_id\":\""+ident<model::MessageId>(90).str()+"\",\"release_id\":\""+ident<model::MessageId>(91).str()+"\",\"image_sha256\":\""+std::string(64,'a')+"\",\"offset\":\"0\",\"stage\":\"status\",\"error\":\"\"}";
}
Envelope inflight_frame(const std::shared_ptr<Active>& active,Kind kind,uint64_t revision){
 const auto& scope=active->session.scope();Envelope e;e.consortium_id=scope.consortium_id;e.origin_octopus_id=scope.peer_id;e.destination_octopus_id=scope.local_id;e.origin_boot_id=scope.peer_boot_id;e.destination_boot_id=scope.local_boot_id;e.coordinator_epoch=scope.epoch;e.config_revision=revision;e.kind=kind;e.ttl_ms=1000;e.payload=parsed(inflight_payload(scope,kind));return e;
}
void queue_reset_ack(const std::shared_ptr<Active>& active,uint64_t revision=0){
 auto ack=inflight_frame(active,Kind::heartbeat,revision?revision:active->session.scope().config_revision);ack.kind=Kind::membership_result;ack.ttl_ms=2000;
 auto payload=active->retirement_payload;payload.pop_back();payload+=",\"result\":\"reset_scheduled\"}";
 active->tls->frames.push_back(envelope_json(ack,payload));
}
void inflight_status_does_not_hide_reset_ack(){
 for(bool older:{false,true})for(auto kind:{Kind::heartbeat,Kind::device_status,Kind::scoped_result,Kind::settings_result,Kind::update_result}){
  reset(true);auto active=connection(false);configuration=2;assert(active->session.advance_configuration(2)==Error::none);active->status.config_revision=2;
  auto prior=inflight_frame(active,kind,older?1:2);Envelope checked;
  assert(parse_envelope(envelope_json(prior,settings_json(prior.payload)),checked)==Error::none);
  assert((older?active->session.discard_stale(checked,clock_ms,clock_ms):active->session.admit(checked,clock_ms,clock_ms))==Error::none);
  const auto request=ident<model::MessageId>(83);assert(runtime_remove_octopus(active->peer,request,1,{})==ESP_OK);
  active->tls->frames.push_back(envelope_json(prior,settings_json(prior.payload)));queue_reset_ack(active);
  const auto until=active->removal_until;assert(finish_retirement(*active));
  assert(parsed(runtime_removal_operation(active->peer,request)).get("reset")->string=="scheduled"&&active->tls->next==2);
  assert(active->session.state()==ConnectionState::restricted&&!active->ready&&!active->management_ready&&!active->status.ready&&!active->status.management_ready);
  assert(active->session.admit(checked,clock_ms,clock_ms)!=Error::none&&active->removal_until==until&&clock_ms==100&&sent_timeouts.front()==2000);
 }
 for(unsigned fault=0;fault<10;++fault){
  reset(true);auto active=connection(false,false,fault==9?known_capabilities&~qualified_lookup:known_capabilities);configuration=2;assert(active->session.advance_configuration(2)==Error::none);active->status.config_revision=2;
  auto prior=inflight_frame(active,Kind::scoped_result,2);
  switch(fault){
   case 0:prior.origin_boot_id=ident<model::BootId>(99);break;
   case 1:prior.destination_boot_id=ident<model::BootId>(99);break;
   case 2:prior.origin_octopus_id=ident<model::OctopusId>(99);break;
   case 3:prior.destination_octopus_id=ident<model::OctopusId>(99);break;
   case 4:prior.consortium_id=ident<model::ConsortiumId>(99);break;
   case 5:++prior.coordinator_epoch;break;
   case 6:prior.config_revision=3;break;
   case 7:prior.config_revision=0;break;
   case 8:prior.payload.get("snapshot")->object.emplace_back("unexpected",Json{});break;
   default:break;
  }
  const auto request=ident<model::MessageId>(84);assert(runtime_remove_octopus(active->peer,request,1,{})==ESP_OK);
  active->tls->frames.push_back(envelope_json(prior,settings_json(prior.payload)));queue_reset_ack(active);
  assert(!finish_retirement(*active)&&active->tls->next==1&&parsed(runtime_removal_operation(active->peer,request)).get("reset")->string=="unconfirmed");
 }
 // Discarding an obsolete observation never permits an obsolete ACK.
 reset(true);auto current=connection(false);configuration=2;assert(current->session.advance_configuration(2)==Error::none);current->status.config_revision=2;
 const auto request_with_old_ack=ident<model::MessageId>(86);assert(runtime_remove_octopus(current->peer,request_with_old_ack,1,{})==ESP_OK);
 const auto old_status=inflight_frame(current,Kind::scoped_result,1);current->tls->frames.push_back(envelope_json(old_status,settings_json(old_status.payload)));queue_reset_ack(current,1);
 assert(!finish_retirement(*current)&&current->tls->next==2&&parsed(runtime_removal_operation(current->peer,request_with_old_ack)).get("reset")->string=="unconfirmed");
 // The original bounded drain cannot turn a valid status stream into an
 // unbounded wait, or interpret a later ACK after its original deadline.
 reset(true);auto active=connection(false);const auto request=ident<model::MessageId>(85);assert(runtime_remove_octopus(active->peer,request,1,{})==ESP_OK);
 const auto status_frame=inflight_frame(active,Kind::scoped_result,1);for(unsigned n=0;n<32;++n)active->tls->frames.push_back(envelope_json(status_frame,settings_json(status_frame.payload)));queue_reset_ack(active);
 assert(!finish_retirement(*active)&&active->tls->next==32&&active->removal_until==2100);
 std::cout<<"PASS reset ACK follows current/obsolete in-flight typed observations/results without granting readiness; exact peer/boot/epoch, future config, schema, capability, deadline and 32-frame bounds remain fenced\n";
}
void issued_removal_scope_denials(){
 for(unsigned fault=0;fault<19;++fault){
  reset(true);auto active=connection(false);const auto request=ident<model::MessageId>(82);
  assert(runtime_remove_octopus(active->peer,request,1,active->status.boot_id)==ESP_OK);configuration=2;
  switch(fault){
   case 0:{auto scope=active->session.scope();scope.peer_boot_id=ident<model::BootId>(99);assert(active->session.authenticate(scope,scope.peer_id,PeerRole::actuator,true)==Error::none);break;}
   case 1:{auto scope=active->session.scope();scope.link_id=ident<model::LinkId>(99);assert(active->session.authenticate(scope,scope.peer_id,PeerRole::actuator,true)==Error::none);break;}
   case 2:boot=ident<model::BootId>(99);break;
   case 3:group.epoch=2;break;
   case 4:group.consortium=ident<model::ConsortiumId>(99);break;
   case 5:group.authority=ident<model::OctopusId>(99);break;
   case 6:status.authority_ready=false;break;
   case 7:active->tls->authenticated=ident<model::OctopusId>(99);break;
   case 8:active->removal_request=ident<model::MessageId>(99);break;
   case 9:removal_operations.front().scope.reset();break;
   case 10:removal_operations.front().complete=true;break;
   case 11:++active->removal_until;break;
   case 12:clock_ms+=2000;break;
   case 13:active->retirement_payload.replace(active->retirement_payload.find(request.str()),32,ident<model::MessageId>(99).str());break;
   case 14:active->retiring=false;break;
   case 15:configuration=0;break;
   case 16:group.peers.push_back({active->peer,"new-leaf","new-pin",true});break;
   case 17:group.revision=1;break;
   case 18:hash_delay=2000;break;
  }
  assert(!finish_retirement(*active)&&sent.empty()&&!markers&&!restarts);
 }
 std::cout<<"PASS issued removal permit survives only local config advance: changed scope/boot/link/epoch/authority/authentication/mutation, unissued/replaced/expired permits and original-deadline exhaustion deny without send\n";
}
}
int main(){amity::consortium::codec_tests();amity::consortium::receiver_tests();amity::consortium::source_tests();amity::consortium::empty_context_refresh_allows_reviewed_removal();amity::consortium::ack_tests();amity::consortium::inflight_status_does_not_hide_reset_ack();amity::consortium::issued_removal_scope_denials();}
'''

def main():
 parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--source',type=Path,default=ROOT/'components/consortium/runtime.cpp')
 source=parser.parse_args().source.read_text();private=(ROOT/'components/consortium/removal_runtime.inc').read_text()
 actual=function((ROOT/'components/consortium/tls_link.cpp').read_text(),'std::string envelope_json(')+''.join(function(source,name) for name in ['void request_session_restart(', 'void context_capture_completed(', 'bool initialize_context(', 'bool lifecycle_receive(','std::string retirement_hash(','bool finish_retirement(','bool retire_member(','esp_err_t runtime_remove_octopus(','std::string runtime_removal_operation(','bool runtime_removed_device_reset_authorized('])
 with tempfile.TemporaryDirectory(prefix='amity-removal-') as temp:
  temp=Path(temp);(temp/'esp_err.h').write_text('#pragma once\nusing esp_err_t=int;constexpr int ESP_OK=0,ESP_ERR_INVALID_ARG=1,ESP_ERR_INVALID_STATE=2,ESP_ERR_NO_MEM=3,ESP_ERR_TIMEOUT=4,ESP_FAIL=5,ESP_ERR_NOT_FOUND=6,ESP_ERR_NOT_SUPPORTED=7;\n')
  cpp=temp/'test.cpp';binary=temp/'test';cpp.write_text(PREFIX+'namespace amity::consortium {\n'+private+actual+'\n}\n'+SUITE)
  sources=[ROOT/'components/consortium'/name for name in ['json.cpp','wire.cpp','session.cpp','owned_intent.cpp','settings.cpp','update.cpp','transfer.cpp']]+[ROOT/'components/amity_system/model.cpp']
  subprocess.run(shlex.split(os.environ.get('CXX','c++'))+['-std=c++17','-Wall','-Wextra','-Werror','-g','-fsanitize=address,undefined','-I'+str(temp)]+['-I'+str(ROOT/path) for path in ['components/consortium','components/consortium/include','components/amity_system/include']]+list(map(str,[cpp]+sources))+['-o',str(binary)],check=True)
  subprocess.run([str(binary)],check=True)
if __name__=='__main__':main()
