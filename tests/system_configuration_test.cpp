#include "intent_configuration_fixture.hpp"
#include "candidate_network.hpp"
#include <iostream>

void codecs(){
    std::string error;auto c=direct_configuration();ConfigurationV2 decoded;const auto text=serialize_configuration_v2(c);
    assert(text.find("\"version\":3")!=std::string::npos&&parse_configuration_v2(text,decoded,error)&&serialize_configuration_v2(decoded)==text);
    for(const auto* field:{"enabled","route_id","domains","binding_policies","resources","signal_edges","routes","capabilities","physical_address","logical_address"})assert(text.find(std::string("\"")+field+"\"")==std::string::npos);
    auto reject=[&](std::string raw){ConfigurationV2 ignored;assert(!parse_configuration_v2(raw,ignored,error));};
    reject(text+" trailing");reject(std::string(9,'[')+std::string(9,']'));reject(std::string(32769,' '));reject(text.substr(0,text.size()-1)+",\"version\":3}");
    auto extra=text;extra.insert(extra.size()-1,",\"enabled\":true");reject(extra);
    extra=text;auto at=extra.find("\"config_revision\":\"1\"");extra.replace(at,21,"\"config_revision\":1");reject(extra);
    extra=text;at=extra.find("\"osd_name\":\"Source\"");extra.replace(at,19,"\"logical_address\":4");reject(extra);
    auto presentation=c;presentation.activities[0].label="Renamed";presentation.components[0].record.label="Player";presentation.octopuses[0].label="Shelf";assert(executable_policy_hash(presentation)==executable_policy_hash(c));
    auto semantic=c;semantic.activities[0].switch_input=6;assert(executable_policy_hash(semantic)!=executable_policy_hash(c));
    semantic=c;semantic.components[0].record.qualified_names[0].port.tentacle_id=1;assert(!validate_configuration_v2(semantic,error));
    semantic=c;semantic.components.push_back(semantic.components[0]);assert(!validate_configuration_v2(semantic,error));
    semantic=c;auto alias=semantic.components[0].record.qualified_names[0];alias.port.side=alias.port.side==model::Side::front?model::Side::back:model::Side::front;semantic.components[0].record.qualified_names.push_back(alias);assert(!validate_configuration_v2(semantic,error));
    semantic=c;semantic.components[0].record.qualified_names.clear();semantic.components[0].kind="placeholder";assert(!validate_configuration_v2(semantic,error));
    semantic.components[0].record.label=semantic.components[0].generated_label="Legacy non CEC console";assert(validate_configuration_v2(semantic,error));

}
void legacy_and_absence(){
    auto c=direct_configuration();std::string error;ConfigurationV2 decoded;
    assert(parse_configuration_v2(legacy_v2_json(c),decoded,error)&&decoded.legacy_encoding);
    assert(decoded.activities[0].activity_id==c.activities[0].activity_id&&decoded.activities[0].source==c.activities[0].source&&decoded.activities[0].display==c.activities[0].display);
    for(const auto& component:decoded.components){assert(component.record.qualified_names.size()==1);const auto& name=component.record.qualified_names[0];assert(name.port.octopus_id==*c.controller_octopus_id&&name.port.tentacle_id==0&&name.osd_name==component.generated_label);assert(name.port.side==(component.record.component_id==decoded.activities[0].display?model::Side::front:model::Side::back));}
    assert(serialize_configuration_v2(decoded).find("enabled")==std::string::npos);
    // All ordinary AVR/switch selections survive with no observations at all.
    auto base=empty_configuration();Memory memory;ConfigurationStore store(memory);LegacyConfiguration legacy;
    legacy.activities={{"Watch","TV","Source","AVR","",0},{"Play Wii","TV","Wii","AVR","AVR",6}};legacy.quirks.push_back({0x123456,{0x04},{0x36}});legacy.aliases.push_back({1,true,"Console"});
    std::vector<std::string> tasks;assert(migrate_legacy(store,memory,"original v1",legacy,base.consortium_id,*base.controller_octopus_id,{},random_source(),decoded,tasks,error)==ConfigResult::ok&&tasks.empty());
    const auto console=std::find_if(decoded.activities.begin(),decoded.activities.end(),[](const auto& a){return a.label=="Play Wii";});assert(decoded.activities.size()==2&&console!=decoded.activities.end()&&console->switch_input==6&&console->audio==console->switch_device&&decoded.quirks.size()==1&&!decoded.quirks[0].component_id);
    assert(decoded.external_slots[1].alias=="Console"&&decoded.external_slots[1].hidden&&memory.values.at("legacy_backup")=="original v1");
    const auto ids=decoded.activities;assert(store.commit(decoded,error)==ConfigResult::ok);ConfigurationStore reboot(memory);assert(reboot.recover(decoded,error)==ConfigResult::ok&&decoded.activities[1].activity_id==ids[1].activity_id);
}
void journal(){
    auto c=direct_configuration();std::string error;Memory baseline;ConfigurationStore original(baseline);ConfigurationV2 active;commit(original,c,{},active);
    auto labels=active;labels.activities[0].label="Cinema";assert(original.stage(labels,active.revisions(),error)==ConfigResult::ok&&original.staged()->revisions()==RevisionPair({1,2}));assert(original.commit(active,error)==ConfigResult::ok);
    auto semantic=active;semantic.activities[0].switch_input=4;
    const auto before=baseline.values;
    for(unsigned cut=1;cut<=80;++cut){Memory memory;memory.values=before;ConfigurationStore transaction(memory);ConfigurationV2 old;assert(transaction.recover(old,error)==ConfigResult::ok);memory.operations=0;memory.cut_at=cut;try{if(transaction.stage(semantic,old.revisions(),error)==ConfigResult::ok){transaction.commit(old,error);}}catch(const PowerCut&){}memory.cut_at=0;ConfigurationStore reboot(memory);ConfigurationV2 recovered;assert(reboot.recover(recovered,error)==ConfigResult::ok);assert(recovered.activities[0].switch_input==0||recovered.activities[0].switch_input==4);assert(recovered.activities[0].activity_id==c.activities[0].activity_id);}
    // Old v2 journal bytes remain readable until the protected-boot caller
    // explicitly stages/commits the canonical v3 replacement.
    Memory old;install_legacy_record(old,c);const auto record=old.values.at("c0");ConfigurationStore upgrade(old);assert(upgrade.recover(active,error)==ConfigResult::ok&&active.legacy_encoding&&old.values.at("c0")==record);
    const auto saved=old.values;
    for(unsigned cut=1;cut<=80;++cut){Memory memory;memory.values=saved;ConfigurationStore transaction(memory);ConfigurationV2 recovered;assert(transaction.recover(recovered,error)==ConfigResult::ok);memory.operations=0;memory.cut_at=cut;try{if(transaction.stage(recovered,recovered.revisions(),error)==ConfigResult::ok){transaction.commit(recovered,error);}}catch(const PowerCut&){}memory.cut_at=0;ConfigurationStore reboot(memory);assert(reboot.recover(recovered,error)==ConfigResult::ok&&recovered.activities[0].activity_id==c.activities[0].activity_id);}
    const std::vector<std::string> obsolete{"intent0","intent1","imark0","imark1","slice0","slice1","smark0","smark1"};
    for(const auto& key:obsolete)old.values[key]="obsolete distributed state";
    ConfigurationStore cleanup(old);assert(cleanup.recover(active,error)==ConfigResult::ok);
    for(const auto& key:obsolete)assert(!old.values.count(key));
    // Saving offline member locations does not require that device to approve
    // activity policy. Only the original controller owns this configuration.
    auto peer=active;const auto owner=id<model::OctopusId>(70);peer.octopuses.push_back({owner,"offline","offline",false,1});peer.components[0].record.qualified_names[0].port.octopus_id=owner;assert(cleanup.stage(peer,active.revisions(),error)==ConfigResult::ok);
}
void ownership_and_slots(){
    auto c=direct_configuration();std::string error;bool changed=false;assert(bind_original_controller(c,*c.controller_octopus_id,changed,error)&&!changed);assert(!bind_original_controller(c,id<model::OctopusId>(80),changed,error));
    auto unbound=c;unbound.controller_octopus_id.reset();assert(bind_original_controller(unbound,*c.controller_octopus_id,changed,error)&&changed);
    auto candidate=c;auto other=c.activities[0];other.activity_id=id<model::ActivityId>(71);other.label="Other";candidate.activities.push_back(other);candidate.zones[0].activity_order.push_back(other.activity_id);assert(reconcile_external_slots(c,candidate,error)&&candidate.external_slots.size()==2);
    auto deleted=candidate;deleted.activities.erase(deleted.activities.begin());deleted.zones[0].activity_order.erase(deleted.zones[0].activity_order.begin());deleted.activity_order.erase(deleted.activity_order.begin());assert(reconcile_external_slots(candidate,deleted,error)&&deleted.external_slots[0].tombstone&&deleted.external_slots[1].activity_id==other.activity_id);
    auto reused=deleted;reused.external_slots[0].activity_id=other.activity_id;assert(!reconcile_external_slots(deleted,reused,error));
    auto hardware=c;assert(reconcile_hardware_profile(hardware,*c.controller_octopus_id,4,changed,error)&&changed&&hardware.octopuses[0].supported_tentacles==4);assert(serialize_configuration_v2(hardware).find("domains")==std::string::npos);
}
void candidate_network_recovery(){
    CandidateNetwork value;value.join_id=id<model::JoinId>(170);value.proposal_digest=std::string(64,'c');value.old_ssid="Old offline network";value.old_password="retained-secret";value.old_driver_configuration=std::string("binary\0configuration",20);value.ssid="Candidate LAN";value.password="candidate-secret";std::string error;CandidateNetwork recovered;
    Memory memory;CandidateNetworkJournal journal(memory);assert(journal.stage(value,error)==ConfigResult::ok);assert(journal.recover(recovered,error)==ConfigResult::ok&&!recovered.decided&&recovered.old_driver_configuration==value.old_driver_configuration);const auto before=memory.values;
    assert(journal.finish(value.join_id,value.proposal_digest,error)==ConfigResult::conflict);assert(journal.decide(id<model::JoinId>(171),value.proposal_digest,true,error)==ConfigResult::conflict);
    assert(journal.abandon(id<model::JoinId>(171),value.proposal_digest,error)==ConfigResult::conflict);assert(journal.abandon(value.join_id,std::string(64,'d'),error)==ConfigResult::conflict);
    // The adapter restores the exact old driver bytes before abandon. A cut
    // before cleanup retains the complete rollback material for passive retry.
    for(unsigned cut=1;cut<=8;++cut){Memory interrupted;interrupted.values=before;interrupted.cut_at=cut;CandidateNetworkJournal cancellation(interrupted);try{cancellation.abandon(value.join_id,value.proposal_digest,error);}catch(const PowerCut&){}interrupted.cut_at=0;CandidateNetworkJournal boot(interrupted);const auto status=boot.recover(recovered,error);assert(status==ConfigResult::missing||(status==ConfigResult::ok&&!recovered.decided&&recovered.old_driver_configuration==value.old_driver_configuration&&recovered.old_password==value.old_password));}
    for(unsigned cut=1;cut<=12;++cut){Memory interrupted;interrupted.values=before;interrupted.cut_at=cut;CandidateNetworkJournal transaction(interrupted);try{transaction.decide(value.join_id,value.proposal_digest,true,error);}catch(const PowerCut&){}interrupted.cut_at=0;CandidateNetworkJournal reboot(interrupted);assert(reboot.recover(recovered,error)==ConfigResult::ok&&recovered.old_password==value.old_password&&recovered.password==value.password);}
    assert(journal.decide(value.join_id,value.proposal_digest,true,error)==ConfigResult::ok);assert(journal.decide(value.join_id,value.proposal_digest,false,error)==ConfigResult::conflict);assert(journal.abandon(value.join_id,value.proposal_digest,error)==ConfigResult::conflict);const auto decided=memory.values;
    for(unsigned cut=1;cut<=8;++cut){Memory interrupted;interrupted.values=decided;interrupted.cut_at=cut;CandidateNetworkJournal transaction(interrupted);try{transaction.finish(value.join_id,value.proposal_digest,error);}catch(const PowerCut&){}interrupted.cut_at=0;CandidateNetworkJournal reboot(interrupted);const auto status=reboot.recover(recovered,error);assert(status==ConfigResult::missing||(status==ConfigResult::ok&&recovered.decided));}
    assert(journal.finish(value.join_id,value.proposal_digest,error)==ConfigResult::ok&&memory.values.at("net_candidate")=="NETWORK_DONE\n");Memory damaged;damaged.values=before;damaged.values["net_candidate"][80]^=1;CandidateNetworkJournal corrupt(damaged);assert(corrupt.recover(recovered,error)==ConfigResult::recovery_required);
}
int main(){codecs();legacy_and_absence();journal();ownership_and_slots();candidate_network_recovery();std::cout<<"PASS intent-only configuration: scoped selectors, AVR/switch migration without discovery, old-v2 recovery, preserved slots/authority, 160 storage interruptions\n";}
