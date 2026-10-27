// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "amity/consortium/wire.hpp"
#include "amity/system/owned_intent.hpp"

namespace amity::consortium {
// The envelope supplies authenticated identities, boot/link/config fencing and
// the causal deadline. The payload carries the original action and selector.
std::string owned_intent_json(const model::OwnedIntent&, model::ExecutionLeaseId);
Error parse_owned_intent(const Json&, model::OwnedIntent&, model::ExecutionLeaseId&);
std::string owned_snapshot_json(const model::OwnedAdapterSnapshot&);
bool parse_owned_snapshot(const Json&,model::OwnedAdapterSnapshot&);
bool valid_owned_result(const Json&);
}
