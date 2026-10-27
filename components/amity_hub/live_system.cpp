#include "amity/live_system.hpp"
#include "coordinator_intent.hpp"
#include <limits>
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include <cstdlib>
#endif

namespace amity {
using namespace system;
#ifdef ESP_PLATFORM
void* LiveSystem::operator new(size_t size) {
    auto* p = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT); if (!p) std::abort(); return p;
}
void LiveSystem::operator delete(void* p) { heap_caps_free(p); }
#endif
LiveSystem::LiveSystem(ConsortiumId group, OctopusId owner, BootId boot, std::function<bool(uint8_t*, size_t)> random, uint64_t epoch)
    : random_(std::move(random)), consortium_(group), octopus_(owner), boot_(boot), epoch_(epoch), coordinator_(random_) {}
void LiveSystem::attach(CecPortRef key, cec::Bus& bus, uint8_t identity, uint16_t physical) {
    if (key.tentacle_id >= amity::max_tentacles || ports_.size() >= amity::max_cec_ports || port(key)) return;
    const auto index = ports_.size();
    ports_.push_back({key, &bus, identity, physical, &wire_generations_[index], {}, {}, {}, false, 0, 0});
    ports_.back().slot_index = index;
    coordinator_.set_wire_adapter(key,
        [this,key](const WireWork* batch,size_t count){auto* p=port(key);return p&&submit_wire(*p,batch,count);},
        [this,key](uint64_t id){if(auto* p=port(key))cancel_wire(*p,id);});
}
std::pair<std::optional<uint16_t>, std::optional<uint16_t>> LiveSystem::front_physical_addresses(uint8_t id) const {
    auto front = std::find_if(ports_.begin(), ports_.end(), [&](const auto& p) { return p.key.tentacle_id == id && p.key.side == Side::front; });
    if (front == ports_.end() || !front->present) return {};
    std::optional<FullyQualifiedCecAddress> reporter;
    for (const auto& p : config_.port_templates) if (p.port == front->key && p.physical_address &&
        p.physical_address->provenance == PaProvenance::immediate_peer_report) reporter = p.physical_address->reporter;
    std::optional<uint16_t> observed;
    size_t reporters = 0;
    for (const auto& back : ports_) if (back.key.tentacle_id == id && back.key.side == Side::back && back.present && back.verified && back.verified->status == ScanStatus::complete)
        for (const auto& observation : back.verified->observations) if (observation.endpoint.port == back.key &&
            observation.kind == ObservationKind::native_appliance && observation.owner_boot_id == boot_ && !observation.stale &&
            observation.device_type != 0xff && valid_physical_address(observation.physical_address, false) &&
            observation.received_at_ms <= last_tick_ms_ && last_tick_ms_ - observation.received_at_ms <= 30000 &&
            (!reporter || observation.endpoint == *reporter)) {
            ++reporters; observed = observation.physical_address;
        }
    if (reporters != 1) observed.reset();
    const auto selected = observed ? observed : valid_physical_address(front->physical) ? std::optional<uint16_t>{front->physical} : std::optional<uint16_t>{};
    return {observed, selected};
}
void LiveSystem::set_tentacle_present(uint8_t id, bool present, uint64_t now) {
    if (id >= amity::max_tentacles) return;
    bool changed = false;
    for (auto& p : ports_) if (p.key.tentacle_id == id && p.present != present) {
        changed = true; p.present = present; ++*p.wire_generation;
        p.bus->install_receive_policy({true, 15}); p.identity = 15;
        if (p.scan) discovery_.cancel_scan(p.scan);
        p.scan = {}; p.verified_scan = {}; p.verified.reset(); p.claiming = false; p.probe.reset(); p.allocation_probe = false;
        p.claim_index = p.vacant_polls = 0; p.next_claim_ms = now;
        for (auto& policy : config_.port_templates) if (policy.port == p.key) {
            policy.owned_logical_address.reset();
            p.claiming = present && (policy.role == PortRole::tv_controller || policy.role == PortRole::playback_proxy);
        }
        observations_.erase(std::remove_if(observations_.begin(), observations_.end(), [&](const auto& o) { return o.endpoint.port == p.key; }), observations_.end());
    }
    if (!changed || !configured_) return;
    std::vector<std::pair<SessionId, uint64_t>> affected;
    for (const auto& policy : coordinator_.policies()) if (policy.port.tentacle_id == id)
        for (const auto& grant : policy.route_grants) affected.emplace_back(grant.session_id, grant.route_generation);
    if (!present) {
        for (size_t i = 0; i < guard_count_; ++i)
            for (const auto& binding : bindings_) if (binding.binding_id == binding_guards_[i].id && binding.endpoint.port.tentacle_id == id) ++binding_guards_[i].generation;
        for (const auto& route : affected) coordinator_.fence_remote_route(route.first, route.second);
    }
    // Binding withdrawal cancels unsafe old-target cleanup. Returning hardware
    // must collect fresh names/addresses and cannot recreate the lost session.
    if (!resolve(now, false, error_)) { error_ = "HDMI attachment synchronization failed: " + error_; resolving_ = true; }
    if (!present) for (const auto& session : coordinator_.snapshot().sessions)
        if (std::any_of(affected.begin(), affected.end(), [&](const auto& route) { return route.first == session.session_id; })) {
            coordinator_.stop(session.zone_id, now);
            if (session.zone_id == config_.primary_zone_id) { selected_slot_ = -1; held_.fill({}); ++activity_generation_; }
        }
    next_discovery_ms_ = now;
}
LiveSystem::Port* LiveSystem::port(const CecPortRef& key) { for (auto& p : ports_) if (p.key == key) return &p; return nullptr; }
bool LiveSystem::preserves_configuration(const LocalConfiguration& config) const {
    return configured_ && config.executable_hash == config_.executable_hash && config.topology.config_revision >= config_.topology.config_revision &&
        config.member == config_.member && config.coordinator_id == config_.coordinator_id && config.coordinator_epoch == config_.coordinator_epoch;
}
bool LiveSystem::revision_rebase_ready(const LocalConfiguration& config) const {
    return preserves_configuration(config) && (config.topology.config_revision == config_.topology.config_revision || coordinator_.can_rebase_revision(config.topology.config_revision));
}
void LiveSystem::fence_revision_work() { for (auto& p : ports_) ++*p.wire_generation; }
bool LiveSystem::configure(LocalConfiguration config, uint64_t now, std::string& error) {
    // A physical selector never survives an intervening configuration edit.
    remote_selection_.reset();
    if (preserves_configuration(config)) {
        if (config.topology.config_revision != config_.topology.config_revision) {
            if (!coordinator_.rebase_revision(config.topology.config_revision)) { error = "in-progress plan must finish before exact-policy revision rebase"; return false; }
            config_.topology.config_revision = config.topology.config_revision;
            for (auto& p : config_.port_templates) p.config_revision = config.topology.config_revision;
        }
        config_.activities = std::move(config.activities); config_.external_slots = std::move(config.external_slots);
        config_.presentation_revision = config.presentation_revision; return true;
    }
    if (!quiescent()) { error = "local holds or wire work have not drained"; return false; }
    configured_ = false; installed_ = false;
    if (config.port_templates.size() > amity::max_cec_ports || config.topology.bindings.size() > 128) {
        error = "configuration references unsupported local port capacity"; return false;
    }
    for (auto& policy : config.port_templates) {
        auto* physical = port(policy.port);
        if (policy.port.consortium_id != consortium_ || policy.port.octopus_id != octopus_ ||
            (!physical && policy.role != PortRole::disabled && policy.role != PortRole::monitor)) { error = "configuration references an unavailable HDMI attachment"; return false; }
        const auto prior = std::find_if(config_.port_templates.begin(), config_.port_templates.end(), [&](const auto& p) {
            return p.port == policy.port && p.role == policy.role && p.domain_id == policy.domain_id && p.domain_revision == policy.domain_revision;
        });
        const bool active = policy.role == PortRole::tv_controller || policy.role == PortRole::playback_proxy;
        const bool retained = physical && prior != config_.port_templates.end() && physical->present &&
            (policy.role == PortRole::tv_controller ? physical->identity == 0 :
             policy.role == PortRole::playback_proxy && (physical->identity == 4 || physical->identity == 8 || physical->identity == 11));
        if (physical) {
            if (!retained) physical->identity = 15;
            physical->claiming = active && physical->present && physical->identity == 15;
            physical->claim_index = physical->vacant_polls = 0; physical->next_claim_ms = now;
        }
        if (physical && active && physical->identity < 15) policy.owned_logical_address = physical->identity;
        else policy.owned_logical_address.reset();
        policy.owner_boot_id = boot_; policy.coordinator_epoch = config.member ? config.coordinator_epoch : epoch_; policy.policy_generation = 1;
        policy.config_revision = config.topology.config_revision;
        // A same-process singleton coordinator owns its lifetime; network-issued
        // policies instead use the authenticated receiver lease at ingress.
        // The freshly authenticated configuration allows bounded identity and
        // discovery only. Actuation still has no route grants; preparation and
        // activation replace this with a receiver lease of at most five seconds.
        policy.authority_expires_at_ms = config.member ? now + 30000 : std::numeric_limits<uint64_t>::max();
    }
    // A new semantic install needs a new discovery deadline and initiator.
    // Otherwise request_scan can coalesce with a paused pre-install job and
    // inherit its elapsed deadline or observations. configure may itself run
    // while maintenance-paused; the first resumed tick must start discovery.
    for (auto& p : ports_) { if (p.scan) discovery_.cancel_scan(p.scan); p.scan = {}; p.verified_scan = {}; p.verified.reset(); }
    next_discovery_ms_ = now;
    config_ = std::move(config); observations_.clear(); bindings_.clear(); requests_.clear(); activity_outcomes_.clear();
    suspended_requests_.clear();
    if (!config_.member) {
        std::vector<config_store::RequestedSession> saved; std::string reason;
        if (config_store::load_last_requested_sessions(consortium_, saved, reason) == ESP_OK)
            for (const auto& request : saved) if (std::any_of(config_.activities.begin(), config_.activities.end(), [&](const auto& a) {
                return a.zone_id == request.zone_id && a.activity_id == request.activity_id;
            })) suspended_requests_.push_back({request.zone_id, request.activity_id});
    }
    guard_count_ = 0;
    for (const auto& b : config_.topology.bindings) { binding_guards_[guard_count_].id = b.binding_id; binding_guards_[guard_count_].revision = 0; ++binding_guards_[guard_count_].generation; ++guard_count_; }
    if (!resolve(now, true, error)) {
        for (auto& p : ports_) { ++*p.wire_generation; p.bus->install_receive_policy({true, 15}); }
        error_ = error; return false;
    }
    for (auto& p : ports_) {
        ++*p.wire_generation;
        p.authority_expired = false;
        auto policy = std::find_if(coordinator_.policies().begin(), coordinator_.policies().end(), [&](const auto& q) { return q.port == p.key; });
        p.bus->install_receive_policy({true, policy != coordinator_.policies().end() && policy->owned_logical_address ? *policy->owned_logical_address : uint8_t{15}});
    }
    configured_ = true; installed_ = true; selected_slot_ = -1; held_.fill({}); ++activity_generation_;
    if (!preflight_) error_.clear();
    discover(now); return true;
}
bool LiveSystem::resolve(uint64_t now, bool initial, std::string& error) {
    preflight_ = false;
    const auto qualified_pa = [](const PortPolicySnapshot& policy) {
        return policy.physical_address && policy.physical_address->confirmed && valid_physical_address(policy.physical_address->physical_address, false) &&
            (policy.physical_address->provenance == PaProvenance::immediate_peer_report || policy.physical_address->provenance == PaProvenance::user_confirmed_fallback);
    };
    for (auto& policy : config_.port_templates) if (policy.role == PortRole::playback_proxy && !qualified_pa(policy)) {
        std::optional<CecObservation> evidence; bool ambiguous = false;
        // The qualified two-sided tentacle carries HDMI/EDID straight through.
        // Its exact configured native peer may report the attachment PA. A
        // minimum across unrelated devices or ports is never attachment proof.
        for (const auto& proxy : config_.topology.bindings) if (proxy.endpoint.port == policy.port && proxy.kind == BindingKind::proxy_representation)
            for (const auto& native : config_.topology.bindings) if (native.component_id == proxy.component_id && native.kind == BindingKind::native_appliance && native.device_name &&
                native.endpoint.port.octopus_id == policy.port.octopus_id && native.endpoint.port.tentacle_id == policy.port.tentacle_id && native.endpoint.port.side == Side::back) {
                auto* physical = port(native.endpoint.port); if (!physical) continue;
                auto scan = physical->verified; if (!scan) continue;
                auto resolved = resolve_native_name(*native.device_name, *scan, now); if (!resolved.endpoint) continue;
                for (const auto& observation : scan->observations) if (observation.endpoint == *resolved.endpoint && valid_physical_address(observation.physical_address, false) &&
                    observation.kind == ObservationKind::native_appliance && observation.owner_boot_id == boot_ && !observation.stale) {
                    if (evidence && evidence->physical_address != observation.physical_address) ambiguous = true;
                    evidence = observation;
                }
            }
        if (!evidence) for (const auto& paired : config_.port_templates) if (policy.port.side == Side::front && paired.port.side == Side::back &&
            paired.port.tentacle_id == policy.port.tentacle_id && paired.port.octopus_id == policy.port.octopus_id && paired.role == PortRole::tv_controller) {
            auto* physical = port(paired.port); if (!physical) continue;
            auto scan = physical->verified; if (!scan || scan->status != ScanStatus::complete) continue;
            // Before component selection, one actual RPA reporter on the
            // approved isolated paired side is attachment evidence. Several
            // reporters are ambiguous even if their numeric PAs happen to match.
            size_t reporters = 0;
            for (const auto& observation : scan->observations) if (observation.kind == ObservationKind::native_appliance &&
                observation.endpoint.port == paired.port && observation.owner_boot_id == boot_ && !observation.stale && observation.device_type != 0xff && valid_physical_address(observation.physical_address, false)) {
                ++reporters; evidence = observation;
            }
            if (reporters != 1) { evidence.reset(); ambiguous = true; }
        }
        if (evidence && !ambiguous) policy.physical_address = PhysicalAddressEvidence{policy.port, policy.domain_id, policy.domain_revision,
            evidence->physical_address, PaProvenance::immediate_peer_report, evidence->endpoint, true};
        if (!qualified_pa(policy)) preflight_ = true;
    }
    auto topology = config_.topology; auto policies = config_.port_templates;
    std::vector<CecPortRef> promoted;
    topology.ready_owners = {octopus_};
    for (auto& p : policies) {
        auto* physical = port(p.port);
        if (!physical || !physical->present ||
            ((p.role == PortRole::tv_controller || p.role == PortRole::playback_proxy) && !p.owned_logical_address) ||
            (p.role == PortRole::playback_proxy && !qualified_pa(p))) { p.role = !physical ? PortRole::disabled : PortRole::monitor; p.owned_logical_address.reset(); }
        if (!initial) for (const auto& installed : coordinator_.policies()) if (installed.port == p.port) p.authority_expires_at_ms = installed.authority_expires_at_ms;
        if (!initial) for (const auto& installed : coordinator_.policies()) if (installed.port == p.port &&
            (installed.role == PortRole::monitor || installed.role == PortRole::disabled) &&
            (p.role == PortRole::playback_proxy || p.role == PortRole::tv_controller)) promoted.push_back(p.port);
        p.bindings.clear();
        for (auto& binding : topology.bindings) if (binding.endpoint.port == p.port) {
            auto prior = std::find_if(bindings_.begin(), bindings_.end(), [&](const auto& b) { return b.binding_id == binding.binding_id; });
            binding.status = BindingStatus::unresolved; binding.binding_revision = prior == bindings_.end() ? 1 : prior->binding_revision;
            if (p.role == PortRole::monitor || p.role == PortRole::disabled) {
                binding.status = BindingStatus::unresolved;
            } else if (binding.kind == BindingKind::proxy_representation && p.owned_logical_address) {
                binding.endpoint.logical_address = *p.owned_logical_address; binding.status = BindingStatus::verified;
            } else if (binding.device_name) {
                if (auto* physical = port(binding.endpoint.port)) if (auto scan = physical->verified) {
                    const auto resolved = resolve_native_name(*binding.device_name, *scan, now);
                    if (resolved.endpoint) {
                        binding.endpoint = *resolved.endpoint; binding.status = BindingStatus::verified;
                        if (binding.capability == Capability::routing) {
                            const auto evidence = std::find_if(scan->observations.begin(), scan->observations.end(), [&](const auto& o) { return o.endpoint == binding.endpoint && o.kind == ObservationKind::native_appliance; });
                            if (evidence == scan->observations.end() || !valid_physical_address(evidence->physical_address, false)) binding.status = BindingStatus::unresolved;
                            else for (const auto& route : topology.routes) for (const auto& step : route.steps)
                                if (step.binding_id == binding.binding_id && step.operation == OperationKind::select_path && step.arguments.physical_path != evidence->physical_address) {
                                    binding.status = BindingStatus::quarantined; error_ = "native physical path changed; explicit route revalidation required";
                                }
                        }
                    }
                }
            }
            if (prior != bindings_.end() && (prior->endpoint != binding.endpoint || prior->status != binding.status)) {
                ++binding.binding_revision;
                for (size_t i = 0; i < guard_count_; ++i) if (binding_guards_[i].id == binding.binding_id) ++binding_guards_[i].generation;
                if (auto* physical = port(binding.endpoint.port)) ++*physical->wire_generation;
            }
            for (size_t i = 0; i < guard_count_; ++i) if (binding_guards_[i].id == binding.binding_id) binding_guards_[i].revision = binding.binding_revision;
            if (binding.status == BindingStatus::verified) {
                auto permission = std::find_if(config_.port_templates.begin(), config_.port_templates.end(), [&](const auto& q) { return q.port == p.port; });
                for (const auto& original : permission->bindings) if (original.binding.binding_id == binding.binding_id) {
                    auto allowed = original; allowed.binding = binding; p.bindings.push_back(std::move(allowed));
                }
            }
        }
    }
    auto result = initial ? coordinator_.install(topology, policies, octopus_, now) : coordinator_.refresh_bindings(topology, policies, now);
    if (!result) { error = result.reason; return false; }
    bindings_ = std::move(topology.bindings);
    if (preflight_) error_ = "waiting for qualified attachment physical-address evidence";
    else if (error_ == "waiting for qualified attachment physical-address evidence") error_.clear();
    for (auto key : promoted) if (auto* physical = port(key)) {
        ++*physical->wire_generation;
        for (const auto& p : coordinator_.policies()) if (p.port == key) {
            physical->bus->install_receive_policy({true, p.owned_logical_address.value_or(15)});
            if (physical->scan) discovery_.cancel_scan(physical->scan);
        }
    }
    if (!promoted.empty()) discover(now);
    return true;
}
void LiveSystem::discover(uint64_t now) {
    if (paused_) return;
    next_discovery_ms_ = now + 20000;
    // One job owns each electrical domain. Prefer a freshly claimed initiator
    // over a monitor so directed OSD queries use a legal source address, and
    // remember the actual physical scanner when several ports share the job.
    for (bool owned : {true, false}) for (auto& p : ports_) {
        if (!p.present) continue;
        auto policy = std::find_if(coordinator_.policies().begin(), coordinator_.policies().end(), [&](const auto& q) { return q.port == p.key; });
        if (policy == coordinator_.policies().end() || policy->role == PortRole::disabled || p.authority_expired || policy->authority_expires_at_ms <= now) continue;
        if (policy->owned_logical_address.has_value() != owned) continue;
        ScanRequest request{fresh<ScanId>(), policy->domain_id, policy->domain_revision, p.key, boot_, 30000, 1100, 5000, policy->owned_logical_address};
        if (auto scan = discovery_.request_scan(request, now)) {
            p.scan_driver = *scan == request.scan_id || (*scan == p.scan && p.scan_driver);
            p.scan = *scan;
        }
    }
}
AdmissionResult LiveSystem::discover_authorized(uint32_t ttl, uint64_t now) {
    if (!configured_ || !config_.member || paused_ || !ttl || ttl > 5000)
        return {AdmissionCode::unauthorized, "current authenticated member discovery authority required"};
    bool identities_ready = true;
    for (auto& p : ports_) identities_ready = identity_available(p) && identities_ready;
    if (!identities_ready) return {AdmissionCode::overloaded, "expired CEC identity validation is pending"};
    auto result = coordinator_.renew_discovery_authority(ttl, now); if (!result) return result;
    for (auto& p : ports_) { p.authority_expired = false; p.bus->install_receive_policy({true, p.identity}); }
    discover(now); return {AdmissionCode::accepted, {}};
}
void LiveSystem::observe(Port& p, const cec::ReceiveObservation& received, uint64_t now) {
    const auto& frame = received.frame;
    if (!frame.size || frame.source() >= 15) return;
    for (const auto& policy : coordinator_.policies()) if (policy.port == p.key && policy.owned_logical_address == frame.source()) return;
    auto policy = std::find_if(config_.port_templates.begin(), config_.port_templates.end(), [&](const auto& q) { return q.port == p.key; });
    if (policy == config_.port_templates.end()) return;
    auto prior = std::find_if(observations_.begin(), observations_.end(), [&](const auto& o) { return o.endpoint.port == p.key && o.endpoint.logical_address == frame.source(); });
    // A presence observation from this scan may retain only metadata actually
    // received in this scan. Never promote a cached name to fresh evidence.
    CecObservation observation = prior == observations_.end() || prior->scan_id != p.scan ? CecObservation{} : *prior;
    observation.endpoint = {p.key, frame.source()}; observation.owner_boot_id = boot_; observation.local_sequence = ++p.sequence;
    observation.domain_id = policy->domain_id; observation.domain_revision = policy->domain_revision; observation.scan_id = p.scan; observation.received_at_ms = now;
    auto verified = p.verified;
    const CecObservation* metadata = prior == observations_.end() ? nullptr : &*prior;
    if (verified) for (const auto& cached : verified->observations) if (cached.endpoint == observation.endpoint) { metadata = &cached; break; }
    bool changed = false;
    if (frame.size >= 5 && frame.data[1] == 0x84) {
        auto address = uint16_t(frame.data[2] << 8 | frame.data[3]);
        changed = metadata && metadata->physical_address != 0xffff && metadata->physical_address != address;
        observation.physical_address = address; observation.device_type = frame.data[4];
    }
    if (frame.size >= 5 && frame.data[1] == 0x87) observation.vendor_id = uint32_t(frame.data[2]) << 16 | uint32_t(frame.data[3]) << 8 | frame.data[4];
    if (frame.size >= 3 && frame.data[1] == 0x47) {
        std::string name(reinterpret_cast<const char*>(&frame.data[2]), frame.size - 2);
        const bool valid = QualifiedDeviceName{p.key, name}.valid();
        changed = changed || (metadata && metadata->osd_name && (!valid || *metadata->osd_name != name));
        if (valid && verified) for (const auto& cached : verified->observations)
            if (cached.endpoint.port == observation.endpoint.port && cached.endpoint != observation.endpoint && cached.osd_name && *cached.osd_name == name) changed = true;
        if (valid) observation.osd_name = std::move(name); else observation.osd_name.reset();
    }
    if (changed) { p.verified_scan = {}; p.verified.reset(); discover(now); observation.scan_id = p.scan; resolving_ = true; }
    if (prior == observations_.end()) { if (observations_.size() < 160) observations_.push_back(observation); } else *prior = observation;
    discovery_.observe(p.scan, observation, now);
    if (changed) { resolving_ = !resolve(now, false, error_); if (resolving_) error_ = "binding synchronization failed: " + error_; }
    if (frame.size > 1) coordinator_.observe({p.key, frame.source(), frame.destination(), {frame.data + 1, frame.data + frame.size}}, now);
}
bool LiveSystem::submit_wire(Port& p, const WireWork* work, size_t count) {
    if (!p.present || !work || !count || count > p.submitted.size()) return false;
    std::array<size_t,10> slots{};size_t available=0;
    for(size_t i=0;i<p.submitted.size()&&available<count;++i)
        if(!p.submitted[i].active&&work_generations_[p.slot_index][i].load()!=UINT32_MAX)slots[available++]=i;
    if(available!=count)return false;
    std::array<cec::Submission,10> batch{};
    for(size_t i=0;i<count;++i){
        const auto& item=work[i];
        if(!item.work_id||item.frame.port!=p.key||item.frame.payload.empty()||item.frame.payload.size()>15)return false;
        auto& target=batch[i];target.frame.size=static_cast<uint8_t>(1+item.frame.payload.size());
        target.frame.data[0]=item.frame.initiator<<4|item.frame.destination;
        std::copy(item.frame.payload.begin(),item.frame.payload.end(),target.frame.data+1);
        target.fence={p.wire_generation,p.wire_generation->load()};
        if(item.emergency_release){
            bool found=false;
            for(size_t j=0;j<guard_count_;++j)if(binding_guards_[j].id==item.binding_id&&binding_guards_[j].revision==item.binding_revision){
                target.fence={&binding_guards_[j].generation,binding_guards_[j].generation.load()};found=true;break;
            }
            if(!found)return false;
        }else{
            auto expiry=item.expires_at_ms;
            for(const auto& binding:bindings_)if(binding.binding_id==item.binding_id&&binding.kind==BindingKind::native_appliance&&p.verified)
                for(const auto& observation:p.verified->observations)if(observation.endpoint==binding.endpoint&&observation.osd_name&&binding.device_name&&*observation.osd_name==binding.device_name->osd_name){
                    const auto evidence_expiry=observation.received_at_ms+30001;
                    expiry=expiry?std::min(expiry,evidence_expiry):evidence_expiry;
                }
            if(expiry&&expiry<=UINT64_MAX/1000)target.fence.expires_at_us=expiry*1000;
        }
        auto& generation=work_generations_[p.slot_index][slots[i]];
        target.fence.secondary_generation=&generation;target.fence.expected_secondary_generation=++generation;
        target.operation_token=next_token_++;if(!target.operation_token)target.operation_token=next_token_++;
        target.attempts=item.attempts;target.requires_previous_start=item.requires_previous_start;
    }
    if(!p.bus->submit_guarded_batch(batch.data(),count))return false;
    for(size_t i=0;i<count;++i)p.submitted[slots[i]]={work[i].work_id,batch[i].operation_token,true};
    return true;
}
void LiveSystem::cancel_wire(Port& p,uint64_t id){
    for(size_t i=0;i<p.submitted.size();++i)if(p.submitted[i].active&&p.submitted[i].work_id==id)
        ++work_generations_[p.slot_index][i];
}
bool LiveSystem::identity_available(Port& p) {
    if (!p.authority_expired || p.identity >= 15 || p.reclaim_ready) return true;
    p.reclaim_requested = true;
    return false;
}
void LiveSystem::service_port(Port& p, uint64_t now) {
    cec::ReceiveObservation received;
    for (unsigned i = 0; i < 32 && p.bus->receive_observation(received); ++i) if (!paused_ && p.present) observe(p, received, now);
    cec::Completion completion;
    // A completion may immediately admit the next held press. Bound this
    // pass to the driver's capacity so a fast completion producer cannot keep
    // the owner from processing another key or physical wire.
    for (unsigned i = 0; i < 8 && p.bus->take_completion(completion); ++i) {
        auto submitted=std::find_if(p.submitted.begin(),p.submitted.end(),[&](const auto& item){return item.active&&item.token==completion.operation_token;});
        if(submitted!=p.submitted.end()){
            const auto id=submitted->work_id;submitted->active=false;
            const auto result=completion.result==cec::TxResult::Success?WireOutcome::success:completion.result==cec::TxResult::Nack?WireOutcome::nack:
                completion.result==cec::TxResult::ArbitrationLost?WireOutcome::arbitration_lost:completion.result==cec::TxResult::Cancelled?WireOutcome::canceled:
                completion.result==cec::TxResult::Expired?WireOutcome::expired:WireOutcome::line_error;
            coordinator_.complete_frame(p.key,id,result,now,completion.started);continue;
        }
        if(completion.operation_token!=p.token)continue;
        p.wire = false;
        if (p.reclaim_poll) {
            p.reclaim_poll = false;
            if (completion.result == cec::TxResult::Nack) {
                if (++p.reclaim_vacant_polls == 2) { p.reclaim_ready = true; p.reclaim_requested = false; }
            } else { p.reclaim_vacant_polls = 0; p.reclaim_requested = false; }
        } else if (p.allocation_probe) {
            p.allocation_probe = false;
            if (p.claiming && p.present) {
                if (completion.result == cec::TxResult::Nack) {
                    if (++p.vacant_polls == 2) {
                        p.identity = p.claim_address; p.claiming = false;
                        for (auto& policy : config_.port_templates) if (policy.port == p.key) policy.owned_logical_address = p.identity;
                        if (p.scan) discovery_.cancel_scan(p.scan);
                        p.scan = {}; p.verified_scan = {}; p.verified.reset(); resolving_ = true;
                    }
                } else { p.vacant_polls = 0; ++p.claim_index; }
            }
        } else if (p.probe) {
            if (p.probe->kind == ProbeKind::presence_poll && completion.result == cec::TxResult::Success && p.probe->destination != p.identity) {
                cec::ReceiveObservation poll; poll.frame = {{static_cast<uint8_t>(p.probe->destination << 4 | p.identity)}, 1}; observe(p, poll, now);
            }
            p.probe.reset();
        }
    }
    if (p.wire) return;
    if (!p.present) return;
    if (p.reclaim_requested && !paused_) {
        const cec::Frame poll{{static_cast<uint8_t>((p.identity << 4) | p.identity)}, 1};
        p.token = next_token_++; if (!p.token) p.token = next_token_++;
        p.reclaim_poll = p.bus->submit_guarded(poll, {p.wire_generation, p.wire_generation->load(), nullptr, 0, (now + 1500) * 1000}, p.token, 1);
        p.wire = p.reclaim_poll;
        if (!p.wire) { p.reclaim_requested = false; p.reclaim_vacant_polls = 0; }
        return;
    }
    if (p.claiming && !paused_ && now >= p.next_claim_ms) {
        const auto desired = std::find_if(config_.port_templates.begin(), config_.port_templates.end(), [&](const auto& policy) { return policy.port == p.key; });
        if (desired != config_.port_templates.end()) {
            // Reserve identity allocation order within one electrical domain;
            // independent domains advance concurrently without blocking waits.
            for (const auto& other : ports_) if (&other != &p && other.claiming && other.present &&
                ((other.allocation_probe && other.wire) || (&other < &p && now >= other.next_claim_ms)))
                for (const auto& policy : config_.port_templates) if (policy.port == other.key && policy.domain_id == desired->domain_id) return;
            const uint8_t candidates[] = {4, 8, 11};
            const uint8_t count = desired->role == PortRole::tv_controller ? 1 : 3;
            while (p.claim_index < count) {
                const uint8_t candidate = desired->role == PortRole::tv_controller ? 0 : candidates[p.claim_index];
                const bool reserved = std::any_of(config_.port_templates.begin(), config_.port_templates.end(), [&](const auto& policy) {
                    auto* other = port(policy.port);
                    return policy.port != p.key && policy.domain_id == desired->domain_id && other && other->present && other->identity == candidate;
                });
                if (reserved) { ++p.claim_index; p.vacant_polls = 0; continue; }
                p.claim_address = candidate; p.allocation_probe = true;
                const cec::Frame poll{{static_cast<uint8_t>(candidate << 4 | candidate)}, 1};
                p.token = next_token_++; if (!p.token) p.token = next_token_++;
                p.wire = p.bus->submit_guarded(poll, {p.wire_generation, p.wire_generation->load(), nullptr, 0, (now + 1500) * 1000}, p.token, 1);
                if (!p.wire) { p.allocation_probe = false; p.vacant_polls = 0; ++p.claim_index; }
                return;
            }
            p.claim_index = p.vacant_polls = 0; p.next_claim_ms = now + 5000;
        }
    }
    if (p.scan && !paused_) {
        auto before = discovery_.snapshot(p.scan);
        if (auto probe = p.scan_driver ? discovery_.advance_scan(p.scan, now) : std::optional<DiscoveryProbe>{}) {
            if (probe->destination == p.identity) return;
            cec::Frame frame{{static_cast<uint8_t>(probe->initiator << 4 | probe->destination)}, 1};
            if (probe->kind != ProbeKind::presence_poll) { frame.data[1] = probe->kind == ProbeKind::physical_address ? 0x83 : probe->kind == ProbeKind::vendor_id ? 0x8c : 0x46; frame.size = 2; }
            p.token = next_token_++; p.probe = *probe;
            p.wire = p.bus->submit_guarded(frame, {p.wire_generation, p.wire_generation->load(), nullptr, 0, (now + 1500) * 1000}, p.token, probe->kind == ProbeKind::presence_poll ? 2 : 1);
            if (!p.wire) p.probe.reset();
        }
        auto after = discovery_.snapshot(p.scan);
        if (before && after && (before->status != after->status || (after->status == ScanStatus::complete && p.verified_scan != p.scan))) {
            if (after->status == ScanStatus::complete) { p.verified_scan = p.scan; p.verified = after; resolving_ = true; ++discovery_generation_; }
            else if (after->status == ScanStatus::timeout) resolving_ = true;
        }
    }
}
void LiveSystem::tick(uint64_t now) {
    if (!configured_) return;
    const auto previous_tick = last_tick_ms_;
    last_tick_ms_ = now;
    // A replacement scan may still be probing a noisy line when old evidence
    // expires. Withdraw native bindings at the actual age boundary instead of
    // waiting for that job's completion; no physical presence is inferred.
    bool evidence_expired = false;
    for (const auto& p : ports_) if (p.verified)
        for (const auto& observation : p.verified->observations)
            if (observation.received_at_ms <= now && now - observation.received_at_ms > 30000 &&
                (observation.received_at_ms > previous_tick || previous_tick - observation.received_at_ms <= 30000)) evidence_expired = true;
    if (evidence_expired && !resolve(now, false, error_)) error_ = "binding synchronization failed: " + error_;
    if (config_.member) for (auto& p : ports_) {
        auto policy = std::find_if(coordinator_.policies().begin(), coordinator_.policies().end(), [&](const auto& q) { return q.port == p.key; });
        if (policy != coordinator_.policies().end() && policy->authority_expires_at_ms <= now) {
            if (!p.authority_expired) {
                ++*p.wire_generation; p.bus->install_receive_policy({true, 15}); p.authority_expired = true;
                p.reclaim_requested = p.reclaim_poll = p.reclaim_ready = false; p.reclaim_vacant_polls = 0;
            }
            if (p.scan) discovery_.cancel_scan(p.scan);
        }
    }
    // Refresh real presence/identity evidence before its thirty-second expiry.
    // Each complete unchanged scan retains binding/route generations; queries
    // neither wake appliances nor recreate an activity. Jobs are bounded and
    // terminal DiscoveryService records are evicted before allocating another.
    if (!paused_ && now >= next_discovery_ms_) discover(now);
    for (auto& p : ports_) service_port(p, now);
    if (resolving_) { resolving_ = !resolve(now, false, error_); if (resolving_) error_ = "binding synchronization failed: " + error_; }
    coordinator_.tick(now);
    if (remote_selection_) advance_remote_selection(now);
    const auto state = coordinator_.snapshot();
    for (auto& outcome : activity_outcomes_) if (outcome.stage == "admitted" && outcome.plan_id && (!state.executing_plan || *state.executing_plan != *outcome.plan_id)) {
        const auto applied = std::find_if(state.sessions.begin(), state.sessions.end(), [&](const auto& s) { return s.plan_id == *outcome.plan_id; });
        outcome.stage = applied == state.sessions.end() ? "failed" : applied->status == SessionStatus::active ? "applied" : "degraded";
        if (outcome.stage != "applied") outcome.error = state.error;
    }
}
bool LiveSystem::select(ZoneId zone, ActivityId activity, MessageId request, std::optional<uint64_t> expected, uint64_t now, std::string& error, uint64_t deadline) {
    remote_selection_.reset();
    if (!configured_) { error = "schema2 configuration is unavailable"; return false; }
    auto prior = std::find_if(requests_.begin(), requests_.end(), [&](const auto& r) { return r.request_id == request; });
    SelectRequest intent;
    if (prior != requests_.end()) intent = *prior;
    else {
        const auto state = coordinator_.snapshot();
        if (requests_.size() >= 64) {
            auto retired = std::find_if(requests_.begin(), requests_.end(), [&](const auto& cached) {
                return cached.expected_state_revision && *cached.expected_state_revision < state.state_revision &&
                    (!state.executing_plan || *state.executing_plan != cached.plan_id) &&
                    std::none_of(state.sessions.begin(), state.sessions.end(), [&](const auto& s) { return s.plan_id == cached.plan_id; });
            });
            if (retired != requests_.end()) {
                const auto id = retired->request_id;
                activity_outcomes_.erase(std::remove_if(activity_outcomes_.begin(), activity_outcomes_.end(), [&](const auto& o) { return o.request_id == id; }), activity_outcomes_.end());
                requests_.erase(retired);
            }
        }
        if (requests_.size() >= 64) { error = "intent result capacity exhausted"; return false; }
        intent = {request, zone, activity, fresh<SessionId>(), fresh<PlanId>(), expected.value_or(state.state_revision), deadline}; requests_.push_back(intent);
    }
    if (intent.zone_id != zone || intent.activity_id != activity || (expected && intent.expected_state_revision != expected)) { error = "request identifier reused with different intent"; return false; }
    auto result = coordinator_.select(intent, now);
    auto outcome = std::find_if(activity_outcomes_.begin(), activity_outcomes_.end(), [&](const auto& o) { return o.request_id == request; });
    if (outcome == activity_outcomes_.end()) { activity_outcomes_.push_back({request, {}, {}, {}}); outcome = std::prev(activity_outcomes_.end()); }
    if (!result.plan) { error = result.reason; error_ = error; outcome->stage = "rejected"; outcome->error = error; return false; }
    if (result.status != PlanningStatus::duplicate) { outcome->plan_id = result.plan->plan_id; outcome->stage = "admitted"; outcome->error.clear(); }
    if (result.status == PlanningStatus::duplicate) return true;
    std::string persistence_error;
    if (config_store::remember_requested_session(consortium_, zone, activity, persistence_error) != ESP_OK) {
        coordinator_.quiesce(); error = "requested activity could not be persisted: " + persistence_error; error_ = error;
        outcome->stage = "rejected"; outcome->error = error; return false;
    }
    suspended_requests_.erase(std::remove_if(suspended_requests_.begin(), suspended_requests_.end(), [&](const auto& request) { return request.zone_id == zone; }), suspended_requests_.end());
    if (zone == config_.primary_zone_id) {
        selected_slot_ = -1; for (const auto& slot : config_.external_slots) if (!slot.tombstone && slot.activity_id == activity) selected_slot_ = slot.slot;
        ++activity_generation_; held_.fill({});
    }
    error_.clear(); return true;
}
bool LiveSystem::select_slot(int slot, uint64_t now, std::string& error) {
    if (slot < 0) return stop(config_.primary_zone_id, now);
    for (const auto& s : config_.external_slots) if (s.slot == slot && !s.tombstone && !s.hidden && std::any_of(config_.activities.begin(), config_.activities.end(), [&](const auto& activity) { return activity.activity_id == s.activity_id && activity.zone_id == config_.primary_zone_id; }))
        return select(config_.primary_zone_id, s.activity_id, fresh<MessageId>(), {}, now, error);
    error = "selector slot is unavailable"; return false;
}
bool LiveSystem::stop(ZoneId zone, uint64_t now) {
    if (zone == config_.primary_zone_id) release(now);
    const bool ok = coordinator_.stop(zone, now);
    if (ok) {
        suspended_requests_.erase(std::remove_if(suspended_requests_.begin(), suspended_requests_.end(), [&](const auto& request) { return request.zone_id == zone; }), suspended_requests_.end());
        std::string error;
        if (config_store::remember_requested_session(consortium_, zone, {}, error) != ESP_OK) { error_ = "stopped state could not be persisted: " + error; return false; }
        if (zone == config_.primary_zone_id) { selected_slot_ = -1; ++activity_generation_; }
    }
    return ok;
}
bool LiveSystem::key(uint8_t key, bool repeat, uint64_t now) {
    if (!configured_) return false;
    if (key == 0x40) return stop(config_.primary_zone_id, now);
    if (repeat && held_[key]) return true;
    const auto session = coordinator_.input_session(config_.primary_zone_id);
    if (!session) return false;
    auto activity = std::find_if(config_.activities.begin(), config_.activities.end(), [&](const auto& a) { return a.activity_id == session->activity_id; });
    if (activity == config_.activities.end()) return false;
    auto target = ((key >= 0x41 && key <= 0x43) || key == 0x65 || key == 0x66) ? activity->audio : activity->source;
    if (!target) return false;
    auto token = fresh<HoldToken>(); auto result = coordinator_.key(activity->zone_id, *target, key, repeat, token, now);
    if (result && repeat) held_[key] = token;
    return bool(result);
}
bool LiveSystem::release_key(uint8_t key, uint64_t now) {
    if (!held_[key]) return true;
    const auto token = *held_[key]; held_[key].reset();
    return coordinator_.release(token, now ? now : last_tick_ms_);
}
bool LiveSystem::release(uint64_t now) {
    bool accepted = true;
    for (size_t key = 0; key < held_.size(); ++key)
        accepted = release_key(static_cast<uint8_t>(key), now) && accepted;
    return accepted;
}
AdmissionResult LiveSystem::advance_remote_selection(uint64_t now) {
    if (!remote_selection_) return {AdmissionCode::stale, "selector is no longer pending"};
    auto reject = [&](const char* reason) {
        remote_selection_.reset(); error_ = reason;
        return AdmissionResult{AdmissionCode::stale, reason};
    };
    auto& pending = *remote_selection_;
    if (pending.config_revision != config_.topology.config_revision || pending.presentation_revision != config_.presentation_revision)
        return reject("activity selection context changed before admission");
    auto state = coordinator_.snapshot();
    for (const auto& current : state.sessions)
        if (std::none_of(pending.original_sessions.begin(),pending.original_sessions.end(),[&](const auto& prior) {
            return prior.session_id == current.session_id && prior.route_generation == current.route_generation &&
                prior.activity_id == current.activity_id && prior.zone_id == current.zone_id;
        })) return reject("activity selection was superseded");
    // Route fences are asynchronous state transitions, not a queue of CEC
    // frames. Leave the owner free to admit all unrelated input and wire work.
    if (state.executing_plan) return {AdmissionCode::accepted, {}};
    for (const auto& session : state.sessions) if (session.zone_id != pending.intent.zone_id) {
        if (std::find(pending.stopping.begin(), pending.stopping.end(), session.zone_id) == pending.stopping.end()) {
            if (!stop(session.zone_id, now)) return reject("previous screen could not stop");
            pending.stopping.push_back(session.zone_id);
        }
    }
    state = coordinator_.snapshot();
    if (state.executing_plan || std::any_of(state.sessions.begin(), state.sessions.end(), [&](const auto& session) { return session.zone_id != pending.intent.zone_id; }))
        return {AdmissionCode::accepted, {}};
    const auto intent = pending.intent;
    remote_selection_.reset();
    std::string error;
    const bool accepted = select(intent.zone_id, intent.activity_id, intent.request_id, state.state_revision, now, error, intent.admissible_until_ms);
    return {accepted ? AdmissionCode::accepted : AdmissionCode::stale, error};
}
AdmissionResult LiveSystem::input(const InputIntent& intent, uint64_t now) {
    if (!configured_ || config_.member) return {AdmissionCode::unauthorized, "input needs local coordinator authority"};
    if (intent.action == InputAction::hold_release) return {coordinator_.release(intent.hold_token, now) ? AdmissionCode::accepted : AdmissionCode::stale, "hold is no longer active"};
    if (intent.admissible_until_ms && now >= intent.admissible_until_ms) return {AdmissionCode::expired, "original input authority expired before owner admission"};
    const auto state = coordinator_.snapshot();
    if (intent.expected_state_revision != state.state_revision) return {AdmissionCode::stale, "input context changed"};
    if (intent.action == InputAction::select_remote_activity) {
        if (remote_selection_ || state.executing_plan) return {AdmissionCode::overloaded, "an activity transition is already pending"};
        if (std::none_of(config_.activities.begin(),config_.activities.end(),[&](const auto& activity) { return activity.activity_id == intent.activity_id && activity.zone_id == intent.zone_id; }))
            return {AdmissionCode::invalid, "selected activity is unavailable"};
        auto admitted = intent;
        // IPC freshness applies before owner admission. Accepted selection
        // inherits the normal planner/authority fences, without a new CEC cap.
        admitted.admissible_until_ms = 0;
        remote_selection_ = RemoteSelection{admitted, state.sessions, {}, config_.topology.config_revision, config_.presentation_revision};
        return advance_remote_selection(now);
    }
    if (intent.action == InputAction::select_activity) {
        std::string error; const bool accepted = select(intent.zone_id, intent.activity_id, intent.request_id, intent.expected_state_revision, now, error);
        return {accepted ? AdmissionCode::accepted : AdmissionCode::stale, error};
    }
    if (intent.action == InputAction::stop_zone) {
        remote_selection_.reset();
        return {stop(intent.zone_id, now) ? AdmissionCode::accepted : AdmissionCode::stale, "zone transition unavailable"};
    }
    const auto session = coordinator_.input_session(intent.zone_id);
    if (!session) return {AdmissionCode::stale, "zone has no active input route"};
    auto activity = std::find_if(config_.activities.begin(), config_.activities.end(), [&](const auto& a) { return a.activity_id == session->activity_id; });
    if (activity == config_.activities.end()) return {AdmissionCode::stale, "activity was removed"};
    const auto target = ((intent.key >= 0x41 && intent.key <= 0x43) || intent.key == 0x65 || intent.key == 0x66) ? activity->audio : activity->source;
    if (!target || (intent.component_id && *intent.component_id != *target)) return {AdmissionCode::unauthorized, "key component is outside the current route"};
    return coordinator_.key(intent.zone_id, *target, intent.key, intent.repeat, intent.hold_token, now, intent.admissible_until_ms);
}
void LiveSystem::quiesce() { remote_selection_.reset(); release(); for (auto& counter : wire_generations_) ++counter; coordinator_.quiesce(); }
void LiveSystem::fault(const std::string& error) { quiesce(); for (auto& p : ports_) p.bus->install_receive_policy({true, 15}); configured_ = false; error_ = error; }
AdmissionResult LiveSystem::execute_authorized(const AuthorizedCecOperation& operation, uint64_t now) {
    if (!configured_) return {AdmissionCode::unauthorized, "schema2 configuration is unavailable"};
    auto* physical = port(operation.target.port);
    if (config_.member && physical && operation.kind != OperationKind::release_key && !identity_available(*physical))
        return {AdmissionCode::overloaded, "expired CEC identity validation is pending"};
    auto result = coordinator_.execute_authorized(operation, now);
    if (result && config_.member && physical && operation.kind != OperationKind::release_key) {
        physical->authority_expired = false; physical->bus->install_receive_policy({true, physical->identity});
    }
    return result;
}
AdmissionResult LiveSystem::prepare_remote_plan(const Plan& plan, const std::vector<PortPolicySnapshot>& policies, uint64_t now) {
    if (!configured_ || !config_.member) return {AdmissionCode::unauthorized, "remote preparation requires an authenticated member slice"};
    auto executable = policies;
    for (auto& policy : executable) policy.bindings.erase(std::remove_if(policy.bindings.begin(), policy.bindings.end(), [](const auto& permission) {
        return permission.binding.status != BindingStatus::verified;
    }), policy.bindings.end());
    return coordinator_.prepare_remote_plan(plan, executable, now);
}
AdmissionResult LiveSystem::fence_remote_route(SessionId session, uint64_t generation) {
    if (!configured_ || !config_.member) return {AdmissionCode::unauthorized, "remote fencing requires a member slice"};
    for (auto& p : ports_) for (const auto& policy : coordinator_.policies()) if (policy.port == p.key &&
        std::any_of(policy.route_grants.begin(), policy.route_grants.end(), [&](const auto& grant) { return grant.session_id == session && grant.route_generation <= generation; })) ++*p.wire_generation;
    return coordinator_.fence_remote_route(session, generation);
}
bool LiveSystem::remote_route_quiescent(SessionId session, uint64_t generation) const { return coordinator_.remote_route_quiescent(session, generation); }
AdmissionResult LiveSystem::activate_remote_plan(PlanId id, uint32_t ttl, uint64_t now) {
    if (!configured_ || !config_.member) return {AdmissionCode::unauthorized, "remote activation requires a member slice"};
    bool identities_ready = true;
    for (auto& p : ports_) identities_ready = identity_available(p) && identities_ready;
    if (!identities_ready) return {AdmissionCode::overloaded, "expired CEC identity validation is pending"};
    auto result = coordinator_.activate_remote_plan(id, ttl, now);
    if (result) for (auto& p : ports_) for (const auto& policy : coordinator_.policies()) if (policy.port == p.key) {
        p.authority_expired = false; p.bus->install_receive_policy({true, policy.owned_logical_address.value_or(15)});
    }
    return result;
}
AdmissionResult LiveSystem::renew_remote_authority(PlanId id, SessionId session, uint64_t generation, uint32_t ttl, uint64_t now) {
    if (!configured_ || !config_.member) return {AdmissionCode::unauthorized, "authority renewal requires a member slice"};
    bool identities_ready = true;
    for (auto& p : ports_) identities_ready = identity_available(p) && identities_ready;
    if (!identities_ready) return {AdmissionCode::overloaded, "expired CEC identity validation is pending"};
    auto result = coordinator_.renew_remote_authority(id, session, generation, ttl, now);
    if (result) for (auto& p : ports_) for (const auto& policy : coordinator_.policies()) if (policy.port == p.key && policy.authority_expires_at_ms > now) {
        p.authority_expired = false; p.bus->install_receive_policy({true, policy.owned_logical_address.value_or(15)});
    }
    return result;
}
void LiveSystem::abort_remote_plan(PlanId id) { coordinator_.abort_remote_plan(id); }
std::vector<PortPolicySnapshot> LiveSystem::local_policy_snapshot() const {
    auto policies = coordinator_.policies();
    for (auto& policy : policies) for (const auto& binding : bindings_) if (binding.endpoint.port == policy.port &&
        std::none_of(policy.bindings.begin(), policy.bindings.end(), [&](const auto& permission) { return permission.binding.binding_id == binding.binding_id; })) {
        auto diagnostic = binding; diagnostic.role = policy.role;
        if (policy.role == PortRole::monitor || policy.role == PortRole::disabled) diagnostic.status = BindingStatus::unresolved;
        policy.bindings.push_back({std::move(diagnostic), false, false, false, false});
    }
    return policies;
}
ExecutionSnapshot LiveSystem::execution_snapshot() const {
    auto state = coordinator_.snapshot();
    state.activity_outcomes = activity_outcomes_;
    for (const auto& p : ports_) {
        if (auto scan = p.verified) {
            for (const auto& observation : scan->observations) if (observation.endpoint.port == p.key) state.native_observations.push_back(observation);
        }
        else for (const auto& observation : observations_) if (observation.endpoint.port == p.key) state.native_observations.push_back(observation);
    }
    state.suspended_requested_sessions = suspended_requests_;
    for (auto& observation : state.native_observations) {
        auto p = std::find_if(ports_.begin(), ports_.end(), [&](const auto& port) { return port.key == observation.endpoint.port; });
        auto scan = p == ports_.end() ? std::shared_ptr<const DiscoverySnapshot>{} : p->verified;
        observation.stale = observation.owner_boot_id != boot_ || !scan || scan->scan_id != observation.scan_id || scan->status != ScanStatus::complete ||
            last_tick_ms_ < observation.received_at_ms || last_tick_ms_ - observation.received_at_ms > 30000;
    }
    if (installed_) { state.applied_config_revision = config_.topology.config_revision; state.applied_presentation_revision = config_.presentation_revision; state.applied_policy_hash = config_.executable_hash; }
    return state;
}
Snapshot LiveSystem::snapshot(const Snapshot& legacy) const {
    auto state = legacy; state.activities.assign(16, {}); state.current_activity = selected_slot_; state.activity_generation = activity_generation_; state.discovery_generation = discovery_generation_;
    state.ready = configured_;
    for (const auto& policy : coordinator_.policies()) {
        if (policy.port.tentacle_id != 0) continue; // Legacy projection has no connector identity.
        if (policy.port.side == Side::front) { state.front_address = policy.owned_logical_address.value_or(15); state.front_physical_address = policy.physical_address ? policy.physical_address->physical_address : 0xffff; }
        else state.back_address = policy.owned_logical_address.value_or(15);
    }
    state.devices.clear(); state.discovering = false;
    for (const auto& p : ports_) if (auto scan = discovery_.snapshot(p.scan)) {
        state.discovering = state.discovering || (scan->status != ScanStatus::complete && scan->status != ScanStatus::timeout && scan->status != ScanStatus::cancelled);
        for (const auto& o : scan->observations) if (p.key.tentacle_id == 0 && o.endpoint.port == p.key && o.osd_name) {
            const auto resolved = resolve_native_name({o.endpoint.port, *o.osd_name}, *scan, last_tick_ms_);
            state.devices.push_back({*o.osd_name, o.endpoint.port.side == Side::front ? BusSide::Front : BusSide::Back, o.endpoint.logical_address,
                o.physical_address, o.device_type, o.vendor_id, resolved.endpoint == o.endpoint && o.owner_boot_id == boot_ && !o.stale});
        }
    }
    for (const auto& slot : config_.external_slots) if (slot.slot < 16 && !slot.tombstone && !slot.hidden) {
        auto a = std::find_if(config_.activities.begin(), config_.activities.end(), [&](const auto& a) { return a.activity_id == slot.activity_id; });
        if (a != config_.activities.end() && a->zone_id == config_.primary_zone_id) state.activities[slot.slot].name = slot.alias.empty() ? a->label : slot.alias;
    }
    auto execution = coordinator_.snapshot(); state.activity_settling = execution.executing_plan.has_value(); state.activity_error = error_.empty() ? execution.error : error_; return state;
}
}
