// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "amity/consortium/wire.hpp"
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
enum class ConnectionState { disconnected, authenticating, negotiating, synchronizing, ready, restricted };
enum class PeerRole { coordinator, actuator, provisional_candidate };
class Session {
public:
    // authenticated_peer must come from exact-leaf mTLS, never the Hello body.
    Error authenticate(const LinkScope &, model::OctopusId authenticated_peer,PeerRole,bool active_membership);
    Error hello(const HelloFrame &,uint32_t supported_capabilities);
    Error synchronized(uint64_t applied_config_revision,bool membership_current,bool owner_metadata_current);
    // Restart configuration exchange on the same authenticated stream. A
    // supplied fresh logical link fences old work without changing TLS or
    // identity. No positive work until synchronized again.
    Error begin_synchronization(uint64_t target_revision,model::LinkId new_link={});
    // Called by the owning link task after an authenticated in-place snapshot.
    // Preserve peer/boot/link identity.
    Error advance_configuration(uint64_t revision);
    void disconnect();
    void restrict_lifecycle();
    void restrict_management();
    ConnectionState state() const {return state_;}
    const LinkScope &scope() const {return scope_;}
    // Recheck positive work both on receipt and immediately before execution.
    Error admit(const Envelope &,uint64_t received_at_ms,uint64_t now_ms);
    // Validate obsolete in-flight frames without applying them.
    // Success is a discard, never permission to execute or renew old authority.
    Error discard_stale(const Envelope &,uint64_t received_at_ms,uint64_t now_ms);
    // During a reset barrier, discard authenticated current/older frames from
    // the abandoned exchange without applying work or granting authority.
    Error discard_synchronization_frame(const Envelope &,uint64_t received_at_ms,uint64_t now_ms);
    // Drain observations/results already on the authenticated stream before
    // lifecycle restriction. This never restores permission to execute work.
    Error discard_lifecycle_frame(const Envelope &,uint64_t received_at_ms,uint64_t now_ms);
    uint32_t negotiated_capabilities() const {return capabilities_;}
private:
    Error common(const Envelope &,uint64_t,uint64_t,bool stale=false,bool synchronization_discard=false) const;
    Error typed(const Envelope &) const;
    bool permitted(Kind,bool discarding=false) const;
    LinkScope scope_;ConnectionState state_=ConnectionState::disconnected;PeerRole role_=PeerRole::actuator;
    bool active_=false,management_=false;uint32_t capabilities_=0;
};
} // namespace amity::consortium
