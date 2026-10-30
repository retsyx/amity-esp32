#pragma once
#include "amity/hub_service.hpp"
#include "amity/system/execution.hpp"
#include "system_configuration.hpp"
#include <mutex>

namespace amity {
// Original Amity facade. Saved equipment IDs map to qualified name selectors;
// the owner resolves each selector using its ordinary local discovery cache.
class SystemControl {
public:
    void set_identity(system::ConsortiumId group,system::OctopusId owner){consortium_=group;octopus_=owner;local_.set_system_identity(group,owner);}
    const system::ConsortiumId& consortium_id()const{return consortium_;}
    const system::OctopusId& octopus_id()const{return octopus_;}
    void set_boot_identity(system::BootId boot){boot_=boot;local_.set_boot_identity(boot);}
    system::BootId boot_identity()const{return boot_;}
    void set_hardware_error(std::string error){if(error.size()>192)error.resize(192);std::atomic_store(&hardware_error_,std::make_shared<const std::string>(std::move(error)));}
    std::string hardware_error()const{auto p=std::atomic_load(&hardware_error_);return p?*p:std::string{};}
    void set_coordinator_authority(bool enabled,uint64_t epoch){local_coordinator_.store(enabled&&epoch==1);local_.set_coordinator_authority(enabled&&epoch==1,epoch);}
    esp_err_t transition_coordinator_authority(bool enabled,uint64_t epoch);
    system::CecPortRef port(BusSide side)const{return {consortium_,octopus_,0,side==BusSide::Front?system::Side::front:system::Side::back};}
    esp_err_t start(int front,int back,uint16_t pa=0x1000){return local_.start(front,back,pa);}
    esp_err_t start(const cec::BoardProfile& profile,uint16_t pa=0x1000){auto e=local_.start(profile,pa);if(e==ESP_OK)profile_=profile;return e;}
    esp_err_t start(const cec::BoardProfile& profile,const std::array<uint16_t,amity::max_tentacles>& pa){auto e=local_.start(profile,pa);if(e==ESP_OK)profile_=profile;return e;}
    uint8_t supported_tentacles()const{return profile_.tentacle_count;}
    bool connector_enabled(uint8_t id)const{return cec::connector_enabled(profile_,id);}
    uint8_t configured_tentacles_mask()const{uint8_t mask=0;for(uint8_t i=0;i<supported_tentacles();++i)if(connector_enabled(i))mask|=uint8_t(1u<<i);return mask;}
    std::optional<bool> connector_presence(uint8_t id)const{return local_.connector_presence(id);}
    esp_err_t set_presence_reader(std::function<int(int)> f){return local_.set_presence_reader(std::move(f));}
    std::vector<AdapterRuntimeState> adapter_runtime_snapshot()const{return local_.adapter_runtime_snapshot();}
    esp_err_t commit_adapter_settings(system::BootId boot,std::vector<std::pair<uint8_t,uint64_t>> presence,uint64_t deadline,std::function<esp_err_t()> persist){return local_.commit_adapter_settings(boot,std::move(presence),deadline,std::move(persist));}
    bool accepts_local_input()const;
    esp_err_t request_activity(int index);
    bool select_activity(int index){return request_activity(index)==ESP_OK;}
    int activity_target()const{return local_.activity_target();}
    uint32_t activity_generation()const{return local_.activity_generation();}
    uint32_t activity_selection()const{return local_.activity_selection();}
    bool activity_setup_in_progress()const{return local_.activity_setup_in_progress();}
    bool press_key(uint8_t key,bool repeat=false);
    bool press_key_for_activity(uint8_t key,uint32_t generation,bool repeat=false){return accepts_local_input()&&local_.press_key_for_activity(key,generation,repeat);}
    bool release_key(uint8_t key=0xff);
    bool standby(){return request_activity(-1)==ESP_OK;}
    esp_err_t request_remote_activity(unsigned index);
    esp_err_t request_remote_activity(unsigned index,uint32_t generation);
    bool press_remote_key(uint8_t key,bool repeat=false){return press_key(key,repeat);}
    bool press_remote_key_for_activity(uint8_t key,uint32_t generation,bool repeat=false){return press_key_for_activity(key,generation,repeat);}
    bool release_remote_key(uint8_t key=0xff){return release_key(key);}
    esp_err_t repair_remote_activity(uint32_t generation);
    esp_err_t stop_remote_zone(){return stop_remote_zone(remote_activity_generation());}
    esp_err_t stop_remote_zone(uint32_t generation);
    uint32_t remote_activity_generation()const{return accepts_local_input()?activity_generation():0;}
    int remote_activity_target()const{return accepts_local_input()?activity_target():-1;}
    bool discover(){return local_.discover();}
    bool discover_owned(system::OctopusId owner,uint8_t adapter){return local_.discover_owned(owner,adapter);}
    bool refresh_owned_context(system::OctopusId owner,uint64_t revision,std::function<void(system::OwnedContextResult)> completed){return local_.refresh_owned_context(owner,revision,std::move(completed));}
    Snapshot snapshot()const{return local_.snapshot();}
    void on_change(std::function<void(const Snapshot&)> f){local_.on_change(std::move(f));}
    esp_err_t configure(config_store::Configuration,std::function<esp_err_t()> commit={});
    esp_err_t configure_passive_metadata(const config_store::Configuration&);
    esp_err_t request_activity(system::ZoneId,system::ActivityId,system::MessageId,std::optional<uint64_t> expected={});
    esp_err_t stop_zone(system::ZoneId);
    system::ExecutionSnapshot execution_snapshot()const;
    std::vector<system::CecObservation> native_observations()const;
    std::optional<config_store::Configuration> active_configuration()const{auto p=std::atomic_load(&active_);return p?std::optional<config_store::Configuration>(*p):std::nullopt;}
    std::optional<config_store::ConfiguredActivity> external_activity(unsigned)const;
    std::optional<config_store::ExternalSlot> external_slot(unsigned)const;
    std::optional<unsigned> external_activity_index(const Snapshot&)const;
    std::optional<unsigned> external_activity_index()const{return external_activity_index(local_.snapshot());}
    using OwnedSender=HubService::OwnedSender;
    void set_owned_sender(OwnedSender sender){std::atomic_store(&sender_,sender?std::make_shared<const OwnedSender>(std::move(sender)):std::shared_ptr<const OwnedSender>{});}
    void set_owned_observer(std::function<void(const system::OwnedAdapterSnapshot&)> observer){local_.set_owned_observer(std::move(observer));}
    void set_owned_authorizer(std::function<bool(const system::OwnedAuthorization&)> check){local_.set_owned_authorizer(std::move(check));}
    void set_owned_deadline(std::function<uint64_t(const system::OwnedAuthorization&)> check){local_.set_owned_deadline(std::move(check));}
    void set_owned_input_observer(std::function<void(const system::OwnedIntent&,const system::OwnedAuthorization&,const system::AdmissionResult&)> observer){local_.set_owned_input_observer(std::move(observer));}
    system::AdmissionResult submit_owned(const system::OwnedIntent& intent,const system::OwnedAuthorization& auth,uint32_t timeout=1000){return local_.submit_owned(intent,auth,timeout);}
    esp_err_t bind_owned_controller(system::OctopusId controller,uint64_t revision){return local_.bind_owned_controller(controller,revision);}
    bool update_owned_snapshot(system::OwnedAdapterSnapshot report){return local_.update_owned_snapshot(std::move(report));}
    std::vector<system::OwnedAdapterSnapshot> owned_snapshots()const{return local_.owned_snapshots();}
    bool fence_owned(system::OctopusId controller,system::LinkId link){return local_.fence_owned(controller,link);}
    bool owned_input_result(system::OctopusId owner,uint64_t generation,uint64_t sequence,bool present){return local_.owned_input_result(owner,generation,sequence,present);}
    uint64_t executing_configuration_revision()const{return local_.executing_configuration_revision();}
    esp_err_t retire_membership(){local_coordinator_.store(false);return local_.retire_membership();}
    esp_err_t begin_maintenance(){const auto result=local_.begin_maintenance();if(result==ESP_OK)maintenance_.store(true);return result;}
    void end_maintenance(){local_.end_maintenance();maintenance_.store(false);}
    bool maintenance_ready()const{return local_.maintenance_ready();}
private:
    bool dispatch_owned(const system::OwnedIntent&);
    std::vector<Activity> activities(const config_store::Configuration&)const;
    void update_outcomes_locked(const Snapshot&,const config_store::Configuration*)const;
    void refresh_outcomes()const;
    HubService local_;
    cec::BoardProfile profile_=cec::legacy_board2_profile;
    system::ConsortiumId consortium_;
    system::OctopusId octopus_;
    system::BootId boot_;
    std::shared_ptr<const config_store::Configuration> active_;
    std::shared_ptr<const std::string> hardware_error_;
    std::shared_ptr<const OwnedSender> sender_;
    std::atomic<bool> local_coordinator_{true},maintenance_{false};
    struct Pending {
        system::MessageId id;int target;uint32_t selection;
        config_store::RevisionPair revisions;
        std::string stage="admitted",error;
    };
    mutable std::mutex outcomes_mutex_;
    mutable std::vector<Pending> pending_;
};
}
