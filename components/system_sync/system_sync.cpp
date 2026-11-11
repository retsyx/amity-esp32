#include "amity/system_sync.hpp"
#include "amity/system_control.hpp"
#include "amity/consortium/runtime.hpp"
#include "amity/consortium/limits.hpp"
#include "amity/consortium/owned_intent.hpp"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <mutex>
#include <set>
#include <array>
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
    ConfigurationActivationStatus activation;
    bool activating=false;
    std::atomic<bool> configuration_pending{false};
    uint64_t published_revision=0,owner_revision=0;
    std::vector<s::OwnedAdapterSnapshot> reports,pending_reports;
    std::array<std::optional<c::RuntimeSnapshotRetry>,amity::max_tentacles> retry_reports;
    std::set<s::OctopusId> maintenance;
} state;
template<class T>T fresh(){return T::generate([](uint8_t* data,size_t size){esp_fill_random(data,size);return true;}).value_or(T{});}
std::optional<c::RuntimePeerStatus> peer(s::OctopusId id){for(const auto& p:c::runtime_peers())if(p.id==id)return p;return {};}
void observe(const s::OwnedAdapterSnapshot& report){
    if(report.owner!=state.control->octopus_id()||report.adapter>=amity::max_tentacles||!c::runtime_status().member)return;
    std::lock_guard<std::mutex> guard(state.mutex);
    auto prior=std::find_if(state.pending_reports.begin(),state.pending_reports.end(),[&](const auto& p){return p.adapter==report.adapter;});
    if(prior!=state.pending_reports.end())*prior=report;
    else if(state.pending_reports.size()<amity::max_tentacles)state.pending_reports.push_back(report);
    state.retry_reports[report.adapter].reset();
}
void retry_report(const c::RuntimeSnapshotRetry& retry){
    if(retry.adapter>=amity::max_tentacles)return;
    std::lock_guard<std::mutex> guard(state.mutex);
    if(!state.control||retry.owner_boot!=state.control->boot_identity())return;
    // Selection and exact current-scope validation are atomic against other
    // retry/observer callbacks. Otherwise a callback may validate, pause, and
    // replace a newer flag after its own link has become stale. This read-only
    // lookup takes runtime locks; runtime invokes control hooks only after
    // releasing them, preserving the state -> runtime lock order here.
    if(retry.link&&(retry.config_revision!=state.owner_revision||c::runtime_link_id(retry.controller,retry.controller_boot)!=retry.link))return;
    // A fresh owner observation supersedes a failed/expired publication. Never
    // put its older payload back over an observation arriving during send.
    if(std::none_of(state.pending_reports.begin(),state.pending_reports.end(),[&](const auto& p){return p.adapter==retry.adapter;}))
        state.retry_reports[retry.adapter]=retry;
}
bool accept_report(s::OctopusId sender,const s::OwnedAdapterSnapshot& report){
    // Removal revokes the peer before taking this lock to purge both caches.
    // Validate and publish in the same critical section: an earlier report
    // either finishes before that purge or observes the revoked membership.
    std::lock_guard<std::mutex> guard(state.mutex);
    const auto remote=peer(sender);
    if(!remote||report.owner!=sender||report.boot!=remote->boot_id||report.adapter>=remote->supported_tentacles)return false;
    if(state.maintenance.count(sender))return false;
    auto prior=std::find_if(state.reports.begin(),state.reports.end(),[&](const auto& p){return p.owner==sender&&p.adapter==report.adapter;});
    if(prior!=state.reports.end())*prior=report;else if(state.reports.size()<amity::max_tentacles*c::maximum_roster_peers)state.reports.push_back(report);else return false;
    return state.control->update_owned_snapshot(report);
}
std::string snapshot(s::OctopusId){
    const auto group=c::runtime_status();
    if(group.coordinator){
        auto config=state.control->active_configuration();if(!config)return {};
        return "{\"controller\":\""+state.control->octopus_id().str()+"\",\"config_revision\":\""+std::to_string(config->config_revision)+"\"}";
    }
    std::string out="{\"adapters\":[";
    for(const auto& report:state.control->owned_snapshots())if(report.owner==state.control->octopus_id()){
        auto bytes=c::owned_snapshot_json(report);if(bytes.empty())return {};if(out.back()!='[')out+=',';out+=bytes;
    }
    return out+"]}";
}
std::string configuration_snapshot(s::OctopusId,uint64_t revision){
    if(!revision||!c::runtime_status().coordinator)return {};
    return "{\"controller\":\""+state.control->octopus_id().str()+"\",\"config_revision\":\""+std::to_string(revision)+"\"}";
}
void publish_configuration_revision(){
    if(!c::runtime_status().coordinator)return;
    const auto config=state.control->active_configuration();if(!config)return;
    {std::lock_guard<std::mutex> guard(state.mutex);if(state.published_revision==config->config_revision)return;}
    const auto applied=state.control->applied_configuration();
    if(applied.config_revision!=config->config_revision||applied.presentation_revision!=config->presentation_revision)return;
    if(applied.policy_hash.size()!=64)return;
    {std::lock_guard<std::mutex> guard(state.mutex);if(state.published_revision==config->config_revision)return;state.published_revision=config->config_revision;}
    c::runtime_configuration_changed(config->config_revision,applied.policy_hash);
}
void flush_configuration(){
    if(state.configuration_pending.exchange(false))publish_configuration_revision();
}
c::Error synchronize(s::OctopusId sender,const c::Json& value){
    if(value.type!=c::Json::Type::string)return c::Error::invalid_message;
    c::Json document;c::JsonError error;if(!c::parse_json(value.string,document,error,32768,2048))return c::Error::invalid_message;
    const auto group=c::runtime_status();
    if(group.coordinator){
        if(!c::closed(document,{"adapters"})||document.get("adapters")->type!=c::Json::Type::array||document.get("adapters")->array.size()>amity::max_tentacles)return c::Error::invalid_message;
        std::set<uint8_t> seen;for(const auto& item:document.get("adapters")->array){s::OwnedAdapterSnapshot report;if(!c::parse_owned_snapshot(item,report)||!seen.insert(report.adapter).second||!accept_report(sender,report))return c::Error::invalid_message;}
        return c::Error::none;
    }
    s::OctopusId controller;uint64_t revision=0;
    if(!group.member||!c::closed(document,{"controller","config_revision"})||!c::id(document.get("controller"),controller)||controller!=sender||!c::decimal(document.get("config_revision"),revision)||!revision)return c::Error::unauthorized_role;
    if(state.control->bind_owned_controller(controller,revision)!=ESP_OK)return c::Error::not_ready;
    {std::lock_guard<std::mutex> guard(state.mutex);state.owner_revision=revision;}
    return c::Error::none;
}
c::Error receive(s::OctopusId sender,const c::Envelope& envelope,uint64_t deadline,s::LinkId link){
    if(envelope.kind==c::Kind::scoped_result){
        if(!c::runtime_status().coordinator)return c::Error::unauthorized_role;
        const auto* snapshot=envelope.payload.get("snapshot");s::OwnedAdapterSnapshot report;
        return snapshot&&c::closed(envelope.payload,{"snapshot"})&&c::parse_owned_snapshot(*snapshot,report)&&accept_report(sender,report)?c::Error::none:c::Error::invalid_message;
    }
    if(envelope.kind!=c::Kind::scoped_intent||!c::runtime_status().member)return c::Error::unauthorized_role;
    s::OwnedIntent intent;
    if(c::parse_owned_intent(envelope.payload,intent)!=c::Error::none)return c::Error::invalid_message;
    s::AdmissionResult admitted;
    if(intent.kind==s::OwnedIntentKind::key_edges){
        admitted=state.control->submit_key_edges(intent.edges,intent.edge_count,intent.generation,link,deadline);
    }else{
        if(intent.target.port.octopus_id!=state.control->octopus_id()||intent.target.port.consortium_id!=state.control->consortium_id())return c::Error::target_not_local;
        s::OwnedAuthorization auth{sender,envelope.origin_boot_id,envelope.destination_boot_id,link,envelope.config_revision,deadline};
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
    std::vector<s::OwnedAdapterSnapshot> removed;
    for(const auto& report:state.reports)if(report.owner==owner&&(!boot||report.boot==boot)){auto missing=report;missing.ready=false;missing.discovering=false;missing.devices.clear();removed.push_back(missing);}
    state.reports.erase(std::remove_if(state.reports.begin(),state.reports.end(),[&](const auto& report){return report.owner==owner&&(!boot||report.boot==boot);}),state.reports.end());
    for(auto& report:removed)state.control->update_owned_snapshot(std::move(report));
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
void flush_reports(){
    std::vector<s::OwnedAdapterSnapshot> reports;
    decltype(state.retry_reports) retries;
    uint64_t revision;
    {std::lock_guard<std::mutex> guard(state.mutex);reports.swap(state.pending_reports);retries.swap(state.retry_reports);revision=state.owner_revision;}
    if(reports.empty()&&std::none_of(retries.begin(),retries.end(),[](const auto& retry){return retry.has_value();}))return;
    if(!c::runtime_status().member)return;
    // This facade cache is updated before the owner observer runs. Publication
    // and recovery always recapture it on this task, never on the TLS task.
    const auto latest=state.control->owned_snapshots();
    const auto peers=c::runtime_peers();
    for(uint8_t adapter=0;adapter<amity::max_tentacles;++adapter){
        const bool observed=std::any_of(reports.begin(),reports.end(),[&](const auto& p){return p.adapter==adapter;});
        if(!observed&&!retries[adapter])continue;
        auto retry=observed?c::RuntimeSnapshotRetry{}:*retries[adapter];retry.adapter=adapter;
        const auto local_boot=state.control->boot_identity();
        if(retry.owner_boot&&retry.owner_boot!=local_boot)continue;
        if(retry.link&&(retry.config_revision!=revision||c::runtime_link_id(retry.controller,retry.controller_boot)!=retry.link))continue;
        const auto report=std::find_if(latest.begin(),latest.end(),[&](const auto& p){return p.adapter==adapter&&p.owner==state.control->octopus_id()&&p.boot==local_boot;});
        if(report==latest.end())continue;
        bool sent=false;
        if(retry.link)sent=c::runtime_send_owned_result(retry.controller,retry.link,*report,1000,retry.config_revision)==ESP_OK;
        else for(const auto& remote:peers)if(remote.ready){sent=c::runtime_send_owned_result(remote.id,{},*report,1000)==ESP_OK;break;}
        if(!sent){retry.owner_boot=local_boot;retry_report(retry);} // Bounded recapture flag, never expired payload or executable work.
    }
}
void adopt_owners(){
    auto config=state.control->active_configuration();if(!config||!c::runtime_status().coordinator)return;
    for(const auto& remote:c::runtime_peers())if(remote.ready&&remote.enrolled){
        auto found=std::find_if(config->octopuses.begin(),config->octopuses.end(),[&](const auto& owner){return owner.octopus_id==remote.id;});
        if(found!=config->octopuses.end()&&found->supported_tentacles>=remote.supported_tentacles)continue;
        auto next=*config;if(found==config->octopuses.end()){
            size_t number=1;std::string label;
            do{label="Amity "+std::to_string(number++);}while(std::any_of(next.octopuses.begin(),next.octopuses.end(),[&](const auto& owner){return owner.label==label||owner.generated_label==label;}));
            next.octopuses.push_back({remote.id,label,label,false,remote.supported_tentacles});
        }
        else for(auto& owner:next.octopuses)if(owner.octopus_id==remote.id)owner.supported_tentacles=remote.supported_tentacles;
        activate_distributed_configuration(std::move(next),config->revisions(),fresh<s::MessageId>());return;
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
        c::runtime_control_hooks({});control.set_owned_sender({});control.set_owned_observer({});control.set_owned_authorizer({});control.set_owned_deadline({});
        std::lock_guard<std::mutex> guard(state.mutex);state.control=nullptr;return ESP_ERR_NO_MEM;
    }
    control.set_owned_sender(send);control.set_owned_observer(observe);control.set_owned_authorizer(c::runtime_owned_authorized);control.set_owned_deadline(c::runtime_owned_deadline);
    // The owner signals only after its cached applied revisions advance. Keep
    // publication off the owner task and coalesce changes until this worker runs.
    control.on_configuration_applied([]{state.configuration_pending.store(true);});
    state.configuration_pending.store(true); // Capture configuration applied before startup.
    c::runtime_control_hooks({snapshot,synchronize,receive,disconnected,[](s::OctopusId owner,uint64_t revision,std::function<void(s::OwnedContextResult)> completed){return state.control->refresh_owned_context(owner,revision,std::move(completed));},[]{return state.control->executing_configuration_revision();},configuration_snapshot,retry_report,restart_session});return ESP_OK;
}
esp_err_t activate_distributed_configuration(cs::Configuration candidate,cs::RevisionPair expected,s::MessageId request,
    std::function<esp_err_t()> before_commit){
    SystemControl* control=nullptr;
    {std::lock_guard<std::mutex> guard(state.mutex);if(!state.control||state.activating)return ESP_ERR_INVALID_STATE;state.activating=true;control=state.control;state.activation={request,0,0,"admitted",{}};}
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
    {std::lock_guard<std::mutex> guard(state.mutex);state.activation={request,revisions.config_revision,revisions.presentation_revision,result==ESP_OK?"configuring":"failed",result==ESP_OK?std::string{}:error.empty()?"Configuration could not be saved":error};}
    return result;
}
ConfigurationActivationStatus configuration_activation_status(){
    std::lock_guard<std::mutex> guard(state.mutex);
    if(state.control&&state.activation.phase=="configuring"){
        const auto applied=state.control->applied_configuration();
        if(applied.config_revision==state.activation.config_revision&&applied.presentation_revision==state.activation.presentation_revision)state.activation.phase="applied";
    }
    return state.activation;
}
void forget_owner(s::OctopusId owner){
    if(state.control)disconnected(owner,{},{});
    // Explicit membership removal forgets diagnostic observations even when
    // exact outstanding-key cleanup cannot enter the owner task immediately.
    // Runtime authorization has already revoked that owner. Keep its transient
    // wire cleanup state separate from this latest-observation cache.
    std::lock_guard<std::mutex> guard(state.mutex);
    if(state.control)state.control->forget_owned_snapshots(owner);
    state.reports.erase(std::remove_if(state.reports.begin(),state.reports.end(),[&](const auto& report){return report.owner==owner;}),state.reports.end());
    state.pending_reports.erase(std::remove_if(state.pending_reports.begin(),state.pending_reports.end(),[&](const auto& report){return report.owner==owner;}),state.pending_reports.end());
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
    std::lock_guard<std::mutex> guard(state.mutex);for(const auto& report:state.reports)for(const auto& d:report.devices)if(d.resolved){s::CecObservation o;o.endpoint={{state.control->consortium_id(),report.owner,report.adapter,d.side},d.logical_address};o.osd_name=d.osd_name;o.owner_boot_id=report.boot;o.scan_id=s::ScanId::parse(report.boot.str()).value_or(s::ScanId{});o.local_sequence=report.discovery_generation+1;o.physical_address=d.physical_address;o.device_type=d.device_type;o.vendor_id=d.vendor_id;o.stale=state.maintenance.count(report.owner);out.push_back(std::move(o));}
    return out;
}
}
