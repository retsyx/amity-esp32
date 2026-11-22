// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "amity/consortium/credential_types.hpp"
namespace amity::consortium::platform {
// There is no failover: the coordinator is always the original controller.
inline bool original_controller(const Group& group,model::OctopusId local){
    return local&&group.role==Group::coordinator&&group.authority==local;
}
}
