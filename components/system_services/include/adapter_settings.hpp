// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "amity/consortium/wire.hpp"
#include <string>
namespace amity { class SystemControl; }
namespace system_services::adapter_settings {
// Register after the native owner starts. No scan is initiated by reading.
void install(amity::SystemControl&);
std::string snapshot();
// The deadline is receiver-local and preserves the original HTTP/transport
// authority. Success means one durable batch and a quiescent target owner.
amity::consortium::Error apply(amity::system::MessageId,
    const amity::consortium::Json&, uint64_t receiver_deadline_ms,
    std::string& response);
// Invoke only after a durable applied result's transmission attempt completes,
// whether delivery succeeds or fails. Admission alone never schedules restart.
void after_apply();
}
