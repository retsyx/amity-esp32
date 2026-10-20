// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "amity/consortium/wire.hpp"
namespace amity::consortium::platform {
struct Identity {
    model::OctopusId id;
    std::string der,key_der,spki;
};
struct Peer {
    model::OctopusId id;
    std::string der,pin;
    bool active=false;
};
struct Group {
    enum Role{unclaimed,coordinator,member,prepared,retired};
    Role role=unclaimed;
    model::ConsortiumId consortium;
    model::OctopusId authority;
    uint64_t epoch=0,revision=0;
    std::string authority_host;
    model::MutationId mutation_id;std::string mutation_digest;
    model::JoinId join_id;model::OctopusId joining_id;std::string join_digest,join_hostname;uint64_t join_revision=0;
    std::vector<Peer> peers;
};
}
