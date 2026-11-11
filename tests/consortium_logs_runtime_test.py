#!/usr/bin/env python3
"""Exercise actual bounded member log dispatch/pulls with real wire/session code."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PREFIX = r'''
#include "amity/consortium/runtime.hpp"
#include "controller_policy.hpp"
#include <algorithm>
#include <array>
#include <cassert>
#include <deque>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <vector>
#define EXT_RAM_BSS_ATTR
namespace amity::consortium {
using namespace platform;
uint64_t clock_ms=100;uint64_t now_ms(){return clock_ms;}
template<class T>T ident(unsigned n){char value[33];snprintf(value,sizeof(value),"%032x",n);return *T::parse(value);}
template<class T>T random_id(){static unsigned next=1000;return ident<T>(next++);}
struct Queued{Kind kind;std::string payload;uint32_t ttl;uint64_t created,revision;std::shared_ptr<const model::OwnedIntent> owned;};
struct FakeTls{model::OctopusId authenticated;bool healthy=true;};
struct Active{
 std::shared_ptr<FakeTls> tls=std::make_shared<FakeTls>();Session session;model::OctopusId peer;
 std::mutex mutex;std::deque<Queued> queue;RuntimePeerStatus status;
 bool cancelled=false,retiring=false,restart_requested=false,execution_fenced=false,ready=true,management_ready=false;
 LogSnapshot log_snapshot;model::MessageId log_transfer;size_t log_next=0;uint64_t log_expires=0;
};
std::mutex lock;Identity identity;Group group;RuntimeStatus status;
std::vector<std::shared_ptr<Active>> links;
bool current_peer_credential(const Active& active,const Group& current){for(const auto& peer:current.peers)if(peer.id==active.peer&&peer.id==active.tls->authenticated&&peer.active)return true;return false;}
bool settings_enrolled(model::OctopusId owner){for(const auto& peer:group.peers)if(peer.id==owner&&peer.active)return true;return false;}
std::vector<Json> sent;bool send_success=true;
bool send(Active&,Kind kind,std::string_view payload,uint32_t ttl){Json parsed;JsonError detail;assert(kind==Kind::log_result&&ttl==2000&&parse_json(payload,parsed,detail)&&valid_log_payload(kind,parsed));sent.push_back(std::move(parsed));return send_success;}
unsigned pdMS_TO_TICKS(unsigned ms){return ms;}
std::function<void()> tick;
void vTaskDelay(unsigned ms){clock_ms+=ms;if(tick)tick();}
}
'''
SUITE = r'''
namespace amity::consortium {
Json parsed(std::string_view body){Json value;JsonError error;assert(parse_json(body,value,error));return value;}
Envelope message(const std::shared_ptr<Active>& active,Kind kind,std::string_view payload){
 const auto& s=active->session.scope();Envelope e;e.consortium_id=s.consortium_id;e.origin_octopus_id=s.peer_id;e.destination_octopus_id=s.local_id;e.origin_boot_id=s.peer_boot_id;e.destination_boot_id=s.local_boot_id;e.coordinator_epoch=s.epoch;e.config_revision=s.config_revision;e.kind=kind;e.ttl_ms=2000;e.payload=parsed(payload);assert(valid_log_payload(kind,e.payload));return e;
}
bool dispatch(const std::shared_ptr<Active>& active,const Envelope& e){return active->session.admit(e,clock_ms,clock_ms)==Error::none&&log_dispatch(active,e,clock_ms);}
std::shared_ptr<Active> setup(bool main=true){
 clock_ms=100;links.clear();sent.clear();send_success=true;tick={};log_hooks={};log_pull={};group={};identity={};status={};
 identity.id=ident<model::OctopusId>(1);group.role=main?Group::coordinator:Group::member;group.authority=main?identity.id:ident<model::OctopusId>(2);group.epoch=1;group.consortium=ident<model::ConsortiumId>(3);status.authority_ready=main;
 auto a=std::make_shared<Active>();a->peer=ident<model::OctopusId>(2);a->tls->authenticated=a->peer;group.peers.push_back({a->peer,"leaf","pin",true});
 LinkScope scope{ident<model::LinkId>(4),group.consortium,identity.id,a->peer,group.authority,ident<model::BootId>(5),ident<model::BootId>(6),1,7};
 assert(a->session.authenticate(scope,a->peer,main?PeerRole::actuator:PeerRole::coordinator,true)==Error::none);
 HelloFrame h;h.consortium_id=scope.consortium_id;h.octopus_id=a->peer;h.boot_id=scope.peer_boot_id;h.coordinator_epoch=1;h.config_revision=7;h.required_capabilities=known_capabilities;
 assert(a->session.hello(h,known_capabilities)==Error::none&&a->session.synchronized(7,true,true)==Error::none);
 a->status.id=a->peer;a->status.boot_id=scope.peer_boot_id;a->status.config_revision=7;a->status.negotiated_capabilities=known_capabilities;links.push_back(a);return a;
}
std::shared_ptr<Active> add_member(unsigned number){
 auto a=std::make_shared<Active>();a->peer=ident<model::OctopusId>(number);a->tls->authenticated=a->peer;group.peers.push_back({a->peer,"leaf","pin",true});
 LinkScope scope{ident<model::LinkId>(100+number),group.consortium,identity.id,a->peer,group.authority,ident<model::BootId>(5),ident<model::BootId>(200+number),1,7};
 assert(a->session.authenticate(scope,a->peer,PeerRole::actuator,true)==Error::none);
 HelloFrame h;h.consortium_id=scope.consortium_id;h.octopus_id=a->peer;h.boot_id=scope.peer_boot_id;h.coordinator_epoch=1;h.config_revision=7;h.required_capabilities=known_capabilities;
 assert(a->session.hello(h,known_capabilities)==Error::none&&a->session.synchronized(7,true,true)==Error::none);
 a->status.id=a->peer;a->status.boot_id=scope.peer_boot_id;a->status.config_revision=7;a->status.negotiated_capabilities=known_capabilities;links.push_back(a);return a;
}
std::string decode(const Json& value){const auto& hex=value.get("data")->string;std::string result;for(size_t i=0;i<hex.size();i+=2){auto n=[](char c){return c<='9'?c-'0':c-'a'+10;};result+=char((n(hex[i])<<4)|n(hex[i+1]));}return result;}
std::string content(size_t size){std::string value;for(size_t i=0;i<size;++i)value+=char(i%256);return value;}
void receiver_snapshot_lifecycle(){
 auto a=setup(false);auto transfer=ident<model::MessageId>(10);std::string live=content(log_capacity);const auto expected=live;unsigned snapshots=0,released=0;
 log_hooks.snapshot=[&]{++snapshots;auto* data=new char[std::max<size_t>(live.size(),1)];std::copy(live.begin(),live.end(),data);return LogSnapshot{std::shared_ptr<const char>(data,[&](const char* bytes){++released;delete[] bytes;}),live.size()};};
 std::string received;
 for(size_t offset=0;offset<expected.size();offset+=log_chunk_bytes){
  assert(dispatch(a,message(a,Kind::log_request,log_request_json(transfer,offset))));
  assert(sent.back().get("error")->string.empty());received+=decode(sent.back());live.assign(log_capacity,'X');
  assert(snapshots==1&&released==(offset+log_chunk_bytes==expected.size()?1u:0u));
 }
 assert(received==expected&&sent.size()==24&&!a->log_snapshot&&!a->log_transfer&&!a->cancelled&&a->tls->healthy);
 // The next download gets a fresh snapshot and cancellation releases it.
 const auto next=ident<model::MessageId>(11);assert(dispatch(a,message(a,Kind::log_request,log_request_json(next,0))));assert(snapshots==2&&released==1);
 assert(dispatch(a,message(a,Kind::log_request,log_request_json(transfer,0,true)))&&a->log_snapshot);
 assert(dispatch(a,message(a,Kind::log_request,log_request_json(next,0,true)))&&!a->log_snapshot&&released==2);
 // Empty logs are a successful, empty final chunk with immediate release.
 live.clear();assert(dispatch(a,message(a,Kind::log_request,log_request_json(transfer,0))));assert(sent.back().get("total")->integer==0&&decode(sent.back()).empty()&&released==3);
 std::cout<<"PASS actual secondary: stable 24KiB snapshot while ring changes, exact binary chunks, final/cancel/empty release and healthy TLS\n";
}
void receiver_errors_and_expiry(){
 auto a=setup(false);auto first=ident<model::MessageId>(20),second=ident<model::MessageId>(21);unsigned releases=0;
 const auto body=content(2048);log_hooks.snapshot=[&]{auto* data=new char[body.size()];std::copy(body.begin(),body.end(),data);return LogSnapshot{std::shared_ptr<const char>(data,[&](const char* bytes){++releases;delete[] bytes;}),body.size()};};
 assert(dispatch(a,message(a,Kind::log_request,log_request_json(first,0)))&&a->log_snapshot);
 assert(dispatch(a,message(a,Kind::log_request,log_request_json(second,0)))&&sent.back().get("error")->string=="busy"&&a->log_transfer==first);
 assert(dispatch(a,message(a,Kind::log_request,log_request_json(first,17)))&&sent.back().get("error")->string=="expired"&&a->log_next==1024);
 clock_ms+=15000;assert(dispatch(a,message(a,Kind::log_request,log_request_json(first,1024)))&&sent.back().get("error")->string=="expired"&&!a->log_snapshot&&releases==1);
 log_hooks={};assert(dispatch(a,message(a,Kind::log_request,log_request_json(second,0)))&&sent.back().get("error")->string=="unavailable"&&!a->log_snapshot);
 assert(!a->cancelled&&a->tls->healthy);
 auto wrong=message(a,Kind::log_result,log_result_json(second,0,0,{}));assert(!dispatch(a,wrong));
 std::cout<<"PASS secondary busy/offset/expiry/unavailable return bounded errors without replacing TLS; role direction rejects\n";
}
using Responder=std::function<void(const std::shared_ptr<Active>&,const Json&)>;
void respond(const std::shared_ptr<Active>& a,const Json& request,const std::string& source,std::string_view error=""){
 auto transfer=*model::MessageId::parse(request.get("transfer_id")->string);const auto offset=static_cast<size_t>(request.get("offset")->integer);
 assert(offset<=source.size()||!error.empty());const auto data=error.empty()?std::string_view(source).substr(offset,std::min(log_chunk_bytes,source.size()-offset)):std::string_view{};
 assert(dispatch(a,message(a,Kind::log_result,log_result_json(transfer,offset,error.empty()?source.size():0,data,error))));
}
void pump(const std::shared_ptr<Active>& a,const Responder& response){
 if(a->queue.empty())return;const auto queued=std::move(a->queue.front());a->queue.pop_front();assert(queued.kind==Kind::log_request&&queued.ttl&&queued.ttl<=2000&&queued.revision==7);
 const auto request=parsed(queued.payload);assert(valid_log_payload(queued.kind,request));if(!request.get("close")->boolean)response(a,request);
}
void sender_exact_and_bounded(){
 auto a=setup();const auto transport=a->tls;const auto original=a->session.scope();const auto source=content(log_capacity);size_t calls=0;std::string received;
 tick=[&]{pump(a,[&](const auto& peer,const auto& request){++calls;respond(peer,request,source);});};
 assert(pull_logs(a->peer,[&](std::string_view bytes){assert(bytes.size()<=1024&&bytes.data()==log_chunk.data());received+=bytes;
  assert(pull_logs(a->peer,[](auto){assert(false);return true;})==ESP_ERR_INVALID_STATE);return true;})==ESP_OK);
 assert(received==source&&calls==24&&!log_pull.busy&&a->queue.size()==1&&parsed(a->queue.back().payload).get("close")->boolean);
 assert(a->tls==transport&&a->session.scope()==original&&!a->cancelled&&a->tls->healthy);
 // Three authenticated owners share the same single chunk storage on one main.
 a=setup();auto second=add_member(30),third=add_member(31);second->ready=false;second->management_ready=true;second->session.restrict_management();
 tick=[&]{for(const auto& peer:links)pump(peer,[&](const auto& recipient,const auto& request){respond(recipient,request,source);});};
 for(const auto& owner:{a,second,third}){
  received.clear();assert(pull_logs(owner->peer,[&](auto chunk){assert(chunk.data()==log_chunk.data());received+=chunk;return true;})==ESP_OK);
  assert(received==source&&sizeof(log_chunk)==1024&&!log_pull.busy&&links.size()==3);
  for(const auto& peer:links)assert(peer->queue.size()<=1&&!peer->log_snapshot&&!peer->cancelled&&peer->tls->healthy);
 }
 a=setup();tick=[&]{pump(a,[](const auto& peer,const auto& request){respond(peer,request,{});});};unsigned chunks=0;
 assert(pull_logs(a->peer,[&](auto){++chunks;return true;})==ESP_OK&&chunks==0);
 std::cout<<"PASS actual main: exact 24KiB/empty pulls, one fixed 1KiB consumer buffer, serialized collectors and management-ready TLS\n";
}
void sender_failures_and_cancellation(){
 auto a=setup();const auto body=content(4096);size_t chunks=0;
 tick=[&]{pump(a,[&](const auto& peer,const auto& request){respond(peer,request,body);});};
 assert(pull_logs(a->peer,[&](auto){++chunks;return false;})==ESP_FAIL&&chunks==1&&!log_pull.busy&&a->queue.size()==1&&parsed(a->queue.back().payload).get("close")->boolean);
 a=setup();clock_ms=500;assert(pull_logs(a->peer,[](auto){assert(false);return true;})==ESP_ERR_TIMEOUT&&clock_ms==2500&&!log_pull.busy&&a->queue.size()==1&&!a->cancelled&&a->tls->healthy);
 a=setup();tick=[&]{a->cancelled=true;};assert(pull_logs(a->peer,[](auto){assert(false);return true;})==ESP_ERR_INVALID_STATE&&!log_pull.busy&&a->queue.empty());
 for(const auto* error:{"busy","unavailable","expired"}){a=setup();tick=[&]{pump(a,[&](const auto& peer,const auto& request){respond(peer,request,{},error);});};const auto expected=std::string_view(error)=="expired"?ESP_ERR_TIMEOUT:ESP_ERR_INVALID_STATE;
  assert(pull_logs(a->peer,[](auto){assert(false);return true;})==expected&&!log_pull.busy&&!a->cancelled&&a->tls->healthy);}
 // Fixed overall deadline stops a slowly advancing transfer, preserving prior bytes.
 a=setup();chunks=0;tick=[&]{pump(a,[&](const auto& peer,const auto& request){respond(peer,request,content(log_capacity));});};
 assert(pull_logs(a->peer,[&](auto){++chunks;clock_ms+=1000;return true;})==ESP_ERR_TIMEOUT&&chunks==15&&!log_pull.busy&&!a->cancelled);
 std::cout<<"PASS main consumer abort/cancel, fixed per-chunk/overall deadlines, disconnect, partial data and secondary errors preserve healthy streams\n";
}
void sender_identity_and_late_result_fences(){
 auto a=setup();const auto source=content(2048);unsigned requests=0;std::string received;
 tick=[&]{pump(a,[&](const auto& peer,const auto& request){++requests;const auto id=*model::MessageId::parse(request.get("transfer_id")->string);const auto offset=request.get("offset")->integer;
  assert(dispatch(peer,message(peer,Kind::log_result,log_result_json(ident<model::MessageId>(99),offset,source.size(),std::string_view(source).substr(offset,1024)))));
  assert(!log_pull.received);
  if(!offset){assert(dispatch(peer,message(peer,Kind::log_result,log_result_json(id,1024,source.size(),std::string_view(source).substr(1024)))));assert(!log_pull.received);}
  auto stale=message(peer,Kind::log_result,log_result_json(id,offset,source.size(),std::string_view(source).substr(offset,1024)));stale.origin_boot_id=ident<model::BootId>(99);assert(!dispatch(peer,stale)&&!log_pull.received);
  respond(peer,request,source);
 });};assert(pull_logs(a->peer,[&](auto bytes){received+=bytes;return true;})==ESP_OK&&received==source&&requests==2);
 a=setup();requests=0;tick=[&]{pump(a,[&](const auto& peer,const auto& request){++requests;respond(peer,request,content(requests==1?2048:3072));});};
 received.clear();assert(pull_logs(a->peer,[&](auto bytes){received+=bytes;return true;})==ESP_FAIL&&received.size()==1024&&!a->cancelled&&!log_pull.busy);
 a=setup();a->status.negotiated_capabilities&=~peer_logs;assert(pull_logs(a->peer,[](auto){return true;})==ESP_ERR_NOT_SUPPORTED&&a->queue.empty());
 a=setup();group.peers.clear();assert(pull_logs(a->peer,[](auto){return true;})==ESP_ERR_INVALID_STATE&&a->queue.empty());
 a=setup(false);assert(pull_logs(a->peer,[](auto){return true;})==ESP_ERR_INVALID_STATE&&a->queue.empty());
 a=setup();status.authority_ready=false;assert(pull_logs(a->peer,[](auto){return true;})==ESP_ERR_INVALID_STATE&&a->queue.empty());
 std::cout<<"PASS exact peer/transfer/offset/boot fences, stable total, capability/current-roster/main authorization and late-result isolation\n";
}
}
int main(){using namespace amity::consortium;receiver_snapshot_lifecycle();receiver_errors_and_expiry();sender_exact_and_bounded();sender_failures_and_cancellation();sender_identity_and_late_result_fences();}
'''

def main():
    actual = (ROOT/'components/consortium/logs_runtime.inc').read_text()
    with tempfile.TemporaryDirectory(prefix='amity-logs-runtime-') as directory:
        temp = Path(directory)
        (temp/'esp_err.h').write_text('#pragma once\nusing esp_err_t=int;constexpr int ESP_OK=0,ESP_ERR_INVALID_ARG=1,ESP_ERR_INVALID_STATE=2,ESP_ERR_NO_MEM=3,ESP_ERR_TIMEOUT=4,ESP_FAIL=5,ESP_ERR_NOT_FOUND=6,ESP_ERR_NOT_SUPPORTED=7;\n')
        cpp, binary = temp/'test.cpp', temp/'test'
        cpp.write_text(PREFIX+'namespace amity::consortium {\n'+actual+'\n}\n'+SUITE)
        sources = [ROOT/'components/consortium'/name for name in
                   ('json.cpp','wire.cpp','session.cpp','owned_intent.cpp','settings.cpp','update.cpp','transfer.cpp')]
        sources.append(ROOT/'components/amity_system/model.cpp')
        subprocess.run(shlex.split(os.environ.get('CXX','c++'))+
                       ['-std=c++17','-Wall','-Wextra','-Werror','-g','-fsanitize=address,undefined','-I'+str(temp)]+
                       ['-I'+str(ROOT/path) for path in ('components/consortium','components/consortium/include','components/amity_system/include')]+
                       [str(cpp)]+list(map(str,sources))+['-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True)

if __name__ == '__main__':
    main()
