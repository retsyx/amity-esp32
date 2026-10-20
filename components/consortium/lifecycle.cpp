// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#include "amity/consortium/lifecycle.hpp"
#include <algorithm>
namespace amity::consortium {
namespace {
bool digest(std::string_view s){return s.size()==64&&std::all_of(s.begin(),s.end(),[](char c){return(c>='a'&&c<='f')||(c>='0'&&c<='9');});}
bool valid(const MemberCredential &c){return c.id&&!c.leaf_der.empty()&&c.leaf_der.size()<=1024&&digest(c.spki_sha256);}
}
MembershipJournal::MembershipJournal(MembershipRecord loaded,Save save):record_(std::move(loaded)),save_(std::move(save)){
    const auto state=record_.state;
    const bool recognized=state==MembershipState::unclaimed||state==MembershipState::prepared||state==MembershipState::committed||state==MembershipState::active||state==MembershipState::retired||state==MembershipState::recovery_required;
    const bool claimed=state!=MembershipState::unclaimed&&state!=MembershipState::recovery_required;
    if(!recognized||(claimed&&(record_.epoch!=1||!record_.consortium_id||!record_.coordinator_id)))record_.state=MembershipState::recovery_required;
    if((state==MembershipState::prepared||state==MembershipState::committed||state==MembershipState::active)&&(!record_.join_id||!digest(record_.join_digest)))record_.state=MembershipState::recovery_required;
    if(record_.state==MembershipState::recovery_required)record_.join_digest.clear();
    // Readiness is current-session evidence and never durable reboot authority.
    record_.ready=false;
}
Error MembershipJournal::persist(MembershipRecord next){if(!save_||!save_(next))return Error::storage_failure;record_=std::move(next);return Error::none;}
bool MembershipJournal::join(model::JoinId id,std::string_view hash)const{return id==record_.join_id&&hash==record_.join_digest;}
Error MembershipJournal::prepare_authenticated(const MembershipRecord &p){
    if(record_.state!=MembershipState::unclaimed)return join(p.join_id,p.join_digest)&&record_.state==MembershipState::prepared&&p.consortium_id==record_.consortium_id&&p.coordinator_id==record_.coordinator_id&&p.epoch==record_.epoch&&p.membership_revision==record_.membership_revision?Error::none:Error::transaction_conflict;
    if(p.state!=MembershipState::prepared||!p.consortium_id||!p.coordinator_id||p.epoch!=1||!p.join_id||!digest(p.join_digest)||p.ready||p.mutation_id||!p.mutation_digest.empty())return Error::invalid_message;
    return persist(p);
}
Error MembershipJournal::challenge(model::JoinId id,std::string_view hash,model::LinkId link,model::MessageId nonce){
    if(!join(id,hash)||(record_.state!=MembershipState::prepared&&record_.state!=MembershipState::committed)||!link||!nonce)return Error::transaction_conflict;
    proof_link_=link;nonce_=nonce;lan_proved_=false;return Error::none;
}
Error MembershipJournal::prove_lan(model::JoinId id,std::string_view hash,model::LinkId link,model::MessageId nonce){
    if(!join(id,hash)||!proof_link_||proof_link_!=link||nonce_!=nonce)return Error::transaction_conflict;
    lan_proved_=true;return Error::none;
}
Error MembershipJournal::commit(model::JoinId id,std::string_view hash,uint64_t revision){
    if(!join(id,hash))return Error::transaction_conflict;
    if(record_.state==MembershipState::committed||record_.state==MembershipState::active)return record_.membership_revision==revision?Error::none:Error::transaction_conflict;
    if(record_.state!=MembershipState::prepared||!lan_proved_||revision<=record_.membership_revision)return Error::not_ready;
    auto next=record_;next.state=MembershipState::committed;next.membership_revision=revision;next.ready=false;return persist(next);
}
Error MembershipJournal::candidate_active(model::JoinId id,std::string_view hash,uint64_t revision){
    if(!join(id,hash)||record_.membership_revision!=revision)return Error::transaction_conflict;
    if(record_.state==MembershipState::active)return Error::none;
    if(record_.state!=MembershipState::committed)return Error::not_ready;
    auto next=record_;next.state=MembershipState::active;next.ready=false;return persist(next);
}
Error MembershipJournal::ready_after_sync(model::JoinId id,std::string_view hash,uint64_t revision){
    if(!join(id,hash)||record_.membership_revision!=revision)return Error::transaction_conflict;
    if(record_.state!=MembershipState::active)return Error::not_ready;
    record_.ready=true;return Error::none;
}
Error MembershipJournal::abort(model::JoinId id,std::string_view hash){
    if(!join(id,hash))return Error::transaction_conflict;
    if(record_.state==MembershipState::retired&&record_.mutation_id&&record_.mutation_digest==hash)return Error::none;
    if(record_.state!=MembershipState::prepared)return Error::not_ready;
    auto next=record_;next.state=MembershipState::retired;next.ready=false;next.mutation_id=*model::MutationId::parse(id.str());next.mutation_digest=std::string(hash);
    auto err=persist(next);if(err==Error::none)disconnected();return err;
}
Error MembershipJournal::retire(model::MutationId mutation,std::string_view hash,uint64_t revision){
    if(record_.state==MembershipState::retired)return mutation==record_.mutation_id&&hash==record_.mutation_digest&&revision==record_.membership_revision?Error::none:Error::transaction_conflict;
    if(!mutation||!digest(hash)||revision<=record_.membership_revision)return Error::invalid_message;
    if(record_.state!=MembershipState::active&&record_.state!=MembershipState::committed)return Error::not_ready;
    auto next=record_;next.state=MembershipState::retired;next.mutation_id=mutation;next.mutation_digest=std::string(hash);next.membership_revision=revision;next.ready=false;
    // The adapter invalidates leases/holds as soon as retirement is requested;
    // an I/O failure cannot preserve execution readiness in RAM.
    record_.ready=false;auto err=persist(next);disconnected();return err;
}
void MembershipJournal::disconnected(){proof_link_={};nonce_={};lan_proved_=false;record_.ready=false;}
const MemberCredential *MembershipRoster::lookup(model::OctopusId id)const{for(const auto &c:members_)if(c.id==id)return &c;return nullptr;}
Error MembershipRoster::restore(std::vector<MemberCredential> members,uint64_t revision){
    if(members.size()>capacity)return Error::queue_full;
    for(size_t i=0;i<members.size();++i){if(!valid(members[i]))return Error::invalid_message;for(size_t j=0;j<i;++j)if(members[i].id==members[j].id||members[i].spki_sha256==members[j].spki_sha256)return Error::identity_conflict;}
    members_=std::move(members);revision_=revision;return Error::none;
}
Error MembershipRoster::add(MemberCredential member,uint64_t expected){
    if(expected!=revision_||revision_==UINT64_MAX)return Error::stale_config;
    if(!valid(member))return Error::invalid_message;
    for(const auto &old:members_)if(old.id==member.id||old.spki_sha256==member.spki_sha256)return Error::identity_conflict;
    if(members_.size()>=capacity)return Error::queue_full;
    auto next=members_;next.push_back(std::move(member));if(!save_||!save_(next,revision_+1))return Error::storage_failure;
    members_=std::move(next);
    ++revision_;
    return Error::none;
}
Error MembershipRoster::revoke(model::OctopusId id,uint64_t expected){
    if(expected!=revision_||revision_==UINT64_MAX)return Error::stale_config;
    if(!lookup(id))return Error::unauthorized_role;
    auto next=members_;next.erase(std::remove_if(next.begin(),next.end(),[&](const auto &c){return c.id==id;}),next.end());
    if(!save_||!save_(next,revision_+1))return Error::storage_failure;
    members_=std::move(next);
    ++revision_;
    return Error::none;
}
bool OperationalCredentialPolicy::accepts(const MemberCredential &record,const VerifiedLeafEvidence &e){
    if(!valid(record)||!e.profile_valid||!e.self_signature_valid||!e.key_possession_verified||e.certificate_identity!=record.id||e.leaf_der!=record.leaf_der||e.spki_sha256!=record.spki_sha256)return false;
    return (e.flags&~uint32_t(certificate_expired|certificate_future))==0;
}
Error check_release(const ReleaseCompatibility &r,uint32_t installed,uint32_t rollback,uint32_t caps){
    if(r.protocol_min>1||r.protocol_max<1||r.protocol_min>r.protocol_max||(r.required_capabilities&~caps))return Error::protocol_mismatch;
    if(!r.image_bytes||r.image_bytes>r.slot_bytes||installed<r.readable_storage_min||installed>r.readable_storage_max||r.writable_storage>rollback)return Error::not_ready;
    if(r.production&&(!r.publisher_signature_verified||!r.native_image_verified))return Error::unauthorized_role;
    return Error::none;
}
} // namespace amity::consortium
