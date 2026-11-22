#!/usr/bin/env python3
"""Run the actual in-place synchronizer against scripted authenticated TLS I/O."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile
from consortium_gatt_callback_test import function
from setup_timing_support import timing_support

ROOT = Path(__file__).resolve().parents[1]
PREFIX = r'''
#include "amity/consortium/runtime.hpp"
#include "runtime_persistence.hpp"
#include "tls_receive_trace.hpp"
#include "amity/consortium/owned_intent.hpp"
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
#include <cstring>
#include <deque>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <sys/socket.h>
#include <tuple>
#include <unistd.h>
namespace amity::network_diagnostics {Snapshot snapshot(){return {};}}
namespace amity::consortium {
using namespace platform;
uint64_t clock_ms=100;
uint64_t now_ms(){return clock_ms;}
template<class T>T ident(unsigned value){char text[33];snprintf(text,sizeof(text),"%032x",value);return *T::parse(text);}
template<class T>T random_id(){static unsigned next=1000;return ident<T>(next++);}
struct Queued{Kind kind;std::string payload;uint32_t ttl=0;uint64_t created=0,revision=0;std::shared_ptr<const model::OwnedIntent> owned;std::shared_ptr<const model::OwnedAdapterSnapshot> snapshot{};bool firmware_stream=false;};
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
 std::mutex mutex;bool cancelled=false,retiring=false,management_ready=false,ready=true,context_ready=true,context_requested=false,refreshing=false;
 LinkScope context_retry_scope;uint64_t context_retry_at=0;
 RuntimePeerStatus status;std::deque<Queued> queue;std::string endpoint_hint,expected_hostname,advertised_hostname;
 std::atomic_bool execution_fenced{false},restart_requested{false};model::LinkId cleaned_link;
 SynchronizationRound synchronization;std::optional<SynchronizationRound> pending_synchronization;bool execution_supported=true;model::MessageId update_begin_id;
 LogSnapshot log_snapshot;model::MessageId log_transfer;size_t log_next=0;uint64_t log_expires=0;
 uint32_t advertised_generation=0;int wakeup=-1;struct{bool pending=false;}release_ack;
};
std::atomic<uint32_t> hostname_generation{1};
unsigned wakes=0;void wake(Active&){++wakes;}
// ACK observation has a separate native test; this fixture tests IPC policy.
void begin_release_ack(Active&,const model::OwnedIntent&,uint64_t,const network_diagnostics::Snapshot&){}
void emit_key_network(const model::OctopusId&,uint8_t,uint64_t,uint8_t,bool,const char*,const network_diagnostics::Snapshot&,const network_diagnostics::Snapshot&){}
std::mutex lock,persistence_lock;RuntimeControlHooks controls;RuntimeStatus status;Group group;Identity identity;
RuntimeNetworkHooks network;RuntimeUpdateHooks updates;
std::vector<std::tuple<model::OctopusId,model::BootId,model::LinkId>> restart_fences;
uint64_t configuration=7;
std::vector<std::shared_ptr<Active>> links;
unsigned shutdowns=0;
esp_err_t save_group(const Group&){return ESP_OK;}
esp_err_t runtime_send_update(model::OctopusId,Kind,std::string,uint32_t){assert(false);return ESP_ERR_NOT_SUPPORTED;}
std::function<void(Active&,const Envelope&)> on_send;
bool send(Active&,Kind,std::string_view,uint32_t ttl=1000);
bool current_peer_credential(const Active&,const Group&){return true;}
Error remember_hostname(model::OctopusId,const std::string&,std::unique_lock<std::mutex>&){return Error::none;}
unsigned pdMS_TO_TICKS(unsigned ms){return ms/10;}
void vTaskDelay(unsigned ticks){clock_ms+=ticks*10;}
}
int capture_shutdown(int,int){++amity::consortium::shutdowns;return 0;}
#define shutdown capture_shutdown
'''
PREFIX += 'namespace amity::consortium {\n' + function(
    (ROOT/'components/consortium/logs_runtime.inc').read_text(),
    'void log_snapshot_clear(') + '\nbool send_log_chunk(Active&){return true;}\n}\n'
SUPPORT = r'''
namespace amity::consortium {
std::string packet(const Envelope& e){
 return "{\"config_revision\":\""+std::to_string(e.config_revision)+"\",\"kind\":\""+kind_name(e.kind)+"\",\"ttl_ms\":"+std::to_string(e.ttl_ms)+",\"payload\":"+settings_json(e.payload)+"}";
}
bool send(Active& active,Kind kind,std::string_view payload,uint32_t ttl){
 auto e=outgoing(active,kind,ttl);JsonError error;
 if(kind==Kind::update_chunk){const auto frame=update_chunk_envelope(e,payload);assert(!frame.empty()&&parse_envelope(frame,e)==Error::none);}
 else assert(parse_json(payload,e.payload,error)&&valid_envelope_payload(kind,e.payload));
 active.tls->sent.push_back(e);if(on_send)on_send(active,e);return true;
}
Envelope inbound(const LinkScope&,uint64_t revision,Kind kind,std::string_view payload){
 Envelope e;
 e.config_revision=revision;e.kind=kind;e.ttl_ms=1000;
 JsonError error;assert(parse_json(payload,e.payload,error)&&valid_envelope_payload(kind,e.payload));return e;
}
void feed(Active& active,const Envelope& e){active.tls->incoming.push_back(packet(e));}
std::shared_ptr<Active> start(bool server){
 auto a=std::make_shared<Active>();
 const auto coordinator=ident<model::OctopusId>(2),member=ident<model::OctopusId>(3);const auto mainboot=ident<model::BootId>(4),memberboot=ident<model::BootId>(5);
 LinkScope scope{ident<model::LinkId>(6),ident<model::ConsortiumId>(1),server?coordinator:member,server?member:coordinator,coordinator,server?mainboot:memberboot,server?memberboot:mainboot,7};
 a->peer=scope.peer_id;a->tls->authenticated=a->peer;a->status.id=a->peer;a->status.boot_id=scope.peer_boot_id;a->status.config_revision=7;a->status.ready=true;
 HelloFrame h;h.consortium_id=scope.consortium_id;h.octopus_id=scope.peer_id;h.boot_id=scope.peer_boot_id;h.config_revision=7;
 assert(a->session.authenticate(scope,scope.peer_id,server?PeerRole::actuator:PeerRole::coordinator,true)==Error::none&&a->session.hello(h)==Error::none&&a->session.synchronized(7,true,true)==Error::none);
 clock_ms=100;controls={};updates={};restart_fences.clear();controls.restart_session=[](auto owner,auto peer_boot,auto old_link){restart_fences.emplace_back(owner,peer_boot,old_link);return true;};network={};network.get_hostname=[server]{return server?"amity-main":"amity-member";};configuration=7;status={};status.authority_ready=server;identity.id=scope.local_id;group={};group.consortium=scope.consortium_id;group.role=server?Group::coordinator:Group::member;group.authority=coordinator;links={a};on_send={};shutdowns=0;diagnostic_logs.clear();key_transfer_logs.clear();
 return a;
}
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
 controls.synchronize=[](auto,uint64_t){assert(false&&"The main binds no configuration");return Error::none;};
 controls.control=[](const auto&,const auto&,auto){assert(false&&"Observation refresh must not deliver a CEC completion");return Error::none;};
 on_send=[&](Active& active,const Envelope&){unchanged(active,original,tls);assert(cached.size()==3);};
 const auto woken=wakes;runtime_configuration_changed(8);
 assert(configuration==8&&a->session.scope()==original&&a->ready&&a->status.ready&&shutdowns==0&&cached.size()==3&&wakes==woken+1); // The link task wakes at once.
 assert(refresh_turn(*a));unchanged(*a,original,tls);assert(cached.size()==3&&a->session.scope().config_revision==8&&a->status.config_revision==8);
 assert(!a->context_ready&&a->refreshing);
 // One marker in the old scope; there is no transfer or reply to wait for.
 model::MessageId round;std::string phase;uint64_t target=0;
 assert(a->tls->sent.size()==1&&a->tls->sent[0].kind==Kind::synchronization&&a->tls->sent[0].config_revision==7);
 assert(parse_synchronization(a->tls->sent[0].payload,round,phase,target)==Error::none&&phase=="advance"&&target==8);
 // Messages already in flight from the old scope are discarded, never applied.
 const auto stale=inbound(original,7,Kind::device_status,device_status);
 assert(a->session.admit(stale,clock_ms,clock_ms)==Error::stale_config&&a->session.discard_stale(stale,clock_ms,clock_ms)==Error::none);
 const auto result=inbound(original,7,Kind::scoped_result,observation(original));
 assert(a->session.admit(result,clock_ms,clock_ms)==Error::stale_config&&a->session.discard_stale(result,clock_ms,clock_ms)==Error::none);
 runtime_configuration_changed(6);assert(configuration==8&&shutdowns==0);
 assert(a->advertised_hostname.empty());on_send={};assert(refresh_local_hostname(*a));
 assert(a->tls->sent.size()==2&&a->tls->sent.back().kind==Kind::device_status&&a->tls->sent.back().config_revision==8&&a->tls->sent.back().payload.get("hostname")->string=="renamed-main");
 assert(refresh_local_hostname(*a)&&a->tls->sent.size()==2);unchanged(*a,original,tls);
}
void future_context_recaptures_after_refresh(){
 auto a=start(true);const auto original=a->session.scope();
 runtime_configuration_changed(8);
 model::OwnedIntent context;context.kind=model::OwnedIntentKind::context;context.target.port={original.consortium_id,original.peer_id,0,model::Side::front};context.config_revision=8;context.generation=1;
 assert(runtime_send_owned(context)==ESP_OK&&a->queue.empty());
 auto positive=context;positive.kind=model::OwnedIntentKind::key_edges;positive.edges[0]={4,true};positive.edge_count=1;
 assert(runtime_send_owned(positive)==ESP_ERR_INVALID_STATE&&a->queue.empty());
 assert(refresh_turn(*a)&&a->session.scope().config_revision==8&&!a->context_ready&&!a->cancelled&&a->queue.empty());
 assert(pump(*a)&&diagnostic_logs.empty()); // No old context payload or deadline.
 unsigned requested=0;controls.ready=[&](auto,auto,auto){++requested;return true;};assert(initialize_context(*a)&&initialize_context(*a)&&requested==1&&a->context_requested&&!a->context_ready);
 assert(runtime_send_owned(positive)==ESP_ERR_INVALID_STATE&&a->queue.empty());
 // The asynchronous callback reads current state when it actually runs. Its
 // fresh current generation enters before any later admitted action.
 context.generation=3;context.selected=true;assert(runtime_send_owned(context)==ESP_OK&&a->context_ready&&a->queue.size()==1&&a->queue.front().created==clock_ms);
 positive.generation=3;assert(runtime_send_owned(positive)==ESP_OK&&a->queue.size()==2);
 const auto before=a->tls->sent.size();assert(pump(*a)&&pump(*a)&&a->tls->sent.size()==before+2);
 model::OwnedIntent delivered;
 assert(parse_owned_intent(a->tls->sent[before].payload,delivered)==Error::none&&delivered.kind==model::OwnedIntentKind::context&&delivered.generation==3&&!a->tls->sent[before].payload.get("execution_lease_id")&&!a->tls->sent[before].payload.get("sequence"));
 assert(parse_owned_intent(a->tls->sent[before+1].payload,delivered)==Error::none&&delivered.kind==model::OwnedIntentKind::key_edges&&!a->tls->sent[before+1].payload.get("sequence")&&a->tls->sent[before+1].config_revision==8);
 unchanged(*a,original,a->tls.get());
}
void member_refresh(){
 auto a=start(false);const auto original=a->session.scope();const auto* tls=a->tls.get();unsigned applied=0;std::vector<std::string> cached={"Member TV","Member Player"};
 a->advertised_hostname="renamed-member";network.get_hostname=[]{return "renamed-member";};
 const auto advance=inbound(original,7,Kind::synchronization,synchronization_json(ident<model::MessageId>(81),"advance",8));
 assert(a->session.admit(advance,clock_ms,clock_ms)==Error::none); // The real outer loop admits the coordinator's marker.
 controls.synchronize=[&](auto owner,uint64_t revision){assert(owner==a->peer&&revision==8&&a->session.scope()==original&&cached.size()==2);++applied;return Error::none;};
 LinkTrace trace;assert(bind_configuration(*a,8,trace)&&apply_configuration_advance(*a,false,8,trace));unchanged(*a,original,tls);
 assert(configuration==8&&applied==1&&cached.size()==2&&shutdowns==0&&a->session.scope().config_revision==8&&a->status.config_revision==8);
 assert(a->advertised_hostname.empty()&&a->tls->sent.empty()&&!a->context_ready);
 assert(refresh_local_hostname(*a)&&a->tls->sent.size()==1&&a->tls->sent.back().kind==Kind::device_status&&a->tls->sent.back().config_revision==8&&a->tls->sent.back().payload.get("hostname")->string=="renamed-member");unchanged(*a,original,tls);
 const auto repeated=inbound(original,8,Kind::synchronization,synchronization_json(ident<model::MessageId>(82),"advance",8));
 assert(a->session.admit(repeated,clock_ms,clock_ms)==Error::stale_config); // A marker never moves back or repeats.
 auto main=start(true);assert(main->session.admit(advance,clock_ms,clock_ms)==Error::unauthorized_role); // Only the coordinator advances.
}
void rejected_bind_keeps_scope(){
 auto a=start(false);const auto original=a->session.scope();
 controls.synchronize=[](auto,uint64_t){return Error::not_ready;};
 LinkTrace trace;assert(!bind_configuration(*a,8,trace)&&std::string(trace.reason)=="synchronization_apply"&&trace.error==Error::not_ready);
 assert(a->session.scope()==original&&configuration==7&&a->ready);
 controls.synchronize={};trace=LinkTrace{};assert(!bind_configuration(*a,8,trace)&&std::string(trace.reason)=="synchronization_hooks");
}
void immutable_queue(){
 auto a=start(true);on_send={};const auto original=a->session.scope();
 model::OwnedIntent i;i.kind=model::OwnedIntentKind::key_edges;i.target.port={original.consortium_id,original.peer_id,0,model::Side::front};i.generation=1;i.edges[0]={4,true};i.edge_count=1;i.config_revision=8;
 auto owned=std::make_shared<const model::OwnedIntent>(i);Queued queued{Kind::scoped_intent,{},500,100,8,owned};a->queue.push_back(queued);
 runtime_configuration_changed(8);assert(a->queue.size()==1&&a->queue.front().owned==owned&&a->queue.front().created==100&&a->queue.front().ttl==500&&a->session.scope()==original&&shutdowns==0);
 std::string payload;assert(prepare_queued(*a,queued,payload)==QueuedState::stale&&payload.empty());
 assert(a->session.advance_configuration(8)==Error::none);a->status.config_revision=8;a->context_ready=false;
 assert(prepare_queued(*a,queued,payload)==QueuedState::waiting);
 a->context_ready=true;assert(prepare_queued(*a,queued,payload)==QueuedState::ready);
 Json parsed;JsonError error;model::OwnedIntent decoded;assert(parse_json(payload,parsed,error)&&parse_owned_intent(parsed,decoded)==Error::none&&!parsed.get("execution_lease_id"));
 assert(queued.owned==owned&&queued.revision==8&&queued.created==100&&queued.ttl==500&&owned->config_revision==8);
 auto stale=queued;stale.revision=7;assert(prepare_queued(*a,stale,payload)==QueuedState::stale);
 // The actual worker block drops an expired press at its original deadline,
 // even if current context was unavailable during refresh. Its hold never starts.
 clock_ms=600;a->context_ready=false;assert(pump(*a)&&a->tls->sent.empty()&&a->queue.empty()&&!a->restart_requested);
 assert(diagnostic_logs.size()==1&&diagnostic_logs.front().find("Key dropped stage=expired")!=std::string::npos&&diagnostic_logs.front().find("phase=down age_ms=500 ttl_ms=500")!=std::string::npos);
 auto settings=Queued{Kind::settings_read,"{\"request_id\":\""+ident<model::MessageId>(91).str()+"\"}",1000,600,7,{}};
 a->queue.push_back(settings);clock_ms=601;assert(pump(*a)&&a->queue.empty()&&a->tls->sent.empty()); // no old management-payload restamp
 a->context_ready=true;clock_ms=650;queued.created=650;a->queue.push_back(queued);assert(pump(*a)&&a->tls->sent.size()==1&&a->tls->sent.back().config_revision==8);
}
void expired_presses_are_dropped_without_restart(){
 auto a=start(false);on_send={};const auto scope=a->session.scope();
 std::vector<uint8_t> retries;controls.report_dropped=[&](uint8_t adapter){retries.push_back(adapter);};
 model::OwnedAdapterSnapshot report;report.owner=scope.local_id;report.boot=scope.local_boot_id;report.adapter=0;report.ready=true;
 auto key=std::make_shared<model::OwnedIntent>();key->kind=model::OwnedIntentKind::key_edges;key->generation=1;key->config_revision=scope.config_revision;key->edges[0]={4,true};key->edge_count=1;
 key->target={{scope.consortium_id,scope.peer_id,0,model::Side::back},"Player"};
 const auto observation=Queued{Kind::scoped_result,{},1000,100,scope.config_revision,{},std::make_shared<const model::OwnedAdapterSnapshot>(report)};
 const auto input=Queued{Kind::scoped_intent,{},1000,100,scope.config_revision,key};
 a->queue.push_back(observation);clock_ms=1200;assert(pump(*a)&&a->queue.empty()&&a->tls->sent.empty()&&!a->cancelled&&diagnostic_logs.empty());
 assert(retries==std::vector<uint8_t>{0});
 a->queue.push_back(observation);assert(queue_control(a->peer,Kind::scoped_intent,1000,{},scope.config_revision,key)==ESP_OK&&!a->cancelled&&shutdowns==0&&a->queue.size()==1);
 assert(retries.size()==2&&diagnostic_logs.empty()&&a->queue.front().owned==key);
 // A late press is dropped at admission without ending the session.
 a->queue.clear();a->queue.push_back(input);
 assert(queue_control(a->peer,Kind::scoped_result,1000,{},scope.config_revision)==ESP_OK);
 assert(!a->restart_requested&&!a->execution_fenced&&a->ready&&!a->cancelled&&shutdowns==0&&a->queue.size()==1&&!a->queue.front().owned);
 assert(diagnostic_logs.size()==1&&diagnostic_logs.front().find("Key dropped stage=expired")!=std::string::npos&&diagnostic_logs.front().find("age_ms=1100 ttl_ms=1000")!=std::string::npos);
 // The worker drops it the same way.
 a=start(false);a->queue.push_back(input);clock_ms=1200;assert(pump(*a)&&a->queue.empty()&&a->tls->sent.empty()&&!a->restart_requested);
 assert(diagnostic_logs.size()==1&&diagnostic_logs.front().find("Key dropped stage=expired")!=std::string::npos);
 // A release is never lost to delay, and keeps its full lifetime on the wire.
 a=start(false);auto release=std::make_shared<model::OwnedIntent>(*key);release->edges[0].down=false;
 a->queue.push_back(Queued{Kind::scoped_intent,{},2000,100,scope.config_revision,release});clock_ms=5000;
 assert(pump(*a)&&a->queue.empty()&&a->tls->sent.size()==1&&a->tls->sent[0].ttl_ms==2000&&diagnostic_logs.empty());
 // A newer activity selection replaces an older one still queued for the owner.
 auto first=std::make_shared<model::OwnedIntent>();first->kind=model::OwnedIntentKind::activity;first->generation=1;first->config_revision=scope.config_revision;first->selected=true;
 first->target={{scope.consortium_id,scope.peer_id,0,model::Side::front},""};auto second=std::make_shared<model::OwnedIntent>(*first);second->generation=2;
 const auto woken=wakes;
 assert(queue_control(a->peer,Kind::scoped_intent,1000,{},scope.config_revision,first)==ESP_OK&&queue_control(a->peer,Kind::scoped_intent,1000,{},scope.config_revision,second)==ESP_OK&&wakes==woken+2); // Admitted work wakes its link.
 assert(a->queue.size()==1&&a->queue.front().owned==second);
 clock_ms+=5000;assert(pump(*a)&&a->queue.empty()&&a->tls->sent.size()==2&&a->tls->sent[1].ttl_ms==1000&&diagnostic_logs.empty()); // Late, never dropped.
}
void release_crosses_pending_revision(){
 const auto released=[](const Envelope& sent){model::OwnedIntent delivered;
  return sent.kind==Kind::scoped_intent&&parse_owned_intent(sent.payload,delivered)==Error::none&&delivered.edge_count==1&&delivered.edges[0].key==0x41&&!delivered.edges[0].down;};
 // The owner applied revision 8 while the link still runs scope 7, before or
 // after the revision reaches the link. Button-up for a hold this link carried
 // leaves in scope 7, ahead of the advance marker; a new press is refused.
 for(const bool published:{false,true}){
  auto a=start(true);on_send={};const auto scope=a->session.scope();
  model::OwnedIntent down;down.kind=model::OwnedIntentKind::key_edges;down.target.port={scope.consortium_id,scope.peer_id,0,model::Side::front};
  down.config_revision=7;down.generation=1;down.edges[0]={0x41,true};down.edge_count=1;
  assert(runtime_send_owned(down)==ESP_OK&&pump(*a)&&a->tls->sent.size()==1);
  controls.configuration_revision=[]{return uint64_t(8);};
  if(published)runtime_configuration_changed(8);
  auto press=down;press.config_revision=8;press.edges[0].key=4;
  assert(runtime_send_owned(press)==ESP_ERR_INVALID_STATE);
  auto release=down;release.config_revision=8;release.edges[0].down=false;
  assert(runtime_send_owned(release)==ESP_OK&&a->queue.size()==1);
  if(!published)runtime_configuration_changed(8);
  assert(refresh_turn(*a)&&a->session.scope().config_revision==7&&a->tls->sent.size()==1);
  assert(pump(*a)&&a->queue.empty()&&a->tls->sent.size()==2&&a->tls->sent[1].config_revision==7&&released(a->tls->sent[1]));
  assert(refresh_turn(*a)&&a->session.scope().config_revision==8&&a->tls->sent.size()==3&&a->tls->sent[2].kind==Kind::synchronization);
  assert(!a->cancelled&&!a->restart_requested);
 }
 // A second revision applied before the link advances does not discard it.
 {
  auto a=start(true);on_send={};const auto scope=a->session.scope();
  model::OwnedIntent release;release.kind=model::OwnedIntentKind::key_edges;release.target.port={scope.consortium_id,scope.peer_id,0,model::Side::front};
  release.config_revision=8;release.generation=1;release.edges[0]={0x41,false};release.edge_count=1;
  controls.configuration_revision=[]{return uint64_t(8);};assert(runtime_send_owned(release)==ESP_OK);
  controls.configuration_revision=[]{return uint64_t(9);};runtime_configuration_changed(9);
  for(unsigned turn=0;turn<3;++turn)assert(refresh_turn(*a)&&pump(*a));
  assert(a->session.scope().config_revision==9&&a->queue.empty()&&a->tls->sent.size()==2&&released(a->tls->sent[0])&&a->tls->sent[0].config_revision==7);
 }
 // A link that advanced between admission and queueing carries it in its new scope.
 {
  auto a=start(true);on_send={};const auto scope=a->session.scope();
  runtime_configuration_changed(8);assert(refresh_turn(*a)&&a->session.scope().config_revision==8);a->tls->sent.clear();
  auto release=std::make_shared<model::OwnedIntent>();release->kind=model::OwnedIntentKind::key_edges;release->target.port={scope.consortium_id,scope.peer_id,0,model::Side::front};
  release->config_revision=7;release->generation=1;release->edges[0]={0x41,false};release->edge_count=1;
  assert(queue_control(a->peer,Kind::scoped_intent,2000,scope.link_id,7,release,clock_ms)==ESP_OK);
  assert(pump(*a)&&a->queue.empty()&&a->tls->sent.size()==1&&a->tls->sent[0].config_revision==8&&released(a->tls->sent[0]));
 }
 // Button-up admitted while the link writes the advance marker, before it
 // applies the new scope, follows the marker in that scope.
 {
  auto a=start(true);on_send={};const auto scope=a->session.scope();
  model::OwnedIntent down;down.kind=model::OwnedIntentKind::key_edges;down.target.port={scope.consortium_id,scope.peer_id,0,model::Side::front};
  down.config_revision=7;down.generation=1;down.edges[0]={0x41,true};down.edge_count=1;
  assert(runtime_send_owned(down)==ESP_OK&&pump(*a)&&a->tls->sent.size()==1);
  controls.configuration_revision=[]{return uint64_t(8);};runtime_configuration_changed(8);
  auto release=down;release.config_revision=8;release.edges[0].down=false;
  int admitted=-1;
  on_send=[&](Active&,const Envelope& sent){if(sent.kind==Kind::synchronization&&admitted<0)admitted=runtime_send_owned(release);};
  assert(refresh_turn(*a)&&admitted==ESP_OK&&a->session.scope().config_revision==8);on_send={};
  for(unsigned turn=0;turn<3;++turn)assert(refresh_turn(*a)&&pump(*a));
  assert(a->queue.empty()&&a->tls->sent.size()==3&&a->tls->sent[1].kind==Kind::synchronization&&released(a->tls->sent[2])&&a->tls->sent[2].config_revision==8);
  assert(!a->cancelled&&!a->restart_requested);
 }
}
}
int main(){using namespace amity::consortium;terminal_receive_trace();coordinator_refresh();member_refresh();future_context_recaptures_after_refresh();rejected_bind_keeps_scope();immutable_queue();expired_presses_are_dropped_without_restart();release_crosses_pending_revision();std::cout<<"PASS actual in-place configuration advance: both roles retain TLS/link/boots/readiness/cache; one forward-only marker; old-scope messages discarded; fresh context and original queued revision/deadline; late presses dropped without restart, releases and selections delivered late, releases cross a pending revision\n";}
'''


def main():
    source = (ROOT / 'components/consortium/runtime.cpp').read_text()
    # Run the same queue block as the link task, including break-on-expired-
    # causal-work and send-time TTL reduction; no copy of its decisions.
    start = source.index('\n', source.index('        if(!active->retiring&&!refresh_local_hostname(*active))')) + 1
    end = source.index('        fd_set reads;', start)
    pump = '\nbool pump(Active& storage){Active* active=&storage;LinkTrace trace;trace.phase="operational";while(true){' + source[start:end] + '(void)more;return true;}trace_link_closed(storage,trace,now_ms());return false;}\n'
    actual = ''.join(function(source, name) for name in (
        'Envelope outgoing(', 'bool bind_configuration(', 'bool apply_configuration_advance(', 'bool advance_configuration(', 'bool finish_synchronization(', 'bool exchange_hostname(Active& active,const std::string& expected,LinkTrace* diagnostic=nullptr) {',
        'bool synchronization_current(', 'bool synchronization_recoverable(', 'bool synchronization_barrier(', 'bool fence_synchronization_scope(', 'bool restart_synchronization_scope(', 'bool synchronize_connection(', 'bool refresh_local_hostname(',
        'void runtime_configuration_changed('))
    actual = source[source.index('static bool pure_key_release('):source.index('struct SynchronizationRound {')] + function(source, 'void request_session_restart(') + source[source.index('struct LinkTraceSnapshot {'):source.index('std::optional<uint8_t> dropped_report(')] + actual
    actual += source[source.index('std::optional<uint8_t> dropped_report('):source.index('enum class QueuedState')]
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
                   ('json.cpp', 'wire.cpp', 'owned_intent.cpp', 'session.cpp', 'settings.cpp', 'update.cpp')]
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
