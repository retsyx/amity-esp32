#include "amity/system/coordinator.hpp"
#include <limits>

namespace amity::system {
namespace {
bool same_arguments(const OperationArguments& a, const OperationArguments& b) {
    return a.on == b.on && a.key == b.key && a.physical_path == b.physical_path && a.query_opcode == b.query_opcode &&
        a.hold_token == b.hold_token && a.repeat == b.repeat;
}
bool same_operation(const AuthorizedCecOperation& a, const AuthorizedCecOperation& b) {
    return a.command_id == b.command_id && a.kind == b.kind && a.component_id == b.component_id && a.binding_id == b.binding_id &&
        a.target == b.target && a.fence == b.fence && a.execution_expires_at_ms == b.execution_expires_at_ms && same_arguments(a.arguments, b.arguments);
}
bool same_permission(const BindingPermission& a, const BindingPermission& b) {
    const auto& x = a.binding; const auto& y = b.binding;
    return x.binding_id == y.binding_id && x.component_id == y.component_id && x.capability == y.capability && x.kind == y.kind &&
        x.device_name == y.device_name && x.endpoint == y.endpoint && x.owner_octopus_id == y.owner_octopus_id && x.domain_id == y.domain_id &&
        x.domain_revision == y.domain_revision && x.binding_revision == y.binding_revision && x.role == y.role && x.status == y.status &&
        x.proxy_identity_resource_id == y.proxy_identity_resource_id && a.discrete_power_on == b.discrete_power_on &&
        a.discrete_power_off == b.discrete_power_off && a.image_view_on == b.image_view_on && a.exact_path_selection == b.exact_path_selection;
}
}
Coordinator::Coordinator(std::function<bool(uint8_t*, size_t)> random) : random_(std::move(random)) {}
Coordinator::Lane* Coordinator::lane(const CecPortRef& port) {
    for (auto& l : lanes_) if (l.port == port) return &l;
    return nullptr;
}
AdmissionResult Coordinator::install(Topology topology, std::vector<PortPolicySnapshot> policies, OctopusId local, uint64_t now) {
    if (!local || policies.size() > 10) return {AdmissionCode::invalid, "unsupported local port capacity"};
    for (size_t i = 0; i < policies.size(); ++i) {
        if (policies[i].port.octopus_id != local || !policies[i].route_grants.empty()) return {AdmissionCode::unauthorized, "configuration cannot install route authority"};
        for (size_t j = 0; j < i; ++j) if (policies[i].port == policies[j].port ||
            (policies[i].domain_id == policies[j].domain_id && policies[i].owned_logical_address &&
             policies[i].owned_logical_address == policies[j].owned_logical_address)) return {AdmissionCode::invalid, "duplicate port or domain ACK ownership"};
    }
    std::vector<Lane> candidate;
    for (const auto& p : policies) {
        candidate.push_back({p.port, LocalCecAgent{}});
        for (const auto& adapter : wire_adapters_) if (adapter.port == p.port) candidate.back().agent.set_wire_adapter(adapter.submit, adapter.cancel);
        auto result = candidate.back().agent.install_policy(p, now);
        if (!result) return result;
    }
    if (!quiescent()) return {AdmissionCode::stale, "old wire work must drain before activation"};
    local_ = local; topology_ = std::move(topology); policies_ = std::move(policies); lanes_ = std::move(candidate);
    planner_ = std::make_unique<ResourcePlanner>(topology_);
    plan_.reset(); execution_.reset(); commands_.clear(); held_.clear(); authorized_.clear(); remote_plan_.reset(); active_remote_plans_.clear(); remote_policies_.clear(); remote_owner_policies_.clear(); remote_progress_.clear(); error_.clear();
    return {AdmissionCode::accepted, {}};
}
bool Coordinator::can_rebase_revision(uint64_t next) const {
    if (!planner_ || plan_ || remote_plan_ || !planner_->can_rebase_revision(topology_.config_revision, next)) return false;
    return std::all_of(lanes_.begin(), lanes_.end(), [&](const auto& l) { return l.agent.can_rebase_revision(topology_.config_revision, next); });
}
bool Coordinator::rebase_revision(uint64_t next) {
    if (!can_rebase_revision(next)) return false;
    const auto previous = topology_.config_revision;
    for (auto& l : lanes_) if (!l.agent.rebase_revision(previous, next)) return false;
    for (auto& p : policies_) { p.config_revision = next; ++p.policy_generation; }
    for (auto& p : active_remote_plans_) p.config_revision = next;
    topology_.config_revision = next;
    return planner_->rebase_revision(previous, next);
}
AdmissionResult Coordinator::refresh_bindings(Topology topology, std::vector<PortPolicySnapshot> policies, uint64_t now) {
    if (!planner_ || topology.config_revision != topology_.config_revision || policies.size() != policies_.size())
        return {AdmissionCode::stale, "binding update belongs to another configuration"};
    const bool unchanged = std::all_of(policies_.begin(), policies_.end(), [&](const auto& old) {
        const auto next = std::find_if(policies.begin(), policies.end(), [&](const auto& p) { return p.port == old.port; });
        if (next == policies.end() || next->role != old.role || next->owned_logical_address != old.owned_logical_address ||
            next->domain_id != old.domain_id || next->domain_revision != old.domain_revision || next->bindings.size() != old.bindings.size() ||
            bool(next->physical_address) != bool(old.physical_address)) return false;
        if (old.physical_address && (next->physical_address->physical_address != old.physical_address->physical_address ||
            next->physical_address->confirmed != old.physical_address->confirmed || next->physical_address->provenance != old.physical_address->provenance ||
            next->physical_address->reporter != old.physical_address->reporter)) return false;
        return std::all_of(old.bindings.begin(), old.bindings.end(), [&](const auto& p) {
            return std::any_of(next->bindings.begin(), next->bindings.end(), [&](const auto& q) { return same_permission(p, q); });
        });
    }) && std::all_of(topology_.bindings.begin(), topology_.bindings.end(), [&](const auto& old) {
        return old.owner_octopus_id != local_ || std::any_of(topology.bindings.begin(), topology.bindings.end(), [&](const auto& next) {
            return next.binding_id == old.binding_id && next.endpoint == old.endpoint && next.binding_revision == old.binding_revision && next.status == old.status;
        });
    });
    // Real identity evidence was refreshed, but unchanged control policy does
    // not invalidate an in-progress plan, a hold, or prepared remote metadata.
    if (unchanged) return {AdmissionCode::accepted, {}};
    if (plan_) fail("bindings changed during execution; explicit reconciliation required");
    remote_plan_.reset(); remote_policies_.clear();
    for (const auto& binding : topology_.bindings) if (binding.owner_octopus_id != local_) topology.bindings.push_back(binding);
    for (const auto& owner : topology_.ready_owners) if (owner != local_ && std::find(topology.ready_owners.begin(), topology.ready_owners.end(), owner) == topology.ready_owners.end()) topology.ready_owners.push_back(owner);
    for (const auto& old : topology_.bindings) {
        auto next = std::find_if(topology.bindings.begin(), topology.bindings.end(), [&](const auto& b) { return b.binding_id == old.binding_id; });
        if (next == topology.bindings.end() || next->endpoint != old.endpoint || next->binding_revision != old.binding_revision || next->status != BindingStatus::verified)
            for (auto& l : lanes_) l.agent.fence_binding(old.binding_id, old.binding_revision, false);
    }
    // Preserve route grants for unchanged sessions, never issue an actuation.
    const auto qualified = [&](SessionId id) {
        const auto session = std::find_if(planner_->sessions().begin(), planner_->sessions().end(), [&](const auto& s) { return s.session_id == id; });
        if (session == planner_->sessions().end()) {
            const auto remote = std::find_if(active_remote_plans_.begin(), active_remote_plans_.end(), [&](const auto& plan) { return plan.proposed_session.session_id == id; });
            if (remote == active_remote_plans_.end()) return false;
            const auto route = std::find_if(topology.routes.begin(), topology.routes.end(), [&](const auto& r) { return r.route_id == remote->proposed_session.route_id; });
            return route != topology.routes.end() && std::none_of(route->required_bindings.begin(), route->required_bindings.end(), [&](const auto& binding) {
                const auto prior = std::find_if(topology_.bindings.begin(), topology_.bindings.end(), [&](const auto& b) { return b.binding_id == binding && b.owner_octopus_id == local_; });
                return prior != topology_.bindings.end() && std::none_of(topology.bindings.begin(), topology.bindings.end(), [&](const auto& b) {
                    return b.binding_id == binding && b.status == BindingStatus::verified && b.owner_octopus_id == local_;
                });
            });
        }
        const auto route = std::find_if(topology.routes.begin(), topology.routes.end(), [&](const auto& r) { return r.route_id == session->route_id; });
        return route != topology.routes.end() && std::none_of(route->required_bindings.begin(), route->required_bindings.end(), [&](const auto& binding) {
            return std::none_of(topology.bindings.begin(), topology.bindings.end(), [&](const auto& b) { return b.binding_id == binding && b.status == BindingStatus::verified &&
                std::find(topology.ready_owners.begin(), topology.ready_owners.end(), b.owner_octopus_id) != topology.ready_owners.end(); });
        });
    };
    for (auto& p : policies) {
        auto old = std::find_if(policies_.begin(), policies_.end(), [&](const auto& q) { return q.port == p.port; });
        if (old == policies_.end()) return {AdmissionCode::invalid, "port changed during binding update"};
        p.policy_generation = old->policy_generation + 1;
        for (const auto& g : old->route_grants) if (g.expires_at_ms > now && qualified(g.session_id) && std::any_of(p.bindings.begin(), p.bindings.end(), [&](const auto& b) { return b.binding.binding_id == g.binding_id; })) p.route_grants.push_back(g);
        for (const auto& session : planner_->sessions()) {
            const auto route = std::find_if(topology.routes.begin(), topology.routes.end(), [&](const auto& r) { return r.route_id == session.route_id; });
            if (route == topology.routes.end() || std::any_of(route->required_bindings.begin(), route->required_bindings.end(), [&](const auto& id) {
                return std::none_of(topology.bindings.begin(), topology.bindings.end(), [&](const auto& b) { return b.binding_id == id && b.status == BindingStatus::verified &&
                    std::find(topology.ready_owners.begin(), topology.ready_owners.end(), b.owner_octopus_id) != topology.ready_owners.end(); });
            })) continue;
            for (const auto& permission : p.bindings) if (std::find(route->required_bindings.begin(), route->required_bindings.end(), permission.binding.binding_id) != route->required_bindings.end() &&
                std::none_of(p.route_grants.begin(), p.route_grants.end(), [&](const auto& g) { return g.binding_id == permission.binding.binding_id && g.session_id == session.session_id && g.route_generation == session.route_generation; })) {
                RouteGrant grant{permission.binding.binding_id, session.session_id, session.route_generation, p.authority_expires_at_ms, {}};
                for (const auto& step : route->steps) if (step.binding_id == grant.binding_id && step.operation == OperationKind::select_path) grant.permitted_paths.push_back(step.arguments.physical_path);
                p.route_grants.push_back(std::move(grant));
            }
        }
    }
    const auto checked = check_policies(policies, now); if (!checked) return checked;
    if (!planner_->update_bindings(topology)) return {AdmissionCode::stale, "planner bindings could not synchronize"};
    const auto installed = install_policies(policies, now); if (!installed) return installed;
    topology_ = std::move(topology); policies_ = std::move(policies);
    return {AdmissionCode::accepted, {}};
}
PlanningResult Coordinator::select(const SelectRequest& request, uint64_t now) {
    if (request.admissible_until_ms && now >= request.admissible_until_ms) return {PlanningStatus::stale, {}, {}, "original input authority expired"};
    if (!planner_ || plan_ || remote_plan_) return {PlanningStatus::overloaded, {}, {}, "configuration unavailable or transition in progress"};
    auto result = planner_->select_activity(request);
    if (!result.plan) { error_ = result.reason; return result; }
    if (result.status == PlanningStatus::duplicate) return result;
    for (const auto& owner : result.plan->owners) if (owner != local_ && (!remote_hooks_.prepare || !remote_hooks_.fence || !remote_hooks_.activate || !remote_hooks_.send)) {
        planner_->abort_plan(result.plan->plan_id); return {PlanningStatus::unresolved, {}, {}, "remote owner has no synchronized execution adapter"};
    }
    plan_ = *result.plan; execution_ = std::make_unique<PlanExecution>(*plan_); commands_.clear(); error_.clear(); observed_evidence_ = false; verify_deadline_ = 0;
    remote_progress_.clear(); local_fenced_ = local_grants_installed_ = false; plan_deadline_ = now + 10000;
    for (const auto& owner : plan_->owners) {
        if (owner == local_) execution_->prepared(local_, true);
        else {
            remote_progress_.push_back({owner});
            if (!remote_hooks_.prepare(owner, *plan_)) { fail("remote preparation queue rejected plan"); return {PlanningStatus::overloaded, {}, {}, error_}; }
        }
    }
    tick(now, false); return result;
}
AdmissionResult Coordinator::check_policies(const std::vector<PortPolicySnapshot>& candidate, uint64_t now) const {
    for (const auto& p : candidate) {
        const auto local = std::find_if(lanes_.begin(), lanes_.end(), [&](const auto& l) { return l.port == p.port; });
        if (local == lanes_.end()) return {AdmissionCode::invalid, "missing native connection for policy"};
        const auto checked = local->agent.check_policy(p, now); if (!checked) return checked;
    }
    return {AdmissionCode::accepted, {}};
}
AdmissionResult Coordinator::install_policies(const std::vector<PortPolicySnapshot>& candidate, uint64_t now) {
    // Pure preflight all lanes before any installed generation, cancellation
    // guard, or grant changes. Owner serialization forbids completion reentry.
    const auto checked = check_policies(candidate, now); if (!checked) return checked;
    for (const auto& p : candidate) {
        const auto installed = lane(p.port)->agent.install_policy(p, now); if (!installed) return installed;
    }
    return {AdmissionCode::accepted, {}};
}
bool Coordinator::install_grants(uint64_t now) {
    auto candidate = policies_;
    for (auto& p : candidate) {
        p.route_grants.erase(std::remove_if(p.route_grants.begin(), p.route_grants.end(), [&](const auto& g) {
            return plan_->replaces_session && g.session_id == *plan_->replaces_session;
        }), p.route_grants.end());
        const auto route = std::find_if(topology_.routes.begin(), topology_.routes.end(), [&](const auto& r) { return r.route_id == plan_->proposed_session.route_id; });
        for (const auto& b : p.bindings) if (std::find(route->required_bindings.begin(), route->required_bindings.end(), b.binding.binding_id) != route->required_bindings.end()) {
            RouteGrant grant{b.binding.binding_id, plan_->proposed_session.session_id, plan_->proposed_session.route_generation, p.authority_expires_at_ms, {}};
            for (const auto& step : plan_->steps) if (step.binding_id == b.binding.binding_id && step.operation == OperationKind::select_path) grant.permitted_paths.push_back(step.arguments.physical_path);
            p.route_grants.push_back(std::move(grant));
        }
        ++p.policy_generation;
    }
    const auto installed = install_policies(candidate, now);
    if (!installed) { error_ = installed.reason; return false; }
    policies_ = std::move(candidate); return true;
}
AuthorizedCecOperation Coordinator::operation(const ComponentBinding& b, const Session& s, OperationKind kind, const OperationArguments& args, uint64_t now) {
    const auto& source = b.owner_octopus_id == local_ ? policies_ : remote_owner_policies_;
    const auto p = std::find_if(source.begin(), source.end(), [&](const auto& p) { return p.port == b.endpoint.port; });
    if (p == source.end() || p->config_revision != topology_.config_revision) return {};
    auto expiry = std::min(p->authority_expires_at_ms, now + 5000);
    if (plan_ && plan_->proposed_session.session_id == s.session_id && plan_->execution_expires_at_ms)
        expiry = std::min(expiry, plan_->execution_expires_at_ms);
    return {fresh<CommandId>(), kind, b.component_id, b.binding_id, b.endpoint,
        {p->owner_boot_id, p->coordinator_epoch, p->config_revision, b.domain_revision, b.binding_revision,
         s.session_id, s.route_generation, p->policy_generation},
        expiry, args};
}
void Coordinator::fail(const std::string& reason) {
    error_ = reason;
    if (plan_) {
        if (remote_hooks_.abort) for (const auto& remote : remote_progress_) remote_hooks_.abort(remote.owner, *plan_);
        for (auto& l : lanes_) l.agent.close_route(plan_->proposed_session.session_id, plan_->proposed_session.route_generation);
        const bool uncertain_effects = execution_ && std::any_of(execution_->step_results().begin(), execution_->step_results().end(), [](auto r) { return r != StepResult::not_sent; });
        if (uncertain_effects) planner_->commit_plan(plan_->plan_id, false); else planner_->abort_plan(plan_->plan_id);
    }
    plan_.reset(); execution_.reset(); commands_.clear(); remote_progress_.clear(); plan_deadline_ = 0;
}
void Coordinator::tick(uint64_t now, bool allow_repeat) {
    for (auto& l : lanes_) l.agent.tick(now, allow_repeat);
    std::vector<OctopusId> expired;
    for (const auto& p : remote_owner_policies_) if (p.authority_expires_at_ms <= now &&
        std::find(expired.begin(), expired.end(), p.port.octopus_id) == expired.end() &&
        std::none_of(remote_owner_policies_.begin(), remote_owner_policies_.end(), [&](const auto& other) { return other.port.octopus_id == p.port.octopus_id && other.authority_expires_at_ms > now; })) expired.push_back(p.port.octopus_id);
    for (auto owner : expired) update_remote_owner(owner, {}, false, 0, now);
    for (auto& record : authorized_) if (record.outcome.result == StepResult::admitted) {
        auto* l = lane(record.operation.target.port);
        if (l && !l->agent.command_pending(record.operation.command_id)) record.outcome = {record.operation.command_id, StepResult::timed_out, "command expired or was fenced before wire start"};
    }
    if (!plan_) return;
    if (plan_->execution_expires_at_ms && now >= plan_->execution_expires_at_ms) { fail("original input authority expired during planning or execution"); return; }
    if (plan_deadline_ && now >= plan_deadline_) { fail("distributed execution or observation deadline elapsed"); return; }
    if (verify_deadline_ && now >= verify_deadline_ && execution_->status() == TransitionStatus::executing) {
        fail("required observation or operation deadline elapsed"); return;
    }
    for (const auto& command : commands_) if (execution_->step_results()[command.step] == StepResult::admitted &&
        command.port.octopus_id == local_ && !lane(command.port)->agent.command_pending(command.command_id)) {
        fail("operation expired or was fenced before wire start"); return;
    }
    if (execution_->status() == TransitionStatus::fencing) {
        std::optional<Session> old;
        if (plan_->replaces_session) for (const auto& session : planner_->sessions()) if (session.session_id == *plan_->replaces_session) old = session;
        if (!local_fenced_) {
            if (old) for (auto& l : lanes_) l.agent.close_route(old->session_id, old->route_generation);
            local_fenced_ = true;
        }
        for (auto& remote : remote_progress_) if (!remote.fence_sent) {
            remote.fence_sent = true;
            if (!remote_hooks_.fence(remote.owner, *plan_, old)) { fail("remote fence queue rejected plan"); return; }
        }
        if (old) for (auto& l : lanes_) if (!l.agent.quiescent(old->session_id, old->route_generation)) return;
        for (const auto& remote : remote_progress_) if (!remote.fenced) return;
        if (!local_grants_installed_) {
            if (!install_grants(now)) { fail(error_.empty() ? "prepared local policy could not install" : error_); return; }
            local_grants_installed_ = true;
            if (std::find(plan_->owners.begin(), plan_->owners.end(), local_) != plan_->owners.end()) execution_->barrier_complete(local_);
        }
        for (auto& remote : remote_progress_) if (!remote.activate_sent) {
            remote.activate_sent = true;
            if (!remote_hooks_.activate(remote.owner, *plan_, 5000)) { fail("remote activation queue rejected plan"); return; }
        }
    }
    if (execution_->status() == TransitionStatus::executing) for (auto index : execution_->ready_steps()) {
        const auto& step = plan_->steps[index];
        auto b = std::find_if(topology_.bindings.begin(), topology_.bindings.end(), [&](const auto& b) { return b.binding_id == step.binding_id; });
        if (b == topology_.bindings.end()) { fail("step binding no longer resolved"); return; }
        auto op = operation(*b, plan_->proposed_session, step.operation, step.arguments, now);
        auto* target = lane(b->endpoint.port);
        auto result = target ? target->agent.execute(op, now) : AdmissionResult{
            op.execution_expires_at_ms > now && remote_hooks_.send && remote_hooks_.send(b->owner_octopus_id, plan_->plan_id, op) ? AdmissionCode::accepted : AdmissionCode::overloaded, "remote operation queue admission failed"};
        if (!result) { execution_->record(index, StepResult::rejected); fail(result.reason); return; }
        commands_.push_back({op.command_id, index, b->endpoint.port}); execution_->record(index, StepResult::admitted);
        if (!verify_deadline_) verify_deadline_ = now + 10000;
    }
    if (execution_->status() == TransitionStatus::active || execution_->status() == TransitionStatus::verifying) {
        verify_deadline_ = verify_deadline_ ? std::min(verify_deadline_, now + 3000) : now + 3000;
        if (execution_->status() == TransitionStatus::active || now >= verify_deadline_) {
            // A completed wire plan alone never establishes picture or power.
            if (execution_->status() == TransitionStatus::verifying) error_ = "required equipment state was not observed before deadline";
            planner_->commit_plan(plan_->plan_id, observed_evidence_ && execution_->status() == TransitionStatus::active); plan_.reset(); execution_.reset(); commands_.clear(); remote_progress_.clear(); verify_deadline_ = plan_deadline_ = 0;
        }
    }
}
void Coordinator::set_wire_adapter(const CecPortRef& port, WireSubmit submit, WireCancel cancel) {
    auto found = std::find_if(wire_adapters_.begin(), wire_adapters_.end(), [&](const auto& adapter) { return adapter.port == port; });
    if (found == wire_adapters_.end()) {
        if (wire_adapters_.size() >= 10) return;
        wire_adapters_.push_back({port, std::move(submit), std::move(cancel)}); found = wire_adapters_.end() - 1;
    } else { found->submit = std::move(submit); found->cancel = std::move(cancel); }
    if (auto* local = lane(port)) local->agent.set_wire_adapter(found->submit, found->cancel);
}
bool Coordinator::local_repeat_ready(uint64_t now) const {
    return std::any_of(lanes_.begin(), lanes_.end(), [&](const auto& l) { return l.agent.repeat_ready(now); });
}
bool Coordinator::local_repeat_in_flight() const {
    return std::any_of(lanes_.begin(), lanes_.end(), [](const auto& l) { return l.agent.repeating_frame_in_flight(); });
}
std::vector<AuthorizedCecOperation> Coordinator::active_key_contexts() const {
    std::vector<AuthorizedCecOperation> result;
    for (const auto& l : lanes_) {
        auto active = l.agent.active_key_contexts();
        result.insert(result.end(), active.begin(), active.end());
    }
    return result;
}
void Coordinator::complete_frame(const CecPortRef& port, uint64_t work_id, WireOutcome outcome, uint64_t now, bool started) {
    auto* l = lane(port); if (!l) return;
    const auto completed = l->agent.complete_frame(work_id, outcome, now, started); if (!completed) return;
    const auto command = *completed;
    const bool applied = outcome == WireOutcome::success;
    for (auto& record : authorized_) if (record.operation.command_id == command) {
        if (!applied) {
            const char* reason = outcome == WireOutcome::nack ? "nack" : outcome == WireOutcome::arbitration_lost ? "arbitration_lost" :
                outcome == WireOutcome::canceled ? "canceled" : outcome == WireOutcome::expired ? "expired" : outcome == WireOutcome::admission_rejected ? "admission_rejected" : "line_error";
            record.outcome = {command, outcome == WireOutcome::canceled || outcome == WireOutcome::admission_rejected ? StepResult::rejected : outcome == WireOutcome::expired ? StepResult::timed_out : StepResult::unknown, reason};
        } else if (!l->agent.command_pending(command) && record.outcome.result == StepResult::admitted)
            record.outcome = {command, record.observed ? StepResult::observed : StepResult::applied, {}};
    }
    auto step = std::find_if(commands_.begin(), commands_.end(), [&](const auto& s) { return s.command_id == command; });
    if (step != commands_.end() && execution_) {
        if (!applied) { execution_->record(step->step, StepResult::unknown); fail("wire operation failed or has uncertain outcome"); }
        else if (!l->agent.command_pending(command)) execution_->record(step->step, StepResult::applied);
    }
    tick(now, false);
}
AdmissionResult Coordinator::key(ZoneId zone, ComponentId component, uint8_t key, bool held, HoldToken token, uint64_t now, uint64_t deadline) {
    if (deadline && now >= deadline) return {AdmissionCode::expired, "original input authority expired"};
    if (!planner_ || plan_ || !token) return {AdmissionCode::stale, "no stable route for input"};
    auto session = std::find_if(planner_->sessions().begin(), planner_->sessions().end(), [&](const auto& s) { return s.zone_id == zone && s.status != SessionStatus::suspended_after_restart; });
    if (session == planner_->sessions().end()) return {AdmissionCode::unauthorized, "zone has no applied session"};
    const bool volume = (key >= 0x41 && key <= 0x43) || key == 0x65 || key == 0x66;
    auto b = std::find_if(topology_.bindings.begin(), topology_.bindings.end(), [&](const auto& b) { return b.component_id == component && b.status == BindingStatus::verified &&
        (volume ? b.capability == Capability::volume || b.capability == Capability::audio : b.capability == Capability::navigation); });
    if (b == topology_.bindings.end()) return {AdmissionCode::unsupported, "input capability unresolved"};
    if (held_.size() >= 64) return {AdmissionCode::overloaded, "held input capacity exhausted"};
    OperationArguments args; args.key = key; args.hold_token = token; args.repeat = held;
    auto op = operation(*b, *session, OperationKind::press_key, args, now);
    if (deadline) op.execution_expires_at_ms = std::min(op.execution_expires_at_ms, deadline);
    auto* local = lane(b->endpoint.port);
    auto result = local ? local->agent.execute(op, now) : AdmissionResult{
        op.execution_expires_at_ms > now && remote_hooks_.send && remote_hooks_.send(b->owner_octopus_id, session->plan_id, op) ? AdmissionCode::accepted : AdmissionCode::expired, "remote input authority unavailable"};
    if (result && held && !local) held_.push_back({token, op});
    return result;
}
bool Coordinator::release(HoldToken token, uint64_t now) {
    for (const auto& held : held_) if (held.token == token && held.operation.target.port.octopus_id != local_) {
        auto release = held.operation; release.command_id = fresh<CommandId>(); release.kind = OperationKind::release_key;
        PlanId plan; if (planner_) for (const auto& session : planner_->sessions()) if (session.session_id == release.fence.session_id) plan = session.plan_id;
        const bool sent = plan && remote_hooks_.send && remote_hooks_.send(release.target.port.octopus_id, plan, release);
        held_.erase(std::remove_if(held_.begin(), held_.end(), [&](const auto& h) { return h.token == token; }), held_.end()); return sent;
    }
    held_.erase(std::remove_if(held_.begin(), held_.end(), [&](const auto& hold) { return hold.token == token; }), held_.end());
    for (auto& l : lanes_) if (l.agent.release_hold(token, now)) return true;
    return false;
}
AdmissionResult Coordinator::execute_authorized(const AuthorizedCecOperation& operation, uint64_t now) {
    authorized_.erase(std::remove_if(authorized_.begin(), authorized_.end(), [&](const auto& r) { return r.operation.execution_expires_at_ms <= now; }), authorized_.end());
    for (const auto& record : authorized_) if (record.operation.command_id == operation.command_id)
        return same_operation(record.operation, operation) ? record.result : AdmissionResult{AdmissionCode::stale, "command ID reused with different authority or arguments"};
    if (authorized_.size() >= 64) return {AdmissionCode::overloaded, "command admission cache is full"};
    auto* target = lane(operation.target.port);
    if (!target) return {AdmissionCode::unauthorized, "operation target is not a local port"};
    if (operation.kind != OperationKind::release_key) {
        auto renewed = target->agent.renew_authority(operation, now); if (!renewed) return renewed;
        for (auto& p : policies_) if (p.port == operation.target.port) {
            p.authority_expires_at_ms = std::max(p.authority_expires_at_ms, operation.execution_expires_at_ms);
            for (auto& g : p.route_grants) if (g.session_id == operation.fence.session_id && g.route_generation == operation.fence.route_generation)
                g.expires_at_ms = std::max(g.expires_at_ms, operation.execution_expires_at_ms);
        }
    }
    auto result = target->agent.execute(operation, now);
    if (operation.execution_expires_at_ms > now && operation.execution_expires_at_ms - now <= 5000)
        authorized_.push_back({operation, result, {operation.command_id, result ? StepResult::admitted : StepResult::rejected, result.reason}, false});
    return result;
}
AdmissionResult Coordinator::prepare_remote_plan(const Plan& plan, const std::vector<PortPolicySnapshot>& supplied, uint64_t now) {
    if (active_remote_plans_.size() >= 64 || std::any_of(active_remote_plans_.begin(), active_remote_plans_.end(), [&](const auto& active) { return active.plan_id == plan.plan_id; }))
        return {AdmissionCode::overloaded, "active remote plan capacity or ID reuse"};
    if (plan_ || (remote_plan_ && remote_plan_->plan_id != plan.plan_id)) return {AdmissionCode::overloaded, "another plan is prepared"};
    if (!plan.plan_id || !plan.proposed_session.session_id || !plan.proposed_session.route_generation ||
        plan.config_revision != topology_.config_revision || supplied.size() != policies_.size()) return {AdmissionCode::stale, "plan does not match installed configuration"};
    const auto route = std::find_if(topology_.routes.begin(), topology_.routes.end(), [&](const auto& r) { return r.route_id == plan.proposed_session.route_id; });
    const auto activity = std::find_if(topology_.activities.begin(), topology_.activities.end(), [&](const auto& a) {
        return a.activity_id == plan.proposed_session.activity_id && a.zone_id == plan.proposed_session.zone_id && a.route_id == plan.proposed_session.route_id;
    });
    if (route == topology_.routes.end() || activity == topology_.activities.end()) return {AdmissionCode::unauthorized, "plan route is not committed"};
    for (const auto& step : plan.steps) if (std::none_of(route->steps.begin(), route->steps.end(), [&](const auto& expected) {
        return expected.binding_id == step.binding_id && expected.operation == step.operation && same_arguments(expected.arguments, step.arguments) && expected.require_observation == step.require_observation;
    })) return {AdmissionCode::unauthorized, "plan contains an unconfigured semantic step"};
    std::vector<PortPolicySnapshot> candidate;
    for (const auto& current : policies_) {
        const auto next = std::find_if(supplied.begin(), supplied.end(), [&](const auto& p) { return p.port == current.port; });
        if (next == supplied.end() || next->owner_boot_id != current.owner_boot_id || next->coordinator_epoch != current.coordinator_epoch ||
            next->config_revision != current.config_revision || next->domain_id != current.domain_id || next->domain_revision != current.domain_revision ||
            next->policy_generation != current.policy_generation + 1 || next->bindings.size() != current.bindings.size()) return {AdmissionCode::stale, "prepared policy does not match exact current owner snapshot"};
        for (const auto& permission : current.bindings) if (std::none_of(next->bindings.begin(), next->bindings.end(), [&](const auto& p) { return same_permission(permission, p); }))
            return {AdmissionCode::stale, "prepared binding changed after synchronization"};
        auto policy = current; ++policy.policy_generation; policy.authority_expires_at_ms = now + 5000;
        policy.route_grants.erase(std::remove_if(policy.route_grants.begin(), policy.route_grants.end(), [&](const auto& g) { return g.expires_at_ms <= now; }), policy.route_grants.end());
        for (const auto& grant : next->route_grants) {
            if (grant.session_id != plan.proposed_session.session_id || grant.route_generation != plan.proposed_session.route_generation) continue;
            if (std::find(route->required_bindings.begin(), route->required_bindings.end(), grant.binding_id) == route->required_bindings.end() ||
                std::none_of(current.bindings.begin(), current.bindings.end(), [&](const auto& p) { return p.binding.binding_id == grant.binding_id; }))
                return {AdmissionCode::unauthorized, "route grant is outside the committed local route"};
            std::vector<uint16_t> paths;
            for (const auto& step : route->steps) if (step.binding_id == grant.binding_id && step.operation == OperationKind::select_path) paths.push_back(step.arguments.physical_path);
            if (paths != grant.permitted_paths) return {AdmissionCode::unauthorized, "path grant differs from committed route"};
            auto exact = grant; exact.expires_at_ms = now + 5000; policy.route_grants.push_back(std::move(exact));
        }
        for (auto id : route->required_bindings) if (std::any_of(current.bindings.begin(), current.bindings.end(), [&](const auto& p) { return p.binding.binding_id == id; }) &&
            std::none_of(policy.route_grants.begin(), policy.route_grants.end(), [&](const auto& g) { return g.binding_id == id && g.session_id == plan.proposed_session.session_id && g.route_generation == plan.proposed_session.route_generation; }))
                return {AdmissionCode::unauthorized, "required local route binding has no grant"};
        LocalCecAgent validator; auto valid = validator.install_policy(policy, now); if (!valid) return valid;
        candidate.push_back(std::move(policy));
    }
    remote_plan_ = plan; remote_policies_ = std::move(candidate); return {AdmissionCode::accepted, {}};
}
AdmissionResult Coordinator::fence_remote_route(SessionId session, uint64_t generation) {
    if (!session || !generation) return {AdmissionCode::invalid, "invalid route fence"};
    for (auto& l : lanes_) l.agent.close_route(session, generation);
    active_remote_plans_.erase(std::remove_if(active_remote_plans_.begin(), active_remote_plans_.end(), [&](const auto& plan) {
        return plan.proposed_session.session_id == session && plan.proposed_session.route_generation <= generation;
    }), active_remote_plans_.end());
    return {AdmissionCode::accepted, {}};
}
bool Coordinator::remote_route_quiescent(SessionId session, uint64_t generation) const {
    for (const auto& l : lanes_) if (!l.agent.quiescent(session, generation)) return false;
    return true;
}
AdmissionResult Coordinator::renew_discovery_authority(uint32_t ttl, uint64_t now) {
    if (!ttl || ttl > 5000 || now > UINT64_MAX - ttl) return {AdmissionCode::invalid, "fresh bounded discovery authority required"};
    for (auto& policy : policies_) {
        auto* local = lane(policy.port); if (!local) return {AdmissionCode::unauthorized, "missing local identity lane"};
        auto result = local->agent.renew_discovery_authority(policy.port, ttl, now); if (!result) return result;
        policy.authority_expires_at_ms = std::max(policy.authority_expires_at_ms, now + ttl);
    }
    return {AdmissionCode::accepted, {}};
}
AdmissionResult Coordinator::activate_remote_plan(PlanId id, uint32_t ttl, uint64_t now) {
    if (!remote_plan_ || remote_plan_->plan_id != id || !ttl || ttl > 5000) return {AdmissionCode::stale, "unknown prepared plan or invalid receiver authority lease"};
    if (remote_plan_->replaces_session) for (const auto& p : policies_) for (const auto& g : p.route_grants)
        if (g.session_id == *remote_plan_->replaces_session && !remote_route_quiescent(g.session_id, g.route_generation)) return {AdmissionCode::stale, "old route has not drained"};
    for (auto& p : remote_policies_) {
        p.authority_expires_at_ms = now + ttl;
        p.route_grants.erase(std::remove_if(p.route_grants.begin(), p.route_grants.end(), [&](const auto& g) { return remote_plan_->replaces_session && g.session_id == *remote_plan_->replaces_session; }), p.route_grants.end());
        for (auto& g : p.route_grants) g.expires_at_ms = std::min(g.expires_at_ms, p.authority_expires_at_ms);
    }
    const auto installed = install_policies(remote_policies_, now); if (!installed) return installed;
    policies_ = std::move(remote_policies_);
    active_remote_plans_.push_back(*remote_plan_); remote_plan_.reset(); return {AdmissionCode::accepted, {}};
}
void Coordinator::abort_remote_plan(PlanId id) {
    if (remote_plan_ && remote_plan_->plan_id == id) { remote_plan_.reset(); remote_policies_.clear(); }
    auto active = std::find_if(active_remote_plans_.begin(), active_remote_plans_.end(), [&](const auto& plan) { return plan.plan_id == id; });
    if (active != active_remote_plans_.end()) { const auto session = active->proposed_session; fence_remote_route(session.session_id, session.route_generation); }
}
AdmissionResult Coordinator::renew_remote_authority(PlanId id, SessionId session, uint64_t generation, uint32_t ttl, uint64_t now) {
    if (!ttl || ttl > 5000 || std::none_of(active_remote_plans_.begin(), active_remote_plans_.end(), [&](const auto& plan) {
        return plan.plan_id == id && plan.proposed_session.session_id == session && plan.proposed_session.route_generation == generation;
    })) return {AdmissionCode::stale, "no exact active remote plan for receiver authority renewal"};
    bool renewed = false;
    for (auto& p : policies_) {
        auto grant = std::find_if(p.route_grants.begin(), p.route_grants.end(), [&](const auto& g) { return g.session_id == session && g.route_generation == generation; });
        if (grant == p.route_grants.end()) continue;
        const auto permission = std::find_if(p.bindings.begin(), p.bindings.end(), [&](const auto& b) { return b.binding.binding_id == grant->binding_id; });
        if (permission == p.bindings.end()) return {AdmissionCode::stale, "active route binding disappeared"};
        const auto& binding = permission->binding;
        AuthorizedCecOperation authority{fresh<CommandId>(), binding.kind == BindingKind::native_appliance ? OperationKind::query_state : OperationKind::announce_active_source,
            binding.component_id, binding.binding_id, binding.endpoint,
            {p.owner_boot_id, p.coordinator_epoch, p.config_revision, binding.domain_revision, binding.binding_revision, session, generation, p.policy_generation}, now + ttl, {}};
        auto result = lane(p.port)->agent.renew_authority(authority, now); if (!result) return result;
        p.authority_expires_at_ms = std::max(p.authority_expires_at_ms, now + ttl);
        for (auto& g : p.route_grants) if (g.session_id == session && g.route_generation == generation) g.expires_at_ms = std::max(g.expires_at_ms, now + ttl);
        renewed = true;
    }
    return {renewed ? AdmissionCode::accepted : AdmissionCode::stale, renewed ? std::string{} : "active route has no current local grants"};
}
AdmissionResult Coordinator::update_remote_owner(OctopusId owner, std::vector<PortPolicySnapshot> snapshots, bool ready, uint32_t ttl, uint64_t now) {
    if (!planner_ || !owner || owner == local_ || (ready && (!ttl || ttl > 5000))) return {AdmissionCode::invalid, "invalid synchronized owner snapshot"};
    for (const auto& p : snapshots) {
        if (p.port.octopus_id != owner || p.config_revision != topology_.config_revision || !p.owner_boot_id || !p.coordinator_epoch || !p.policy_generation)
            return {AdmissionCode::stale, "owner snapshot has another identity or configuration"};
        for (const auto& b : p.bindings) if (b.binding.owner_octopus_id != owner || b.binding.endpoint.port != p.port ||
            !b.binding.binding_id || !b.binding.component_id || !b.binding.binding_revision || b.binding.domain_id != p.domain_id ||
            b.binding.domain_revision != p.domain_revision || b.binding.role != p.role || b.binding.endpoint.logical_address > 15 ||
            static_cast<unsigned>(b.binding.status) > static_cast<unsigned>(BindingStatus::retired) ||
            (b.binding.status == BindingStatus::verified && !b.binding.endpoint.valid()))
            return {AdmissionCode::unauthorized, "owner snapshot includes an invalid or nonlocal binding"};
    }
    bool changed = (std::find(topology_.ready_owners.begin(), topology_.ready_owners.end(), owner) != topology_.ready_owners.end()) != ready;
    if (static_cast<size_t>(std::count_if(remote_owner_policies_.begin(), remote_owner_policies_.end(), [&](const auto& p) { return p.port.octopus_id == owner; })) != snapshots.size()) changed = true;
    for (const auto& p : snapshots) {
        auto old = std::find_if(remote_owner_policies_.begin(), remote_owner_policies_.end(), [&](const auto& old) { return old.port == p.port; });
        if (old == remote_owner_policies_.end() || old->owner_boot_id != p.owner_boot_id || old->coordinator_epoch != p.coordinator_epoch ||
            old->policy_generation != p.policy_generation || old->bindings.size() != p.bindings.size()) changed = true;
        else for (const auto& binding : p.bindings) if (std::none_of(old->bindings.begin(), old->bindings.end(), [&](const auto& b) { return same_permission(binding, b); })) changed = true;
    }
    if (changed && plan_) fail("remote owner binding or authority changed during plan execution");
    if (!ready) for (const auto& session : planner_->sessions()) {
        auto route = std::find_if(topology_.routes.begin(), topology_.routes.end(), [&](const auto& r) { return r.route_id == session.route_id; });
        if (route != topology_.routes.end() && std::any_of(topology_.bindings.begin(), topology_.bindings.end(), [&](const auto& b) {
            return b.owner_octopus_id == owner && std::find(route->required_bindings.begin(), route->required_bindings.end(), b.binding_id) != route->required_bindings.end();
        })) for (auto& l : lanes_) l.agent.close_route(session.session_id, session.route_generation);
    }
    auto topology = topology_;
    std::vector<ComponentBinding> unavailable;
    for (const auto& binding : topology.bindings) if (binding.owner_octopus_id == owner &&
        (!ready || std::none_of(snapshots.begin(), snapshots.end(), [&](const auto& p) {
            return std::any_of(p.bindings.begin(), p.bindings.end(), [&](const auto& b) { return b.binding.binding_id == binding.binding_id; });
        }))) { auto cached = binding; cached.status = BindingStatus::unresolved; unavailable.push_back(std::move(cached)); }
    topology.bindings.erase(std::remove_if(topology.bindings.begin(), topology.bindings.end(), [&](const auto& b) { return b.owner_octopus_id == owner; }), topology.bindings.end());
    topology.bindings.insert(topology.bindings.end(), unavailable.begin(), unavailable.end());
    topology.ready_owners.erase(std::remove(topology.ready_owners.begin(), topology.ready_owners.end(), owner), topology.ready_owners.end());
    remote_owner_policies_.erase(std::remove_if(remote_owner_policies_.begin(), remote_owner_policies_.end(), [&](const auto& p) { return p.port.octopus_id == owner; }), remote_owner_policies_.end());
    if (ready) {
        topology.ready_owners.push_back(owner);
        for (auto& p : snapshots) {
            p.authority_expires_at_ms = now + ttl;
            for (const auto& b : p.bindings) topology.bindings.push_back(b.binding);
            remote_owner_policies_.push_back(std::move(p));
        }
    }
    if (changed && !planner_->update_bindings(topology)) return {AdmissionCode::stale, "planner could not accept owner synchronization"};
    topology_ = std::move(topology); return {AdmissionCode::accepted, {}};
}
bool Coordinator::remote_plan_result(OctopusId owner, PlanId id, RemotePlanPhase phase, bool accepted, const std::string& error, uint64_t now) {
    if (!plan_ && remote_plan_ && remote_plan_->plan_id == id && phase == RemotePlanPhase::fence) {
        auto remote = std::find_if(remote_progress_.begin(), remote_progress_.end(), [&](const auto& state) { return state.owner == owner; });
        if (remote == remote_progress_.end()) return false;
        if (!accepted) { error_ = error.empty() ? "remote stop barrier rejected; resources remain reserved" : error; return true; }
        remote->fenced = true;
        if (std::all_of(remote_progress_.begin(), remote_progress_.end(), [](const auto& state) { return state.fenced; })) {
            planner_->stop_session(remote_plan_->proposed_session.session_id); remote_plan_.reset(); remote_progress_.clear(); error_.clear();
        }
        tick(now, false); return true;
    }
    if (!plan_ || plan_->plan_id != id) return false;
    auto remote = std::find_if(remote_progress_.begin(), remote_progress_.end(), [&](const auto& state) { return state.owner == owner; });
    if (remote == remote_progress_.end()) return false;
    if (!accepted) { fail(error.empty() ? "remote owner rejected prepared plan" : error); return true; }
    if (phase == RemotePlanPhase::prepare) {
        if (execution_->status() != TransitionStatus::preparing) return true;
        execution_->prepared(owner, true);
    } else if (phase == RemotePlanPhase::fence) {
        if (!remote->fence_sent) return false;
        remote->fenced = true;
    } else {
        if (!remote->activate_sent) return false;
        if (!remote->activated) {
            remote->activated = true;
            for (auto& p : remote_owner_policies_) if (p.port.octopus_id == owner) { ++p.policy_generation; p.authority_expires_at_ms = now + 5000; }
            execution_->barrier_complete(owner);
        }
    }
    tick(now, false); return true;
}
bool Coordinator::remote_operation_result(OctopusId owner, CommandId command, StepResult result, const std::string& error, uint64_t now) {
    auto hold = std::find_if(held_.begin(), held_.end(), [&](const auto& h) { return h.operation.command_id == command && h.operation.target.port.octopus_id == owner; });
    if (hold != held_.end()) {
        // Transmission outcomes describe a command, not the physical button's
        // state. Keep exact cleanup until its matching up/cancel/route fence.
        return true;
    }
    if (!plan_ || !execution_) return false;
    auto pending = std::find_if(commands_.begin(), commands_.end(), [&](const auto& c) { return c.command_id == command && c.port.octopus_id == owner; });
    if (pending == commands_.end()) return false;
    if (result == StepResult::rejected || result == StepResult::timed_out || result == StepResult::unknown) {
        execution_->record(pending->step, result); fail(error.empty() ? "remote operation failed or outcome is unknown" : error); return true;
    }
    if (result != StepResult::admitted && result != StepResult::applied && result != StepResult::observed) return false;
    if (!execution_->record(pending->step, result)) return false;
    if (result == StepResult::observed) observed_evidence_ = true;
    tick(now, false); return true;
}
bool Coordinator::stop(ZoneId zone, uint64_t now) {
    if (!planner_ || plan_) return false;
    if (remote_plan_) {
        if (remote_plan_->proposed_session.zone_id != zone) return false;
        for (auto& remote : remote_progress_) if (!remote.fenced && remote_hooks_.fence) remote_hooks_.fence(remote.owner, *remote_plan_, remote_plan_->proposed_session);
        return true;
    }
    for (const auto& s : planner_->sessions()) if (s.zone_id == zone) {
        const auto id = s.session_id; for (auto& l : lanes_) l.agent.close_route(id, s.route_generation);
        const auto route = std::find_if(topology_.routes.begin(), topology_.routes.end(), [&](const auto& r) { return r.route_id == s.route_id; });
        std::vector<OctopusId> owners;
        if (route != topology_.routes.end()) for (const auto& b : topology_.bindings) if (b.owner_octopus_id != local_ &&
            std::find(route->required_bindings.begin(), route->required_bindings.end(), b.binding_id) != route->required_bindings.end() &&
            std::find(owners.begin(), owners.end(), b.owner_octopus_id) == owners.end()) owners.push_back(b.owner_octopus_id);
        if (!owners.empty()) {
            remote_plan_.emplace(); remote_plan_->plan_id = s.plan_id; remote_plan_->proposed_session = s; remote_plan_->config_revision = topology_.config_revision;
            remote_progress_.clear();
            for (auto owner : owners) {
                remote_progress_.push_back({owner, true});
                if (!remote_hooks_.fence || !remote_hooks_.fence(owner, *remote_plan_, s)) error_ = "remote stop barrier unavailable; resources remain reserved";
            }
            tick(now, false); return true;
        }
        planner_->stop_session(id); tick(now, false); return true;
    }
    return true;
}
void Coordinator::observe(const CecFrame& frame, uint64_t now) {
    if (auto* l = lane(frame.port)) l->agent.observe(frame, now);
    if (frame.payload.size() >= 2) for (auto& record : authorized_) {
        const auto& op = record.operation;
        if (op.execution_expires_at_ms <= now || op.target.port != frame.port || op.target.logical_address != frame.initiator) continue;
        const auto current = std::find_if(policies_.begin(), policies_.end(), [&](const auto& p) { return p.port == op.target.port; });
        if (current == policies_.end() || current->owner_boot_id != op.fence.owner_boot_id || current->coordinator_epoch != op.fence.coordinator_epoch ||
            current->config_revision != op.fence.config_revision || current->policy_generation != op.fence.policy_generation ||
            std::none_of(current->bindings.begin(), current->bindings.end(), [&](const auto& b) { return b.binding.binding_id == op.binding_id && b.binding.binding_revision == op.fence.binding_revision && b.binding.endpoint == op.target; })) continue;
        const bool power = (op.kind == OperationKind::ensure_power || (op.kind == OperationKind::query_state && op.arguments.query_opcode == 0x8f)) &&
            frame.payload[0] == 0x90 && frame.payload[1] == (op.arguments.on ? 0 : 1);
        const bool audio = op.kind == OperationKind::query_state && op.arguments.query_opcode == 0x71 && frame.payload[0] == 0x7a;
        if (power || audio) { record.observed = true; if (record.outcome.result == StepResult::applied) record.outcome.result = StepResult::observed; }
    }
    if (!plan_ || frame.payload.size() < 2) return;
    for (const auto& command : commands_) {
        const auto& step = plan_->steps[command.step];
        if (!step.require_observation) continue;
        const auto binding = std::find_if(topology_.bindings.begin(), topology_.bindings.end(), [&](const auto& b) { return b.binding_id == step.binding_id; });
        if (binding == topology_.bindings.end() || binding->endpoint.port != frame.port || binding->endpoint.logical_address != frame.initiator) continue;
        const bool power = (step.operation == OperationKind::ensure_power || (step.operation == OperationKind::query_state && step.arguments.query_opcode == 0x8f)) &&
            frame.payload[0] == 0x90 && frame.payload[1] == (step.arguments.on ? 0 : 1);
        const bool audio = step.operation == OperationKind::query_state && step.arguments.query_opcode == 0x71 && frame.payload[0] == 0x7a;
        if ((power || audio) && execution_->record(command.step, StepResult::observed)) observed_evidence_ = true;
    }
    tick(now, false);
}
bool Coordinator::quiescent() const { for (const auto& l : lanes_) if (l.agent.pending_work() || l.agent.held_keys()) return false; return true; }
void Coordinator::quiesce() {
    if (plan_) fail("configuration transition closed the old plan");
    for (const auto& policy : policies_) for (const auto& grant : policy.route_grants)
        for (auto& l : lanes_) l.agent.close_route(grant.session_id, grant.route_generation);
    for (auto& l : lanes_) l.agent.quiesce();
    held_.clear();
    remote_plan_.reset(); active_remote_plans_.clear(); remote_policies_.clear(); remote_progress_.clear();
    if (!planner_) return;
    auto sessions = planner_->sessions();
    for (const auto& session : sessions) { for (auto& l : lanes_) l.agent.close_route(session.session_id, session.route_generation); planner_->stop_session(session.session_id); }
}
ExecutionSnapshot Coordinator::snapshot() const {
    ExecutionSnapshot s; if (planner_) { s.state_revision = planner_->state_revision(); s.sessions = planner_->sessions(); }
    if (plan_) { s.executing_plan = plan_->plan_id; s.steps = execution_->step_results(); }
    else if (remote_plan_ && !remote_progress_.empty()) s.executing_plan = remote_plan_->plan_id;
    for (const auto& record : authorized_) s.command_outcomes.push_back(record.outcome);
    s.error = error_; return s;
}
}
