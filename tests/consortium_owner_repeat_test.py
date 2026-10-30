#!/usr/bin/env python3
"""Execute actual qualified state-change codec, FIFO handoff and link fencing."""
from pathlib import Path
import os, shlex, subprocess, tempfile
from consortium_gatt_callback_test import function
ROOT=Path(__file__).resolve().parents[1]
PREFIX=r'''
#include "amity/consortium/runtime.hpp"
#include "amity/consortium/owned_intent.hpp"
#include "amity/consortium/update.hpp"
#include "controller_policy.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <deque>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <sys/socket.h>
#include <thread>
namespace amity::consortium {
using namespace platform;
uint64_t clock_ms=100;uint64_t now_ms(){return clock_ms;}
std::vector<std::string> diagnostic_logs;
void assert_log_unlocked();
template<class... Args>void capture_log(const char*,const char* format,Args... args){assert_log_unlocked();char text[1024];snprintf(text,sizeof(text),format,args...);assert(std::string(text).size()<384);if(std::string(format).find("Link scope")==0)return;diagnostic_logs.emplace_back(text);}
#define ESP_LOGW capture_log
#define ESP_LOGI capture_log
template<class T>T ident(unsigned n){char text[33];snprintf(text,sizeof(text),"%032x",n);return *T::parse(text);}
struct Queued{Kind kind;std::string payload;uint32_t ttl;uint64_t created;uint64_t revision=0;std::shared_ptr<const model::OwnedIntent> owned;};
struct Tls{int socket=-1;model::OctopusId authenticated;};
struct Active{Session session;std::shared_ptr<Tls> tls=std::make_shared<Tls>();model::OctopusId peer;std::mutex mutex,close_mutex;bool cancelled=false,retiring=false,ready=true,management_ready=false,context_ready=true,context_requested=false,closed_notified=false,refreshing=false;RuntimePeerStatus status;std::atomic<uint64_t> own_control_deadline{0},own_realtime_deadline{0};std::deque<Queued> queue;};
constexpr size_t maximum_links=2;
std::mutex lock;std::vector<std::shared_ptr<Active>> links;RuntimeControlHooks controls;RuntimeStatus status;Group group;
struct Identity{model::OctopusId id;}identity;
model::BootId boot;uint64_t configuration=1;
void assert_log_unlocked(){
 bool available=false;std::thread contender([&]{
  if(!lock.try_lock())return;
  available=true;for(const auto& active:links){if(active->mutex.try_lock())active->mutex.unlock();else available=false;}
  lock.unlock();
 });contender.join();assert(available); // SDK logging must run after both guards.
}
esp_err_t runtime_send_update(model::OctopusId,Kind,std::string,uint32_t){return ESP_ERR_NOT_SUPPORTED;}
}
'''
SUITE=r'''
namespace amity::consortium {
std::shared_ptr<Active> connection(unsigned link=7){
 auto a=std::make_shared<Active>();a->peer=ident<model::OctopusId>(3);a->status.id=a->peer;a->status.boot_id=ident<model::BootId>(8);a->status.config_revision=1;a->status.negotiated_capabilities=known_capabilities;a->status.control_lease=ident<model::ExecutionLeaseId>(11);a->status.realtime_lease=ident<model::ExecutionLeaseId>(12);a->status.control_usable_until_ms=a->status.realtime_usable_until_ms=5100;
 const LinkScope scope{ident<model::LinkId>(link),ident<model::ConsortiumId>(1),ident<model::OctopusId>(2),a->peer,ident<model::OctopusId>(2),ident<model::BootId>(4),a->status.boot_id,1,1};
 assert(a->session.authenticate(scope,scope.peer_id,PeerRole::actuator,true)==Error::none);return a;
}
void reset(){diagnostic_logs.clear();links.clear();controls={};status={};status.authority_ready=true;group={};group.role=Group::coordinator;group.epoch=1;identity.id=group.authority=ident<model::OctopusId>(2);clock_ms=100;}
model::OwnedIntent down(unsigned sequence=1){model::OwnedIntent i;i.kind=model::OwnedIntentKind::key_down;i.target={{ident<model::ConsortiumId>(1),ident<model::OctopusId>(3),0,model::Side::back},"Player"};i.generation=2;i.sequence=sequence;i.config_revision=1;i.token=ident<model::HoldToken>(50+sequence);i.key=0x41;i.repeat=true;return i;}
void closure(){
 reset();auto old=connection(),current=connection(9);links={old,current};unsigned calls=0;
 controls.disconnected=[&](auto owner,auto boot,auto link){++calls;assert(owner==old->peer&&boot==old->status.boot_id&&link==ident<model::LinkId>(7));return true;};
 assert(fence_replaced_links(*current)&&old->cancelled&&!old->ready&&!current->cancelled&&calls==1);assert(notify_link_closed(*old)&&calls==1);reap_closed_links();assert(links.size()==1);
 reset();old=connection();current=connection(9);links={old,current};bool fenced=false;controls.disconnected=[&](auto,auto,auto){return fenced;};assert(!fence_replaced_links(*current));reap_closed_links();assert(links.size()==2);fenced=true;assert(fence_replaced_links(*current));reap_closed_links();assert(links.size()==1);
 // New-link readiness waits for completion of the old owner fence.
 reset();old=connection();current=connection(9);links={old,current};std::mutex gate;std::condition_variable cv;bool entered=false,finish=false;calls=0;
 controls.disconnected=[&](auto,auto,auto){std::unique_lock<std::mutex> guard(gate);++calls;entered=true;cv.notify_all();cv.wait(guard,[&]{return finish;});return true;};
 auto closing=std::async(std::launch::async,[&]{return notify_link_closed(*old);});{std::unique_lock<std::mutex> guard(gate);cv.wait(guard,[&]{return entered;});}
 auto replacement=std::async(std::launch::async,[&]{return fence_replaced_links(*current);});assert(replacement.wait_for(std::chrono::milliseconds(30))==std::future_status::timeout);{std::lock_guard<std::mutex> guard(gate);finish=true;}cv.notify_all();assert(closing.get()&&replacement.get()&&calls==1);
}
void queues(){
 reset();auto a=connection();links={a};auto i=down();
 assert(runtime_send_control(a->peer,Kind::heartbeat,"first",1000)==ESP_OK);assert(runtime_send_owned(i)==ESP_OK);i.kind=model::OwnedIntentKind::key_up;++i.sequence;assert(runtime_send_owned(i)==ESP_OK);
 assert(a->queue.size()==3&&a->queue[0].payload=="first"&&a->queue[1].kind==Kind::scoped_intent&&queued_cleanup(a->queue[2])); // strict causal FIFO, no cleanup priority reordering
 reset();a=connection();links={a};for(unsigned n=0;n<15;++n)assert(runtime_send_control(a->peer,Kind::heartbeat,"{}",1000)==ESP_OK);assert(runtime_send_control(a->peer,Kind::heartbeat,"{}",1000)==ESP_ERR_NO_MEM);i=down();i.kind=model::OwnedIntentKind::key_up;assert(runtime_send_owned(i)==ESP_OK&&a->queue.size()==16);assert(runtime_send_owned(i)==ESP_ERR_NO_MEM&&a->cancelled);
 reset();a=connection();links={a};i=down();i.kind=model::OwnedIntentKind::key_up;assert(runtime_send_owned(i)==ESP_OK);clock_ms+=2000;assert(runtime_send_control(a->peer,Kind::heartbeat,"{}",1000)==ESP_ERR_INVALID_STATE&&a->cancelled);assert(diagnostic_logs.size()==1&&diagnostic_logs[0].find("reason=causal_expired phase=queue_admission")!=std::string::npos&&diagnostic_logs[0].find("kind=scoped_intent age_ms=2000 ttl_ms=2000")!=std::string::npos);
 reset();a=connection();links={a};i=down();i.kind=model::OwnedIntentKind::source_input;assert(runtime_send_owned(i)==ESP_OK);clock_ms+=1000;assert(runtime_send_control(a->peer,Kind::heartbeat,"{}",1000)==ESP_ERR_INVALID_STATE&&a->cancelled);
 reset();a=connection();links={a};const auto result=std::string("{\"input_result\":{\"generation\":\"2\",\"sequence\":\"1\",\"source_present\":true}}");
 assert(runtime_send_owned_result(a->peer,ident<model::LinkId>(99),result)==ESP_ERR_INVALID_STATE&&a->queue.empty());
 for(unsigned n=0;n<15;++n)assert(runtime_send_control(a->peer,Kind::heartbeat,"{}",1000)==ESP_OK);
 assert(runtime_send_owned_result(a->peer,a->session.scope().link_id,result)==ESP_OK&&a->queue.size()==16);
 assert(runtime_send_owned_result(a->peer,a->session.scope().link_id,result)==ESP_ERR_NO_MEM&&a->cancelled);assert(diagnostic_logs.size()==1&&diagnostic_logs[0].find("reason=cleanup_queue_full")!=std::string::npos);
 reset();a=connection();links={a};assert(runtime_send_control(a->peer,Kind::heartbeat,"{}",100)==ESP_OK);clock_ms+=100;assert(runtime_send_control(a->peer,Kind::heartbeat,"{}",100)==ESP_OK&&a->queue.size()==1);assert(runtime_send_control(a->peer,static_cast<Kind>(21),"{}",1000)==ESP_ERR_NOT_SUPPORTED);
 reset();a=connection();links={a};a->status.realtime_usable_until_ms=clock_ms;assert(runtime_send_owned(down())==ESP_ERR_INVALID_STATE);assert(a->queue.empty());
}
void initial_context(){
 reset();auto a=connection();links={a};a->context_ready=false;
 auto i=down();assert(runtime_send_owned(i)==ESP_ERR_INVALID_STATE&&a->queue.empty());
 i.kind=model::OwnedIntentKind::source_input;assert(runtime_send_owned(i)==ESP_ERR_INVALID_STATE);
 unsigned refreshes=0;controls.ready=[&](auto owner,auto,auto){++refreshes;assert(owner==a->peer&&!a->context_ready);assert(runtime_send_owned(down(2))==ESP_ERR_INVALID_STATE);return true;};
 // True means the recapture entered owner-task IPC, not that it has run.
 assert(initialize_context(*a)&&initialize_context(*a)&&refreshes==1&&!a->context_ready&&a->context_requested&&a->queue.empty());
 assert(runtime_send_owned(down(2))==ESP_ERR_INVALID_STATE);
 i.kind=model::OwnedIntentKind::context;i.target.osd_name.clear();assert(runtime_send_owned(i)==ESP_OK&&a->queue.size()==1);
 assert(a->context_ready&&!a->context_requested&&runtime_send_owned(down(2))==ESP_OK&&a->queue.size()==2);
 assert(a->queue[0].owned&&a->queue[0].owned->kind==model::OwnedIntentKind::context&&a->queue[0].revision==1);
 reset();a=connection();links={a};a->context_ready=false;assert(!initialize_context(*a)&&!a->context_ready&&!a->context_requested);controls.ready=[](auto,auto,auto){return false;};assert(!initialize_context(*a)&&!a->context_ready&&!a->context_requested);
 // No saved selectors means an admitted recapture may emit no context. Local
 // discovery and exact key-up cleanup still reach the owner independently.
 reset();a=connection();links={a};group.consortium=a->session.scope().consortium_id;a->context_ready=false;a->refreshing=true;
 unsigned empty_requests=0;std::function<void(model::OwnedContextResult)> empty_completed;
 controls.ready=[&](auto,auto,auto completed){++empty_requests;empty_completed=std::move(completed);return true;};assert(initialize_context(*a));
 empty_completed(model::OwnedContextResult::published);
 assert(!a->refreshing&&!a->context_ready&&a->context_requested&&a->queue.empty());
 assert(initialize_context(*a)&&empty_requests==1); // Empty success is complete, not another capture request.
 auto scan=down();scan.kind=model::OwnedIntentKind::discover;scan.target.osd_name.clear();assert(runtime_send_owned(scan)==ESP_OK&&!a->context_ready);
 auto up=down(2);up.kind=model::OwnedIntentKind::key_up;assert(runtime_send_owned(up)==ESP_OK&&!a->context_ready);
 assert(runtime_send_owned(down(3))==ESP_ERR_INVALID_STATE);
}
void future_context_is_recaptured(){
 reset();auto a=connection();links={a};configuration=2;
 auto current=down();current.kind=model::OwnedIntentKind::context;current.target.osd_name.clear();current.config_revision=2;current.generation=8;current.sequence=10;
 assert(runtime_send_owned(current)==ESP_OK&&a->queue.empty()&&a->context_ready&&!a->cancelled);
 auto action=down(11);action.config_revision=2;action.generation=8;
 assert(runtime_send_owned(action)==ESP_ERR_INVALID_STATE&&a->queue.empty()&&!a->cancelled);
 action.kind=model::OwnedIntentKind::power;assert(runtime_send_owned(action)==ESP_ERR_INVALID_STATE&&a->queue.empty());
 // The exchange may take longer than the ordinary action TTL. No old payload
 // or deadline is retained, so configuration alone cannot expire leased work.
 clock_ms+=1150;assert(a->queue.empty()&&!a->cancelled);
 const auto scope=a->session.scope();HelloFrame hello;hello.consortium_id=scope.consortium_id;hello.octopus_id=scope.peer_id;hello.boot_id=scope.peer_boot_id;hello.coordinator_epoch=1;hello.config_revision=1;hello.required_capabilities=known_capabilities;
 assert(a->session.hello(hello,known_capabilities)==Error::none&&a->session.synchronized(1,true,true)==Error::none);
 assert(a->session.advance_configuration(2)==Error::none);a->status.config_revision=2;a->context_ready=false;a->refreshing=true;a->context_requested=false;
 a->status.control_lease=ident<model::ExecutionLeaseId>(90);a->status.control_usable_until_ms=clock_ms+4900;
 unsigned requested=0;controls.ready=[&](auto,auto,auto){++requested;return true;};assert(initialize_context(*a)&&requested==1&&!a->context_ready&&a->context_requested);
 action.kind=model::OwnedIntentKind::source_input;assert(runtime_send_owned(action)==ESP_ERR_INVALID_STATE&&a->queue.empty());
 // Recapture reads the current activity now, with a newer generation/sequence.
 current.generation=9;current.sequence=20;current.selected=true;assert(runtime_send_owned(current)==ESP_OK&&a->context_ready&&!a->refreshing&&a->queue.size()==1);
 assert(a->queue[0].owned->generation==9&&a->queue[0].owned->sequence==20&&a->queue[0].created==clock_ms&&a->queue[0].revision==2);
 action.generation=9;action.sequence=21;assert(runtime_send_owned(action)==ESP_OK&&a->queue.size()==2&&a->queue[1].owned->sequence==21);
 // Once ordinary work is admitted, its original TTL and fence remain intact.
 a->queue.pop_front();clock_ms+=1000;assert(runtime_send_control(a->peer,Kind::heartbeat,"{}",1000)==ESP_ERR_INVALID_STATE&&a->cancelled);
 assert(diagnostic_logs.size()==1&&diagnostic_logs[0].find("reason=causal_expired")!=std::string::npos);
}
void asynchronous_context_failure_is_scoped(){
 reset();auto a=connection();links={a};group.consortium=a->session.scope().consortium_id;a->context_ready=false;
 std::function<void(model::OwnedContextResult)> done;
 controls.ready=[&](auto owner,uint64_t revision,auto completed){assert(owner==a->peer&&revision==1);done=std::move(completed);return true;};
 assert(initialize_context(*a)&&a->context_requested&&!a->context_ready);
 clock_ms=5101;auto context=down();context.kind=model::OwnedIntentKind::context;context.target.osd_name.clear();
 assert(runtime_send_owned(context)==ESP_ERR_INVALID_STATE&&a->queue.empty()&&!a->cancelled);
 done(model::OwnedContextResult::failed);assert(a->cancelled&&!a->context_requested&&!a->context_ready&&diagnostic_logs.size()==1);
 assert(diagnostic_logs[0].find("reason=context_recapture_failed")!=std::string::npos);done(model::OwnedContextResult::failed);assert(diagnostic_logs.size()==1);
 // A local R+1 configuration may be executing before the runtime publishes
 // that revision. The old command reports stale, leaving its healthy R link.
 reset();a=connection();links={a};group.consortium=a->session.scope().consortium_id;a->context_ready=false;
 controls.ready=[&](auto,uint64_t,auto completed){done=std::move(completed);return true;};assert(initialize_context(*a));
 const auto original=a->session.scope();done(model::OwnedContextResult::stale);assert(!a->cancelled&&a->session.scope()==original&&diagnostic_logs.empty());
 configuration=2;a->refreshing=true;done(model::OwnedContextResult::published);
 assert(a->refreshing&&!a->context_ready&&a->context_requested&&!a->cancelled); // Old success cannot finish a newer target revision.
 HelloFrame hello;hello.consortium_id=original.consortium_id;hello.octopus_id=original.peer_id;hello.boot_id=original.peer_boot_id;hello.coordinator_epoch=1;hello.config_revision=1;hello.required_capabilities=known_capabilities;
 assert(a->session.hello(hello,known_capabilities)==Error::none&&a->session.synchronized(1,true,true)==Error::none&&a->session.advance_configuration(2)==Error::none);
 configuration=2;a->status.config_revision=2;a->context_requested=false;a->context_ready=false;
 std::function<void(model::OwnedContextResult)> newer;
 controls.ready=[&](auto,uint64_t revision,auto completed){assert(revision==2);newer=std::move(completed);return true;};assert(initialize_context(*a));
 done(model::OwnedContextResult::failed);done(model::OwnedContextResult::published);assert(!a->cancelled&&a->context_requested&&a->refreshing&&diagnostic_logs.empty());
 context=down(20);context.kind=model::OwnedIntentKind::context;context.target.osd_name.clear();context.config_revision=2;assert(runtime_send_owned(context)==ESP_OK);newer(model::OwnedContextResult::published);
 assert(a->context_ready&&!a->context_requested&&!a->cancelled&&a->queue.size()==1);
 // Nor can an old callback fence a replacement authenticated connection.
 links.clear();auto replacement=connection(9);links={replacement};done(model::OwnedContextResult::failed);assert(!replacement->cancelled&&diagnostic_logs.empty());
}
void diagnostic_lines_fit_capture(){
 reset();auto a=connection();LinkTrace trace;trace.phase="configuration_receive";trace.fail("synchronization_interleaving",Error::unsupported_kind);trace.has_kind=trace.has_queue=true;trace.kind=Kind::execution_lease;trace.created=0;trace.ttl=UINT32_MAX;trace.queued_revision=UINT64_MAX;trace.action_code=INT32_MIN;trace.socket_error=INT32_MIN;
 auto captured=snapshot_link_trace(*a,trace,UINT64_MAX);captured.scope.config_revision=UINT64_MAX;emit_link_trace(captured);assert(diagnostic_logs.size()==1&&diagnostic_logs[0].size()<384&&diagnostic_logs[0].find("qrev=18446744073709551615")!=std::string::npos);
}
void authorization(){
 reset();auto a=connection();links={a};
 // Receiver owns the leases. A long control lease cannot prolong a held key.
 group.role=Group::member;group.authority=a->peer;boot=ident<model::BootId>(4);configuration=1;
 a->own_control_deadline=1000;a->own_realtime_deadline=200;
 const auto scope=a->session.scope();
 model::OwnedAuthorization auth{a->peer,scope.peer_boot_id,boot,scope.link_id,1,200};
 auth.authority=model::OwnedAuthority::realtime;assert(runtime_owned_deadline(auth)==200);
 clock_ms=200;assert(!runtime_owned_authorized(auth));
 auth.authority=model::OwnedAuthority::control;assert(runtime_owned_deadline(auth)==1000);
 auth.authority=model::OwnedAuthority::cleanup;assert(runtime_owned_deadline(auth)==UINT64_MAX);
 auth.link_id=ident<model::LinkId>(99);assert(!runtime_owned_authorized(auth));auth.link_id=scope.link_id;
 auth.controller_boot=ident<model::BootId>(99);assert(!runtime_owned_authorized(auth));auth.controller_boot=scope.peer_boot_id;
 auth.config_revision=2;assert(!runtime_owned_authorized(auth));auth.config_revision=1;
 runtime_fence_owned_link(a->peer,ident<model::LinkId>(99));assert(!a->cancelled);
 runtime_fence_owned_link(a->peer,scope.link_id);assert(a->cancelled&&!runtime_owned_authorized(auth));assert(diagnostic_logs.size()==1&&diagnostic_logs[0].find("reason=causal_result_fence")!=std::string::npos);runtime_fence_owned_link(a->peer,scope.link_id);assert(diagnostic_logs.size()==1);
 reset();identity.id=group.authority=ident<model::OctopusId>(2);boot=ident<model::BootId>(4);
 auth={identity.id,boot,boot,{},1,0};assert(runtime_owned_deadline(auth)==UINT64_MAX);
}
void codec_and_session(){
 auto input=down();auto lease=ident<model::ExecutionLeaseId>(12);auto text=owned_intent_json(input,lease);assert(!text.empty());Json value;JsonError error;assert(parse_json(text,value,error));model::OwnedIntent parsed;model::ExecutionLeaseId received;assert(parse_owned_intent(value,parsed,received)==Error::none&&parsed.target==input.target&&parsed.repeat&&parsed.token==input.token);
 auto bad=value;bad.object.emplace_back("enabled",Json{});assert(parse_owned_intent(bad,parsed,received)==Error::invalid_message);bad=value;bad.get("target")->object.emplace_back("logical_address",Json{});assert(parse_owned_intent(bad,parsed,received)==Error::invalid_message);bad=value;bad.get("arguments")->object.emplace_back("hold_lease_ms",Json{});assert(parse_owned_intent(bad,parsed,received)==Error::invalid_message);input.target.osd_name.clear();assert(owned_intent_json(input,lease).empty());input=down();input.sequence=0;assert(owned_intent_json(input,lease).empty());
 LinkScope scope{ident<model::LinkId>(7),ident<model::ConsortiumId>(1),ident<model::OctopusId>(3),ident<model::OctopusId>(2),ident<model::OctopusId>(2),ident<model::BootId>(8),ident<model::BootId>(4),1,1};
 Session session;assert(session.authenticate(scope,scope.peer_id,PeerRole::coordinator,true)==Error::none);HelloFrame h;h.consortium_id=scope.consortium_id;h.octopus_id=scope.peer_id;h.boot_id=scope.peer_boot_id;h.coordinator_epoch=1;h.required_capabilities=known_capabilities;assert(session.hello(h,known_capabilities)==Error::none&&session.synchronized(1,true,true)==Error::none);assert(session.execution_leases().issue(lease,LeaseClass::realtime_input,500,100)==Error::none);
 Envelope e;e.consortium_id=scope.consortium_id;e.message_id=ident<model::MessageId>(70);e.origin_octopus_id=scope.peer_id;e.destination_octopus_id=scope.local_id;e.origin_boot_id=scope.peer_boot_id;e.destination_boot_id=scope.local_boot_id;e.coordinator_epoch=e.config_revision=1;e.kind=Kind::scoped_intent;e.ttl_ms=500;e.payload=value;
 assert(session.admit(e,100,100)==Error::none&&session.admit(e,100,100)==Error::duplicate);e.message_id=ident<model::MessageId>(71);assert(session.admit(e,600,600)==Error::lease_expired);
 input=down(2);input.kind=model::OwnedIntentKind::key_up;text=owned_intent_json(input,lease);assert(parse_json(text,e.payload,error));assert(session.admit(e,600,600)==Error::none); // exact-token up independent of expired positive lease
 e.message_id=ident<model::MessageId>(72);e.origin_boot_id=ident<model::BootId>(99);assert(session.admit(e,600,600)==Error::stale_boot);e.origin_boot_id=scope.peer_boot_id;e.config_revision=2;assert(session.admit(e,600,600)==Error::stale_config);
 model::OwnedAdapterSnapshot report;report.owner=scope.local_id;report.boot=scope.local_boot_id;report.devices.push_back({model::Side::back,"Player",4,4,0x1000,1,true});text=owned_snapshot_json(report);assert(!text.empty()&&parse_json(text,value,error));model::OwnedAdapterSnapshot decoded;assert(parse_owned_snapshot(value,decoded)&&decoded.devices.front().osd_name=="Player");value.get("devices")->array.push_back(value.get("devices")->array.front());assert(!parse_owned_snapshot(value,decoded));
}
}
int main(){using namespace amity::consortium;closure();queues();initial_context();future_context_is_recaptured();asynchronous_context_failure_is_scoped();diagnostic_lines_fit_capture();authorization();codec_and_session();std::cout<<"PASS qualified transport: actual FIFO/key-up capacity, link loss/replacement fences, scoped codecs, stale boots/config, leases and duplicate rejection\\n";}
'''
def main():
 source=(ROOT/'components/consortium/runtime.cpp').read_text()
 names=['bool initialize_context(','bool notify_link_closed(','bool fence_replaced_links(','void reap_closed_links(','static bool queued_cleanup(const Queued& queued)','static bool queued_causal(','static esp_err_t queue_control(','esp_err_t runtime_send_control(','esp_err_t runtime_send_owned_result(','void runtime_fence_owned_link(','esp_err_t runtime_send_owned(','uint64_t runtime_owned_deadline(','bool runtime_owned_authorized(']
 trace=source[source.index('struct LinkTrace {'):source.index('static bool queued_cleanup(')]
 retries=source[source.index('std::optional<RuntimeSnapshotRetry> snapshot_retry('):source.index('enum class QueuedState')]
 diagnostics=source[source.index('struct LinkTraceSnapshot {'):source.index('std::optional<RuntimeSnapshotRetry> snapshot_retry(')]
 actual=trace+diagnostics+retries+''.join(function(source,name) for name in names)
 with tempfile.TemporaryDirectory(prefix='amity-qualified-transport-') as directory:
  temp=Path(directory);(temp/'esp_err.h').write_text('#pragma once\nusing esp_err_t=int;constexpr int ESP_OK=0,ESP_ERR_INVALID_ARG=1,ESP_ERR_INVALID_STATE=2,ESP_ERR_NO_MEM=3,ESP_ERR_TIMEOUT=4,ESP_FAIL=5,ESP_ERR_NOT_FOUND=6,ESP_ERR_NOT_SUPPORTED=7;\n')
  cpp=temp/'test.cpp';binary=temp/'test';cpp.write_text(PREFIX+'namespace amity::consortium {\n'+actual+'\n}\n'+SUITE)
  sources=[ROOT/'components/consortium'/name for name in ['json.cpp','wire.cpp','owned_intent.cpp','session.cpp','settings.cpp','update.cpp','transfer.cpp']]+[ROOT/'components/amity_system/model.cpp']
  subprocess.run(shlex.split(os.environ.get('CXX','c++'))+['-std=c++17','-Wall','-Wextra','-Werror','-pthread','-g','-fsanitize=address,undefined','-I'+str(temp)]+['-I'+str(ROOT/path) for path in ['components/consortium','components/consortium/include','components/amity_system/include']]+list(map(str,[cpp]+sources))+['-o',str(binary)],check=True)
  subprocess.run([str(binary)],check=True)
if __name__=='__main__':main()
