#pragma once
#include "amity/system/model.hpp"
#include <memory>

namespace amity::system {

enum class ObservationKind { native_appliance, proxy_representation };
struct CecObservation {
    FullyQualifiedCecAddress endpoint;
    std::optional<std::string> osd_name;
    BootId owner_boot_id;
    uint64_t local_sequence = 0;
    DomainId domain_id;
    uint64_t domain_revision = 0;
    ObservationKind kind = ObservationKind::native_appliance;
    uint16_t physical_address = 0xffff;
    ScanId scan_id;
    uint64_t received_at_ms = 0; // Receiver's clock only; never the peer clock.
    bool stale = false;
    uint8_t device_type = 0xff;
    uint32_t vendor_id = 0;
};
enum class ScanStatus { queued, probing, collecting, complete, timeout, cancelled, invalidated };
struct DiscoverySnapshot {
    ScanId scan_id;
    uint64_t generation = 0;
    DomainId domain_id;
    uint64_t domain_revision = 0;
    ScanStatus status = ScanStatus::queued;
    std::vector<CecObservation> observations;
    std::vector<std::string> issues;
};
enum class ResolutionStatus { resolved, unresolved, ambiguous, incomplete, stale };
struct NameResolution {
    ResolutionStatus status = ResolutionStatus::unresolved;
    std::optional<FullyQualifiedCecAddress> endpoint;
    std::string issue;
};
NameResolution resolve_native_name(const QualifiedDeviceName&, const DiscoverySnapshot&,
                                   uint64_t receiver_now_ms, uint64_t maximum_age_ms = 30000);

struct ScanRequest {
    ScanId scan_id;
    DomainId domain_id;
    uint64_t domain_revision = 0;
    CecPortRef scanner_port;
    BootId scanner_boot_id;
    uint64_t whole_job_ms = 30000;
    uint64_t quiet_ms = 1100;
    uint64_t collection_ms = 5000;
    std::optional<uint8_t> owned_initiator;
};
enum class ProbeKind { presence_poll, physical_address, osd_name, vendor_id };
struct DiscoveryProbe { ProbeKind kind; CecPortRef port; uint8_t initiator; uint8_t destination; };

// A scheduler advances each job once per owner-loop iteration. No waits or GPIO.
class DiscoveryService {
public:
    explicit DiscoveryService(size_t maximum_jobs = 10, size_t maximum_observations = 128)
        : maximum_jobs_(std::min<size_t>(maximum_jobs, 10)), maximum_observations_(std::min<size_t>(maximum_observations, 128)) {}
    std::optional<ScanId> request_scan(const ScanRequest&, uint64_t receiver_now_ms);
    std::optional<DiscoveryProbe> advance_scan(ScanId, uint64_t receiver_now_ms);
    bool observe(ScanId, const CecObservation&, uint64_t receiver_now_ms);
    void cancel_scan(ScanId);
    void invalidate_domain(DomainId, uint64_t current_revision);
    std::shared_ptr<const DiscoverySnapshot> snapshot(ScanId) const;
private:
    struct Job {
        ScanRequest request;
        DiscoverySnapshot snapshot;
        uint64_t started_at_ms = 0, last_reply_ms = 0, collection_started_ms = 0;
        uint8_t next_address = 0;
        std::vector<DiscoveryProbe> queries;
    };
    Job* find(ScanId);
    const Job* find(ScanId) const;
    size_t maximum_jobs_, maximum_observations_;
    uint64_t generation_ = 0;
    std::vector<Job> jobs_;
};

struct BindingRefresh {
    bool changed = false;
    std::vector<ComponentBinding> proposed_bindings;
    std::string issue;
};
// Fences every capability for the selector before publishing any new endpoint.
// Proposed bindings remain tentative until coordinator/actuator synchronization.
BindingRefresh propose_native_refresh(const QualifiedDeviceName&, const NameResolution&,
    const DiscoverySnapshot&, const std::vector<ComponentBinding>&,
    const std::function<void(BindingId, uint64_t)>& fence_old_binding);

enum class PaProvenance { unknown, immediate_peer_report, legacy_minimum_hint, user_confirmed_fallback };
struct PhysicalAddressEvidence {
    CecPortRef attachment;
    DomainId domain_id;
    uint64_t domain_revision = 0;
    uint16_t physical_address = 0xffff;
    PaProvenance provenance = PaProvenance::unknown;
    std::optional<FullyQualifiedCecAddress> reporter;
    bool confirmed = false;
};
struct PhysicalAddressResolution {
    std::optional<PhysicalAddressEvidence> selected;
    std::optional<PhysicalAddressEvidence> hint;
    std::string issue;
};
PhysicalAddressResolution resolve_physical_address(const CecPortRef&, DomainId, uint64_t,
    const std::vector<PhysicalAddressEvidence>&);

enum class PollOutcome { vacant, occupied, wire_error };
struct IdentityRequest { CecPortRef port; PortRole role; std::optional<PhysicalAddressEvidence> physical_address; };
struct IdentityAllocation { CecPortRef port; PortRole role; uint8_t logical_address; uint16_t physical_address; };
struct AllocationResult { bool accepted = false; std::vector<IdentityAllocation> allocations; std::string issue; };
AllocationResult allocate_domain_identities(DomainId, uint64_t domain_revision, bool domain_confirmed,
    const std::vector<IdentityRequest>&, bool qualified_cec20_backup_addresses,
    const std::function<PollOutcome(const CecPortRef&, uint8_t)>& poll);

} // namespace amity::system
