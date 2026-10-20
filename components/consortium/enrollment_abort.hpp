#pragma once
#include "amity/consortium/credential_types.hpp"
#include "amity/consortium/wire.hpp"

namespace amity::consortium::platform {
// Retains only the already-authenticated, removed provisional credential for
// one terminal transaction. It never reinstalls that credential in the roster.
struct EnrollmentAbort {
    Peer peer;
    model::ConsortiumId consortium;
    model::OctopusId authority;
    uint64_t epoch=0;
    model::JoinId join;
    std::string digest;
    bool matches(const Group& current,model::OctopusId authenticated) const {
        return peer.id==authenticated&&consortium==current.consortium&&authority==current.authority&&epoch==current.epoch;
    }
    bool matches(const Json& payload) const {
        const auto* id=payload.get("join_id");const auto* hash=payload.get("join_digest");
        return id&&id->type==Json::Type::string&&id->string==join.str()&&hash&&hash->type==Json::Type::string&&hash->string==digest;
    }
    bool may_send(const Json& payload) const {
        if(!matches(payload))return false;
        const auto* phase=payload.get("phase");if(!phase||phase->type!=Json::Type::string)return false;
        if(phase->string=="abort"){
            const auto* reason=payload.get("reason");
            return closed(payload,{"phase","join_id","join_digest","reason"})&&reason&&reason->type==Json::Type::string&&
                (reason->string=="cancelled"||reason->string=="incompatible"||reason->string=="network_failed"||reason->string=="internal_error");
        }
        const auto* request=payload.get("request");
        return closed(payload,{"phase","join_id","join_digest","request"})&&phase->string=="status"&&request&&request->type==Json::Type::boolean&&request->boolean;
    }
};
enum class EnrollmentAbortStatus { invalid,ignore,prepared,committed,active,aborted,unknown };
inline EnrollmentAbortStatus enrollment_abort_status(const EnrollmentAbort& pending,const Json& payload){
    if(!pending.matches(payload))return EnrollmentAbortStatus::invalid;
    const auto* phase=payload.get("phase");if(!phase||phase->type!=Json::Type::string)return EnrollmentAbortStatus::invalid;
    // A prove/ack may already have been queued before the removal. Consume it
    // without treating it as authority to continue the enrollment transaction.
    if(phase->string=="prove"||phase->string=="ack")return EnrollmentAbortStatus::ignore;
    const auto* request=payload.get("request");const auto* state=payload.get("state");
    if(phase->string!="status"||!request||request->type!=Json::Type::boolean||request->boolean||!state||state->type!=Json::Type::string)return EnrollmentAbortStatus::invalid;
    if(state->string=="prepared")return EnrollmentAbortStatus::prepared;
    if(state->string=="committed")return EnrollmentAbortStatus::committed;
    if(state->string=="active")return EnrollmentAbortStatus::active;
    if(state->string=="aborted")return EnrollmentAbortStatus::aborted;
    if(state->string=="unknown")return EnrollmentAbortStatus::unknown;
    return EnrollmentAbortStatus::invalid;
}
}
