// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#include "amity/consortium/bindings.hpp"
#include <algorithm>
namespace amity::consortium {
namespace {
bool parse_binding(const Json &v,model::ComponentBinding &b){
    // Run the same closed binding validator used by the wire envelope by
    // accepting only the native refresh shape here; proxy bindings never move
    // as a consequence of an appliance's OSD name resolution.
    if(!closed(v,{"binding_id","component_id","capability","binding_kind","endpoint","owner_octopus_id","domain_id","domain_revision","binding_revision","role","physical_address_evidence_ref","status","device_name"}))return false;
    if(!id(v.get("binding_id"),b.binding_id)||!id(v.get("component_id"),b.component_id)||!id(v.get("owner_octopus_id"),b.owner_octopus_id)||!id(v.get("domain_id"),b.domain_id)||!id(v.get("physical_address_evidence_ref"),b.physical_address_evidence_ref)||!decimal(v.get("domain_revision"),b.domain_revision)||!decimal(v.get("binding_revision"),b.binding_revision)||!parse_address(*v.get("endpoint"),b.endpoint))return false;
    if(v.get("binding_kind")->type!=Json::Type::string||v.get("binding_kind")->string!="native_appliance")return false;
    model::QualifiedDeviceName name;if(!parse_name(*v.get("device_name"),name))return false;b.device_name=name;b.kind=model::BindingKind::native_appliance;
    const auto cap=v.get("capability"),role=v.get("role"),status=v.get("status");if(cap->type!=Json::Type::string||role->type!=Json::Type::string||status->type!=Json::Type::string)return false;
    if(cap->string=="power")b.capability=model::Capability::power;else if(cap->string=="navigation")b.capability=model::Capability::navigation;else if(cap->string=="audio")b.capability=model::Capability::audio;else if(cap->string=="routing")b.capability=model::Capability::routing;else if(cap->string=="query_state")b.capability=model::Capability::query_state;else return false;
    if(role->string=="playback_proxy")b.role=model::PortRole::playback_proxy;else if(role->string=="tv_controller")b.role=model::PortRole::tv_controller;else if(role->string=="monitor")b.role=model::PortRole::monitor;else if(role->string=="disabled")b.role=model::PortRole::disabled;else return false;
    if(status->string=="tentative")b.status=model::BindingStatus::tentative;else if(status->string=="verified")b.status=model::BindingStatus::verified;else return false;
    return b.owner_octopus_id==b.endpoint.port.octopus_id&&name.port==b.endpoint.port;
}
bool same_record(const model::ComponentBinding &a,const model::ComponentBinding &b){
    return a.binding_id==b.binding_id&&a.component_id==b.component_id&&a.capability==b.capability&&a.kind==b.kind&&a.device_name==b.device_name&&a.endpoint==b.endpoint&&a.owner_octopus_id==b.owner_octopus_id&&a.domain_id==b.domain_id&&a.domain_revision==b.domain_revision&&a.binding_revision==b.binding_revision&&a.role==b.role&&a.physical_address_evidence_ref==b.physical_address_evidence_ref&&a.proxy_identity_resource_id==b.proxy_identity_resource_id;
}
bool same_resolution(const BindingResolution &a,const BindingResolution &b){
    if(a.resolution_id!=b.resolution_id||a.device_name!=b.device_name||a.observation_boot_id!=b.observation_boot_id||a.observation_sequence!=b.observation_sequence||a.bindings.size()!=b.bindings.size())return false;
    for(size_t i=0;i<a.bindings.size();++i)if(a.bindings[i].expected_binding_revision!=b.bindings[i].expected_binding_revision||!same_record(a.bindings[i].binding,b.bindings[i].binding))return false;
    return true;
}
}
Error parse_binding_resolution(const Json &v,BindingResolution &out){
    if(!closed(v,{"resolution_id","device_name","observation_boot_id","observation_sequence","bindings"}))return Error::invalid_message;
    BindingResolution r;if(!id(v.get("resolution_id"),r.resolution_id)||!parse_name(*v.get("device_name"),r.device_name)||!id(v.get("observation_boot_id"),r.observation_boot_id)||!decimal(v.get("observation_sequence"),r.observation_sequence))return Error::invalid_message;
    const auto entries=v.get("bindings");if(entries->type!=Json::Type::array||entries->array.empty()||entries->array.size()>8)return Error::invalid_message;
    for(const auto &e:entries->array){BindingReplacement next;if(!closed(e,{"expected_binding_revision","binding"})||!decimal(e.get("expected_binding_revision"),next.expected_binding_revision)||!parse_binding(*e.get("binding"),next.binding)||!next.binding.device_name||*next.binding.device_name!=r.device_name)return Error::invalid_message;r.bindings.push_back(std::move(next));}
    out=std::move(r);return Error::none;
}
Error BindingSynchronizer::restore(std::vector<model::ComponentBinding> installed){
    if(installed.size()>64)return Error::queue_full;
    for(size_t i=0;i<installed.size();++i){
        const auto &b=installed[i];
        if(!b.binding_id||!b.component_id||!b.domain_id||!b.endpoint.valid()||b.owner_octopus_id!=local_||
            (b.kind==model::BindingKind::native_appliance&&(!b.device_name||!b.device_name->valid()||b.device_name->port!=b.endpoint.port))||
            (b.kind==model::BindingKind::proxy_representation&&(b.device_name||!b.proxy_identity_resource_id)))
            return Error::invalid_message;
        for(size_t j=0;j<i;++j)
            if(installed[j].binding_id==b.binding_id)return Error::invalid_message;
    }
    bindings_=std::move(installed);evidence_.reset();last_.reset();return Error::none;
}
Error BindingSynchronizer::observe(const ResolutionEvidence &e){
    if(!e.device_name.valid()||e.device_name.port.octopus_id!=local_||!e.boot_id||e.logical_address>14)return Error::invalid_message;
    if(evidence_&&evidence_->device_name==e.device_name&&evidence_->boot_id==e.boot_id&&e.sequence<=evidence_->sequence)return Error::stale_binding;
    for(auto &b:bindings_)if(b.device_name&&*b.device_name==e.device_name){if(b.status==model::BindingStatus::verified&&(!e.unique||b.endpoint.logical_address!=e.logical_address)){if(fence_)fence_(b.binding_id,b.binding_revision);b.status=model::BindingStatus::quarantined;}}
    evidence_=e;return Error::none;
}
Error BindingSynchronizer::install(const BindingResolution &r,bool authority){
    if(!authority)return Error::not_ready;
    if(last_&&last_->resolution_id==r.resolution_id)return same_resolution(*last_,r)?Error::none:Error::transaction_conflict;
    if(!r.resolution_id||!evidence_||!evidence_->unique||evidence_->device_name!=r.device_name||evidence_->boot_id!=r.observation_boot_id||evidence_->sequence!=r.observation_sequence)return Error::stale_binding;
    size_t expected_count=0;for(const auto &b:bindings_)if(b.device_name&&*b.device_name==r.device_name)++expected_count;
    if(!expected_count||r.bindings.size()!=expected_count||r.bindings.size()>8)return Error::stale_binding;
    auto next=bindings_;std::vector<model::BindingId> seen;
    for(const auto &replacement:r.bindings){const auto &b=replacement.binding;
        if(std::find(seen.begin(),seen.end(),b.binding_id)!=seen.end())return Error::invalid_message;
        seen.push_back(b.binding_id);
        auto old=std::find_if(next.begin(),next.end(),[&](const auto &v){return v.binding_id==b.binding_id;});
        if(old==next.end()||!old->device_name||*old->device_name!=r.device_name||old->binding_revision!=replacement.expected_binding_revision||b.binding_revision<=old->binding_revision)return Error::stale_binding;
        if(!b.endpoint.valid()||!b.binding_id||!b.component_id||!b.domain_id||!b.device_name||*b.device_name!=r.device_name||b.kind!=model::BindingKind::native_appliance||b.component_id!=old->component_id||b.capability!=old->capability||b.role!=old->role||b.owner_octopus_id!=local_||b.endpoint.port!=old->endpoint.port||b.endpoint.logical_address!=evidence_->logical_address||!b.physical_address_evidence_ref)return Error::invalid_message;
        if(b.domain_id!=old->domain_id||b.domain_revision<old->domain_revision)return Error::stale_domain;
        *old=b;old->status=model::BindingStatus::verified;
    }
    bindings_.swap(next);last_=r;return Error::none;
}
bool BindingSynchronizer::target_current(const Operation &op)const{
    for(const auto &b:bindings_)if(b.binding_id==op.binding_id)return b.status==model::BindingStatus::verified&&b.component_id==op.component_id&&b.endpoint==op.target&&b.domain_id==op.domain_id&&b.binding_revision==op.binding_revision&&b.domain_revision==op.domain_revision;
    return false;
}
} // namespace amity::consortium
