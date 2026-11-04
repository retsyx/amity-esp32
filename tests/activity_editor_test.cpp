#include "intent_configuration_fixture.hpp"
#include "../components/web_admin/activity_editor_model.hpp"
#include "../components/web_admin/include/json_input.hpp"
#include <iostream>
namespace ae=web_admin::activity_editor;
ae::Row row(const ConfiguredActivity& a){return {a.activity_id,a.label,ae::component_value(a.display),ae::component_value(a.source),ae::component_value(a.audio),ae::component_value(a.switch_device),a.switch_input};}
void test_current_activity_retention(){
    auto previous=switch_configuration(direct_configuration());
    previous.activities[1].switch_device=fixture_equipment(previous,"Matrix","switch",model::Side::back);
    const auto current=previous.activities[1].activity_id;
    assert(retains_activity(previous,previous,current));
    const auto rejects=[&](auto change){
        auto candidate=previous;change(candidate);
        assert(!retains_activity(previous,candidate,current));
    };
    rejects([](auto& c){c.activities.erase(c.activities.begin()+1);});
    rejects([](auto& c){c.activities[1].activity_id=id<model::ActivityId>(900);});
    rejects([](auto& c){c.activities[1].zone_id=id<model::ZoneId>(900);});
    rejects([](auto& c){c.activities[1].label="Renamed current activity";});
    rejects([](auto& c){c.activities[1].generated_label="Different generated name";});
    rejects([](auto& c){c.activities[1].label_override=!c.activities[1].label_override;});
    rejects([](auto& c){++c.activities[1].switch_input;});
    for(auto role:{&ConfiguredActivity::source,&ConfiguredActivity::display,
                  &ConfiguredActivity::audio,&ConfiguredActivity::switch_device}){
        rejects([role](auto& c){(c.activities[1].*role).reset();});
    }
    // Shared equipment is part of the running activity even when a change
    // originates through another activity that selects the same display.
    assert(previous.activities[0].display==previous.activities[1].display);
    for(auto selected:{previous.activities[1].source,previous.activities[1].display,
                      previous.activities[1].audio,previous.activities[1].switch_device}){
        rejects([selected](auto& c){
            auto component=std::find_if(c.components.begin(),c.components.end(),
                [&](const auto& value){return value.record.component_id==selected;});
            component->record.qualified_names[0].osd_name="Moved source";
        });
    }
    rejects([](auto& c){
        const auto source=c.activities[1].source;
        auto component=std::find_if(c.components.begin(),c.components.end(),
            [&](const auto& value){return value.record.component_id==source;});
        component->record.qualified_names[0].port.side=model::Side::front;
    });
    rejects([](auto& c){
        const auto source=c.activities[1].source;
        c.components.erase(std::remove_if(c.components.begin(),c.components.end(),
            [&](const auto& value){return value.record.component_id==source;}),c.components.end());
    });

    // Reordering the rows themselves must resolve the current activity by ID.
    // Other activity edits and presentation-only equipment changes are free
    // to commit without changing the running activity's execution choices.
    auto candidate=previous;
    candidate.activities[0].label="Renamed inactive activity";
    candidate.activities[0].source.reset();
    const auto unused=previous.activities[0].source;
    for(auto& component:candidate.components){
        component.record.label="Updated equipment label";
        component.generated_label="Updated generated label";
        component.label_override=true;
        if(component.record.component_id==unused)
            component.record.qualified_names[0].osd_name="Other source";
    }
    ++candidate.config_revision;++candidate.presentation_revision;
    std::reverse(candidate.activities.begin(),candidate.activities.end());
    std::reverse(candidate.activity_order.begin(),candidate.activity_order.end());
    std::reverse(candidate.zones[0].activity_order.begin(),candidate.zones[0].activity_order.end());
    assert(retains_activity(previous,candidate,current));

    // Saving another row through the editor must preserve the current row's
    // name metadata, including a generated name behind a personal override.
    for(bool overridden:{false,true}){
        auto saved=switch_configuration(direct_configuration());
        saved.activities[0].label_override=overridden;
        if(overridden)saved.activities[0].label="Personal Watch";
        const auto before=saved.activities[0];
        std::vector<ae::Row> rows;for(const auto& activity:saved.activities)rows.push_back(row(activity));
        rows[1].name="Renamed other activity";
        std::reverse(rows.begin(),rows.end());
        const auto available=ae::choices(saved,{},{});
        Configuration edited;std::vector<std::string> unresolved;std::string error;
        assert(ae::edit(saved,rows,available,random_source(),edited,unresolved,error));
        assert(unresolved.empty()&&retains_activity(saved,edited,before.activity_id));
        const auto retained=std::find_if(edited.activities.begin(),edited.activities.end(),
            [&](const auto& activity){return activity.activity_id==before.activity_id;});
        assert(retained!=edited.activities.end()&&retained->label==before.label&&
               retained->generated_label==before.generated_label&&retained->label_override==overridden);
        assert(edited.activity_order==std::vector<model::ActivityId>({saved.activities[1].activity_id,before.activity_id}));
        assert(edited.activities[0].label=="Renamed other activity");
    }
}
int main(){
    test_current_activity_retention();
    auto c=direct_configuration();auto avr=switch_configuration(c,"Play Wii");std::vector<std::string> unresolved;std::string error;
    // No equipment observations or path approval are required to retain or save
    // local and remote selections, including AVR volume and switching.
    const auto choices=ae::choices(avr,{},{{*avr.controller_octopus_id,"amity-main"}});Configuration edited;
    std::vector<ae::Row> rows;for(const auto& a:avr.activities)rows.push_back(row(a));assert(ae::edit(avr,rows,choices,random_source(),edited,unresolved,error)&&unresolved.empty());
    assert(edited.activities[1].switch_input==6&&edited.activities[1].switch_device==avr.activities[1].switch_device&&edited.activities[1].audio==avr.activities[1].audio);
    for(const auto& choice:choices){assert(choice.location.empty()&&!choice.observation);}
    // Equipment labels omit side and the lone main's hostname. Enrolled
    // members, including offline ones, make owner names useful on every row.
    const auto grouped=ae::choices(avr,{},{{*avr.controller_octopus_id,"amity-main"}},true);
    for(size_t n=0;n<choices.size();++n){
        assert(grouped[n].location=="amity-main");
        assert(grouped[n].value==choices[n].value&&grouped[n].native_name==choices[n].native_name);
    }
    auto solo=empty_configuration();
    std::vector<model::CecObservation> discovered;
    for(auto side:{model::Side::front,model::Side::back}){
        model::CecObservation o;o.endpoint={{solo.consortium_id,*solo.controller_octopus_id,0,side},4};
        o.osd_name="Player";o.device_type=4;discovered.push_back(o);
    }
    const auto found=ae::choices(solo,discovered,{{*solo.controller_octopus_id,"amity-main"}});
    assert(found.size()==2&&found[0].location.empty()&&found[1].location.empty());
    assert(found[0].value!=found[1].value&&found[0].native_name->port.side!=found[1].native_name->port.side);
    // Capacity is the owner's compiled maximum, independent of populated
    // connectors. A current one-adapter build overrides retained capacity 4.
    solo.octopuses[0].supported_tentacles=4;
    const ae::OwnerHostnames solo_names{{*solo.controller_octopus_id,"amity-main"}};
    const auto multiple=ae::choices(solo,discovered,solo_names);
    const auto recompiled=ae::choices(solo,discovered,solo_names,false,{{*solo.controller_octopus_id,1}});
    const auto named_single=ae::choices(solo,discovered,solo_names,true,{{*solo.controller_octopus_id,1}});
    for(size_t n=0;n<multiple.size();++n){
        assert(multiple[n].location=="Adapter 1"&&recompiled[n].location.empty()&&named_single[n].location=="amity-main");
        assert(multiple[n].value==recompiled[n].value&&multiple[n].native_name==recompiled[n].native_name);
    }
    const auto first=avr.activities[0].activity_id,second=avr.activities[1].activity_id;
    auto ordered=rows;std::reverse(ordered.begin(),ordered.end());ordered[0].name="Renamed Wii";assert(ae::edit(avr,ordered,choices,random_source(),edited,unresolved,error));assert(edited.activity_order==std::vector<model::ActivityId>({second,first}));assert(edited.external_slots[0].activity_id==first&&edited.external_slots[1].activity_id==second);
    assert(ae::edit(avr,{rows[1]},choices,random_source(),edited,unresolved,error)&&edited.activities.size()==1&&edited.external_slots[0].tombstone&&edited.activities[0].activity_id==second);
    auto invalid=rows[0];invalid.source="component:"+id<model::ComponentId>(999).str();assert(!ae::edit(avr,{invalid},choices,random_source(),edited,unresolved,error));assert(!ae::edit(avr,{rows[0],rows[0]},choices,random_source(),edited,unresolved,error));
    // A saved scoped name remains selected when its owner is unreachable.
    auto remote=avr;const auto member=id<model::OctopusId>(70);remote.octopuses.push_back({member,"Old friendly label","",true,4});for(auto& x:remote.components)if(x.record.component_id==remote.activities[1].audio)for(auto& n:x.record.qualified_names){n.port.octopus_id=member;n.port.tentacle_id=3;}
    const ae::OwnerHostnames names{{*remote.controller_octopus_id,"amity-main"},{member,"amity-den"}};
    const auto offline=ae::choices(remote,{},names,true);const auto receiver=std::find_if(offline.begin(),offline.end(),[&](const auto& choice){return choice.component_id==remote.activities[1].audio;});assert(receiver!=offline.end()&&receiver->location=="amity-den · Adapter 4"&&!receiver->observation);assert(ae::edit(remote,rows,offline,random_source(),edited,unresolved,error)&&unresolved.empty());assert(serialize_configuration(edited).find("domains")==std::string::npos);
    // Removed owners stay identified on retained selections, but do not make
    // the remaining main's equipment display a redundant hostname.
    const auto retained=ae::choices(remote,{},names,false);
    for(const auto& choice:retained){
        const bool local=choice.native_name->port.octopus_id==*remote.controller_octopus_id;
        assert(choice.location==(local?"":"amity-den · Adapter 4"));
    }
    // New exact selectors may be saved without requiring a current presence
    // observation. Equal names on different owners retain distinct targets.
    auto empty=empty_configuration();empty.octopuses.push_back({member,"amity-den","amity-den",false,4});std::vector<ae::Choice> exact;
    for(auto owner:{*empty.controller_octopus_id,member}){model::QualifiedDeviceName name{{empty.consortium_id,owner,0,model::Side::back},"Source"};exact.push_back({"native:"+sha256_hex(model::name_json(name)),"Source","",{"source"},{},name,{}});}
    model::QualifiedDeviceName screen{{empty.consortium_id,*empty.controller_octopus_id,0,model::Side::front},"TV"};exact.push_back({"screen","TV","",{"display","audio"},{},screen,{}});
    ae::Row fresh;fresh.name="Remote source";fresh.source=exact[1].value;fresh.display=exact[2].value;fresh.audio=exact[2].value;assert(ae::edit(empty,{fresh},exact,random_source(),edited,unresolved,error)&&unresolved.empty());const auto& selection=edited.activities[0];const auto source=std::find_if(edited.components.begin(),edited.components.end(),[&](const auto& v){return v.record.component_id==selection.source;});assert(source!=edited.components.end()&&source->record.qualified_names[0].port.octopus_id==member);
    // Manual components use only saved ownership, connection capacity and OSD
    // identity. No observations or scan are needed, including for offline peers.
    auto manual=empty_configuration();manual.octopuses.push_back({member,"Old label","Old label",true,4});
    const auto removed=id<model::OctopusId>(71);manual.octopuses.push_back({removed,"Removed","Removed",false,4});
    const ae::OwnerHostnames manual_names{{*manual.controller_octopus_id,"amity-main"},{member,"amity-den"}};
    const auto owners=ae::component_owners(manual,manual_names,{member});
    assert(owners.size()==2&&owners[0].name=="amity-main"&&owners[1].id==member&&owners[1].name=="amity-den"&&owners[1].supported_tentacles==4);
    const auto current_owners=ae::component_owners(manual,manual_names,{member},{{member,1},{*manual.controller_octopus_id,4}});
    assert(current_owners[0].supported_tentacles==4&&current_owners[1].supported_tentacles==1);
    const auto unnamed=ae::component_owners(manual,{}, {member});assert(unnamed[1].name=="Name unavailable");
    const std::string component_body="{\"request_id\":\""+id<model::MessageId>(91).str()+"\",\"expected_config_revision\":\"1\",\"expected_presentation_revision\":\"1\",\"octopus_id\":\""+member.str()+"\",\"tentacle_id\":3,\"side\":\"back\",\"role\":\"audio\",\"osd_name\":\"Receiver\"}";
    const auto parse_component=[&](const std::string& body,ae::ComponentSubmission& value){auto* json=web_admin::parse_json_input(body);const bool ok=ae::parse_component_submission(json,manual,value,error);cJSON_Delete(json);return ok;};
    ae::ComponentSubmission component;assert(parse_component(component_body,component));model::ComponentId component_id;
    assert(ae::add_component(manual,component,owners,random_source(),edited,component_id,error));
    assert(edited.components.size()==1&&edited.components[0].kind=="receiver"&&edited.components[0].record.qualified_names[0]==component.name&&edited.activities.empty());
    Configuration roundtrip;assert(parse_configuration(serialize_configuration(edited),roundtrip,error));
    const auto offline_added=ae::choices(roundtrip,{},manual_names,true);assert(offline_added.size()==1&&offline_added[0].value==ae::component_value(component_id)&&offline_added[0].location=="amity-den · Adapter 4"&&!offline_added[0].observation);
    auto selected_component=component;selected_component.role="switch";Configuration reused;model::ComponentId same_id;
    assert(ae::add_component(edited,selected_component,owners,random_source(),reused,same_id,error)&&same_id==component_id&&reused.components.size()==1);
    selected_component.role="source";assert(!ae::add_component(edited,selected_component,owners,random_source(),reused,same_id,error));
    selected_component=component;selected_component.name.port.side=model::Side::front;assert(ae::add_component(edited,selected_component,owners,random_source(),reused,same_id,error)&&reused.components.size()==2&&same_id!=component_id);
    selected_component=component;selected_component.name.port.octopus_id=*manual.controller_octopus_id;selected_component.name.port.tentacle_id=3;
    assert(ae::add_component(manual,selected_component,current_owners,random_source(),reused,same_id,error)&&reused.octopuses[0].supported_tentacles==4);
    assert(!ae::add_component(manual,component,current_owners,random_source(),reused,same_id,error));
    for(auto invalid_owner:{removed,id<model::OctopusId>(999)}){selected_component=component;selected_component.name.port.octopus_id=invalid_owner;assert(!ae::add_component(manual,selected_component,owners,random_source(),reused,same_id,error));}
    for(const auto& name:std::vector<std::string>{"",std::string(15,'x'),"Bad\nName"}){selected_component=component;selected_component.name.osd_name=name;assert(!ae::add_component(manual,selected_component,owners,random_source(),reused,same_id,error));}
    selected_component=component;selected_component.name.port.side=static_cast<model::Side>(99);assert(!ae::add_component(manual,selected_component,owners,random_source(),reused,same_id,error));
    selected_component=component;selected_component.role="unknown";assert(!ae::add_component(manual,selected_component,owners,random_source(),reused,same_id,error));
    const auto invalid_component=[&](const std::string& from,const std::string& to){auto bad=component_body;const auto offset=bad.find(from);assert(offset!=std::string::npos);bad.replace(offset,from.size(),to);ae::ComponentSubmission invalid;assert(!parse_component(bad,invalid));};
    invalid_component("\"side\":\"back\"","\"side\":\"left\"");invalid_component("\"role\":\"audio\"","\"role\":\"unknown\"");invalid_component("\"tentacle_id\":3","\"tentacle_id\":1.5");invalid_component("\"tentacle_id\":3","\"tentacle_id\":-1");invalid_component("\"tentacle_id\":3","\"tentacle_id\":4");invalid_component("\"osd_name\":\"Receiver\"","\"osd_name\":\"\"");invalid_component("\"expected_config_revision\":\"1\"","\"expected_config_revision\":\"2\"");invalid_component("\"osd_name\":\"Receiver\"","\"osd_name\":\"Receiver\",\"scan\":true");
    // API revision/identity/type checks remain strict even though availability
    // and topology approvals were removed.
    const std::string body="{\"version\":1,\"request_id\":\""+id<model::MessageId>(90).str()+"\",\"expected_config_revision\":\"1\",\"expected_presentation_revision\":\"1\",\"activities\":[{\"activity_id\":\""+first.str()+"\",\"name\":\"Watch\",\"display\":\""+rows[0].display+"\",\"source\":\""+rows[0].source+"\",\"audio\":\""+rows[0].audio+"\"}]}";
    ae::Submission submission;assert(ae::parse_submission(body,avr,submission,error));auto bad=body;bad.insert(bad.size()-1,",\"enabled\":true");assert(!ae::parse_submission(bad,avr,submission,error));bad=body;auto at=bad.find("\"expected_config_revision\":\"1\"");bad.replace(at,30,"\"expected_config_revision\":1");assert(!ae::parse_submission(bad,avr,submission,error));
    std::cout<<"PASS activity editor: AVR/switch paths, manual unobserved components, offline scoped selections, cross-owner targets, stable IDs/order/slots\n";
}
