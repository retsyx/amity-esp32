// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "amity/consortium/wire.hpp"
#include <array>
#include <functional>

namespace amity::consortium {
struct LinkScope {
    model::LinkId link_id;
    model::ConsortiumId consortium_id;
    model::OctopusId local_id, peer_id, coordinator_id;
    model::BootId local_boot_id, peer_boot_id;
    uint64_t epoch = 0, config_revision = 0;
    friend bool operator==(const LinkScope &a,const LinkScope &b) {
        return a.link_id==b.link_id&&a.consortium_id==b.consortium_id&&a.local_id==b.local_id&&a.peer_id==b.peer_id&&a.coordinator_id==b.coordinator_id&&a.local_boot_id==b.local_boot_id&&a.peer_boot_id==b.peer_boot_id&&a.epoch==b.epoch&&a.config_revision==b.config_revision;
    }
};
// Issuer-local time only. Scope changes invalidate rather than transplant leases.
template<class LeaseId> class LeaseBook {
public:
    void bind(const LinkScope &scope) {invalidate();scope_=scope;bound_=true;}
    Error issue(LeaseId id,LeaseClass kind,uint32_t duration,uint64_t now) {
        const uint32_t cap=kind==LeaseClass::realtime_input?2000:10000;
        if(!bound_||!id||!duration||duration>cap||now>UINT64_MAX-duration)return Error::invalid_message;
        for(const auto &v:entries_)if(v.active&&v.id==id)return Error::transaction_conflict;
        size_t slot=entries_.size(),oldest=entries_.size(),count=0;
        for(size_t i=0;i<entries_.size();++i){auto &v=entries_[i];if(v.active&&now>=v.deadline)v.active=false;if(!v.active){slot=i;continue;}if(v.kind==kind){++count;if(oldest==entries_.size()||v.issued<entries_[oldest].issued)oldest=i;}}
        if(count>=2)slot=oldest;
        if(slot==entries_.size())return Error::queue_full;
        entries_[slot]={id,kind,now,now+duration,true};return Error::none;
    }
    bool valid(LeaseId id,LeaseClass kind,const LinkScope &scope,uint64_t now) const {
        if(!bound_||!(scope_==scope))return false;
        for(const auto &v:entries_)if(v.active&&v.id==id&&v.kind==kind&&now>=v.issued&&now<v.deadline)return true;
        return false;
    }
    std::optional<uint64_t> deadline(LeaseId id,LeaseClass kind,const LinkScope &scope,uint64_t now)const {
        if(!bound_||!(scope_==scope))return {};
        for(const auto &v:entries_)if(v.active&&v.id==id&&v.kind==kind&&now>=v.issued&&now<v.deadline)return v.deadline;
        return {};
    }
    size_t live(LeaseClass kind,uint64_t now) const {size_t count=0;for(const auto &v:entries_)if(v.active&&v.kind==kind&&now>=v.issued&&now<v.deadline)++count;return count;}
    void invalidate(){for(auto &v:entries_)v.active=false;bound_=false;}
private:
    struct Lease {LeaseId id;LeaseClass kind=LeaseClass::control;uint64_t issued=0,deadline=0;bool active=false;};
    std::array<Lease,4> entries_{};LinkScope scope_;bool bound_=false;
};
enum class ConnectionState { disconnected, authenticating, negotiating, synchronizing, ready, restricted };
enum class PeerRole { coordinator, actuator, provisional_candidate };
class Session {
public:
    // authenticated_peer must come from exact-leaf mTLS, never the Hello body.
    Error authenticate(const LinkScope &, model::OctopusId authenticated_peer,PeerRole,bool active_membership);
    Error hello(const HelloFrame &,uint32_t supported_capabilities);
    Error synchronized(uint64_t applied_config_revision,bool membership_current,bool owner_metadata_current);
    // Called by the owning link task after an authenticated in-place snapshot.
    // Preserve peer/boot/link identity and replay history; require fresh leases.
    Error advance_configuration(uint64_t revision);
    void disconnect();
    void restrict_lifecycle();
    void restrict_management();
    ConnectionState state() const {return state_;}
    const LinkScope &scope() const {return scope_;}
    LeaseBook<model::ExecutionLeaseId> &execution_leases(){return execution_;}
    // Recheck positive work both on receipt and immediately before execution.
    Error admit(const Envelope &,uint64_t received_at_ms,uint64_t now_ms,bool remember_message=true);
    // Validate and remember obsolete in-flight frames without applying them.
    // Success is a discard, never permission to execute or renew old authority.
    Error discard_stale(const Envelope &,uint64_t received_at_ms,uint64_t now_ms,bool remember_message=true);
    uint32_t negotiated_capabilities() const {return capabilities_;}
private:
    Error common(const Envelope &,uint64_t,uint64_t,bool stale=false) const;
    Error typed(const Envelope &,uint64_t,bool leases) const;
    Error replay(const Envelope &,uint64_t,bool);
    Error admit_checked(const Envelope &,uint64_t,uint64_t,bool);
    bool permitted(Kind) const;
    struct Seen {model::MessageId id;uint64_t at=0;bool valid=false;};
    std::array<Seen,64> seen_{};size_t next_seen_=0;
    LinkScope scope_;ConnectionState state_=ConnectionState::disconnected;PeerRole role_=PeerRole::actuator;
    bool active_=false,management_=false;uint32_t capabilities_=0;
    LeaseBook<model::ExecutionLeaseId> execution_;
};
} // namespace amity::consortium
