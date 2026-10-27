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
inline ConfigurationV2 empty_configuration(){ConfigurationV2 c;c.consortium_id=id<model::ConsortiumId>(1);c.primary_zone_id=id<model::ZoneId>(10);c.controller_octopus_id=id<model::OctopusId>(2);c.octopuses.push_back({*c.controller_octopus_id,"amity-main","amity-main",false,1});c.zones.push_back({c.primary_zone_id,"Main screen","Main screen",false,{}});return c;}
inline ConfigurationV2 direct_configuration(){auto c=empty_configuration();std::vector<std::string> tasks;std::string error;ConfigurationV2 out;assert(propose_local_activity(c,{"Watch","TV","Source","TV","",0},{},random_source(),out,tasks,error));assert(tasks.empty());return out;}
inline void commit(ConfigurationStore& store,ConfigurationV2 c,RevisionPair expected,ConfigurationV2& out){std::string error;assert(store.stage(std::move(c),expected,error)==ConfigResult::ok);assert(store.commit(out,error)==ConfigResult::ok);}
inline std::string legacy_v2_json(const ConfigurationV2& c){auto* root=cJSON_Parse(serialize_configuration_v2(c).c_str());assert(root);cJSON_ReplaceItemInObjectCaseSensitive(root,"version",cJSON_CreateNumber(2));for(const auto* key:{"domains","binding_policies","resources","signal_edges","routes"})cJSON_AddArrayToObject(root,key);for(auto* a=cJSON_GetObjectItemCaseSensitive(root,"activities")->child;a;a=a->next){cJSON_AddStringToObject(a,"route_id",id<model::MessageId>(900).str().c_str());cJSON_AddBoolToObject(a,"enabled",false);}for(auto* e=cJSON_GetObjectItemCaseSensitive(root,"components")->child;e;e=e->next){cJSON_AddArrayToObject(e,"capabilities");cJSON_ReplaceItemInObjectCaseSensitive(e,"qualified_names",cJSON_CreateArray());cJSON_ReplaceItemInObjectCaseSensitive(e,"kind",cJSON_CreateString("placeholder"));}char* text=cJSON_PrintUnformatted(root);std::string result(text);cJSON_free(text);cJSON_Delete(root);return result;}
inline void install_legacy_record(Memory& memory,const ConfigurationV2& c){const auto payload=legacy_v2_json(c);const auto record="AMITY2\n"+c.consortium_id.str()+"\n"+model::format_uint64(c.config_revision)+"\n"+model::format_uint64(c.presentation_revision)+"\n"+std::to_string(payload.size())+"\n"+sha256_hex(payload)+"\n"+payload;memory.values["c0"]=record;memory.values["cm0"]="2:1:0:"+sha256_hex(record);}
