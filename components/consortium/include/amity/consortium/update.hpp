// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "amity/consortium/wire.hpp"
namespace amity::consortium {
// Closed peer-update messages. Firmware crypto/storage remain actuator-local;
// no network message can replace its compiled verifier or skip manifest checks.
bool is_update_kind(Kind);
bool valid_update(Kind, const Json&);
}
