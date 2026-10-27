// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "amity/system/discovery.hpp"
namespace amity::system {
enum class AdmissionCode { accepted, pending, invalid, stale, expired, unauthorized, unsupported, overloaded, unresolved };
struct AdmissionResult {
    AdmissionCode code = AdmissionCode::invalid;
    std::string reason;
    std::optional<bool> source_present = std::nullopt;
    explicit operator bool() const { return code==AdmissionCode::accepted; }
};
// Volatile UI correlation only. Activity selections are never persisted.
struct ActiveActivity {
    SessionId session_id;
    ZoneId zone_id;
    ActivityId activity_id;
    uint64_t generation = 0;
};
struct ActivityOutcome { MessageId request_id; std::string stage, error; };
struct ExecutionSnapshot {
    uint64_t applied_config_revision = 0, applied_presentation_revision = 0;
    std::string applied_policy_hash;
    uint64_t state_revision = 0;
    bool activity_settling = false;
    std::vector<ActiveActivity> sessions;
    std::vector<ActivityOutcome> activity_outcomes;
    std::vector<CecObservation> native_observations;
    std::string error;
};
}
