#include "amity/system_control.hpp"
namespace amity {
esp_err_t SystemControl::transition_coordinator_authority(bool enabled,uint64_t epoch){
    auto config=std::atomic_load(&active_);
    if(enabled&&(epoch!=1||(config&&config->controller_octopus_id!=octopus_)))return ESP_ERR_INVALID_STATE;
    auto result=local_.transition_coordinator_authority(enabled,epoch);
    if(result==ESP_OK)local_coordinator_.store(enabled);
    return result;
}
std::vector<Activity> SystemControl::activities(const config_store::Configuration& config)const{
    std::vector<Activity> out;
    for(auto id:config_store::ordered_activities(config))for(const auto& a:config.activities)if(a.activity_id==id){
        auto value=[](const auto& ref){return ref?ref->str():std::string{};};
        out.push_back({a.label,value(a.display),value(a.source),value(a.audio),value(a.switch_device),a.switch_input,a.activity_id});
    }
    return out;
}
bool SystemControl::dispatch_owned(const system::OwnedIntent& intent){
    const auto sender=std::atomic_load(&sender_);return sender&&(*sender)(intent);
}
esp_err_t SystemControl::configure(config_store::Configuration config,std::function<esp_err_t()> commit){
    refresh_outcomes();
    std::string error;
    if(!accepts_local_input()||!config_store::validate_configuration(config,error)||config.consortium_id!=consortium_||config.controller_octopus_id!=octopus_)return ESP_ERR_INVALID_ARG;
    std::vector<system::ScopedSelector> selectors;
    for(const auto& c:config.components)for(const auto& name:c.record.qualified_names)
        selectors.push_back({c.record.component_id.str(),name});
    std::vector<system::OctopusId> owners;
    for(const auto& owner:config.octopuses)owners.push_back(owner.octopus_id);
    const auto previous=std::atomic_load(&active_);
    auto stored=std::make_shared<const config_store::Configuration>(std::move(config));
    const auto can_commit=[previous,stored](const Snapshot& state){
        if(!previous)return state.current_activity<0;
        if(state.applied_config_revision!=previous->config_revision||state.applied_presentation_revision!=previous->presentation_revision)return false;
        if(state.current_activity<0)return true;
        const auto order=config_store::ordered_activities(*previous);
        return static_cast<size_t>(state.current_activity)<order.size()&&config_store::retains_activity(*previous,*stored,order[state.current_activity]);
    };
    const auto persist=[this,stored,commit=std::move(commit)]{
        const auto result=commit?commit():ESP_OK;
        // Publish the saved order before owner execution can expose that new
        // order. The owner admission lock and queued-save reservation keep
        // selections out until its matching snapshot has been published.
        if(result==ESP_OK)std::atomic_store(&active_,stored);
        return result;
    };
    return local_.configure_scoped(activities(*stored),std::move(selectors),[this](const auto& intent){return dispatch_owned(intent);},octopus_,stored->config_revision,persist,stored->presentation_revision,std::move(owners),can_commit);
}
esp_err_t SystemControl::configure_passive_metadata(const config_store::Configuration& config){
    std::string error;if(local_coordinator_.load()||!config_store::validate_configuration(config,error)||config.consortium_id!=consortium_)return ESP_ERR_INVALID_ARG;
    std::atomic_store(&active_,std::make_shared<const config_store::Configuration>(config));return ESP_OK;
}
esp_err_t SystemControl::request_activity(system::ZoneId zone,system::ActivityId activity,system::MessageId request,std::optional<uint64_t> expected){
    refresh_outcomes();
    if(!accepts_local_input()||!request)return ESP_ERR_INVALID_STATE;
    if(expected&&*expected!=execution_snapshot().state_revision)return ESP_ERR_INVALID_STATE;
    auto config=std::atomic_load(&active_);if(!config)return ESP_ERR_INVALID_STATE;
    auto order=config_store::ordered_activities(*config);auto found=std::find(order.begin(),order.end(),activity);
    const auto target=std::find_if(config->activities.begin(),config->activities.end(),[&](const auto& a){return a.activity_id==activity&&a.zone_id==zone;});
    if(found==order.end()||target==config->activities.end())return ESP_ERR_INVALID_ARG;
    const int index=static_cast<int>(found-order.begin());
    std::lock_guard<std::mutex> lock(outcomes_mutex_);
    auto terminal=pending_.end();
    if(pending_.size()==32){terminal=std::find_if(pending_.begin(),pending_.end(),[](const auto& value){return value.stage!="admitted";});if(terminal==pending_.end())return ESP_ERR_NO_MEM;}
    uint32_t selection=0;const auto result=local_.request_activity(index,&selection,activity);
    if(result==ESP_OK){if(terminal!=pending_.end())pending_.erase(terminal);pending_.push_back({request,index,selection,config->revisions(),"admitted",{}});}
    return result;
}
esp_err_t SystemControl::stop_zone(system::ZoneId zone){
    const auto state=execution_snapshot();if(!state.sessions.empty()&&state.sessions.front().zone_id!=zone)return ESP_ERR_INVALID_ARG;
    return request_activity(-1);
}
void SystemControl::update_outcomes_locked(const Snapshot& state,const config_store::Configuration* config)const{
    for(auto& pending:pending_){
        if(pending.stage!="admitted")continue;
        if(!config||config->revisions()!=pending.revisions){pending.stage="rejected";pending.error="The saved configuration changed before this request completed";continue;}
        const auto processed=state.processed_activity_selection;
        if(processed!=pending.selection){
            if(static_cast<int32_t>(processed-pending.selection)>0){pending.stage="rejected";pending.error="Playback changed before this request completed";}
            continue;
        }
        if(state.activity_settling)continue;
        if(!state.activity_error.empty()){pending.stage="failed";pending.error=state.activity_error;}
        else if(state.current_activity==pending.target)pending.stage="applied";
        else {pending.stage="rejected";pending.error="Playback changed before this request completed";}
    }
}
void SystemControl::refresh_outcomes()const{
    const auto config=std::atomic_load(&active_);const auto state=local_.snapshot();
    std::lock_guard<std::mutex> lock(outcomes_mutex_);update_outcomes_locked(state,config.get());
}
system::ExecutionSnapshot SystemControl::execution_snapshot()const{
    system::ExecutionSnapshot out;auto config=std::atomic_load(&active_);const auto state=local_.snapshot();
    out.state_revision=local_.activity_selection();out.activity_settling=state.activity_settling;out.error=state.activity_error;
    const bool installed=config&&state.applied_config_revision==config->config_revision&&state.applied_presentation_revision==config->presentation_revision;
    if(installed){
        out.applied_config_revision=config->config_revision;out.applied_presentation_revision=config->presentation_revision;out.applied_policy_hash=config_store::executable_policy_hash(*config);
    }
    if(config&&state.current_activity>=0&&static_cast<size_t>(state.current_activity)<state.activities.size()){
        const auto id=state.activities[state.current_activity].activity_id;
        for(const auto& a:config->activities)if(a.activity_id==id)out.sessions.push_back({system::SessionId::parse(boot_.str()).value_or(system::SessionId{}),a.zone_id,id,state.activity_generation});
    }
    {std::lock_guard<std::mutex> lock(outcomes_mutex_);update_outcomes_locked(state,config.get());for(const auto& pending:pending_)out.activity_outcomes.push_back({pending.id,pending.stage,pending.error});}
    out.native_observations=native_observations();return out;
}
std::vector<system::CecObservation> SystemControl::native_observations()const{
    std::vector<system::CecObservation> out;
    for(const auto& a:local_.owned_snapshots())if(a.owner==octopus_)for(const auto& d:a.devices)if(d.resolved){system::CecObservation o;o.endpoint={{consortium_,a.owner,a.adapter,d.side},d.logical_address};o.osd_name=d.osd_name;o.owner_boot_id=a.boot;o.local_sequence=a.discovery_generation+1;o.scan_id=system::ScanId::parse(a.boot.str()).value_or(system::ScanId{});o.physical_address=d.physical_address;o.device_type=d.device_type;o.vendor_id=d.vendor_id;out.push_back(std::move(o));}
    return out;
}
std::optional<config_store::ConfiguredActivity> SystemControl::external_activity(unsigned slot)const{
    auto config=std::atomic_load(&active_);if(!config)return {};auto* activity=config_store::activity_for_slot(*config,slot);return activity&&activity->zone_id==config->primary_zone_id?std::optional<config_store::ConfiguredActivity>(*activity):std::nullopt;
}
std::optional<config_store::ExternalSlot> SystemControl::external_slot(unsigned slot)const{
    auto config=std::atomic_load(&active_);if(config)for(const auto& s:config->external_slots)if(s.slot==slot)return s;return {};
}
std::optional<unsigned> SystemControl::external_activity_index(const Snapshot& state)const{
    auto config=std::atomic_load(&active_);
    if(!config)return state.current_activity<0?std::nullopt:std::optional<unsigned>{static_cast<unsigned>(state.current_activity)};
    if(state.current_activity<0||static_cast<size_t>(state.current_activity)>=state.activities.size())return {};
    const auto id=state.activities[state.current_activity].activity_id;
    for(const auto& slot:config->external_slots)if(!slot.tombstone&&slot.activity_id==id){
        const auto* activity=config_store::activity_for_slot(*config,slot.slot);
        if(activity&&activity->zone_id==config->primary_zone_id)return slot.slot;
    }
    return {};
}

}
