#pragma once
#include "amity/consortium/credential_types.hpp"

namespace amity::consortium::platform {
// A cancelled prepared candidate remains claimed and passive. Only physical
// reclaim may restore an independent authority; the receipt survives reboot.
inline const char* candidate_join_state(const Group& group,model::JoinId join,std::string_view digest){
    if(!join||group.join_id!=join||group.join_digest!=digest)return "unknown";
    if(group.role==Group::prepared)return "prepared";
    if(group.role==Group::member)return "active";
    if(group.role==Group::retired&&group.mutation_id&&group.mutation_digest==digest)return "aborted";
    return "unknown";
}
inline bool abort_prepared_candidate(const Group& current,model::JoinId join,std::string_view digest,Group& next){
    const auto state=std::string_view(candidate_join_state(current,join,digest));
    if(state=="aborted"){next=current;return true;}
    if(state!="prepared")return false;
    const auto mutation=model::MutationId::parse(join.str());if(!mutation)return false;
    next=current;next.role=Group::retired;next.mutation_id=*mutation;next.mutation_digest=std::string(digest);
    for(auto& peer:next.peers)peer.active=false;
    return true;
}
}
