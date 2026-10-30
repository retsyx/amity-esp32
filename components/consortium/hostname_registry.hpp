// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "amity/consortium/wire.hpp"
#include "amity/consortium/hostname.hpp"
#include <functional>
namespace amity::consortium {
// Reset-owned public metadata, deliberately separate from the active allowlist.
// Offline peers retain their authenticated names; explicit removal erases them.
class HostnameRegistry {
public:
    using Entry=std::pair<model::OctopusId,std::string>;
    static constexpr size_t capacity=64;
    bool restore(std::string_view record) {
        Json value;JsonError error;uint64_t version=0;
        if(!parse_json(record,value,error,8192,256)||!closed(value,{"version","devices"})||!json_uint(value.get("version"),version,1,1))return false;
        const auto* devices=value.get("devices");if(devices->type!=Json::Type::array||devices->array.size()>capacity)return false;
        std::vector<Entry> next;
        for(const auto& item:devices->array){model::OctopusId owner;std::string name;
            if(!closed(item,{"octopus_id","hostname"})||!id(item.get("octopus_id"),owner)||!json_string(item.get("hostname"),name,1,32)||!valid_hostname(name))return false;
            for(const auto& prior:next)if(prior.first==owner)return false;
            next.emplace_back(owner,std::move(name));
        }
        entries_=std::move(next);return true;
    }
    std::string lookup(model::OctopusId owner)const {for(const auto& item:entries_)if(item.first==owner)return item.second;return {};}
    Error remember(model::OctopusId owner,const std::string& name,const std::function<bool(std::string_view)>& save) {
        if(!owner||!valid_hostname(name))return Error::invalid_message;
        if(lookup(owner).empty()&&entries_.size()>=capacity)return Error::queue_full;
        if(lookup(owner)==name)return Error::none;
        auto next=entries_;auto found=std::find_if(next.begin(),next.end(),[&](const auto& item){return item.first==owner;});
        if(found==next.end())next.emplace_back(owner,name);else found->second=name;
        std::sort(next.begin(),next.end(),[](const auto& a,const auto& b){return a.first<b.first;});
        return commit(std::move(next),save);
    }
    Error forget(model::OctopusId owner,const std::function<bool(std::string_view)>& save) {
        if(!owner)return Error::invalid_message;
        if(lookup(owner).empty())return Error::none;
        auto next=entries_;
        next.erase(std::remove_if(next.begin(),next.end(),[&](const auto& item){return item.first==owner;}),next.end());
        return commit(std::move(next),save);
    }
    const std::vector<Entry>& entries()const{return entries_;}
private:
    Error commit(std::vector<Entry> next,const std::function<bool(std::string_view)>& save) {
        std::string record="{\"version\":1,\"devices\":[";
        for(size_t index=0;index<next.size();++index){if(index)record+=",";record+="{\"octopus_id\":\""+next[index].first.str()+"\",\"hostname\":\""+next[index].second+"\"}";}
        record+="]}";if(!save||!save(record))return Error::storage_failure;
        entries_=std::move(next);return Error::none;
    }
    std::vector<Entry> entries_;
};
}
