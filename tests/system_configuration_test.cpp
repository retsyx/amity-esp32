#include "intent_configuration_fixture.hpp"
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>

std::string json_text(cJSON* value){char* text=cJSON_PrintUnformatted(value);assert(text);std::string result(text);cJSON_free(text);return result;}
void canonical_bytes_and_projections(){
    // Frozen current-format example, independent of the C++ encoder. cJSON
    // supplies the established escaping and field-preserving hash projections.
    const char* fixture=R"JSON({
      "version":4,"consortium_id":"00000000000000000000000000000001",
      "config_revision":"18446744073709551615","presentation_revision":"18446744073709551614",
      "controller_octopus_id":"00000000000000000000000000000002",
      "activity_order":["0000000000000000000000000000001f","0000000000000000000000000000001e"],
      "octopuses":[
        {"octopus_id":"00000000000000000000000000000002","supported_tentacles":4},
        {"octopus_id":"00000000000000000000000000000003","supported_tentacles":1}],
      "components":[
        {"component_id":"00000000000000000000000000000014","label":"Player \"A\"","kind":"source","qualified_name":{"octopus_id":"00000000000000000000000000000002","tentacle_id":3,"side":"back","osd_name":"Player \"A\""}},
        {"component_id":"00000000000000000000000000000015","label":"Café / TV","kind":"display","qualified_name":{"octopus_id":"00000000000000000000000000000003","tentacle_id":0,"side":"front","osd_name":"TV"}},
        {"component_id":"00000000000000000000000000000016","label":"Turntable (analog input)","kind":"placeholder","qualified_name":null}],
      "activities":[
        {"activity_id":"0000000000000000000000000000001e","label":"Watch \\","source":"00000000000000000000000000000014","display":"00000000000000000000000000000015","audio":"00000000000000000000000000000015","switch_device":"00000000000000000000000000000015","switch_input":255},
        {"activity_id":"0000000000000000000000000000001f","label":"Empty","source":null,"display":null,"audio":null,"switch_device":null,"switch_input":0}],
      "external_slots":[
        {"slot":0,"activity_id":"0000000000000000000000000000001e","tombstone":false,"hidden":true,"alias":"Favorite 🐙"},
        {"slot":15,"activity_id":"00000000000000000000000000000020","tombstone":true,"hidden":false,"alias":""}]
    })JSON";
    for(bool global_order:{false,true}){
        auto* saved=cJSON_Parse(fixture);assert(saved);
        if(!global_order)cJSON_DeleteItemFromObjectCaseSensitive(saved,"activity_order");
        const auto expected=json_text(saved);Configuration c;std::string error;
        assert(parse_configuration(expected,c,error));
        std::reverse(c.octopuses.begin(),c.octopuses.end());std::reverse(c.components.begin(),c.components.end());
        std::reverse(c.activities.begin(),c.activities.end());std::reverse(c.external_slots.begin(),c.external_slots.end());
        assert(serialize_configuration(c)==expected);
        const auto retain=[](cJSON* object,const std::set<std::string>& fields){for(auto* field=object->child;field;){auto* next=field->next;if(!fields.count(field->string))cJSON_DeleteItemFromObjectCaseSensitive(object,field->string);field=next;}};
        auto* presentation=cJSON_Duplicate(saved,true);assert(presentation);
        retain(presentation,{"components","activities","external_slots","activity_order"});
        for(const auto* category:{"components","activities","external_slots"})
            for(auto* row=cJSON_GetObjectItemCaseSensitive(presentation,category)->child;row;row=row->next)
                retain(row,{"component_id","activity_id","slot","label","hidden","alias"});
        assert(presentation_hash(c)==sha256_hex(json_text(presentation)));cJSON_Delete(presentation);
        for(const auto* field:{"config_revision","presentation_revision","activity_order"})cJSON_DeleteItemFromObjectCaseSensitive(saved,field);
        for(const auto* category:{"octopuses","components","activities","external_slots"})
            for(auto* row=cJSON_GetObjectItemCaseSensitive(saved,category)->child;row;row=row->next)
                for(const auto* field:{"label","kind","hidden","alias"})cJSON_DeleteItemFromObjectCaseSensitive(row,field);
        assert(serialize_configuration(c,true)==json_text(saved));assert(executable_policy_hash(c)==sha256_hex(json_text(saved)));cJSON_Delete(saved);
        // Canonicalization must reorder the owned candidate without changing
        // presentation order or bytes across a restart.
        Memory memory;ConfigurationStore store(memory);ConfigurationSnapshot active;commit(store,c,{},active);
        assert(std::is_sorted(active->configuration.components.begin(),active->configuration.components.end(),[](const auto& a,const auto& b){return a.record.component_id<b.record.component_id;}));
        ConfigurationStore reboot(memory);ConfigurationSnapshot recovered;assert(reboot.recover(recovered,error)==ConfigResult::ok);
        assert(active->canonical==recovered->canonical);
    }
}

void saved_text_and_staged_record_integrity(){
    for(unsigned field=0;field<3;++field)for(char invalid:{'\0','\n','\x1f','\x7f'}){
        auto c=direct_configuration();std::string* value=nullptr;
        switch(field){case 0:value=&c.components[0].record.label;break;
        case 1:value=&c.activities[0].label;break;default:value=&c.external_slots[0].alias;break;}
        value->assign("before");value->push_back(invalid);value->append("after");
        std::string error;Memory memory;ConfigurationStore store(memory);
        assert(!validate_configuration(c,error)&&store.stage(c,{},error)==ConfigResult::invalid&&memory.writes==0);
    }
    Memory memory;ConfigurationStore store(memory);ConfigurationSnapshot active;commit(store,direct_configuration(),{},active);
    const auto committed=memory.values;const auto active_text=active->canonical;auto next=active->configuration;next.activities[0].label="Next";std::string error;
    assert(store.stage(next,active->configuration.revisions(),error)==ConfigResult::ok);
    for(const auto* key:{"c0","c1"})if(memory.values.count(key)&&(!committed.count(key)||committed.at(key)!=memory.values.at(key))){memory.values[key].back()=' ';break;}
    const auto damaged=memory.values;ConfigurationSnapshot rejected;assert(store.commit(rejected,error)==ConfigResult::recovery_required&&memory.values==damaged);
    ConfigurationStore reboot(memory);ConfigurationSnapshot recovered;assert(reboot.recover(recovered,error)==ConfigResult::ok&&recovered->canonical==active_text);
}

void codecs(){
    std::string error;auto c=direct_configuration();Configuration decoded;const auto text=serialize_configuration(c);
    assert(text.find("\"version\":4")!=std::string::npos&&parse_configuration(text,decoded,error)&&serialize_configuration(decoded)==text);
    for(const auto* field:{"enabled","route_id","domains","binding_policies","resources","signal_edges","routes","capabilities","physical_address","logical_address","zones","primary_zone_id","zone_id","generated_label","label_override","qualified_names"})assert(text.find(std::string("\"")+field+"\"")==std::string::npos);
    // Saved selectors omit the consortium; parsing scopes them by the document's.
    const auto group=c.consortium_id.str();const auto first=text.find("\"consortium_id\"");
    assert(first!=std::string::npos&&text.find("\"consortium_id\"",first+1)==std::string::npos);
    auto moved=text;moved.replace(moved.find(group),group.size(),id<model::ConsortiumId>(9).str());Configuration rescoped;
    assert(parse_configuration(moved,rescoped,error)&&rescoped.consortium_id==id<model::ConsortiumId>(9)&&rescoped.components.size()==2);
    for(const auto& component:rescoped.components)assert(component.record.qualified_name&&component.record.qualified_name->port.consortium_id==rescoped.consortium_id);
    auto reject=[&](std::string raw){Configuration ignored;assert(!parse_configuration(raw,ignored,error));};
    reject(text+" trailing");reject(std::string(9,'[')+std::string(9,']'));reject(std::string(configuration_payload_limit+1,' '));reject(text.substr(0,text.size()-1)+",\"version\":3}");
    auto extra=text;extra.insert(extra.size()-1,",\"enabled\":true");reject(extra);
    extra=text;auto at=extra.find("\"config_revision\":\"1\"");extra.replace(at,21,"\"config_revision\":1");reject(extra);
    extra=text;at=extra.find("\"osd_name\":\"Source\"");extra.replace(at,19,"\"logical_address\":4");reject(extra);
    auto presentation=c;presentation.activities[0].label="Renamed";presentation.components[0].record.label="Player";assert(executable_policy_hash(presentation)==executable_policy_hash(c));
    auto semantic=c;semantic.activities[0].switch_input=6;assert(executable_policy_hash(semantic)!=executable_policy_hash(c));
    semantic=c;semantic.components[0].record.qualified_name->port.tentacle_id=1;assert(!validate_configuration(semantic,error));
    semantic=c;semantic.components[0].record.qualified_name->port.consortium_id=id<model::ConsortiumId>(9);assert(!validate_configuration(semantic,error));
    semantic=c;semantic.components.push_back(semantic.components[0]);assert(!validate_configuration(semantic,error));
    semantic=c;auto copy=semantic.components[0];copy.record.component_id=id<model::ComponentId>(500);semantic.components.push_back(copy);assert(!validate_configuration(semantic,error));
    semantic=c;semantic.components[0].record.qualified_name.reset();semantic.components[0].kind="placeholder";assert(!validate_configuration(semantic,error));
    semantic.components[0].record.label="Non-CEC console input";assert(validate_configuration(semantic,error));
    const auto unnamed=serialize_configuration(semantic);assert(unnamed.find("\"qualified_name\":null")!=std::string::npos);
    Configuration placeholder;assert(parse_configuration(unnamed,placeholder,error)&&serialize_configuration(placeholder)==unnamed);
    semantic.components[0].kind="source";assert(!validate_configuration(semantic,error));
}
void current_format_only(){
    auto c=switch_configuration(direct_configuration());std::string error;Configuration decoded;
    const auto encoded=serialize_configuration(c);
    const auto rejected=[&](const std::string& raw){Configuration unchanged=c;assert(!parse_configuration(raw,unchanged,error));assert(serialize_configuration(unchanged)==encoded);};
    const auto modify=[&](const std::function<void(cJSON*)>& change){auto* root=cJSON_Parse(encoded.c_str());assert(root);change(root);char* text=cJSON_PrintUnformatted(root);std::string raw(text);cJSON_free(text);cJSON_Delete(root);return raw;};
    for(int version:{1,2,3,5})rejected(modify([&](cJSON* root){cJSON_ReplaceItemInObjectCaseSensitive(root,"version",cJSON_CreateNumber(version));}));
    rejected(modify([](cJSON* root){cJSON_ReplaceItemInObjectCaseSensitive(root,"version",cJSON_CreateBool(true));}));
    rejected(modify([](cJSON* root){cJSON_ReplaceItemInObjectCaseSensitive(root,"version",cJSON_CreateNumber(3.5));}));
    for(const auto* key:{"version","consortium_id","config_revision","presentation_revision","controller_octopus_id","octopuses","components","activities","external_slots"})
        rejected(modify([&](cJSON* root){cJSON_DeleteItemFromObjectCaseSensitive(root,key);}));
    for(const auto* category:{"octopuses","components","activities","external_slots"}){
        auto* root=cJSON_Parse(encoded.c_str());auto* row=cJSON_GetObjectItemCaseSensitive(root,category)->child;std::vector<std::string> required;
        for(auto* field=row->child;field;field=field->next)required.push_back(field->string);
        cJSON_Delete(root);
        for(const auto& key:required)rejected(modify([&](cJSON* value){cJSON_DeleteItemFromObjectCaseSensitive(cJSON_GetObjectItemCaseSensitive(value,category)->child,key.c_str());}));
        rejected(modify([&](cJSON* value){cJSON_AddBoolToObject(cJSON_GetObjectItemCaseSensitive(value,category)->child,"unrecognized",true);}));
    }
    // Saved selectors are scoped by the document's consortium only.
    const auto selector=[](cJSON* root){return cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(root,"components")->child,"qualified_name");};
    rejected(modify([&](cJSON* root){cJSON_AddStringToObject(selector(root),"consortium_id","00000000000000000000000000000001");}));
    for(const auto* key:{"octopus_id","tentacle_id","side","osd_name"})rejected(modify([&](cJSON* root){cJSON_DeleteItemFromObjectCaseSensitive(selector(root),key);}));
    rejected(modify([](cJSON* root){auto* row=cJSON_GetObjectItemCaseSensitive(root,"components")->child;auto* names=cJSON_CreateArray();
        cJSON_AddItemToArray(names,cJSON_DetachItemFromObjectCaseSensitive(row,"qualified_name"));cJSON_AddItemToObject(row,"qualified_name",names);}));
    // Version 3 fields are rejected rather than ignored or converted.
    const auto first_row=[](cJSON* root,const char* category){return cJSON_GetObjectItemCaseSensitive(root,category)->child;};
    rejected(modify([](cJSON* root){cJSON_AddItemToObject(root,"zones",cJSON_CreateArray());}));
    rejected(modify([](cJSON* root){cJSON_AddStringToObject(root,"primary_zone_id","0000000000000000000000000000000a");}));
    rejected(modify([&](cJSON* root){cJSON_AddStringToObject(first_row(root,"activities"),"zone_id","0000000000000000000000000000000a");}));
    rejected(modify([&](cJSON* root){cJSON_AddStringToObject(first_row(root,"octopuses"),"label","Amity 1");}));
    for(const auto* category:{"components","activities"}){
        rejected(modify([&](cJSON* root){cJSON_AddStringToObject(first_row(root,category),"generated_label","Generated");}));
        rejected(modify([&](cJSON* root){cJSON_AddBoolToObject(first_row(root,category),"label_override",false);}));
    }
    rejected(modify([&](cJSON* root){auto* row=first_row(root,"components");auto* names=cJSON_CreateArray();
        cJSON_AddItemToArray(names,cJSON_DetachItemFromObjectCaseSensitive(row,"qualified_name"));cJSON_AddItemToObject(row,"qualified_names",names);}));
    assert(parse_configuration(encoded,decoded,error)&&serialize_configuration(decoded)==encoded);
    // Offline selections are current records, not imports or observations.
    assert(decoded.activities[1].switch_input==6&&decoded.activities[1].audio==decoded.activities[1].switch_device);
    assert(decoded.activities[1].source&&decoded.activities[1].display);
    auto absent=decoded;const auto member=id<model::OctopusId>(70);absent.octopuses.push_back({member,1});
    for(auto& component:absent.components)if(component.record.component_id==absent.activities[1].audio)component.record.qualified_name->port.octopus_id=member;
    assert(validate_configuration(absent,error));
}
void activity_capacity(){
    // The editor's sixteen activities are the saved limit. Activities need no
    // external slot or order entry, so only the count rejects the seventeenth.
    auto c=direct_configuration();std::string error;
    while(c.activities.size()<max_configured_activities-1){auto activity=c.activities.front();activity.label="Activity "+std::to_string(c.activities.size());fixture_activity(c,activity);}
    const auto unlisted=[](Configuration& value){auto activity=value.activities.front();activity.label="Unlisted "+std::to_string(value.activities.size());
        activity.activity_id=id<model::ActivityId>(900+static_cast<unsigned>(value.activities.size()));value.activities.push_back(activity);};
    unlisted(c);Configuration decoded;
    assert(c.activities.size()==max_configured_activities&&c.activity_order.size()<max_configured_activities&&c.external_slots.size()<max_configured_activities);
    assert(validate_configuration(c,error)&&parse_configuration(serialize_configuration(c),decoded,error));
    auto over=c;unlisted(over);
    assert(over.activity_order==c.activity_order&&over.external_slots.size()==c.external_slots.size());
    assert(!validate_configuration(over,error)&&!parse_configuration(serialize_configuration(over),decoded,error));
}
void initialization(){
    Configuration initialized;std::string error;const auto group=id<model::ConsortiumId>(1);const auto owner=id<model::OctopusId>(2);
    assert(initialize_configuration(group,owner,4,initialized,error));
    assert(initialized.controller_octopus_id==owner&&initialized.consortium_id==group&&initialized.octopuses.size()==1&&initialized.octopuses[0].octopus_id==owner&&initialized.octopuses[0].supported_tentacles==4);
    assert(initialized.activities.empty()&&initialized.components.empty()&&initialized.external_slots.empty()&&initialized.activity_order.empty());
    assert(validate_configuration(initialized,error));const auto before=serialize_configuration(initialized);
    for(uint8_t capacity:{uint8_t(0),uint8_t(amity::max_tentacles+1)})assert(!initialize_configuration(group,owner,capacity,initialized,error));
    assert(!initialize_configuration({},owner,4,initialized,error));assert(!initialize_configuration(group,{},4,initialized,error));
    assert(serialize_configuration(initialized)==before);
}
void journal(){
    auto c=direct_configuration();std::string error;Memory baseline;ConfigurationStore original(baseline);ConfigurationSnapshot active;commit(original,c,{},active);
    auto labels=active->configuration;labels.activities[0].label="Cinema";assert(original.stage(labels,active->configuration.revisions(),error)==ConfigResult::ok&&original.staged()->configuration.revisions()==RevisionPair({1,2}));assert(original.commit(active,error)==ConfigResult::ok);
    auto semantic=active->configuration;semantic.activities[0].switch_input=4;
    const auto before=baseline.values;
    for(unsigned cut=1;cut<=80;++cut){Memory memory;memory.values=before;ConfigurationStore transaction(memory);ConfigurationSnapshot old;assert(transaction.recover(old,error)==ConfigResult::ok);memory.operations=0;memory.cut_at=cut;try{if(transaction.stage(semantic,old->configuration.revisions(),error)==ConfigResult::ok){transaction.commit(old,error);}}catch(const PowerCut&){}memory.cut_at=0;ConfigurationStore reboot(memory);ConfigurationSnapshot recovered;assert(reboot.recover(recovered,error)==ConfigResult::ok);assert(recovered->configuration.activities[0].switch_input==0||recovered->configuration.activities[0].switch_input==4);assert(recovered->configuration.activities[0].activity_id==c.activities[0].activity_id);}
    // Only the current journal record header is readable. A matching digest
    // cannot authorize another saved format or cause committed data to be erased.
    for(const auto* header:{"AMITY3", "AMITY5"}){
        Memory memory;memory.values=before;const auto marker=memory.values.count("cm1")?memory.values.at("cm1"):memory.values.at("cm0");
        const auto slot=marker.substr(4,1);const auto record_key=slot=="1"?"c1":"c0";const auto mark_key=slot=="1"?"cm1":"cm0";
        auto& record=memory.values.at(record_key);record.replace(0,6,header);memory.values[mark_key]=marker.substr(0,6)+sha256_hex(record);
        const auto saved=memory.values;ConfigurationStore rejected(memory);assert(rejected.recover(active,error)==ConfigResult::recovery_required&&memory.values==saved);
    }
    baseline.values["unrelated"]="application data";ConfigurationStore cleanup(baseline);assert(cleanup.recover(active,error)==ConfigResult::ok&&baseline.values.at("unrelated")=="application data");
    // Saving offline member locations does not require that device to approve
    // activity policy. Only the original controller owns this configuration.
    auto peer=active->configuration;const auto owner=id<model::OctopusId>(70);peer.octopuses.push_back({owner,1});peer.components[0].record.qualified_name->port.octopus_id=owner;assert(cleanup.stage(peer,active->configuration.revisions(),error)==ConfigResult::ok);
}
void ownership_and_slots(){
    auto c=direct_configuration();std::string error;bool changed=false;assert(validate_configuration(c,error));
    auto unbound=c;unbound.controller_octopus_id.reset();assert(!validate_configuration(unbound,error));
    Memory memory;ConfigurationStore store(memory);ConfigurationSnapshot active;commit(store,c,{},active);
    auto foreign=active->configuration;const auto other_owner=id<model::OctopusId>(80);foreign.octopuses.push_back({other_owner,1});foreign.controller_octopus_id=other_owner;assert(validate_configuration(foreign,error)&&store.stage(foreign,active->configuration.revisions(),error)==ConfigResult::invalid);
    auto candidate=c;auto other=c.activities[0];other.activity_id=id<model::ActivityId>(71);other.label="Other";candidate.activities.push_back(other);candidate.activity_order.push_back(other.activity_id);assert(reconcile_external_slots(c,candidate,error)&&candidate.external_slots.size()==2);
    auto deleted=candidate;deleted.activities.erase(deleted.activities.begin());deleted.activity_order.erase(deleted.activity_order.begin());assert(reconcile_external_slots(candidate,deleted,error)&&deleted.external_slots[0].tombstone&&deleted.external_slots[1].activity_id==other.activity_id);
    auto reused=deleted;reused.external_slots[0].activity_id=other.activity_id;assert(!reconcile_external_slots(deleted,reused,error));
    auto hardware=c;assert(reconcile_hardware_profile(hardware,*c.controller_octopus_id,4,changed,error)&&changed&&hardware.octopuses[0].supported_tentacles==4);assert(serialize_configuration(hardware).find("domains")==std::string::npos);
}
void explicit_owner_removal(){
    auto previous=direct_configuration();std::string error;const auto removed=id<model::OctopusId>(70),other=id<model::OctopusId>(71);
    previous.octopuses.push_back({removed,1});previous.octopuses.push_back({other,1});
    const auto selected=fixture_equipment(previous,"Remote source","source",model::Side::back);
    for(auto& component:previous.components)if(component.record.component_id==selected)component.record.qualified_name->port.octopus_id=removed;
    std::vector<model::ActivityId> doomed;
    for(unsigned role=0;role<4;++role){auto activity=previous.activities.front();activity.activity_id={};activity.label="Dependent "+std::to_string(role);
        if(role==0)activity.source=selected;else if(role==1)activity.display=selected;else if(role==2)activity.audio=selected;else{activity.switch_device=selected;activity.switch_input=7;}
        fixture_activity(previous,activity);doomed.push_back(previous.activities.back().activity_id);
        previous.external_slots.back().alias="Removed Amity shortcut";previous.external_slots.back().hidden=true;
    }
    const auto unused=fixture_equipment(previous,"Unused remote","source",model::Side::back);
    for(auto& component:previous.components)if(component.record.component_id==unused)component.record.qualified_name->port.octopus_id=removed;
    previous.external_slots.front().alias="Keep this shortcut";previous.external_slots.front().hidden=true;
    const auto original=serialize_configuration(previous);Configuration candidate;std::vector<model::ActivityId> affected;
    assert(!propose_octopus_removal(previous,*previous.controller_octopus_id,candidate,affected,error));
    assert(propose_octopus_removal(previous,removed,candidate,affected,error)&&affected==doomed&&serialize_configuration(previous)==original);
    assert(candidate.activities.size()==1&&candidate.activities.front().activity_id==previous.activities.front().activity_id);
    assert(candidate.activity_order==std::vector<model::ActivityId>{previous.activities.front().activity_id});
    assert(serialize_configuration(candidate).find(removed.str())==std::string::npos);
    assert(candidate.octopuses.size()==2&&candidate.octopuses.back().octopus_id==other);
    assert(candidate.components.size()==2&&candidate.external_slots.front().alias=="Keep this shortcut"&&candidate.external_slots.front().hidden);
    // Reserved integration slots retain no device/equipment metadata and cannot
    // be reassigned to an unrelated activity when a device is deleted.
    for(size_t index=1;index<candidate.external_slots.size();++index){const auto& slot=candidate.external_slots[index];assert(slot.activity_id==doomed[index-1]&&slot.tombstone&&!slot.hidden&&slot.alias.empty());}
    Memory memory;ConfigurationStore store(memory);ConfigurationSnapshot active;commit(store,previous,{},active);
    assert(store.stage(candidate,active->configuration.revisions(),error)==ConfigResult::ok&&store.commit(active,error)==ConfigResult::ok);
    assert(active->canonical.find(removed.str())==std::string::npos&&active->configuration.activities.size()==1);
    for(size_t index=1;index<active->configuration.external_slots.size();++index)assert(active->configuration.external_slots[index].tombstone&&active->configuration.external_slots[index].alias.empty());
}
// The repository example is a saved configuration in the current format.
void example_configuration(const char* path){
    std::ifstream input(path);assert(input);const std::string text((std::istreambuf_iterator<char>(input)),std::istreambuf_iterator<char>());
    Configuration c;std::string error;assert(parse_configuration(text,c,error));
    auto* example=cJSON_Parse(text.c_str());assert(example);assert(serialize_configuration(c)==json_text(example));cJSON_Delete(example);
}
int main(int argc,char** argv){assert(argc==2);example_configuration(argv[1]);canonical_bytes_and_projections();saved_text_and_staged_record_integrity();codecs();current_format_only();activity_capacity();initialization();journal();ownership_and_slots();explicit_owner_removal();std::cout<<"PASS intent-only configuration: canonical bytes/hash projections, saved-text invariants, staged-record tampering, scoped selectors, current-format rejection, sixteen activities, AVR/switch selections without discovery, explicit all-role owner deletion, preserved slots/authority, 80 storage interruptions, current-format example\n";}
