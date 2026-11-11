#pragma once
#include "system_configuration.hpp"
#include "cJSON.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <map>
using namespace config_store;
template<class T>T id(unsigned n){char text[33];std::snprintf(text,sizeof(text),"%032x",n);return T::parse(text).value();}
struct PowerCut{};
struct Memory:ConfigBackend{
    std::map<std::string,std::string> values;unsigned operations=0,cut_at=0,writes=0;
    void step(){if(++operations==cut_at)throw PowerCut{};}
    ReadResult read(const char* key,std::string& value)override{const auto found=values.find(key);if(found==values.end()){step();return ReadResult::missing;}value=found->second;step();return ReadResult::found;}
    bool write(const char* key,const std::string& value)override{values[key]=value;++writes;step();return true;}
    bool erase(const char* key)override{values.erase(key);step();return true;}
};
inline auto random_source(){static uint64_t seed=1000;seed+=1000;return [counter=seed](uint8_t* data,size_t size)mutable{std::fill(data,data+size,0);++counter;for(size_t n=0;n<std::min<size_t>(size,8);++n)data[size-1-n]=uint8_t(counter>>(n*8));return true;};}
inline Configuration empty_configuration(){Configuration c;c.consortium_id=id<model::ConsortiumId>(1);c.primary_zone_id=id<model::ZoneId>(10);c.controller_octopus_id=id<model::OctopusId>(2);c.octopuses.push_back({*c.controller_octopus_id,"amity-main","amity-main",false,1});c.zones.push_back({c.primary_zone_id,"Main screen","Main screen",false,{}});return c;}
// Construct current saved records directly; no old-format import seam is used.
inline model::ComponentId fixture_equipment(Configuration& c, const std::string& name,
    const std::string& kind, model::Side side) {
    for (const auto& component : c.components)
        if (component.record.qualified_names.size() == 1 && component.record.qualified_names[0] ==
            model::QualifiedDeviceName{{c.consortium_id, *c.controller_octopus_id, 0, side}, name})
            return component.record.component_id;
    ConfiguredComponent component;
    component.record.component_id = *model::ComponentId::generate(random_source());
    component.record.label = component.generated_label = name; component.kind = kind;
    component.record.qualified_names.push_back({{c.consortium_id, *c.controller_octopus_id, 0, side}, name});
    c.components.push_back(component); return component.record.component_id;
}
inline void fixture_activity(Configuration& c, ConfiguredActivity activity) {
    activity.activity_id = *model::ActivityId::generate(random_source());
    activity.zone_id = c.primary_zone_id;
    c.activities.push_back(activity); c.activity_order.push_back(activity.activity_id);
    for (auto& zone : c.zones) if (zone.zone_id == c.primary_zone_id) zone.activity_order.push_back(activity.activity_id);
    c.external_slots.push_back({static_cast<uint16_t>(c.external_slots.size()), activity.activity_id, false, false, {}});
    std::string error; assert(validate_configuration(c, error));
}
inline Configuration direct_configuration() {
    auto c = empty_configuration(); ConfiguredActivity activity;
    activity.label = activity.generated_label = "Watch";
    activity.source = fixture_equipment(c, "Source", "source", model::Side::back);
    activity.display = activity.audio = fixture_equipment(c, "TV", "display", model::Side::front);
    fixture_activity(c, activity); return c;
}
inline Configuration switch_configuration(Configuration c, const std::string& label = "Console") {
    ConfiguredActivity activity; activity.label = activity.generated_label = label;
    activity.source = fixture_equipment(c, "Wii", "source", model::Side::back);
    activity.display = fixture_equipment(c, "TV", "display", model::Side::front);
    activity.audio = activity.switch_device = fixture_equipment(c, "AVR", "receiver", model::Side::back);
    activity.switch_input = 6; fixture_activity(c, activity); return c;
}
inline void commit(ConfigurationStore& store,Configuration c,RevisionPair expected,Configuration& out){std::string error;assert(store.stage(std::move(c),expected,error)==ConfigResult::ok);ConfigurationSnapshot snapshot;assert(store.commit(snapshot,error)==ConfigResult::ok);out=snapshot->configuration;}

inline void commit(ConfigurationStore& store,Configuration c,RevisionPair expected,ConfigurationSnapshot& out){std::string error;assert(store.stage(std::move(c),expected,error)==ConfigResult::ok);assert(store.commit(out,error)==ConfigResult::ok);}
