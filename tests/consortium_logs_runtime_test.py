#!/usr/bin/env python3
"""Exercise actual streamed member logs and bounded pulls with real wire/session code."""
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
#include <new>
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
void wake(Active&){}
bool current_peer_credential(const Active& active,const Group& current){for(const auto& peer:current.peers)if(peer.id==active.peer&&peer.id==active.tls->authenticated&&peer.active)return true;return false;}
bool settings_enrolled(model::OctopusId owner){for(const auto& peer:group.peers)if(peer.id==owner&&peer.active)return true;return false;}
std::vector<LogChunk> sent;bool send_success=true;
bool send(Active&,Kind kind,std::string_view payload,uint32_t ttl){LogChunk chunk;assert(kind==Kind::log_result&&ttl==2000&&decode_log_chunk(payload,chunk));sent.push_back(std::move(chunk));return send_success;}
unsigned pdMS_TO_TICKS(unsigned ms){return ms;}
std::function<void()> tick;
void vTaskDelay(unsigned ms){clock_ms+=ms;if(tick)tick();}
}
'''
SUITE = r'''
namespace amity::consortium {
Json parsed(std::string_view body){Json value;JsonError error;assert(parse_json(body,value,error));return value;}
Envelope request_message(const std::shared_ptr<Active>& active,model::MessageId transfer){
 Envelope e;e.config_revision=active->session.scope().config_revision;e.kind=Kind::log_request;e.ttl_ms=2000;e.payload=parsed(log_request_json(transfer));assert(valid_envelope_payload(e.kind,e.payload));return e;
}
Envelope chunk_message(const std::shared_ptr<Active>& active,const LogChunk& chunk){
 Envelope e;e.config_revision=active->session.scope().config_revision;e.kind=Kind::log_result;e.ttl_ms=2000;
 Envelope decoded;assert(parse_envelope(log_chunk_envelope(e,encode_log_chunk(chunk)),decoded)==Error::none);return decoded;
}
LogChunk chunk(model::MessageId transfer,size_t offset,size_t total,std::string bytes,bool unavailable=false){LogChunk value;value.transfer=transfer;value.offset=uint32_t(offset);value.total=uint32_t(total);value.unavailable=unavailable;value.bytes=std::move(bytes);return value;}
bool dispatch(const std::shared_ptr<Active>& active,const Envelope& e){return active->session.admit(e,clock_ms,clock_ms)==Error::none&&log_dispatch(active,e,clock_ms);}
std::shared_ptr<Active> setup(bool main=true){
 clock_ms=100;links.clear();sent.clear();send_success=true;tick={};log_hooks={};log_pull={};group={};identity={};status={};
 identity.id=ident<model::OctopusId>(1);group.role=main?Group::coordinator:Group::member;group.authority=main?identity.id:ident<model::OctopusId>(2);group.consortium=ident<model::ConsortiumId>(3);status.authority_ready=main;
 auto a=std::make_shared<Active>();a->peer=ident<model::OctopusId>(2);a->tls->authenticated=a->peer;group.peers.push_back({a->peer,"leaf","pin",true});
 LinkScope scope{ident<model::LinkId>(4),group.consortium,identity.id,a->peer,group.authority,ident<model::BootId>(5),ident<model::BootId>(6),7};
 assert(a->session.authenticate(scope,a->peer,main?PeerRole::actuator:PeerRole::coordinator,true)==Error::none);
 HelloFrame h;h.consortium_id=scope.consortium_id;h.octopus_id=a->peer;h.boot_id=scope.peer_boot_id;h.config_revision=7;
 assert(a->session.hello(h)==Error::none&&a->session.synchronized(7,true,true)==Error::none);
 a->status.id=a->peer;a->status.boot_id=scope.peer_boot_id;a->status.config_revision=7;links.push_back(a);return a;
}
std::shared_ptr<Active> add_member(unsigned number){
 auto a=std::make_shared<Active>();a->peer=ident<model::OctopusId>(number);a->tls->authenticated=a->peer;group.peers.push_back({a->peer,"leaf","pin",true});
 LinkScope scope{ident<model::LinkId>(100+number),group.consortium,identity.id,a->peer,group.authority,ident<model::BootId>(5),ident<model::BootId>(200+number),7};
 assert(a->session.authenticate(scope,a->peer,PeerRole::actuator,true)==Error::none);
 HelloFrame h;h.consortium_id=scope.consortium_id;h.octopus_id=a->peer;h.boot_id=scope.peer_boot_id;h.config_revision=7;
 assert(a->session.hello(h)==Error::none&&a->session.synchronized(7,true,true)==Error::none);
 a->status.id=a->peer;a->status.boot_id=scope.peer_boot_id;a->status.config_revision=7;links.push_back(a);return a;
}
std::string content(size_t size){std::string value;for(size_t i=0;i<size;++i)value+=char(i%256);return value;}
// The member's link loop sends one chunk per pass until its stream ends.
std::string stream(const std::shared_ptr<Active>& a,size_t& chunks){
 std::string received;chunks=0;
 while(a->log_transfer){const auto before=sent.size();assert(send_log_chunk(*a)&&sent.size()==before+1);received+=sent.back().bytes;++chunks;}
 return received;
}
void receiver_snapshot_lifecycle(){
 auto a=setup(false);const auto transfer=ident<model::MessageId>(10);std::string live=content(log_capacity);const auto expected=live;unsigned snapshots=0,released=0;
 log_hooks.snapshot=[&]{++snapshots;auto* data=new char[std::max<size_t>(live.size(),1)];std::copy(live.begin(),live.end(),data);return LogSnapshot{std::shared_ptr<const char>(data,[&](const char* bytes){++released;delete[] bytes;}),live.size()};};
 assert(dispatch(a,request_message(a,transfer))&&sent.empty()&&snapshots==1);
 live.assign(log_capacity,'X');size_t chunks=0;
 assert(stream(a,chunks)==expected&&chunks==log_capacity/log_chunk_bytes&&released==1&&!a->log_snapshot&&!a->log_transfer&&!a->cancelled&&a->tls->healthy);
 for(size_t n=0;n<sent.size();++n)assert(sent[n].transfer==transfer&&sent[n].offset==n*log_chunk_bytes&&sent[n].total==log_capacity&&!sent[n].unavailable);
 // A new request replaces an unfinished stream and releases its snapshot.
 const auto next=ident<model::MessageId>(11);assert(dispatch(a,request_message(a,next))&&send_log_chunk(*a)&&a->log_transfer==next&&a->log_next==log_chunk_bytes);
 assert(dispatch(a,request_message(a,transfer))&&released==2&&snapshots==3&&a->log_transfer==transfer&&!a->log_next);
 sent.clear();assert(stream(a,chunks).size()==log_capacity&&sent.front().transfer==transfer&&released==3);
 // Empty logs are one successful, empty final chunk with immediate release.
 live.clear();sent.clear();assert(dispatch(a,request_message(a,next)));stream(a,chunks);
 assert(chunks==1&&sent.back().total==0&&sent.back().bytes.empty()&&!sent.back().unavailable&&released==4);
 std::cout<<"PASS actual member: one request streams a stable 24KiB snapshot in ordered chunks while the ring changes; replacement and empty logs release it\n";
}
void receiver_errors(){
 auto a=setup(false);size_t chunks=0;
 assert(dispatch(a,request_message(a,ident<model::MessageId>(21))));stream(a,chunks);
 assert(chunks==1&&sent.back().unavailable&&!sent.back().total&&!a->log_transfer&&!a->cancelled&&a->tls->healthy);
 send_success=false;assert(dispatch(a,request_message(a,ident<model::MessageId>(22)))&&!send_log_chunk(*a)&&!a->log_transfer);send_success=true;
 assert(!dispatch(a,chunk_message(a,chunk(ident<model::MessageId>(21),0,0,{}))));
 std::cout<<"PASS member reports an unavailable log once without replacing TLS; a failed send ends its stream; role direction rejects\n";
}
using Responder=std::function<void(const std::shared_ptr<Active>&,model::MessageId)>;
void stream_source(const std::shared_ptr<Active>& a,model::MessageId transfer,const std::string& source){
 for(size_t offset=0;;offset+=log_chunk_bytes){
  const auto size=std::min(log_chunk_bytes,source.size()-offset);
  assert(dispatch(a,chunk_message(a,chunk(transfer,offset,source.size(),source.substr(offset,size)))));
  if(offset+size==source.size())break;
 }
}
void pump(const std::shared_ptr<Active>& a,const Responder& response){
 if(a->queue.empty())return;
 const auto queued=std::move(a->queue.front());a->queue.pop_front();assert(queued.kind==Kind::log_request&&queued.ttl==2000&&queued.revision==7);
 const auto request=parsed(queued.payload);assert(valid_envelope_payload(queued.kind,request));response(a,*model::MessageId::parse(request.get("transfer_id")->string));
}
void sender_exact_and_bounded(){
 auto a=setup();const auto transport=a->tls;const auto original=a->session.scope();const auto source=content(log_capacity);size_t requests=0,calls=0;std::string received;
 tick=[&]{pump(a,[&](const auto& peer,auto transfer){++requests;stream_source(peer,transfer,source);});};
 assert(pull_logs(a->peer,[&](std::string_view bytes){assert(bytes.size()<=log_chunk_bytes);++calls;received+=bytes;
  assert(pull_logs(a->peer,[](auto){assert(false);return true;})==ESP_ERR_INVALID_STATE);return true;})==ESP_OK);
 assert(received==source&&requests==1&&calls==log_capacity/log_chunk_bytes&&!log_pull.busy&&!log_pull.bytes&&a->queue.empty());
 assert(a->tls==transport&&a->session.scope()==original&&!a->cancelled&&a->tls->healthy);
 // Three authenticated owners, one a management-ready member running other firmware.
 a=setup();auto second=add_member(30),third=add_member(31);second->ready=false;second->management_ready=true;second->session.restrict_management();
 tick=[&]{for(const auto& peer:links)pump(peer,[&](const auto& recipient,auto transfer){stream_source(recipient,transfer,source);});};
 for(const auto& owner:{a,second,third}){
  received.clear();assert(pull_logs(owner->peer,[&](auto bytes){received+=bytes;return true;})==ESP_OK);
  assert(received==source&&!log_pull.busy&&links.size()==3);
  for(const auto& peer:links)assert(peer->queue.empty()&&!peer->cancelled&&peer->tls->healthy);
 }
 a=setup();tick=[&]{pump(a,[](const auto& peer,auto transfer){stream_source(peer,transfer,{});});};unsigned chunks=0;
 assert(pull_logs(a->peer,[&](auto){++chunks;return true;})==ESP_OK&&chunks==0);
 std::cout<<"PASS actual main: one request per pull, exact 24KiB/empty logs, serialized collectors and management-ready TLS\n";
}
void sender_failures_and_cancellation(){
 auto a=setup();const auto body=content(2*log_chunk_bytes);size_t chunks=0;
 tick=[&]{pump(a,[&](const auto& peer,auto transfer){stream_source(peer,transfer,body);});};
 assert(pull_logs(a->peer,[&](auto){++chunks;return false;})==ESP_FAIL&&chunks==1&&!log_pull.busy&&a->queue.empty());
 // Without an answer the pull waits two seconds and withdraws its request.
 a=setup();clock_ms=500;assert(pull_logs(a->peer,[](auto){assert(false);return true;})==ESP_ERR_TIMEOUT&&clock_ms==2500&&!log_pull.busy&&a->queue.empty()&&!a->cancelled&&a->tls->healthy);
 a=setup();tick=[&]{a->cancelled=true;};assert(pull_logs(a->peer,[](auto){assert(false);return true;})==ESP_ERR_INVALID_STATE&&!log_pull.busy&&a->queue.empty());
 a=setup();tick=[&]{pump(a,[&](const auto& peer,auto transfer){assert(dispatch(peer,chunk_message(peer,chunk(transfer,0,0,{},true))));});};
 assert(pull_logs(a->peer,[](auto){assert(false);return true;})==ESP_ERR_NO_MEM&&!log_pull.busy&&!a->cancelled&&a->tls->healthy);
 // The overall deadline stops a slowly advancing stream, preserving prior bytes.
 a=setup();chunks=0;const auto source=content(log_capacity);model::MessageId slow;size_t offset=0;
 tick=[&]{pump(a,[&](const auto&,auto transfer){slow=transfer;});
  if(slow&&offset<source.size()){assert(dispatch(a,chunk_message(a,chunk(slow,offset,source.size(),source.substr(offset,log_chunk_bytes)))));offset+=log_chunk_bytes;}};
 assert(pull_logs(a->peer,[&](auto){++chunks;clock_ms+=3000;return true;})==ESP_ERR_TIMEOUT&&chunks==5&&!log_pull.busy&&!a->cancelled);
 std::cout<<"PASS main consumer abort/cancel, progress and overall deadlines, disconnect, partial data and unavailable logs preserve healthy streams\n";
}
void sender_identity_and_late_chunk_fences(){
 auto a=setup();const auto source=content(2*log_chunk_bytes);std::string received;
 tick=[&]{pump(a,[&](const auto& peer,auto transfer){
  assert(dispatch(peer,chunk_message(peer,chunk(ident<model::MessageId>(99),0,source.size(),source.substr(0,log_chunk_bytes))))&&!log_pull.received);
  stream_source(peer,transfer,source);
 });};assert(pull_logs(a->peer,[&](auto bytes){received+=bytes;return true;})==ESP_OK&&received==source);
 // A changed total or an offset gap ends the pull after the bytes received so far.
 a=setup();tick=[&]{pump(a,[&](const auto& peer,auto transfer){
  assert(dispatch(peer,chunk_message(peer,chunk(transfer,0,2*log_chunk_bytes,source.substr(0,log_chunk_bytes)))));
  assert(dispatch(peer,chunk_message(peer,chunk(transfer,log_chunk_bytes,3*log_chunk_bytes,source.substr(0,log_chunk_bytes)))));
 });};
 received.clear();assert(pull_logs(a->peer,[&](auto bytes){received+=bytes;return true;})==ESP_FAIL&&received.size()==log_chunk_bytes&&!a->cancelled&&!log_pull.busy);
 a=setup();tick=[&]{pump(a,[&](const auto& peer,auto transfer){assert(dispatch(peer,chunk_message(peer,chunk(transfer,log_chunk_bytes,2*log_chunk_bytes,source.substr(0,log_chunk_bytes)))));});};
 assert(pull_logs(a->peer,[](auto){assert(false);return true;})==ESP_FAIL&&!log_pull.busy);
 assert(dispatch(a,chunk_message(a,chunk(ident<model::MessageId>(98),0,1,"x")))&&!log_pull.busy); // A late chunk belongs to no pull.
 a=setup();group.peers.clear();assert(pull_logs(a->peer,[](auto){return true;})==ESP_ERR_INVALID_STATE&&a->queue.empty());
 a=setup(false);assert(pull_logs(a->peer,[](auto){return true;})==ESP_ERR_INVALID_STATE&&a->queue.empty());
 a=setup();status.authority_ready=false;assert(pull_logs(a->peer,[](auto){return true;})==ESP_ERR_INVALID_STATE&&a->queue.empty());
 std::cout<<"PASS exact peer/transfer/offset fences, stable total, current-roster/main authorization and late-chunk isolation\n";
}
}
int main(){using namespace amity::consortium;receiver_snapshot_lifecycle();receiver_errors();sender_exact_and_bounded();sender_failures_and_cancellation();sender_identity_and_late_chunk_fences();}
'''

def main():
    actual = (ROOT/'components/consortium/logs_runtime.inc').read_text()
    with tempfile.TemporaryDirectory(prefix='amity-logs-runtime-') as directory:
        temp = Path(directory)
        (temp/'esp_err.h').write_text('#pragma once\nusing esp_err_t=int;constexpr int ESP_OK=0,ESP_ERR_INVALID_ARG=1,ESP_ERR_INVALID_STATE=2,ESP_ERR_NO_MEM=3,ESP_ERR_TIMEOUT=4,ESP_FAIL=5,ESP_ERR_NOT_FOUND=6,ESP_ERR_NOT_SUPPORTED=7;\n')
        cpp, binary = temp/'test.cpp', temp/'test'
        cpp.write_text(PREFIX+'namespace amity::consortium {\n'+actual+'\n}\n'+SUITE)
        sources = [ROOT/'components/consortium'/name for name in
                   ('json.cpp','wire.cpp','session.cpp','owned_intent.cpp','settings.cpp','update.cpp')]
        sources.append(ROOT/'components/amity_system/model.cpp')
        subprocess.run(shlex.split(os.environ.get('CXX','c++'))+
                       ['-std=c++17','-Wall','-Wextra','-Werror','-g','-fsanitize=address,undefined','-I'+str(temp)]+
                       ['-I'+str(ROOT/path) for path in ('components/consortium','components/consortium/include','components/amity_system/include')]+
                       [str(cpp)]+list(map(str,sources))+['-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True)

if __name__ == '__main__':
    main()
