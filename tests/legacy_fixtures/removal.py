"""Removal fixture: a scripted TLS link, the link state, the runtime globals and recording
seams."""
PREFIX = r'''
#include "amity/consortium/runtime.hpp"
#include "runtime_persistence.hpp"
#include "amity/consortium/owned_intent.hpp"
#include "amity/consortium/settings.hpp"
#include "amity/consortium/update.hpp"
#include "controller_policy.hpp"
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
struct FakeTls{bool server=false;int socket=-1;model::OctopusId authenticated;std::vector<Peer> allowed;std::vector<std::string> frames;size_t next=0;bool received_bulk()const{Envelope envelope;return next&&parse_envelope(frames[next-1],envelope)==Error::none&&is_update_kind(envelope.kind);}bool receive(std::string& out,uint32_t timeout){assert(timeout<=2000);if(next==frames.size()){clock_ms+=timeout;return false;}out=frames[next++];return true;}};
struct Active{Session session;model::OctopusId peer;std::shared_ptr<FakeTls> tls=std::make_shared<FakeTls>();std::mutex mutex;std::deque<int> queue;bool cancelled=false,retiring=false,ready=true,management_ready=false,refreshing=false,context_ready=false,context_requested=false;LinkScope context_retry_scope;uint64_t context_retry_at=0;std::atomic_bool restart_requested{false},execution_fenced{false};RuntimePeerStatus status;model::MessageId removal_request;uint64_t removal_until=0;};
void wake(Active&){}
std::mutex lock,persistence_lock;Group group;Identity identity;RuntimeStatus status;RuntimeLifecycleHooks lifecycle;RuntimeControlHooks controls;model::BootId boot;uint64_t configuration=1;std::vector<std::shared_ptr<Active>> links;
// Diagnostics are independently exercised by runtime transport tests. This
// fixture executes the completion and real roster-removal decisions directly.
struct LinkTrace{enum class Recovery{other,terminal,recoverable};const char* phase="";bool retained=false;bool fail(const char*,Error=Error::none,Recovery=Recovery::other){return false;}};
struct DeferredLinkTrace{void capture(const Active&,const LinkTrace&,uint64_t){}};
#define ESP_LOGW(...) ((void)0)
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
