#include "amity/system_sync.hpp"
#include "amity/consortium/wire.hpp"
#include <set>
namespace amity::synchronization {
namespace s = system;
namespace c = consortium;
namespace cs = config_store;
namespace {
template<class T> bool has(const std::set<T>& ids, const T& id) { return ids.count(id) != 0; }
template<class T, class F> void retain(std::vector<T>& values, F predicate) { values.erase(std::remove_if(values.begin(), values.end(), [&](const T& value) { return !predicate(value); }), values.end()); }
std::string dec(uint64_t n) { return "\"" + s::format_uint64(n) + "\""; }
std::string number(unsigned n) { return std::to_string(n); }
}
cs::ConfigurationV2 owner_slice(const cs::ConfigurationV2& source,s::OctopusId owner) { return cs::configuration_owner_slice(source,owner); }

std::string owner_state_json(const cs::ConfigurationV2& config,s::OctopusId owner,s::BootId boot,uint64_t epoch,const std::vector<s::PortPolicySnapshot>& policies,const std::vector<s::CecObservation>& observations,uint64_t now) {
    if(!owner||!boot||!epoch||policies.size()>amity::max_cec_ports||observations.size()>160)return {};
    std::string out="{\"owner_octopus_id\":\""+owner.str()+"\",\"owner_boot_id\":\""+boot.str()+"\",\"coordinator_epoch\":"+dec(epoch)+",\"config_revision\":"+dec(config.config_revision)+",\"presentation_revision\":"+dec(config.presentation_revision)+",\"policy_hash\":\""+cs::executable_policy_hash(config)+"\",\"ports\":[";
    bool first=true;
    for(const auto& p:policies){
        if(p.port.octopus_id!=owner||p.owner_boot_id!=boot||p.config_revision!=config.config_revision||p.coordinator_epoch!=epoch||p.bindings.size()>128)return {};
        if(!first)out+=',';
        first=false;
        out+="{\"tentacle_id\":"+number(p.port.tentacle_id)+",\"side\":\""+s::side_name(p.port.side)+"\",\"domain_id\":\""+p.domain_id.str()+"\",\"domain_revision\":"+dec(p.domain_revision)+",\"policy_generation\":"+dec(p.policy_generation)+",\"owned_logical_address\":"+number(p.owned_logical_address.value_or(15))+",\"role\":"+number(static_cast<unsigned>(p.role));
        if(p.physical_address){const auto& pa=*p.physical_address;out+=",\"physical_address\":"+number(pa.physical_address)+",\"physical_address_provenance\":"+number(static_cast<unsigned>(pa.provenance))+",\"physical_address_confirmed\":"+(pa.confirmed?"true":"false");if(pa.reporter)out+=",\"reporter\":"+s::address_json(*pa.reporter);}
        out+=",\"bindings\":[";bool first_b=true;
        for(const auto& permission:p.bindings){const auto& b=permission.binding;if(!first_b)out+=',';first_b=false;out+="{\"binding_id\":\""+b.binding_id.str()+"\",\"binding_revision\":"+dec(b.binding_revision)+",\"logical_address\":"+number(b.endpoint.logical_address)+",\"status\":"+number(static_cast<unsigned>(b.status))+",\"physical_address_evidence_ref\":\""+b.physical_address_evidence_ref.str()+"\"}";}
        out+="]}";
    }
    out+="],\"observations\":[";first=true;
    for(const auto& o:observations){
        if(o.kind!=s::ObservationKind::native_appliance||!o.osd_name||o.endpoint.port.octopus_id!=owner||o.owner_boot_id!=boot||!o.scan_id||!o.local_sequence)continue;
        s::QualifiedDeviceName name{o.endpoint.port,*o.osd_name};if(!name.valid()||!o.endpoint.valid())continue;
        const auto age=now>=o.received_at_ms?std::min<uint64_t>(300000,now-o.received_at_ms):300000;
        if(!first)out+=',';
        first=false;out+="{\"device_name\":"+s::name_json(name)+",\"logical_address\":"+number(o.endpoint.logical_address)+",\"observation_sequence\":"+dec(o.local_sequence)+",\"scan_id\":\""+o.scan_id.str()+"\",\"domain_id\":\""+o.domain_id.str()+"\",\"domain_revision\":"+dec(o.domain_revision)+",\"physical_address\":"+number(o.physical_address)+",\"device_type\":"+number(o.device_type)+",\"vendor_id\":"+number(o.vendor_id)+",\"age_ms\":"+std::to_string(age)+",\"stale\":"+((o.stale||age>=30000)?"true":"false")+"}";
    }
    out+="]";
    out+="}";return out.size()<=32768?out:std::string{};
}
bool parse_owner_state(std::string_view bytes,const cs::ConfigurationV2& config,s::OctopusId owner,s::BootId boot,uint64_t epoch,std::vector<s::PortPolicySnapshot>& out,std::string& error,std::vector<s::CecObservation>* observations,uint64_t receiver_now) {
    auto fail=[&](const char* reason){error=reason;return false;};
    c::Json root;c::JsonError detail;uint64_t cfg,pres,received_epoch;s::OctopusId received_owner;s::BootId received_boot;std::string hash;
    if(!c::parse_json(bytes,root,detail,32768,4096)||!c::closed(root,{"owner_octopus_id","owner_boot_id","coordinator_epoch","config_revision","presentation_revision","policy_hash","ports","observations"})||!c::id(root.get("owner_octopus_id"),received_owner)||received_owner!=owner||!c::id(root.get("owner_boot_id"),received_boot)||received_boot!=boot||!c::decimal(root.get("coordinator_epoch"),received_epoch)||received_epoch!=epoch||!c::decimal(root.get("config_revision"),cfg)||cfg!=config.config_revision||!c::decimal(root.get("presentation_revision"),pres)||pres!=config.presentation_revision||!c::json_string(root.get("policy_hash"),hash,64,64)||hash!=cs::executable_policy_hash(config))return fail("Owner snapshot scope/revisions/hash do not match");
    const auto* ports=root.get("ports");if(ports->type!=c::Json::Type::array||ports->array.size()>amity::max_cec_ports)return fail("Invalid owner port capacity");
    std::vector<s::PortPolicySnapshot> parsed;std::set<std::pair<uint8_t,s::Side>> seen;std::set<s::BindingId> seen_bindings;
    for(const auto& value:ports->array){
        uint64_t tentacle,la,role;std::string side;s::PortPolicySnapshot p;
        if(!c::closed(value,{"tentacle_id","side","domain_id","domain_revision","policy_generation","owned_logical_address","role","bindings"},{"physical_address","physical_address_provenance","physical_address_confirmed","reporter"})||!c::json_uint(value.get("tentacle_id"),tentacle,0,amity::last_tentacle_id)||!c::json_string(value.get("side"),side,4,5)||!s::parse_side(side)||!c::id(value.get("domain_id"),p.domain_id)||!c::decimal(value.get("domain_revision"),p.domain_revision)||!p.domain_revision||!c::decimal(value.get("policy_generation"),p.policy_generation)||!p.policy_generation||!c::json_uint(value.get("owned_logical_address"),la,0,15)||!c::json_uint(value.get("role"),role,0,3))return fail("Invalid owner policy record");
        p.port={config.consortium_id,owner,static_cast<uint8_t>(tentacle),*s::parse_side(side)};if(!seen.insert({p.port.tentacle_id,p.port.side}).second)return fail("Duplicate owner port");
        const auto board=std::find_if(config.octopuses.begin(),config.octopuses.end(),[&](const auto& b){return b.octopus_id==owner;});if(board==config.octopuses.end()||tentacle>=board->supported_tentacles)return fail("Owner advertised an unsupported physical connector");
        p.owner_boot_id=boot;p.coordinator_epoch=epoch;p.config_revision=cfg;p.role=static_cast<s::PortRole>(role);if(la<15)p.owned_logical_address=static_cast<uint8_t>(la);
        const cs::ConfiguredPort* configured=nullptr;const cs::ConfiguredDomain* domain=nullptr;
        for(const auto& d:config.domains)for(const auto& port:d.ports)if(port.port==p.port){configured=&port;domain=&d;}
        const bool unavailable=(p.role==s::PortRole::disabled||p.role==s::PortRole::monitor)&&la==15;
        if(configured){if(domain->domain_id!=p.domain_id||domain->domain_revision!=p.domain_revision||(configured->role!=p.role&&!unavailable))return fail("Owner changed a configured electrical role");}
        else if((p.role!=s::PortRole::monitor&&p.role!=s::PortRole::disabled)||la!=15)return fail("Unconfigured port cannot advertise active authority");
        for(const auto& component:config.components)p.components.push_back(component.record);
        if(value.get("physical_address")){
            uint64_t pa,provenance;bool confirmed;if(!c::json_uint(value.get("physical_address"),pa,0,65534)||!s::valid_physical_address(static_cast<uint16_t>(pa))||!c::json_uint(value.get("physical_address_provenance"),provenance,0,3)||!c::json_bool(value.get("physical_address_confirmed"),confirmed))return fail("Invalid attachment PA evidence");
            s::PhysicalAddressEvidence evidence{p.port,p.domain_id,p.domain_revision,static_cast<uint16_t>(pa),static_cast<s::PaProvenance>(provenance),{},confirmed};
            if(value.get("reporter")){s::FullyQualifiedCecAddress reporter;if(!c::parse_address(*value.get("reporter"),reporter))return fail("Invalid attachment reporter");
                bool scoped=reporter.port==p.port;
                if(!scoped&&p.role==s::PortRole::playback_proxy&&p.port.side==s::Side::front&&reporter.port.side==s::Side::back&&reporter.port.consortium_id==p.port.consortium_id&&reporter.port.octopus_id==p.port.octopus_id&&reporter.port.tentacle_id==p.port.tentacle_id)for(const auto& domain:config.domains)for(const auto& port:domain.ports)if(domain.confirmed&&port.port==reporter.port&&port.role==s::PortRole::tv_controller)scoped=true;
                if(!scoped)return fail("Foreign attachment reporter");
                evidence.reporter=reporter;}
            if(evidence.provenance==s::PaProvenance::immediate_peer_report&&!evidence.reporter)return fail("Peer PA lacks actual reporter");
            p.physical_address=evidence;
        }else if(value.get("physical_address_provenance")||value.get("physical_address_confirmed")||value.get("reporter"))return fail("Incomplete PA evidence");
        const auto* bindings=value.get("bindings");if(bindings->type!=c::Json::Type::array||bindings->array.size()>128)return fail("Invalid binding capacity");
        for(const auto& entry:bindings->array){s::ComponentBinding b;uint64_t address,status;
            if(!c::closed(entry,{"binding_id","binding_revision","logical_address","status","physical_address_evidence_ref"})||!c::id(entry.get("binding_id"),b.binding_id)||!seen_bindings.insert(b.binding_id).second||!c::decimal(entry.get("binding_revision"),b.binding_revision)||!b.binding_revision||!c::json_uint(entry.get("logical_address"),address,0,15)||!c::json_uint(entry.get("status"),status,0,4)||!c::id(entry.get("physical_address_evidence_ref"),b.physical_address_evidence_ref))return fail("Invalid binding observation");
            const cs::BindingPolicy* policy=nullptr;for(const auto& candidate:config.binding_policies)if(candidate.binding_id==b.binding_id&&candidate.port==p.port)policy=&candidate;if(!policy)return fail("Unconfigured binding in owner snapshot");
            b.component_id=policy->component_id;b.capability=policy->capability;b.kind=policy->kind;b.device_name=policy->device_name;b.proxy_identity_resource_id=policy->proxy_identity_resource_id;b.endpoint={p.port,static_cast<uint8_t>(address)};b.owner_octopus_id=owner;b.domain_id=p.domain_id;b.domain_revision=p.domain_revision;b.role=p.role;b.status=static_cast<s::BindingStatus>(status);
            if(unavailable&&b.status==s::BindingStatus::verified)return fail("Unavailable HDMI adapter cannot advertise a verified binding");
            const auto component=std::find_if(p.components.begin(),p.components.end(),[&](const auto& item){return item.component_id==b.component_id;});
            if(b.status==s::BindingStatus::verified&&(component==p.components.end()||!s::valid_binding(b,*component)))return fail("Verified binding violates selector or representation contract");
            p.bindings.push_back({b,policy->discrete_power_on,policy->discrete_power_off,policy->image_view_on,policy->exact_path_selection});
        }
        for(const auto& policy:config.binding_policies)if(policy.port==p.port&&!seen_bindings.count(policy.binding_id))return fail("Owner snapshot omitted a configured binding");
        parsed.push_back(std::move(p));
    }
    for(const auto& domain:config.domains)for(const auto& port:domain.ports)if(port.port.octopus_id==owner&&!seen.count({port.port.tentacle_id,port.port.side}))return fail("Owner snapshot omitted a configured port");
    const auto* native=root.get("observations");if(native->type!=c::Json::Type::array||native->array.size()>160)return fail("Invalid native observation capacity");
    std::vector<s::CecObservation> equipment;
    for(const auto& item:native->array){
        s::CecObservation o;s::QualifiedDeviceName name;uint64_t la,pa,type,vendor,age;
        if(!c::closed(item,{"device_name","logical_address","observation_sequence","scan_id","domain_id","domain_revision","physical_address","device_type","vendor_id","age_ms","stale"})||!c::parse_name(*item.get("device_name"),name)||name.port.octopus_id!=owner||name.port.consortium_id!=config.consortium_id||!c::json_uint(item.get("logical_address"),la,0,14)||!c::decimal(item.get("observation_sequence"),o.local_sequence)||!o.local_sequence||!c::id(item.get("scan_id"),o.scan_id)||!c::id(item.get("domain_id"),o.domain_id)||!c::decimal(item.get("domain_revision"),o.domain_revision)||!o.domain_revision||!c::json_uint(item.get("physical_address"),pa,0,65535)||!c::json_uint(item.get("device_type"),type,0,255)||!c::json_uint(item.get("vendor_id"),vendor,0,16777215)||!c::json_uint(item.get("age_ms"),age,0,300000)||!c::json_bool(item.get("stale"),o.stale))return fail("Invalid exact native observation");
        const auto port=std::find_if(parsed.begin(),parsed.end(),[&](const auto& p){return p.port==name.port;});if(port==parsed.end()||port->domain_id!=o.domain_id||port->domain_revision!=o.domain_revision)return fail("Native observation belongs to a foreign attachment");
        o.endpoint={name.port,static_cast<uint8_t>(la)};o.osd_name=name.osd_name;o.owner_boot_id=boot;o.physical_address=static_cast<uint16_t>(pa);o.device_type=static_cast<uint8_t>(type);o.vendor_id=static_cast<uint32_t>(vendor);o.kind=s::ObservationKind::native_appliance;o.stale=o.stale||age>=30000||receiver_now<age;o.received_at_ms=receiver_now>=age?receiver_now-age:0;
        if(std::any_of(equipment.begin(),equipment.end(),[&](const auto& prior){return prior.endpoint==o.endpoint;}))return fail("Duplicate native endpoint observation");
        equipment.push_back(std::move(o));
    }
    if(observations)*observations=std::move(equipment);
    out=std::move(parsed);error.clear();return true;
}
}
