#include "amity/system/discovery.hpp"
#include <limits>
#include <set>

namespace amity::system {
namespace {
bool terminal(ScanStatus status) {
    return status == ScanStatus::complete || status == ScanStatus::timeout ||
           status == ScanStatus::cancelled || status == ScanStatus::invalidated;
}
bool fresh(const CecObservation& o, const DiscoverySnapshot& s, uint64_t now, uint64_t maximum_age) {
    return o.endpoint.valid() && o.owner_boot_id && !o.stale && o.domain_id == s.domain_id &&
           o.domain_revision == s.domain_revision && o.scan_id == s.scan_id &&
           o.received_at_ms <= now && now - o.received_at_ms <= maximum_age;
}
}
NameResolution resolve_native_name(const QualifiedDeviceName& name, const DiscoverySnapshot& snapshot,
                                   uint64_t now, uint64_t maximum_age) {
    if (!name.valid()) return {ResolutionStatus::unresolved, {}, "invalid qualified name"};
    if (snapshot.status != ScanStatus::complete)
        return {ResolutionStatus::incomplete, {}, "complete bounded discovery required to establish uniqueness"};
    std::set<uint8_t> candidates;
    for (const auto& o : snapshot.observations) {
        if (o.kind != ObservationKind::native_appliance || !fresh(o, snapshot, now, maximum_age) ||
            o.endpoint.port != name.port || !o.osd_name || *o.osd_name != name.osd_name) continue;
        // Same current domain/LA is one identity; contradictory aliases quarantine it.
        for (const auto& alias : snapshot.observations) {
            if (alias.kind == ObservationKind::native_appliance && fresh(alias, snapshot, now, maximum_age) &&
                alias.endpoint.logical_address == o.endpoint.logical_address && alias.osd_name &&
                *alias.osd_name != name.osd_name)
                return {ResolutionStatus::ambiguous, {}, "conflicting native reports at one domain/logical address"};
        }
        candidates.insert(o.endpoint.logical_address);
    }
    if (candidates.empty()) return {ResolutionStatus::unresolved, {}, "no fresh native OSD match on configured attachment"};
    if (candidates.size() != 1) return {ResolutionStatus::ambiguous, {}, "multiple native appliances match the qualified name"};
    return {ResolutionStatus::resolved, FullyQualifiedCecAddress{name.port, *candidates.begin()}, {}};
}
DiscoveryService::Job* DiscoveryService::find(ScanId id) {
    for (auto& j : jobs_) if (j.request.scan_id == id) return &j;
    return nullptr;
}
const DiscoveryService::Job* DiscoveryService::find(ScanId id) const {
    for (const auto& j : jobs_) if (j.request.scan_id == id) return &j;
    return nullptr;
}
std::optional<ScanId> DiscoveryService::request_scan(const ScanRequest& request, uint64_t now) {
    if (!request.scan_id || !request.domain_id || !request.domain_revision || !request.scanner_port.valid() ||
        !request.scanner_boot_id || !request.whole_job_ms || request.whole_job_ms > 30000 ||
        !request.quiet_ms || !request.collection_ms || request.collection_ms > 5000 ||
        (request.owned_initiator && *request.owned_initiator >= 15) || generation_ == std::numeric_limits<uint64_t>::max()) return {};
    for (const auto& j : jobs_) {
        if (j.request.domain_id == request.domain_id && j.request.domain_revision == request.domain_revision &&
            !terminal(j.snapshot.status)) return j.request.scan_id;
        if (j.request.scan_id == request.scan_id) return {};
    }
    if (jobs_.size() == maximum_jobs_) {
        auto done = std::find_if(jobs_.begin(), jobs_.end(), [](const Job& j) { return terminal(j.snapshot.status); });
        if (done == jobs_.end()) return {};
        jobs_.erase(done);
    }
    Job job;
    job.request = request; job.started_at_ms = now; job.last_reply_ms = now;
    job.snapshot = {request.scan_id, ++generation_, request.domain_id, request.domain_revision, ScanStatus::queued, {}, {}};
    jobs_.push_back(std::move(job));
    return request.scan_id;
}
std::optional<DiscoveryProbe> DiscoveryService::advance_scan(ScanId id, uint64_t now) {
    Job* j = find(id); if (!j || terminal(j->snapshot.status)) return {};
    if (generation_ == std::numeric_limits<uint64_t>::max()) { j->snapshot.status = ScanStatus::invalidated; return {}; }
    if (now < j->started_at_ms || now - j->started_at_ms >= j->request.whole_job_ms) {
        j->snapshot.status = ScanStatus::timeout; j->snapshot.issues.push_back("whole-job deadline reached");
        j->snapshot.generation = ++generation_; return {};
    }
    if (j->next_address < 15) {
        j->snapshot.status = ScanStatus::probing;
        auto address = j->next_address++;
        return DiscoveryProbe{ProbeKind::presence_poll, j->request.scanner_port, j->request.owned_initiator.value_or(15), address};
    }
    if (!j->queries.empty()) {
        auto query = j->queries.front(); j->queries.erase(j->queries.begin()); return query;
    }
    if (j->snapshot.status != ScanStatus::collecting) {
        j->snapshot.status = ScanStatus::collecting; j->collection_started_ms = now; j->last_reply_ms = now;
    }
    const uint64_t elapsed = now - j->collection_started_ms;
    if (now - j->last_reply_ms >= j->request.quiet_ms) j->snapshot.status = ScanStatus::complete;
    else if (elapsed >= j->request.collection_ms) {
        j->snapshot.status = ScanStatus::timeout; j->snapshot.issues.push_back("reply collection remained busy");
    }
    j->snapshot.generation = ++generation_;
    return {};
}
bool DiscoveryService::observe(ScanId id, const CecObservation& observation, uint64_t now) {
    Job* j = find(id);
    if (!j || terminal(j->snapshot.status) || !observation.endpoint.valid() || !observation.owner_boot_id ||
        generation_ == std::numeric_limits<uint64_t>::max() || now < j->started_at_ms || now < j->last_reply_ms ||
        observation.domain_id != j->request.domain_id || observation.domain_revision != j->request.domain_revision ||
        observation.scan_id != id || !observation.local_sequence || observation.endpoint.port.consortium_id != j->request.scanner_port.consortium_id ||
        (observation.osd_name && !QualifiedDeviceName{observation.endpoint.port, *observation.osd_name}.valid())) return false;
    if (observation.endpoint.port == j->request.scanner_port && observation.owner_boot_id != j->request.scanner_boot_id) return false;
    auto current = std::find_if(j->snapshot.observations.begin(), j->snapshot.observations.end(), [&](const CecObservation& o) {
        return o.endpoint == observation.endpoint;
    });
    if (current != j->snapshot.observations.end()) {
        if (current->owner_boot_id != observation.owner_boot_id || current->local_sequence >= observation.local_sequence) return false;
        *current = observation; current->received_at_ms = now;
    } else {
        if (j->snapshot.observations.size() >= maximum_observations_) {
            j->snapshot.issues.push_back("observation capacity exhausted");
            j->snapshot.status = ScanStatus::timeout; return false;
        }
        auto copy = observation; copy.received_at_ms = now; j->snapshot.observations.push_back(std::move(copy));
        if (observation.endpoint.port == j->request.scanner_port) {
            j->queries.push_back({ProbeKind::physical_address, j->request.scanner_port,
                j->request.owned_initiator.value_or(15), observation.endpoint.logical_address});
            if (j->request.owned_initiator)
                j->queries.push_back({ProbeKind::osd_name, j->request.scanner_port, *j->request.owned_initiator, observation.endpoint.logical_address});
            j->queries.push_back({ProbeKind::vendor_id, j->request.scanner_port,
                j->request.owned_initiator.value_or(15), observation.endpoint.logical_address});
        }
    }
    j->last_reply_ms = now; j->snapshot.generation = ++generation_;
    return true;
}
void DiscoveryService::cancel_scan(ScanId id) {
    auto* j = find(id); if (j && !terminal(j->snapshot.status)) { j->snapshot.status = ScanStatus::cancelled; j->queries.clear(); if (generation_ != std::numeric_limits<uint64_t>::max()) j->snapshot.generation = ++generation_; }
}
void DiscoveryService::invalidate_domain(DomainId id, uint64_t revision) {
    for (auto& j : jobs_) if (j.request.domain_id == id && j.request.domain_revision != revision && !terminal(j.snapshot.status)) {
        j.snapshot.status = ScanStatus::invalidated; j.queries.clear(); if (generation_ != std::numeric_limits<uint64_t>::max()) j.snapshot.generation = ++generation_;
    }
}
std::shared_ptr<const DiscoverySnapshot> DiscoveryService::snapshot(ScanId id) const {
    const auto* j = find(id); return j ? std::make_shared<const DiscoverySnapshot>(j->snapshot) : nullptr;
}
BindingRefresh propose_native_refresh(const QualifiedDeviceName& name, const NameResolution& resolution,
    const DiscoverySnapshot& snapshot, const std::vector<ComponentBinding>& bindings,
    const std::function<void(BindingId, uint64_t)>& fence) {
    BindingRefresh result;
    if (!fence) { result.issue = "local binding fence is required"; return result; }
    std::vector<ComponentBinding> affected;
    for (const auto& b : bindings) if (b.kind == BindingKind::native_appliance && b.device_name && *b.device_name == name) affected.push_back(b);
    if (affected.empty()) { result.issue = "no native bindings for selector"; return result; }
    if (resolution.status == ResolutionStatus::incomplete) { result.issue = resolution.issue; return result; }
    bool invalid_revision = false;
    for (const auto& b : affected) {
        if (b.domain_id != snapshot.domain_id || b.domain_revision != snapshot.domain_revision ||
            b.binding_revision == std::numeric_limits<uint64_t>::max()) {
            invalid_revision = true;
        }
    }
    if (invalid_revision) {
        for (const auto& b : affected) fence(b.binding_id, b.binding_revision);
        result.issue = "topology revision changed or binding revision exhausted"; return result;
    }
    const bool resolved = resolution.status == ResolutionStatus::resolved && resolution.endpoint && resolution.endpoint->port == name.port;
    bool needs_change = !resolved;
    for (const auto& b : affected) if (resolved && b.endpoint != *resolution.endpoint) needs_change = true;
    if (!needs_change) return result;
    for (const auto& b : affected) fence(b.binding_id, b.binding_revision);
    for (auto b : affected) {
        ++b.binding_revision;
        if (resolved) { b.endpoint = *resolution.endpoint; b.status = BindingStatus::tentative; }
        else b.status = BindingStatus::quarantined;
        result.proposed_bindings.push_back(std::move(b));
    }
    result.changed = true; result.issue = resolved ? "awaiting coordinator/actuator binding synchronization" : resolution.issue;
    return result;
}
PhysicalAddressResolution resolve_physical_address(const CecPortRef& attachment, DomainId domain, uint64_t revision,
    const std::vector<PhysicalAddressEvidence>& evidence) {
    PhysicalAddressResolution result;
    std::vector<PhysicalAddressEvidence> valid;
    for (const auto& e : evidence) if (e.attachment == attachment && e.domain_id == domain && e.domain_revision == revision &&
        valid_physical_address(e.physical_address, false)) valid.push_back(e);
    for (auto provenance : {PaProvenance::immediate_peer_report, PaProvenance::user_confirmed_fallback}) {
        for (const auto& e : valid) if (e.provenance == provenance && e.confirmed &&
            (provenance != PaProvenance::immediate_peer_report || (e.reporter && e.reporter->valid()))) {
            if (result.selected && result.selected->physical_address != e.physical_address) {
                result.selected.reset(); result.issue = "conflicting attachment-specific physical addresses"; return result;
            }
            result.selected = e;
        }
        if (result.selected) return result;
    }
    for (const auto& e : valid) if (e.provenance == PaProvenance::legacy_minimum_hint &&
        (!result.hint || e.physical_address < result.hint->physical_address)) result.hint = e;
    result.issue = result.hint ? "legacy minimum is a hint; confirm the immediate peer or fallback" : "physical address unresolved for this attachment";
    return result;
}
AllocationResult allocate_domain_identities(DomainId domain, uint64_t revision, bool confirmed,
    const std::vector<IdentityRequest>& requests, bool backup,
    const std::function<PollOutcome(const CecPortRef&, uint8_t)>& poll) {
    AllocationResult result;
    if (!domain || !revision || !confirmed || !poll || requests.size() > 10) { result.issue = "confirmed domain and bounded wire polling required"; return result; }
    std::set<uint8_t> allocated;
    bool tv = false;
    for (const auto& request : requests) {
        if (!request.port.valid()) { result.issue = "invalid port"; return result; }
        if (request.role == PortRole::disabled || request.role == PortRole::monitor) continue;
        std::vector<uint8_t> candidates;
        uint16_t pa = 0;
        if (request.role == PortRole::tv_controller) {
            if (tv) { result.issue = "one virtual TV is permitted per domain"; return result; }
            tv = true; candidates = {0};
        } else if (request.role == PortRole::playback_proxy) {
            const auto& e = request.physical_address;
            if (!e || e->attachment != request.port || e->domain_id != domain || e->domain_revision != revision ||
                !e->confirmed || (e->provenance != PaProvenance::immediate_peer_report && e->provenance != PaProvenance::user_confirmed_fallback) ||
                !valid_physical_address(e->physical_address, false)) { result.issue = "playback physical address is unconfirmed or inapplicable"; return result; }
            pa = e->physical_address; candidates = {4, 8, 11}; if (backup) { candidates.push_back(12); candidates.push_back(13); }
        } else { result.issue = "unsupported port role"; return result; }
        bool found = false;
        for (auto la : candidates) {
            if (allocated.count(la)) continue;
            // A wire error is not vacancy. Keep the two independent vacancy polls.
            if (poll(request.port, la) != PollOutcome::vacant || poll(request.port, la) != PollOutcome::vacant) continue;
            allocated.insert(la); result.allocations.push_back({request.port, request.role, la, pa}); found = true; break;
        }
        if (!found) { result.allocations.clear(); result.issue = request.role == PortRole::tv_controller ?
            "TV address 0 is occupied or unproven; no fallback TV identity is permitted" : "playback identity capacity exhausted (standard addresses 4, 8, 11)"; return result; }
    }
    result.accepted = true;
    return result;
}
} // namespace amity::system
