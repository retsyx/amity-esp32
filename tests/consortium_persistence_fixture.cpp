// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#include "amity/consortium/runtime.hpp"
#include "amity/consortium/owned_intent.hpp"
#include "amity/consortium/update.hpp"
#include "controller_policy.hpp"
#include "enrollment_abort.hpp"
#include "hostname_registry.hpp"
#include "runtime_persistence.hpp"
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <future>
#include <iostream>
#include <map>
#include <thread>
#include <sys/socket.h>

namespace amity::consortium {
using namespace platform;
using namespace std::chrono_literals;
template<class T>T ident(unsigned n){char text[33];snprintf(text,sizeof(text),"%032x",n);return *T::parse(text);}
uint64_t now_ms(){return 100;}
struct Queued {Kind kind;std::string payload;uint32_t ttl=0;uint64_t created=0,revision=0;std::shared_ptr<const model::OwnedIntent> owned;std::shared_ptr<const model::OwnedAdapterSnapshot> snapshot{};bool firmware_stream=false;};
struct FakeTls {bool server=true;int socket=-1;model::OctopusId authenticated;std::vector<Peer> allowed;};
struct Active {
    Session session;model::OctopusId peer;std::shared_ptr<FakeTls> tls=std::make_shared<FakeTls>();
    std::mutex mutex;std::deque<Queued> queue;bool cancelled=false,retiring=false,ready=true,management_ready=false,refreshing=false,context_ready=true,context_requested=false;
    std::atomic_bool restart_requested{false},execution_fenced{false};RuntimePeerStatus status;
    std::optional<EnrollmentAbort> enrollment_abort;model::MessageId removal_request;uint64_t removal_until=0;std::string retirement_payload;
};
std::mutex lock,persistence_lock;
Group group,durable_group;Identity identity;RuntimeStatus status;RuntimeLifecycleHooks lifecycle;RuntimeControlHooks controls;
model::BootId boot;uint64_t configuration=1;HostnameRegistry hostnames;std::vector<std::shared_ptr<Active>> links;
struct SavedSettings {model::OctopusId owner;};std::vector<SavedSettings> settings_caches,settings_operations;
struct {bool local=false;std::weak_ptr<Active> link;}settings_authorization;
// Diagnostic emission is the only non-storage seam omitted from admission.
struct LinkTrace {const char* phase="";bool retained=false,has_kind=false;Kind kind{};void queued(const Queued&){}bool fail(const char*){return false;}};
struct DeferredLinkTrace {void capture(const Active&,const LinkTrace&,uint64_t){}};
struct SnapshotRetryBatch {void dropped(const Active&,const Queued&){}};
template<class... T>void quiet_log(T&&...){}
#define ESP_LOGI(...) quiet_log(__VA_ARGS__)
std::string sha256(std::string_view){return std::string(64,'a');}
bool current_peer_credential(const Active& active,const Group& value){for(const auto& peer:value.peers)if(peer.id==active.peer&&peer.id==active.tls->authenticated)for(const auto& allowed:active.tls->allowed)if(peer.id==allowed.id&&peer.der==allowed.der&&peer.pin==allowed.pin)return true;return false;}

// The storage boundary really waits: no task sleeps to infer acquisition.
struct Storage {
    std::mutex mutex;std::condition_variable changed;bool blocked=false,entered=false,released=false,fail=false;
    unsigned writes=0,inflight=0,maximum=0;std::map<std::string,std::string> records;
    void arm(bool failing=false){std::lock_guard<std::mutex> guard(mutex);blocked=true;entered=released=false;fail=failing;}
    void await(){std::unique_lock<std::mutex> guard(mutex);assert(changed.wait_for(guard,2s,[&]{return entered;}));}
    void release(){std::lock_guard<std::mutex> guard(mutex);released=true;changed.notify_all();}
    bool write(const std::function<void()>& commit){
        std::unique_lock<std::mutex> guard(mutex);++writes;++inflight;maximum=std::max(maximum,inflight);
        if(blocked){entered=true;changed.notify_all();changed.wait(guard,[&]{return released;});blocked=false;}
        const bool success=!fail;if(success)commit();--inflight;return success;
    }
} storage;
esp_err_t save_group(const Group& next){return storage.write([&]{durable_group=next;})?ESP_OK:ESP_FAIL;}
esp_err_t write_record(const char*,const char* key,std::string_view bytes){return storage.write([&]{storage.records[key]=bytes;})?ESP_OK:ESP_FAIL;}

// ACTUAL_RUNTIME

std::shared_ptr<Active> connection(unsigned number){
    auto active=std::make_shared<Active>();active->peer=ident<model::OctopusId>(number);active->tls->authenticated=active->peer;
    Peer peer{active->peer,"leaf-"+std::to_string(number),"pin-"+std::to_string(number),true};group.peers.push_back(peer);active->tls->allowed={peer};
    LinkScope scope{ident<model::LinkId>(100+number),group.consortium,identity.id,active->peer,identity.id,boot,ident<model::BootId>(200+number),1,1};
    assert(active->session.authenticate(scope,scope.peer_id,PeerRole::actuator,true)==Error::none);
    HelloFrame hello;hello.consortium_id=scope.consortium_id;hello.octopus_id=scope.peer_id;hello.boot_id=scope.peer_boot_id;hello.coordinator_epoch=1;hello.config_revision=1;hello.required_capabilities=known_capabilities;
    assert(active->session.hello(hello,known_capabilities)==Error::none&&active->session.synchronized(1,true,true)==Error::none);
    active->status.id=active->peer;active->status.boot_id=scope.peer_boot_id;active->status.config_revision=1;active->status.negotiated_capabilities=known_capabilities;active->status.ready=true;links.push_back(active);return active;
}
void reset(){
    group={};hostnames={};links.clear();removal_operations.clear();settings_caches.clear();settings_operations.clear();settings_authorization={};
    identity.id=ident<model::OctopusId>(1);boot=ident<model::BootId>(9);group.role=Group::coordinator;group.consortium=ident<model::ConsortiumId>(10);group.authority=identity.id;group.epoch=group.revision=1;
    status={};status.initialized=status.credential_ready=status.authority_ready=true;configuration=1;controls={};lifecycle={};storage.records.clear();storage.fail=false;storage.writes=storage.maximum=0;storage.blocked=storage.entered=storage.released=false;
}
template<class Future>void ready(Future& future){assert(future.wait_for(2s)==std::future_status::ready);}
void responsive(const std::shared_ptr<Active>& other){
    auto status_read=std::async(std::launch::async,[]{return runtime_status();});ready(status_read);assert(status_read.get().active_peers>=1);
    auto key=std::async(std::launch::async,[&]{model::OwnedIntent intent;intent.kind=model::OwnedIntentKind::key_edges;intent.target.port.consortium_id=group.consortium;intent.target.port.octopus_id=other->peer;intent.config_revision=1;intent.generation=1;intent.edge_count=1;intent.edges[0]={1,true};return runtime_send_owned(intent);});
    ready(key);assert(key.get()==ESP_OK);std::lock_guard<std::mutex> guard(other->mutex);assert(!other->queue.empty()&&other->queue.back().owned->kind==model::OwnedIntentKind::key_edges);
}
void hostname_serialization(){
    reset();auto first=connection(2),other=connection(3);storage.arm();
    auto writing=std::async(std::launch::async,[&]{remember_live_hostname(*first,"first");});storage.await();responsive(other);
    {std::lock_guard<std::mutex> guard(lock);assert(hostnames.entries().empty());}
    std::promise<void> started;auto second=std::async(std::launch::async,[&]{started.set_value();remember_live_hostname(*other,"second");});started.get_future().wait();
    // A competing writer may wait, but cannot take the runtime lock while it waits.
    responsive(other);storage.release();ready(writing);writing.get();ready(second);second.get();
    HostnameRegistry reboot;assert(reboot.restore(storage.records.at("hostnames")));assert(reboot.lookup(first->peer)=="first"&&reboot.lookup(other->peer)=="second"&&storage.maximum==1);
    storage.arm(true);auto failed=std::async(std::launch::async,[&]{remember_live_hostname(*first,"failed-name");});storage.await();responsive(other);storage.release();ready(failed);failed.get();
    assert(hostnames.lookup(first->peer)=="first"&&first->status.hostname=="failed-name");
}
void membership_serialization(){
    reset();auto first=connection(2),other=connection(3),third=connection(4);storage.arm();
    auto remove=std::async(std::launch::async,[&]{return runtime_remove_octopus(first->peer,ident<model::MessageId>(20),1,{});});storage.await();responsive(other);
    {std::lock_guard<std::mutex> guard(lock);assert(group.peers.size()==3&&group.revision==1);first->cancelled=true;}
    std::promise<void> started;auto second=std::async(std::launch::async,[&]{started.set_value();return runtime_remove_octopus(third->peer,ident<model::MessageId>(21),2,{});});started.get_future().wait();responsive(other);
    storage.release();ready(remove);assert(remove.get()==ESP_OK);ready(second);assert(second.get()==ESP_OK);
    assert(group.revision==3&&group.peers.size()==1&&group.peers[0].id==other->peer&&durable_group.revision==3&&durable_group.peers.size()==1&&storage.maximum==1);
    assert(removal_operations.front().reset=="unconfirmed"&&!first->retiring); // A disconnect cannot undo Remove or redirect reset.
}
void removal_cannot_resurrect_name(){
    reset();auto first=connection(2),other=connection(3);remember_live_hostname(*first,"saved-name");storage.arm();
    auto remove=std::async(std::launch::async,[&]{return runtime_remove_octopus(first->peer,ident<model::MessageId>(22),1,{});});storage.await();responsive(other);
    auto stale_name=std::async(std::launch::async,[&]{remember_live_hostname(*first,"stale-name");});storage.release();ready(remove);assert(remove.get()==ESP_OK);ready(stale_name);stale_name.get();
    assert(hostnames.lookup(first->peer)=="saved-name");assert(runtime_forget_removed_octopus(first->peer)==ESP_OK);remember_live_hostname(*first,"late-name");
    HostnameRegistry reboot;assert(reboot.restore(storage.records.at("hostnames"))&&reboot.lookup(first->peer).empty()&&hostnames.lookup(first->peer).empty());
}
void disconnected_target_lifetime(){
    reset();auto first=connection(2),other=connection(3);const auto owner=first->peer;std::weak_ptr<Active> weak=first;storage.arm();
    auto remove=std::async(std::launch::async,[&]{return runtime_remove_octopus(owner,ident<model::MessageId>(23),1,{});});storage.await();
    {std::lock_guard<std::mutex> guard(lock);first->cancelled=true;links.erase(links.begin());first.reset();}
    responsive(other);storage.release();ready(remove);assert(remove.get()==ESP_OK&&weak.expired()&&group.peers.size()==1&&removal_operations.front().reset=="unconfirmed");
}
}
int main(){using namespace amity::consortium;membership_serialization();hostname_serialization();removal_cannot_resurrect_name();disconnected_target_lifetime();std::cout<<"PASS blocked flash: status and independent-owner keys remain responsive; writers serialize, failed writes do not publish, removal cannot be undone by late names or disconnected targets\n";}
