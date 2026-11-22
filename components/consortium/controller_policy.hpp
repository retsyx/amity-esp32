// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "amity/consortium/credential_types.hpp"
namespace amity::consortium::platform {
// Epoch1 records anchor the original controller. A historical transfer cannot
// prove the first-setup identity; preserve it for deliberate physical recovery.
inline bool controller_membership_compatible(const Group& group){
    return group.role==Group::unclaimed||group.epoch==1;
}
inline bool original_controller(const Group& group,model::OctopusId local){
    return local&&controller_membership_compatible(group)&&
        group.role==Group::coordinator&&group.authority==local;
}
}
