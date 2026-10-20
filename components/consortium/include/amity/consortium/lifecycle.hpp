// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "amity/consortium/session.hpp"
#include <functional>
#include <vector>

namespace amity::consortium {
enum class MembershipState { unclaimed=0, prepared=1, committed=2, active=3, retired=4, recovery_required=7 };
struct MembershipRecord {
    MembershipState state=MembershipState::unclaimed;
    model::ConsortiumId consortium_id;
    model::OctopusId coordinator_id;
    uint64_t epoch=0,membership_revision=0;
    model::JoinId join_id;
    std::string join_digest;
    model::MutationId mutation_id;
    std::string mutation_digest;
    bool ready=false;
};
// This core does not implement cryptography, flash serialization, or generate
// trust. Its adapter must verify Security2 and exact-leaf mTLS before calling
// authenticated transitions. Save must atomically commit the complete record.
class MembershipJournal {
public:
    using Save=std::function<bool(const MembershipRecord &)>;
    MembershipJournal(MembershipRecord loaded,Save save);
    const MembershipRecord &record()const{return record_;}
    Error prepare_authenticated(const MembershipRecord &proposal);
    Error challenge(model::JoinId,std::string_view digest,model::LinkId,model::MessageId nonce);
    Error prove_lan(model::JoinId,std::string_view digest,model::LinkId,model::MessageId nonce);
    Error commit(model::JoinId,std::string_view digest,uint64_t membership_revision);
    Error candidate_active(model::JoinId,std::string_view digest,uint64_t membership_revision);
    Error ready_after_sync(model::JoinId,std::string_view digest,uint64_t membership_revision);
    Error abort(model::JoinId,std::string_view digest);
    Error retire(model::MutationId,std::string_view digest,uint64_t membership_revision);
    void disconnected();
    bool permits_execution()const{return record_.state==MembershipState::active&&record_.ready;}
private:
    bool join(model::JoinId,std::string_view)const;
    Error persist(MembershipRecord);
    MembershipRecord record_;Save save_;
    model::LinkId proof_link_;model::MessageId nonce_;bool lan_proved_=false;
};
// Active allowlist is authoritative even if tombstones have been pruned.
// Pins are public SHA-256 SPKI identifiers, never trust learned from mDNS.
struct MemberCredential {
    model::OctopusId id;
    std::string leaf_der,spki_sha256;
    bool provisional=false;
};
class MembershipRoster {
public:
    static constexpr size_t capacity=8; // build ceiling, not a protocol limit
    using Save=std::function<bool(const std::vector<MemberCredential>&,uint64_t)>;
    explicit MembershipRoster(Save save):save_(std::move(save)){}
    Error restore(std::vector<MemberCredential>,uint64_t revision);
    Error add(MemberCredential,uint64_t expected_revision);
    Error revoke(model::OctopusId,uint64_t expected_revision);
    const MemberCredential *lookup(model::OctopusId)const;
    uint64_t revision()const{return revision_;}
private:
    std::vector<MemberCredential> members_;uint64_t revision_=0;Save save_;
};
// Dedicated operational certificate policy only. TLS adapter still requires
// VERIFY_REQUIRED, exact enrolled leaf anchors, key proof and P-256 profile.
// No function here may be reused for HTTPS or OTA time validation.
enum VerifyFlag : uint32_t { certificate_expired=1,certificate_future=2,certificate_other_failure=4 };
struct VerifiedLeafEvidence {
    std::string leaf_der,spki_sha256;
    model::OctopusId certificate_identity;
    uint32_t flags=0;
    bool profile_valid=false,self_signature_valid=false,key_possession_verified=false;
};
class OperationalCredentialPolicy {
public:
    static bool accepts(const MemberCredential &,const VerifiedLeafEvidence &);
};
struct ReleaseCompatibility {
    uint8_t protocol_min=1,protocol_max=1;
    uint32_t required_capabilities=0;
    uint32_t readable_storage_min=1,readable_storage_max=1,writable_storage=1;
    uint64_t slot_bytes=0,image_bytes=0;
    bool production=false,publisher_signature_verified=false,native_image_verified=false;
};
Error check_release(const ReleaseCompatibility &,uint32_t installed_storage,uint32_t rollback_readable_max,uint32_t implemented_capabilities);
} // namespace amity::consortium
