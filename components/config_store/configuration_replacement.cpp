#include "configuration_replacement.hpp"
#include <set>
namespace config_store {
namespace {
std::string key(const model::CecPortRef& p){return p.consortium_id.str()+":"+p.octopus_id.str()+":"+std::to_string(p.tentacle_id)+":"+model::side_name(p.side);}
}
bool propose_octopus_replacement(const Configuration& previous,const ReplacementApproval& approval,
    Configuration& candidate,std::vector<model::ActivityId>& affected,std::string& error){
    const auto fail=[&](const char* reason){error=reason;return false;};affected.clear();
    if(!validate_configuration(previous,error))return false;
    if(!previous.controller_octopus_id||approval.previous==*previous.controller_octopus_id)return fail("The original controller cannot be moved to replacement hardware");
    if(!approval.previous||!approval.replacement||approval.previous==approval.replacement||!approval.previous_revoked||!approval.previous_isolated||!approval.replacement_enrolled)return fail("Revoke and isolate the old hardware, then enroll the replacement using its own credentials");
    const auto target=std::find_if(previous.octopuses.begin(),previous.octopuses.end(),[&](const auto& b){return b.octopus_id==approval.replacement;});
    if(target==previous.octopuses.end())return fail("The replacement Amity is not enrolled");
    std::set<std::string> old_ports,new_ports;
    for(const auto& p:approval.ports)if(!p.previous.valid()||!p.replacement.valid()||p.previous.consortium_id!=previous.consortium_id||p.replacement.consortium_id!=previous.consortium_id||p.previous.octopus_id!=approval.previous||p.replacement.octopus_id!=approval.replacement||p.replacement.tentacle_id>=target->supported_tentacles||!old_ports.insert(key(p.previous)).second||!new_ports.insert(key(p.replacement)).second)return fail("Use distinct valid replacement HDMI connection mappings");
    std::set<std::string> selections;std::set<model::ComponentId> changed;
    auto out=previous;
    for(auto& c:out.components)for(auto& selector:c.record.qualified_names)if(selector.port.octopus_id==approval.previous){
        const auto selected=std::find_if(approval.equipment.begin(),approval.equipment.end(),[&](const auto& e){return e.component==c.record.component_id&&e.previous==selector;});
        if(selected==approval.equipment.end()||!selected->same_equipment_confirmed||!selected->replacement.valid()||selected->replacement.port.consortium_id!=previous.consortium_id||selected->replacement.port.octopus_id!=approval.replacement||selected->replacement.port.tentacle_id>=target->supported_tentacles)return fail("Map each saved equipment selector to its intended replacement connection");
        const auto mapped=std::find_if(approval.ports.begin(),approval.ports.end(),[&](const auto& p){return p.previous==selector.port;});
        if(mapped!=approval.ports.end()&&mapped->replacement!=selected->replacement.port)return fail("Equipment and HDMI adapter mappings disagree");
        if(!selections.insert(model::name_json(selector)).second)return fail("Equipment mappings repeat an old selector");
        selector=selected->replacement;changed.insert(c.record.component_id);
    }
    if(changed.empty())return fail("The old Amity has no saved equipment to replace");
    for(const auto& e:approval.equipment)if(!selections.count(model::name_json(e.previous)))return fail("An equipment mapping does not refer to a saved old selector");
    for(const auto& a:out.activities)if((a.source&&changed.count(*a.source))||(a.display&&changed.count(*a.display))||(a.audio&&changed.count(*a.audio))||(a.switch_device&&changed.count(*a.switch_device)))affected.push_back(a.activity_id);
    if(!validate_configuration(out,error))return false;
    candidate=std::move(out);error.clear();return true;
}
bool restore_main_configuration(const Configuration& backup,const Configuration& current,
    bool isolated,Configuration& out,std::vector<std::string>& tasks,std::string& error){
    const auto fail=[&](const char* reason){error=reason;return false;};tasks.clear();
    if(!validate_configuration(backup,error)||!validate_configuration(current,error))return false;
    if(!isolated)return fail("Confirm the previous main Amity is switched off and disconnected before restoring");
    if(!backup.controller_octopus_id||!current.controller_octopus_id)return fail("Restore requires a backup identifying its original main Amity and a newly set up main Amity");
    const auto old=*backup.controller_octopus_id,replacement=*current.controller_octopus_id;
    if(old==replacement&&backup.consortium_id==current.consortium_id){out=backup;out.config_revision=current.config_revision;out.presentation_revision=current.presentation_revision;return true;}
    if(!current.components.empty()||!current.activities.empty()||!current.external_slots.empty()||current.octopuses.size()!=1)return fail("Restore another main Amity's backup onto a freshly set up empty main Amity");
    const auto hardware=std::find_if(current.octopuses.begin(),current.octopuses.end(),[&](const auto& b){return b.octopus_id==replacement;});
    if(hardware==current.octopuses.end())return fail("The replacement main Amity hardware profile is missing");
    if(std::any_of(backup.octopuses.begin(),backup.octopuses.end(),[&](const auto& b){return b.octopus_id==replacement;}))return fail("The replacement identity already names a different device in this backup");
    auto candidate=backup;candidate.consortium_id=current.consortium_id;candidate.controller_octopus_id=replacement;candidate.config_revision=current.config_revision;candidate.presentation_revision=current.presentation_revision;
    for(auto& b:candidate.octopuses)if(b.octopus_id==old){b.octopus_id=replacement;b.supported_tentacles=hardware->supported_tentacles;}
    for(auto& c:candidate.components)for(auto& selector:c.record.qualified_names){selector.port.consortium_id=current.consortium_id;if(selector.port.octopus_id==old){if(selector.port.tentacle_id>=hardware->supported_tentacles)return fail("The replacement main does not support an HDMI adapter used by this backup");selector.port.octopus_id=replacement;}}
    // Portable choices are restored directly. Availability is determined by the
    // owner-local name lookup when used; no executable approval is imported.
    if(!validate_configuration(candidate,error))return false;
    if(serialize_configuration(candidate).size()>configuration_payload_limit)return fail("The backup exceeds saved configuration capacity");
    tasks.push_back("Reset and add the other Amity devices again; saved equipment locations do not enroll or authorize them.");
    tasks.push_back("Pair your remote and reconnect your home apps to the new main Amity.");
    out=std::move(candidate);error.clear();return true;
}
}
