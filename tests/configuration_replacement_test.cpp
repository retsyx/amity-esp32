#include "intent_configuration_fixture.hpp"
#include "configuration_replacement.hpp"
#include <iostream>
int main(){
    auto config=direct_configuration();ConfigurationV2 with_switch;std::vector<std::string> tasks;std::string error;assert(propose_local_activity(config,{"Console","TV","Wii","AVR","AVR",6},{},random_source(),with_switch,tasks,error));config=with_switch;
    const auto old=id<model::OctopusId>(70),replacement=id<model::OctopusId>(71);config.octopuses.push_back({old,"Old member","",false,1});config.octopuses.push_back({replacement,"New member","",false,4});
    ReplacementApproval approval;approval.previous=old;approval.replacement=replacement;approval.previous_revoked=approval.previous_isolated=approval.replacement_enrolled=true;
    for(auto& component:config.components)for(auto& selector:component.record.qualified_names){selector.port.octopus_id=old;auto new_name=selector;new_name.port.octopus_id=replacement;new_name.port.tentacle_id=3;approval.equipment.push_back({component.record.component_id,selector,new_name,true});}
    for(auto side:{model::Side::front,model::Side::back})approval.ports.push_back({{config.consortium_id,old,0,side},{config.consortium_id,replacement,3,side}});
    ConfigurationV2 proposed;std::vector<model::ActivityId> affected;assert(propose_octopus_replacement(config,approval,proposed,affected,error));assert(affected.size()==2&&proposed.activities[1].switch_input==6&&proposed.activities[1].switch_device==config.activities[1].switch_device);
    for(const auto& c:proposed.components)for(const auto& n:c.record.qualified_names)assert(n.port.octopus_id==replacement&&n.port.tentacle_id==3);
    for(size_t n=0;n<config.activities.size();++n)assert(proposed.activities[n].activity_id==config.activities[n].activity_id);
    auto denied=approval;denied.previous_isolated=false;assert(!propose_octopus_replacement(config,denied,proposed,affected,error));denied=approval;denied.replacement_enrolled=false;assert(!propose_octopus_replacement(config,denied,proposed,affected,error));denied=approval;denied.previous=*config.controller_octopus_id;assert(!propose_octopus_replacement(config,denied,proposed,affected,error));denied=approval;denied.equipment.pop_back();assert(!propose_octopus_replacement(config,denied,proposed,affected,error));
    // Fresh-main restore rebases the original owner's selectors, keeps member
    // selectors and every AVR/switch choice, and imports no authority.
    auto backup=direct_configuration();assert(propose_local_activity(backup,{"Console","TV","Wii","AVR","AVR",6},{},random_source(),with_switch,tasks,error));backup=with_switch;backup.external_slots[0].slot=7;
    auto fresh=empty_configuration();const auto main=id<model::OctopusId>(88);fresh.controller_octopus_id=main;fresh.octopuses[0].octopus_id=main;fresh.consortium_id=id<model::ConsortiumId>(89);
    assert(!restore_main_configuration(backup,fresh,false,proposed,tasks,error));assert(restore_main_configuration(backup,fresh,true,proposed,tasks,error));assert(proposed.controller_octopus_id==main&&proposed.activities[1].switch_input==6&&proposed.external_slots[0].slot==7&&proposed.activity_order==backup.activity_order);
    for(const auto& c:proposed.components)for(const auto& selector:c.record.qualified_names)assert(selector.port.octopus_id==main&&selector.port.consortium_id==fresh.consortium_id);
    Memory memory;ConfigurationStore store(memory);ConfigurationV2 active;commit(store,fresh,{},active);const auto baseline=memory.values;
    for(unsigned cut=1;cut<=80;++cut){Memory trial;trial.values=baseline;ConfigurationStore transaction(trial);ConfigurationV2 before;assert(transaction.recover(before,error)==ConfigResult::ok);trial.operations=0;trial.cut_at=cut;try{if(transaction.stage(proposed,before.revisions(),error)==ConfigResult::ok){transaction.commit(before,error);}}catch(const PowerCut&){}trial.cut_at=0;ConfigurationStore reboot(trial);ConfigurationV2 recovered;assert(reboot.recover(recovered,error)==ConfigResult::ok&&recovered.controller_octopus_id==main);assert(recovered.activities.empty()||recovered.activities.size()==backup.activities.size());if(!recovered.activities.empty())assert(recovered.activities[1].switch_input==6||recovered.activities[0].switch_input==6);}
    std::cout<<"PASS selector replacement/restore: absent equipment allowed, original authority retained, explicit member isolation/enrollment, stable AVR/switch choices and 80 storage interruptions\n";
}
