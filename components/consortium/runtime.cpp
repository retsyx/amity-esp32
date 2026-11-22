// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#ifdef ESP_PLATFORM
#include "platform.hpp"
#include "admission_crypto.hpp"
#include "config_store.hpp"
#include "controller_policy.hpp"
#include "hostname_registry.hpp"
#include "runtime_persistence.hpp"
#include "tcp_ack_trace.hpp"
#include "network_diagnostics.hpp"
#include "wifi_signal.hpp"
#include "amity/consortium/bootstrap.hpp"
#include "amity/consortium/enrollment.hpp"
#include "amity/consortium/update.hpp"
#include "amity/consortium/owned_intent.hpp"
#include "amity/consortium/settings.hpp"
#include "system_configuration.hpp"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "nvs.h"
#include "esp_netif.h"
#include "mdns.h"
#include "psa/crypto.h"
#include "mbedtls/platform_util.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/api.h"
#include "lwip/tcpip.h"
#include "lwip/priv/tcp_priv.h"
// The pinned SDK declares this diagnostic accessor outside its C-linkage block.
extern "C" {
#include "lwip/priv/sockets_priv.h"
}
#include <fcntl.h>
#include <cerrno>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <deque>
#include <cstdio>
#include <cstring>
namespace amity::consortium {
using namespace platform;
namespace {
struct Queued {
    Kind kind;std::string payload;uint32_t ttl=0;uint64_t created=0,revision=0;
    std::shared_ptr<const model::OwnedIntent> owned;
    std::shared_ptr<const model::OwnedAdapterSnapshot> snapshot{};
    bool firmware_stream=false;
};
// Diagnostic metadata only: no retained payload, retry or execution decision.
struct LinkTrace {
    const char* phase="handshake";
    const char* reason="cancelled";
    Error error=Error::none;
    Kind kind=Kind::device_status;
    bool has_kind=false,unexpected=false,has_queue=false,retained=false;
    uint64_t created=0,queued_revision=0;
    uint32_t ttl=0;
    int action_code=-1;
    int socket_error=0;
    platform::TlsReceiveDiagnostic receive;
    bool fail(const char* why,Error result=Error::none){reason=why;error=result;unexpected=true;return false;}
    void queued(const Queued& value){kind=value.kind;has_kind=has_queue=true;created=value.created;ttl=value.ttl;queued_revision=value.revision;action_code=value.owned?static_cast<int>(value.owned->kind):-1;}
};
static bool pure_key_release(const model::OwnedIntent& intent){return intent.kind==model::OwnedIntentKind::key_edges&&intent.edge_count==1&&!intent.edges[0].down;}
static bool selection_intent(const model::OwnedIntent& intent){return intent.kind==model::OwnedIntentKind::activity||intent.kind==model::OwnedIntentKind::context;}
static bool queued_cleanup(const Queued& queued){
    return queued.owned&&pure_key_release(*queued.owned);
}
// A late press is dropped, so its hold never starts. Releases, activity
// selections and admitted image bytes are delivered late rather than lost;
// a newer selection replaces an older queued one instead.
static bool queued_expires(const Queued& queued){
    return !queued.firmware_stream&&!(queued.owned&&(pure_key_release(*queued.owned)||selection_intent(*queued.owned)));
}
static bool queued_expired(const Queued& queued,uint64_t now){
    return now<queued.created||(queued_expires(queued)&&now-queued.created>=queued.ttl);
}
static void log_dropped_intent(const Queued& queued,uint64_t now){
    if(!queued.owned)return;
    const auto& intent=*queued.owned;const auto age=now>=queued.created?now-queued.created:0;
    if(intent.kind==model::OwnedIntentKind::key_edges)
        ESP_LOGW("consortium","Key dropped stage=expired owner=%s key=%u generation=%llu phase=%s age_ms=%llu ttl_ms=%u",intent.target.port.octopus_id.str().c_str(),static_cast<unsigned>(intent.edges[0].key),
            static_cast<unsigned long long>(intent.generation),intent.edge_count==2?"tap":"down",static_cast<unsigned long long>(age),queued.ttl);
    else ESP_LOGW("consortium","Owner request dropped stage=expired owner=%s action=%d age_ms=%llu ttl_ms=%u",intent.target.port.octopus_id.str().c_str(),static_cast<int>(intent.kind),static_cast<unsigned long long>(age),queued.ttl);
}
struct DroppedIntents {
    std::vector<Queued> intents;uint64_t now=0;
    // Declared before queue/global guards, so logging runs after unlock.
    ~DroppedIntents(){for(const auto& queued:intents)log_dropped_intent(queued,now);}
};
struct SynchronizationRound {model::MessageId id;uint64_t revision=0;};
struct Active {
    std::shared_ptr<TlsLink> tls;
    Session session;
    model::OctopusId peer;
    // Runtime-lock protected ownership of a pending outbound connection and
    // the authenticated peer boot that claimed this connection.
    model::OctopusId connection_peer;
    model::BootId claimed_boot;
    std::mutex mutex;
    std::mutex close_mutex;bool closed_notified=false;
    std::deque<Queued> queue;
    // Owned only by the link task, including its socket's lifetime.
    TcpAckTrace release_ack;
    network_diagnostics::Snapshot release_network_before;
    model::OctopusId release_ack_owner;
    uint64_t release_ack_generation=0;
    uint8_t release_ack_key=0,release_ack_edges=0;
    bool release_ack_down=false;
    // Link-task owned: a member's last Wi-Fi signal report to the main.
    std::optional<LinkScope> wifi_scope;
    uint64_t wifi_reported_at=0;
    network_diagnostics::WifiSignal wifi_reported;
    std::atomic_bool cancelled{false},retiring{false},ready{false},management_ready{false},context_ready{false},context_requested{false},refreshing{false};
    std::atomic_bool execution_fenced{false};
    std::atomic_bool restart_requested{false};
    model::LinkId cleaned_link;
    bool lifecycle_only=false;
    std::string endpoint_hint;
    model::MessageId removal_request;uint64_t removal_until=0;
    model::MessageId update_begin_id;
    RuntimePeerStatus status;
    // Link-task owned, at most one PSRAM snapshot for the main's current pull.
    LogSnapshot log_snapshot;
    model::MessageId log_transfer;
    size_t log_next=0;
    uint64_t log_expires=0;
    // One incoming application restart marker, never ordinary work or frames.
    SynchronizationRound synchronization;
    std::optional<SynchronizationRound> pending_synchronization;
    bool execution_supported=true;
    std::string expected_hostname,advertised_hostname;
};
// Caller holds the runtime and link mutexes. This ends application authority
// immediately; the owning link task performs cleanup and a fresh logical
// handshake without replacing the authenticated transport or replaying work.
void request_session_restart(Active& active){
    active.restart_requested=true;active.execution_fenced=true;
    active.ready=false;active.management_ready=false;active.status.ready=false;active.status.management_ready=false;
    active.context_ready=false;active.context_requested=false;
    active.queue.clear();
}
// Writers acquire persistence_lock before lock, and release lock during flash.
std::mutex persistence_lock;
std::mutex lock;
std::mutex connection_lock;
std::mutex enrollment_lock;
Identity identity;
Group group;
HostnameRegistry hostnames;
struct InviteRequest {model::MessageId scan_id;};
struct Admission {
    RuntimeAdmissionStatus view;
    uint64_t generation=0,deadline=0;
    std::string selected_token,hostname;
    bool submitted=false;
} admission;
model::ConsortiumId singleton;
model::BootId boot;
uint64_t configuration=1;
uint8_t hardware_tentacles=1,hardware_tentacles_mask=1;
RuntimeStatus status;
RuntimeRadioHooks radio;
RuntimeNetworkHooks network;
bool current_peer_credential(const Active& active,const Group& current);
// Both persistence_lock and guard are held. The registry prepares its private
// next entries before storage, then publishes only after this callback relocks.
Error remember_hostname(model::OctopusId owner,const std::string& name,std::unique_lock<std::mutex>& guard){
    return hostnames.remember(owner,name,[&](std::string_view bytes){
        return without_runtime_lock(guard,[&]{return write_record("amity_group","hostnames",bytes);})==ESP_OK;
    });
}
// Storage failure must not discard authenticated transport or its live name.
void remember_live_hostname(Active& active,const std::string& name){
    std::lock_guard<std::mutex> persistence(persistence_lock);
    std::unique_lock<std::mutex> guard(lock);
    if(!current_peer_credential(active,group))return;
    {std::lock_guard<std::mutex> snapshot(active.mutex);active.status.hostname=name;}
    if(remember_hostname(active.peer,name,guard)!=Error::none)
        status.last_error="The saved device name could not be updated.";
}
RuntimeControlHooks controls;
RuntimeLifecycleHooks lifecycle;
RuntimeUpdateHooks updates;
std::vector<std::shared_ptr<Active>> links;
std::atomic_bool initialized{false};
std::atomic_bool invitation_running{false};
std::atomic_bool invitation_cancelled{false};
uint64_t candidate_generation=0,candidate_deadline=0,pending_radio_release=0;
bool candidate_passive=false;
int listener=-1;
void error(std::string value){std::lock_guard<std::mutex> guard(lock);status.last_error=std::move(value);}
template<class T>T random_id(){auto value=T::parse(random_hex(16));return value?*value:T{};}
Envelope outgoing(const Active &link,Kind kind,uint32_t ttl){Envelope e;e.config_revision=link.session.scope().config_revision;e.kind=kind;e.ttl_ms=ttl;return e;}
bool lifecycle_kind(Kind kind){return kind==Kind::membership_event||kind==Kind::membership_result;}
bool current_peer_credential(const Active &link,const Group &current){
    for(const auto &peer:current.peers)if(peer.id==link.tls->authenticated)for(const auto &allowed:link.tls->allowed)if(allowed.id==peer.id&&allowed.der==peer.der&&allowed.pin==peer.pin)return true;
    return false;
}
bool send(Active &link,Kind kind,std::string_view payload,uint32_t ttl=1000){
    if(link.cancelled|| (link.lifecycle_only&&!lifecycle_kind(kind)) || (link.retiring&&kind!=Kind::membership_event&&kind!=Kind::membership_result))return false;
    if(link.management_ready&&!is_update_kind(kind)&&!is_log_kind(kind)&&!lifecycle_kind(kind))return false;
    if(link.restart_requested&&kind!=Kind::synchronization&&kind!=Kind::device_status&&!lifecycle_kind(kind))return false;
    // The TLS anchor snapshot may predate a roster mutation. Ordinary work must
    // still belong to the current allowlist immediately before it is sent.
    if(kind!=Kind::membership_event&&kind!=Kind::membership_result){
        std::lock_guard<std::mutex> guard(lock);
        if(!current_peer_credential(link,group))return false;
    }
    const auto envelope=outgoing(link,kind,ttl);
    const auto frame=kind==Kind::update_chunk?update_chunk_envelope(envelope,payload):envelope_json(envelope,payload);
    return !frame.empty()&&link.tls->send(frame,is_update_kind(kind));
}
#include "settings_runtime.inc"
#include "logs_runtime.inc"
#include "removal_runtime.inc"
std::string control_json(model::JoinId join,std::string_view digest,std::string_view phase,std::string_view extra){return "{\"phase\":\""+std::string(phase)+"\",\"join_id\":\""+join.str()+"\",\"join_digest\":\""+std::string(digest)+"\""+std::string(extra)+"}";}
bool enroll_lan(Active &active,bool server){
    BoundedSetupTimings<4> timing("enrollment",now_ms);timing.peer=active.peer;timing.link=active.session.scope().link_id;timing.begin("total");
    std::lock_guard<std::mutex> transaction(enrollment_lock);
    Group current;{std::lock_guard<std::mutex> guard(lock);current=group;}
    if(!current.join_id||(server&&current.joining_id!=active.peer))return timing.done(current.role!=Group::prepared);
    const auto digest=current.join_digest;std::string incoming;Envelope envelope;auto receive=[&](){
        if(!active.tls->receive(incoming))return false;
        const auto received=now_ms();
        return parse_envelope(incoming,envelope)==Error::none&&active.tls->received_bulk()==is_update_kind(envelope.kind)&&active.session.admit(envelope,received,received)==Error::none&&envelope.kind==Kind::enrollment_control&&envelope.payload.get("join_id")->string==current.join_id.str()&&envelope.payload.get("join_digest")->string==digest;
    };
    if(server){
        const auto store=timing.begin("membership_store");
        {std::lock_guard<std::mutex> persistence(persistence_lock);std::unique_lock<std::mutex> guard(lock);if(group.join_id!=current.join_id||group.join_digest!=digest)return false;auto next=group;bool found=false;for(auto &peer:next.peers)if(peer.id==active.peer){peer.active=true;found=true;}if(!found)return false;next.revision=std::max(next.revision,next.join_revision);if(without_runtime_lock(guard,[&]{return save_group(next);})!=ESP_OK)return false;group=std::move(next);status.membership_revision=group.revision;}
        timing.finish(store,true);
        const auto acknowledgement=timing.begin("commit_ack");
        if(!send(active,Kind::enrollment_control,control_json(current.join_id,digest,"commit",",\"membership_revision\":\""+std::to_string(current.join_revision)+"\""))||!receive()||envelope.payload.get("phase")->string!="ack")return false;
        uint64_t acknowledged=0;if(!decimal(envelope.payload.get("membership_revision"),acknowledged)||acknowledged!=current.join_revision)return false;
        timing.finish(acknowledgement,true);const auto finalize=timing.begin("finalize_store");
        {std::lock_guard<std::mutex> persistence(persistence_lock);std::unique_lock<std::mutex> guard(lock);if(group.join_id!=current.join_id||group.join_digest!=digest)return false;auto next=group;next.join_id={};next.joining_id={};next.join_digest.clear();next.join_hostname.clear();next.join_revision=0;if(without_runtime_lock(guard,[&]{return save_group(next);})!=ESP_OK)return false;group=std::move(next);}
        timing.finish(finalize,true);
    }else{
        if(!receive()||envelope.payload.get("phase")->string!="commit")return false;
        uint64_t revision=0;if(!decimal(envelope.payload.get("membership_revision"),revision)||revision!=current.join_revision)return false;
        {std::lock_guard<std::mutex> persistence(persistence_lock);std::unique_lock<std::mutex> guard(lock);if(group.join_id!=current.join_id||group.join_digest!=digest)return false;auto next=group;next.role=Group::member;next.revision=std::max(next.revision,revision);for(auto &p:next.peers)p.active=true;if(without_runtime_lock(guard,[&]{return save_group(next);})!=ESP_OK)return false;group=std::move(next);status.member=true;status.membership_revision=group.revision;}
        if(!send(active,Kind::enrollment_control,control_json(current.join_id,digest,"ack",",\"membership_revision\":\""+std::to_string(revision)+"\"")))return false;
        if(current.role==Group::prepared){
            // The old singleton SystemControl identity remains fenced. Let the
            // durable LAN acknowledgement leave TCP, then rebuild from the new
            // group ID/member role before accepting any configuration slice.
            runtime_close_enrollment();vTaskDelay(pdMS_TO_TICKS(500));
            // This link task has a PSRAM stack. Restart disables the flash
            // cache, so use the existing internal-stack broker and fail closed
            // if it cannot accept the handoff.
            config_store::with_internal_flash_stack([]{esp_restart();});return false;
        }
    }
    if(!server)runtime_close_enrollment();
    return timing.done(true);
}
// A member binds its controller's configuration revision before it
// acknowledges a barrier or applies an advance. Its device reports then
// travel as ordinary results on the ready session.
bool bind_configuration(Active& active,uint64_t revision,LinkTrace& trace){
    BoundedSetupTimings<2> timing("synchronize",now_ms);timing.peer=active.peer;timing.link=active.session.scope().link_id;timing.begin("total");
    trace.phase="configuration_apply";
    RuntimeControlHooks hooks;{std::lock_guard<std::mutex> guard(lock);hooks=controls;}
    if(!hooks.synchronize)return timing.done(trace.fail("synchronization_hooks",Error::not_ready));
    const auto apply=timing.begin("config_apply");const auto applied=hooks.synchronize(active.peer,revision);timing.finish(apply,applied==Error::none);
    if(applied!=Error::none)return timing.done(trace.fail("synchronization_apply",applied));
    return timing.done(true);
}
// Live configuration changes keep the logical session. Frames carry the new
// revision after this point; older in-flight frames are discarded as stale.
bool apply_configuration_advance(Active& active,bool server,uint64_t target,LinkTrace& trace){
    trace.phase="configuration_advance";
    std::lock_guard<std::mutex> guard(lock);std::lock_guard<std::mutex> queue(active.mutex);
    const auto advanced=active.session.advance_configuration(target);if(advanced!=Error::none)return trace.fail("synchronization_advance",advanced);
    active.status.config_revision=target;active.context_ready=false;active.context_requested=false;active.refreshing=true;
    // A hostname update can cross the change and be discarded as stale.
    // Publish it again in the new scope.
    active.advertised_hostname.clear();
    if(!server)configuration=target;
    return true;
}
bool advance_configuration(Active& active,uint64_t target,LinkTrace& trace){
    trace.phase="configuration_advance";
    if(!send(active,Kind::synchronization,synchronization_json(random_id<model::MessageId>(),"advance",target),10000))return trace.fail("synchronization_advance_send");
    return apply_configuration_advance(active,true,target,trace);
}
bool finish_synchronization(Active& active,bool server,uint64_t revision,LinkTrace& trace){
    trace.phase="synchronization";
    const auto synchronized=active.session.synchronized(revision,true,true);if(synchronized!=Error::none)return trace.fail("synchronization_ready",synchronized);
    {std::lock_guard<std::mutex> guard(active.mutex);active.ready=true;active.status.ready=true;}
    if(!server){std::lock_guard<std::mutex> persistence(persistence_lock);std::unique_lock<std::mutex> guard(lock);configuration=revision;if(group.role==Group::member&&!active.endpoint_hint.empty()&&group.authority_host!=active.endpoint_hint){auto next=group;next.authority_host=active.endpoint_hint;if(without_runtime_lock(guard,[&]{return save_group(next);})==ESP_OK)group=std::move(next);}}
    return true;
}
bool synchronization_current(const Active& active){
    std::lock_guard<std::mutex> guard(lock);
    const auto scope=active.session.scope();
    return !active.cancelled&&!active.retiring&&!active.lifecycle_only&&active.tls->usable()&&current_peer_credential(active,group)&&
        group.consortium==scope.consortium_id&&group.authority==scope.coordinator_id&&
        ((group.role==Group::coordinator&&group.authority==scope.local_id)||(group.role==Group::member&&group.authority==scope.peer_id));
}
bool synchronization_recoverable(const Active& active,const LinkTrace& trace){
    if(!synchronization_current(active))return false;
    // Application delivery failure ends its logical execution scope. The
    // original action is never retried; only synchronization may start again.
    switch(trace.error){
    case Error::wrong_consortium:case Error::stale_boot:
    case Error::identity_conflict:case Error::unauthorized_role:case Error::target_not_local:
    case Error::unsupported_kind:return false;
    default:return true;
    }
}
bool synchronization_barrier(Active& active,bool server,SynchronizationRound& round,LinkTrace& trace){
    trace.phase="synchronization_barrier";
    const auto until=now_ms()+10000;
    // TCP order makes the ACK a boundary: all work from the abandoned round
    // precedes it. No old frames or application payloads are kept for replay.
    for(unsigned count=0;count<64&&synchronization_current(active)&&now_ms()<until;++count){
        if(!server&&active.pending_synchronization){round=*active.pending_synchronization;active.pending_synchronization.reset();return true;}
        const auto now=now_ms();if(now>=until)break;
        std::string body;if(!active.tls->receive(body,static_cast<uint32_t>(until-now)))return trace.fail("synchronization_barrier_receive");
        Envelope envelope;const auto parsed=parse_envelope(body,envelope);if(parsed!=Error::none)return trace.fail("synchronization_barrier_parse",parsed);
        if(active.tls->received_bulk()!=is_update_kind(envelope.kind))return trace.fail("synchronization_frame_class",Error::invalid_message);
        const auto received=now_ms();auto admitted=active.session.admit(envelope,received,received);
        if(admitted==Error::stale_config){admitted=active.session.discard_stale(envelope,received,received);if(admitted==Error::none)continue;}
        if(admitted!=Error::none&&envelope.kind!=Kind::synchronization){
            const auto discarded=active.session.discard_synchronization_frame(envelope,received,received);
            if(discarded==Error::none)continue;
        }
        if(admitted!=Error::none)return trace.fail("synchronization_barrier_admit",admitted);
        if(envelope.kind==Kind::synchronization){
            SynchronizationRound incoming;std::string phase;
            if(parse_synchronization(envelope.payload,incoming.id,phase,incoming.revision)!=Error::none)return trace.fail("synchronization_barrier_payload",Error::invalid_message);
            if(!server&&phase=="begin"){round=incoming;return true;}
            if(server&&incoming.id==round.id&&incoming.revision==round.revision){
                if(phase=="ack")return true;
                if(phase=="reject")return trace.fail("synchronization_barrier_rejected");
            }
            continue;
        }

    }
    return trace.fail("synchronization_barrier_timeout");
}
bool fence_synchronization_scope(Active& active,LinkTrace& trace){
    log_snapshot_clear(active);
    const auto scope=active.session.scope();
    RuntimeControlHooks hooks;RuntimeUpdateHooks update_hooks;
    model::MessageId interrupted_update;bool already_cleaned=false;
    {std::lock_guard<std::mutex> guard(lock);std::lock_guard<std::mutex> queue(active.mutex);
        // End execution before waiting for the peer's barrier. A late callback
        // for this ended scope cannot treat withheld readiness as lost delivery
        // and tear down the independently healthy transport.
        active.execution_fenced=true;active.ready=false;active.management_ready=false;active.status.ready=false;active.status.management_ready=false;
        interrupted_update=active.update_begin_id;active.update_begin_id={};
        already_cleaned=active.cleaned_link==scope.link_id;
        hooks=controls;update_hooks=updates;
    }
    // End the exact upload before owner cleanup or a repeated fence can return.
    // Its maintenance state must not block synchronization on retained TLS.
    if(interrupted_update&&update_hooks.disconnected)update_hooks.disconnected(active.peer,interrupted_update);
    if(already_cleaned)return true;
    // This is an execution-session fence, not a device disconnect. Keep its
    // independently discovered HDMI observations and authenticated TLS object.
    if(!hooks.restart_session||!hooks.restart_session(active.peer,scope.peer_boot_id,scope.link_id))return trace.fail("synchronization_fence",Error::not_ready);
    {std::lock_guard<std::mutex> queue(active.mutex);active.queue.clear();}
    active.cleaned_link=scope.link_id;
    return true;
}
bool restart_synchronization_scope(Active& active,const SynchronizationRound& round,LinkTrace& trace){
    const auto scope=active.session.scope();
    const auto next=model::LinkId::parse(round.id.str());
    if(!next||*next==scope.link_id)return trace.fail("synchronization_link",Error::identity_conflict);
    if(!fence_synchronization_scope(active,trace))return false;
    std::lock_guard<std::mutex> guard(lock);std::lock_guard<std::mutex> queue(active.mutex);
    if(active.cancelled||active.retiring||!current_peer_credential(active,group)||!(active.session.scope()==scope))return trace.fail("synchronization_scope",Error::identity_conflict);
    const auto begun=active.session.begin_synchronization(round.revision,*next);
    if(begun!=Error::none)return trace.fail("synchronization_begin",begun);
    // IPC work was admitted to the ended link, not the replacement's scope.
    // Never restamp it into a fresh logical session on this same transport.
    active.queue.clear();active.synchronization=round;
    active.context_ready=false;active.context_requested=false;active.refreshing=true;


    return true;
}
bool exchange_hostname(Active&,const std::string&,LinkTrace*);
bool synchronize_connection(Active& active,bool server,bool publish_ready=true,uint64_t revision=0,LinkTrace* diagnostic=nullptr){
    LinkTrace local;auto& trace=diagnostic?*diagnostic:local;
    // An authenticated peer running another image may receive firmware,
    // log collection and explicit removal. Do not exchange application state
    // until it reboots into the same image and completes a fresh hello.
    if(!active.execution_supported||!publish_ready){
        std::lock_guard<std::mutex> guard(active.mutex);
        active.session.restrict_management();active.ready=false;active.status.ready=false;
        active.management_ready=true;active.status.management_ready=true;
        active.context_ready=false;active.context_requested=false;active.queue.clear();
        active.execution_fenced=false;active.restart_requested=false;
        return true;
    }
    // Connection ownership and authentication are already established. Failed
    // application rounds only withhold readiness; they never replace TLS.
    bool retry=false;
    while(synchronization_current(active)){
        if(retry){
            ESP_LOGW("consortium","Synchronization incomplete reason=%s peer=%s link=%s; retaining connection",trace.reason,active.peer.str().c_str(),active.session.scope().link_id.str().c_str());
            vTaskDelay(std::max<unsigned>(1,pdMS_TO_TICKS(1000)));
        }
        retry=true;trace.error=Error::none;
        if(!fence_synchronization_scope(active,trace)){if(!synchronization_recoverable(active,trace))return false;continue;}
        SynchronizationRound round;
        if(server){
            {std::lock_guard<std::mutex> guard(lock);round.revision=std::max(configuration,active.session.scope().config_revision);}
            if(revision)round.revision=std::max(round.revision,revision);
            round.id=random_id<model::MessageId>();
            if(!send(active,Kind::synchronization,synchronization_json(round.id,"begin",round.revision),10000)){trace.fail("synchronization_barrier_send");if(!synchronization_recoverable(active,trace))return false;continue;}
        }
        if(!synchronization_barrier(active,server,round,trace)){
            if(!synchronization_recoverable(active,trace))return false;
            continue;
        }
        if(!restart_synchronization_scope(active,round,trace)){if(!synchronization_recoverable(active,trace))return false;continue;}
        // The member binds the round's configuration revision before it
        // acknowledges, so the acknowledgement means the member is current.
        const bool bound=server||bind_configuration(active,round.revision,trace);
        if(bound&&!server&&!send(active,Kind::synchronization,synchronization_json(round.id,"ack",round.revision),10000)){
            trace.fail("synchronization_barrier_ack_send");if(!synchronization_recoverable(active,trace))return false;continue;
        }
        if(bound&&exchange_hostname(active,active.expected_hostname,&trace)&&finish_synchronization(active,server,round.revision,trace)){
            {std::lock_guard<std::mutex> guard(active.mutex);active.status.config_revision=round.revision;active.status.management_ready=false;active.execution_fenced=false;active.restart_requested=false;}
            return true;
        }
        if(!synchronization_recoverable(active,trace))return false;
        if(!server&&!active.pending_synchronization&&!send(active,Kind::synchronization,synchronization_json(round.id,"reject",round.revision),10000)){
            trace.fail("synchronization_reject_send");if(!synchronization_recoverable(active,trace))return false;
        }
    }
    return false;
}
bool recover_operational_frame(Active& active,bool server,LinkTrace& trace){
    if(!synchronization_recoverable(active,trace))return false;
    // Expired or undeliverable application work ends its execution session,
    // not TLS. Finish exact cleanup before asking the peer to reset. Transient
    // owner-IPC pressure cannot turn into a transport replacement either.
    while(synchronization_current(active)){
        if(!fence_synchronization_scope(active,trace)){
            if(!synchronization_recoverable(active,trace))return false;
            vTaskDelay(std::max<unsigned>(1,pdMS_TO_TICKS(1000)));continue;
        }
        if(!server&&active.synchronization.id&&!active.pending_synchronization&&!send(active,Kind::synchronization,synchronization_json(active.synchronization.id,"reject",active.synchronization.revision),10000)){
            trace.fail("synchronization_reject_send");if(!synchronization_recoverable(active,trace))return false;
            vTaskDelay(std::max<unsigned>(1,pdMS_TO_TICKS(1000)));continue;
        }
        return synchronize_connection(active,server,active.execution_supported,0,&trace);
    }
    return false;
}
bool application_failure_recoverable(const Active& active,const LinkTrace& trace){
    if(!synchronization_recoverable(active,trace)||trace.error==Error::invalid_message)return false;
    // These failures concern authenticated protocol/security admission, not
    // lost ordinary application work. Keep their existing terminal fences.
    for(const auto* terminal:{"socket_select","envelope_parse","synchronization_control_admit","synchronization_control_parse","stale_frame",
        "membership_revoked","retirement","retirement_admit",
        "update_admit","update_authority","control_admit","settings_admit","settings_boot","settings_authority"})
        if(!strcmp(trace.reason,terminal))return false;
    if(active.restart_requested)return true;
    for(const auto* recoverable:{"application_restart_requested","cleanup_queue_full","causal_result_fence","context_recapture_failed",
        "initial_context","control_dispatch","frame_receive_expired","queued_send","queued_invalid","settings_dispatch","update_not_ready","update_stream"})
        if(!strcmp(trace.reason,recoverable))return true;
    return false;
}
bool lifecycle_receive(Active &active,Envelope &envelope,uint32_t timeout=5000){
    const auto deadline=now_ms()+timeout;
    unsigned frames=0;
    while(!active.cancelled&&now_ms()<deadline&&frames++<32){
        const auto before=now_ms();if(before>=deadline)return false;
        std::string bytes;if(!active.tls->receive(bytes,std::min<uint64_t>(5000,deadline-before))||parse_envelope(bytes,envelope)!=Error::none||active.tls->received_bulk()!=is_update_kind(envelope.kind))return false;
        const auto now=now_ms();if(now>=deadline)return false;const auto admitted=active.session.admit(envelope,now,now);
        if(admitted==Error::none)return true;
        // Valid observations/results can precede the reset ACK on the same
        // ordered stream. Drain them without executing work or granting
        // management/readiness; their original peer/boot/type fences
        // remain valid even when their configuration is already obsolete.
        if(active.session.discard_lifecycle_frame(envelope,now,now)!=Error::none)return false;
    }
    return false;
}
bool exchange_hostname(Active& active,const std::string& expected,LinkTrace* diagnostic=nullptr) {
    BoundedSetupTimings<1> timing("hostname",now_ms);timing.peer=active.peer;timing.link=active.session.scope().link_id;timing.begin("exchange");
    LinkTrace local_trace;auto& trace=diagnostic?*diagnostic:local_trace;trace.phase="hostname";
    RuntimeNetworkHooks hooks;{std::lock_guard<std::mutex> guard(lock);hooks=network;}
    if(!hooks.get_hostname)return trace.fail("hostname_hooks",Error::not_ready);
    const auto local=hooks.get_hostname();if(!valid_hostname(local))return trace.fail("hostname_capture",Error::invalid_message);
    if(!send(active,Kind::device_status,"{\"hostname\":\""+local+"\"}",1000))return trace.fail("hostname_send");
    active.advertised_hostname=local;
    std::string raw;if(!active.tls->receive(raw))return trace.fail("hostname_receive");
    Envelope response;const auto received=now_ms();
    const auto parsed=parse_envelope(raw,response);if(parsed!=Error::none)return trace.fail("hostname_parse",parsed);
    if(active.tls->received_bulk()!=is_update_kind(response.kind))return trace.fail("hostname_frame_class",Error::invalid_message);
    const auto admitted=active.session.admit(response,received,received);if(admitted!=Error::none)return trace.fail("hostname_admission",admitted);
    if(response.kind==Kind::synchronization){
        SynchronizationRound round;std::string phase;
        const auto parsed_round=parse_synchronization(response.payload,round.id,phase,round.revision);if(parsed_round!=Error::none)return trace.fail("hostname_synchronization_parse",parsed_round);
        if(phase=="begin"&&active.session.scope().peer_id==active.session.scope().coordinator_id){active.pending_synchronization=round;return trace.fail("synchronization_restarted");}
        if(phase=="reject"&&round.id==active.synchronization.id&&round.revision==active.synchronization.revision)return trace.fail("synchronization_peer_rejected");
        return trace.fail("hostname_synchronization_phase",Error::invalid_message);
    }
    if(response.kind!=Kind::device_status||!response.payload.get("hostname"))return trace.fail("hostname_kind",Error::invalid_message);
    const auto& name=response.payload.get("hostname")->string;
    if(!expected.empty()&&name!=expected)return trace.fail("hostname_identity",Error::identity_conflict);
    std::lock_guard<std::mutex> persistence(persistence_lock);
    std::unique_lock<std::mutex> guard(lock);
    if(!current_peer_credential(active,group))return trace.fail("hostname_credential",Error::identity_conflict);
    const auto remembered=remember_hostname(active.peer,name,guard);if(remembered!=Error::none)return trace.fail("hostname_persist",remembered);
    std::lock_guard<std::mutex> snapshot(active.mutex);active.status.hostname=name;return timing.done(true);
}
// Hostnames are public observation metadata, independent of the ordered
// connection's authenticated identity and application synchronization scope.
bool refresh_local_hostname(Active& active){
    if(active.management_ready)return true;
    RuntimeNetworkHooks hooks;{std::lock_guard<std::mutex> guard(lock);hooks=network;}
    if(!hooks.get_hostname)return true;
    const auto name=hooks.get_hostname();if(!valid_hostname(name)||name==active.advertised_hostname)return true;
    if(!send(active,Kind::device_status,"{\"hostname\":\""+name+"\"}",1000))return false;
    active.advertised_hostname=name;return true;
}
// Members report their station signal for the main's diagnostics. Samples
// vary constantly: report a change at most every ten seconds, and promptly in
// a new scope, whose start may discard an old-scope report.
bool report_wifi_signal(Active& active){
    const auto scope=active.session.scope();
    if(!active.ready||active.management_ready||active.lifecycle_only||scope.peer_id!=scope.coordinator_id)return true;
    const auto signal=network_diagnostics::wifi_signal();if(!signal.known)return true;
    const auto now=now_ms();
    if(active.wifi_scope&&*active.wifi_scope==scope&&(now-active.wifi_reported_at<10000||
       (signal.rssi==active.wifi_reported.rssi&&signal.channel==active.wifi_reported.channel)))return true;
    if(!send(active,Kind::device_status,wifi_signal_json({signal.rssi,signal.channel}),1000))return false;
    active.wifi_scope=scope;active.wifi_reported_at=now;active.wifi_reported=signal;return true;
}
bool receive_live_device_status(Active& active,const Envelope& envelope,uint64_t received){
    if(envelope.kind!=Kind::device_status||active.session.admit(envelope,received,received)!=Error::none)return false;
    const auto* name=envelope.payload.get("hostname");if(name&&(name->type!=Json::Type::string||!valid_hostname(name->string)))return false;
    {std::lock_guard<std::mutex> guard(lock);if(!current_peer_credential(active,group))return false;}
    if(const auto signal=parse_wifi_signal(envelope.payload)){
        std::lock_guard<std::mutex> snapshot(active.mutex);
        active.status.wifi_known=true;active.status.wifi_rssi=signal->rssi;active.status.wifi_channel=signal->channel;
    }
    if(name)remember_live_hostname(active,name->string);
    return true;
}
bool notify_link_closed(Active& active){
    std::lock_guard<std::mutex> once(active.close_mutex);
    if(active.closed_notified)return true;
    RuntimeControlHooks hooks;{std::lock_guard<std::mutex> guard(lock);
        // Only a connection that claimed this owner can publish its loss.
        // Failed authentication and rejected duplicates never owned the live
        // owner state, even when their Hello named the same peer and boot.
        if(!active.claimed_boot){active.closed_notified=true;return true;}
        hooks=controls;
    }
    const auto scope=active.session.scope();
    if(hooks.disconnected&&active.peer&&scope.peer_boot_id&&scope.link_id&&!hooks.disconnected(active.peer,scope.peer_boot_id,scope.link_id))return false;
    active.closed_notified=true;return true;
}
bool fence_replaced_links(Active& active){
    std::vector<std::shared_ptr<Active>> previous;
    const auto peer_boot=active.session.scope().peer_boot_id;
    {std::lock_guard<std::mutex> guard(lock);
        if(active.cancelled||!peer_boot)return false;
        // Repeated handshakes from the same running peer cannot displace its
        // existing connection, even while that connection is synchronizing.
        for(const auto& old:links)if(old.get()!=&active&&old->peer==active.peer&&old->claimed_boot==peer_boot&&!old->cancelled&&old->tls&&old->tls->usable()&&current_peer_credential(*old,group)){
            active.claimed_boot={};active.cancelled=true;return false;
        }
        active.claimed_boot=peer_boot;
        for(auto& old:links)if(old.get()!=&active&&old->peer==active.peer&&old->claimed_boot){
            if(!old->cancelled){old->cancelled=true;old->ready=false;old->management_ready=false;::shutdown(old->tls->socket,SHUT_RDWR);}
            previous.push_back(old);
        }
    }
    for(auto& old:previous)if(!notify_link_closed(*old))return false;
    return true;
}
void reap_closed_links(){
    std::vector<std::shared_ptr<Active>> pending;
    {std::lock_guard<std::mutex> guard(lock);for(const auto& active:links)if(active->cancelled)pending.push_back(active);}
    for(const auto& active:pending)if(notify_link_closed(*active)){std::lock_guard<std::mutex> guard(lock);links.erase(std::remove(links.begin(),links.end(),active),links.end());}
}
bool establish(Active &active,bool server,LinkTrace* diagnostic=nullptr){
    BoundedSetupTimings<7> timing("establish",now_ms);timing.begin("total");
    LinkTrace fallback;auto& trace=diagnostic?*diagnostic:fallback;trace.phase="hello";
    Group current;Identity local;uint64_t config;uint8_t profile_count,profile_mask;
    {std::lock_guard<std::mutex> guard(lock);current=group;local=identity;config=configuration;profile_count=hardware_tentacles;profile_mask=hardware_tentacles_mask;}
    HelloFrame ours;ours.consortium_id=current.consortium;ours.octopus_id=local.id;ours.boot_id=boot;ours.config_revision=config;
    const auto *application=esp_app_get_description();ours.firmware_identity=hex(std::string_view(reinterpret_cast<const char*>(application->app_elf_sha256),32));
    ours.supported_tentacles=profile_count;ours.configured_tentacles_mask=profile_mask;
    if(current.join_id&&((!server&&current.role==Group::prepared)||(server&&current.joining_id==active.tls->authenticated))){ours.pending_join_id=current.join_id;ours.join_digest=current.join_digest;}
    const auto hello=timing.begin("hello_exchange");
    std::string incoming;if(!active.tls->send(hello_json(ours)))return trace.fail("hello_send");if(!active.tls->receive(incoming))return trace.fail("hello_receive");
    if(active.tls->received_bulk())return trace.fail("hello_frame_class",Error::invalid_message);
    HelloFrame peer;const auto parsed=parse_hello(incoming,peer);if(parsed!=Error::none)return trace.fail("hello_parse",parsed);
    const bool matching_firmware=peer.firmware_identity==ours.firmware_identity;
    if(peer.octopus_id!=active.tls->authenticated||peer.consortium_id!=current.consortium)return trace.fail("hello_validation");
    timing.peer=peer.octopus_id;timing.finish(hello,true);const auto link_stage=timing.begin("link_id");
    {std::lock_guard<std::mutex> guard(lock);if(group.consortium!=current.consortium||group.authority!=current.authority||group.revision!=current.revision||!current_peer_credential(active,group))return trace.fail("hello_roster_changed");
    active.peer=peer.octopus_id;}model::LinkId link;trace.phase="link_id";
    if(server){link=random_id<model::LinkId>();if(!active.tls->send("{\"type\":\"link\",\"link_id\":\""+link.str()+"\"}"))return trace.fail("link_id_send");}
    else{Json j;JsonError error;if(!active.tls->receive(incoming)||active.tls->received_bulk()||!parse_json(incoming,j,error)||!closed(j,{"type","link_id"})||j.get("type")->type!=Json::Type::string||j.get("type")->string!="link"||!id(j.get("link_id"),link))return trace.fail("link_id_receive");}
    timing.link=link;timing.finish(link_stage,true);
    if(peer.pending_join_id&&!server&&(!current.join_id||*peer.pending_join_id!=current.join_id||peer.join_digest!=current.join_digest))return trace.fail("join_scope");
    const bool pending_join=current.join_id&&((server&&current.joining_id==peer.octopus_id)||(!server&&peer.pending_join_id.has_value()));
    const bool provisional=current.role==Group::prepared||pending_join;
    LinkScope scope{link,current.consortium,local.id,peer.octopus_id,current.authority,boot,peer.boot_id,server?ours.config_revision:peer.config_revision};
    const auto operational_role=server?PeerRole::actuator:PeerRole::coordinator;
    const auto role=provisional?PeerRole::provisional_candidate:operational_role;
    trace.phase="session_authentication";
    auto admitted=active.session.authenticate(scope,active.tls->authenticated,role,!provisional);if(admitted!=Error::none)return trace.fail("session_authenticate",admitted);
    admitted=active.session.hello(peer);if(admitted!=Error::none)return trace.fail("session_hello",admitted);
    trace.phase="replacement_fence";if(!timing.step("replacement_fence",[&]{return fence_replaced_links(active);}))return trace.fail("replacement_fence");
    if(provisional){trace.phase="enrollment_commit";if(!enroll_lan(active,server))return trace.fail("enrollment_commit");
        admitted=active.session.authenticate(scope,active.tls->authenticated,operational_role,true);if(admitted!=Error::none)return trace.fail("member_authenticate",admitted);
        admitted=active.session.hello(peer);if(admitted!=Error::none)return trace.fail("member_hello",admitted);}
    {std::lock_guard<std::mutex> guard(active.mutex);active.status.id=peer.octopus_id;active.status.boot_id=peer.boot_id;active.status.config_revision=scope.config_revision;active.status.firmware_identity=peer.firmware_identity;active.status.supported_tentacles=peer.supported_tentacles;active.status.configured_tentacles_mask=peer.configured_tentacles_mask;}
    active.expected_hostname=server&&pending_join?current.join_hostname:std::string{};
    active.execution_supported=matching_firmware;
    return timing.done(true);
}
bool finish_retirement(Active &active){
    bool acknowledged=false;model::MessageId request;uint64_t until=0;
    {std::lock_guard<std::mutex> guard(active.mutex);request=active.removal_request;until=active.removal_until;active.ready=false;active.management_ready=false;active.status.ready=false;active.status.management_ready=false;active.queue.clear();}
    struct Result{Active& active;model::MessageId request;bool& acknowledged;~Result(){removal_finish(active.peer,request,acknowledged);}} result{active,request,acknowledged};
    active.session.restrict_lifecycle();
    const auto now=now_ms();if(!request||now>=until||until-now>2000||active.cancelled)return false;
    {
        std::lock_guard<std::mutex> guard(lock);
        const auto issued=std::find_if(removal_operations.begin(),removal_operations.end(),[&](const auto& operation){return operation.owner==active.peer&&operation.id==request&&!operation.complete;});
        if(issued==removal_operations.end()||!issued->scope||issued->until!=until)return false;
        // The reset belongs to the exact reviewed link and boot that Remove
        // admitted, even after it commits newer local configuration.
        const auto& scope=*issued->scope;
        if(!(active.session.scope()==scope)||!active.retiring||!active.tls->server||active.tls->authenticated!=scope.peer_id||active.peer!=scope.peer_id||
           !original_controller(group,identity.id)||!status.authority_ready||std::any_of(group.peers.begin(),group.peers.end(),[&](const auto& peer){return peer.id==scope.peer_id;}))return false;
    }
    const auto sending=now_ms();if(sending>=until)return false;
    if(!send(active,Kind::membership_event,"{\"phase\":\"factory_reset\"}",static_cast<uint32_t>(until-sending)))return false;
    const auto sent=now_ms();if(sent>=until)return false;
    Envelope response;if(!lifecycle_receive(active,response,static_cast<uint32_t>(until-sent))||response.kind!=Kind::membership_result)return false;
    acknowledged=true;return acknowledged;
}
bool retire_member(Active &active,const Envelope &envelope){
    if(envelope.ttl_ms>2000||!active.tls||active.tls->server||active.cancelled)return false;
    RuntimeLifecycleHooks hooks;
    {std::lock_guard<std::mutex> guard(lock);
        if(group.role!=Group::member||!status.member||group.authority!=active.peer||identity.id==active.peer||!current_peer_credential(active,group)||!lifecycle.retire||!lifecycle.factory_reset||!lifecycle.after_factory_reset)return false;
        status.member=false;status.authority_ready=false;status.synchronized=false;status.phase="removed_reset_pending";hooks=lifecycle;
        for(auto &other:links)if(other.get()!=&active){other->cancelled=true;::shutdown(other->tls->socket,SHUT_RDWR);}
    }
    active.session.restrict_lifecycle();
    {std::lock_guard<std::mutex> guard(active.mutex);active.ready=false;active.management_ready=false;active.status.ready=false;active.status.management_ready=false;active.queue.clear();}
    if(hooks.retire()!=ESP_OK||hooks.factory_reset(active.peer)!=ESP_OK)return false;
    // Explicit Remove authorized the reset. A lost ACK cannot strand a durable
    // reset marker; restart only after the response transmission attempt ends.
    const bool sent=send(active,Kind::membership_result,"{\"phase\":\"factory_reset\",\"result\":\"reset_scheduled\"}",2000);hooks.after_factory_reset();return sent;
}
auto context_capture_completed(const LinkScope&,model::OwnedContextResult) -> void;
bool initialize_context(Active& active){
    RuntimeControlHooks hooks;LinkScope scope;
    {std::lock_guard<std::mutex> guard(lock);std::lock_guard<std::mutex> queue(active.mutex);
        if(active.cancelled||active.retiring||active.restart_requested||active.execution_fenced||!active.ready)return false;
        if(active.context_ready||active.context_requested)return true;
        active.context_requested=true;hooks=controls;scope=active.session.scope();
    }
    // The hook may only admit an asynchronous owner-task recapture. Readiness
    // changes when its fresh current context actually enters this link's FIFO.
    if(hooks.ready&&hooks.ready(active.peer,scope.config_revision,[scope](model::OwnedContextResult result){context_capture_completed(scope,result);}))return true;
    {std::lock_guard<std::mutex> guard(lock);std::lock_guard<std::mutex> queue(active.mutex);active.context_requested=false;}
    return false;
}
TcpAckSample sample_tcp_ack(int socket){
    TcpAckSample sample;
    // The calling link task retains its TlsLink: only its destructor closes
    // this descriptor. Other tasks may shut it down; the core lock protects
    // that PCB removal, and a missing connection/PCB is a diagnostic result.
    // Unlike LOCK_TCPIP_CORE, diagnostics must never wait for this mutex.
    if(xSemaphoreTake(lock_tcpip_core,0)!=pdTRUE){sample.state=TcpAckSampleState::busy;return sample;}
    sys_thread_tcpip(LWIP_CORE_LOCK_MARK_HOLDER);
    const auto* sock=lwip_socket_dbg_get_socket(socket);
    const auto* conn=sock?sock->conn:nullptr;
    const auto* pcb=conn&&NETCONNTYPE_GROUP(conn->type)==NETCONN_TCP?conn->pcb.tcp:nullptr;
    if(pcb){
        sample.state=TcpAckSampleState::ready;
        sample.buffered=pcb->snd_lbb;sample.acknowledged=pcb->lastack;
        sample.retransmissions=pcb->nrtx;
        sample.rto_ms=pcb->rto>0?static_cast<uint32_t>(pcb->rto)*TCP_SLOW_INTERVAL:0;
    }
    UNLOCK_TCPIP_CORE();
    return sample;
}
void emit_key_network(const model::OctopusId& owner,uint8_t key,uint64_t generation,uint8_t edges,bool down,const char* state,
                      const network_diagnostics::Snapshot& before,const network_diagnostics::Snapshot& network){
    // Station-wide observations, including other peers and HTTPS traffic;
    // neither these counters nor driver success identify this key's packets.
    const bool release=edges==1&&!down;
    const char* phase=edges==2?"tap":down?"down":"up";
    const auto delta=[](uint32_t after,uint32_t prior){return static_cast<unsigned>(after-prior);};
    // Times and counters share wrapping 32-bit milliseconds. An absent
    // progress event is explicit rather than presented as a fresh event.
    const auto age=[&](uint32_t last,bool observed){return observed?static_cast<unsigned>(network.now_ms-last):UINT32_MAX;};
    ESP_LOGI("consortium","%s stage=network_sta owner=%s key=%u generation=%llu phase=%s edge_count=%u status=%s reg=%u regerr=%ld tx_nm=%u tx_err=%u tx_done=%u tx_fail=%u rx_nm=%u rx_err=%u tx_age_ms=%u done_age_ms=%u rx_age_ms=%u",
        release?"Key release":"Key transfer",owner.str().c_str(),static_cast<unsigned>(key),static_cast<unsigned long long>(generation),phase,static_cast<unsigned>(edges),state,
        static_cast<unsigned>(network.tx_done_registered!=0),static_cast<long>(network.registration_error),
        delta(network.tx.no_mem,before.tx.no_mem),delta(network.tx.other,before.tx.other),delta(network.tx_done_ok,before.tx_done_ok),delta(network.tx_done_fail,before.tx_done_fail),delta(network.rx.no_mem,before.rx.no_mem),delta(network.rx.other,before.rx.other),
        age(network.tx.last_call_ms,network.tx.attempts||network.tx.last_call_ms),age(network.tx_done_last_ms,network.tx_done_ok||network.tx_done_fail||network.tx_done_last_ms),age(network.rx.last_call_ms,network.rx.attempts||network.rx.last_call_ms));
    const auto path=[&](const char* direction,const network_diagnostics::PathSnapshot& prior,const network_diagnostics::PathSnapshot& current){
        // An entry is not a completed handoff. The active gauge is read
        // directly, rather than inferred from independently loaded counters.
        // Spans include descheduling; they do not prove an SDK lock blocked.
        ESP_LOGI("consortium","%s stage=network_path owner=%s key=%u generation=%llu phase=%s edge_count=%u status=%s path=%s calls=%u ok=%u active=%u/%u in_age=%u ok_age=%u last_ms=%u slow=%u slow_age=%u slow_ms=%u",
            release?"Key release":"Key transfer",owner.str().c_str(),static_cast<unsigned>(key),static_cast<unsigned long long>(generation),phase,static_cast<unsigned>(edges),state,direction,
            delta(current.attempts,prior.attempts),delta(current.ok,prior.ok),static_cast<unsigned>(prior.active),static_cast<unsigned>(current.active),
            age(current.last_call_ms,current.attempts||current.last_call_ms),age(current.last_ok_ms,current.ok||current.last_ok_ms),static_cast<unsigned>(current.last_duration_ms),
            delta(current.slow_calls,prior.slow_calls),age(current.last_slow_ms,current.slow_calls||current.last_slow_ms),static_cast<unsigned>(current.last_slow_duration_ms));
    };
    path("tx",before.tx,network.tx);path("rx",before.rx,network.rx);
    // The lwIP timer is on demand. Entry gaps include the previous callback,
    // its normal interval, and any inactive period; they do not prove CPU
    // starvation. This key's established TCP stream supplies the context.
    const auto& timer=network.tcp_timer;
    const auto& prior_timer=before.tcp_timer;
    ESP_LOGI("consortium","%s stage=tcp_timer owner=%s key=%u generation=%llu phase=%s edge_count=%u status=%s calls=%u done=%u active=%u/%u in_age=%u out_age=%u last_ms=%u gaps=%u gap_age=%u gap_ms=%u",
        release?"Key release":"Key transfer",owner.str().c_str(),static_cast<unsigned>(key),static_cast<unsigned long long>(generation),phase,static_cast<unsigned>(edges),state,
        delta(timer.calls,prior_timer.calls),delta(timer.completed,prior_timer.completed),static_cast<unsigned>(prior_timer.active),static_cast<unsigned>(timer.active),
        age(timer.last_entry_ms,timer.calls||timer.last_entry_ms),age(timer.last_complete_ms,timer.completed||timer.last_complete_ms),static_cast<unsigned>(timer.last_duration_ms),
        delta(timer.late_entries,prior_timer.late_entries),age(timer.last_late_ms,timer.late_entries||timer.last_late_ms),static_cast<unsigned>(timer.last_late_gap_ms));
    const auto console_elapsed=delta(network.console_elapsed_ms,before.console_elapsed_ms);
    const auto console_slow=delta(network.console_slow_calls,before.console_slow_calls);
    if(console_elapsed>=100||console_slow){
        // Completed global caller durations may overlap, without causality.
        // Both snapshots precede our reports, excluding their own console cost.
        ESP_LOGI("consortium","%s stage=console_all owner=%s key=%u generation=%llu phase=%s edge_count=%u status=%s calls=%u sum_ms=%u slow=%u last_began_age_ms=%u last_duration_ms=%u",
            release?"Key release":"Key transfer",owner.str().c_str(),static_cast<unsigned>(key),static_cast<unsigned long long>(generation),phase,static_cast<unsigned>(edges),state,
            delta(network.console_calls,before.console_calls),console_elapsed,console_slow,
            age(network.console_last_slow_began_ms,network.console_slow_calls||network.console_last_slow_began_ms),static_cast<unsigned>(network.console_last_slow_duration_ms));
    }
}
void emit_release_ack(const Active& active,const char* state){
    const auto& trace=active.release_ack;
    const bool network_due=trace.elapsed_ms>=250||trace.max_nrtx||
        (std::strcmp(state,"acked")&&std::strcmp(state,"replaced"));
    // Sample before logging, always outside the TCP/IP core and runtime locks.
    const auto network=network_due?network_diagnostics::snapshot():network_diagnostics::Snapshot{};
    const bool release=active.release_ack_edges==1&&!active.release_ack_down;
    const char* phase=active.release_ack_edges==2?"tap":active.release_ack_down?"down":"up";
    ESP_LOGI("consortium","%s stage=ack owner=%s key=%u generation=%llu phase=%s edge_count=%u status=%s elapsed_ms=%llu pending_bytes=%u max_nrtx=%u initial_rto_ms=%u poll_gap_ms=%llu busy=%u",
        release?"Key release":"Key transfer",active.release_ack_owner.str().c_str(),static_cast<unsigned>(active.release_ack_key),static_cast<unsigned long long>(active.release_ack_generation),phase,static_cast<unsigned>(active.release_ack_edges),state,
        static_cast<unsigned long long>(trace.elapsed_ms),trace.pending_bytes,static_cast<unsigned>(trace.max_nrtx),trace.initial_rto_ms,static_cast<unsigned long long>(trace.poll_gap_ms),trace.busy_samples);
    if(network_due)emit_key_network(active.release_ack_owner,active.release_ack_key,active.release_ack_generation,active.release_ack_edges,active.release_ack_down,state,active.release_network_before,network);
}
void poll_release_ack(Active& active){
    if(!active.release_ack.pending)return;
    const auto sample=sample_tcp_ack(active.tls->socket);
    if(const auto* state=active.release_ack.observe(sample,now_ms()))emit_release_ack(active,state);
}
void begin_release_ack(Active& active,const model::OwnedIntent& intent,uint64_t written_at,const network_diagnostics::Snapshot& network_before){
    // A second key message replaces metadata only, never transport work.
    if(active.release_ack.pending){
        poll_release_ack(active);
        if(active.release_ack.pending)emit_release_ack(active,"replaced");
    }
    active.release_ack_owner=intent.target.port.octopus_id;active.release_ack_key=intent.edges[0].key;active.release_ack_generation=intent.generation;
    active.release_ack_edges=intent.edge_count;active.release_ack_down=intent.edges[0].down;
    active.release_network_before=network_before;
    const auto sample=sample_tcp_ack(active.tls->socket);
    if(const auto* state=active.release_ack.begin(sample,written_at,now_ms()))emit_release_ack(active,state);
}
void finish_release_ack(Active& active){
    poll_release_ack(active);
    if(active.release_ack.pending){active.release_ack.pending=false;emit_release_ack(active,"session_ended");}
}
struct LinkTraceSnapshot {
    LinkTrace trace;
    LinkScope scope;
    model::OctopusId peer,authenticated_peer;
    uint64_t now=0;
};
LinkTraceSnapshot snapshot_link_trace(const Active& active,const LinkTrace& trace,uint64_t now){
    return {trace,active.session.scope(),active.peer,active.tls->authenticated,now};
}
void emit_link_trace(const LinkTraceSnapshot& captured){
    const auto& trace=captured.trace;const auto& scope=captured.scope;
    const auto age=trace.has_queue&&captured.now>=trace.created?captured.now-trace.created:0;
    const bool reversed=trace.has_queue&&captured.now<trace.created;
    // The shipping log capture bounds each line to 384 bytes. Keep cause and
    // timing first, with a second bounded identity line correlated by link ID.
    if(trace.unexpected)
        ESP_LOGW("consortium","%s reason=%s phase=%s kind=%s age_ms=%llu ttl_ms=%u qrev=%llu action=%d rev=%llu error=%s errno=%d skew=%d link=%s",
            trace.retained?"Session restart requested":"Link closed",trace.reason,trace.phase,trace.has_kind?kind_name(trace.kind):"none",static_cast<unsigned long long>(age),trace.ttl,static_cast<unsigned long long>(trace.queued_revision),trace.action_code,
            static_cast<unsigned long long>(scope.config_revision),error_name(trace.error),trace.socket_error,reversed,scope.link_id.str().c_str());
    else
        ESP_LOGI("consortium","Link closed reason=%s phase=%s link=%s",trace.reason,trace.phase,scope.link_id.str().c_str());
    ESP_LOGI("consortium","Link scope peer=%s authenticated_peer=%s link=%s local_boot=%s peer_boot=%s revision=%llu",
        captured.peer.str().c_str(),captured.authenticated_peer.str().c_str(),scope.link_id.str().c_str(),scope.local_boot_id.str().c_str(),scope.peer_boot_id.str().c_str(),static_cast<unsigned long long>(scope.config_revision));
    if(trace.receive.cause!=platform::TlsReceiveDiagnostic::Cause::none)
        ESP_LOGW("consortium","%s cause=%s code=%d bytes=%u expected=%u progress_ms=%llu total_ms=%llu poll_gap_ms=%llu read_ms=%llu link=%s",
            trace.receive.expired()?"Application frame expired":"TLS receive failed",trace.receive.name(),trace.receive.code,trace.receive.bytes,trace.receive.expected,static_cast<unsigned long long>(trace.receive.progress_ms),static_cast<unsigned long long>(trace.receive.total_ms),
            static_cast<unsigned long long>(trace.receive.poll_gap_ms),static_cast<unsigned long long>(trace.receive.read_ms),scope.link_id.str().c_str());
}
void trace_link_closed(const Active& active,const LinkTrace& trace,uint64_t now){emit_link_trace(snapshot_link_trace(active,trace,now));}
struct DeferredLinkTrace {
    std::array<LinkTraceSnapshot,maximum_roster_peers> snapshots;
    size_t size=0;
    void capture(const Active& active,const LinkTrace& trace,uint64_t now){if(size<snapshots.size())snapshots[size++]=snapshot_link_trace(active,trace,now);}
    // Construct before runtime/queue guards; emit only after they unlock. The
    // snapshot has no mutable Active pointer, payload or deferred work.
    ~DeferredLinkTrace(){for(size_t i=0;i<size;++i)emit_link_trace(snapshots[i]);}
};
void context_capture_completed(const LinkScope& scope,model::OwnedContextResult result){
    if(result==model::OwnedContextResult::stale)return;
    DeferredLinkTrace diagnostics;std::lock_guard<std::mutex> guard(lock);
    if(!original_controller(group,identity.id)||!status.authority_ready||group.consortium!=scope.consortium_id)return;
    for(auto& active:links)if(!active->cancelled&&!active->retiring&&!active->restart_requested&&!active->execution_fenced&&active->ready){
        std::lock_guard<std::mutex> queue(active->mutex);
        if(!(active->session.scope()==scope))continue;
        // Empty selector sets publish successfully without an outbound context.
        // Finish the revision refresh, but keep positive work gated until an
        // actual current context enters the link FIFO. Leave the accepted
        // request parked so an empty capture is not repeated on every operational turn.
        if(result==model::OwnedContextResult::published){if(scope.config_revision==configuration)active->refreshing=false;continue;}
        LinkTrace trace;trace.phase="owner_context";trace.fail("context_recapture_failed");trace.retained=true;diagnostics.capture(*active,trace,now_ms());
        request_session_restart(*active);
    }
}
std::optional<RuntimeSnapshotRetry> snapshot_retry(const Active& active,const Queued& queued){
    if(!queued.snapshot||active.cancelled||active.retiring||!active.ready)return {};
    const auto scope=active.session.scope();
    if(scope.peer_id!=scope.coordinator_id||queued.revision!=scope.config_revision)return {};
    const auto& snapshot=*queued.snapshot;
    if(snapshot.owner!=scope.local_id||snapshot.boot!=scope.local_boot_id)return {};
    return RuntimeSnapshotRetry{scope.peer_id,scope.local_boot_id,scope.peer_boot_id,scope.link_id,scope.config_revision,snapshot.adapter};
}
void notify_snapshot_retry(const RuntimeSnapshotRetry& retry){
    RuntimeControlHooks hooks;{std::lock_guard<std::mutex> guard(lock);hooks=controls;}
    if(hooks.snapshot_retry)hooks.snapshot_retry(retry);
}
struct SnapshotRetryBatch {
    std::vector<RuntimeSnapshotRetry> requests;
    void dropped(const Active& active,const Queued& queued){if(auto retry=snapshot_retry(active,queued))requests.push_back(*retry);}
    // Declared before queue/global guards, so callbacks execute after unlock.
    ~SnapshotRetryBatch(){for(const auto& retry:requests)notify_snapshot_retry(retry);}
};
enum class QueuedState {ready,waiting,stale,invalid};
QueuedState prepare_queued(Active& active,const Queued& queued,std::string& payload){
    if(queued.revision!=active.session.scope().config_revision)return QueuedState::stale;
    if(queued.snapshot){payload="{\"snapshot\":"+owned_snapshot_json(*queued.snapshot)+"}";return QueuedState::ready;}
    if(!queued.owned){payload=queued.payload;return QueuedState::ready;}
    const auto& intent=*queued.owned;
    if(intent.config_revision!=queued.revision)return QueuedState::invalid;
    if(!active.context_ready&&intent.kind!=model::OwnedIntentKind::context&&intent.kind!=model::OwnedIntentKind::activity&&intent.kind!=model::OwnedIntentKind::discover&&!pure_key_release(intent))return QueuedState::waiting;
    payload=owned_intent_json(intent);
    return payload.empty()?QueuedState::invalid:QueuedState::ready;
}
void active_loop_work(void *arg){
    std::unique_ptr<std::shared_ptr<Active>> holder(static_cast<std::shared_ptr<Active>*>(arg));auto active=*holder;holder.reset();bool server=active->tls->server;
    LinkTrace trace;trace.phase="tls_handshake";const bool handshaken=active->tls->handshake();if(!handshaken)trace.fail("tls_handshake");
    if(!handshaken||!establish(*active,server,&trace)){if(!active->lifecycle_only&&!active->cancelled)error("Operational authentication failed");active->cancelled=true;}else{
        {std::lock_guard<std::mutex> guard(lock);status.connected=true;status.synchronized=false;status.phase="membership_committed";}
        if(!synchronize_connection(*active,server,active->execution_supported,0,&trace))active->cancelled=true;
        else{std::lock_guard<std::mutex> guard(lock);status.synchronized=active->ready.load();status.phase=active->ready?"enrollment_ready":"membership_committed";}
    }
    uint64_t receive_started=0,receive_until=0;bool retirement_attempted=false;
    while(!active->cancelled){
      while(!active->cancelled){
        poll_release_ack(*active);
        trace=LinkTrace{};trace.phase="operational";
        if(active->log_snapshot&&now_ms()>=active->log_expires)log_snapshot_clear(*active);
        if(active->restart_requested){trace.fail("application_restart_requested");break;}
        if(active->retiring&&!receive_until){retirement_attempted=true;finish_retirement(*active);break;}
        if(server&&active->ready&&!active->retiring&&!receive_until){
            uint64_t target;{std::lock_guard<std::mutex> guard(lock);target=configuration;}
            if(target>active->session.scope().config_revision){
                bool drain=false;{std::lock_guard<std::mutex> guard(active->mutex);
                    while(!active->queue.empty()&&active->queue.front().revision!=active->session.scope().config_revision&&active->queue.front().revision!=target)active->queue.pop_front();
                    drain=!active->queue.empty()&&active->queue.front().revision==active->session.scope().config_revision;}
                if(!drain&&!advance_configuration(*active,target,trace)){
                    if(!synchronization_recoverable(*active,trace)||!synchronize_connection(*active,true,true,target,&trace))break;
                }
            }
        }
        trace.phase="operational";trace.has_kind=trace.has_queue=false;
        if(server&&!active->retiring&&active->ready&&!receive_until&&!initialize_context(*active)){trace.fail("initial_context");break;}
        if(!active->retiring&&!report_wifi_signal(*active)){trace.fail("wifi_signal_send");break;}
        if(!active->retiring&&!refresh_local_hostname(*active)){trace.fail("hostname_send");break;}
        Queued queued;std::string payload;bool have=false,invalid=false;if(!active->retiring){std::lock_guard<std::mutex> guard(active->mutex);
            while(!active->queue.empty()&&active->queue.front().revision<active->session.scope().config_revision)active->queue.pop_front();
            if(!active->queue.empty()){
                const auto& front=active->queue.front();const bool expired=queued_expired(front,now_ms());
                const auto prepared=expired?QueuedState::ready:prepare_queued(*active,front,payload);
                if(prepared==QueuedState::invalid)invalid=true;
                if(prepared==QueuedState::ready||expired){queued=std::move(active->queue.front());active->queue.pop_front();have=true;}
            }}
        // Admission can add work while this task waits for the queue lock.
        // Expire and budget the observed item against the current clock.
        const auto now=now_ms();
        if(invalid){trace.fail("queued_invalid");break;}
        if(have&&queued_expired(queued,now)){
            if(auto retry=snapshot_retry(*active,queued))notify_snapshot_retry(*retry);
            else log_dropped_intent(queued,now);
        }
        if(have&&!queued_expired(queued,now)){
            // A press keeps its original deadline. Work that must not be lost
            // carries its full lifetime, which the receiver times from receipt.
            const auto ttl=queued_expires(queued)?queued.ttl-static_cast<uint32_t>(now-queued.created):queued.ttl;
            const bool key=queued.owned&&queued.owned->kind==model::OwnedIntentKind::key_edges&&queued.owned->edge_count>=1&&queued.owned->edge_count<=2;
            const auto write_started=key?now_ms():0;
            const auto network_before=key?network_diagnostics::snapshot():network_diagnostics::Snapshot{};
            const bool sent=send(*active,queued.kind,payload,ttl);
            if(key){
                const auto completed=now_ms();const auto& intent=*queued.owned;
                if(sent)begin_release_ack(*active,intent,completed,network_before);
                else emit_key_network(intent.target.port.octopus_id,intent.edges[0].key,intent.generation,intent.edge_count,intent.edges[0].down,"write_failed",network_before,network_diagnostics::snapshot());
                const char* phase=intent.edge_count==2?"tap":intent.edges[0].down?"down":"up";
                ESP_LOGI("consortium","%s stage=write owner=%s key=%u generation=%llu phase=%s edge_count=%u age_ms=%llu elapsed_ms=%llu sent=%d",
                    pure_key_release(intent)?"Key release":"Key transfer",intent.target.port.octopus_id.str().c_str(),static_cast<unsigned>(intent.edges[0].key),static_cast<unsigned long long>(intent.generation),phase,static_cast<unsigned>(intent.edge_count),
                    static_cast<unsigned long long>(write_started>=queued.created?write_started-queued.created:0),static_cast<unsigned long long>(completed>=write_started?completed-write_started:0),sent);
            }
            if(!sent){trace.queued(queued);trace.fail("queued_send");break;}
        }
        fd_set reads;FD_ZERO(&reads);FD_SET(active->tls->socket,&reads);timeval wait{0,20000};int ready=::select(active->tls->socket+1,&reads,nullptr,nullptr,&wait);if(ready<0){trace.socket_error=errno;trace.fail("socket_select");break;}
        // An idle connection has no receive deadline. Input fixes the start;
        // a decoded bulk prefix selects sixty seconds from that same edge.
        // Ordinary input, including an undecoded TLS record, retains five.
        if(!ready&&mbedtls_ssl_check_pending(&active->tls->ssl)==0&&!receive_until)continue;
        const auto polled_at=now_ms();if(!receive_until){receive_started=polled_at;receive_until=polled_at+5000;}
        if(active->tls->receiving_bulk())receive_until=receive_started+FrameReader::bulk_frame_timeout_ms;
        // Poll even on an idle socket while a frame is in flight, advancing its
        // existing progress/total deadlines without blocking the IPC pump.
        std::string text;const auto input=polled_at>=receive_until?active->tls->expire_receive():active->tls->poll_receive(text);
        if(input==TlsLink::ReceiveResult::pending)continue;
        if(input==TlsLink::ReceiveResult::failed){trace.fail("frame_receive");break;}
        receive_until=0;
        if(input==TlsLink::ReceiveResult::expired){
            trace.fail("frame_receive_expired");
            break;
        }
        Envelope envelope;const auto parsed=parse_envelope(text,envelope);if(parsed!=Error::none){trace.fail("envelope_parse",parsed);break;}const auto received=now_ms();trace.kind=envelope.kind;trace.has_kind=true;
        if(active->tls->received_bulk()!=is_update_kind(envelope.kind)){trace.fail("frame_class",Error::invalid_message);break;}
        if(envelope.kind==Kind::synchronization){
            const auto admitted=active->session.admit(envelope,received,received);
            if(admitted!=Error::none){trace.fail("synchronization_control_admit",admitted);break;}
            SynchronizationRound round;std::string phase;
            if(parse_synchronization(envelope.payload,round.id,phase,round.revision)!=Error::none){trace.fail("synchronization_control_parse",Error::invalid_message);break;}
            if(!server&&phase=="begin"){
                active->pending_synchronization=round;
                if(!synchronize_connection(*active,false,active->execution_supported,round.revision,&trace))break;
            }else if(!server&&phase=="advance"){
                if(!bind_configuration(*active,round.revision,trace)||!apply_configuration_advance(*active,false,round.revision,trace)){
                    if(!recover_operational_frame(*active,false,trace))break;
                }
            }else if(server&&phase=="reject"&&round.id==active->synchronization.id&&round.revision==active->synchronization.revision){
                if(!synchronize_connection(*active,true,active->execution_supported,round.revision,&trace))break;
            }
            continue;
        }
        if(envelope.config_revision<active->session.scope().config_revision){
            const auto discarded=active->session.discard_stale(envelope,received,received);
            if(discarded==Error::none)continue;
            trace.fail("stale_frame",discarded);break;
        }
        if(server){bool enrolled=false;{std::lock_guard<std::mutex> guard(lock);for(const auto &peer:group.peers)if(peer.id==active->peer&&peer.active)enrolled=true;}if(!enrolled){if(active->retiring){retirement_attempted=true;finish_retirement(*active);trace.reason="retired";}else trace.fail("membership_revoked");break;}}
        if(!server&&envelope.kind==Kind::membership_event){const auto admitted=active->session.admit(envelope,received,received);if(admitted!=Error::none){trace.fail("retirement_admit",admitted);break;}trace.phase="retirement";if(!retire_member(*active,envelope))trace.fail("retirement");else trace.reason="retired";break;}

        if(envelope.kind==Kind::device_status){
            if(!receive_live_device_status(*active,envelope,received)){trace.fail("device_status_admission");break;}
            continue;
        }
        if(is_log_kind(envelope.kind)){
            const auto admitted=active->session.admit(envelope,received,received);
            if(admitted!=Error::none){trace.fail("log_admit",admitted);break;}
            if(!log_dispatch(active,envelope,received)){trace.fail("log_dispatch");break;}
            continue;
        }
        if(is_settings_kind(envelope.kind)){
            // Preserve wire/security failures as terminal before the settings
            // helper's boolean result can also represent a raced local reset
            // or failed application delivery on a healthy transport.
            const auto admitted=active->session.admit(envelope,received,received);
            if(admitted!=Error::none){trace.fail("settings_admit",admitted);break;}
            if(envelope.kind==Kind::settings_write){
                DeviceSettingsWrite write;
                if(parse_device_settings_write(*envelope.payload.get("settings"),write)!=Error::none){trace.fail("settings_admit",Error::invalid_message);break;}
                if(write.expected_boot!=boot){trace.fail("settings_boot",Error::stale_boot);break;}
            }else if(envelope.kind==Kind::settings_result&&envelope.payload.get("stage")->string=="applied"&&envelope.payload.get("snapshot")->get("boot_id")->string!=active->session.scope().peer_boot_id.str()){
                trace.fail("settings_boot",Error::stale_boot);break;
            }
            bool allowed=false;
            {std::lock_guard<std::mutex> guard(lock);allowed=envelope.kind==Kind::settings_result?
                (original_controller(group,identity.id)&&status.authority_ready&&settings_enrolled(active->peer)):
                (group.role==Group::member&&group.authority==active->peer&&current_peer_credential(*active,group));}
            if(!allowed){trace.fail("settings_authority",Error::unauthorized_role);break;}
            const bool dispatched=settings_dispatch(active,envelope,received);
            if(active->restart_requested){trace.fail("application_restart_requested");break;}
            if(!dispatched){trace.fail("settings_dispatch");break;}continue;
        }
        if(is_update_kind(envelope.kind)) {
            const auto admitted=active->session.admit(envelope,received,received);if(admitted!=Error::none){trace.fail("update_admit",admitted);break;}
            if(!active->ready&&!active->management_ready){trace.fail("update_not_ready");break;}
            RuntimeUpdateHooks hooks; bool allowed=false;
            {std::lock_guard<std::mutex> guard(lock);hooks=updates;
                allowed=envelope.kind==Kind::update_result ? (server&&group.role==Group::coordinator&&status.authority_ready) :
                    (!server&&group.role==Group::member&&group.authority==active->peer);}
            if(!allowed){trace.fail("update_authority",Error::unauthorized_role);break;}
            if(envelope.kind==Kind::update_result) {if(hooks.result) hooks.result(active->peer,envelope.payload);}
            else {
                const auto handled=hooks.handle?hooks.handle(active->peer,envelope):Error::not_ready;
                if(envelope.kind==Kind::update_chunk&&handled!=Error::none){trace.fail("update_stream",handled);break;}
                if(handled==Error::none) {
                    if(envelope.kind==Kind::update_begin) active->update_begin_id=*model::MessageId::parse(envelope.payload.get("request_id")->string);
                    if(envelope.kind==Kind::update_install||envelope.kind==Kind::update_abort) active->update_begin_id={};
                }
            }
        }
        else{
            const auto admitted=active->session.admit(envelope,received,received);
            if(admitted!=Error::none){trace.fail("control_admit",admitted);break;}
            if(active->restart_requested){trace.fail("application_restart_requested");break;}
            RuntimeControlHooks hooks;{std::lock_guard<std::mutex> guard(lock);hooks=controls;}
            uint64_t deadline=0;bool key_cleanup=false;
            const bool intent=envelope.kind==Kind::scoped_intent;
            const bool selection=intent&&(envelope.intent->kind==model::OwnedIntentKind::context||envelope.intent->kind==model::OwnedIntentKind::activity);
            if(intent){
                key_cleanup=pure_key_release(*envelope.intent);
                if(received>UINT64_MAX-envelope.ttl_ms)continue;
                deadline=received+envelope.ttl_ms;
            }
            if(hooks.control){
                const auto result=hooks.control(active->session.scope(),envelope,deadline);
                if(active->restart_requested){trace.fail("application_restart_requested");break;}
                if(!server&&result==Error::none&&selection){active->context_ready=true;active->refreshing=false;}
                if(result!=Error::none&&(envelope.kind==Kind::scoped_result||key_cleanup||selection)){trace.fail("control_dispatch",result);break;}
            }else if(envelope.kind==Kind::scoped_result||envelope.kind==Kind::scoped_intent){
                trace.fail("control_dispatch",Error::not_ready);break;
            }
        }
      }
      finish_release_ack(*active);
      if(active->cancelled||active->retiring||!application_failure_recoverable(*active,trace))break;
      trace.retained=true;trace.receive=active->tls->receive_diagnostic();
      emit_link_trace(snapshot_link_trace(*active,trace,now_ms()));
      if(!recover_operational_frame(*active,server,trace))break;
      receive_until=0;
    }
    if(server&&active->retiring&&!retirement_attempted)finish_retirement(*active);
    log_snapshot_clear(*active);
    if(active->update_begin_id) {RuntimeUpdateHooks hooks;{std::lock_guard<std::mutex> guard(lock);hooks=updates;}if(hooks.disconnected) hooks.disconnected(active->peer,active->update_begin_id);}
    if(!trace.unexpected&&active->retiring)trace.reason="retired";
    trace.retained=false;
    trace.receive=active->tls->receive_diagnostic();
    trace_link_closed(*active,trace,now_ms());
    active->cancelled=true;
    const bool fenced=notify_link_closed(*active);
    active->session.disconnect();::shutdown(active->tls->socket,SHUT_RDWR);
    {std::lock_guard<std::mutex> guard(lock);if(fenced)links.erase(std::remove(links.begin(),links.end(),active),links.end());status.connected=std::any_of(links.begin(),links.end(),[](const auto& link){return !link->cancelled;});status.synchronized=false;}
    // Return through C++ so the final shared_ptr frees TLS/certificate state.
}
void active_loop(void *arg){active_loop_work(arg);vTaskDeleteWithCaps(nullptr);}
bool connection_available(model::OctopusId peer){
    std::lock_guard<std::mutex> guard(lock);
    return std::any_of(links.begin(),links.end(),[&](const auto& link){return !link->cancelled&&link->tls&&link->tls->usable()&&(link->connection_peer==peer||link->peer==peer);});
}
void launch(std::shared_ptr<TlsLink> tls,std::string endpoint_hint={},model::OctopusId expected={}){
    auto active=std::make_shared<Active>();active->tls=std::move(tls);active->endpoint_hint=std::move(endpoint_hint);active->connection_peer=expected;
    {std::lock_guard<std::mutex> guard(lock);
        if(links.size()>=maximum_connection_slots)return;
        if(expected&&std::any_of(links.begin(),links.end(),[&](const auto& link){return !link->cancelled&&link->tls&&link->tls->usable()&&(link->connection_peer==expected||link->peer==expected);}))return;
        links.push_back(active);
    }
    auto arg=new std::shared_ptr<Active>(active);
    if(xTaskCreateWithCaps(active_loop,"group_link",16384,arg,3,nullptr,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)!=pdPASS){delete arg;std::lock_guard<std::mutex> guard(lock);links.erase(std::remove(links.begin(),links.end(),active),links.end());}
}
bool connect_bounded(int fd,const sockaddr *address,socklen_t length){
    const auto flags=fcntl(fd,F_GETFL,0);if(flags<0||fcntl(fd,F_SETFL,flags|O_NONBLOCK)<0)return false;
    int result=::connect(fd,address,length);if(result<0&&errno!=EINPROGRESS)return false;
    if(result<0){fd_set writes;FD_ZERO(&writes);FD_SET(fd,&writes);timeval timeout{5,0};if(::select(fd+1,nullptr,&writes,nullptr,&timeout)<=0)return false;int error=0;socklen_t size=sizeof(error);if(getsockopt(fd,SOL_SOCKET,SO_ERROR,&error,&size)||error)return false;}
    return fcntl(fd,F_SETFL,flags)==0;
}
void ensure_connection(const Identity& local,const Group& current,const std::string& endpoint){
    // Acquiring a connection is independent of application synchronization.
    // Serialize acquisition so even simultaneous requests share the same
    // pending handshake rather than creating competing sockets.
    std::lock_guard<std::mutex> acquisition(connection_lock);
    if(connection_available(current.authority)||endpoint.empty())return;
    addrinfo hints{};hints.ai_socktype=SOCK_STREAM;hints.ai_family=AF_INET;hints.ai_flags=AI_NUMERICHOST;addrinfo *addresses=nullptr;
    if(getaddrinfo(endpoint.c_str(),"7443",&hints,&addresses)!=0)return;
    int fd=::socket(addresses->ai_family,SOCK_STREAM,0);
    BoundedSetupTimings<1> timing("network",now_ms);timing.peer=current.authority;
    if(fd>=0&&timing.step("tcp_connect",[&]{return connect_bounded(fd,addresses->ai_addr,addresses->ai_addrlen);})){auto tls=std::make_shared<TlsLink>();if(tls->setup(local,current.peers,false,fd,current.authority))launch(tls,endpoint,current.authority);}
    else if(fd>=0)::close(fd);
    freeaddrinfo(addresses);
}
constexpr const char *group_service="_amity-group";
void advertise_group_hint(const Group &current){
    static std::string advertised;
    if(current.role!=Group::coordinator){if(!advertised.empty()){mdns_service_remove(group_service,"_tcp");advertised.clear();}return;}
    const auto key=current.consortium.str()+current.authority.str();if(key==advertised)return;
    const auto group_id=current.consortium.str(),coordinator=current.authority.str();
    mdns_txt_item_t items[]={{"group",group_id.c_str()},{"coordinator",coordinator.c_str()}};
    const auto instance="Amity System "+coordinator.substr(24);
    if(!advertised.empty())mdns_service_remove(group_service,"_tcp");
    if(mdns_service_add(instance.c_str(),group_service,"_tcp",7443,items,2)==ESP_OK)advertised=key;else advertised.clear();
}
std::vector<std::string> discover_group_hints(const Group &current){
    std::vector<std::string> hints;mdns_result_t *results=nullptr;
    if(mdns_query_ptr(group_service,"_tcp",2000,8,&results)!=ESP_OK)return hints;
    size_t visited=0;
    for(auto *entry=results;entry&&visited++<8;entry=entry->next){
        if(entry->port!=7443||entry->txt_count>8||!entry->txt||!entry->txt_value_len)continue;
        std::string group_id,coordinator;bool duplicate=false;
        for(size_t i=0;i<entry->txt_count;++i){const auto &item=entry->txt[i];if(!item.key||!item.value||entry->txt_value_len[i]>64){duplicate=true;break;}std::string *target=nullptr;if(!strcmp(item.key,"group"))target=&group_id;else if(!strcmp(item.key,"coordinator"))target=&coordinator;if(target){if(!target->empty()){duplicate=true;break;}target->assign(item.value,entry->txt_value_len[i]);}}
        if(duplicate||group_id!=current.consortium.str()||coordinator!=current.authority.str())continue;
        size_t addresses=0;for(auto *address=entry->addr;address&&addresses++<8&&hints.size()<2;address=address->next){if(address->addr.type!=ESP_IPADDR_TYPE_V4||!address->addr.u_addr.ip4.addr)continue;char text[16];snprintf(text,sizeof(text),IPSTR,IP2STR(&address->addr.u_addr.ip4));if(std::find(hints.begin(),hints.end(),text)==hints.end())hints.emplace_back(text);}
    }
    mdns_query_results_free(results);return hints;
}
void defer_radio_release(uint64_t generation){if(!generation)return;std::lock_guard<std::mutex> guard(lock);pending_radio_release=generation;}
bool local_ipv4_ready(){
    auto* station=esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");esp_netif_ip_info_t address{};
    return station&&esp_netif_is_netif_up(station)&&esp_netif_get_ip_info(station,&address)==ESP_OK&&address.ip.addr;
}
void network_loop(void*){
    uint32_t backoff=1000;uint64_t next_discovery=0,ipv4_wait_at=0;size_t next_hint=0;std::vector<std::string> discovered;bool ipv4_waiting=false;
    while(true){Group current;Identity local;bool may_connect;{std::lock_guard<std::mutex> guard(lock);current=group;local=identity;may_connect=current.role!=Group::prepared||(candidate_passive&&gatt_candidate_transport_idle());}
        advertise_group_hint(current);
        reap_closed_links();
        if(current.role==Group::coordinator){if(listener<0){int fd=::socket(AF_INET,SOCK_STREAM,0);int yes=1;setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof(yes));sockaddr_in addr{};addr.sin_family=AF_INET;addr.sin_port=htons(7443);addr.sin_addr.s_addr=INADDR_ANY;if(fd<0||::bind(fd,reinterpret_cast<sockaddr*>(&addr),sizeof(addr))||::listen(fd,2)){if(fd>=0)::close(fd);vTaskDelay(pdMS_TO_TICKS(1000));continue;}listener=fd;timeval timeout{0,200000};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));}
            int fd=::accept(listener,nullptr,nullptr);if(fd>=0){BoundedSetupTimings<1> timing("network",now_ms);const auto accepted=timing.begin("tcp_accepted");timing.finish(accepted,true);auto tls=std::make_shared<TlsLink>();if(tls->setup(local,current.peers,true,fd))launch(tls);}
        }else if((current.role==Group::member||current.role==Group::prepared)&&may_connect){
            // Enrollment and boot synchronization need the authority's data;
            // acquire its connection only if none is already available.
            const bool connected=connection_available(current.authority);
            if(connected)backoff=1000;
            if(!connected&&!current.peers.empty()){
                // DHCP may still be running after an early headless boot.
                // Do not turn a missing local address into a failed peer hint,
                // discovery query or retry backoff. Other loop work still runs.
                if(!local_ipv4_ready()){
                    if(!ipv4_waiting){ipv4_waiting=true;ipv4_wait_at=now_ms();}
                }else{
                    if(ipv4_waiting){
                        SetupTimingEntry waited;waited.stage="local_ipv4_wait";waited.at_ms=now_ms();waited.elapsed_ms=waited.at_ms>=ipv4_wait_at?waited.at_ms-ipv4_wait_at:0;waited.result="ok";waited.finished=true;
                        emit_setup_timing("network",waited,current.authority,{},{});ipv4_waiting=false;
                    }
                    // The transaction already supplies a numeric authority hint.
                    // Try it immediately once; failure still reaches the existing
                    // bounded discovery/rotation path. Only pinned TLS grants trust.
                    const bool first_known=next_hint==0&&!current.authority_host.empty();
                    if(!first_known&&now_ms()>=next_discovery){discovered=discover_group_hints(current);next_discovery=now_ms()+10000;}
                    std::vector<std::string> candidates;if(!current.authority_host.empty())candidates.push_back(current.authority_host);
                    for(const auto &hint:discovered)if(std::find(candidates.begin(),candidates.end(),hint)==candidates.end())candidates.push_back(hint);
                    const auto endpoint=candidates.empty()?std::string{}:candidates[next_hint++%candidates.size()];
                    ensure_connection(local,current,endpoint);
                    vTaskDelay(pdMS_TO_TICKS(backoff+(uint8_t(random_hex(1)[0])%100)));backoff=std::min<uint32_t>(backoff*2,10000);
                }
            }
        }
        uint64_t generation=0;{std::lock_guard<std::mutex> guard(lock);if(candidate_deadline&&now_ms()>=candidate_deadline)generation=candidate_generation;}
        if(generation)runtime_close_enrollment();
        if(gatt_quiescent()){RuntimeRadioHooks hooks;uint64_t release=0;{std::lock_guard<std::mutex> guard(lock);release=pending_radio_release;pending_radio_release=0;hooks=radio;}if(release&&hooks.release)hooks.release(release);}
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
std::string proposal_json(model::JoinId join,model::ConsortiumId consortium,const Identity& coordinator,const Peer& candidate,uint64_t revision,const std::string &host,const std::string &hostname){
    // The boot identity and this transaction's just-verified candidate already
    // contain their derived pins. Copy those exact records, without repeating
    // signatures or trusting pins supplied by an unauthenticated document.
    JoinProposal p;p.join_id=join;p.consortium_id=consortium;p.coordinator_id=coordinator.id;p.candidate_id=candidate.id;p.revision=revision;p.coordinator_leaf=coordinator.der;p.candidate_leaf=candidate.der;p.coordinator_pin=coordinator.spki;p.candidate_pin=candidate.pin;p.host=host;p.candidate_hostname=hostname;
    return encode_join_proposal(p);
}
std::string host_ip(){auto iface=esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");esp_netif_ip_info_t ip{};if(!iface||esp_netif_get_ip_info(iface,&ip)!=ESP_OK||!ip.ip.addr)return {};char text[16];snprintf(text,sizeof(text),IPSTR,IP2STR(&ip.ip));return text;}
esp_err_t bootstrap_request(std::string_view request,std::string &response){
    Json p;JsonError err;if(!parse_json(request,p,err)||!p.get("phase")||p.get("phase")->type!=Json::Type::string)return ESP_ERR_INVALID_ARG;
    std::unique_lock<std::mutex> persistence(persistence_lock);
    std::unique_lock<std::mutex> guard(lock);if(!candidate_deadline||now_ms()>=candidate_deadline)return ESP_ERR_INVALID_STATE;
    if(p.get("phase")->string=="identify"){if(!closed(p,{"phase"}))return ESP_ERR_INVALID_ARG;if(!network.set_hostname||!network.get_hostname)return ESP_ERR_NOT_SUPPORTED;response="{\"phase\":\"identity\",\"octopus_id\":\""+identity.id.str()+"\",\"leaf\":\""+hex(identity.der)+"\",\"networked\":"+(host_ip().empty()?"false":"true")+"}";return ESP_OK;}
    if(p.get("phase")->string=="wifi"){
        model::JoinId join;std::string digest,ssid,password;
        if(!closed(p,{"phase","join_id","digest","ssid","password"})||!id(p.get("join_id"),join)||!json_string(p.get("digest"),digest,64,64)||group.role!=Group::prepared||join!=group.join_id||digest!=group.join_digest||p.get("ssid")->type!=Json::Type::string||p.get("password")->type!=Json::Type::string||!unhex(p.get("ssid")->string,ssid,32)||ssid.empty()||!unhex(p.get("password")->string,password,64))return ESP_ERR_INVALID_ARG;
        if(!host_ip().empty()){response="{\"phase\":\"networked\",\"ready\":true}";return ESP_OK;}
        auto hooks=network;if(!hooks.stage_connect||!hooks.commit)return ESP_ERR_NOT_SUPPORTED;
        status.phase="connecting_wifi";guard.unlock();persistence.unlock();
        // Commit as soon as the station has an address. The durable prepared
        // decision already binds this candidate to the main; there is no
        // earlier network to return to if the join does not finish.
        auto result=hooks.stage_connect(ssid,password);
        std::fill(password.begin(),password.end(),0);password.clear();
        if(result==ESP_OK&&host_ip().empty())result=ESP_ERR_INVALID_STATE;
        if(result==ESP_OK)result=hooks.commit();
        if(result!=ESP_OK&&hooks.rollback)hooks.rollback();
        guard.lock();status.phase=result==ESP_OK?"finishing_enrollment":"network_failed";
        if(result!=ESP_OK)return result;
        response="{\"phase\":\"networked\",\"ready\":true}";return ESP_OK;
    }
    if(p.get("phase")->string!="prepare"||!closed(p,{"phase","proposal","digest"})||p.get("proposal")->type!=Json::Type::string||p.get("digest")->type!=Json::Type::string)return ESP_ERR_INVALID_ARG;
    const auto proposal=p.get("proposal")->string;if(sha256(proposal)!=p.get("digest")->string)return ESP_ERR_INVALID_ARG;
    JoinProposal decoded;if(!decode_join_proposal(proposal,decoded)||decoded.candidate_id!=identity.id||decoded.candidate_leaf!=identity.der||decoded.candidate_pin!=identity.spki)return ESP_ERR_INVALID_ARG;
    Group next;Peer authority;next.join_id=decoded.join_id;next.join_hostname=decoded.candidate_hostname;next.consortium=decoded.consortium_id;next.authority=decoded.coordinator_id;next.revision=decoded.revision;next.authority_host=decoded.host;authority.id=next.authority;authority.der=decoded.coordinator_leaf;
    if(!certificate(authority.der,authority.id,authority.pin)||authority.pin!=decoded.coordinator_pin)return ESP_ERR_INVALID_ARG;
    next.role=Group::prepared;next.join_digest=p.get("digest")->string;next.join_revision=next.revision;next.joining_id=identity.id;next.peers.push_back(authority);
    if(group.role!=Group::unclaimed&&!(group.role==Group::prepared&&group.join_id==next.join_id&&group.join_digest==next.join_digest))return ESP_ERR_INVALID_STATE;
    if(!network.set_hostname||!network.get_hostname)return ESP_ERR_NOT_SUPPORTED;
    const auto network_hooks=network;
    const auto named=without_runtime_lock(guard,[&]{const auto result=network_hooks.set_hostname(decoded.candidate_hostname);return result==ESP_OK&&network_hooks.get_hostname()!=decoded.candidate_hostname?ESP_ERR_INVALID_STATE:result;});if(named!=ESP_OK)return named;
    auto saved=without_runtime_lock(guard,[&]{return save_group(next);});if(saved!=ESP_OK)return saved;group=std::move(next);status.member=true;status.authority_ready=false;status.phase="finishing_enrollment";candidate_passive=false;auto hooks=lifecycle;const auto prepared_id=group.join_id;const auto prepared_digest=group.join_digest;guard.unlock();persistence.unlock();const auto fenced=hooks.retire?hooks.retire():ESP_ERR_INVALID_STATE;guard.lock();if(group.join_id!=prepared_id||group.join_digest!=prepared_digest||fenced!=ESP_OK)return ESP_ERR_INVALID_STATE;candidate_passive=true;gatt_server_seal();response="{\"phase\":\"prepared\",\"join_id\":\""+group.join_id.str()+"\",\"digest\":\""+group.join_digest+"\",\"hostname\":\""+decoded.candidate_hostname+"\"}";return ESP_OK;
}
void admission_failed(model::MessageId scan,const std::string& failure){
    std::lock_guard<std::mutex> guard(lock);
    if(admission.view.scan_id!=scan)return;
    // A lost BLE reply cannot undo a prepared transaction. The exact pinned
    // LAN path may still finish it; report uncertainty until that is known.
    const bool prepared=group.join_id&&group.joining_id==admission.view.candidate_id&&group.join_hostname==admission.hostname;
    const bool committed=hostnames.lookup(admission.view.candidate_id)==admission.hostname&&
        std::any_of(group.peers.begin(),group.peers.end(),[&](const auto& peer){return peer.id==admission.view.candidate_id&&peer.active;});
    if(prepared||committed){
        admission.view.phase="finishing";
        admission.view.error="Setup could not be confirmed yet. Check the Amity devices list before trying again.";
        admission.deadline=0;status.phase="finishing_enrollment";
        return;
    }
    admission.view.phase=invitation_cancelled?"cancelled":"failed";
    admission.view.error=failure;
    status.phase="enrollment_failed";status.last_error=failure;

}
void invite_worker_work(void *arg){
    BoundedSetupTimings<13> timing("invite",now_ms);
    std::unique_ptr<InviteRequest> invite(static_cast<InviteRequest*>(arg));
    const auto scan=invite->scan_id;invite.reset();timing.scan=scan;
    uint64_t generation=0,deadline=0;RuntimeRadioHooks hooks;
    {std::lock_guard<std::mutex> guard(lock);hooks=radio;}
    const bool reserved=!invitation_cancelled&&hooks.reserve&&hooks.reserve(true,generation)==ESP_OK;
    if(!reserved){admission_failed(scan,"Adding an Amity could not start. Finish the current setup operation and try again.");return;}
    // All paths return through C++ and release the exact reserved radio owner.
    struct Cleanup {uint64_t generation;~Cleanup(){gatt_client_disconnect();defer_radio_release(generation);}} cleanup{generation};
    {std::lock_guard<std::mutex> guard(lock);if(admission.view.scan_id!=scan||invitation_cancelled)return;admission.generation=generation;admission.deadline=deadline=now_ms()+enrollment_window_ms;}
    auto current=[&](){return !invitation_cancelled&&now_ms()<deadline;};
    while(current()){
        std::vector<RuntimeAdmissionCandidate> candidates;
        const auto discovered=gatt_client_discover(generation,candidates);
        if(!current())break;
        if(discovered!=ESP_OK||candidates.empty()){vTaskDelay(pdMS_TO_TICKS(100));continue;}
        {std::lock_guard<std::mutex> guard(lock);if(admission.view.scan_id!=scan||invitation_cancelled)return;admission.view.candidates=std::move(candidates);admission.view.phase="found";}
        break;
    }
    std::string token,hostname;bool submitted=false;
    while(current()){
        std::vector<RuntimeAdmissionCandidate> candidates;
        if(gatt_client_candidates(generation,candidates)!=ESP_OK)break;
        {std::lock_guard<std::mutex> guard(lock);if(admission.view.scan_id!=scan)return;if(admission.submitted){token=admission.selected_token;hostname=admission.hostname;submitted=true;break;}if(invitation_cancelled)break;admission.view.candidates=std::move(candidates);admission.view.phase="found";}
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if(!submitted){admission_failed(scan,invitation_cancelled?"Adding this Amity was cancelled.":"No Amity was added before setup expired. Open Add Amity and try again.");return;}
    const auto selected=timing.begin("worker_selected");timing.finish(selected,true);timing.begin("selected_total");
    if(!current()){
        admission_failed(scan,invitation_cancelled?"Adding this Amity was cancelled.":"No Amity was added before setup expired. Open Add Amity and try again.");return;
    }
    bool good=current()&&timing.step("gatt_connect",[&]{return gatt_client_connect_candidate(token,generation)==ESP_OK;});
    AdmissionClient client;
    good=good&&timing.step("crypto_handshake",[&]{return client.handshake([&](std::string_view bytes,std::string& reply){return current()&&gatt_client_exchange(false,bytes,reply)&&current();});});
    auto secure=[&](std::string_view plain,std::string& result){std::string encrypted,reply;return current()&&client.encrypt(plain,encrypted)&&gatt_client_exchange(true,encrypted,reply)&&current()&&client.decrypt(reply,result);};
    std::string response,failure;Json identity_json;JsonError err;Peer peer;bool networked=false;
    good=good&&timing.step("identify_rpc",[&]{return secure("{\"phase\":\"identify\"}",response);});
    if(good){
        const auto validation=timing.begin("peer_validation");
        good=parse_json(response,identity_json,err)&&closed(identity_json,{"phase","octopus_id","leaf","networked"})&&id(identity_json.get("octopus_id"),peer.id)&&peer.id.str()==client.peer_id()&&identity_json.get("leaf")->type==Json::Type::string&&unhex(identity_json.get("leaf")->string,peer.der,1024)&&certificate(peer.der,peer.id,peer.pin)&&json_bool(identity_json.get("networked"),networked);
        timing.peer=peer.id;timing.finish(validation,good);
    }
    Group current_group;Identity local;
    {std::lock_guard<std::mutex> guard(lock);current_group=group;local=identity;if(good&&admission.view.scan_id==scan){admission.view.candidate_id=peer.id;if(hostnames.lookup(peer.id).empty()&&hostnames.entries().size()>=HostnameRegistry::capacity)good=false;}}
    const auto join=random_id<model::JoinId>();std::string proposal,digest;
    if(good){
        const auto proposal_stage=timing.begin("proposal_store");
        const auto host=host_ip();good=current()&&!host.empty()&&current_group.role==Group::coordinator&&current_group.peers.size()<maximum_roster_peers&&current_group.revision<UINT64_MAX;
        for(const auto& old:current_group.peers)if(old.id==peer.id||old.pin==peer.pin)good=false;
        if(good){
            proposal=proposal_json(join,current_group.consortium,local,peer,current_group.revision+1,host,hostname);digest=sha256(proposal);
            current_group.join_id=join;current_group.joining_id=peer.id;current_group.join_digest=digest;current_group.join_hostname=hostname;current_group.join_revision=current_group.revision+1;current_group.peers.push_back(peer);
            std::lock_guard<std::mutex> persistence(persistence_lock);std::unique_lock<std::mutex> guard(lock);
            good=current()&&!proposal.empty()&&admission.view.scan_id==scan&&group.role==Group::coordinator&&!group.join_id&&group.revision+1==current_group.join_revision&&without_runtime_lock(guard,[&]{return save_group(current_group);})==ESP_OK;
            if(good)group=current_group;
        }
        timing.finish(proposal_stage,good);
    }
    if(good){std::string quoted;quoted.reserve(proposal.size()+128);for(char c:proposal){if(c=='"'||c=='\\')quoted+='\\';quoted+=c;}good=timing.step("prepare_rpc",[&]{return secure("{\"phase\":\"prepare\",\"proposal\":\""+quoted+"\",\"digest\":\""+digest+"\"}",response);});}
    if(good){
        const auto acknowledged=timing.begin("prepare_ack");
        Json prepared;good=parse_json(response,prepared,err)&&closed(prepared,{"phase","join_id","digest","hostname"})&&prepared.get("phase")->type==Json::Type::string&&prepared.get("phase")->string=="prepared"&&prepared.get("join_id")->type==Json::Type::string&&prepared.get("join_id")->string==join.str()&&prepared.get("digest")->type==Json::Type::string&&prepared.get("digest")->string==digest&&prepared.get("hostname")->type==Json::Type::string&&prepared.get("hostname")->string==hostname;
        if(good){std::lock_guard<std::mutex> persistence(persistence_lock);std::unique_lock<std::mutex> guard(lock);good=group.join_id==join&&group.join_digest==digest&&group.joining_id==peer.id&&remember_hostname(peer.id,hostname,guard)==Error::none;}
        timing.finish(acknowledged,good);
    }
    if(good&&!networked){
        const auto wifi=timing.begin("wifi_rpc");
        RuntimeNetworkHooks network_hooks;{std::lock_guard<std::mutex> guard(lock);network_hooks=network;status.phase="connecting_wifi";}
        std::string ssid,password;good=network_hooks.export_credentials&&network_hooks.export_credentials(ssid,password)&&!ssid.empty()&&ssid.size()<=32&&password.size()<=64;
        if(good)good=secure("{\"phase\":\"wifi\",\"join_id\":\""+current_group.join_id.str()+"\",\"digest\":\""+digest+"\",\"ssid\":\""+hex(ssid)+"\",\"password\":\""+hex(password)+"\"}",response);
        mbedtls_platform_zeroize(password.data(),password.size());timing.finish(wifi,good);
    }
    if(!good){admission_failed(scan,failure.empty()?"Adding this Amity was interrupted. Check its setup window, firmware and connection, then try again.":failure);return;}
    {std::lock_guard<std::mutex> guard(lock);if(admission.view.scan_id==scan){admission.view.phase="finishing";admission.deadline=0;}status.phase="finishing_enrollment";}
    const auto finishing=timing.begin("finishing");timing.finish(finishing,true);timing.done(true);
}
void invite_worker(void *arg){invite_worker_work(arg);invitation_running=false;vTaskDeleteWithCaps(nullptr);}
esp_err_t recovery_required(model::OctopusId local,esp_err_t result,const char *reason){
    identity.id=local;status.initialized=true;status.authority_ready=false;status.coordinator=false;status.member=true;status.credential_ready=false;status.phase="recovery_required";status.last_error=reason;
    return result;
}
// Each caller first initializes PSA and fully validates its own freshly loaded
// Group. Reuse only this call's exact verified identity; never cache a boot
// preflight across calls or generate an identity during an unclaimed preflight.
esp_err_t validate_startup(model::OctopusId local,const Group& stored,RuntimeStartup& out,Identity* verified_identity=nullptr,BoundedSetupTimings<5>* timing=nullptr){
    out={};if(verified_identity)*verified_identity={};
    if(!local){out.role=RuntimeStartup::recovery_required;return ESP_ERR_INVALID_STATE;}
    if(stored.role==Group::unclaimed)return ESP_OK;
    if(stored.peers.size()>maximum_roster_peers){out.role=RuntimeStartup::recovery_required;return ESP_ERR_INVALID_SIZE;}
    if((stored.role==Group::member||stored.role==Group::prepared)&&stored.joining_id!=local){out.role=RuntimeStartup::recovery_required;return ESP_ERR_INVALID_STATE;}
    Identity verified;const auto identity_stage=timing?timing->begin("identity_load"):0;
    const auto loaded=platform::load_identity(local,verified);
    if(timing)timing->finish(identity_stage,loaded==ESP_OK,1);
    if(loaded!=ESP_OK||(stored.role==Group::coordinator&&stored.authority!=local)){out.role=RuntimeStartup::recovery_required;return ESP_ERR_INVALID_STATE;}
    if(verified_identity)*verified_identity=std::move(verified);
    out.role=static_cast<RuntimeStartup::Role>(stored.role);out.consortium_id=stored.consortium;out.authority_id=stored.authority;out.membership_revision=stored.revision;
    return ESP_OK;
}
}
esp_err_t runtime_read_startup(model::OctopusId local,RuntimeStartup &out){
    out={};if(!local||psa_crypto_init()!=PSA_SUCCESS){out.role=RuntimeStartup::recovery_required;return ESP_ERR_INVALID_STATE;}
    Group stored;auto result=load_group(stored);
    if(result!=ESP_OK){out.role=RuntimeStartup::recovery_required;return result;}
    return validate_startup(local,stored,out);
}
esp_err_t runtime_set_hardware_profile(uint8_t count,uint8_t mask){
    if(!count||count>amity::max_tentacles||mask>=(1u<<count))return ESP_ERR_INVALID_ARG;
    std::lock_guard<std::mutex> guard(lock);if(initialized.load())return ESP_ERR_INVALID_STATE;
    hardware_tentacles=count;hardware_tentacles_mask=mask;return ESP_OK;
}
esp_err_t runtime_start(model::ConsortiumId consortium,model::OctopusId octopus,model::BootId current_boot,uint64_t config){
    BoundedSetupTimings<5> timing("startup",now_ms);timing.peer=octopus;timing.begin("total");
    if(!consortium||!octopus||!current_boot||!config||initialized.exchange(true))return ESP_ERR_INVALID_STATE;
    if(psa_crypto_init()!=PSA_SUCCESS)return recovery_required(octopus,ESP_FAIL,"Crypto initialization failed; factory reset required");
    const auto group_stage=timing.begin("load_group");auto result=load_group(group);timing.finish(group_stage,result==ESP_OK,1);
    if(result!=ESP_OK)return recovery_required(octopus,result,"Saved membership is unreadable; factory reset required");
    RuntimeStartup startup;const auto validation_stage=timing.begin("startup_validation");
    const auto validated=validate_startup(octopus,group,startup,&identity,&timing);timing.finish(validation_stage,validated==ESP_OK,1);
    if(validated!=ESP_OK)return recovery_required(octopus,ESP_ERR_INVALID_STATE,"Saved membership is invalid; factory reset required");
    if(group.role==Group::unclaimed){
        const auto identity_stage=timing.begin("identity_load");result=platform::load_identity(octopus,identity,true);timing.finish(identity_stage,result==ESP_OK,1);
        if(result!=ESP_OK)return recovery_required(octopus,result,"Saved credentials are unreadable; factory reset required");
    }
    singleton=consortium;boot=current_boot;configuration=config;
    std::string name_record;const auto name_result=read_record("amity_group","hostnames",name_record,8192);if(name_result!=ESP_ERR_NVS_NOT_FOUND&&(name_result!=ESP_OK||!hostnames.restore(name_record))){hostnames={};status.last_error="Saved device names are unavailable. Names will refresh when devices reconnect.";}
    // Operational startup validates PSA and stored identity/authority. The
    // admission self-test runs only during an explicit Add attempt; it cannot
    // delay boot or prevent the independent Matter setup from choosing a main.
    status.initialized=status.credential_ready=true;status.authority_ready=original_controller(group,identity.id);status.coordinator=group.role==Group::coordinator&&status.authority_ready;status.member=group.role==Group::member||group.role==Group::prepared;candidate_passive=status.member;status.membership_revision=group.revision;status.leaf_spki_sha256=identity.spki;status.phase=group.role==Group::prepared?"finishing_enrollment":(group.role==Group::unclaimed?"awaiting_setup":"idle");
    if(startup.passive()){status.authority_ready=false;status.coordinator=false;status.member=true;}
    if(xTaskCreateWithCaps(network_loop,"group_net",6144,nullptr,2,nullptr,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)!=pdPASS)return ESP_ERR_NO_MEM;
    timing.done(true);return ESP_OK;
}
void runtime_configuration_changed(uint64_t revision){
    if(!revision)return;
    std::lock_guard<std::mutex> guard(lock);if(revision<=configuration)return;
    configuration=revision;
    // The link task advances its authenticated scope in-band. Activity edits
    // do not disconnect owners or invalidate their HDMI observations.
}
RuntimeStatus runtime_status(){
    std::lock_guard<std::mutex> guard(lock);auto out=status;out.active_peers=0;out.connected=out.synchronized=false;
    for(const auto &link:links){
        if(link->cancelled||link->retiring||!link->peer||!link->tls||link->tls->authenticated!=link->peer||!current_peer_credential(*link,group))continue;
        std::lock_guard<std::mutex> snapshot(link->mutex);
        if(link->status.id!=link->peer||!link->status.boot_id)continue;
        ++out.active_peers;out.connected=true;
        // This aggregate has always meant at least one ready link. Per-owner
        // readiness is reported separately for each owner.
        out.synchronized=out.synchronized||(link->ready&&link->status.ready);
    }
    return out;
}
esp_err_t runtime_claim_original_controller(){
    std::lock_guard<std::mutex> persistence(persistence_lock);
    std::unique_lock<std::mutex> guard(lock);
    if(!status.initialized||!status.credential_ready)return ESP_ERR_INVALID_STATE;
    if(original_controller(group,identity.id))return ESP_OK;
    // Only the application's verified Matter/network completion path calls
    // this entry. The durable candidate decision wins any concurrent claim.
    if(group.role!=Group::unclaimed||group.join_id||candidate_passive||candidate_generation||candidate_deadline||!singleton)return ESP_ERR_INVALID_STATE;
    Group next;next.role=Group::coordinator;next.consortium=singleton;next.authority=identity.id;next.revision=1;
    const auto saved=without_runtime_lock(guard,[&]{return save_group(next);});if(saved!=ESP_OK){candidate_passive=true;status.phase="recovery_required";status.last_error="Controller setup could not be stored; restart to recover the saved role";return saved;}
    group=std::move(next);status.coordinator=true;status.authority_ready=true;status.member=false;status.membership_revision=1;status.phase="idle";return ESP_OK;
}
bool runtime_original_setup_eligible(){
    std::lock_guard<std::mutex> guard(lock);
    return status.initialized&&group.role==Group::unclaimed&&!group.join_id&&!candidate_passive&&!candidate_generation&&!candidate_deadline&&bool(singleton);
}
void runtime_radio_hooks(RuntimeRadioHooks hooks){std::lock_guard<std::mutex> guard(lock);radio=std::move(hooks);}
void runtime_network_hooks(RuntimeNetworkHooks hooks){std::lock_guard<std::mutex> guard(lock);network=std::move(hooks);}
void runtime_lifecycle_hooks(RuntimeLifecycleHooks hooks){std::lock_guard<std::mutex> guard(lock);if(hooks.retire)lifecycle.retire=std::move(hooks.retire);if(hooks.revoked)lifecycle.revoked=std::move(hooks.revoked);if(hooks.factory_reset)lifecycle.factory_reset=std::move(hooks.factory_reset);if(hooks.after_factory_reset)lifecycle.after_factory_reset=std::move(hooks.after_factory_reset);}
void runtime_update_hooks(RuntimeUpdateHooks hooks){std::lock_guard<std::mutex> guard(lock);updates=std::move(hooks);}
void runtime_control_hooks(RuntimeControlHooks hooks){std::lock_guard<std::mutex> guard(lock);controls=std::move(hooks);}
std::optional<model::LinkId> runtime_link_id(model::OctopusId owner,model::BootId peer_boot){
    std::lock_guard<std::mutex> guard(lock);
    for(const auto& active:links)if(active->peer==owner&&!active->cancelled&&!active->retiring&&!active->restart_requested&&!active->execution_fenced&&active->ready){
        std::lock_guard<std::mutex> snapshot(active->mutex);
        if(active->status.boot_id==peer_boot&&current_peer_credential(*active,group))return active->session.scope().link_id;
    }
    return {};
}

esp_err_t runtime_register_gatt(){return gatt_register();}
esp_err_t runtime_open_enrollment(uint64_t generation){
    std::lock_guard<std::mutex> persistence(persistence_lock);
    std::lock_guard<std::mutex> guard(lock);
    if(!status.initialized||!status.credential_ready||!generation||candidate_passive||candidate_deadline||group.role!=Group::unclaimed)return ESP_ERR_INVALID_STATE;
    const auto e=gatt_server_open(generation,identity.id,bootstrap_request);
    if(e==ESP_OK){candidate_generation=generation;candidate_deadline=now_ms()+enrollment_window_ms;status.enrollment_open=true;status.phase="enrollment_open";}
    return e;
}
void runtime_close_enrollment(){uint64_t generation;{std::lock_guard<std::mutex> guard(lock);generation=candidate_generation;candidate_generation=candidate_deadline=0;status.enrollment_open=false;if(group.role==Group::unclaimed&&!candidate_passive)status.phase="awaiting_setup";}gatt_server_close();defer_radio_release(generation);}
bool runtime_cancel_radio_workflows(){
    {std::lock_guard<std::mutex> persistence(persistence_lock);std::lock_guard<std::mutex> guard(lock);
        invitation_cancelled=true;
        if(group.join_id&&group.joining_id==admission.view.candidate_id){admission.view.phase="finishing";admission.view.error="Setup could not be confirmed yet. Check the Amity devices list before trying again.";}
        else if(admission.view.phase=="scanning"||admission.view.phase=="found"||admission.view.phase=="connecting"){admission.view.phase="cancelled";admission.view.error="Adding this Amity was cancelled.";}
    }
    runtime_close_enrollment();gatt_client_disconnect();
    return !invitation_running&&gatt_quiescent();
}
RuntimeAdmissionStatus runtime_admission_status(){
    std::lock_guard<std::mutex> guard(lock);
    if(admission.view.phase=="finishing"&&admission.view.candidate_id){
        for(const auto& link:links)if(link->peer==admission.view.candidate_id&&!link->cancelled&&link->ready){
            std::lock_guard<std::mutex> snapshot(link->mutex);
            if(link->status.ready&&link->status.hostname==admission.hostname){
                // Completion describes this Add operation, not the continued
                // presence of its member. A new scan replaces the view.
                admission.view.phase="complete";admission.view.error.clear();break;
            }
        }
    }
    return admission.view;
}
esp_err_t runtime_scan_admission(model::MessageId scan){
    if(!scan)return ESP_ERR_INVALID_ARG;
    std::lock_guard<std::mutex> guard(lock);
    if(!status.initialized||!status.credential_ready||group.role!=Group::coordinator||!status.authority_ready||group.join_id||group.peers.size()>=maximum_roster_peers||!radio.reserve)return ESP_ERR_INVALID_STATE;
    if(invitation_running.exchange(true))return ESP_ERR_INVALID_STATE;
    admission={};admission.view.scan_id=scan;admission.view.phase="scanning";
    invitation_cancelled=false;status.last_error.clear();
    auto* request=new InviteRequest{scan};
    if(xTaskCreateWithCaps(invite_worker,"group_join",16384,request,2,nullptr,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)!=pdPASS){delete request;invitation_running=false;admission.view.phase="failed";admission.view.error="Setup could not start.";return ESP_ERR_NO_MEM;}
    return ESP_OK;
}
esp_err_t runtime_cancel_admission(model::MessageId scan){
    {std::lock_guard<std::mutex> persistence(persistence_lock);std::lock_guard<std::mutex> guard(lock);
        if(!scan||admission.view.scan_id!=scan)return ESP_ERR_INVALID_STATE;
        if(admission.view.phase=="cancelled")return ESP_OK;
        // A prepared enrollment is reconciled by its durable transaction. It
        // cannot be undone by closing the browser or by clearing radio state.
        if(group.join_id||admission.view.phase=="finishing"||admission.view.phase=="complete")return ESP_ERR_INVALID_STATE;
        invitation_cancelled=true;admission.view.phase="cancelled";admission.view.error="Adding this Amity was cancelled.";
    }
    gatt_client_disconnect();return ESP_OK;
}
esp_err_t runtime_add_octopus(model::MessageId scan,const std::string& token,const std::string& hostname){
    BoundedSetupTimings<1> timing("admission",now_ms);timing.scan=scan;const auto selected=timing.begin("selected");
    if(!scan||token.empty()||token.size()>128||!valid_hostname(hostname))return ESP_ERR_INVALID_ARG;
    std::lock_guard<std::mutex> guard(lock);
    if(group.role!=Group::coordinator||!status.authority_ready||group.join_id||group.peers.size()>=maximum_roster_peers||!invitation_running||invitation_cancelled||admission.view.scan_id!=scan||admission.view.phase!="found"||!admission.deadline||now_ms()>=admission.deadline)return ESP_ERR_INVALID_STATE;
    if(std::none_of(admission.view.candidates.begin(),admission.view.candidates.end(),[&](const auto& item){return item.token==token;}))return ESP_ERR_INVALID_ARG;
    if(!network.get_hostname||network.get_hostname()==hostname)return ESP_ERR_INVALID_ARG;
    for(const auto& peer:group.peers){
        const auto linked=settings_link(peer.id);const auto name=linked.status.hostname.empty()?hostnames.lookup(peer.id):linked.status.hostname;
        if(name==hostname)return ESP_ERR_INVALID_ARG;
    }
    settings_expire(now_ms());if(settings_hostname_pending({},hostname))return ESP_ERR_INVALID_ARG;
    admission.selected_token=token;admission.hostname=hostname;admission.submitted=true;admission.view.phase="connecting";timing.finish(selected,true);
    return ESP_OK;
}
esp_err_t runtime_remove_octopus(model::OctopusId id,model::MessageId request,uint64_t expected_revision,std::optional<model::BootId> expected_target_boot){
    if(!id||!request||!expected_revision||(expected_target_boot&&!expected_target_boot->valid()))return ESP_ERR_INVALID_ARG;
    RuntimeLifecycleHooks hooks;const auto now=now_ms();
    {std::lock_guard<std::mutex> persistence(persistence_lock);std::unique_lock<std::mutex> guard(lock);removal_expire(now);
        for(const auto& operation:removal_operations)if(operation.id==request)return operation.owner==id?ESP_OK:ESP_ERR_INVALID_ARG;
        if(!original_controller(group,identity.id)||!status.authority_ready||id==identity.id||group.revision!=expected_revision||group.revision==UINT64_MAX||(group.join_id&&group.joining_id!=id))return ESP_ERR_INVALID_STATE;
        if(removal_operations.size()>=10)return ESP_ERR_NO_MEM;
        auto next=group;auto found=std::find_if(next.peers.begin(),next.peers.end(),[&](const auto &p){return p.id==id;});if(found==next.peers.end())return ESP_ERR_NOT_FOUND;
        const bool joining=group.join_id&&group.joining_id==id;
        std::shared_ptr<Active> target;std::optional<LinkScope> target_scope;
        if(!joining)for(const auto& active:links){
            if(!active->tls||!active->tls->server||active->peer!=id||active->cancelled||active->retiring||(!active->ready&&!active->management_ready)||!current_peer_credential(*active,group))continue;
            std::lock_guard<std::mutex> queue_guard(active->mutex);
            if(active->status.boot_id!=active->session.scope().peer_boot_id)continue;
            if(active->refreshing||active->status.config_revision!=configuration)return ESP_ERR_INVALID_STATE;
            if(expected_target_boot&&*expected_target_boot!=active->status.boot_id)return ESP_ERR_INVALID_STATE;
            target=active;target_scope=active->session.scope();break;
        }
        next.peers.erase(found);
        if(next.joining_id==id){next.join_id={};next.joining_id={};next.join_digest.clear();next.join_hostname.clear();next.join_revision=0;}
        ++next.revision;const auto saved=without_runtime_lock(guard,[&]{return save_group(next);});if(saved!=ESP_OK)return saved;
        // Removal is durable even if the admitted connection disappeared or
        // synchronized while flash was busy. Never restamp its reset request
        // onto another boot, link, or configuration scope.
        const bool had_target=bool(target);
        if(target){
            const auto admitted_target=target;
            std::lock_guard<std::mutex> snapshot(admitted_target->mutex);
            if(target->cancelled||target->retiring||(!target->ready&&!target->management_ready)||
               !(target->session.scope()==*target_scope)||target->status.boot_id!=target_scope->peer_boot_id||
               target->refreshing||configuration!=target_scope->config_revision)target.reset();
        }
        group=std::move(next);status.membership_revision=group.revision;hooks=lifecycle;
        removal_operations.push_back({id,request,now+2000,target?"pending":had_target?"unconfirmed":"offline",!target,target?target_scope:std::nullopt});
        for(auto& active:links){
            if(active->peer!=id)continue;
            if(active==target){
                std::lock_guard<std::mutex> queue_guard(active->mutex);active->queue.clear();active->ready=false;active->management_ready=false;active->status.ready=false;active->status.management_ready=false;
                active->removal_request=request;active->removal_until=now+2000;active->retiring=true;
            }else{active->cancelled=true;::shutdown(active->tls->socket,SHUT_RDWR);}
        }
        if(joining){status.phase="enrollment_revoked";status.last_error="Enrollment cancelled. Physically factory reset the removed Amity before adding it again.";}
        removal_expire(now);
    }
    if(hooks.revoked)hooks.revoked(id);
    return ESP_OK;
}
std::string runtime_removal_operation(model::OctopusId owner,model::MessageId request){
    std::lock_guard<std::mutex> guard(lock);removal_expire(now_ms());
    for(const auto& operation:removal_operations)if(operation.owner==owner&&operation.id==request){
        const std::string message=!operation.complete?"Removing this Amity and requesting its factory reset.":operation.reset=="scheduled"?"Amity removed. It acknowledged a scheduled factory reset.":operation.reset=="offline"?"Amity removed. It was offline; physically factory reset it before adding it again.":"Amity removed. Factory reset was not confirmed; physically factory reset it before adding it again.";
        return "{\"stage\":\""+std::string(operation.complete?"complete":"admitted")+"\",\"reset\":\""+operation.reset+"\",\"message\":"+settings_quote(message)+"}";
    }
    return "{\"stage\":\"complete\",\"reset\":\"unconfirmed\",\"message\":\"The removal result is no longer available. Physically factory reset the removed Amity before adding it again.\"}";
}
esp_err_t runtime_forget_removed_octopus(model::OctopusId owner){
    if(!owner)return ESP_ERR_INVALID_ARG;
    RuntimeLifecycleHooks hooks;
    {
        std::lock_guard<std::mutex> persistence(persistence_lock);
        std::unique_lock<std::mutex> guard(lock);
        if(!original_controller(group,identity.id)||!status.authority_ready||owner==identity.id||
           std::any_of(group.peers.begin(),group.peers.end(),[&](const auto& peer){return peer.id==owner;}))
            return ESP_ERR_INVALID_STATE;
        const auto saved=hostnames.forget(owner,[&](std::string_view bytes){
            return without_runtime_lock(guard,[&]{return write_record("amity_group","hostnames",bytes);})==ESP_OK;
        });
        if(saved!=Error::none)return ESP_FAIL;
        settings_caches.erase(std::remove_if(settings_caches.begin(),settings_caches.end(),
            [&](const auto& item){return item.owner==owner;}),settings_caches.end());
        settings_operations.erase(std::remove_if(settings_operations.begin(),settings_operations.end(),
            [&](const auto& item){return item.owner==owner;}),settings_operations.end());
        if(!settings_authorization.local){
            const auto active=settings_authorization.link.lock();
            if(active&&active->peer==owner)settings_authorization={};
        }
        hooks=lifecycle;
    }
    // Clear owner-local snapshots for saved-only purges as well as roster
    // removals. Reset acknowledgement state remains available to the caller.
    if(hooks.revoked)hooks.revoked(owner);
    return ESP_OK;
}
void runtime_settings_hooks(RuntimeSettingsHooks hooks){std::lock_guard<std::mutex> guard(lock);settings_hooks=std::move(hooks);}
void runtime_log_hooks(RuntimeLogHooks hooks){std::lock_guard<std::mutex> guard(lock);log_hooks=std::move(hooks);}
esp_err_t runtime_pull_logs(model::OctopusId owner,const std::function<bool(std::string_view)>& consume){return pull_logs(owner,consume);}
bool runtime_device_settings_authorized(model::MessageId id){std::lock_guard<std::mutex> guard(lock);return settings_authorized_locked(id);}
std::string runtime_device_settings(model::OctopusId owner){
    RuntimeSettingsHooks hooks;{std::lock_guard<std::mutex> guard(lock);if(!original_controller(group,identity.id)||!status.authority_ready)return "{\"status\":\"error\",\"settings\":null,\"error\":\"Settings are available on the main Amity only.\"}";if(owner==identity.id)hooks=settings_hooks;}
    if(hooks.snapshot){const auto snapshot=hooks.snapshot();if(!snapshot.empty())return "{\"status\":\"ready\",\"settings\":"+snapshot+",\"error\":\"\"}";return "{\"status\":\"error\",\"settings\":null,\"error\":\"The device settings are not ready.\"}";}
    std::lock_guard<std::mutex> guard(lock);const auto now=now_ms();settings_expire(now);auto* cache=settings_cache(owner,settings_enrolled(owner));
    const auto old=cache&&!cache->snapshot.empty()?cache->snapshot:"null";
    const auto wrapper=[&](std::string_view state,std::string_view error){return "{\"status\":"+settings_quote(state)+",\"settings\":"+old+",\"error\":"+settings_quote(error)+"}";};
    const auto link=settings_link(owner);
    if(!settings_enrolled(owner)||!link.active)return wrapper("offline","The device is offline. Saved details cannot be edited.");
    if(!link.active->ready)return wrapper("unsupported","Update this device before reading or editing its settings.");
    if(!cache)return wrapper("error","Settings are busy. Try again shortly.");
    if(cache->boot==link.status.boot_id&&cache->received&&now>=cache->received&&now-cache->received<2000&&cache->error.empty())return wrapper("ready","");
    if(cache->read_id&&now<cache->read_until)return wrapper("reading","");
    if(cache->read_id){cache->read_id={};cache->read_until=0;cache->error="The device did not answer. Open its settings again.";}
    if(cache->attempted&&now-cache->attempted<1000)return wrapper("error",cache->error);
    std::lock_guard<std::mutex> queue(link.active->mutex);if(link.active->queue.size()>=16)return wrapper("error","The device is busy. Try again shortly.");
    cache->read_id=random_id<model::MessageId>();cache->read_link=link.active->session.scope().link_id;cache->read_until=now+5000;cache->attempted=now;
    link.active->queue.push_back({Kind::settings_read,"{\"request_id\":\""+cache->read_id.str()+"\"}",2000,now,link.status.config_revision,{}});return wrapper("reading","");
}
esp_err_t runtime_write_device_settings(model::OctopusId owner,model::MessageId id,const Json& core,uint64_t admitted_until){
    DeviceSettingsWrite write;if(!id||parse_device_settings_write(core,write)!=Error::none)return ESP_ERR_INVALID_ARG;
    const auto encoded=device_settings_write_json(write);const auto now=now_ms();if(!admitted_until)admitted_until=now+5000;admitted_until=std::min<uint64_t>(admitted_until,now+5000);if(admitted_until<=now)return ESP_ERR_TIMEOUT;
    bool local=false;
    {std::lock_guard<std::mutex> guard(lock);settings_expire(now);
        if(!original_controller(group,identity.id)||!status.authority_ready)return ESP_ERR_INVALID_STATE;
        for(const auto& operation:settings_operations)if(operation.owner==owner&&operation.id==id)return operation.core==encoded?ESP_OK:ESP_ERR_INVALID_ARG;
        for(const auto& operation:settings_operations)if(operation.owner==owner&&operation.stage=="admitted")return ESP_ERR_INVALID_STATE;
        if(settings_operations.size()>=11)return ESP_ERR_NO_MEM;
        if(write.hostname){
            if(owner!=identity.id&&network.get_hostname&&network.get_hostname()==*write.hostname)return ESP_ERR_INVALID_ARG;
            for(const auto& peer:group.peers)if(peer.id!=owner&&peer.active){
                const auto linked=settings_link(peer.id);const auto name=linked.status.hostname.empty()?hostnames.lookup(peer.id):linked.status.hostname;
                if(name==*write.hostname)return ESP_ERR_INVALID_ARG;
            }
            if(group.joining_id&&group.joining_id!=owner&&group.join_hostname==*write.hostname)return ESP_ERR_INVALID_ARG;
            if(admission.submitted&&(admission.view.phase=="connecting"||admission.view.phase=="finishing")&&admission.view.candidate_id!=owner&&admission.hostname==*write.hostname)return ESP_ERR_INVALID_ARG;
            if(settings_hostname_pending(owner,*write.hostname))return ESP_ERR_INVALID_ARG;
        }
        local=owner==identity.id;
        if(local){if(write.expected_boot!=boot)return ESP_ERR_INVALID_STATE;settings_operations.push_back({owner,id,boot,{},encoded,"admitted","",admitted_until,write.hostname});}
        else {
            if(!settings_enrolled(owner))return ESP_ERR_INVALID_STATE;
            const auto link=settings_link(owner);
            if(!link.active||!link.active->ready||write.expected_boot!=link.status.boot_id)return ESP_ERR_INVALID_STATE;
            const auto until=std::min<uint64_t>(admitted_until,now+5000);if(until<=now)return ESP_ERR_TIMEOUT;
            std::lock_guard<std::mutex> queue(link.active->mutex);if(link.active->queue.size()>=16)return ESP_ERR_NO_MEM;
            const auto payload="{\"request_id\":\""+id.str()+"\",\"settings\":"+encoded+"}";
            link.active->queue.push_back({Kind::settings_write,payload,static_cast<uint32_t>(until-now),now,link.status.config_revision,{}});settings_operations.push_back({owner,id,link.status.boot_id,link.active->session.scope().link_id,encoded,"admitted","",until,write.hostname});
        }
    }
    if(local){const auto response=settings_apply({},id,core,admitted_until);Json result;JsonError detail;if(!parse_json(response,result,detail))return ESP_FAIL;
        {std::lock_guard<std::mutex> guard(lock);for(auto& operation:settings_operations)if(operation.owner==owner&&operation.id==id){operation.stage=result.get("stage")->string;operation.error=operation.stage=="applied"?"":settings_message(result.get("error")->string);break;}settings_expire(now_ms());}
    }
    return ESP_OK;
}
std::string runtime_device_settings_operation(model::OctopusId owner,model::MessageId id){
    std::string response;
    {std::lock_guard<std::mutex> guard(lock);settings_expire(now_ms());for(auto& operation:settings_operations)if(operation.owner==owner&&operation.id==id){
        if(operation.stage=="admitted"&&owner!=identity.id){const auto link=settings_link(owner);if(!link.active||link.active->session.scope().link_id!=operation.link){operation.stage="unknown";operation.error="The connection changed before the result was confirmed. The request will not be repeated.";}}
        response="{\"stage\":"+settings_quote(operation.stage)+",\"message\":"+settings_quote(operation.error)+",\"rebooting\":false}";break;
    }}
    return response.empty()?"{\"stage\":\"unknown\",\"message\":\"The result is no longer available. The request will not be repeated.\",\"rebooting\":false}":response;
}
static esp_err_t queue_update(model::OctopusId owner,Kind kind,std::string payload,uint32_t ttl,bool firmware_stream){
    if(!is_update_kind(kind)||!ttl||ttl>10000||payload.size()>6144)return ESP_ERR_INVALID_ARG;
    if(kind==Kind::update_chunk){UpdateChunk chunk;if(!decode_update_chunk(payload,chunk))return ESP_ERR_INVALID_ARG;}
    std::lock_guard<std::mutex> guard(lock);
    const bool result=kind==Kind::update_result;
    if(result ? (group.role!=Group::member||group.authority!=owner) : (group.role!=Group::coordinator||!status.authority_ready))return ESP_ERR_INVALID_STATE;
    for(auto& active:links)if(active->peer==owner&&!active->cancelled&&!active->retiring&&!active->restart_requested&&!active->execution_fenced&&(active->ready||active->management_ready)){
        std::lock_guard<std::mutex> qguard(active->mutex);if(active->queue.size()>=16)return ESP_ERR_NO_MEM;
        active->queue.push_back({kind,std::move(payload),ttl,now_ms(),active->status.config_revision,{},{},firmware_stream});return ESP_OK;
    }
    return ESP_ERR_INVALID_STATE;
}
esp_err_t runtime_send_update(model::OctopusId owner,Kind kind,std::string payload,uint32_t ttl){
    return queue_update(owner,kind,std::move(payload),ttl,kind==Kind::update_chunk);
}
esp_err_t runtime_stage_update(model::OctopusId owner,std::string payload,uint32_t ttl){
    return queue_update(owner,Kind::update_install,std::move(payload),ttl,true);
}
static esp_err_t queue_control(model::OctopusId owner,Kind kind,uint32_t ttl,std::optional<model::LinkId> expected_link,uint64_t revision=0,std::shared_ptr<const model::OwnedIntent> owned=nullptr,uint64_t created=0,std::shared_ptr<const model::OwnedAdapterSnapshot> snapshot=nullptr){
    SnapshotRetryBatch retries;DeferredLinkTrace diagnostics;DroppedIntents dropped;
    std::lock_guard<std::mutex> guard(lock);if(!status.authority_ready&&group.role==Group::coordinator)return ESP_ERR_INVALID_STATE;
    for(auto &active:links)if(active->peer==owner&&!active->cancelled&&!active->retiring&&!active->restart_requested&&!active->execution_fenced&&active->ready){
        if(expected_link&&active->session.scope().link_id!=*expected_link)continue;
        std::lock_guard<std::mutex> qguard(active->mutex);const auto now=now_ms();dropped.now=now;
        const bool selection=owned&&selection_intent(*owned);
        active->queue.erase(std::remove_if(active->queue.begin(),active->queue.end(),[&](const auto& queued){
            if(selection&&queued.owned&&selection_intent(*queued.owned))return true;
            const bool expired=queued_expired(queued,now);
            if(expired&&queued.snapshot)retries.dropped(*active,queued);
            else if(expired)dropped.intents.push_back(queued);
            return expired;
        }),active->queue.end());
        const bool cleanup=queued_cleanup({kind,{},ttl,now,revision,owned});
        if(active->queue.size()>=(cleanup?16u:15u)){
            if(cleanup){LinkTrace trace;trace.phase="queue_admission";trace.kind=kind;trace.has_kind=true;trace.fail("cleanup_queue_full");trace.retained=true;diagnostics.capture(*active,trace,now);request_session_restart(*active);}
            return ESP_ERR_NO_MEM;
        }
        const bool current_context=owned&&(owned->kind==model::OwnedIntentKind::context||owned->kind==model::OwnedIntentKind::activity)&&owned->config_revision==active->session.scope().config_revision;
        active->queue.push_back({kind,{},ttl,created?created:now,revision?revision:active->status.config_revision,std::move(owned),std::move(snapshot)});
        if(current_context){active->context_ready=true;active->context_requested=false;active->refreshing=false;}
        return ESP_OK;
    }
    return ESP_ERR_INVALID_STATE;
}
esp_err_t runtime_send_owned_result(model::OctopusId owner,model::LinkId link,const model::OwnedAdapterSnapshot& snapshot,uint64_t revision){
    return queue_control(owner,Kind::scoped_result,1000,link?std::optional<model::LinkId>(link):std::nullopt,revision,{},0,std::make_shared<const model::OwnedAdapterSnapshot>(snapshot));
}
void runtime_fence_owned_link(model::OctopusId owner,model::LinkId link){
    if(!link)return;
    DeferredLinkTrace diagnostics;std::lock_guard<std::mutex> guard(lock);
    for(auto& active:links)if(active->peer==owner&&active->session.scope().link_id==link&&!active->execution_fenced&&!active->cancelled&&!active->retiring){
        std::lock_guard<std::mutex> queue(active->mutex);
        LinkTrace trace;trace.phase="result_delivery";trace.fail("causal_result_fence");trace.retained=true;diagnostics.capture(*active,trace,now_ms());
        request_session_restart(*active);
    }
}
esp_err_t runtime_send_owned(const model::OwnedIntent& intent){
    const bool key_trace=intent.kind==model::OwnedIntentKind::key_edges&&intent.edge_count>=1&&intent.edge_count<=2;
    const auto admission_started=key_trace?now_ms():0;
    // Return through this scope before logging, including every rejection, so
    // neither the runtime nor link queue mutex is held by the diagnostic.
    const auto admitted=[&]() -> esp_err_t {
    RuntimeControlHooks hooks;{std::lock_guard<std::mutex> guard(lock);hooks=controls;}
    const auto admitted_revision=hooks.configuration_revision?hooks.configuration_revision():intent.config_revision;
    RuntimePeerStatus peer;model::LinkId link;bool pending=false;
    {std::lock_guard<std::mutex> guard(lock);if(!original_controller(group,identity.id)||!status.authority_ready)return ESP_ERR_INVALID_STATE;
        if(intent.kind==model::OwnedIntentKind::key_edges&&intent.target.port.consortium_id!=group.consortium)return ESP_ERR_INVALID_ARG;
        if(!intent.config_revision||(intent.config_revision!=configuration&&intent.config_revision!=admitted_revision))return ESP_ERR_INVALID_STATE;
        for(const auto& active:links)if(active->peer==intent.target.port.octopus_id&&active->ready&&!active->cancelled&&!active->retiring&&!active->restart_requested&&!active->execution_fenced){
            std::lock_guard<std::mutex> qguard(active->mutex);peer=active->status;link=active->session.scope().link_id;
            if(intent.config_revision<peer.config_revision)return ESP_ERR_INVALID_STATE;
            pending=intent.config_revision>peer.config_revision;
            if(pending&&intent.kind!=model::OwnedIntentKind::context)return ESP_ERR_INVALID_STATE;
            if(intent.kind==model::OwnedIntentKind::context&&(pending||(active->refreshing&&!active->context_requested))){
                if(!intent.generation||!intent.target.port.valid())return ESP_ERR_INVALID_ARG;
                // Configuration application already schedules the in-band
                // revision exchange. Its normal synchronization hook recaptures
                // current context afterward; no old context payload/deadline
                // waits behind that exchange. Ordinary work is never deferred
                // this way or replayed after a revision/link change.
                return ESP_OK;
            }
            const bool independent=intent.kind==model::OwnedIntentKind::discover||pure_key_release(intent);
            if(!active->context_ready&&intent.kind!=model::OwnedIntentKind::context&&intent.kind!=model::OwnedIntentKind::activity&&!independent)return ESP_ERR_INVALID_STATE;
            break;}}
    if(!link)return ESP_ERR_INVALID_STATE;
    const bool key=intent.kind==model::OwnedIntentKind::key_edges;
    const bool cleanup=pure_key_release(intent);
    const auto now=now_ms();
    const bool port_only=intent.kind==model::OwnedIntentKind::context||intent.kind==model::OwnedIntentKind::discover||intent.kind==model::OwnedIntentKind::activity;
    if(!intent.generation||!(key?bool(intent.target.port.consortium_id)&&bool(intent.target.port.octopus_id):(port_only?intent.target.port.valid():intent.target.valid())))return ESP_ERR_INVALID_ARG;
    const uint32_t cap=cleanup?2000:key?500:1000;
    const uint32_t ttl=cap;
    const auto result=queue_control(intent.target.port.octopus_id,Kind::scoped_intent,ttl,link,intent.config_revision,std::make_shared<const model::OwnedIntent>(intent),now);
    if(result!=ESP_OK&&intent.kind==model::OwnedIntentKind::context)runtime_fence_owned_link(intent.target.port.octopus_id,link);
    return result;
    }();
    if(key_trace){
        const auto completed=now_ms();
        const char* phase=intent.edge_count==2?"tap":intent.edges[0].down?"down":"up";
        ESP_LOGI("consortium","%s stage=admit owner=%s key=%u generation=%llu phase=%s edge_count=%u elapsed_ms=%llu result=%d",
            pure_key_release(intent)?"Key release":"Key transfer",intent.target.port.octopus_id.str().c_str(),static_cast<unsigned>(intent.edges[0].key),static_cast<unsigned long long>(intent.generation),phase,static_cast<unsigned>(intent.edge_count),
            static_cast<unsigned long long>(completed>=admission_started?completed-admission_started:0),admitted);
    }
    return admitted;
}
bool runtime_owned_authorized(const model::OwnedAuthorization& auth){
    RuntimeControlHooks hooks;{std::lock_guard<std::mutex> guard(lock);hooks=controls;}
    const auto admitted=hooks.configuration_revision?hooks.configuration_revision():0;
    std::lock_guard<std::mutex> guard(lock);
    if(auth.owner_boot!=boot)return false;
    if(auth.controller_id==identity.id)return (auth.config_revision==configuration||auth.config_revision==admitted)&&!auth.link_id&&auth.controller_boot==boot&&original_controller(group,identity.id)&&status.authority_ready;
    if(auth.config_revision!=configuration)return false;
    if(group.role!=Group::member||group.authority!=auth.controller_id)return false;
    for(const auto& active:links)if(active->peer==auth.controller_id&&active->ready&&!active->cancelled&&!active->retiring&&!active->restart_requested&&!active->execution_fenced){const auto& scope=active->session.scope();if(scope.link_id==auth.link_id&&scope.peer_boot_id==auth.controller_boot&&scope.local_boot_id==auth.owner_boot&&scope.config_revision==auth.config_revision){
        return true;
    }}
    return false;
}
std::vector<RuntimePeerStatus> runtime_peers(){
    std::lock_guard<std::mutex> guard(lock);std::vector<RuntimePeerStatus> out;
    for(const auto &peer:group.peers){RuntimePeerStatus result;unsigned selected=0;
        for(const auto &active:links)if(!active->cancelled&&!active->retiring&&active->peer==peer.id&&active->tls&&
            active->tls->authenticated==peer.id&&current_peer_credential(*active,group)){
            std::lock_guard<std::mutex> qguard(active->mutex);
            // Hello/authentication writes scope before publishing initialized
            // status under this mutex. Do not read that in-flight scope.
            if(active->status.id!=peer.id||!active->status.boot_id)continue;
            const auto scope=active->session.scope();
            if(active->status.boot_id!=scope.peer_boot_id)continue;
            const bool available=!active->execution_fenced&&!active->restart_requested;
            const bool ready=available&&active->ready&&active->status.ready,management=available&&active->management_ready&&active->status.management_ready;
            const unsigned rank=ready?3:management?2:1;
            if(rank>selected){result=active->status;result.ready=ready;result.management_ready=management;selected=rank;}
        }
        // Enrollment identity belongs to the durable roster. A TLS handshake
        // with a partial status object cannot replace it with an all-zero ID.
        result.id=peer.id;if(result.hostname.empty())result.hostname=hostnames.lookup(peer.id);result.enrolled=peer.active;result.provisional=!peer.active;
        out.push_back(result);
    }
    return out;
}
std::vector<std::pair<model::OctopusId,std::string>> runtime_known_hostnames(){
    std::lock_guard<std::mutex> guard(lock);auto out=hostnames.entries();
    for(const auto& active:links)if(!active->cancelled&&!active->retiring&&current_peer_credential(*active,group)){
        std::lock_guard<std::mutex> snapshot(active->mutex);if(active->status.hostname.empty())continue;
        auto found=std::find_if(out.begin(),out.end(),[&](const auto& entry){return entry.first==active->peer;});
        if(found==out.end())out.emplace_back(active->peer,active->status.hostname);else found->second=active->status.hostname;
    }
    return out;
}
} // namespace
#endif
