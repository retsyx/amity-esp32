#include "configuration_replacement.hpp"
#include <map>
#include <set>
namespace config_store {
namespace {
std::string key(const model::CecPortRef& p){return p.consortium_id.str()+":"+p.octopus_id.str()+":"+std::to_string(p.tentacle_id)+":"+model::side_name(p.side);}
const ReplacementPort* port(const ReplacementApproval& a,const model::CecPortRef& old){for(const auto& p:a.ports)if(p.previous==old)return &p;return nullptr;}
const ReplacementEquipment* equipment(const ReplacementApproval& a,model::ComponentId id,const model::QualifiedDeviceName& old){for(const auto& e:a.equipment)if(e.component==id&&e.previous==old)return &e;return nullptr;}
const model::CecObservation* unique(const std::vector<model::CecObservation>& observations,const model::QualifiedDeviceName& name){
    const model::CecObservation* found=nullptr;
    for(const auto& o:observations)if(!o.stale&&o.kind==model::ObservationKind::native_appliance&&o.owner_boot_id&&o.scan_id&&o.local_sequence&&o.osd_name&&o.endpoint.port==name.port&&*o.osd_name==name.osd_name){
        if(found&&found->endpoint!=o.endpoint)return nullptr;
        found=&o;
    }
    if(found)for(const auto& alias:observations)if(!alias.stale&&alias.kind==model::ObservationKind::native_appliance&&alias.endpoint==found->endpoint&&alias.osd_name&&*alias.osd_name!=name.osd_name)return nullptr;
    return found;
}
}
bool propose_octopus_replacement(const ConfigurationV2& previous,const ReplacementApproval& approval,
    const std::vector<model::CecObservation>& observations,const std::function<bool(uint8_t*,size_t)>& random,
    ConfigurationV2& candidate,std::vector<model::ActivityId>& affected,std::string& error){
    const auto fail=[&](const char* message){error=message;return false;};affected.clear();
    if(!validate_configuration_v2(previous,error))return false;
    if(!previous.controller_octopus_id)return fail("Establish the original controller before replacing an added device");
    if(approval.previous==*previous.controller_octopus_id)return fail("The original controller and its remote cannot be moved to replacement hardware");
    if(!approval.previous||!approval.replacement||approval.previous==approval.replacement||!approval.previous_revoked||!approval.previous_isolated||!approval.replacement_enrolled)
        return fail("Revoke and physically isolate or positively retire the old hardware, then enroll the replacement with its own credentials");
    const auto target=std::find_if(previous.octopuses.begin(),previous.octopuses.end(),[&](const auto& o){return o.octopus_id==approval.replacement;});
    if(target==previous.octopuses.end()||!target->supported_tentacles||target->supported_tentacles>amity::max_tentacles)return fail("Replacement requires a known Amity device hardware profile");
    std::set<std::string> sources,targets;
    for(const auto& p:approval.ports){
        const auto from=key(p.previous),to=key(p.replacement);
        if(!p.previous.valid()||!p.replacement.valid()||p.previous.octopus_id!=approval.previous||p.replacement.octopus_id!=approval.replacement||p.previous.consortium_id!=previous.consortium_id||p.replacement.consortium_id!=previous.consortium_id||p.replacement.tentacle_id>=target->supported_tentacles||!p.electrical_isolation_confirmed||!sources.insert(from).second||!targets.insert(to).second)
            return fail("Confirm each distinct replacement connector and its electrical domain; connector mappings cannot merge");
        if(p.confirmed_attachment_pa&&!model::valid_physical_address(*p.confirmed_attachment_pa,false))return fail("Replacement attachment physical address is invalid");
    }
    std::set<std::string> selectors;
    for(const auto& selected:approval.equipment){
        const auto component=std::find_if(previous.components.begin(),previous.components.end(),[&](const auto& c){return c.record.component_id==selected.component;});
        if(component==previous.components.end()||selected.previous.port.octopus_id!=approval.previous||std::find(component->record.qualified_names.begin(),component->record.qualified_names.end(),selected.previous)==component->record.qualified_names.end()||!selectors.insert(selected.component.str()+":"+model::name_json(selected.previous)).second)
            return fail("Equipment mappings must name each actual old selector exactly once");
    }
    for(const auto& attachment:approval.ports)if(std::none_of(previous.domains.begin(),previous.domains.end(),[&](const auto& domain){return domain.owner_octopus_id==approval.previous&&std::any_of(domain.ports.begin(),domain.ports.end(),[&](const auto& p){return p.port==attachment.previous;});}))
        return fail("Connector mapping is not an attachment of the old owner");
    std::set<model::ComponentId> changed_components;std::set<model::BindingId> old_bindings;
    candidate=previous;
    for(auto& c:candidate.components)for(auto& name:c.record.qualified_names)if(name.port.octopus_id==approval.previous){
        const auto* mapped=equipment(approval,c.record.component_id,name);const auto* attachment=port(approval,name.port);
        if(!mapped||!attachment||!mapped->same_equipment_confirmed||mapped->replacement.port!=attachment->replacement||!mapped->replacement.valid()||!unique(observations,mapped->replacement))
            return fail("Confirm the same logical equipment against one fresh native OSD report on its mapped connector");
        const auto* observed=unique(observations,mapped->replacement);
        if((c.kind=="display"&&observed->device_type!=0)||(c.kind=="source"&&observed->device_type!=1&&observed->device_type!=3&&observed->device_type!=4))
            return fail("Reported replacement device type does not match the confirmed equipment role");
        name=mapped->replacement;changed_components.insert(c.record.component_id);
    }
    // Newly enrolled boards may have monitor-only quarantine attachments. Never
    // overwrite a role or an existing source merely because its name matches.
    for(auto& d:candidate.domains)if(d.owner_octopus_id!=approval.previous)
        d.ports.erase(std::remove_if(d.ports.begin(),d.ports.end(),[&](const auto& p){
            if(!targets.count(key(p.port)))return false;
            return p.role==model::PortRole::disabled||p.role==model::PortRole::monitor;
        }),d.ports.end());
    candidate.domains.erase(std::remove_if(candidate.domains.begin(),candidate.domains.end(),[](const auto& d){return d.ports.empty();}),candidate.domains.end());
    for(auto& d:candidate.domains)if(d.owner_octopus_id==approval.previous){
        if(d.domain_revision==UINT64_MAX)return fail("Electrical-domain revision exhausted");
        for(auto& p:d.ports){
            const auto* mapped=port(approval,p.port);if(!mapped)return fail("Every attachment in the old domain needs an explicit replacement mapping");
            p.port=mapped->replacement;p.physical_address.reset();
            if(p.role==model::PortRole::playback_proxy){
                if(!mapped->confirmed_attachment_pa)return fail("Confirm the replacement playback connector's HDMI physical address");
                p.physical_address=model::PhysicalAddressEvidence{p.port,d.domain_id,d.domain_revision+1,*mapped->confirmed_attachment_pa,model::PaProvenance::user_confirmed_fallback,{},true};
            }
        }
        d.owner_octopus_id=approval.replacement;++d.domain_revision;d.confirmed=true;
    }
    std::map<model::BindingId,model::BindingId> bindings;
    for(auto& b:candidate.binding_policies)if(b.port.octopus_id==approval.previous){
        const auto* mapped=port(approval,b.port);if(!mapped)return fail("Binding has no approved replacement connector");
        if(b.device_name){const auto* selected=equipment(approval,b.component_id,*b.device_name);if(!selected)return fail("Binding has no confirmed native equipment mapping");b.device_name=selected->replacement;}
        const auto fresh=model::BindingId::generate(random);if(!fresh||std::any_of(previous.binding_policies.begin(),previous.binding_policies.end(),[&](const auto& prior){return prior.binding_id==*fresh;}))return fail("Fresh binding identifier allocation failed or reused a retired ID");
        old_bindings.insert(b.binding_id);bindings.emplace(b.binding_id,*fresh);b.binding_id=*fresh;b.port=mapped->replacement;
    }
    for(auto& resource:candidate.resources)if(resource.owner_octopus_id==approval.previous)resource.owner_octopus_id=approval.replacement;
    for(auto& route:candidate.routes){
        for(auto& binding:route.required_bindings)if(bindings.count(binding))binding=bindings.at(binding);
        for(auto& step:route.steps)if(bindings.count(step.binding_id)){
            step.binding_id=bindings.at(step.binding_id);
            if(step.operation==model::OperationKind::select_path){
                const auto selected=std::find_if(candidate.binding_policies.begin(),candidate.binding_policies.end(),[&](const auto& b){return b.binding_id==step.binding_id;});
                const auto* observation=selected!=candidate.binding_policies.end()&&selected->device_name?unique(observations,*selected->device_name):nullptr;
                if(!observation||!model::valid_physical_address(observation->physical_address,false))return fail("Replacement routing requires a fresh native physical path");
                step.arguments.physical_path=observation->physical_address;
            }
        }
        for(auto& demand:route.demands)if(demand.control_owner==approval.previous)demand.control_owner=approval.replacement;
    }
    for(const auto& activity:candidate.activities)if((activity.source&&changed_components.count(*activity.source))||(activity.display&&changed_components.count(*activity.display))||(activity.audio&&changed_components.count(*activity.audio))||(activity.switch_device&&changed_components.count(*activity.switch_device)))affected.push_back(activity.activity_id);
    if(approval.ports.empty()||old_bindings.empty())return fail("Old hardware has no mapped executable attachments");
    if(!validate_configuration_v2(candidate,error))return false;
    error.clear();return true;
}
bool restore_main_configuration(const ConfigurationV2& backup,const ConfigurationV2& current,
    bool isolated,ConfigurationV2& out,std::vector<std::string>& tasks,std::string& error){
    const auto fail=[&](const char* message){error=message;return false;};
    tasks.clear();
    if(!validate_configuration_v2(backup,error)||!validate_configuration_v2(current,error))return false;
    if(!isolated)return fail("Confirm that the previous main Amity is switched off and disconnected before restoring");
    if(!backup.controller_octopus_id||!current.controller_octopus_id)return fail("Restore requires a backup identifying its original controller and a newly set up main Amity");
    const auto old=*backup.controller_octopus_id,replacement=*current.controller_octopus_id;
    if(old==replacement&&backup.consortium_id==current.consortium_id){out=backup;out.config_revision=current.config_revision;out.presentation_revision=current.presentation_revision;return true;}
    // Public metadata about old members is not an enrolled roster. It remains
    // unavailable until those physical devices are reset and deliberately added.
    if(!current.components.empty()||!current.activities.empty()||!current.binding_policies.empty()||!current.resources.empty()||!current.routes.empty()||!current.signal_edges.empty()||!current.external_slots.empty()||!current.quirks.empty()||current.octopuses.size()!=1)
        return fail("A backup from another main Amity can be restored only onto a freshly set up, empty main Amity");
    const auto actual=std::find_if(current.octopuses.begin(),current.octopuses.end(),[&](const auto& b){return b.octopus_id==replacement;});
    if(actual==current.octopuses.end())return fail("The replacement main Amity hardware profile is missing");
    if(old!=replacement&&std::any_of(backup.octopuses.begin(),backup.octopuses.end(),[&](const auto& b){return b.octopus_id==replacement;}))return fail("The replacement identity already names another device in this backup");
    auto candidate=backup;candidate.consortium_id=current.consortium_id;candidate.controller_octopus_id=replacement;
    candidate.config_revision=current.config_revision;candidate.presentation_revision=current.presentation_revision;
    const auto remap=[&](model::CecPortRef& p){p.consortium_id=current.consortium_id;if(p.octopus_id==old){if(p.tentacle_id>=actual->supported_tentacles)return false;p.octopus_id=replacement;}return true;};
    for(auto& board:candidate.octopuses)if(board.octopus_id==old){board.octopus_id=replacement;board.supported_tentacles=actual->supported_tentacles;}
    for(auto& component:candidate.components)for(auto& name:component.record.qualified_names)if(!remap(name.port))return fail("The replacement main Amity does not support an HDMI adapter used by this backup");
    candidate.domains.erase(std::remove_if(candidate.domains.begin(),candidate.domains.end(),[&](const auto& domain){return domain.owner_octopus_id!=old||std::any_of(domain.ports.begin(),domain.ports.end(),[&](const auto& port){return port.port.octopus_id!=old;});}),candidate.domains.end());
    for(auto& domain:candidate.domains){
        if(domain.domain_revision==UINT64_MAX)return fail("The backup's HDMI connection revision is exhausted");
        ++domain.domain_revision;if(domain.owner_octopus_id==old)domain.owner_octopus_id=replacement;
        for(auto& port:domain.ports){if(!remap(port.port))return fail("The replacement main Amity does not support an HDMI adapter used by this backup");port.physical_address.reset();}
    }
    // Restoring the old path or commands would import stale physical evidence.
    // Retain logical fields and selectors so Scan + Edit/Save can repair them.
    candidate.binding_policies.clear();candidate.resources.clear();candidate.routes.clear();
    candidate.legacy_remote_configuration.clear();
    for(auto& activity:candidate.activities)activity.enabled=false;
    for(auto& edge:candidate.signal_edges)edge.support=model::Support::unknown;
    if(!validate_configuration_v2(candidate,error))return false;
    if(serialize_configuration_v2(candidate).size()>configuration_payload_limit)return fail("The portable backup exceeds this device's configuration storage capacity");
    tasks.push_back("Activities were restored disabled. Scan HDMI devices, review their selections, then Save before starting playback.");
    tasks.push_back("Reset and add the other Amity devices again. Their old saved locations do not enroll or authorize them.");
    tasks.push_back("Pair your remote and reconnect your home apps to this new main Amity.");
    out=std::move(candidate);error.clear();return true;
}
}
