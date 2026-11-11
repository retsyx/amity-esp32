#!/usr/bin/env python3
"""Run the actual in-place synchronizer against scripted authenticated TLS I/O."""
from pathlib import Path
import argparse
import os
import shlex
import subprocess
import tempfile
from consortium_gatt_callback_test import function
from setup_timing_support import timing_support

ROOT = Path(__file__).resolve().parents[1]
PREFIX = r'''
#include "amity/consortium/runtime.hpp"
#include "tls_receive_trace.hpp"
#include "amity/consortium/owned_intent.hpp"
#include "amity/consortium/transfer.hpp"
#include "amity/consortium/settings.hpp"
#include "amity/consortium/update.hpp"
#include "controller_policy.hpp"
#include "network_diagnostics.hpp"
#include "amity/consortium/hostname.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <deque>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <sys/socket.h>
#include <tuple>
namespace amity::network_diagnostics {Snapshot snapshot(){return {};}}
namespace amity::consortium {
using namespace platform;
uint64_t clock_ms=100;
uint64_t now_ms(){return clock_ms;}
template<class T>T ident(unsigned value){char text[33];snprintf(text,sizeof(text),"%032x",value);return *T::parse(text);}
template<class T>T random_id(){static unsigned next=1000;return ident<T>(next++);}
// Cryptography has independent coverage; this lower-layer fixture gives the
// real SnapshotTransfer a deterministic digest and still checks its binding.
std::string sha256(std::string_view){return std::string(64,'a');}
struct Queued{Kind kind;std::string payload;uint32_t ttl=0;uint64_t created=0,revision=0;std::shared_ptr<const model::OwnedIntent> owned;};
std::vector<std::string> diagnostic_logs,key_transfer_logs;
template<class... Args>void capture_log(const char* level,const char* tag,const char* format,Args... args){
 const int length=snprintf(nullptr,0,format,args...);assert(length>=0);std::vector<char> text(static_cast<size_t>(length)+1);snprintf(text.data(),text.size(),format,args...);assert(length<384);if(std::string(format).find("Link scope")==0)return;if(std::string(text.data()).find("Key release")==0||std::string(text.data()).find("Key transfer")==0){key_transfer_logs.emplace_back(text.data());return;}diagnostic_logs.push_back(std::string(level)+":"+tag+" "+text.data());
}
template<class... Args>void ESP_LOGW(const char* tag,const char* format,Args... args){capture_log("W",tag,format,args...);}
template<class... Args>void ESP_LOGI(const char* tag,const char* format,Args... args){capture_log("I",tag,format,args...);}
struct FakeTls{
 bool last_bulk=false,bulk_receiving=false;bool received_bulk()const{return last_bulk;}bool receiving_bulk()const{return bulk_receiving;}
 int socket=-1;model::OctopusId authenticated;std::deque<std::string> incoming;std::vector<Envelope> sent;unsigned receives=0;
 bool healthy=true;bool usable()const{return healthy;}TlsReceiveDiagnostic receive_diagnostic()const{return {};}
 std::function<void()> before_receive;
 bool receive(std::string& value,uint32_t timeout=10000){++receives;if(before_receive)before_receive();if(incoming.empty()){clock_ms+=timeout;return false;}value=std::move(incoming.front());incoming.pop_front();Envelope envelope;last_bulk=parse_envelope(value,envelope)==Error::none&&is_update_kind(envelope.kind);return true;}
};
struct SynchronizationRound {model::MessageId id;uint64_t revision=0;};
struct Active{
 Session session;std::shared_ptr<FakeTls> tls=std::make_shared<FakeTls>();model::OctopusId peer;
 std::mutex mutex;bool cancelled=false,retiring=false,lifecycle_only=false,management_ready=false,ready=true,context_ready=true,context_requested=false,refreshing=false;
 RuntimePeerStatus status;std::deque<Queued> queue;std::string endpoint_hint,expected_hostname,advertised_hostname;
 std::atomic_bool execution_fenced{false},restart_requested{false};model::LinkId cleaned_link;
 SynchronizationRound synchronization;std::optional<SynchronizationRound> pending_synchronization;bool execution_supported=true;model::MessageId update_begin_id;
 LogSnapshot log_snapshot;model::MessageId log_transfer;size_t log_next=0;uint64_t log_expires=0;
};
// ACK observation has a separate native test; this fixture tests IPC policy.
void begin_release_ack(Active&,const model::OwnedIntent&,uint64_t,const network_diagnostics::Snapshot&){}
void emit_key_network(const model::OctopusId&,uint8_t,uint64_t,uint8_t,bool,const char*,const network_diagnostics::Snapshot&,const network_diagnostics::Snapshot&){}
std::mutex lock;RuntimeControlHooks controls;RuntimeStatus status;Group group;Identity identity;
RuntimeNetworkHooks network;RuntimeUpdateHooks updates;
std::vector<std::tuple<model::OctopusId,model::BootId,model::LinkId>> restart_fences;
uint64_t configuration=7;std::string configuration_hash=std::string(64,'a');
std::vector<std::shared_ptr<Active>> links;
unsigned shutdowns=0;
esp_err_t save_group(const Group&){return ESP_OK;}
esp_err_t runtime_send_update(model::OctopusId,Kind,std::string,uint32_t){assert(false);return ESP_ERR_NOT_SUPPORTED;}
std::function<void(Active&,const Envelope&)> on_send;
bool send(Active&,Kind,std::string_view,uint32_t ttl=1000);
bool current_peer_credential(const Active&,const Group&){return true;}
Error remember_hostname(model::OctopusId,const std::string&){return Error::none;}
unsigned pdMS_TO_TICKS(unsigned ms){return ms/10;}
void vTaskDelay(unsigned ticks){clock_ms+=ticks*10;}
}
int capture_shutdown(int,int){++amity::consortium::shutdowns;return 0;}
#define shutdown capture_shutdown
'''
PREFIX += 'namespace amity::consortium {\n' + function(
    (ROOT/'components/consortium/logs_runtime.inc').read_text(),
    'void log_snapshot_clear(') + '\n}\n'
SUPPORT = r'''
namespace amity::consortium {
std::string packet(const Envelope& e){
 return "{\"protocol_version\":1,\"consortium_id\":\""+e.consortium_id.str()+"\",\"origin_octopus_id\":\""+e.origin_octopus_id.str()+"\",\"origin_boot_id\":\""+e.origin_boot_id.str()+"\",\"destination_octopus_id\":\""+e.destination_octopus_id.str()+"\",\"destination_boot_id\":\""+e.destination_boot_id.str()+"\",\"coordinator_epoch\":\""+std::to_string(e.coordinator_epoch)+"\",\"config_revision\":\""+std::to_string(e.config_revision)+"\",\"kind\":\""+kind_name(e.kind)+"\",\"ttl_ms\":"+std::to_string(e.ttl_ms)+",\"payload\":"+settings_json(e.payload)+"}";
}
bool send(Active& active,Kind kind,std::string_view payload,uint32_t ttl){
 auto e=outgoing(active,kind,ttl);JsonError error;assert(parse_json(payload,e.payload,error)&&valid_envelope_payload(kind,e.payload));
 active.tls->sent.push_back(e);if(on_send)on_send(active,e);return true;
}
Envelope inbound(const LinkScope& scope,uint64_t revision,Kind kind,std::string_view payload){
 Envelope e;e.consortium_id=scope.consortium_id;e.origin_octopus_id=scope.peer_id;e.destination_octopus_id=scope.local_id;
 e.origin_boot_id=scope.peer_boot_id;e.destination_boot_id=scope.local_boot_id;e.coordinator_epoch=scope.epoch;e.config_revision=revision;e.kind=kind;e.ttl_ms=1000;
 JsonError error;assert(parse_json(payload,e.payload,error)&&valid_envelope_payload(kind,e.payload));return e;
}
void feed(Active& active,const Envelope& e){active.tls->incoming.push_back(packet(e));}
SnapshotManifest manifest(model::OctopusId owner,model::BootId boot,uint64_t revision,std::string_view kind,std::string_view payload){
 SnapshotManifest m;m.transfer_id=random_id<model::MessageId>();m.owner_octopus_id=owner;m.owner_boot_id=boot;m.config_revision=revision;m.total_length=payload.size();m.payload_hash=sha256(payload);m.document_kind=kind;return m;
}
std::shared_ptr<Active> start(bool server,uint32_t caps=known_capabilities){
 auto a=std::make_shared<Active>();
 const auto coordinator=ident<model::OctopusId>(2),member=ident<model::OctopusId>(3);const auto mainboot=ident<model::BootId>(4),memberboot=ident<model::BootId>(5);
 LinkScope scope{ident<model::LinkId>(6),ident<model::ConsortiumId>(1),server?coordinator:member,server?member:coordinator,coordinator,server?mainboot:memberboot,server?memberboot:mainboot,1,7};
 a->peer=scope.peer_id;a->tls->authenticated=a->peer;a->status.id=a->peer;a->status.boot_id=scope.peer_boot_id;a->status.config_revision=7;a->status.ready=true;a->status.negotiated_capabilities=caps;
 HelloFrame h;h.consortium_id=scope.consortium_id;h.octopus_id=scope.peer_id;h.boot_id=scope.peer_boot_id;h.coordinator_epoch=1;h.config_revision=7;h.required_capabilities=caps;
 assert(a->session.authenticate(scope,scope.peer_id,server?PeerRole::actuator:PeerRole::coordinator,true)==Error::none&&a->session.hello(h,known_capabilities)==Error::none&&a->session.synchronized(7,true,true)==Error::none);
 clock_ms=100;controls={};updates={};restart_fences.clear();controls.restart_session=[](auto owner,auto peer_boot,auto old_link){restart_fences.emplace_back(owner,peer_boot,old_link);return true;};network={};network.get_hostname=[server]{return server?"amity-main":"amity-member";};configuration=7;configuration_hash=std::string(64,'a');status={};status.authority_ready=server;identity.id=scope.local_id;group={};group.consortium=scope.consortium_id;group.role=server?Group::coordinator:Group::member;group.authority=coordinator;group.epoch=1;links={a};on_send={};shutdowns=0;diagnostic_logs.clear();key_transfer_logs.clear();
 return a;
}
std::string controller_slice(const LinkScope& scope,uint64_t revision){return "{\"controller\":\""+scope.coordinator_id.str()+"\",\"config_revision\":\""+std::to_string(revision)+"\"}";}
const std::string owner_state="{\"adapters\":[]}";
const std::string device_status="{\"hostname\":\"amity-member\"}";
std::string observation(const LinkScope& scope){model::OwnedAdapterSnapshot report;report.owner=scope.peer_id;report.boot=scope.peer_boot_id;report.ready=true;return "{\"snapshot\":"+owned_snapshot_json(report)+"}";}
void unchanged(const Active& active,const LinkScope& original,const FakeTls* tls){
 auto expected=original;expected.config_revision=active.session.scope().config_revision;
 assert(active.tls.get()==tls&&active.session.scope()==expected&&active.ready&&active.status.ready&&!active.cancelled&&!active.retiring&&active.session.state()==ConnectionState::ready);
}
}
'''
SUITE = r'''
namespace amity::consortium {

void terminal_receive_trace(){
 auto a=start(true);LinkTrace trace;trace.phase="owner_state_receive";trace.fail("synchronization_receive");
 trace.receive={TlsReceiveDiagnostic::Cause::progress_timeout,-123,4,8196,2169,2169,2100,1400};
 {DeferredLinkTrace deferred;
  {std::lock_guard<std::mutex> guard(lock);deferred.capture(*a,trace,clock_ms);trace.receive={};assert(diagnostic_logs.empty());}
  assert(diagnostic_logs.empty());
 }
 assert(diagnostic_logs.size()==2&&diagnostic_logs[1].find("cause=frame_progress_timeout code=-123 bytes=4 expected=8196 progress_ms=2169 total_ms=2169 poll_gap_ms=2100 read_ms=1400")!=std::string::npos);
 diagnostic_logs.clear();trace.receive={TlsReceiveDiagnostic::Cause::wrapper_timeout,INT32_MIN,UINT32_MAX,UINT32_MAX,UINT64_MAX,UINT64_MAX,UINT64_MAX,UINT64_MAX};
 trace_link_closed(*a,trace,clock_ms);assert(diagnostic_logs.size()==2&&diagnostic_logs[1].size()<384);
}
void coordinator_refresh(){
 auto a=start(true);const auto original=a->session.scope();const auto* tls=a->tls.get();std::vector<std::string> cached={"TV","AVR","Player"};
 a->advertised_hostname="renamed-main";network.get_hostname=[]{return "renamed-main";};
 unsigned applied=0;std::optional<SnapshotManifest> config_manifest;std::optional<Envelope> stale,result;
 controls.snapshot=[](auto){return owner_state;};controls.configuration_snapshot=[&](auto owner,uint64_t revision){assert(owner==a->peer&&revision==8);return controller_slice(original,revision);};
 controls.synchronize=[&](auto owner,const Json& value){++applied;assert(owner==a->peer&&value.type==Json::Type::string&&value.string==owner_state&&cached.size()==3);return Error::none;};
 controls.control=[](auto,const auto&,auto,auto){assert(false&&"Observation refresh must not deliver a CEC completion");return Error::none;};
 a->tls->before_receive=[&]{unchanged(*a,original,tls);assert(cached.size()==3);};
 on_send=[&](Active& active,const Envelope& e){
  unchanged(active,original,tls);SnapshotManifest m;std::string phase;uint32_t next=0;
  if(e.kind==Kind::snapshot_request){assert(parse_snapshot_manifest(e.payload,m,phase,next)==Error::none);
   if(m.document_kind=="configuration"){assert(phase=="begin"&&e.config_revision==7&&m.config_revision==8);config_manifest=m;
    feed(active,inbound(original,7,Kind::device_status,"{\"hostname\":\"renamed-member\"}")); // Admitted metadata interleaving the transfer.
    result=inbound(original,7,Kind::scoped_result,observation(original));feed(active,*result);
    feed(active,inbound(original,7,Kind::snapshot_request,snapshot_manifest_json(m,"ack",0)));}
   else assert(e.config_revision==8&&m.config_revision==8&&(phase=="ack"||phase=="complete"));
  }else{assert(config_manifest&&e.kind==Kind::snapshot_chunk&&e.config_revision==7);
   feed(active,inbound(original,7,Kind::snapshot_request,snapshot_manifest_json(*config_manifest,"complete",1)));
   // Already-in-flight obsolete messages are safely discarded after advance,
   // they cannot masquerade as the new owner-state slice.
   feed(active,inbound(original,7,Kind::device_status,"{\"hostname\":\"renamed-member\"}")); // Old-scope metadata after local advance.
   stale=inbound(original,7,Kind::device_status,device_status);feed(active,*stale);
   const auto state=manifest(original.peer_id,original.peer_boot_id,8,"owner_state",owner_state);
   feed(active,inbound(original,8,Kind::snapshot_request,snapshot_manifest_json(state,"begin",0)));
   feed(active,inbound(original,8,Kind::snapshot_chunk,snapshot_chunk_json(state,0,owner_state)));
  }
 };
 runtime_configuration_changed(8,std::string(64,'b'));
 assert(configuration==8&&a->session.scope()==original&&a->ready&&a->status.ready&&shutdowns==0&&applied==0&&cached.size()==3);
 assert(synchronize(*a,true,true,8));unchanged(*a,original,tls);assert(applied==1&&cached.size()==3&&a->session.scope().config_revision==8&&a->status.config_revision==8);
 assert(!a->context_ready);
 assert(stale&&a->session.admit(*stale,clock_ms,clock_ms)==Error::stale_config&&a->session.discard_stale(*stale,clock_ms,clock_ms)==Error::none);
 assert(result&&a->session.admit(*result,clock_ms,clock_ms)==Error::stale_config&&a->session.discard_stale(*result,clock_ms,clock_ms)==Error::none);
 assert(a->tls->sent.size()==4&&a->tls->sent[0].config_revision==7&&a->tls->sent[1].config_revision==7&&a->tls->sent[2].config_revision==8&&a->tls->sent[3].config_revision==8);
 runtime_configuration_changed(6,std::string(64,'c'));assert(configuration==8&&shutdowns==0);
 assert(a->advertised_hostname.empty());on_send={};assert(refresh_local_hostname(*a));
 assert(a->tls->sent.size()==5&&a->tls->sent.back().kind==Kind::device_status&&a->tls->sent.back().config_revision==8&&a->tls->sent.back().payload.get("hostname")->string=="renamed-main");
 assert(refresh_local_hostname(*a)&&a->tls->sent.size()==5);unchanged(*a,original,tls);
}
void future_context_recaptures_after_slow_refresh(){
 auto a=start(true);const auto original=a->session.scope();unsigned applied=0;std::optional<SnapshotManifest> config_manifest;
 controls.snapshot=[](auto){return owner_state;};controls.configuration_snapshot=[&](auto,uint64_t rev){return controller_slice(original,rev);};controls.synchronize=[&](auto,const Json&){++applied;return Error::none;};
 on_send=[&](Active& active,const Envelope& e){SnapshotManifest m;std::string phase;uint32_t next=0;
  if(e.kind==Kind::snapshot_request){assert(parse_snapshot_manifest(e.payload,m,phase,next)==Error::none);if(m.document_kind=="configuration"){config_manifest=m;feed(active,inbound(original,7,Kind::snapshot_request,snapshot_manifest_json(m,"ack",0)));}}
  else if(e.kind==Kind::snapshot_chunk){assert(config_manifest);feed(active,inbound(original,7,Kind::snapshot_request,snapshot_manifest_json(*config_manifest,"complete",1)));
   const auto state=manifest(original.peer_id,original.peer_boot_id,8,"owner_state",owner_state);feed(active,inbound(original,8,Kind::snapshot_request,snapshot_manifest_json(state,"begin",0)));feed(active,inbound(original,8,Kind::snapshot_chunk,snapshot_chunk_json(state,0,owner_state)));}
 };
 unsigned receives=0;a->tls->before_receive=[&]{if(++receives==3)clock_ms+=1150;};runtime_configuration_changed(8,std::string(64,'b'));
 model::OwnedIntent context;context.kind=model::OwnedIntentKind::context;context.target.port={original.consortium_id,original.peer_id,0,model::Side::front};context.config_revision=8;context.generation=1;
 assert(runtime_send_owned(context)==ESP_OK&&a->queue.empty());
 auto positive=context;positive.kind=model::OwnedIntentKind::key_edges;positive.edges[0]={4,true};positive.edge_count=1;
 assert(runtime_send_owned(positive)==ESP_ERR_INVALID_STATE&&a->queue.empty());
 assert(refresh_turn(*a)&&clock_ms==1250&&applied==1&&a->session.scope().config_revision==8&&!a->context_ready&&!a->cancelled&&a->queue.empty());
 assert(pump(*a)&&diagnostic_logs.empty()); // No old context payload or deadline.
 unsigned requested=0;controls.ready=[&](auto,auto,auto){++requested;return true;};assert(initialize_context(*a)&&initialize_context(*a)&&requested==1&&a->context_requested&&!a->context_ready);
 assert(runtime_send_owned(positive)==ESP_ERR_INVALID_STATE&&a->queue.empty());
 // The asynchronous callback reads current state when it actually runs. Its
 // fresh current generation enters before any later admitted action.
 context.generation=3;context.selected=true;assert(runtime_send_owned(context)==ESP_OK&&a->context_ready&&a->queue.size()==1&&a->queue.front().created==1250);
 positive.generation=3;assert(runtime_send_owned(positive)==ESP_OK&&a->queue.size()==2);
 const auto before=a->tls->sent.size();assert(pump(*a)&&pump(*a)&&a->tls->sent.size()==before+2);
 model::OwnedIntent delivered;
 assert(parse_owned_intent(a->tls->sent[before].payload,delivered)==Error::none&&delivered.kind==model::OwnedIntentKind::context&&delivered.generation==3&&!a->tls->sent[before].payload.get("execution_lease_id")&&!a->tls->sent[before].payload.get("sequence"));
 assert(parse_owned_intent(a->tls->sent[before+1].payload,delivered)==Error::none&&delivered.kind==model::OwnedIntentKind::key_edges&&!a->tls->sent[before+1].payload.get("sequence")&&a->tls->sent[before+1].config_revision==8);
 unchanged(*a,original,a->tls.get());
}
void rejected_interleaved_observations(){
 for(unsigned failure=0;failure<7;++failure){
  auto a=start(true);const auto original=a->session.scope();unsigned applied=0;
  controls.snapshot=[](auto){return owner_state;};controls.configuration_snapshot=[&](auto,uint64_t revision){return controller_slice(original,revision);};
  controls.synchronize=[&](auto,const Json&){++applied;return Error::none;};
  on_send=[&](Active& active,const Envelope& e){
   assert(e.kind==Kind::snapshot_request);SnapshotManifest m;std::string phase;uint32_t next=0;assert(parse_snapshot_manifest(e.payload,m,phase,next)==Error::none&&phase=="begin");
   auto result=inbound(original,7,Kind::scoped_result,observation(original));
   switch(failure){
    case 0:result.origin_boot_id=ident<model::BootId>(99);break;
    case 1:result.destination_boot_id=ident<model::BootId>(99);break;
    case 2:result.origin_octopus_id=ident<model::OctopusId>(99);break;
    case 3:result.destination_octopus_id=ident<model::OctopusId>(99);break;
    case 4:result.consortium_id=ident<model::ConsortiumId>(99);break;
    case 5:result.coordinator_epoch=2;break;
    case 6:result.config_revision=8;break;
   }
   feed(active,result);feed(active,inbound(original,7,Kind::snapshot_request,snapshot_manifest_json(m,"ack",0)));
  };
  LinkTrace trace;assert(!synchronize(*a,true,true,8,nullptr,&trace));assert(applied==0&&a->session.scope()==original);
  assert(trace.unexpected&&trace.has_kind&&trace.kind==Kind::scoped_result&&!trace.has_queue);
  assert(std::string(trace.phase)=="configuration_send"&&std::string(trace.reason)=="synchronization_admission");
 }
}
void member_refresh(){
 auto a=start(false);const auto original=a->session.scope();const auto* tls=a->tls.get();unsigned applied=0;std::vector<std::string> cached={"Member TV","Member Player"};
 a->advertised_hostname="renamed-member";network.get_hostname=[]{return "renamed-member";};
 const auto payload=controller_slice(original,8);const auto incoming=manifest(original.local_id,original.local_boot_id,8,"configuration",payload);
 const auto first=inbound(original,7,Kind::snapshot_request,snapshot_manifest_json(incoming,"begin",0));
 assert(a->session.admit(first,clock_ms,clock_ms)==Error::none); // The real outer loop pre-admits the first manifest.
 feed(*a,inbound(original,7,Kind::snapshot_chunk,snapshot_chunk_json(incoming,0,payload)));
 controls.snapshot=[&](auto){assert(cached.size()==2);return owner_state;};controls.synchronize=[&](auto owner,const Json& value){assert(owner==a->peer&&value.string==payload&&a->session.scope()==original&&cached.size()==2);++applied;return Error::none;};
 a->tls->before_receive=[&]{unchanged(*a,original,tls);assert(cached.size()==2);};
 std::optional<SnapshotManifest> outgoing_state;
 on_send=[&](Active& active,const Envelope& e){
  unchanged(active,original,tls);SnapshotManifest m;std::string phase;uint32_t next=0;
  if(e.kind==Kind::snapshot_request){assert(parse_snapshot_manifest(e.payload,m,phase,next)==Error::none);
   if(m.document_kind=="configuration"){assert(e.config_revision==7&&m.config_revision==8&&(phase=="ack"||phase=="complete"));if(phase=="complete")assert(applied==1&&active.session.scope()==original);}
   else{assert(e.config_revision==8&&phase=="begin");outgoing_state=m;feed(active,inbound(original,8,Kind::snapshot_request,snapshot_manifest_json(m,"ack",0)));}
  }else{assert(outgoing_state&&e.kind==Kind::snapshot_chunk&&e.config_revision==8);feed(active,inbound(original,8,Kind::snapshot_request,snapshot_manifest_json(*outgoing_state,"complete",1)));}
 };
 assert(synchronize(*a,false,true,8,&first));unchanged(*a,original,tls);assert(configuration==8&&applied==1&&cached.size()==2&&shutdowns==0);
 assert(a->advertised_hostname.empty());
 assert(a->tls->sent.size()==4&&a->tls->sent[0].config_revision==7&&a->tls->sent[1].config_revision==7&&a->tls->sent[2].config_revision==8&&a->tls->sent[3].config_revision==8);
 on_send={};assert(refresh_local_hostname(*a));assert(a->tls->sent.size()==5&&a->tls->sent.back().kind==Kind::device_status&&a->tls->sent.back().config_revision==8&&a->tls->sent.back().payload.get("hostname")->string=="renamed-member");unchanged(*a,original,tls);
 assert(!a->context_ready);
}
void rejected_frames(){
 for(unsigned failure=0;failure<5;++failure){auto a=start(false);const auto original=a->session.scope();unsigned applied=0;
  controls.snapshot=[](auto){return owner_state;};controls.synchronize=[&](auto,const Json&){++applied;return Error::none;};
  const auto value=controller_slice(original,8);auto m=manifest(original.local_id,original.local_boot_id,8,"configuration",value);auto e=inbound(original,7,Kind::snapshot_request,snapshot_manifest_json(m,"begin",0));
  switch(failure){case 0:e.origin_boot_id=ident<model::BootId>(99);break;case 1:e.origin_octopus_id=ident<model::OctopusId>(99);break;case 2:e.config_revision=9;break;case 3:e.consortium_id=ident<model::ConsortiumId>(99);break;case 4:m.owner_octopus_id=original.peer_id;e=inbound(original,7,Kind::snapshot_request,snapshot_manifest_json(m,"begin",0));break;}
  feed(*a,e);assert(!synchronize(*a,false,true,8)&&applied==0&&a->session.scope()==original);
 }
 auto old=start(false,known_capabilities&~live_configuration);controls.snapshot=[](auto){return owner_state;};controls.synchronize=[](auto,const Json&){return Error::none;};
 assert(!synchronize(*old,false,true,8)&&old->tls->sent.empty()&&old->session.scope().config_revision==7);
}
void immutable_queue(){
 auto a=start(true);on_send={};const auto original=a->session.scope();
 model::OwnedIntent i;i.kind=model::OwnedIntentKind::key_edges;i.target.port={original.consortium_id,original.peer_id,0,model::Side::front};i.generation=1;i.edges[0]={4,true};i.edge_count=1;i.config_revision=8;
 auto owned=std::make_shared<const model::OwnedIntent>(i);Queued queued{Kind::scoped_intent,{},500,100,8,owned};a->queue.push_back(queued);
 runtime_configuration_changed(8,std::string(64,'b'));assert(a->queue.size()==1&&a->queue.front().owned==owned&&a->queue.front().created==100&&a->queue.front().ttl==500&&a->session.scope()==original&&shutdowns==0);
 std::string payload;assert(prepare_queued(*a,queued,payload)==QueuedState::stale&&payload.empty());
 assert(a->session.advance_configuration(8)==Error::none);a->status.config_revision=8;a->context_ready=false;
 assert(prepare_queued(*a,queued,payload)==QueuedState::waiting);
 a->context_ready=true;assert(prepare_queued(*a,queued,payload)==QueuedState::ready);
 Json parsed;JsonError error;model::OwnedIntent decoded;assert(parse_json(payload,parsed,error)&&parse_owned_intent(parsed,decoded)==Error::none&&!parsed.get("execution_lease_id"));
 assert(queued.owned==owned&&queued.revision==8&&queued.created==100&&queued.ttl==500&&owned->config_revision==8);
 auto stale=queued;stale.revision=7;assert(prepare_queued(*a,stale,payload)==QueuedState::stale);
 // The actual worker block expires the admitted IPC item at its original
 // deadline, even if current context was unavailable during refresh.
 clock_ms=600;a->context_ready=false;assert(!pump(*a)&&a->tls->sent.empty()&&a->queue.empty());
 assert(diagnostic_logs.size()==1&&diagnostic_logs.front().find("reason=causal_expired phase=operational")!=std::string::npos&&diagnostic_logs.front().find("kind=scoped_intent age_ms=500 ttl_ms=500")!=std::string::npos);
 auto settings=Queued{Kind::settings_read,"{\"request_id\":\""+ident<model::MessageId>(91).str()+"\"}",1000,600,7,{}};
 a->queue.push_back(settings);clock_ms=601;assert(pump(*a)&&a->queue.empty()&&a->tls->sent.empty()); // no old management-payload restamp
 a->context_ready=true;clock_ms=650;queued.created=650;a->queue.push_back(queued);assert(pump(*a)&&a->tls->sent.size()==1&&a->tls->sent.back().config_revision==8);
}
void observation_expiry_is_not_lost_causal_work(){
 auto a=start(false);on_send={};const auto scope=a->session.scope();
 std::vector<RuntimeSnapshotRetry> retries;controls.snapshot_retry=[&](const auto& request){retries.push_back(request);};
 model::OwnedAdapterSnapshot report;report.owner=scope.local_id;report.boot=scope.local_boot_id;report.adapter=0;report.ready=true;
 const auto snapshot="{\"snapshot\":"+owned_snapshot_json(report)+"}";
 auto key=std::make_shared<model::OwnedIntent>();key->kind=model::OwnedIntentKind::key_edges;key->generation=1;key->config_revision=scope.config_revision;key->edges[0]={4,true};key->edge_count=1;
 key->target={{scope.consortium_id,scope.peer_id,0,model::Side::back},"Player"};
 const auto observation=Queued{Kind::scoped_result,snapshot,1000,100,scope.config_revision,{}};
 const auto input=Queued{Kind::scoped_intent,{},1000,100,scope.config_revision,key};
 assert(!queued_causal(observation)&&queued_causal(input));
 auto malformed=observation;malformed.payload="{\"snapshot\":{}}";assert(queued_causal(malformed));
 malformed.payload="{}";assert(queued_causal(malformed));malformed.payload="invalid";assert(queued_causal(malformed));
 a->queue.push_back(observation);clock_ms=1200;assert(pump(*a)&&a->queue.empty()&&a->tls->sent.empty()&&!a->cancelled&&diagnostic_logs.empty());
 assert(retries.size()==1&&retries.back().controller==scope.peer_id&&retries.back().owner_boot==scope.local_boot_id&&retries.back().controller_boot==scope.peer_boot_id&&retries.back().link==scope.link_id&&retries.back().config_revision==scope.config_revision&&retries.back().adapter==0);
 a->queue.push_back(observation);assert(queue_control(a->peer,Kind::scoped_intent,{},1000,{},scope.config_revision,key)==ESP_OK&&!a->cancelled&&shutdowns==0&&a->queue.size()==1);
 assert(retries.size()==2&&diagnostic_logs.empty()&&a->queue.front().owned==key);
 a->queue.clear();a->queue.push_back(input);
 assert(queue_control(a->peer,Kind::scoped_result,snapshot,1000,{},scope.config_revision)==ESP_ERR_INVALID_STATE);
 assert(a->restart_requested&&a->execution_fenced&&!a->ready&&!a->context_ready&&!a->cancelled&&a->tls->usable()&&shutdowns==0&&a->queue.empty());
 assert(diagnostic_logs.size()==1&&diagnostic_logs.front().find("reason=causal_expired phase=queue_admission")!=std::string::npos&&diagnostic_logs.front().find("kind=scoped_intent age_ms=1100 ttl_ms=1000")!=std::string::npos);
 a=start(false);a->queue.push_back(input);clock_ms=1200;assert(!pump(*a)&&a->queue.empty()&&a->tls->sent.empty());
 assert(diagnostic_logs.size()==1&&diagnostic_logs.front().find("reason=causal_expired phase=operational")!=std::string::npos);
}
}
int main(){using namespace amity::consortium;terminal_receive_trace();coordinator_refresh();member_refresh();future_context_recaptures_after_slow_refresh();rejected_frames();rejected_interleaved_observations();immutable_queue();observation_expiry_is_not_lost_causal_work();std::cout<<"PASS actual in-place runtime synchronization: both roles retain TLS/link/boots/readiness/cache; strict identity/boot/configuration and observation fences; old-scope observation discarded before new owner state; fresh context and original queued revision/deadline; snapshot expiry independent of causal input state\n";}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT / 'components/consortium/runtime.cpp')
    source = parser.parse_args().source.read_text()
    # Run the same queue block as the link task, including break-on-expired-
    # causal-work and send-time TTL reduction; no copy of its decisions.
    start = source.index('\n', source.index('        if(!active->retiring&&!refresh_local_hostname(*active))')) + 1
    end = source.index('        fd_set reads;', start)
    pump = '\nbool pump(Active& storage){Active* active=&storage;LinkTrace trace;trace.phase="operational";while(true){' + source[start:end] + 'return true;}trace_link_closed(storage,trace,now_ms());return false;}\n'
    actual = ''.join(function(source, name) for name in (
        'Envelope outgoing(', 'bool same_manifest(', 'bool synchronize(', 'bool exchange_hostname(Active& active,const std::string& expected,LinkTrace* diagnostic=nullptr) {',
        'bool synchronization_current(', 'bool synchronization_recoverable(', 'bool synchronization_barrier(', 'bool fence_synchronization_scope(', 'bool restart_synchronization_scope(', 'bool synchronize_connection(', 'bool refresh_local_hostname(',
        'void runtime_configuration_changed(', 'static bool queued_firmware_stream(', 'static bool queued_expired(', 'static bool pure_key_release(', 'static bool queued_cleanup(', 'static bool queued_causal('))
    actual = function(source, 'void request_session_restart(') + source[source.index('struct LinkTraceSnapshot {'):source.index('std::optional<RuntimeSnapshotRetry> snapshot_retry(')] + actual
    actual += source[source.index('std::optional<RuntimeSnapshotRetry> snapshot_retry('):source.index('enum class QueuedState')]
    actual += function(source, 'static esp_err_t queue_control(')
    actual += ''.join(function(source, name) for name in ('bool initialize_context(', 'void runtime_fence_owned_link(', 'esp_err_t runtime_send_owned('))
    refresh_start=source.index('        if(server&&active->ready&&')
    refresh_end=source.index('        trace.phase="operational";',refresh_start)
    refresh='\nbool refresh_turn(Active& storage){auto* active=&storage;const bool server=true;const uint64_t receive_until=0;LinkTrace trace;while(true){'+source[refresh_start:refresh_end]+'return true;}trace_link_closed(storage,trace,now_ms());return false;}\n'
    queued = function(source, 'enum class QueuedState').rstrip() + ';\n' + function(source, 'QueuedState prepare_queued(')
    with tempfile.TemporaryDirectory(prefix='amity-live-configuration-') as directory:
        temp = Path(directory)
        (temp / 'esp_err.h').write_text('#pragma once\nusing esp_err_t=int;constexpr int ESP_OK=0,ESP_ERR_INVALID_ARG=1,ESP_ERR_INVALID_STATE=2,ESP_ERR_NO_MEM=3,ESP_ERR_TIMEOUT=4,ESP_FAIL=5,ESP_ERR_NOT_FOUND=6,ESP_ERR_NOT_SUPPORTED=7;\n')
        cpp, binary = temp / 'test.cpp', temp / 'test'
        trace = function(source, 'struct LinkTrace {').rstrip() + ';\n'
        prefix = PREFIX.replace('struct Active{', trace + 'struct Active{')
        cpp.write_text(timing_support() + prefix + 'namespace amity::consortium {\n' + actual + queued + pump + refresh + '\n}\n' + SUPPORT + SUITE)
        sources = [ROOT / 'components/consortium' / name for name in
                   ('json.cpp', 'wire.cpp', 'owned_intent.cpp', 'session.cpp', 'settings.cpp', 'update.cpp', 'transfer.cpp')]
        sources.append(ROOT / 'components/amity_system/model.cpp')
        subprocess.run(shlex.split(os.environ.get('CXX', 'c++')) +
                       ['-std=c++17', '-Wall', '-Wextra', '-Werror', '-pthread', '-g', '-fsanitize=address,undefined',
                        '-I' + str(temp), '-I' + str(ROOT / 'components/consortium'),
                         '-I' + str(ROOT / 'components/consortium/include'), '-I' + str(ROOT / 'components/amity_system/include'),
                         '-I' + str(ROOT / 'components/network_diagnostics/include')] +
                       list(map(str, [cpp] + sources)) + ['-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
