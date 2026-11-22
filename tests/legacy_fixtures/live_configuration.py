"""In-place synchronizer fixture.

PREFIX holds the scripted TLS link, the link state and the runtime globals. SUPPORT builds
and feeds envelopes and starts an authenticated link.
"""
from pathlib import Path

from slice_support import function

ROOT = Path(__file__).resolve().parents[2]
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
