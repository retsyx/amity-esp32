// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "amity/consortium/wire.hpp"
#include <functional>
namespace amity::consortium {
struct BindingReplacement {uint64_t expected_binding_revision=0;model::ComponentBinding binding;};
struct BindingResolution {
    model::ResolutionId resolution_id;model::QualifiedDeviceName device_name;
    model::BootId observation_boot_id;uint64_t observation_sequence=0;
    std::vector<BindingReplacement> bindings;
};
Error parse_binding_resolution(const Json &,BindingResolution &);
struct ResolutionEvidence {
    model::QualifiedDeviceName device_name;model::BootId boot_id;uint64_t sequence=0;
    uint8_t logical_address=15;bool unique=false;
};
// One local selector refresh transaction at a time, up to eight capability
// bindings. Observation fences old targets immediately; authority must install
// the complete replacement atomically before any new target is usable.
class BindingSynchronizer {
public:
    using Fence=std::function<void(model::BindingId,uint64_t)>;
    explicit BindingSynchronizer(model::OctopusId local,Fence fence):local_(local),fence_(std::move(fence)){}
    Error restore(std::vector<model::ComponentBinding> installed);
    Error observe(const ResolutionEvidence &);
    Error install(const BindingResolution &,bool synchronized_current_authority);
    const std::vector<model::ComponentBinding> &bindings()const{return bindings_;}
    bool target_current(const Operation &)const;
private:
    model::OctopusId local_;Fence fence_;std::vector<model::ComponentBinding> bindings_;
    std::optional<ResolutionEvidence> evidence_;
    std::optional<BindingResolution> last_;
};
} // namespace amity::consortium
