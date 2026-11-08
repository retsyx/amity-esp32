#!/usr/bin/env python3
"""Execute actual qualified state-change codec, FIFO handoff and link fencing."""
from pathlib import Path
import os, shlex, subprocess, tempfile
from consortium_gatt_callback_test import function
ROOT=Path(__file__).resolve().parents[1]
PREFIX=r'''
#include "amity/consortium/runtime.hpp"
#include "tls_receive_trace.hpp"
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
unsigned shutdowns=0;int capture_shutdown(int,int){++shutdowns;return 0;}
std::vector<std::string> diagnostic_logs,key_release_logs;
void assert_log_unlocked();
template<class... Args>void capture_log(const char*,const char* format,Args... args){assert_log_unlocked();char text[1024];snprintf(text,sizeof(text),format,args...);assert(std::string(text).size()<384);if(std::string(format).find("Link scope")==0)return;if(std::string(format).find("Key release")==0){key_release_logs.emplace_back(text);return;}diagnostic_logs.emplace_back(text);}
#define ESP_LOGW capture_log
#define ESP_LOGI capture_log
template<class T>T ident(unsigned n){char text[33];snprintf(text,sizeof(text),"%032x",n);return *T::parse(text);}
struct Queued{Kind kind;std::string payload;uint32_t ttl;uint64_t created;uint64_t revision=0;std::shared_ptr<const model::OwnedIntent> owned;};
struct Tls{int socket=-1;model::OctopusId authenticated;std::vector<Peer> allowed;bool healthy=true;bool usable()const{return healthy;}};
using TlsLink=Tls;
struct Active{Session session;std::shared_ptr<Tls> tls=std::make_shared<Tls>();model::OctopusId peer,connection_peer;model::BootId claimed_boot;std::atomic_bool execution_fenced{false},restart_requested{false};model::LinkId cleaned_link;std::string endpoint_hint;std::mutex mutex,close_mutex;bool cancelled=false,retiring=false,ready=true,management_ready=false,context_ready=true,context_requested=false,closed_notified=false,refreshing=false;RuntimePeerStatus status;std::deque<Queued> queue;};
// ACK observation has a separate native test; this fixture tests IPC policy.
void begin_release_ack(Active&,const model::OwnedIntent&,uint64_t){}
constexpr int pdPASS=1,MALLOC_CAP_SPIRAM=1,MALLOC_CAP_8BIT=2;
std::vector<std::shared_ptr<Active>*> task_arguments;unsigned task_attempts=0;bool task_creation_fails=false;
void active_loop(void*){}
int xTaskCreateWithCaps(void(*entry)(void*),const char*,unsigned stack,void* arg,unsigned priority,void*,unsigned){
 assert(entry==active_loop&&stack==16384&&priority==3);++task_attempts;
 if(task_creation_fails)return -1;
 task_arguments.push_back(static_cast<std::shared_ptr<Active>*>(arg));return pdPASS;
}

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
std::function<void()> before_send;bool write_result=true;unsigned writes=0;uint32_t written_ttl=0;std::string written_payload;
bool send(Active&,Kind kind,std::string_view payload,uint32_t ttl){assert(kind==Kind::scoped_intent);assert_log_unlocked();++writes;written_ttl=ttl;written_payload=payload;if(before_send)before_send();return write_result;}
esp_err_t runtime_send_update(model::OctopusId,Kind,std::string,uint32_t){return ESP_ERR_NOT_SUPPORTED;}
}
#define shutdown amity::consortium::capture_shutdown
'''
SUITE=r'''
namespace amity::consortium {
std::shared_ptr<Active> connection(unsigned link=7,unsigned peer_boot=8){
 auto a=std::make_shared<Active>();a->peer=ident<model::OctopusId>(3);a->status.id=a->peer;a->status.boot_id=a->claimed_boot=ident<model::BootId>(peer_boot);a->tls->authenticated=a->peer;a->tls->allowed={{a->peer,"leaf","pin",true}};a->status.config_revision=1;a->status.negotiated_capabilities=known_capabilities;
 const LinkScope scope{ident<model::LinkId>(link),ident<model::ConsortiumId>(1),ident<model::OctopusId>(2),a->peer,ident<model::OctopusId>(2),ident<model::BootId>(4),a->status.boot_id,1,1};
 assert(a->session.authenticate(scope,scope.peer_id,PeerRole::actuator,true)==Error::none);return a;
}
void reset(){for(auto* argument:task_arguments)delete argument;task_arguments.clear();task_attempts=0;task_creation_fails=false;shutdowns=0;diagnostic_logs.clear();key_release_logs.clear();before_send={};write_result=true;writes=0;written_ttl=0;written_payload.clear();links.clear();controls={};status={};status.authority_ready=true;group={};group.consortium=ident<model::ConsortiumId>(1);group.role=Group::coordinator;group.epoch=1;identity.id=group.authority=ident<model::OctopusId>(2);group.peers={{ident<model::OctopusId>(3),"leaf","pin",true}};clock_ms=100;}
model::OwnedIntent down(){model::OwnedIntent i;i.kind=model::OwnedIntentKind::key_edges;i.target.port={ident<model::ConsortiumId>(1),ident<model::OctopusId>(3),0,model::Side::front};i.generation=2;i.config_revision=1;i.edges[0]={0x41,true};i.edge_count=1;return i;}
void logical_restart_pending(const Active& active){
 assert(active.restart_requested&&active.execution_fenced&&!active.ready&&!active.status.ready&&!active.management_ready&&!active.status.management_ready);
 assert(!active.context_ready&&!active.context_requested&&active.queue.empty()&&!active.cancelled&&active.tls->usable()&&shutdowns==0);
 assert(runtime_send_control(active.peer,Kind::device_status,"{\"hostname\":\"amity-main\"}",1000)==ESP_ERR_INVALID_STATE&&active.queue.empty());
 assert(!diagnostic_logs.empty()&&diagnostic_logs.back().find("Session restart requested reason=")==0);
}
void closure(){
 reset();auto old=connection(),current=connection(9,18);links={old,current};unsigned calls=0;
 controls.disconnected=[&](auto owner,auto boot,auto link){++calls;assert(owner==old->peer&&boot==old->status.boot_id&&link==ident<model::LinkId>(7));return true;};
 assert(fence_replaced_links(*current)&&old->cancelled&&!old->ready&&!current->cancelled&&calls==1);assert(notify_link_closed(*old)&&calls==1);reap_closed_links();assert(links.size()==1);
 reset();old=connection();current=connection(9,18);links={old,current};bool fenced=false;controls.disconnected=[&](auto,auto,auto){return fenced;};assert(!fence_replaced_links(*current));reap_closed_links();assert(links.size()==2);fenced=true;assert(fence_replaced_links(*current));reap_closed_links();assert(links.size()==1);
 // New-link readiness waits for completion of the old owner fence.
 reset();old=connection();current=connection(9,18);links={old,current};std::mutex gate;std::condition_variable cv;bool entered=false,finish=false;calls=0;
 controls.disconnected=[&](auto,auto,auto){std::unique_lock<std::mutex> guard(gate);++calls;entered=true;cv.notify_all();cv.wait(guard,[&]{return finish;});return true;};
 auto closing=std::async(std::launch::async,[&]{return notify_link_closed(*old);});{std::unique_lock<std::mutex> guard(gate);cv.wait(guard,[&]{return entered;});}
 auto replacement=std::async(std::launch::async,[&]{return fence_replaced_links(*current);});assert(replacement.wait_for(std::chrono::milliseconds(30))==std::future_status::timeout);{std::lock_guard<std::mutex> guard(gate);finish=true;}cv.notify_all();assert(closing.get()&&replacement.get()&&calls==1);
}
void duplicate_connections(){
 for(bool ready:{false,true}){
  reset();auto old=connection(),duplicate=connection(9);old->ready=ready;old->status.ready=ready;links={old,duplicate};unsigned fences=0;
  const auto original=old->session.scope();HelloFrame hello;hello.consortium_id=original.consortium_id;hello.octopus_id=original.peer_id;hello.boot_id=original.peer_boot_id;hello.coordinator_epoch=1;hello.required_capabilities=known_capabilities;
  assert(old->session.hello(hello,known_capabilities)==Error::none);if(ready)assert(old->session.synchronized(1,true,true)==Error::none);
  const auto state=old->session.state();
  old->refreshing=!ready;old->queue.push_back({Kind::device_status,"{\"hostname\":\"canonical-work\"}",1000,clock_ms,1,{}});
  controls.disconnected=[&](auto,auto,auto){++fences;old->queue.clear();old->context_ready=false;return true;};
  assert(!fence_replaced_links(*duplicate)&&duplicate->cancelled&&!old->cancelled&&old->ready==ready&&old->session.scope()==original&&old->session.state()==state&&fences==0);
  // Rejection is followed by the real worker/reaper close path. That path
  // must not publish a disconnect for an unclaimed competing connection.
  assert(notify_link_closed(*duplicate)&&duplicate->closed_notified&&fences==0);
  reap_closed_links();
  assert(links.size()==1&&links.front()==old&&fences==0&&!old->cancelled&&old->ready==ready&&old->status.ready==ready
      &&old->session.scope()==original&&old->session.state()==state&&old->refreshing==!ready
      &&old->context_ready&&old->queue.size()==1&&old->queue.front().payload=="{\"hostname\":\"canonical-work\"}");
 }
 // Whichever authenticated Hello claims the peer first owns that boot. Its
 // competitor cannot cancel it, including before Ready/status publication.
 reset();auto one=connection(),two=connection(9);one->claimed_boot=two->claimed_boot={};links={one,two};
 auto first=std::async(std::launch::async,[&]{return fence_replaced_links(*one);});
 auto second=std::async(std::launch::async,[&]{return fence_replaced_links(*two);});
 const bool first_owned=first.get(),second_owned=second.get();
 assert(first_owned!=second_owned&&one->cancelled!=two->cancelled);
 // A known transport failure need not wait for the old worker's cancelled
 // publication before a same-boot replacement may claim ownership.
 reset();auto broken=connection(),recovered=connection(9);broken->tls->healthy=false;links={broken};unsigned failures_fenced=0;
 controls.disconnected=[&](auto owner,auto boot,auto link){++failures_fenced;assert(owner==broken->peer&&boot==broken->claimed_boot&&link==broken->session.scope().link_id);return true;};
 assert(!broken->cancelled&&!connection_available(broken->peer));
 links.push_back(recovered);assert(fence_replaced_links(*recovered)&&broken->cancelled&&!recovered->cancelled&&failures_fenced==1);
 // A stale credential is not a reusable connection, even in the same boot.
 reset();auto old=connection(),replacement=connection(9);group.peers.front().pin="replacement-pin";replacement->tls->allowed.front().pin="replacement-pin";links={old,replacement};unsigned fences=0;
 controls.disconnected=[&](auto owner,auto boot,auto link){++fences;assert(owner==old->peer&&boot==old->claimed_boot&&link==old->session.scope().link_id);return true;};
 assert(fence_replaced_links(*replacement)&&old->cancelled&&!replacement->cancelled&&fences==1);
}
void unclaimed_disconnects(){
 for(bool has_hello_scope:{false,true}){
  reset();auto old=connection();auto failed=has_hello_scope?connection(9):std::make_shared<Active>();
  failed->claimed_boot={};failed->cancelled=true;links={old,failed};unsigned calls=0;
  const auto original=old->session.scope();
  controls.disconnected=[&](auto,auto,auto){++calls;old->context_ready=false;return true;};
  assert(notify_link_closed(*failed)&&failed->closed_notified&&calls==0);
  reap_closed_links();assert(links.size()==1&&links.front()==old&&calls==0&&!old->cancelled&&old->context_ready&&old->session.scope()==original);
 }
}
void connection_launch(){
 reset();const auto expected=ident<model::OctopusId>(3);auto first_tls=std::make_shared<Tls>(),duplicate_tls=std::make_shared<Tls>();
 auto first=std::async(std::launch::async,[&]{launch(first_tls,"192.0.2.1",expected);});
 auto second=std::async(std::launch::async,[&]{launch(duplicate_tls,"192.0.2.2",expected);});first.get();second.get();
 assert(links.size()==1&&task_arguments.size()==1&&task_attempts==1&&connection_available(expected));
 const auto canonical=links.front();assert(!canonical->cancelled&&canonical->connection_peer==expected&&!canonical->peer);
 launch(std::make_shared<Tls>(),"192.0.2.3",expected);
 assert(links.size()==1&&task_arguments.size()==1&&task_attempts==1&&links.front()==canonical);
 canonical->peer=expected;canonical->connection_peer={};canonical->ready=false;
 launch(std::make_shared<Tls>(),"192.0.2.4",expected);
 assert(links.size()==1&&task_attempts==1&&!canonical->cancelled);
 // A failure reported by TLS releases acquisition immediately, even
 // before the owning worker has set cancelled.
 reset();launch(std::make_shared<Tls>(),"192.0.2.1",expected);const auto broken=links.front();broken->tls->healthy=false;
 assert(!broken->cancelled&&!connection_available(expected));
 launch(std::make_shared<Tls>(),"192.0.2.2",expected);
 assert(links.size()==2&&task_arguments.size()==2&&task_attempts==2&&!broken->cancelled&&connection_available(expected));
 // A cancelled/dead connection no longer owns acquisition, and worker
 // creation failure removes its reservation so a later caller may try.
 reset();task_creation_fails=true;launch(std::make_shared<Tls>(),"192.0.2.1",expected);
 assert(links.empty()&&!connection_available(expected)&&task_attempts==1&&task_arguments.empty());
 task_creation_fails=false;launch(std::make_shared<Tls>(),"192.0.2.1",expected);
 assert(links.size()==1&&connection_available(expected)&&task_attempts==2);
 links.front()->cancelled=true;assert(!connection_available(expected));
 launch(std::make_shared<Tls>(),"192.0.2.2",expected);
 assert(links.size()==2&&task_arguments.size()==2&&connection_available(expected));
 reset();
 for(unsigned peer=3;peer<6;++peer){
  const auto owner=ident<model::OctopusId>(peer);launch(std::make_shared<Tls>(),"192.0.2.1",owner);
  assert(connection_available(owner));
 }
 assert(links.size()==3&&task_arguments.size()==3&&task_attempts==3);
 launch(std::make_shared<Tls>(),"192.0.2.1",ident<model::OctopusId>(6));
 assert(links.size()==3&&task_arguments.size()==3&&task_attempts==3&&!connection_available(ident<model::OctopusId>(6)));
 reset();
}
void queues(){
 reset();auto a=connection();links={a};auto i=down();
 auto foreign=i;foreign.target.port.consortium_id=ident<model::ConsortiumId>(99);assert(runtime_send_owned(foreign)==ESP_ERR_INVALID_ARG&&a->queue.empty());
 assert(runtime_send_control(a->peer,Kind::device_status,"{\"hostname\":\"first\"}",1000)==ESP_OK);assert(runtime_send_owned(i)==ESP_OK);i.edges[0].down=false;assert(runtime_send_owned(i)==ESP_OK);
 assert(a->queue.size()==3&&a->queue[0].payload=="{\"hostname\":\"first\"}"&&a->queue[1].kind==Kind::scoped_intent&&queued_cleanup(a->queue[2])); // strict causal FIFO, no cleanup priority reordering
 reset();a=connection();links={a};for(unsigned n=0;n<15;++n)assert(runtime_send_control(a->peer,Kind::device_status,"{\"hostname\":\"amity-main\"}",1000)==ESP_OK);assert(runtime_send_control(a->peer,Kind::device_status,"{\"hostname\":\"amity-main\"}",1000)==ESP_ERR_NO_MEM);i=down();i.edges[0].down=false;assert(runtime_send_owned(i)==ESP_OK&&a->queue.size()==16);assert(runtime_send_owned(i)==ESP_ERR_NO_MEM);logical_restart_pending(*a);assert(a->queue.empty());
 reset();a=connection();links={a};i=down();i.edges[0].down=false;assert(runtime_send_owned(i)==ESP_OK);clock_ms+=2000;assert(runtime_send_control(a->peer,Kind::device_status,"{\"hostname\":\"amity-main\"}",1000)==ESP_ERR_INVALID_STATE);logical_restart_pending(*a);assert(diagnostic_logs.size()==1&&diagnostic_logs[0].find("reason=causal_expired phase=queue_admission")!=std::string::npos&&diagnostic_logs[0].find("kind=scoped_intent age_ms=2000 ttl_ms=2000")!=std::string::npos);
 reset();a=connection();links={a};i=down();i.kind=model::OwnedIntentKind::activity;assert(runtime_send_owned(i)==ESP_OK);clock_ms+=1000;assert(runtime_send_control(a->peer,Kind::device_status,"{\"hostname\":\"amity-main\"}",1000)==ESP_ERR_INVALID_STATE);logical_restart_pending(*a);
 reset();a=connection();links={a};model::OwnedAdapterSnapshot report;report.owner=a->session.scope().local_id;report.boot=a->session.scope().local_boot_id;
 const auto result="{\"snapshot\":"+owned_snapshot_json(report)+"}";
 assert(runtime_send_owned_result(a->peer,ident<model::LinkId>(99),result)==ESP_ERR_INVALID_STATE&&a->queue.empty());
 for(unsigned n=0;n<14;++n)assert(runtime_send_control(a->peer,Kind::device_status,"{\"hostname\":\"amity-main\"}",1000)==ESP_OK);
 assert(runtime_send_owned_result(a->peer,a->session.scope().link_id,result)==ESP_OK&&a->queue.size()==15);
 assert(runtime_send_owned_result(a->peer,a->session.scope().link_id,result)==ESP_ERR_NO_MEM&&!a->restart_requested&&diagnostic_logs.empty());a->queue.clear();
 reset();a=connection();links={a};assert(runtime_send_control(a->peer,Kind::device_status,"{\"hostname\":\"amity-main\"}",100)==ESP_OK);clock_ms+=100;assert(runtime_send_control(a->peer,Kind::device_status,"{\"hostname\":\"amity-main\"}",100)==ESP_OK&&a->queue.size()==1);assert(runtime_send_control(a->peer,static_cast<Kind>(21),"{}",1000)==ESP_ERR_NOT_SUPPORTED);
 // A quiet authenticated stream needs no periodic permission before fresh input.
 reset();a=connection();links={a};
 const auto scope=a->session.scope();HelloFrame hello;hello.consortium_id=scope.consortium_id;hello.octopus_id=scope.peer_id;hello.boot_id=scope.peer_boot_id;hello.coordinator_epoch=1;hello.required_capabilities=known_capabilities;
 assert(a->session.hello(hello,known_capabilities)==Error::none&&a->session.synchronized(1,true,true)==Error::none);
 clock_ms+=60000;assert(runtime_send_owned(down())==ESP_OK&&a->queue.size()==1&&a->queue.front().ttl==500);
}
void initial_context(){
 reset();auto a=connection();links={a};a->context_ready=false;
 auto i=down();assert(runtime_send_owned(i)==ESP_ERR_INVALID_STATE&&a->queue.empty());
 i.kind=model::OwnedIntentKind::activity;assert(runtime_send_owned(i)==ESP_OK&&a->queue.size()==1&&a->context_ready);a->queue.clear();a->context_ready=false;i=down();
 unsigned refreshes=0;controls.ready=[&](auto owner,auto,auto){++refreshes;assert(owner==a->peer&&!a->context_ready);assert(runtime_send_owned(down())==ESP_ERR_INVALID_STATE);return true;};
 // True means the recapture entered owner-task IPC, not that it has run.
 assert(initialize_context(*a)&&initialize_context(*a)&&refreshes==1&&!a->context_ready&&a->context_requested&&a->queue.empty());
 assert(runtime_send_owned(down())==ESP_ERR_INVALID_STATE);
 i.kind=model::OwnedIntentKind::context;i.target.osd_name.clear();assert(runtime_send_owned(i)==ESP_OK&&a->queue.size()==1);
 assert(a->context_ready&&!a->context_requested&&runtime_send_owned(down())==ESP_OK&&a->queue.size()==2);
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
 auto up=down();up.edges[0].down=false;assert(runtime_send_owned(up)==ESP_OK&&!a->context_ready);
 assert(runtime_send_owned(down())==ESP_ERR_INVALID_STATE);
}
void future_context_is_recaptured(){
 reset();auto a=connection();links={a};configuration=2;
 auto current=down();current.kind=model::OwnedIntentKind::context;current.target.osd_name.clear();current.config_revision=2;current.generation=8;
 assert(runtime_send_owned(current)==ESP_OK&&a->queue.empty()&&a->context_ready&&!a->cancelled);
 auto action=down();action.config_revision=2;action.generation=8;
 assert(runtime_send_owned(action)==ESP_ERR_INVALID_STATE&&a->queue.empty()&&!a->cancelled);
 action.kind=model::OwnedIntentKind::key_edges;assert(runtime_send_owned(action)==ESP_ERR_INVALID_STATE&&a->queue.empty());
 // The exchange may take longer than the ordinary action TTL. No old payload
 // or deadline is retained, so configuration alone cannot create or expire an admitted action.
 clock_ms+=1150;assert(a->queue.empty()&&!a->cancelled);
 const auto scope=a->session.scope();HelloFrame hello;hello.consortium_id=scope.consortium_id;hello.octopus_id=scope.peer_id;hello.boot_id=scope.peer_boot_id;hello.coordinator_epoch=1;hello.config_revision=1;hello.required_capabilities=known_capabilities;
 assert(a->session.hello(hello,known_capabilities)==Error::none&&a->session.synchronized(1,true,true)==Error::none);
 assert(a->session.advance_configuration(2)==Error::none);a->status.config_revision=2;a->context_ready=false;a->refreshing=true;a->context_requested=false;
 unsigned requested=0;controls.ready=[&](auto,auto,auto){++requested;return true;};assert(initialize_context(*a)&&requested==1&&!a->context_ready&&a->context_requested);
 action.kind=model::OwnedIntentKind::key_edges;assert(runtime_send_owned(action)==ESP_ERR_INVALID_STATE&&a->queue.empty());
 // Recapture reads the current activity now, with a newer generation.
 current.generation=9;current.selected=true;assert(runtime_send_owned(current)==ESP_OK&&a->context_ready&&!a->refreshing&&a->queue.size()==1);
 assert(a->queue[0].owned->generation==9&&a->queue[0].created==clock_ms&&a->queue[0].revision==2);
 action.generation=9;assert(runtime_send_owned(action)==ESP_OK&&a->queue.size()==2&&a->queue[1].owned->generation==9);
 // Once ordinary work is admitted, its original TTL and fence remain intact.
 a->queue.pop_front();clock_ms+=1000;assert(runtime_send_control(a->peer,Kind::device_status,"{\"hostname\":\"amity-main\"}",1000)==ESP_ERR_INVALID_STATE);logical_restart_pending(*a);
 assert(diagnostic_logs.size()==1&&diagnostic_logs[0].find("reason=causal_expired")!=std::string::npos);
}
void asynchronous_context_failure_is_scoped(){
 reset();auto a=connection();links={a};group.consortium=a->session.scope().consortium_id;a->context_ready=false;
 std::function<void(model::OwnedContextResult)> done;
 controls.ready=[&](auto owner,uint64_t revision,auto completed){assert(owner==a->peer&&revision==1);done=std::move(completed);return true;};
 assert(initialize_context(*a)&&a->context_requested&&!a->context_ready);
 clock_ms=5101;auto context=down();context.kind=model::OwnedIntentKind::context;context.target.osd_name.clear();
 // Failure of the captured context is scoped independently of elapsed wall time.
 assert(a->queue.empty()&&!a->cancelled);
 done(model::OwnedContextResult::failed);logical_restart_pending(*a);assert(!a->context_requested&&!a->context_ready&&diagnostic_logs.size()==1);
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
 context=down();context.kind=model::OwnedIntentKind::context;context.target.osd_name.clear();context.config_revision=2;assert(runtime_send_owned(context)==ESP_OK);newer(model::OwnedContextResult::published);
 assert(a->context_ready&&!a->context_requested&&!a->cancelled&&a->queue.size()==1);
 // Nor can an old callback fence a replacement authenticated connection.
 links.clear();auto replacement=connection(9);links={replacement};done(model::OwnedContextResult::failed);assert(!replacement->cancelled&&diagnostic_logs.empty());
}
void asynchronous_context_ignores_ended_logical_session(){
 reset();configuration=1;auto a=connection();links={a};group.consortium=a->session.scope().consortium_id;a->context_ready=false;
 const auto original=a->session.scope();const auto transport=a->tls;
 HelloFrame hello;hello.consortium_id=original.consortium_id;hello.octopus_id=original.peer_id;hello.boot_id=original.peer_boot_id;hello.coordinator_epoch=1;hello.config_revision=1;hello.required_capabilities=known_capabilities;
 assert(a->session.hello(hello,known_capabilities)==Error::none&&a->session.synchronized(1,true,true)==Error::none);
 std::function<void(model::OwnedContextResult)> old_completed,new_completed;
 controls.ready=[&](auto owner,uint64_t revision,auto completed){assert(owner==a->peer&&revision==1);old_completed=std::move(completed);return true;};
 assert(initialize_context(*a)&&a->context_requested&&!a->context_ready);
 const auto new_link=ident<model::LinkId>(91);
 {std::lock_guard<std::mutex> guard(lock);std::lock_guard<std::mutex> queue(a->mutex);
  assert(a->session.begin_synchronization(1,new_link)==Error::none);a->ready=false;a->context_requested=false;a->refreshing=true;
 }
 assert(a->session.synchronized(1,true,true)==Error::none);a->ready=true;
 controls.ready=[&](auto owner,uint64_t revision,auto completed){assert(owner==a->peer&&revision==1);new_completed=std::move(completed);return true;};
 assert(initialize_context(*a)&&a->context_requested&&!a->context_ready);
 old_completed(model::OwnedContextResult::failed);old_completed(model::OwnedContextResult::published);
 assert(!a->cancelled&&a->context_requested&&!a->context_ready&&a->refreshing&&a->tls==transport&&diagnostic_logs.empty());
 runtime_fence_owned_link(a->peer,original.link_id);assert(!a->cancelled&&a->tls==transport&&diagnostic_logs.empty());
 new_completed(model::OwnedContextResult::published);assert(!a->refreshing&&!a->cancelled&&!a->context_ready&&a->context_requested);
 // Cleanup authority also belongs to the exact logical link, even with the
 // same peer boots, configuration and TLS object on both sides of restart.
 group.role=Group::member;group.authority=a->peer;boot=original.local_boot_id;configuration=1;
 model::OwnedAuthorization old_auth{a->peer,original.peer_boot_id,boot,original.link_id,1,1000};old_auth.authority=model::OwnedAuthority::cleanup;
 auto current_auth=old_auth;current_auth.link_id=new_link;
 assert(!runtime_owned_authorized(old_auth)&&runtime_owned_deadline(current_auth)==UINT64_MAX);
}
void diagnostic_lines_fit_capture(){
 for(bool retained:{false,true}){
  reset();auto a=connection();LinkTrace trace;trace.phase="configuration_receive";trace.fail("application_restart_requested",Error::unsupported_kind);trace.retained=retained;trace.has_kind=trace.has_queue=true;trace.kind=Kind::scoped_intent;trace.created=0;trace.ttl=UINT32_MAX;trace.queued_revision=UINT64_MAX;trace.action_code=INT32_MIN;trace.socket_error=INT32_MIN;
  auto captured=snapshot_link_trace(*a,trace,UINT64_MAX);captured.scope.config_revision=UINT64_MAX;emit_link_trace(captured);assert(diagnostic_logs.size()==1&&diagnostic_logs[0].size()<384&&diagnostic_logs[0].find("qrev=18446744073709551615")!=std::string::npos);
 }
}
void release_sender_timing(){
 for(bool success:{false,true}){
  reset();auto a=connection();links={a};auto up=down();up.edges[0].down=false;
  controls.configuration_revision=[]{clock_ms+=125;return uint64_t{1};};
  assert(runtime_send_owned(up)==ESP_OK&&a->queue.size()==1&&a->queue.front().created==225&&a->queue.front().ttl==2000);
  assert(key_release_logs.size()==1&&key_release_logs[0].find("stage=admit owner="+a->peer.str()+" key=65 generation=2 elapsed_ms=125 result=0")!=std::string::npos);
  clock_ms+=300;before_send=[]{clock_ms+=450;};write_result=success;
  assert(pump(*a)==success&&a->queue.empty()&&writes==1&&written_ttl==1700);
  Json payload;JsonError error;model::OwnedIntent delivered;
  assert(parse_json(written_payload,payload,error)&&parse_owned_intent(payload,delivered)==Error::none&&delivered.edge_count==1&&!delivered.edges[0].down&&delivered.edges[0].key==65&&delivered.generation==2);
  assert(key_release_logs.size()==2&&key_release_logs[1].find("stage=write owner="+a->peer.str()+" key=65 generation=2 age_ms=300 elapsed_ms=450 sent="+(success?"1":"0"))!=std::string::npos);
  assert(!a->restart_requested&&!a->cancelled&&a->tls->usable()&&shutdowns==0);
  if(success)assert(diagnostic_logs.empty());
  else assert(diagnostic_logs.size()==1&&diagnostic_logs[0].find("reason=queued_send")!=std::string::npos);
 }
 reset();auto a=connection();links={a};auto up=down();up.edges[0].down=false;status.authority_ready=false;
 assert(runtime_send_owned(up)==ESP_ERR_INVALID_STATE&&a->queue.empty()&&key_release_logs.size()==1&&key_release_logs[0].find("elapsed_ms=0 result=2")!=std::string::npos&&writes==0);
 reset();a=connection();links={a};assert(runtime_send_owned(down())==ESP_OK&&key_release_logs.empty());
 auto tap=down();tap.edge_count=2;tap.edges[1]={tap.edges[0].key,false};assert(runtime_send_owned(tap)==ESP_OK&&key_release_logs.empty());
 reset();a=connection();links={a};up=down();up.edges[0].down=false;up.generation=UINT64_MAX;clock_ms=UINT64_MAX-1;
 assert(runtime_send_owned(up)==ESP_OK&&key_release_logs.size()==1&&key_release_logs[0].size()<384&&key_release_logs[0].find("generation=18446744073709551615")!=std::string::npos);
}
void restart_flags_gate_front_doors_before_scope_rotation(){
 for(bool pending:{false,true}){
  reset();auto a=connection();links={a};const auto original=a->session.scope();const auto transport=a->tls;
  a->execution_fenced=!pending;a->restart_requested=pending;
  assert(a->ready); // Explicit flags fence any transiently retained ready view.
  assert(runtime_send_control(a->peer,Kind::device_status,"{\"hostname\":\"amity-main\"}",1000)==ESP_ERR_INVALID_STATE);
  assert(runtime_send_owned(down())==ESP_ERR_INVALID_STATE);
  model::OwnedAdapterSnapshot report;report.owner=original.local_id;report.boot=original.local_boot_id;
  assert(runtime_send_owned_result(a->peer,original.link_id,"{\"snapshot\":"+owned_snapshot_json(report)+"}",1000,1)==ESP_ERR_INVALID_STATE);
  unsigned captures=0;a->context_ready=false;controls.ready=[&](auto,auto,auto){++captures;return true;};
  assert(!initialize_context(*a)&&captures==0&&a->queue.empty());
  group.role=Group::member;group.authority=a->peer;boot=original.local_boot_id;
  model::OwnedAuthorization auth{a->peer,original.peer_boot_id,boot,original.link_id,1,5000};auth.authority=model::OwnedAuthority::cleanup;
  assert(!runtime_owned_authorized(auth));
  assert(a->session.scope()==original&&a->tls==transport&&a->tls->usable()&&!a->cancelled&&shutdowns==0&&diagnostic_logs.empty());
 }
}
void authorization(){
 reset();auto a=connection();links={a};
 // Authenticated current membership, boot, link and configuration are authority.
 // Time alone cannot revoke an unchanged stream; each message retains its TTL.
 group.role=Group::member;group.authority=a->peer;boot=ident<model::BootId>(4);configuration=1;
 const auto scope=a->session.scope();
 model::OwnedAuthorization auth{a->peer,scope.peer_boot_id,boot,scope.link_id,1,200};
 for(auto kind:{model::OwnedAuthority::input,model::OwnedAuthority::control,model::OwnedAuthority::cleanup}){
  auth.authority=kind;assert(runtime_owned_deadline(auth)==UINT64_MAX);clock_ms+=60000;assert(runtime_owned_authorized(auth));
 }
 auth.link_id=ident<model::LinkId>(99);assert(!runtime_owned_authorized(auth));auth.link_id=scope.link_id;
 auth.controller_boot=ident<model::BootId>(99);assert(!runtime_owned_authorized(auth));auth.controller_boot=scope.peer_boot_id;
 auth.owner_boot=ident<model::BootId>(99);assert(!runtime_owned_authorized(auth));auth.owner_boot=boot;
 auth.config_revision=2;assert(!runtime_owned_authorized(auth));auth.config_revision=1;
 group.authority=ident<model::OctopusId>(99);assert(!runtime_owned_authorized(auth));group.authority=a->peer;
 runtime_fence_owned_link(a->peer,ident<model::LinkId>(99));assert(!a->cancelled);
 runtime_fence_owned_link(a->peer,scope.link_id);logical_restart_pending(*a);assert(!runtime_owned_authorized(auth));assert(diagnostic_logs.size()==1&&diagnostic_logs[0].find("reason=causal_result_fence")!=std::string::npos);runtime_fence_owned_link(a->peer,scope.link_id);assert(diagnostic_logs.size()==1);
 reset();identity.id=group.authority=ident<model::OctopusId>(2);boot=ident<model::BootId>(4);
 auth={identity.id,boot,boot,{},1,0};assert(runtime_owned_deadline(auth)==UINT64_MAX);
}
void codec_and_session(){
 auto input=down();auto text=owned_intent_json(input);assert(!text.empty());Json value;JsonError error;assert(parse_json(text,value,error));model::OwnedIntent parsed;
 assert(parse_owned_intent(value,parsed)==Error::none&&parsed.kind==model::OwnedIntentKind::key_edges&&parsed.edge_count==1&&parsed.edges[0].key==0x41&&parsed.edges[0].down&&!value.get("target"));
 assert(!value.get("execution_lease_id")&&!value.get("sequence")&&!value.get("setup_generation")&&!value.get("arguments")->get("hold_token")&&!value.get("arguments")->get("press_sequence"));
 for(const auto* action:{"key_down","key_up"}){auto obsolete=value;obsolete.get("action")->string=action;assert(parse_owned_intent(obsolete,parsed)==Error::invalid_message);}
 auto bad=value;bad.object.emplace_back("enabled",Json{});assert(parse_owned_intent(bad,parsed)==Error::invalid_message);bad=value;bad.object.emplace_back("target",Json{});assert(parse_owned_intent(bad,parsed)==Error::invalid_message);bad=value;bad.get("arguments")->object.emplace_back("hold_lease_ms",Json{});assert(parse_owned_intent(bad,parsed)==Error::invalid_message);
 Json old_permission;old_permission.type=Json::Type::string;old_permission.string=std::string(32,'1');bad=value;bad.object.emplace_back("execution_lease_id",old_permission);assert(parse_owned_intent(bad,parsed)==Error::invalid_message);
 bad=value;old_permission.string="1";bad.object.emplace_back("sequence",old_permission);assert(parse_owned_intent(bad,parsed)==Error::invalid_message);
 input.target.osd_name.clear();assert(!owned_intent_json(input).empty());input=down();assert(!owned_intent_json(input).empty());input.kind=model::OwnedIntentKind::activity;assert(!owned_intent_json(input).empty());
 input=down();input.edge_count=2;input.edges[1]={input.edges[0].key,false};text=owned_intent_json(input);assert(!text.empty()&&parse_json(text,bad,error)&&parse_owned_intent(bad,parsed)==Error::none&&parsed.edge_count==2&&!parsed.edges[1].down&&!bad.get("target"));
 input.edges[1].key=0x42;assert(owned_intent_json(input).empty());input=down();
 LinkScope scope{ident<model::LinkId>(7),ident<model::ConsortiumId>(1),ident<model::OctopusId>(3),ident<model::OctopusId>(2),ident<model::OctopusId>(2),ident<model::BootId>(8),ident<model::BootId>(4),1,1};
 Session session;assert(session.authenticate(scope,scope.peer_id,PeerRole::coordinator,true)==Error::none);HelloFrame h;h.consortium_id=scope.consortium_id;h.octopus_id=scope.peer_id;h.boot_id=scope.peer_boot_id;h.coordinator_epoch=1;h.required_capabilities=known_capabilities;assert(session.hello(h,known_capabilities)==Error::none&&session.synchronized(1,true,true)==Error::none);
 Envelope e;e.consortium_id=scope.consortium_id;e.origin_octopus_id=scope.peer_id;e.destination_octopus_id=scope.local_id;e.origin_boot_id=scope.peer_boot_id;e.destination_boot_id=scope.local_boot_id;e.coordinator_epoch=e.config_revision=1;e.kind=Kind::scoped_intent;e.ttl_ms=500;e.payload=value;
 assert(session.admit(e,100,100)==Error::none);
 input=down();text=owned_intent_json(input);assert(parse_json(text,e.payload,error)&&session.admit(e,60000,60000)==Error::none); // Fresh input after long idle needs no grant.
 assert(session.admit(e,60000,60500)==Error::invalid_message); // Its original 500 ms TTL still expires.
 input=down();input.edges[0].down=false;text=owned_intent_json(input);assert(parse_json(text,e.payload,error));assert(parse_owned_intent(e.payload,parsed)==Error::none&&!e.payload.get("arguments")->get("press_sequence"));
 assert(session.admit(e,60600,60600)==Error::none); // Ordered Up carries button state only, without another permission.

 for(auto kind:{model::OwnedIntentKind::activity,model::OwnedIntentKind::context,model::OwnedIntentKind::discover}){
  auto control=down();control.kind=kind;control.selected=true;control.source=model::QualifiedDeviceName{{ident<model::ConsortiumId>(1),ident<model::OctopusId>(3),0,model::Side::back},"Player"};text=owned_intent_json(control);Json payload;
  assert(!text.empty()&&parse_json(text,payload,error)&&!payload.get("execution_lease_id")&&!payload.get("setup_generation")&&parse_owned_intent(payload,parsed)==Error::none);
  assert(!payload.get("sequence")&&!payload.get("activity_step"));
  auto command=e;command.payload=payload;
  assert(session.admit(command,120000,120000)==Error::none);
 }
 for(auto kind:{model::OwnedIntentKind::power,model::OwnedIntentKind::source_input,model::OwnedIntentKind::switch_input,
               model::OwnedIntentKind::query_audio,model::OwnedIntentKind::standby_wire,model::OwnedIntentKind::announce_active_source}){
  auto local=down();local.kind=kind;assert(owned_intent_json(local).empty()); // CEC actions never cross owners.
 }
 e.origin_boot_id=ident<model::BootId>(99);assert(session.admit(e,60600,60600)==Error::stale_boot);e.origin_boot_id=scope.peer_boot_id;e.config_revision=2;assert(session.admit(e,60600,60600)==Error::stale_config);
 model::OwnedAdapterSnapshot report;report.owner=scope.local_id;report.boot=scope.local_boot_id;report.devices.push_back({model::Side::back,"Player",4,4,0x1000,1,true});text=owned_snapshot_json(report);assert(!text.empty()&&parse_json(text,value,error));model::OwnedAdapterSnapshot decoded;assert(parse_owned_snapshot(value,decoded)&&decoded.devices.front().osd_name=="Player");value.get("devices")->array.push_back(value.get("devices")->array.front());assert(!parse_owned_snapshot(value,decoded));
}

}
int main(){using namespace amity::consortium;closure();duplicate_connections();unclaimed_disconnects();connection_launch();queues();initial_context();future_context_is_recaptured();asynchronous_context_failure_is_scoped();asynchronous_context_ignores_ended_logical_session();diagnostic_lines_fit_capture();release_sender_timing();restart_flags_gate_front_doors_before_scope_rotation();authorization();codec_and_session();std::cout<<"PASS qualified transport: actual FIFO/key-up capacity, release sender timing outside locks, link loss/replacement fences, scoped codecs, stale boots/config, original message TTL and ordered button state without periodic grants\\n";}
'''
def main():
 source=(ROOT/'components/consortium/runtime.cpp').read_text()
 names=['bool connection_available(','bool current_peer_credential(','bool initialize_context(','bool notify_link_closed(','bool fence_replaced_links(','void reap_closed_links(','static bool queued_cleanup(const Queued& queued)','static bool queued_causal(','static esp_err_t queue_control(','esp_err_t runtime_send_control(','esp_err_t runtime_send_owned_result(','void runtime_fence_owned_link(','esp_err_t runtime_send_owned(','uint64_t runtime_owned_deadline(','bool runtime_owned_authorized(']
 trace=source[source.index('struct LinkTrace {'):source.index('static bool queued_cleanup(')]
 retries=source[source.index('std::optional<RuntimeSnapshotRetry> snapshot_retry('):source.index('enum class QueuedState')]
 diagnostics=source[source.index('struct LinkTraceSnapshot {'):source.index('std::optional<RuntimeSnapshotRetry> snapshot_retry(')]
 launch_start=source.index('void launch(');launch_body=source.index('){',launch_start)+1
 actual=trace+function(source,'void request_session_restart(')+diagnostics+retries+''.join(function(source,name) for name in names)+source[launch_start:launch_body]+function(source[launch_body:],'{')
 actual+=function(source,'enum class QueuedState').rstrip()+';\n'+function(source,'QueuedState prepare_queued(')
 queue_start=source.index('\n',source.index('        if(!active->retiring&&!refresh_local_hostname(*active))'))+1
 queue_end=source.index('        fd_set reads;',queue_start)
 actual+='\nbool pump(Active& storage){auto* active=&storage;LinkTrace trace;trace.phase="operational";while(true){'+source[queue_start:queue_end]+'return true;}trace_link_closed(storage,trace,now_ms());return false;}\n'
 with tempfile.TemporaryDirectory(prefix='amity-qualified-transport-') as directory:
  temp=Path(directory);(temp/'esp_err.h').write_text('#pragma once\nusing esp_err_t=int;constexpr int ESP_OK=0,ESP_ERR_INVALID_ARG=1,ESP_ERR_INVALID_STATE=2,ESP_ERR_NO_MEM=3,ESP_ERR_TIMEOUT=4,ESP_FAIL=5,ESP_ERR_NOT_FOUND=6,ESP_ERR_NOT_SUPPORTED=7;\n')
  cpp=temp/'test.cpp';binary=temp/'test';cpp.write_text(PREFIX+'namespace amity::consortium {\n'+actual+'\n}\n'+SUITE)
  sources=[ROOT/'components/consortium'/name for name in ['json.cpp','wire.cpp','owned_intent.cpp','session.cpp','settings.cpp','update.cpp','transfer.cpp']]+[ROOT/'components/amity_system/model.cpp']
  subprocess.run(shlex.split(os.environ.get('CXX','c++'))+['-std=c++17','-Wall','-Wextra','-Werror','-pthread','-g','-fsanitize=address,undefined','-I'+str(temp)]+['-I'+str(ROOT/path) for path in ['components/consortium','components/consortium/include','components/amity_system/include']]+list(map(str,[cpp]+sources))+['-o',str(binary)],check=True)
  subprocess.run([str(binary)],check=True)
if __name__=='__main__':main()
