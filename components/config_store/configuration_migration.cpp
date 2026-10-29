#include "system_configuration.hpp"
#include <map>
#include <set>

namespace config_store {
namespace {
struct IdSource {
    std::function<bool(uint8_t*,size_t)> random;
    std::vector<std::string> pool;
    size_t next=0;
    bool failed=false;
    template<class T>T make(){std::optional<T> result;if(!pool.empty()){if(next<pool.size())result=T::parse(pool[next++]);}else result=T::generate(random);if(!result){failed=true;return {};}return *result;}
};
bool write_checked(ConfigBackend& backend,const char* key,const std::string& value){if(!backend.write(key,value))return false;std::string reread;return backend.read(key,reread)==ReadResult::found&&reread==value;}
struct Builder {
    ConfigurationV2& config;
    const std::vector<LegacyEvidence>& evidence;
    IdSource& ids;
    std::map<std::string,model::ComponentId> named;
    model::OctopusId owner;
    std::optional<model::ComponentId> equipment(const std::string& osd,model::Side side,const char* kind){
        if(osd.empty())return {};
        model::QualifiedDeviceName selected{{config.consortium_id,owner,0,side},osd};
        // Discovery may identify the existing local name more precisely, but
        // absence never changes whether the saved activity may be requested.
        const LegacyEvidence* match=nullptr;
        for(const auto& item:evidence)if(item.native&&item.unique&&item.name.osd_name==osd&&item.name.port.side==side&&item.name.port.consortium_id==config.consortium_id){if(match){match=nullptr;break;}match=&item;}
        if(match)selected=match->name;
        const auto key=selected.port.octopus_id.str()+":"+std::to_string(selected.port.tentacle_id)+":"+model::side_name(side)+":"+osd;
        const auto found=named.find(key);if(found!=named.end())return found->second;
        for(const auto& c:config.components)if(std::find(c.record.qualified_names.begin(),c.record.qualified_names.end(),selected)!=c.record.qualified_names.end()){named[key]=c.record.component_id;return c.record.component_id;}
        ConfiguredComponent c;c.record.component_id=ids.make<model::ComponentId>();c.record.label=c.generated_label=osd;c.kind=kind;
        if(selected.valid())c.record.qualified_names.push_back(selected);else c.kind="placeholder";
        config.components.push_back(c);named[key]=c.record.component_id;return c.record.component_id;
    }
    bool activity(const LegacyActivity& legacy){
        ConfiguredActivity a;a.activity_id=ids.make<model::ActivityId>();
        // Keep the original migration ID segment stable across an interrupted
        // v2 upgrade; its former route-ID slot is intentionally unused.
        if(!ids.pool.empty())++ids.next;
        a.zone_id=config.primary_zone_id;a.label=a.generated_label=legacy.name;a.switch_input=legacy.switch_input;
        a.source=equipment(legacy.source,model::Side::back,"source");a.display=equipment(legacy.display,model::Side::front,"display");
        if(!legacy.audio.empty())a.audio=legacy.audio==legacy.display?a.display:equipment(legacy.audio,model::Side::back,legacy.audio==legacy.source?"source":"receiver");
        a.switch_device=equipment(legacy.switch_device,model::Side::back,"switch");
        config.activities.push_back(a);config.activity_order.push_back(a.activity_id);
        for(auto& z:config.zones)if(z.zone_id==a.zone_id)z.activity_order.push_back(a.activity_id);
        if(config.external_slots.size()<16)config.external_slots.push_back({static_cast<uint16_t>(config.external_slots.size()),a.activity_id,false,false,{}});
        return !ids.failed;
    }
};
}

bool propose_local_activity(const ConfigurationV2& previous,const LegacyActivity& desired,const std::vector<LegacyEvidence>& evidence,
    const std::function<bool(uint8_t*,size_t)>& random,ConfigurationV2& out,std::vector<std::string>& tasks,std::string& error){
    if(!previous.controller_octopus_id||previous.activities.size()>=64||desired.name.empty()||desired.name.size()>128){error="Activity capacity exhausted or invalid name";return false;}
    auto candidate=previous;IdSource ids{random,{},0,false};Builder builder{candidate,evidence,ids,{},*previous.controller_octopus_id};
    if(!builder.activity(desired)||!validate_configuration_v2(candidate,error))return false;
    out=std::move(candidate);tasks.clear();error.clear();return true;
}

ConfigResult migrate_legacy(ConfigurationStore& store,ConfigBackend& backend,const std::string& original,const LegacyConfiguration& legacy,
    model::ConsortiumId consortium,model::OctopusId owner,const std::vector<LegacyEvidence>& evidence,
    const std::function<bool(uint8_t*,size_t)>& random,ConfigurationV2& out,std::vector<std::string>& tasks,std::string& error){
    ConfigurationV2 active;const auto recovered=store.recover(active,error);if(recovered==ConfigResult::ok){out=active;tasks.clear();return recovered;}if(recovered!=ConfigResult::missing)return recovered;
    if(!consortium||!owner||original.size()>configuration_compatibility_limit||original.find('\0')!=std::string::npos||legacy.activities.size()>16){error="Invalid legacy migration input";return ConfigResult::invalid;}
    std::string backup;const auto backup_status=backend.read("legacy_backup",backup);
    if(backup_status==ReadResult::error||(backup_status==ReadResult::found&&backup!=original)){error="Legacy backup conflict or storage error";return ConfigResult::recovery_required;}
    if(backup_status==ReadResult::missing&&!write_checked(backend,"legacy_backup",original))return ConfigResult::storage_error;
    const auto prefix="2:"+sha256_hex(original)+":"+consortium.str()+":"+owner.str()+":";
    std::string map;const auto map_status=backend.read("migration_ids",map);if(map_status==ReadResult::error)return ConfigResult::storage_error;
    if(map_status==ReadResult::missing){const auto first=model::MutationId::generate(random),second=model::MutationId::generate(random);if(!first||!second){error="Secure migration ID allocation failed";return ConfigResult::invalid;}map=prefix+first->str()+second->str();if(!write_checked(backend,"migration_ids",map))return ConfigResult::storage_error;}
    if(map.size()!=prefix.size()+64||map.compare(0,prefix.size(),prefix)){error="Migration mapping belongs to another original/system";return ConfigResult::recovery_required;}
    IdSource ids{random,{},0,false};std::set<std::string> unique;
    for(size_t n=0;n<512;++n){const auto value=sha256_hex("Amity migration ID stream v1:"+map.substr(prefix.size())+":"+std::to_string(n)).substr(0,32);if(!model::MutationId::parse(value)||!unique.insert(value).second)return ConfigResult::recovery_required;ids.pool.push_back(value);}
    ConfigurationV2 candidate;std::string retained;const auto retained_status=backend.read("migration_map",retained);if(retained_status==ReadResult::error)return ConfigResult::storage_error;
    if(retained_status==ReadResult::found){if(!parse_configuration_v2(retained,candidate,error)||candidate.consortium_id!=consortium)return ConfigResult::recovery_required;}
    else{
        candidate.consortium_id=consortium;candidate.controller_octopus_id=owner;candidate.primary_zone_id=ids.make<model::ZoneId>();candidate.octopuses.push_back({owner,"Amity 1","Amity 1",false,1});candidate.zones.push_back({candidate.primary_zone_id,"Main screen","Main screen",false,{}});
        Builder builder{candidate,evidence,ids,{},owner};
        for(size_t n=0;n<legacy.activities.size();++n){ids.next=1+n*31;if(!builder.activity(legacy.activities[n])||ids.next>1+(n+1)*31){error="Migration identity capacity exhausted";return ConfigResult::capacity;}}
        for(const auto& alias:legacy.aliases)if(alias.slot<candidate.external_slots.size()){candidate.external_slots[alias.slot].alias=alias.alias;candidate.external_slots[alias.slot].hidden=alias.hidden;}
    }
    bool changed=false;if(!bind_original_controller(candidate,owner,changed,error))return ConfigResult::recovery_required;
    if(!validate_configuration_v2(candidate,error))return ConfigResult::invalid;
    if(!write_checked(backend,"migration_map",serialize_configuration_v2(candidate)))return ConfigResult::storage_error;
    if(serialize_configuration_v2(candidate).size()>configuration_payload_limit){error="Migrated configuration exceeds saved payload capacity";return ConfigResult::capacity;}
    const auto staged=store.stage(candidate,{},error);if(staged!=ConfigResult::ok)return staged;
    out=*store.staged();tasks.clear();return ConfigResult::ok;
}
} // namespace config_store
