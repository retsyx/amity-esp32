#include "intent_configuration_fixture.hpp"
#include "../components/web_admin/activity_editor_model.hpp"
#include "../components/web_admin/include/json_input.hpp"
#include <iostream>
namespace ae=web_admin::activity_editor;
ae::Row row(const ConfiguredActivity& a){return {a.activity_id,a.label,ae::component_value(a.display),ae::component_value(a.source),ae::component_value(a.audio),ae::component_value(a.switch_device),a.switch_input};}
int main(){
    auto c=direct_configuration();ConfigurationV2 avr;std::vector<std::string> unresolved;std::string error;assert(propose_local_activity(c,{"Play Wii","TV","Wii","AVR","AVR",6},{},random_source(),avr,unresolved,error));
    // No equipment observations or path approval are required to retain or save
    // local and remote selections, including AVR volume and switching.
    const auto choices=ae::choices(avr,{},{{*avr.controller_octopus_id,"amity-main"}});ConfigurationV2 edited;
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
    const auto offline=ae::choices(remote,{},names,true);const auto receiver=std::find_if(offline.begin(),offline.end(),[&](const auto& choice){return choice.component_id==remote.activities[1].audio;});assert(receiver!=offline.end()&&receiver->location=="amity-den · Adapter 4"&&!receiver->observation);assert(ae::edit(remote,rows,offline,random_source(),edited,unresolved,error)&&unresolved.empty());assert(serialize_configuration_v2(edited).find("domains")==std::string::npos);
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
    // API revision/identity/type checks remain strict even though availability
    // and topology approvals were removed.
    const std::string body="{\"version\":1,\"request_id\":\""+id<model::MessageId>(90).str()+"\",\"expected_config_revision\":\"1\",\"expected_presentation_revision\":\"1\",\"activities\":[{\"activity_id\":\""+first.str()+"\",\"name\":\"Watch\",\"display\":\""+rows[0].display+"\",\"source\":\""+rows[0].source+"\",\"audio\":\""+rows[0].audio+"\"}]}";
    ae::Submission submission;assert(ae::parse_submission(body,avr,submission,error));auto bad=body;bad.insert(bad.size()-1,",\"enabled\":true");assert(!ae::parse_submission(bad,avr,submission,error));bad=body;auto at=bad.find("\"expected_config_revision\":\"1\"");bad.replace(at,30,"\"expected_config_revision\":1");assert(!ae::parse_submission(bad,avr,submission,error));
    std::cout<<"PASS activity editor: AVR/switch paths, offline scoped selections, cross-owner targets, stable IDs/order/slots, no persistent discovery approval\n";
}
