#include "cec_hub_fixture.hpp"
#include "intent_configuration_fixture.hpp"
#include "amity/system_control.hpp"
#include <iostream>
using namespace amity::system;
static void drain(unsigned count=120){run_idle(static_cast<int>(count));}
static std::string outcome(const amity::SystemControl& control,MessageId id){for(const auto& result:control.execution_snapshot().activity_outcomes)if(result.request_id==id)return result.stage;assert(false);return {};}
static size_t presses(uint8_t key){return std::count_if(fixture_sent.begin(),fixture_sent.end(),[&](const auto& s){return s.frame.size==3&&s.frame.data[1]==0x44&&s.frame.data[2]==key;});}
int main(){
    fixture_present=true;fixture_tick=100000;
    auto config=direct_configuration();const auto boot=id<BootId>(90);
    amity::SystemControl original;original.set_identity(config.consortium_id,*config.controller_octopus_id);original.set_boot_identity(boot);assert(original.start(1,2)==ESP_OK);
    assert(original.configure(config)==ESP_OK);drain(1000);
    assert(original.accepts_local_input()&&original.active_configuration());
    const auto first=config.activities[0].activity_id;
    auto generation=original.remote_activity_generation();const auto selection=original.activity_selection();
    assert(original.request_remote_activity(0,generation+1)==ESP_ERR_INVALID_STATE&&original.activity_selection()==selection);
    assert(original.request_remote_activity(0,generation)==ESP_OK);drain();assert(original.snapshot().current_activity==0&&original.execution_snapshot().sessions[0].activity_id==first);
    generation=original.remote_activity_generation();const auto selected=original.activity_selection();
    assert(original.request_remote_activity(0,generation)==ESP_OK&&original.activity_selection()==selected+1);drain();
    const auto repair=original.activity_selection();assert(original.repair_remote_activity(original.remote_activity_generation())==ESP_OK);drain();assert(original.activity_selection()==repair);
    // An accepted same-target selection must execute before its UI operation
    // completes; later selections do not rewrite a latched terminal outcome.
    const auto same=id<MessageId>(750);assert(original.request_activity(config.primary_zone_id,first,same,original.execution_snapshot().state_revision)==ESP_OK);assert(outcome(original,same)=="admitted");drain();assert(outcome(original,same)=="applied");
    const auto superseded=id<MessageId>(751);assert(original.request_activity(config.primary_zone_id,first,superseded,original.execution_snapshot().state_revision)==ESP_OK);assert(original.stop_remote_zone()==ESP_OK);drain();assert(outcome(original,superseded)=="rejected"&&outcome(original,same)=="applied");assert(original.request_remote_activity(0)==ESP_OK);drain();
    // Normal button-up affects one key only and advances neither activity nor
    // the accepted-selection gate. Both initial presses can enter their CEC
    // FIFOs while the first key's completion remains outstanding.
    generation=original.remote_activity_generation();const auto before_keys=original.activity_selection();
    fixture_async_autocomplete=false;fixture_sent.clear();assert(original.press_remote_key_for_activity(0x41,generation,true));assert(original.press_remote_key_for_activity(3,generation,true));drain(5);
    assert(std::count_if(fixture_sent.begin(),fixture_sent.end(),[](const auto& s){return s.frame.size==3&&s.frame.data[1]==0x44;})==2);
    assert(original.release_remote_key(3));drain(4);assert(original.remote_activity_generation()==generation&&original.activity_selection()==before_keys);
    fixture_async_autocomplete=true;fixture_complete_all();drain(12);fixture_sent.clear();drain(12);assert(presses(0x41)>0&&presses(3)==0);assert(original.release_remote_key(0x41));drain(8);
    // A stable ID request uses presentation order; integration slots stay fixed.
    auto reordered=config;auto second=config.activities[0];second.activity_id=id<ActivityId>(800);second.label="Other screen";reordered.activities.push_back(second);reordered.zones[0].activity_order.push_back(second.activity_id);reordered.activity_order={second.activity_id,first};reordered.external_slots.push_back({1,second.activity_id,false,false,{}});reordered.config_revision=2;reordered.presentation_revision=2;
    assert(original.configure(reordered)==ESP_OK);assert(original.execution_snapshot().sessions.empty());drain();const auto request=id<MessageId>(801);assert(original.request_activity(second.zone_id,first,request,original.execution_snapshot().state_revision)==ESP_OK);drain();assert(original.snapshot().current_activity==1&&original.execution_snapshot().sessions[0].activity_id==first);assert(original.external_activity(0)->activity_id==first&&original.external_activity_index()==0);assert(outcome(original,same)=="applied");
    auto sparse=reordered;sparse.config_revision=3;sparse.presentation_revision=3;sparse.external_slots[0].slot=7;assert(original.configure(sparse)==ESP_OK);drain();assert(original.request_activity(7)==ESP_OK);drain();assert(original.snapshot().current_activity==1&&original.external_activity_index()==7);assert(original.request_activity(0)==ESP_ERR_INVALID_ARG);
    assert(original.request_activity(second.zone_id,first,id<MessageId>(802),original.execution_snapshot().state_revision+1)==ESP_ERR_INVALID_STATE);
    // The original algorithm decides what a missing AVR/source means. Saved
    // selections do not acquire a persistent unavailable state or lose fields.
    assert(original.stop_remote_zone()==ESP_OK);drain();
    auto offline=switch_configuration(sparse,"Offline console");offline.config_revision=4;
    const auto remote=id<OctopusId>(70);offline.octopuses.push_back({remote,"amity-offline","amity-offline",false,1});for(auto& c:offline.components)if(c.record.label=="AVR")for(auto& name:c.record.qualified_names)name.port.octopus_id=remote;
    const auto saved=serialize_configuration(offline);assert(original.configure(offline)==ESP_OK);drain();const auto console=std::find_if(offline.activities.begin(),offline.activities.end(),[](const auto& a){return a.label=="Offline console";});assert(console!=offline.activities.end());assert(original.request_activity(console->zone_id,console->activity_id,id<MessageId>(803),original.execution_snapshot().state_revision)==ESP_OK);drain(1000);assert(original.active_configuration()&&serialize_configuration(*original.active_configuration())==saved);assert(original.activity_target()==2);
    // Configuration persistence runs only after ordinary IPC capacity has been
    // reserved. A full queue and a failed commit leave the active intent intact.
    for(unsigned i=0;i<32;++i)assert(original.press_key(0xff));assert(!original.press_key(0xff));unsigned writes=0;auto changed=offline;changed.config_revision=5;changed.activities[0].switch_input=7;
    assert(original.configure(changed,[&]{++writes;return ESP_OK;})==ESP_ERR_NO_MEM&&writes==0);assert(serialize_configuration(*original.active_configuration())==saved);
    assert(original.begin_maintenance()!=ESP_OK&&original.accepts_local_input());drain();
    assert(original.configure(changed,[&]{++writes;return ESP_FAIL;})==ESP_FAIL&&writes==1);assert(serialize_configuration(*original.active_configuration())==saved);
    // Equal activity labels are insufficient to acknowledge a selector-only
    // update: the owner task must have installed its exact configuration.
    auto relocated=offline;relocated.config_revision=5;for(auto& component:relocated.components)if(component.record.label=="AVR")component.record.qualified_names[0].osd_name="AVR2";
    assert(original.configure(relocated)==ESP_OK);assert(original.execution_snapshot().applied_config_revision!=relocated.config_revision);drain();assert(original.execution_snapshot().applied_config_revision==relocated.config_revision);
    auto labels=relocated;++labels.presentation_revision;labels.activities[0].label="Renamed";assert(original.configure(labels)==ESP_OK);assert(original.execution_snapshot().applied_presentation_revision==0);drain();assert(original.execution_snapshot().applied_presentation_revision==labels.presentation_revision);
    // Original setup keeps the prior effective selection during source lookup.
    // Owner loss ends that lookup with an error, even when the requested index
    // becomes the effective selection as the continuation unwinds.
    auto interrupted_config=direct_configuration();interrupted_config.octopuses.push_back({remote,"amity-offline","amity-offline",false,1});for(auto& component:interrupted_config.components)if(component.record.component_id==interrupted_config.activities[0].source)component.record.qualified_names[0].port.octopus_id=remote;
    amity::SystemControl interrupted;interrupted.set_identity(interrupted_config.consortium_id,*interrupted_config.controller_octopus_id);interrupted.set_boot_identity(id<BootId>(92));interrupted.set_owned_sender([](const auto&){return true;});assert(interrupted.start(1,2)==ESP_OK&&interrupted.configure(interrupted_config)==ESP_OK);drain(1000);
    const auto broken=id<MessageId>(805);assert(interrupted.request_activity(interrupted_config.primary_zone_id,interrupted_config.activities[0].activity_id,broken,interrupted.execution_snapshot().state_revision)==ESP_OK);drain(4);assert(interrupted.snapshot().current_activity==-1&&outcome(interrupted,broken)=="admitted");interrupted.fence_owned(remote,id<LinkId>(93));drain();assert(interrupted.snapshot().current_activity==0&&!interrupted.snapshot().activity_error.empty()&&outcome(interrupted,broken)=="failed");
    // A headless member cannot originate activity, remote or integration work.
    amity::SystemControl member;member.set_identity(config.consortium_id,remote);member.set_boot_identity(id<BootId>(91));member.set_coordinator_authority(false,1);assert(member.start(1,2)==ESP_OK);assert(!member.accepts_local_input()&&member.remote_activity_generation()==0);assert(member.request_remote_activity(0)==ESP_ERR_INVALID_STATE&&!member.press_remote_key(1)&&!member.release_remote_key());assert(member.request_activity(config.primary_zone_id,first,id<MessageId>(804))==ESP_ERR_INVALID_STATE);
    std::cout<<"PASS actual activity facade: generation and gate/repair parity, independent key-up, stable-ID ordered selection, offline AVR/switch intent and IPC-before-persistence\n";
}
