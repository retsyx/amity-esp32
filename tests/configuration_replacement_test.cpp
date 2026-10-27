#include "configuration_replacement.hpp"
#include <cassert>
#include <iostream>
using namespace config_store;namespace m=amity::system;
template<class T>T id(unsigned n){char text[33];std::snprintf(text,sizeof(text),"%032x",n);return *T::parse(text);}
auto random_source(){return [counter=uint64_t{1000}](uint8_t* bytes,size_t size)mutable{std::fill(bytes,bytes+size,0);++counter;for(size_t n=0;n<std::min<size_t>(size,8);++n)bytes[size-1-n]=uint8_t(counter>>(8*n));return true;};}
int main(){
    ConfigurationV2 initial;initial.consortium_id=id<m::ConsortiumId>(1);initial.primary_zone_id=id<m::ZoneId>(10);
    const auto old=id<m::OctopusId>(2),replacement=id<m::OctopusId>(3);const auto controller=id<m::OctopusId>(4);initial.controller_octopus_id=controller;initial.octopuses={{old,"Living room","",false,1},{replacement,"New board","",false,1},{controller,"Controller","",false,1}};
    initial.zones={{initial.primary_zone_id,"TV","",false,{}}};m::CecPortRef back{initial.consortium_id,old,0,m::Side::back};auto front=back;front.side=m::Side::front;
    std::vector<LegacyEvidence> evidence{{{back,"Source"},true,true,0x1000,true,true,false,1},{{front,"TV"},true,true,0,true,true,false,1}};
    ConfigurationV2 previous;std::vector<std::string> tasks;std::string error;std::function<bool(uint8_t*,size_t)> random=random_source();
    assert(propose_local_activity(initial,{"Watch","TV","Source","TV","",0},evidence,random,previous,tasks,error)&&tasks.empty());
    previous.external_slots[0].alias="Games";previous.external_slots[0].hidden=true;
    ReplacementApproval approval;approval.previous=old;approval.replacement=replacement;approval.previous_revoked=approval.previous_isolated=approval.replacement_enrolled=true;
    auto new_front=front,new_back=back;new_front.octopus_id=new_back.octopus_id=replacement;
    // Explicitly swapped physical connector roles are valid; orientation is an
    // installation mapping, not a guessed synonym for TV/playback identity.
    new_front.side=m::Side::back;new_back.side=m::Side::front;
    approval.ports={{front,new_front,true,0x2000},{back,new_back,true,{}}};
    std::vector<m::CecObservation> observations;
    for(const auto& c:previous.components)for(const auto& name:c.record.qualified_names){auto next=name;next.port=name.port==front?new_front:new_back;approval.equipment.push_back({c.record.component_id,name,next,true});m::CecObservation o;o.endpoint={next.port,uint8_t(next.osd_name=="TV"?0:4)};o.owner_boot_id=id<m::BootId>(30);o.scan_id=id<m::ScanId>(31);o.local_sequence=1;o.device_type=next.osd_name=="TV"?0:4;o.osd_name=next.osd_name;o.kind=m::ObservationKind::native_appliance;o.physical_address=next.osd_name=="TV"?0:0x2000;observations.push_back(o);}
    ConfigurationV2 candidate;std::vector<m::ActivityId> affected;
    assert(propose_octopus_replacement(previous,approval,observations,random,candidate,affected,error));
    assert(affected.size()==1&&candidate.activities[0].activity_id==previous.activities[0].activity_id&&candidate.components[0].record.component_id==previous.components[0].record.component_id);
    assert(candidate.external_slots[0].alias=="Games"&&candidate.external_slots[0].hidden&&candidate.controller_octopus_id==controller);
    for(size_t n=0;n<candidate.domains.size();++n)assert(candidate.domains[n].owner_octopus_id==replacement&&candidate.domains[n].domain_revision==previous.domains[n].domain_revision+1);
    for(const auto& b:candidate.binding_policies)assert(std::none_of(previous.binding_policies.begin(),previous.binding_policies.end(),[&](const auto& prior){return prior.binding_id==b.binding_id;}));
    auto forbidden=previous;forbidden.controller_octopus_id=old;assert(!propose_octopus_replacement(forbidden,approval,observations,random,candidate,affected,error));
    auto denied=approval;denied.previous_isolated=false;assert(!propose_octopus_replacement(previous,denied,observations,random,candidate,affected,error));
    denied=approval;denied.ports[1].replacement=denied.ports[0].replacement;assert(!propose_octopus_replacement(previous,denied,observations,random,candidate,affected,error));
    denied=approval;denied.equipment[0].same_equipment_confirmed=false;assert(!propose_octopus_replacement(previous,denied,observations,random,candidate,affected,error));
    auto duplicate=observations;auto clone=duplicate[0];clone.endpoint.logical_address=8;duplicate.push_back(clone);assert(!propose_octopus_replacement(previous,approval,duplicate,random,candidate,affected,error));
    duplicate=observations;duplicate[0].stale=true;assert(!propose_octopus_replacement(previous,approval,duplicate,random,candidate,affected,error));
    auto four=previous;for(auto& board:four.octopuses)if(board.octopus_id==replacement)board.supported_tentacles=4;
    auto fourth=approval;for(auto& mapping:fourth.ports)mapping.replacement.tentacle_id=3;for(auto& mapping:fourth.equipment)mapping.replacement.port.tentacle_id=3;
    auto fourth_observations=observations;for(auto& observation:fourth_observations)observation.endpoint.port.tentacle_id=3;
    auto fourth_random=[counter=uint64_t{9000}](uint8_t* bytes,size_t size)mutable{std::fill(bytes,bytes+size,0);++counter;for(size_t n=0;n<std::min<size_t>(size,8);++n)bytes[size-1-n]=uint8_t(counter>>(8*n));return true;};
    assert(propose_octopus_replacement(four,fourth,fourth_observations,fourth_random,candidate,affected,error));
    assert(candidate.activities.front().activity_id==previous.activities.front().activity_id&&candidate.external_slots.front().activity_id==previous.external_slots.front().activity_id);
    for(const auto& component:candidate.components)for(const auto& name:component.record.qualified_names)assert(name.port.tentacle_id==3);
    std::cout<<"PASS explicit replacement isolation, scoped equipment, fresh bindings, swapped/fourth connectors and immutable original controller\n";
}
