#include "amity/system_sync.hpp"
#include "amity/system_control.hpp"
#include "amity/consortium/runtime.hpp"
#include "amity/consortium/owned_intent.hpp"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <deque>
#include <mutex>
#include <set>
#include <atomic>
#ifdef ESP_PLATFORM
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#endif
namespace amity {
namespace s=system;namespace c=consortium;namespace cs=config_store;
namespace {
struct State {
    std::mutex mutex;
    SystemControl* control=nullptr;
    // Recent saves, oldest first. Each caller follows its own request, so a
    // later save cannot leave an earlier one looking unfinished.
    std::deque<ConfigurationActivationStatus> activations;
    bool activating=false;
    std::atomic<bool> configuration_pending{false};
    uint64_t published_revision=0;
    // Member: the bound controller and the local adapters whose latest
    // report it has not yet accepted for delivery.
    s::OctopusId controller;
    uint8_t dirty_reports=0;
    std::set<s::OctopusId> maintenance;
} state;
void record_activation(ConfigurationActivationStatus status){
    auto found=std::find_if(state.activations.begin(),state.activations.end(),[&](const auto& a){return a.request_id==status.request_id;});
    if(found!=state.activations.end()){*found=std::move(status);return;}
    state.activations.push_back(std::move(status));
    if(state.activations.size()>8)state.activations.pop_front();
}
template<class T>T fresh(){return T::generate([](uint8_t* data,size_t size){esp_fill_random(data,size);return true;}).value_or(T{});}
std::optional<c::RuntimePeerStatus> peer(s::OctopusId id){for(const auto& p:c::runtime_peers())if(p.id==id)return p;return {};}
void mark_report(uint8_t adapter){
    if(adapter>=amity::max_tentacles)return;
    std::lock_guard<std::mutex> guard(state.mutex);state.dirty_reports|=uint8_t(1u<<adapter);
}
void observe(const s::OwnedAdapterSnapshot& report){
    if(report.owner==state.control->octopus_id()&&c::runtime_status().member)mark_report(report.adapter);
}
bool accept_report(const s::OwnedAdapterSnapshot& report){
    // Removal revokes the peer before taking this lock to purge the cache.
    // Validate and publish in the same critical section: an earlier report
    // either finishes before that purge or observes the revoked membership.
    std::lock_guard<std::mutex> guard(state.mutex);
    const auto remote=peer(report.owner);
    if(!remote||report.boot!=remote->boot_id||report.adapter>=remote->supported_tentacles)return false;
    return state.control->update_owned_snapshot(report);
}
void publish_configuration_revision(){
    if(!c::runtime_status().coordinator)return;
    const auto config=state.control->active_configuration();if(!config)return;
    {std::lock_guard<std::mutex> guard(state.mutex);if(state.published_revision==config->config_revision)return;}
    const auto applied=state.control->applied_configuration();
    if(applied.config_revision!=config->config_revision||applied.presentation_revision!=config->presentation_revision)return;
    {std::lock_guard<std::mutex> guard(state.mutex);if(state.published_revision==config->config_revision)return;state.published_revision=config->config_revision;}
    c::runtime_configuration_changed(config->config_revision);
}
void flush_configuration(){
    if(state.configuration_pending.exchange(false))publish_configuration_revision();
}
// The member binds its controller's boot and revision, then republishes every
// adapter report in the new scope as ordinary results.
c::Error synchronize(s::OctopusId controller,s::BootId controller_boot,uint64_t revision){
    if(!c::runtime_status().member||!revision)return c::Error::unauthorized_role;
    if(state.control->bind_owned_controller(controller,revision,controller_boot)!=ESP_OK)return c::Error::not_ready;
    const auto latest=state.control->owned_snapshots();const auto local=state.control->octopus_id();
    std::lock_guard<std::mutex> guard(state.mutex);state.controller=controller;
    for(const auto& report:latest)if(report.owner==local&&report.adapter<amity::max_tentacles)state.dirty_reports|=uint8_t(1u<<report.adapter);
    return c::Error::none;
}
c::Error receive(const c::LinkScope& scope,const c::Envelope& envelope,uint64_t deadline){
    const auto sender=scope.peer_id;const auto link=scope.link_id;
    if(envelope.kind==c::Kind::scoped_result){
        if(!c::runtime_status().coordinator)return c::Error::unauthorized_role;
        const auto* snapshot=envelope.payload.get("snapshot");s::OwnedAdapterSnapshot report;
        if(!snapshot||!c::closed(envelope.payload,{"snapshot"})||!c::parse_owned_snapshot(*snapshot,report))return c::Error::invalid_message;
        report.owner=sender;report.boot=scope.peer_boot_id;
        return accept_report(report)?c::Error::none:c::Error::invalid_message;
    }
    if(envelope.kind!=c::Kind::scoped_intent||!envelope.intent||!c::runtime_status().member)return c::Error::unauthorized_role;
    // The session admitted this frame: its intent is parsed and targets this Amity.
    const auto& intent=*envelope.intent;
    s::AdmissionResult admitted;
    if(intent.kind==s::OwnedIntentKind::key_edges){
        admitted=state.control->submit_key_edges(intent.edges,intent.edge_count,intent.generation,link,deadline);
    }else{
        s::OwnedAuthorization auth{sender,scope.peer_boot_id,scope.local_boot_id,link,envelope.config_revision,deadline};
        admitted=state.control->submit_owned(intent,auth);
    }
    if(intent.kind==s::OwnedIntentKind::key_edges&&
       admitted.code!=s::AdmissionCode::accepted&&admitted.code!=s::AdmissionCode::pending)
        ESP_LOGW("system_sync","Key rejected phase=owner key=%u generation=%llu admission=%d",
            static_cast<unsigned>(intent.edges[0].key),static_cast<unsigned long long>(intent.generation),static_cast<int>(admitted.code));
    switch(admitted.code){case s::AdmissionCode::accepted:case s::AdmissionCode::pending:return c::Error::none;case s::AdmissionCode::overloaded:return c::Error::queue_full;case s::AdmissionCode::expired:return c::Error::expired;case s::AdmissionCode::unresolved:return c::Error::none;case s::AdmissionCode::unauthorized:return c::Error::unauthorized_role;case s::AdmissionCode::stale:return c::Error::stale_boot;default:return c::Error::invalid_message;}
}
bool disconnected(s::OctopusId owner,s::BootId boot,s::LinkId link){
    if(!state.control->fence_owned(owner,link))return false;
    std::lock_guard<std::mutex> guard(state.mutex);
    for(auto report:state.control->owned_snapshots())
        if(report.owner==owner&&(!boot||report.boot==boot)&&(report.ready||report.discovering||!report.devices.empty())){
            report.ready=false;report.discovering=false;report.devices.clear();state.control->update_owned_snapshot(std::move(report));
        }
    return true;
}
bool restart_session(s::OctopusId owner,s::BootId boot,s::LinkId link){
    if(!owner||!boot||!link)return false;
    return state.control->fence_owned(owner,link);
}
bool send(const s::OwnedIntent& intent){
    {std::lock_guard<std::mutex> guard(state.mutex);if(state.maintenance.count(intent.target.port.octopus_id))return false;}
    const auto result=c::runtime_send_owned(intent);
    if(result!=ESP_OK&&intent.kind==s::OwnedIntentKind::key_edges)
        ESP_LOGW("system_sync","Key rejected phase=transport key=%u generation=%llu error=%d",
            static_cast<unsigned>(intent.edges[0].key),static_cast<unsigned long long>(intent.generation),static_cast<int>(result));
    return result==ESP_OK;
}
// Send each marked adapter's latest report to the controller in the link's
// current scope. A report the link could not take, or later dropped, stays
// marked; synchronization marks every adapter again.
void flush_reports(){
    uint8_t dirty;s::OctopusId controller;
    {std::lock_guard<std::mutex> guard(state.mutex);dirty=state.dirty_reports;state.dirty_reports=0;controller=state.controller;}
    if(!dirty||!controller||!c::runtime_status().member)return;
    const auto latest=state.control->owned_snapshots();
    const auto local=state.control->octopus_id();const auto local_boot=state.control->boot_identity();
    for(uint8_t adapter=0;adapter<amity::max_tentacles;++adapter)if(dirty&(1u<<adapter)){
        const auto report=std::find_if(latest.begin(),latest.end(),[&](const auto& p){return p.adapter==adapter&&p.owner==local&&p.boot==local_boot;});
        if(report!=latest.end()&&c::runtime_send_owned_result(controller,*report)!=ESP_OK)mark_report(adapter);
    }
}
void adopt_owners(){
    auto config=state.control->active_configuration();if(!config||!c::runtime_status().coordinator)return;
    for(const auto& remote:c::runtime_peers())if(remote.ready&&remote.enrolled){
        auto found=std::find_if(config->octopuses.begin(),config->octopuses.end(),[&](const auto& owner){return owner.octopus_id==remote.id;});
        if(found!=config->octopuses.end()&&found->supported_tentacles>=remote.supported_tentacles)continue;
        auto next=*config;if(found==config->octopuses.end())next.octopuses.push_back({remote.id,remote.supported_tentacles});
        else for(auto& owner:next.octopuses)if(owner.octopus_id==remote.id)owner.supported_tentacles=remote.supported_tentacles;
        // Retries reuse one request, so a failing adoption replaces its own
        // status rather than pushing out the statuses of followed saves.
        static const auto adoption=fresh<s::MessageId>();
        activate_distributed_configuration(std::move(next),config->revisions(),adoption);return;
    }
}
void worker(void*){
    uint64_t next_state=0;
    for(;;){flush_configuration();flush_reports();const auto now=static_cast<uint64_t>(esp_timer_get_time()/1000);
        if(now>=next_state){if(c::runtime_status().coordinator)adopt_owners();next_state=now+1500;}
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
}
esp_err_t start_configuration_synchronization(SystemControl& control){
    {std::lock_guard<std::mutex> guard(state.mutex);if(state.control)return state.control==&control?ESP_OK:ESP_ERR_INVALID_STATE;state.control=&control;}
#ifdef ESP_PLATFORM
    const auto created=xTaskCreateWithCaps(worker,"system_sync",12288,nullptr,2,nullptr,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
#else
    const auto created=xTaskCreate(worker,"system_sync",12288,nullptr,2,nullptr);
#endif
    if(created!=pdPASS){
        c::runtime_control_hooks({});control.set_owned_sender({});control.set_owned_observer({});control.set_owned_authorizer({});
        std::lock_guard<std::mutex> guard(state.mutex);state.control=nullptr;return ESP_ERR_NO_MEM;
    }
    control.set_owned_sender(send);control.set_owned_observer(observe);control.set_owned_authorizer(c::runtime_owned_authorized);
    // The owner signals only after its cached applied revisions advance. Keep
    // publication off the owner task and coalesce changes until this worker runs.
    control.on_configuration_applied([]{state.configuration_pending.store(true);});
    state.configuration_pending.store(true); // Capture configuration applied before startup.
    c::runtime_control_hooks({synchronize,receive,disconnected,[](s::OctopusId owner,uint64_t revision,std::function<void(s::OwnedContextResult)> completed){return state.control->refresh_owned_context(owner,revision,std::move(completed));},[]{return state.control->executing_configuration_revision();},mark_report,restart_session});return ESP_OK;
}
esp_err_t activate_distributed_configuration(cs::Configuration candidate,cs::RevisionPair expected,s::MessageId request,
    std::function<esp_err_t()> before_commit){
    SystemControl* control=nullptr;
    {std::lock_guard<std::mutex> guard(state.mutex);if(!state.control||state.activating)return ESP_ERR_INVALID_STATE;state.activating=true;control=state.control;record_activation({request,0,0,"admitted",{}});}
    struct Clear{~Clear(){std::lock_guard<std::mutex> guard(state.mutex);state.activating=false;}} clear;
    std::string error;esp_err_t result=ESP_ERR_INVALID_ARG;
    auto revisions=candidate.revisions();cs::ConfigurationSnapshot prepared;
    if(request&&control->accepts_local_input()){
        result=cs::stage_system_configuration(std::move(candidate),expected,error);
        if(result==ESP_OK)result=cs::staged_system_configuration(prepared);
        if(prepared)revisions=prepared->configuration.revisions();
        if(result==ESP_OK)result=control->configure(prepared,[&]{
            if(before_commit){const auto admitted=before_commit();if(admitted!=ESP_OK){error="The reviewed device removal changed before configuration commit";return admitted;}}
            return cs::activate_system_configuration(prepared,error);
        });
    }
    if(result!=ESP_OK)cs::cancel_staged_configuration();
    {std::lock_guard<std::mutex> guard(state.mutex);record_activation({request,revisions.config_revision,revisions.presentation_revision,result==ESP_OK?"configuring":"failed",result==ESP_OK?std::string{}:error.empty()?"Configuration could not be saved":error});}
    return result;
}
ConfigurationActivationStatus configuration_activation_status(s::MessageId request){
    if(!request)return {};
    std::lock_guard<std::mutex> guard(state.mutex);
    for(auto& activation:state.activations)if(activation.request_id==request){
        if(state.control&&activation.phase=="configuring"){
            // Saves apply in order, so a later applied save includes this one.
            const auto applied=state.control->applied_configuration();
            if(applied.config_revision>=activation.config_revision&&applied.presentation_revision>=activation.presentation_revision)activation.phase="applied";
        }
        return activation;
    }
    return {};
}
void forget_owner(s::OctopusId owner){
    if(state.control)disconnected(owner,{},{});
    // Explicit membership removal forgets diagnostic observations even when
    // exact outstanding-key cleanup cannot enter the owner task immediately.
    // Runtime authorization has already revoked that owner. Keep its transient
    // wire cleanup state separate from this latest-observation cache.
    std::lock_guard<std::mutex> guard(state.mutex);
    if(state.control)state.control->forget_owned_snapshots(owner);
    state.maintenance.erase(owner);
}
void set_owner_maintenance(s::OctopusId owner,bool maintenance){std::lock_guard<std::mutex> guard(state.mutex);if(maintenance)state.maintenance.insert(owner);else state.maintenance.erase(owner);}
esp_err_t request_equipment_scan(){
    if(!state.control||!state.control->accepts_local_input())return ESP_ERR_INVALID_STATE;
    bool accepted=state.control->discover();
    for(const auto& remote:c::runtime_peers())if(remote.ready&&remote.enrolled)for(uint8_t adapter=0;adapter<remote.supported_tentacles;++adapter)if(remote.configured_tentacles_mask&(1u<<adapter))accepted=state.control->discover_owned(remote.id,adapter)&&accepted;
    return accepted?ESP_OK:ESP_ERR_INVALID_STATE;
}
std::vector<s::CecObservation> discovered_equipment(){
    if(!state.control)return {};
    auto out=state.control->native_observations();
    const auto local=state.control->octopus_id();const auto reports=state.control->owned_snapshots();
    std::lock_guard<std::mutex> guard(state.mutex);for(const auto& report:reports)if(report.owner!=local)for(const auto& d:report.devices)if(d.resolved){s::CecObservation o;o.endpoint={{state.control->consortium_id(),report.owner,report.adapter,d.side},d.logical_address};o.osd_name=d.osd_name;o.physical_address=d.physical_address;o.device_type=d.device_type;o.vendor_id=d.vendor_id;o.stale=state.maintenance.count(report.owner);out.push_back(std::move(o));}
    return out;
}
}
