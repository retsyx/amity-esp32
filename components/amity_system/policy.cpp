#include "amity/system/policy.hpp"
#include <limits>

namespace amity::system {
namespace {
AdmissionResult ok() { return {AdmissionCode::accepted, {}}; }
AdmissionResult reject(AdmissionCode code, const char* reason) { return {code, reason}; }
bool needs_route(OperationKind kind) { return kind != OperationKind::query_state; }
bool audio_key(uint8_t key) { return (key >= 0x41 && key <= 0x43) || key == 0x65 || key == 0x66; }
bool authorized_grant(const PortPolicySnapshot& p, const AuthorizedCecOperation& o, uint64_t now) {
    for (const auto& grant : p.route_grants)
        if (grant.binding_id == o.binding_id && grant.session_id == o.fence.session_id &&
            grant.route_generation == o.fence.route_generation && grant.expires_at_ms > now) return true;
    return false;
}
}
const PortPolicySnapshot* LocalCecAgent::policy(const CecPortRef& port) const {
    for (const auto& p : policies_) if (p.port == port) return &p;
    return nullptr;
}
const BindingPermission* LocalCecAgent::permission(const PortPolicySnapshot& p, BindingId id) const {
    for (const auto& b : p.bindings) if (b.binding.binding_id == id) return &b;
    return nullptr;
}
AdmissionResult LocalCecAgent::check_policy(const PortPolicySnapshot& p, uint64_t now) const {
    if (!p.port.valid() || !p.owner_boot_id || !p.domain_id || !p.domain_revision || !p.coordinator_epoch ||
        !p.config_revision || !p.policy_generation || p.bindings.size() > 64 || p.route_grants.size() > 64 ||
        p.components.size() > 64 || p.authority_expires_at_ms <= now || p.proxy_osd_name.empty() || p.proxy_osd_name.size() > 14)
        return reject(AdmissionCode::invalid, "invalid or expired policy snapshot");
    if (!policies_.empty() && (p.port.octopus_id != policies_[0].port.octopus_id || p.port.consortium_id != policies_[0].port.consortium_id))
        return reject(AdmissionCode::unauthorized, "a local agent executes exactly one octopus and consortium");
    for (const auto& c : p.components) {
        if (c.qualified_names.size() > 16 || c.label.size() > 128)
            return reject(AdmissionCode::overloaded, "component snapshot exceeds bounded capacity");
        for (const auto& n : c.qualified_names) if (!n.valid()) return reject(AdmissionCode::invalid, "invalid configured qualified name");
    }
    if (p.role == PortRole::disabled || p.role == PortRole::monitor) {
        if (p.owned_logical_address || !p.bindings.empty() || !p.route_grants.empty())
            return reject(AdmissionCode::invalid, "inactive roles cannot own identities or equipment control");
    } else {
        if (!p.owned_logical_address || *p.owned_logical_address >= 15)
            return reject(AdmissionCode::invalid, "active role requires an allocated identity");
        if (p.role == PortRole::tv_controller) {
            if (*p.owned_logical_address != 0) return reject(AdmissionCode::invalid, "TV controller must own address 0");
        } else if (p.role == PortRole::playback_proxy) {
            if (*p.owned_logical_address != 4 && *p.owned_logical_address != 8 && *p.owned_logical_address != 11 &&
                *p.owned_logical_address != 12 && *p.owned_logical_address != 13)
                return reject(AdmissionCode::invalid, "invalid playback identity");
            if (*p.owned_logical_address >= 12 && !p.qualified_cec20_backup_addresses)
                return reject(AdmissionCode::unsupported, "CEC 2.0 backup playback identities need a qualified policy");
            if (!p.physical_address || p.physical_address->attachment != p.port ||
                p.physical_address->domain_id != p.domain_id || p.physical_address->domain_revision != p.domain_revision ||
                !p.physical_address->confirmed || !valid_physical_address(p.physical_address->physical_address, false) ||
                (p.physical_address->provenance != PaProvenance::immediate_peer_report &&
                 p.physical_address->provenance != PaProvenance::user_confirmed_fallback))
                return reject(AdmissionCode::invalid, "unverified attachment physical address");
        } else return reject(AdmissionCode::invalid, "unknown role");
    }
    for (const auto& b : p.bindings) {
        auto component = std::find_if(p.components.begin(), p.components.end(), [&](const Component& c) { return c.component_id == b.binding.component_id; });
        if (component == p.components.end() || !valid_binding(b.binding, *component) || b.binding.endpoint.port != p.port ||
            b.binding.role != p.role || b.binding.domain_id != p.domain_id || b.binding.domain_revision != p.domain_revision ||
            b.binding.status != BindingStatus::verified || (b.binding.kind == BindingKind::proxy_representation &&
                b.binding.endpoint.logical_address != *p.owned_logical_address))
            return reject(AdmissionCode::invalid, "binding is not currently verified for this policy");
    }
    for (size_t i = 0; i < p.bindings.size(); ++i) for (size_t j = i + 1; j < p.bindings.size(); ++j)
        if (p.bindings[i].binding.binding_id == p.bindings[j].binding.binding_id)
            return reject(AdmissionCode::invalid, "duplicate binding identifier");
    for (const auto& g : p.route_grants) {
        if (!g.session_id || !g.route_generation || g.expires_at_ms > p.authority_expires_at_ms ||
            g.expires_at_ms <= now || !permission(p, g.binding_id) || g.permitted_paths.size() > 32)
            return reject(AdmissionCode::invalid, "invalid or oversized route grant");
        for (auto path : g.permitted_paths) if (!valid_physical_address(path, false)) return reject(AdmissionCode::invalid, "invalid permitted physical path");
    }
    for (size_t i = 0; i < p.route_grants.size(); ++i) {
        const auto* a = permission(p, p.route_grants[i].binding_id);
        if (a->binding.kind != BindingKind::proxy_representation) continue;
        for (size_t j = i + 1; j < p.route_grants.size(); ++j) {
            const auto* b = permission(p, p.route_grants[j].binding_id);
            if (b->binding.kind == BindingKind::proxy_representation &&
                a->binding.proxy_identity_resource_id == b->binding.proxy_identity_resource_id &&
                a->binding.binding_id != b->binding.binding_id)
                return reject(AdmissionCode::unauthorized, "conflicting representations of one proxy identity");
        }
    }
    for (const auto& existing : policies_) if (existing.port != p.port && existing.domain_id == p.domain_id &&
        existing.owned_logical_address && p.owned_logical_address && existing.owned_logical_address == p.owned_logical_address)
        return reject(AdmissionCode::unauthorized, "duplicate domain ACK ownership");
    auto found = std::find_if(policies_.begin(), policies_.end(), [&](const PortPolicySnapshot& old) { return old.port == p.port; });
    if (found == policies_.end()) {
        if (policies_.size() == 10) return reject(AdmissionCode::overloaded, "local port capacity exhausted");
    } else {
        if (p.owner_boot_id != found->owner_boot_id || p.coordinator_epoch < found->coordinator_epoch ||
            (p.coordinator_epoch == found->coordinator_epoch && p.policy_generation <= found->policy_generation))
            return {AdmissionCode::stale, "policy generation cannot go backwards or change boot (adapter " +
                std::to_string(p.port.tentacle_id) + (p.port.side == Side::front ? " front" : " back") +
                ", next " + format_uint64(p.policy_generation) + ", installed " + format_uint64(found->policy_generation) + ")"};
        for (const auto& next : p.bindings) for (const auto& old : found->bindings) if (next.binding.binding_id == old.binding.binding_id) {
            if (next.binding.component_id != old.binding.component_id || next.binding.kind != old.binding.kind ||
                next.binding.device_name != old.binding.device_name || next.binding.binding_revision < old.binding.binding_revision ||
                (next.binding.endpoint != old.binding.endpoint && next.binding.binding_revision == old.binding.binding_revision))
                return reject(AdmissionCode::stale, "binding identifiers cannot be recycled or revisions downgraded");
        }
        if ((p.owned_logical_address != found->owned_logical_address || p.role != found->role) &&
            std::any_of(transmissions_.begin(), transmissions_.end(), [&](const auto& work) { return work.port == p.port; }))
            return reject(AdmissionCode::stale, "identity changes require the old wire frame to drain");
    }
    return ok();
}
AdmissionResult LocalCecAgent::install_policy(const PortPolicySnapshot& p, uint64_t now) {
    maintain(now);
    const auto checked = check_policy(p, now); if (!checked) return checked;
    auto found = std::find_if(policies_.begin(), policies_.end(), [&](const PortPolicySnapshot& old) { return old.port == p.port; });
    if (found == policies_.end()) policies_.push_back(p);
    else {
        // The adapter normally fences before synchronization. Enforce it here
        // too: installing a new address may never redirect an old held release.
        for (const auto& old : found->bindings) {
            const auto* next = permission(p, old.binding.binding_id);
            if (!next || next->binding.binding_revision != old.binding.binding_revision ||
                next->binding.endpoint != old.binding.endpoint || p.domain_revision != found->domain_revision ||
                p.owned_logical_address != found->owned_logical_address || p.role != found->role)
                fence_binding(old.binding.binding_id, old.binding.binding_revision, false);
        }
        for (const auto& old_grant : found->route_grants) {
            const auto remains = std::find_if(p.route_grants.begin(), p.route_grants.end(), [&](const RouteGrant& grant) {
                return grant.binding_id == old_grant.binding_id && grant.session_id == old_grant.session_id && grant.route_generation == old_grant.route_generation;
            });
            if (remains == p.route_grants.end()) close_route_impl(old_grant.session_id, old_grant.route_generation, false);
        }
        // A disjoint session may add a grant on the same port. Retain an
        // unchanged hold state/cleanup. Repeats use the current exact policy
        // and route authority; ordinary queued operations stay stale.
        if (p.coordinator_epoch == found->coordinator_epoch && p.config_revision == found->config_revision &&
            p.domain_revision == found->domain_revision && p.role == found->role && p.owned_logical_address == found->owned_logical_address)
            for (auto& h : holds_) if (h.operation.target.port == p.port) {
                const auto* allowed = permission(p, h.operation.binding_id);
                if (allowed && allowed->binding.endpoint == h.operation.target && allowed->binding.binding_revision == h.operation.fence.binding_revision &&
                    authorized_grant(p, h.operation, now)) {
                    h.rebased_revision = p.config_revision; h.rebased_policy_generation = p.policy_generation;
                }
            }
        cancel_transmissions([&](const auto& work) {
            if (work.port != p.port || work.emergency_release) return false;
            if (work.operation.kind == OperationKind::press_key && work.operation.arguments.repeat)
                for (const auto& h : holds_) if (h.token == work.operation.arguments.hold_token && h.active &&
                    h.rebased_policy_generation == p.policy_generation) return false;
            return work.automatic_identity || work.operation.fence.policy_generation != p.policy_generation;
        });
        *found = p;
    }
    prune_fences();
    return ok(); // Installation never emits a CEC frame.
}
bool LocalCecAgent::can_rebase_revision(uint64_t expected, uint64_t next) const {
    return expected && next > expected && !policies_.empty() && std::all_of(policies_.begin(), policies_.end(), [&](const auto& p) {
        return p.config_revision == expected && p.policy_generation != UINT64_MAX;
    });
}
bool LocalCecAgent::rebase_revision(uint64_t expected, uint64_t next) {
    if (!can_rebase_revision(expected, next)) return false;
    // Only the admitted revision changes. Invalidate already submitted old
    // positive guards; no driver-queued press is rewritten as a fresh one.
    cancel_transmissions([&](const auto& work) { return !work.emergency_release && work.operation.fence.config_revision == expected; });
    for (auto& p : policies_) { p.config_revision = next; ++p.policy_generation; }
    for (auto& h : holds_) if (const auto* p = policy(h.operation.target.port)) {
        h.rebased_revision = next; h.rebased_policy_generation = p->policy_generation;
    }
    return true;
}
AdmissionResult LocalCecAgent::renew_discovery_authority(const CecPortRef& port, uint32_t ttl, uint64_t now) {
    maintain(now);
    if (!ttl || ttl > 5000 || now > UINT64_MAX - ttl) return reject(AdmissionCode::invalid, "bounded fresh discovery lease required");
    auto found = std::find_if(policies_.begin(), policies_.end(), [&](const auto& p) { return p.port == port; });
    if (found == policies_.end()) return reject(AdmissionCode::unauthorized, "no committed identity policy");
    // Route grants, command deadlines, holds and generations remain unchanged.
    // This only permits presence/identity work on the same installed attachment.
    found->authority_expires_at_ms = std::max(found->authority_expires_at_ms, now + ttl);
    return ok();
}
bool LocalCecAgent::closed(const AuthorizedCecOperation& o) const {
    for (const auto& b : fenced_bindings_) if (b.first == o.binding_id && o.fence.binding_revision <= b.second) return true;
    for (const auto& r : closed_routes_) if (r.first == o.fence.session_id && o.fence.route_generation <= r.second) return true;
    return false;
}
AdmissionResult LocalCecAgent::validate(const AuthorizedCecOperation& o, uint64_t now, bool internal_reply) const {
    if (fail_closed_) return reject(AdmissionCode::overloaded, "fence capacity exhausted; local execution is passive");
    if ((!o.command_id && !internal_reply) || !o.component_id || !o.binding_id || !o.target.valid()) return reject(AdmissionCode::invalid, "invalid operation identity");
    const auto* p = policy(o.target.port); if (!p) return reject(AdmissionCode::unauthorized, "port has no installed policy");
    if (p->authority_expires_at_ms <= now || o.execution_expires_at_ms <= now || o.execution_expires_at_ms > p->authority_expires_at_ms)
        return reject(AdmissionCode::expired, "receiver-local execution authority expired");
    const auto* b = permission(*p, o.binding_id); if (!b) return reject(AdmissionCode::unauthorized, "binding not permitted");
    if (closed(o) || b->binding.component_id != o.component_id || b->binding.endpoint != o.target ||
        b->binding.status != BindingStatus::verified || o.fence.owner_boot_id != p->owner_boot_id ||
        o.fence.coordinator_epoch != p->coordinator_epoch || o.fence.config_revision != p->config_revision ||
        o.fence.domain_revision != p->domain_revision || o.fence.binding_revision != b->binding.binding_revision ||
        o.fence.policy_generation != p->policy_generation)
        return reject(AdmissionCode::stale, "operation does not match exact execution fence");
    if (needs_route(o.kind) && !authorized_grant(*p, o, now)) return reject(AdmissionCode::unauthorized, "current route grant required");
    if (!compile(o, *p, *b)) return reject(AdmissionCode::unsupported, "operation is unsupported by binding and role");
    return ok();
}
AdmissionResult LocalCecAgent::renew_authority(const AuthorizedCecOperation& fresh, uint64_t now) {
    maintain(now);
    if (fresh.kind == OperationKind::release_key || fresh.execution_expires_at_ms <= now || fresh.execution_expires_at_ms - now > 5000)
        return reject(AdmissionCode::expired, "receiver authority must be fresh and bounded to five seconds");
    auto current = std::find_if(policies_.begin(), policies_.end(), [&](const auto& p) { return p.port == fresh.target.port; });
    if (current == policies_.end()) return reject(AdmissionCode::unauthorized, "no current local policy");
    auto previous = *current;
    current->authority_expires_at_ms = std::max(current->authority_expires_at_ms, fresh.execution_expires_at_ms);
    for (auto& grant : current->route_grants) if (grant.session_id == fresh.fence.session_id && grant.route_generation == fresh.fence.route_generation)
        grant.expires_at_ms = std::max(grant.expires_at_ms, fresh.execution_expires_at_ms);
    auto result = validate(fresh, now);
    if (!result) *current = std::move(previous);
    return result;
}
std::optional<std::vector<CecFrame>> LocalCecAgent::compile(const AuthorizedCecOperation& o, const PortPolicySnapshot& p, const BindingPermission& permission) const {
    if (!p.owned_logical_address) return {};
    const auto& b = permission.binding;
    CecFrame frame{p.port, *p.owned_logical_address, o.target.logical_address, {}};
    switch (o.kind) {
    case OperationKind::ensure_power:
        if (b.kind != BindingKind::native_appliance || b.capability != Capability::power) return {};
        if (o.arguments.on && permission.image_view_on && p.role == PortRole::playback_proxy) frame.payload = {0x04};
        else if (!o.arguments.on && permission.discrete_power_off) frame.payload = {0x36};
        else if (o.arguments.on && permission.discrete_power_on) {
            frame.payload = {0x44, 0x6d}; auto release = frame; release.payload = {0x45}; return std::vector<CecFrame>{frame, release};
        } else return {};
        break;
    case OperationKind::select_path:
        if (b.kind != BindingKind::native_appliance || b.capability != Capability::routing || p.role != PortRole::tv_controller ||
            !permission.exact_path_selection || !valid_physical_address(o.arguments.physical_path, false)) return {};
        {
            bool allowed = false;
            for (const auto& g : p.route_grants) if (g.binding_id == o.binding_id && g.session_id == o.fence.session_id &&
                g.route_generation == o.fence.route_generation &&
                std::find(g.permitted_paths.begin(), g.permitted_paths.end(), o.arguments.physical_path) != g.permitted_paths.end()) allowed = true;
            if (!allowed) return {};
        }
        frame.destination = 15; frame.payload = {0x86, static_cast<uint8_t>(o.arguments.physical_path >> 8), static_cast<uint8_t>(o.arguments.physical_path)}; break;
    case OperationKind::announce_active_source:
        if (b.kind != BindingKind::proxy_representation || b.capability != Capability::proxy_activation || p.role != PortRole::playback_proxy || !p.physical_address) return {};
        frame.destination = 15; frame.payload = {0x82, static_cast<uint8_t>(p.physical_address->physical_address >> 8), static_cast<uint8_t>(p.physical_address->physical_address)}; break;
    case OperationKind::press_key:
    case OperationKind::release_key:
        if (b.kind != BindingKind::native_appliance || (b.capability != Capability::navigation && b.capability != Capability::volume && b.capability != Capability::audio) ||
            !o.arguments.hold_token || !o.arguments.hold_token->valid() || o.arguments.key > 0x76) return {};
        frame.payload = o.kind == OperationKind::press_key ? std::vector<uint8_t>{0x44, o.arguments.key} : std::vector<uint8_t>{0x45};
        if (o.kind == OperationKind::press_key && !o.arguments.repeat) {
            auto release = frame; release.payload = {0x45}; return std::vector<CecFrame>{frame, release};
        }
        break;
    case OperationKind::query_state:
        if (b.kind != BindingKind::native_appliance) return {};
        if (o.arguments.query_opcode != 0x8f && o.arguments.query_opcode != 0x71 && o.arguments.query_opcode != 0x46 &&
            o.arguments.query_opcode != 0x83 && o.arguments.query_opcode != 0x8c && o.arguments.query_opcode != 0x9f) return {};
        if (o.arguments.query_opcode == 0x71 && b.capability != Capability::audio && b.capability != Capability::audio_status && b.capability != Capability::query_state && b.capability != Capability::volume) return {};
        frame.payload = {o.arguments.query_opcode}; break;
    default: return {};
    }
    return std::vector<CecFrame>{frame};
}
AdmissionResult LocalCecAgent::execute(const AuthorizedCecOperation& o, uint64_t now) {
    maintain(now);
    if (o.kind == OperationKind::release_key) {
        if (!o.arguments.hold_token) return reject(AdmissionCode::invalid, "release requires exact hold token");
        for (const auto& h : holds_) if (!h.finite && h.token == *o.arguments.hold_token) {
            if (h.operation.binding_id != o.binding_id || h.operation.target != o.target ||
                (h.operation.fence != o.fence && h.cleanup_fence != o.fence))
                return reject(AdmissionCode::stale, "release cannot migrate between routes or bindings");
            return release_hold(h.token, now) ? ok() : reject(AdmissionCode::stale, "button is no longer pressed");
        }
        return reject(AdmissionCode::stale, "button is no longer pressed");
    }
    const auto admitted = validate(o, now); if (!admitted) return admitted;
    const auto* p = policy(o.target.port); auto frames = *compile(o, *p, *permission(*p, o.binding_id));
    if (frames.front().payload.empty() || frames.front().payload.front() != 0x44)
        return submit_frames(o, frames, now) ? ok() : reject(AdmissionCode::overloaded, "CEC driver did not admit the operation");
    if (o.arguments.hold_token) for (const auto& h : holds_) if (!h.finite && h.token == *o.arguments.hold_token)
        return reject(AdmissionCode::held_key_conflict, "button token is already active");
    auto stream = std::find_if(streams_.begin(), streams_.end(), [&](const auto& value) {
        return value.safe && !value.release_work_id && value.cleanup.target == o.target && value.initiator == frames.front().initiator;
    });
    if (stream != streams_.end() && stream->ended) {
        const auto id = stream->stream_id;
        if (!flush_stream(id, now)) return reject(AdmissionCode::overloaded, "previous press stream has not finished");
        stream = streams_.end(); // New positive work belongs after the admitted final release.
    }
    const bool created = stream == streams_.end();
    if (created) {
        if (streams_.size() >= 16 || next_stream_id_ == UINT64_MAX) return reject(AdmissionCode::overloaded, "press stream capacity exhausted");
        streams_.push_back({++next_stream_id_, o, frames.front().initiator, false, false, true, 0, {}});
        stream = streams_.end() - 1;
    }
    const auto stream_id = stream->stream_id;
    const bool finite = !(o.kind == OperationKind::press_key && o.arguments.repeat);
    const bool isolated_finite = finite && created;
    if (audio_key(o.arguments.key)) stream->audio_context = o;
    if (!isolated_finite) {
        if (holds_.size() >= maximum_holds_) { if (created) discard_stream(stream_id); return reject(AdmissionCode::overloaded, "button state capacity exhausted"); }
        HeldKey h; h.token = o.arguments.hold_token.value_or(HoldToken{}); h.operation = o; h.initiator = frames.front().initiator;
        h.cleanup_fence = o.fence; h.stream_id = stream_id; h.finite = finite; h.up_requested = finite;
        holds_.push_back(std::move(h));
        frames.resize(1); // A finite tap joins the existing aggregate; it cannot release other pressed keys.
    }
    // The original finite key path admits its press, then immediately tries
    // release. Acceptance follows the press even if that fills the FIFO and
    // release drops. Keep the same command correlation for both admitted TXs.
    if (isolated_finite) {
        if (!submit_frames(o, {frames.front()}, now, false, false, stream_id)) {
            discard_stream(stream_id);
            return reject(AdmissionCode::overloaded, "CEC driver did not admit the operation");
        }
        for (auto& value : streams_) if (value.stream_id == stream_id) value.ended = true;
        if (!submit_frames(o, {frames.back()}, now, false, false, stream_id)) {
            ++uncertain_releases_;
            discard_stream(stream_id); // Consume this release decision, including on FIFO-full rejection.
        }
        return ok();
    }
    if (!submit_frames(o, frames, now, false, false, stream_id)) {
        holds_.pop_back();
        if (created) discard_stream(stream_id);
        return reject(AdmissionCode::overloaded, "CEC driver did not admit the operation");
    }
    observe_stream_end(stream_id);
    return ok();
}
bool LocalCecAgent::submit_frames(const AuthorizedCecOperation& o, const std::vector<CecFrame>& frames, uint64_t now,
                                 bool identity, bool internal, uint64_t stream_id) {
    if (!submit_ || !cancel_ || frames.empty() || frames.size() > 2 ||
        next_work_id_ > UINT64_MAX - frames.size()) return false;
    std::vector<WireWork> work; work.reserve(frames.size());
    std::vector<Transmission> records; records.reserve(frames.size());
    for (size_t i = 0; i < frames.size(); ++i) {
        const bool release = !frames[i].payload.empty() && frames[i].payload.front() == 0x45;
        const auto id = ++next_work_id_;
        const auto* installed = policy(frames[i].port);
        const auto expiry = release ? 0 : identity && installed ? installed->authority_expires_at_ms : o.execution_expires_at_ms;
        work.push_back({o.command_id, frames[i], release, o.binding_id, o.fence.binding_revision, expiry,
            !frames[i].payload.empty() && frames[i].payload.front() == 0x44 ? 1u : 3u, id, release && i != 0});
        records.push_back({id, o, frames[i].port, identity, release, installed ? installed->policy_generation : 0, internal, stream_id});
    }
    // This callback copies the stack-owned frames into the only transmit FIFO.
    // It returns actual all-or-none driver admission and never completes inline.
    // Let each wire decide capacity and log its rejected frames. A shared
    // completion-record cap would hide full-FIFO drops and couple separate wires.
    transmissions_.reserve(transmissions_.size() + frames.size()); // Completion registration cannot allocate after driver admission.
    if (!submit_(work.data(), work.size())) return false;
    for (const auto& record : records) transmissions_.push_back(record);
    if (stream_id) {
        for (auto& h : holds_) if (h.stream_id == stream_id && h.operation.command_id == o.command_id) { h.started = true; h.press_work_id = work.front().work_id; }
        for (auto& stream : streams_) if (stream.stream_id == stream_id)
            for (const auto& frame : work) {
                if (frame.emergency_release) stream.release_work_id = frame.work_id;
                else stream.admitted = true;
            }
    }
    if (std::any_of(work.begin(), work.end(), [](const auto& w) { return w.emergency_release; }) && audio_key(o.arguments.key))
        schedule_audio_query(o, now <= UINT64_MAX - 500 ? now + 500 : UINT64_MAX, stream_id);
    return true;
}
void LocalCecAgent::cancel_transmissions(const std::function<bool(const Transmission&)>& matches) {
    if (cancel_) for (const auto& work : transmissions_) if (matches(work)) cancel_(work.work_id);
}
void LocalCecAgent::observe_stream_end(uint64_t id) {
    const bool pressed = std::any_of(holds_.begin(), holds_.end(), [&](const auto& h) { return h.stream_id == id && !h.up_requested; });
    if (!pressed) for (auto& stream : streams_) if (stream.stream_id == id) stream.ended = true;
}
void LocalCecAgent::discard_stream(uint64_t id) {
    holds_.erase(std::remove_if(holds_.begin(), holds_.end(), [&](const auto& h) { return h.stream_id == id; }), holds_.end());
    streams_.erase(std::remove_if(streams_.begin(), streams_.end(), [&](const auto& stream) { return stream.stream_id == id; }), streams_.end());
}
void LocalCecAgent::release_at(size_t index, bool safe, bool dispatch) {
    auto& h = holds_[index]; const auto id = h.stream_id;
    if (!safe) {
        for (auto& stream : streams_) if (stream.stream_id == id) { stream.safe = false; stream.ended = true; if (stream.started) ++uncertain_releases_; }
        h.up_requested = true;
        cancel_transmissions([&](const auto& work) { return work.stream_id == id; });
    }
    // Button-up ends only this logical key. Every admitted press stays in the
    // sole FIFO, including a repeat that has not reached its first wire edge.
    holds_.erase(holds_.begin() + static_cast<std::ptrdiff_t>(index));
    observe_stream_end(id);
    if (dispatch) flush_stream(id, now_ms_);
}
bool LocalCecAgent::flush_stream(uint64_t id, uint64_t now) {
    const auto found = std::find_if(streams_.begin(), streams_.end(), [&](const auto& stream) { return stream.stream_id == id; });
    if (found == streams_.end() || found->release_work_id) return true;
    if (!found->ended) return false;
    if (!found->safe) {
        // Unsafe attachment cleanup retains only completion metadata until the
        // canceled driver work drains. It never submits to the replaced peer.
        if (std::none_of(transmissions_.begin(), transmissions_.end(), [&](const auto& work) { return work.stream_id == id; })) discard_stream(id);
        return true;
    }
    if (!found->admitted) { discard_stream(id); return true; }
    const auto stream = *found; auto release = stream.cleanup; release.kind = OperationKind::release_key; release.command_id = {};
    const CecFrame frame{release.target.port, stream.initiator, release.target.logical_address, {0x45}};
    if (!submit_frames(release, {frame}, now, false, true, id)) {
        ++uncertain_releases_;
        discard_stream(id); // The final-release decision is consumed even when the driver FIFO is full.
        return true;
    }
    if (stream.audio_context) schedule_audio_query(*stream.audio_context, now <= UINT64_MAX - 500 ? now + 500 : UINT64_MAX, id);
    holds_.erase(std::remove_if(holds_.begin(), holds_.end(), [&](const auto& h) { return h.stream_id == id; }), holds_.end());
    return true;
}
bool LocalCecAgent::release_hold(HoldToken token, uint64_t now) {
    if (now) now_ms_ = now;
    for (size_t i = 0; i < holds_.size(); ++i) if (!holds_[i].finite && !holds_[i].up_requested && holds_[i].token == token) {
        release_at(i, true); return true; // Final aggregate up queues UCR immediately behind admitted presses.
    }
    return false;
}
std::optional<AuthorizedCecOperation> LocalCecAgent::held_authority(const HeldKey& held, uint64_t now) const {
    auto operation = held.operation;
    if (held.started && held.rebased_revision) {
        operation.fence.config_revision = held.rebased_revision;
        operation.fence.policy_generation = held.rebased_policy_generation;
    }
    if (held.started) {
        const auto* current = policy(operation.target.port); if (!current) return {};
        operation.execution_expires_at_ms = current->authority_expires_at_ms;
        for (const auto& grant : current->route_grants) if (grant.binding_id == operation.binding_id &&
            grant.session_id == operation.fence.session_id && grant.route_generation == operation.fence.route_generation)
            operation.execution_expires_at_ms = std::min(operation.execution_expires_at_ms, grant.expires_at_ms);
    }
    return validate(operation, now) ? std::optional<AuthorizedCecOperation>{operation} : std::nullopt;
}
void LocalCecAgent::schedule_audio_query(const AuthorizedCecOperation& operation, uint64_t due, uint64_t stream_id) {
    auto query = operation; query.command_id = {}; query.kind = OperationKind::query_state; query.arguments.query_opcode = 0x71;
    const auto* p = policy(query.target.port); if (!p) return;
    query.execution_expires_at_ms = p->authority_expires_at_ms;
    for (const auto& grant : p->route_grants) if (grant.binding_id == query.binding_id && grant.session_id == query.fence.session_id &&
        grant.route_generation == query.fence.route_generation) query.execution_expires_at_ms = std::min(query.execution_expires_at_ms, grant.expires_at_ms);
    const bool started = std::any_of(streams_.begin(), streams_.end(), [&](const auto& stream) { return stream.stream_id == stream_id && stream.started; });
    auto existing = std::find_if(audio_queries_.begin(), audio_queries_.end(), [&](const auto& q) { return q.operation.target == query.target; });
    if (existing != audio_queries_.end()) *existing = {query, due, stream_id, started};
    else if (audio_queries_.size() < maximum_holds_) audio_queries_.push_back({query, due, stream_id, started});
}
void LocalCecAgent::maintain(uint64_t now) {
    now_ms_ = now;
    for (size_t i = holds_.size(); i > 0; --i) {
        const auto stream = std::find_if(streams_.begin(), streams_.end(), [&](const auto& value) { return value.stream_id == holds_[i - 1].stream_id; });
        if (stream == streams_.end() || !stream->safe) { release_at(i - 1, false, false); continue; }
        if (!held_authority(holds_[i - 1], now)) {
            const auto token = holds_[i - 1].token;
            cancel_transmissions([&](const auto& work) { return !work.emergency_release && work.operation.arguments.hold_token == token; });
            release_at(i - 1, true, false);
        }
    }
}
void LocalCecAgent::tick(uint64_t now) {
    maintain(now);
    std::vector<uint64_t> ending; for (const auto& stream : streams_) if (stream.ended && !stream.release_work_id) ending.push_back(stream.stream_id);
    for (const auto id : ending) flush_stream(id, now);
    for (size_t i = audio_queries_.size(); i > 0; --i) {
        auto& query = audio_queries_[i - 1]; const auto* current = policy(query.operation.target.port);
        if (!current || !authorized_grant(*current, query.operation, now) || !validate(query.operation, now, true)) {
            audio_queries_.erase(audio_queries_.begin() + static_cast<std::ptrdiff_t>(i - 1)); continue;
        }
        if (!query.press_started || now < query.due_ms || std::any_of(holds_.begin(), holds_.end(), [&](const auto& h) {
            return h.operation.target == query.operation.target && audio_key(h.operation.arguments.key);
        })) continue;
        const auto frames = compile(query.operation, *current, *permission(*current, query.operation.binding_id));
        if (!submit_frames(query.operation, *frames, now, false, true)) { query.due_ms = now <= UINT64_MAX - 500 ? now + 500 : UINT64_MAX; continue; }
        audio_queries_.erase(audio_queries_.begin() + static_cast<std::ptrdiff_t>(i - 1));
    }
    // Each ready held key owns one independent completion-paced attempt.
    // Another key's ACK, command admission or hold state cannot gate it.
    for (size_t index=0;index<holds_.size();) {
        auto& held=holds_[index];
        if (held.finite || !held.active || held.up_requested || held.press_work_id) { ++index; continue; }
        const auto stream = std::find_if(streams_.begin(), streams_.end(), [&](const auto& value) { return value.stream_id == held.stream_id; });
        if (stream == streams_.end() || !stream->safe || stream->ended || stream->release_work_id) { ++index; continue; }
        const auto operation = held_authority(held, now); if (!operation) { ++index; continue; }
        const auto* p = policy(operation->target.port); const auto frames = compile(*operation, *p, *permission(*p, operation->binding_id));
        if (!submit_frames(*operation, *frames, now, false, false, held.stream_id)) {
            const auto id=held.stream_id;
            holds_.erase(holds_.begin()+static_cast<std::ptrdiff_t>(index));
            observe_stream_end(id);flush_stream(id,now);
            index=0; // Consumed cleanup may also erase finite metadata before this key.
        } else ++index;
    }
}
void LocalCecAgent::fence_binding(BindingId binding, uint64_t revision, bool safe) {
    auto prior = std::find_if(fenced_bindings_.begin(), fenced_bindings_.end(), [&](const auto& b) { return b.first == binding; });
    if (prior != fenced_bindings_.end()) prior->second = std::max(prior->second, revision);
    else if (fenced_bindings_.size() < 640) fenced_bindings_.push_back({binding, revision}); else fail_closed_ = true;
    cancel_transmissions([&](const auto& work) { return work.operation.binding_id == binding && work.operation.fence.binding_revision <= revision && (!safe || !work.emergency_release); });
    if (!safe) for (auto& stream : streams_) {
        bool affected = stream.cleanup.binding_id == binding && stream.cleanup.fence.binding_revision <= revision;
        for (const auto& p : policies_) if (const auto* old = permission(p, binding))
            if (old->binding.binding_revision <= revision && old->binding.endpoint == stream.cleanup.target) affected = true;
        if (affected) {
            stream.safe = false; stream.ended = true;
            cancel_transmissions([&](const auto& work) { return work.stream_id == stream.stream_id ||
                (work.automatic_route_reply && work.operation.target == stream.cleanup.target); });
            audio_queries_.erase(std::remove_if(audio_queries_.begin(), audio_queries_.end(), [&](const auto& query) {
                return query.operation.target == stream.cleanup.target;
            }), audio_queries_.end());
        }
    }
    for (size_t i=holds_.size();i>0;) {
        i=std::min(i,holds_.size());if(!i)break;--i;
        if(holds_[i].operation.binding_id==binding&&holds_[i].operation.fence.binding_revision<=revision)release_at(i,safe);
    }
}
void LocalCecAgent::close_route(SessionId session, uint64_t generation) {
    close_route_impl(session, generation, true);
}
void LocalCecAgent::close_route_impl(SessionId session, uint64_t generation, bool dispatch) {
    auto prior = std::find_if(closed_routes_.begin(), closed_routes_.end(), [&](const auto& r) { return r.first == session; });
    if (prior != closed_routes_.end()) prior->second = std::max(prior->second, generation);
    else if (closed_routes_.size() < 640) closed_routes_.push_back({session, generation}); else fail_closed_ = true;
    cancel_transmissions([&](const auto& work) { return !work.emergency_release && work.operation.fence.session_id == session && work.operation.fence.route_generation <= generation; });
    for(size_t i=holds_.size();i>0;) {
        i=std::min(i,holds_.size());if(!i)break;--i;
        if(holds_[i].operation.fence.session_id==session&&holds_[i].operation.fence.route_generation<=generation)release_at(i,true,dispatch);
    }
}
bool LocalCecAgent::quiescent(SessionId session, uint64_t generation) const {
    return std::none_of(transmissions_.begin(), transmissions_.end(), [&](const auto& w) { return w.operation.fence.session_id == session && w.operation.fence.route_generation <= generation; }) &&
        std::none_of(holds_.begin(), holds_.end(), [&](const auto& h) { return h.operation.fence.session_id == session && h.operation.fence.route_generation <= generation; }) &&
        std::none_of(streams_.begin(), streams_.end(), [&](const auto& stream) { return stream.ended && stream.safe && stream.started &&
            stream.cleanup.fence.session_id == session && stream.cleanup.fence.route_generation <= generation; });
}
bool LocalCecAgent::repeat_ready(uint64_t now) const {
    return std::any_of(holds_.begin(), holds_.end(), [&](const auto& h) {
        return !h.finite && h.active && !h.up_requested && !h.press_work_id && held_authority(h, now).has_value() &&
            std::any_of(streams_.begin(), streams_.end(), [&](const auto& stream) { return stream.stream_id == h.stream_id && stream.safe && !stream.ended; });
    });
}
std::vector<AuthorizedCecOperation> LocalCecAgent::active_key_contexts() const {
    std::vector<AuthorizedCecOperation> result;
    for (const auto& h : holds_) if (!h.finite && !h.up_requested) result.push_back(h.operation);
    return result;
}
bool LocalCecAgent::repeating_frame_in_flight() const {
    return std::any_of(transmissions_.begin(), transmissions_.end(), [](const auto& w) { return !w.emergency_release && w.operation.kind == OperationKind::press_key && w.operation.arguments.repeat; });
}
std::optional<CommandId> LocalCecAgent::complete_frame(uint64_t id, WireOutcome outcome, uint64_t now, bool started) {
    now_ms_ = now;
    const auto found = std::find_if(transmissions_.begin(), transmissions_.end(), [&](const auto& work) { return work.work_id == id; });
    if (found == transmissions_.end()) return {};
    const auto work = *found; transmissions_.erase(found);
    if (!started && (outcome == WireOutcome::canceled || outcome == WireOutcome::expired)) ++rejected_at_wire_start_;
    auto stream = std::find_if(streams_.begin(), streams_.end(), [&](const auto& value) { return value.stream_id == work.stream_id; });
    if (work.emergency_release) {
        if (outcome != WireOutcome::success) ++uncertain_releases_;
        if (!started && (outcome == WireOutcome::canceled || outcome == WireOutcome::expired)) {
            cancel_transmissions([&](const auto& queued) { return queued.automatic_route_reply && queued.operation.kind == OperationKind::query_state &&
                queued.operation.arguments.query_opcode == 0x71 && queued.operation.arguments.hold_token == work.operation.arguments.hold_token; });
            audio_queries_.erase(std::remove_if(audio_queries_.begin(), audio_queries_.end(), [&](const auto& query) {
                return query.operation.arguments.hold_token == work.operation.arguments.hold_token;
            }), audio_queries_.end());
        }
        if (stream != streams_.end()) discard_stream(work.stream_id);
    } else if (stream != streams_.end()) {
        if (started) {
            stream->started = true; stream->cleanup = work.operation;
            for (auto& query : audio_queries_) if (query.stream_id == work.stream_id) query.press_started = true;
        }
        for (size_t i = 0; i < holds_.size(); ++i) if (holds_[i].press_work_id == id && holds_[i].stream_id == work.stream_id) {
            auto& h = holds_[i]; h.press_work_id = 0;
            if (h.finite || h.up_requested || outcome != WireOutcome::success) holds_.erase(holds_.begin() + static_cast<std::ptrdiff_t>(i));
            else if (outcome == WireOutcome::success && !h.up_requested) h.active = true;
            break;
        }
        observe_stream_end(work.stream_id);
        flush_stream(work.stream_id,now);
    }
    prune_fences(); return work.operation.command_id;
}
size_t LocalCecAgent::pending_work() const {
    return transmissions_.size() + std::count_if(streams_.begin(), streams_.end(), [](const auto& stream) {
        return stream.ended && !stream.release_work_id && stream.safe && stream.admitted;
    });
}
bool LocalCecAgent::command_pending(CommandId id) const {
    return std::any_of(transmissions_.begin(), transmissions_.end(), [&](const auto& w) { return w.operation.command_id == id; });
}
void LocalCecAgent::quiesce() {
    audio_queries_.clear();
    cancel_transmissions([](const auto& work) { return !work.emergency_release; });
    while(!holds_.empty())release_at(holds_.size()-1,true);
    audio_queries_.clear();
}
void LocalCecAgent::prune_fences() {
    const auto referenced = [&](const auto& matches) {
        for (const auto& w : transmissions_) if (matches(w.operation)) return true;
        for (const auto& h : holds_) if (matches(h.operation)) return true;
        for (const auto& stream : streams_) if (matches(stream.cleanup)) return true;
        for (const auto& query : audio_queries_) if (matches(query.operation)) return true;
        return false;
    };
    fenced_bindings_.erase(std::remove_if(fenced_bindings_.begin(), fenced_bindings_.end(), [&](const auto& fence) {
        for (const auto& p : policies_) for (const auto& b : p.bindings) if (b.binding.binding_id == fence.first && b.binding.binding_revision <= fence.second) return false;
        return !referenced([&](const auto& o) { return o.binding_id == fence.first && o.fence.binding_revision <= fence.second; });
    }), fenced_bindings_.end());
    closed_routes_.erase(std::remove_if(closed_routes_.begin(), closed_routes_.end(), [&](const auto& fence) {
        for (const auto& p : policies_) for (const auto& g : p.route_grants) if (g.session_id == fence.first && g.route_generation <= fence.second) return false;
        return !referenced([&](const auto& o) { return o.fence.session_id == fence.first && o.fence.route_generation <= fence.second; });
    }), closed_routes_.end());
}
bool LocalCecAgent::observe(const CecFrame& incoming, uint64_t now) {
    if (!incoming.port.valid() || incoming.initiator > 15 || incoming.destination > 15 || incoming.payload.size() > 15) return false;
    const auto* p = policy(incoming.port);
    if (!p || !p->owned_logical_address || p->authority_expires_at_ms <= now || incoming.payload.empty()) return true;
    if (incoming.payload.size() != 1 || incoming.initiator == 15 ||
        (incoming.destination != *p->owned_logical_address && incoming.destination != 15)) return true;
    CecFrame reply{p->port, *p->owned_logical_address, incoming.initiator, {}};
    const auto opcode = incoming.payload[0];
    if (opcode == 0x85 && incoming.destination == 15 && p->role == PortRole::playback_proxy) {
        // An incoming query cannot grant a route. Compile the existing grant.
        for (const auto& g : p->route_grants) {
            const auto* b = permission(*p, g.binding_id);
            if (!b || b->binding.kind != BindingKind::proxy_representation) continue;
            AuthorizedCecOperation operation;
            operation.component_id = b->binding.component_id; operation.binding_id = g.binding_id;
            operation.target = b->binding.endpoint; operation.kind = OperationKind::announce_active_source;
            operation.fence = {p->owner_boot_id, p->coordinator_epoch, p->config_revision, p->domain_revision,
                b->binding.binding_revision, g.session_id, g.route_generation, p->policy_generation};
            operation.execution_expires_at_ms = g.expires_at_ms;
            if (validate(operation, now, true)) {
                submit_frames(operation, *compile(operation, *p, *b), now, false, true); return true;
            }
        }
        return true;
    }
    if (incoming.destination != *p->owned_logical_address) return true;
    if (opcode == 0x83) {
        uint16_t pa = p->role == PortRole::tv_controller ? 0 : p->physical_address->physical_address;
        reply.destination = 15; reply.payload = {0x84, static_cast<uint8_t>(pa >> 8), static_cast<uint8_t>(pa), static_cast<uint8_t>(p->role == PortRole::tv_controller ? 0 : 4)};
    } else if (opcode == 0x46) {
        reply.payload = {0x47}; reply.payload.insert(reply.payload.end(), p->proxy_osd_name.begin(), p->proxy_osd_name.end());
    } else if (opcode == 0x9f) reply.payload = {0x9e, 0x05}; // Implemented CEC 1.4 feature set.
    else return true;
    submit_frames({}, {reply}, now, true);
    return true;
}
} // namespace amity::system
