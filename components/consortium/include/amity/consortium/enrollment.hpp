#pragma once
#include "amity/consortium/wire.hpp"
namespace amity::consortium {
struct JoinProposal {
    model::JoinId join_id;model::ConsortiumId consortium_id;model::OctopusId coordinator_id,candidate_id;
    uint64_t epoch=0,revision=0;std::string coordinator_leaf,candidate_leaf,coordinator_pin,candidate_pin,host,candidate_hostname;
};
// The proposal assigns a hostname and the actuator role, authenticated by the
// immutable proposal digest. Platform also validates both leaf profiles.
std::string encode_join_proposal(const JoinProposal &);
bool decode_join_proposal(std::string_view,JoinProposal &);
}
