#include "cec_hub_fixture.hpp"
#include "amity/live_system.hpp"
#include "system_wire.hpp"
#include <limits>
using namespace amity::system;
template<class T> T live_id(unsigned n) { char text[33]; std::snprintf(text, sizeof(text), "%032x", n); return T::parse(text).value(); }
static LocalConfiguration configuration() {
    LocalConfiguration c; c.topology.config_revision = 1; c.presentation_revision = 1; c.executable_hash = "qualified-fixture";
    const auto group = live_id<ConsortiumId>(1), unused_group = group; (void)unused_group;
    const auto owner = live_id<OctopusId>(2);
    const CecPortRef front{group, owner, 0, Side::front}, back{group, owner, 0, Side::back};
    Component source{live_id<ComponentId>(10), "Source title", {{back, "Source"}}};
    Component display{live_id<ComponentId>(11), "Screen title", {{front, "TV"}}}; c.topology.components = {source, display};
    PortPolicySnapshot f, b; f.port = front; f.domain_id = live_id<DomainId>(20); f.domain_revision = 1; f.role = PortRole::playback_proxy; f.components = c.topology.components;
    f.physical_address = PhysicalAddressEvidence{front, f.domain_id, 1, 0x1000, PaProvenance::user_confirmed_fallback, {}, true};
    b.port = back; b.domain_id = live_id<DomainId>(21); b.domain_revision = 1; b.role = PortRole::tv_controller; b.components = c.topology.components;
    auto binding = [&](unsigned id, const Component& component, Capability capability, PortPolicySnapshot& p, bool proxy = false) {
        ComponentBinding value; value.binding_id = live_id<BindingId>(id); value.component_id = component.component_id; value.capability = capability;
        value.kind = proxy ? BindingKind::proxy_representation : BindingKind::native_appliance;
        if (!proxy) value.device_name = component.qualified_names.front(); else value.proxy_identity_resource_id = live_id<ResourceId>(90);
        value.endpoint.port = p.port; value.owner_octopus_id = owner; value.domain_id = p.domain_id; value.domain_revision = 1; value.binding_revision = 1; value.role = p.role;
        value.physical_address_evidence_ref = live_id<EvidenceId>(100 + id); c.topology.bindings.push_back(value);
        p.bindings.push_back({value, true, true, p.role == PortRole::playback_proxy && !proxy, capability == Capability::routing});
    };
    binding(30, display, Capability::power, f); binding(31, source, Capability::power, b);
    binding(32, source, Capability::routing, b); binding(33, source, Capability::navigation, b); binding(34, source, Capability::proxy_activation, f, true);
    c.port_templates = {f, b};
    c.topology.resources = {{live_id<ResourceId>(90), owner, ResourceCompatibility::exclusive, {"active"}}};
    SignalPort output{source.component_id, 0, SignalDirection::output}, input{display.component_id, 0, SignalDirection::input};
    c.topology.signal_edges = {{output, input, SignalEdgeKind::hdmi_cable, Support::supported}};
    RouteDefinition route; route.route_id = live_id<RouteId>(50); route.source = source.component_id; route.display = display.component_id;
    route.physical_path = {output, input}; route.required_bindings = {live_id<BindingId>(30), live_id<BindingId>(31), live_id<BindingId>(32), live_id<BindingId>(33), live_id<BindingId>(34)};
    route.demands = {{live_id<ResourceId>(90), "active", owner}};
    OperationArguments path; path.physical_path = 0x1000;
    route.steps = {{live_id<BindingId>(30), OperationKind::ensure_power, {}, {}, false},
        {live_id<BindingId>(31), OperationKind::ensure_power, {}, {0}, false},
        {live_id<BindingId>(32), OperationKind::select_path, path, {1}, false},
        {live_id<BindingId>(34), OperationKind::announce_active_source, {}, {2}, false}};
    c.topology.routes = {route}; c.primary_zone_id = live_id<ZoneId>(60);
    c.topology.activities = {{live_id<ActivityId>(70), c.primary_zone_id, route.route_id}};
    c.activities = {{live_id<ActivityId>(70), c.primary_zone_id, "Watch", source.component_id, display.component_id, {}}};
    c.external_slots = {{5, live_id<ActivityId>(70), false, false, ""}}; return c;
}
int main() {
    fixture_present = true;
    amity::HubService service; service.set_system_identity(live_id<ConsortiumId>(1), live_id<OctopusId>(2));
    assert(service.start(1, 2) == ESP_OK);
    fixture_sent.clear(); run_idle(2); assert(fixture_sent.empty()); // Controlled boot is monitor-only until typed install.
    auto config = configuration(); assert(service.configure_v2(config) == ESP_OK);
    fixture_sent.clear(); run_idle(80);
    auto state = service.snapshot();
    assert(state.activities.size() == 16 && state.activities[5].name == "Watch" && state.current_activity == -1);
    assert(service.execution_snapshot().applied_config_revision == 1);
    for (const auto& wire : fixture_sent) assert(wire.frame.size < 2 || (wire.frame.data[1] != 0x44 && wire.frame.data[1] != 0x04 && wire.frame.data[1] != 0x82 && wire.frame.data[1] != 0x86));
    assert(state.devices.size() == 2 && !state.discovering);
    fixture_sent.clear(); assert(service.request_activity(5) == ESP_OK); run_one_command(); run_idle(30);
    auto execution = service.execution_snapshot();
    if (execution.sessions.empty()) std::fprintf(stderr, "execution error: %s snapshot: %s\n", execution.error.c_str(), service.snapshot().activity_error.c_str());
    assert(execution.sessions.size() == 1 && execution.sessions[0].activity_id == live_id<ActivityId>(70));
    assert(execution.sessions[0].status == SessionStatus::degraded); // Wire completion is not observation.
    bool route = false, active = false;
    for (const auto& wire : fixture_sent) { route |= wire.gpio == 2 && wire.frame.data[1] == 0x86; active |= wire.gpio == 1 && wire.frame.data[1] == 0x82; }
    assert(route && active && service.snapshot().current_activity == 5);
    assert(service.press_key(1)); run_one_command(); run_idle(3);
    assert(service.snapshot().current_activity == 5);
    unsigned settings_writes = 0;
    assert(service.commit_adapter_settings(live_id<BootId>(3), {{0,1}}, uint64_t(fixture_time)/1000+5000, [&]{++settings_writes;return ESP_OK;}) == ESP_ERR_INVALID_STATE);
    assert(settings_writes == 0); // A live activity/held key cannot be interrupted by a settings restart.
    auto cosmetic = config; cosmetic.presentation_revision = 2; cosmetic.activities[0].label = "Renamed screen";
    fixture_sent.clear(); assert(service.configure_v2(cosmetic) == ESP_OK); run_idle(3);
    assert(service.snapshot().activities[5].name == "Renamed screen" && service.snapshot().current_activity == 5);
    assert(service.execution_snapshot().applied_presentation_revision == 2);
    for (const auto& wire : fixture_sent) assert(wire.frame.size < 2 || (wire.frame.data[1] != 0x36 && wire.frame.data[1] != 0x82 && wire.frame.data[1] != 0x86));
    assert(service.request_activity(0) == ESP_ERR_INVALID_ARG); // Tombstoned/unused selector.
    const auto request = live_id<MessageId>(400);
    const auto expected = service.execution_snapshot().state_revision;
    assert(service.request_activity_v2(config.primary_zone_id, live_id<ActivityId>(70), request, expected) == ESP_OK);
    run_one_command(); run_idle(30);
    fixture_sent.clear();
    assert(service.request_activity_v2(config.primary_zone_id, live_id<ActivityId>(70), request, expected) == ESP_OK);
    run_one_command(); run_idle(3); assert(fixture_sent.empty()); // Exact retry never replays effects.
    for (unsigned i = 0; i < 260; ++i) {
        assert(service.request_activity(5) == ESP_OK); run_one_command(); run_idle(30);
        assert(!service.execution_snapshot().executing_plan && service.execution_snapshot().sessions.size() == 1);
    }
    fixture_sent.clear();
    assert(service.request_activity_v2(config.primary_zone_id, live_id<ActivityId>(70), request, expected) == ESP_OK);
    run_one_command(); run_idle(3);
    assert(fixture_sent.empty()); // Evicted old request is stale, never a new plan.
    assert(service.execution_snapshot().error.find("state changed") != std::string::npos);
    const auto input_context = service.input_context();
    InputIntent input; input.action = InputAction::key_press; input.zone_id = config.primary_zone_id;
    input.key = 1; input.hold_token = live_id<HoldToken>(4400); input.input_session_id = input_context.input_session_id;
    input.expected_state_revision = input_context.state_revision;
    auto delayed_input = input; delayed_input.admissible_until_ms = uint64_t(fixture_time) / 1000 + 50;
    fixture_sent.clear(); assert(!service.submit_input_intent(delayed_input));
    for (const auto& sent : fixture_sent) assert(sent.frame.size < 2 || sent.frame.data[1] != 0x44);
    assert(service.submit_input_intent(input)); run_idle(3);
    fixture_sent.clear(); input.input_session_id = live_id<InputSessionId>(4401);
    assert(!service.submit_input_intent(input)); run_idle(3); assert(fixture_sent.empty());
    for(unsigned key:{0u,1u}){
        const auto context=service.input_context();const auto before=service.execution_snapshot().state_revision;
        InputIntent tap;tap.action=InputAction::key_press;tap.request_id=live_id<MessageId>(4500+key);tap.zone_id=config.primary_zone_id;
        tap.key=static_cast<uint8_t>(key);tap.repeat=false;tap.hold_token=live_id<HoldToken>(4800+key);
        tap.input_session_id=context.input_session_id;tap.expected_state_revision=context.state_revision;tap.admissible_until_ms=uint64_t(fixture_time)/1000+500;
        fixture_sent.clear();const auto tap_admission=service.submit_input_intent(tap);if(!tap_admission)std::fprintf(stderr,"tap %u rejected: %s\n",key,tap_admission.reason.c_str());assert(tap_admission);run_idle(3);
        unsigned presses=0,releases=0;for(const auto& sent:fixture_sent){if(sent.frame.size==3&&sent.frame.data[1]==0x44&&sent.frame.data[2]==key)++presses;if(sent.frame.size==2&&sent.frame.data[1]==0x45)++releases;}
        assert(presses==1&&releases==1&&service.execution_snapshot().state_revision==before);
    }
    input.input_session_id=service.input_context().input_session_id;input.expected_state_revision=service.input_context().state_revision;input.key=255;
    input.hold_token=live_id<HoldToken>(4999);input.admissible_until_ms=uint64_t(fixture_time)/1000+500;
    fixture_sent.clear();const auto unsupported_tap=service.submit_input_intent(input);assert(!unsupported_tap&&unsupported_tap.code==AdmissionCode::unsupported);run_idle(3);for(const auto& sent:fixture_sent)assert(sent.frame.size<2||sent.frame.data[1]!=0x44);
    // The member side prepares committed semantics without effects, installs a
    // receiver-local lease only after the barrier, and deduplicates admission.
    auto policies = service.local_policy_snapshot(); auto topology = config.topology;
    topology.bindings.clear(); topology.ready_owners = {live_id<OctopusId>(2)};
    for (auto& policy : policies) {
        policy.route_grants.clear();
        for (const auto& permission : policy.bindings) topology.bindings.push_back(permission.binding);
    }
    Coordinator member([](uint8_t* data, size_t size) { static uint8_t counter = 1; std::fill(data, data + size, counter++); return true; });
    std::array<TestWire,2> member_wire;for(size_t i=0;i<policies.size();++i)member_wire[i].bind(member,policies[i].port);
    assert(member.install(topology, policies, live_id<OctopusId>(2), 100));
    Plan remote; remote.plan_id = live_id<PlanId>(9000); remote.config_revision = 1; remote.steps = topology.routes[0].steps;
    remote.proposed_session = {live_id<SessionId>(9001), live_id<ActivityId>(70), config.primary_zone_id, live_id<RouteId>(50), remote.plan_id, 1, SessionStatus::preparing, {}};
    remote.owners = {live_id<OctopusId>(2)};
    auto prepared = policies;
    for (auto& policy : prepared) {
        ++policy.policy_generation;
        for (const auto& permission : policy.bindings) {
            RouteGrant grant{permission.binding.binding_id, remote.proposed_session.session_id, 1, 1000, {}};
            for (const auto& step : remote.steps) if (step.binding_id == grant.binding_id && step.operation == OperationKind::select_path) grant.permitted_paths.push_back(step.arguments.physical_path);
            policy.route_grants.push_back(grant);
        }
    }
    auto changed = prepared; ++changed[0].bindings[0].binding.binding_revision;
    assert(!member.prepare_remote_plan(remote, changed, 100));
    assert(member.prepare_remote_plan(remote, prepared, 100));
    assert(member_wire[0].fifo.empty());
    assert(!member.activate_remote_plan(remote.plan_id, 5001, 101));
    assert(member.activate_remote_plan(remote.plan_id, 1000, 101));
    const auto p = member.policies()[0]; const auto& binding = p.bindings[0].binding;
    AuthorizedCecOperation operation{live_id<CommandId>(9002), OperationKind::ensure_power, binding.component_id, binding.binding_id, binding.endpoint,
        {p.owner_boot_id, p.coordinator_epoch, p.config_revision, binding.domain_revision, binding.binding_revision, remote.proposed_session.session_id, 1, p.policy_generation}, 1000, {}};
    assert(member.execute_authorized(operation, 102)); assert(member.execute_authorized(operation, 102));
    auto wire = member_wire[0].take(102); assert(wire && wire->frame.payload[0] == 0x04);
    member_wire[0].complete(true,103); assert(!member_wire[0].take(103));
    const auto prior_grants = member.policies()[0].route_grants;
    assert(member.renew_discovery_authority(1000, 2000));
    assert(!member_wire[0].take(2000));
    assert(member.policies()[0].route_grants.size() == prior_grants.size());
    for (size_t i = 0; i < prior_grants.size(); ++i) assert(member.policies()[0].route_grants[i].expires_at_ms == prior_grants[i].expires_at_ms);
    assert(!member.renew_remote_authority(remote.plan_id, live_id<SessionId>(9999), 1, 1000, 2000));
    assert(member.renew_remote_authority(remote.plan_id, remote.proposed_session.session_id, 1, 1000, 2000));
    assert(!member_wire[0].take(2000)); // Authority renewal alone never acts.
    operation.command_id = live_id<CommandId>(9004); operation.execution_expires_at_ms = 2500;
    assert(member.execute_authorized(operation, 2001)); assert(member_wire[0].take(2001)); member_wire[0].complete(true,2002);
    auto refreshed_member = member.policies();
    const auto live_grants = refreshed_member[0].route_grants;
    for (auto& policy : refreshed_member) policy.route_grants.clear();
    assert(member.refresh_bindings(topology, refreshed_member, 2002));
    assert(member.policies()[0].route_grants.size() == live_grants.size());
    assert(!member_wire[0].take(2002)); // Unchanged bounded scan does not drop member grants or replay.
    assert(member.fence_remote_route(remote.proposed_session.session_id, 1));
    operation.command_id = live_id<CommandId>(9003); assert(!member.execute_authorized(operation, 2003));
    assert(!member.renew_remote_authority(remote.plan_id, remote.proposed_session.session_id, 1, 1000, 2003));
    assert(member.remote_route_quiescent(remote.proposed_session.session_id, 1));
    // Exercise the real coordinator dependency/barrier path across two owners,
    // including a remote source key and its matching cleanup.
    auto distributed = topology;
    const auto leader_id = live_id<OctopusId>(2), follower_id = live_id<OctopusId>(3);
    auto local_policy = policies[0], follower_policy = policies[1];
    const auto old_back = follower_policy.port; follower_policy.port.octopus_id = follower_id;
    for (auto& component : distributed.components) for (auto& name : component.qualified_names) if (name.port == old_back) name.port = follower_policy.port;
    local_policy.components = follower_policy.components = distributed.components;
    for (auto& permission : follower_policy.bindings) {
        permission.binding.endpoint.port = follower_policy.port; permission.binding.owner_octopus_id = follower_id;
        if (permission.binding.device_name) permission.binding.device_name->port = follower_policy.port;
    }
    distributed.bindings.clear();
    for (const auto& permission : local_policy.bindings) distributed.bindings.push_back(permission.binding);
    for (const auto& permission : follower_policy.bindings) distributed.bindings.push_back(permission.binding);
    distributed.ready_owners = {leader_id};
    Coordinator leader([](uint8_t* data, size_t size) { static uint8_t counter = 20; std::fill(data, data + size, counter++); return true; });
    Coordinator follower([](uint8_t* data, size_t size) { static uint8_t counter = 40; std::fill(data, data + size, counter++); return true; });
    TestWire leader_wire,follower_wire;leader_wire.bind(leader,local_policy.port);follower_wire.bind(follower,follower_policy.port);
    assert(leader.install(distributed, {local_policy}, leader_id, 200));
    assert(follower.install(distributed, {follower_policy}, follower_id, 200));
    assert(leader.update_remote_owner(follower_id, {follower_policy}, true, 5000, 200));
    std::deque<std::function<void()>> messages; std::vector<CommandId> reported;
    unsigned remote_frames = 0, local_frames = 0; uint64_t clock = 200;
    RemoteExecutionHooks hooks;
    hooks.prepare = [&](OctopusId, const Plan& plan) {
        messages.push_back([&, plan] {
            auto prepared_policy = follower.policies()[0]; ++prepared_policy.policy_generation;
            for (const auto& permission : prepared_policy.bindings) {
                RouteGrant grant{permission.binding.binding_id, plan.proposed_session.session_id, plan.proposed_session.route_generation, clock + 5000, {}};
                for (const auto& step : plan.steps) if (step.binding_id == grant.binding_id && step.operation == OperationKind::select_path) grant.permitted_paths.push_back(step.arguments.physical_path);
                prepared_policy.route_grants.push_back(grant);
            }
            auto accepted = follower.prepare_remote_plan(plan, {prepared_policy}, clock);
            assert(leader.remote_plan_result(follower_id, plan.plan_id, RemotePlanPhase::prepare, bool(accepted), accepted.reason, clock));
        }); return true;
    };
    hooks.fence = [&](OctopusId, const Plan& plan, const std::optional<Session>& old) {
        messages.push_back([&, plan, old] { if (old) assert(follower.fence_remote_route(old->session_id, old->route_generation));
            assert(!old || follower.remote_route_quiescent(old->session_id, old->route_generation));
            assert(leader.remote_plan_result(follower_id, plan.plan_id, RemotePlanPhase::fence, true, {}, clock)); }); return true;
    };
    hooks.activate = [&](OctopusId, const Plan& plan, uint32_t ttl) {
        messages.push_back([&, plan, ttl] { assert(follower.activate_remote_plan(plan.plan_id, ttl, clock));
            assert(leader.remote_plan_result(follower_id, plan.plan_id, RemotePlanPhase::activate, true, {}, clock)); }); return true;
    };
    hooks.send = [&](OctopusId, PlanId, const AuthorizedCecOperation& operation) {
        messages.push_back([&, operation] { assert(follower.execute_authorized(operation, clock)); }); return true;
    };
    hooks.abort = [&](OctopusId, const Plan& plan) { follower.abort_remote_plan(plan.plan_id); };
    leader.remote_execution_hooks(std::move(hooks));
    SelectRequest selection{live_id<MessageId>(9100), config.primary_zone_id, live_id<ActivityId>(70), live_id<SessionId>(9101), live_id<PlanId>(9102), leader.snapshot().state_revision};
    assert(leader.select(selection, clock).plan); assert(leader_wire.fifo.empty());
    auto pump = [&] {
        ++clock;
        if (!messages.empty()) { auto deliver = std::move(messages.front()); messages.pop_front(); deliver(); }
        leader.tick(clock);follower.tick(clock);
        if (leader_wire.take(clock)) { ++local_frames; leader_wire.complete(true,clock); }
        if (follower_wire.take(clock)) { ++remote_frames; follower_wire.complete(true,clock); }
        for (const auto& outcome : follower.snapshot().command_outcomes) if ((outcome.result == StepResult::applied || outcome.result == StepResult::observed) &&
            std::find(reported.begin(), reported.end(), outcome.command_id) == reported.end()) {
            reported.push_back(outcome.command_id); leader.remote_operation_result(follower_id, outcome.command_id, outcome.result, outcome.error, clock);
        }
    };
    for (unsigned i = 0; i < 50; ++i) pump();
    assert(!leader.snapshot().executing_plan && leader.snapshot().sessions.size() == 1 && local_frames == 2 && remote_frames == 3);
    assert(leader.key(config.primary_zone_id, live_id<ComponentId>(10), 1, false, live_id<HoldToken>(9200), clock));
    for (unsigned i = 0; i < 20; ++i) pump();
    assert(remote_frames == 5); // Press and matching remote release.
    assert(leader.key(config.primary_zone_id, live_id<ComponentId>(10), 1, true, live_id<HoldToken>(9201), clock));
    for (unsigned i = 0; i < 3; ++i) pump();
    for (unsigned i = 0; i < 3; ++i) pump();
    assert(leader.release(live_id<HoldToken>(9201)));
    for (unsigned i = 0; i < 3; ++i) pump();
    const auto released_frames = remote_frames;
    assert(released_frames > 8); // The owning device repeats locally after each completion, without another press message.
    for (unsigned i = 0; i < 4; ++i) pump();
    assert(remote_frames == released_frames); // Matching release stops its stream.
    assert(leader.stop(config.primary_zone_id, clock));
    assert(leader.snapshot().sessions.size() == 1); // Hold resource demands through the remote barrier.
    assert(leader.select(selection, clock).status == PlanningStatus::overloaded);
    for (unsigned i = 0; i < 10; ++i) pump();
    assert(leader.snapshot().sessions.empty());
    auto verified_topology = topology;
    OperationArguments query_on; query_on.query_opcode = 0x8f; query_on.on = true;
    verified_topology.routes[0].steps.push_back({live_id<BindingId>(30), OperationKind::query_state, query_on, {3}, true});
    Coordinator verification([](uint8_t* data, size_t size) { static uint8_t counter = 60; std::fill(data, data + size, counter++); return true; });
    std::array<TestWire,2> verification_wire;for(size_t i=0;i<policies.size();++i)verification_wire[i].bind(verification,policies[i].port);
    assert(verification.install(verified_topology, policies, leader_id, 10000));
    SelectRequest verify_request{live_id<MessageId>(9300), config.primary_zone_id, live_id<ActivityId>(70), live_id<SessionId>(9301), live_id<PlanId>(9302), verification.snapshot().state_revision};
    assert(verification.select(verify_request, 10000).plan);
    auto same_identity = verification.policies();
    const auto policy_generation = same_identity[0].policy_generation;
    for (auto& policy : same_identity) policy.route_grants.clear();
    assert(verification.refresh_bindings(verified_topology, same_identity, 10000));
    assert(verification.snapshot().executing_plan && verification.policies()[0].policy_generation == policy_generation);

    for (uint64_t now = 10001; now < 10030; ++now){verification.tick(now);for(auto& driver:verification_wire)
        if(driver.take(now))driver.complete(true,now);}
    assert(verification.snapshot().executing_plan);
    const auto display_binding = policies[0].bindings[0].binding;
    verification.observe({display_binding.endpoint.port, display_binding.endpoint.logical_address, 4, {0x90, 1}}, 10030);
    assert(verification.snapshot().executing_plan); // Standby contradicts requested power-on.
    verification.tick(14000);
    assert(!verification.snapshot().executing_plan && verification.snapshot().sessions[0].status == SessionStatus::degraded);
    assert(verification.snapshot().error.find("not observed") != std::string::npos);
    cec::Bus migration_front, migration_back; assert(migration_front.start(1) == ESP_OK && migration_back.start(2) == ESP_OK);
    amity::LiveSystem migration(live_id<ConsortiumId>(1), leader_id, live_id<BootId>(9500), [](uint8_t* data, size_t size) {
        static uint8_t counter = 80; std::fill(data, data + size, counter++); return true;
    });
    migration.attach(config.port_templates[0].port, migration_front, 15, 0xffff);
    migration.attach(config.port_templates[1].port, migration_back, 15, 0);
    auto hinted = config; hinted.topology.config_revision = 2; hinted.executable_hash = "migrated-hint";
    hinted.port_templates[0].physical_address->physical_address = 0x2000; // Deliberately wrong hint.
    hinted.port_templates[0].physical_address->provenance = PaProvenance::legacy_minimum_hint;
    hinted.port_templates[0].physical_address->confirmed = false;
    fixture_sent.clear(); std::string migration_error;
    assert(migration.configure(hinted, 100, migration_error));
    assert(migration.execution_snapshot().applied_config_revision == 2 && migration.snapshot({}).ready);
    assert(migration.local_policy_snapshot()[0].role == PortRole::monitor && !migration.local_policy_snapshot()[0].owned_logical_address);
    for (uint64_t now = 200; now < 20000; now += 100) migration.tick(now);
    assert(migration.execution_snapshot().applied_config_revision == 2 && migration.snapshot({}).ready);
    const auto verified_pa = migration.local_policy_snapshot()[0].physical_address;
    assert(verified_pa && verified_pa->physical_address == 0x1000 && verified_pa->confirmed && verified_pa->provenance == PaProvenance::immediate_peer_report);
    assert(verified_pa->reporter && verified_pa->reporter->port == config.port_templates[1].port);
    assert(!hinted.port_templates[0].physical_address->confirmed); // Persisted hint remains a hint.
    assert(migration.execution_snapshot().sessions.empty() && migration.execution_snapshot().suspended_requested_sessions.size() == 1);
    for (const auto& sent : fixture_sent) assert(sent.frame.size < 2 || (sent.frame.data[1] != 0x44 && sent.frame.data[1] != 0x04 && sent.frame.data[1] != 0x82 && sent.frame.data[1] != 0x86));
    {
        // Semantic installation pauses the owner before its durable marker.
        // A periodic scan already probing must not be reused after installation
        // or postpone fresh attachment evidence until another periodic cycle.
        cec::Bus refresh_front, refresh_back; assert(refresh_front.start(1) == ESP_OK && refresh_back.start(2) == ESP_OK);
        amity::LiveSystem refreshing(live_id<ConsortiumId>(1), leader_id, live_id<BootId>(9501), [](uint8_t* data, size_t size) {
            static uint8_t counter = 120; std::fill(data, data + size, counter++); return true;
        });
        refreshing.attach(config.port_templates[0].port, refresh_front, 15, 0xffff);
        refreshing.attach(config.port_templates[1].port, refresh_back, 15, 0);
        assert(refreshing.configure(hinted, 100, migration_error));
        for (uint64_t now = 200; now < 12000; now += 100) refreshing.tick(now);
        assert(refreshing.execution_snapshot().applied_config_revision == 2);
        refreshing.discover(20100); refreshing.tick(20100); assert(refreshing.snapshot({}).discovering);
        refreshing.maintenance(true); refreshing.tick(20110); // Drain the started probe, preserving its completion mapping.
        auto changed_metadata = hinted; changed_metadata.topology.config_revision = 3;
        changed_metadata.executable_hash = "new-remote-mapping-same-cec-attachments";
        assert(refreshing.configure(changed_metadata, 20120, migration_error));
        assert(refreshing.execution_snapshot().applied_config_revision == 3 && refreshing.execution_snapshot().native_observations.empty()); // Installed monitor policy is not equipment-presence proof.
        refreshing.maintenance(false); fixture_transmitted.clear(); fixture_sent.clear();
        refreshing.tick(20130);
        assert(std::any_of(fixture_transmitted.begin(), fixture_transmitted.end(), [](const auto& sent) {
            return sent.gpio == 1 && sent.frame.size == 1 && sent.frame.destination() == 0;
        })); // A new job starts at address zero immediately, rather than resuming the old job at one.
        for (uint64_t now = 20200; now < 35000; now += 100) refreshing.tick(now);
        assert(refreshing.execution_snapshot().applied_config_revision == 3 && refreshing.snapshot({}).ready);
        assert(refreshing.local_policy_snapshot()[0].physical_address->provenance == PaProvenance::immediate_peer_report);
        for (const auto& sent : fixture_sent) assert(sent.frame.size < 2 || (sent.frame.data[1] != 0x44 && sent.frame.data[1] != 0x04 && sent.frame.data[1] != 0x82 && sent.frame.data[1] != 0x86));
        auto* original_owner_task = latest_task;
        {
            amity::HubService queued;
            queued.set_system_identity(live_id<ConsortiumId>(1), leader_id);
            assert(queued.start(1, 2) == ESP_OK && queued.configure_v2(hinted) == ESP_OK);
            run_idle(80); assert(queued.execution_snapshot().applied_config_revision == 2);
            assert(queued.discover()); run_one_command(); // Leave the old job probing across queue_v2's pause.
            fixture_sent.clear(); fixture_transmitted.clear(); bool committed = false;
            assert(queued.configure_v2(changed_metadata, [&] { committed = true; return ESP_OK; }) == ESP_OK);
            run_idle(1);
            assert(committed && std::any_of(fixture_transmitted.begin(), fixture_transmitted.end(), [](const auto& sent) {
                return sent.gpio == 1 && sent.frame.size == 1 && sent.frame.destination() == 0;
            }));
            run_idle(80);
            const auto applied = queued.execution_snapshot();
            assert(applied.applied_config_revision == 3 && applied.applied_policy_hash == changed_metadata.executable_hash && queued.snapshot().ready);
            for (const auto& sent : fixture_sent) assert(sent.frame.size < 2 || (sent.frame.data[1] != 0x44 && sent.frame.data[1] != 0x04 && sent.frame.data[1] != 0x82 && sent.frame.data[1] != 0x86));
        }
        latest_task = original_owner_task;
    }
    migration.quiesce(); for (uint64_t now = 20000; now < 21000; now += 100) migration.tick(now);
    auto zones = config; zones.topology.config_revision = 3; zones.executable_hash = "two-zone-fixture";
    zones.topology.resources[0].compatibility = ResourceCompatibility::same_mode;
    zones.topology.activities.push_back({live_id<ActivityId>(71), live_id<ZoneId>(61), live_id<RouteId>(50)});
    zones.activities.push_back({live_id<ActivityId>(71), live_id<ZoneId>(61), "Secondary", live_id<ComponentId>(10), live_id<ComponentId>(11), {}});
    zones.external_slots.push_back({10, live_id<ActivityId>(71), false, false, ""});
    assert(migration.configure(zones, 21000, migration_error));
    assert(migration.snapshot({}).activities[10].name.empty());
    assert(!migration.select_slot(10, 21000, migration_error)); // Secondary zone has no primary external selector.
    for (uint64_t now = 21100; now < 40000; now += 100) migration.tick(now);
    assert(migration.select(zones.primary_zone_id, live_id<ActivityId>(70), live_id<MessageId>(9600), {}, 40000, migration_error));
    for (uint64_t now = 40100; now < 43000; now += 100) migration.tick(now);
    const auto primary_generation = migration.snapshot({}).activity_generation;
    assert(migration.snapshot({}).current_activity == 5);
    assert(migration.select(live_id<ZoneId>(61), live_id<ActivityId>(71), live_id<MessageId>(9601), {}, 43000, migration_error));
    for (uint64_t now = 43100; now < 46000; now += 100) migration.tick(now);
    assert(migration.snapshot({}).current_activity == 5 && migration.snapshot({}).activity_generation == primary_generation);
    assert(migration.stop(live_id<ZoneId>(61), 46000));
    assert(migration.snapshot({}).current_activity == 5 && migration.execution_snapshot().sessions.size() == 1);
    const auto quiet_bindings = migration.local_policy_snapshot(); fixture_sent.clear();
    for (uint64_t now = 46100; now < 61000; now += 100) migration.tick(now);
    fixture_completion_ready = [](int gpio) { return gpio != 2; };
    for (uint64_t now = 61000; now < 66000; now += 100) migration.tick(now);
    // One independent line completed its refresh while the other scan is still
    // collecting. Last complete, genuinely fresh evidence keeps its bindings.
    const auto partial_refresh = migration.local_policy_snapshot();
    for (size_t i = 0; i < quiet_bindings.size(); ++i) for (size_t j = 0; j < quiet_bindings[i].bindings.size(); ++j)
        assert(quiet_bindings[i].bindings[j].binding.binding_revision == partial_refresh[i].bindings[j].binding.binding_revision);
    fixture_completion_ready = {};
    for (uint64_t now = 66000; now < 80000; now += 100) migration.tick(now);
    assert(migration.snapshot({}).current_activity == 5 && migration.snapshot({}).activity_generation == primary_generation);
    const auto quiet_observations = migration.execution_snapshot().native_observations;
    assert(!quiet_observations.empty());
    for (const auto& observation : quiet_observations) assert(!observation.stale && observation.received_at_ms > 60000);
    const auto refreshed_bindings = migration.local_policy_snapshot();
    for (size_t i = 0; i < quiet_bindings.size(); ++i) for (size_t j = 0; j < quiet_bindings[i].bindings.size(); ++j)
        assert(quiet_bindings[i].bindings[j].binding.binding_revision == refreshed_bindings[i].bindings[j].binding.binding_revision);
    for (const auto& sent : fixture_sent) assert(sent.frame.size < 2 || (sent.frame.data[1] != 0x44 && sent.frame.data[1] != 0x04 && sent.frame.data[1] != 0x82 && sent.frame.data[1] != 0x86));

    InputIntent causal; causal.action = InputAction::key_press; causal.zone_id = zones.primary_zone_id;
    causal.key = 1; causal.repeat = true; causal.hold_token = live_id<HoldToken>(9700);
    causal.expected_state_revision = migration.execution_snapshot().state_revision; causal.admissible_until_ms = 80050;
    const auto saved_fixture_time=fixture_time;fixture_time=80000000;fixture_driver_paused=true;fixture_wire_deadlines=true;
    fixture_sent.clear(); assert(migration.input(causal, 80000));
    fixture_time=80060000;fixture_driver_paused=false;migration.tick(80060);
    fixture_wire_deadlines=false;fixture_time=saved_fixture_time;
    assert(std::any_of(fixture_sent.begin(), fixture_sent.end(), [](const auto& sent) { return sent.frame.size == 3 && sent.frame.data[1] == 0x44; }));
    causal.action = InputAction::hold_release; assert(migration.input(causal,80061)); migration.tick(80062);
    causal.action = InputAction::key_press;
    causal.hold_token = live_id<HoldToken>(9701); causal.admissible_until_ms = 80150;
    assert(migration.input(causal, 80100)); migration.tick(80101); migration.tick(80102);
    fixture_sent.clear(); migration.tick(80151); migration.tick(80152);
    assert(std::any_of(fixture_sent.begin(), fixture_sent.end(), [](const auto& sent) { return sent.frame.size == 3 && sent.frame.data[1] == 0x44; }));
    assert(std::none_of(fixture_sent.begin(), fixture_sent.end(), [](const auto& sent) { return sent.frame.size == 2 && sent.frame.data[1] == 0x45; }));
    causal.action = InputAction::hold_release; causal.admissible_until_ms = 1;
    assert(migration.input(causal, 80153)); migration.tick(80154); migration.tick(80155);
    causal.action = InputAction::key_press; causal.hold_token = live_id<HoldToken>(9702); causal.admissible_until_ms = 80250;
    assert(migration.input(causal, 80200)); migration.tick(80201); migration.tick(80202);
    causal.action = InputAction::hold_release; causal.admissible_until_ms = 1;
    fixture_sent.clear();assert(migration.input(causal, 80203)); migration.tick(80204);
    assert(std::any_of(fixture_sent.begin(), fixture_sent.end(), [](const auto& sent) { return sent.frame.size == 2 && sent.frame.data[1] == 0x45; }));
    causal.action = InputAction::select_activity; causal.activity_id = live_id<ActivityId>(70); causal.request_id = live_id<MessageId>(9703);
    causal.admissible_until_ms = 80350;fixture_driver_paused=true;fixture_wire_deadlines=true;fixture_time=80300000;
    fixture_sent.clear(); assert(migration.input(causal, 80300));
    // IPC freshness applies before admission. Slow CEC completion cannot
    // expire a selector that the owner already accepted.
    fixture_time=80360000;fixture_driver_paused=false;
    for(uint64_t now=80360;now<80400;++now)migration.tick(now);
    fixture_wire_deadlines=false;fixture_time=saved_fixture_time;
    assert(!migration.execution_snapshot().executing_plan && migration.execution_snapshot().error.empty());
    assert(std::any_of(fixture_sent.begin(), fixture_sent.end(), [](const auto& sent) {
        return sent.frame.size >= 2 && (sent.frame.data[1] == 0x04 || sent.frame.data[1] == 0x82 || sent.frame.data[1] == 0x86);
    }));

    cec::Bus passive_front,passive_back;assert(passive_front.start(1)==ESP_OK&&passive_back.start(2)==ESP_OK);
    amity::LiveSystem returning_member(live_id<ConsortiumId>(1),leader_id,live_id<BootId>(9800),[](uint8_t* data,size_t size){static uint8_t counter=100;std::fill(data,data+size,counter++);return true;});
    returning_member.attach(config.port_templates[0].port,passive_front,15,0xffff);returning_member.attach(config.port_templates[1].port,passive_back,15,0);
    auto member_config=config;member_config.member=true;member_config.coordinator_id=follower_id;member_config.coordinator_epoch=1;
    assert(returning_member.configure(member_config,100,migration_error));for(uint64_t now=200;now<31000;now+=100)returning_member.tick(now);
    returning_member.tick(31001);auto returning_policies=returning_member.local_policy_snapshot();
    auto returning_plan=remote;returning_plan.plan_id=live_id<PlanId>(9801);returning_plan.proposed_session.plan_id=returning_plan.plan_id;returning_plan.proposed_session.session_id=live_id<SessionId>(9802);
    for(auto& policy:returning_policies){++policy.policy_generation;policy.authority_expires_at_ms=36100;for(const auto& permission:policy.bindings){RouteGrant grant{permission.binding.binding_id,returning_plan.proposed_session.session_id,1,36100,{}};for(const auto& step:returning_plan.steps)if(step.binding_id==grant.binding_id&&step.operation==OperationKind::select_path)grant.permitted_paths.push_back(step.arguments.physical_path);policy.route_grants.push_back(std::move(grant));}}
    assert(returning_member.prepare_remote_plan(returning_plan,returning_policies,31100));fixture_sent.clear();
    fixture_transmit=[](int gpio,const cec::Frame& frame){return gpio==1&&frame.destination()==4?cec::TxResult::Success:cec::TxResult::Nack;};
    const auto blocked_identity = returning_member.activate_remote_plan(returning_plan.plan_id,1000,31101);
    assert(!blocked_identity && blocked_identity.code == AdmissionCode::overloaded);
    assert(fixture_sent.empty()); // Admission never waits for even the first poll.
    returning_member.tick(31102); returning_member.tick(31103);
    for(const auto& policy:returning_member.local_policy_snapshot())assert(policy.route_grants.empty());
    for(const auto& sent:fixture_sent)assert(sent.frame.size==1); // Rejected activation was not retained.
    fixture_transmit={}; fixture_sent.clear();
    assert(!returning_member.activate_remote_plan(returning_plan.plan_id,1000,31104));
    for(uint64_t now=31105;now<31110;++now)returning_member.tick(now);
    for(const auto& policy:returning_member.local_policy_snapshot())assert(policy.route_grants.empty());
    for(const auto& sent:fixture_sent)assert(sent.frame.size==1);
    fixture_sent.clear();
    assert(returning_member.activate_remote_plan(returning_plan.plan_id,1000,31110));
    assert(fixture_sent.empty()); // Only a fresh request installs authority.
    returning_member.tick(32111); fixture_sent.clear();
    const auto renewal_pending = returning_member.renew_remote_authority(returning_plan.plan_id,returning_plan.proposed_session.session_id,1,1000,32112);
    assert(!renewal_pending && renewal_pending.code == AdmissionCode::overloaded && fixture_sent.empty());
    for(uint64_t now=32113;now<32119;++now)returning_member.tick(now);
    for(const auto& policy:returning_member.local_policy_snapshot())assert(policy.authority_expires_at_ms<=32112);
    for(const auto& sent:fixture_sent)assert(sent.frame.size==1);
    fixture_sent.clear();
    assert(returning_member.renew_remote_authority(returning_plan.plan_id,returning_plan.proposed_session.session_id,1,1000,32119));
    assert(fixture_sent.empty());

    // The manual-clock LiveSystem cases above intentionally do not run the
    // original service task. Use a fresh, continuously serviced owner here.
    auto* saved_owner_task=latest_task;const auto saved_owner_tick=fixture_tick;
    {
    fixture_tick=10000;fixture_incoming.clear();
    amity::HubService service;service.set_system_identity(live_id<ConsortiumId>(1),live_id<OctopusId>(2));
    assert(service.start(1,2)==ESP_OK&&service.configure_v2(config)==ESP_OK);run_idle(100);
    assert(service.request_activity(5) == ESP_OK); run_one_command(); run_idle(30);
    assert(service.press_key(1, true)); run_one_command(); run_idle(2);
    fixture_sent.clear(); assert(service.transition_coordinator_authority(false, 1) == ESP_OK);
    assert(!service.snapshot().ready && service.local_policy_snapshot().empty() && !service.press_key(2));
    for (const auto& sent : fixture_sent) assert(sent.frame.size < 2 || sent.frame.data[1] == 0x45);
    fixture_sent.clear(); assert(service.transition_coordinator_authority(true, 1) == ESP_OK);
    assert(fixture_sent.empty() && service.local_policy_snapshot().empty()); // Promotion alone never restores roles or a route.
    assert(service.configure_v2(config) == ESP_OK); run_idle(80);
    assert(service.snapshot().current_activity == -1);
    assert(service.request_activity(5) == ESP_OK); run_one_command(); run_idle(30);
    const auto normal_tick = fixture_tick; fixture_tick = 1000;
    assert(service.press_key(1, true)); run_one_command(); run_idle(2);
    const auto before_rebase = service.execution_snapshot();
    auto rebased_config = config; rebased_config.topology.config_revision = 2;
    fixture_sent.clear(); bool rebase_committed = false;
    assert(service.configure_v2(rebased_config, [&] { rebase_committed = true; return ESP_OK; }) == ESP_OK);
    assert(rebase_committed && service.execution_snapshot().applied_config_revision == 2);
    assert(service.execution_snapshot().sessions[0].session_id == before_rebase.sessions[0].session_id && service.snapshot().current_activity == 5);
    for (const auto& sent : fixture_sent) assert(sent.frame.size < 2 || sent.frame.data[1] == 0x83 || sent.frame.data[1] == 0x46 || sent.frame.data[1] == 0x8c || (sent.frame.size == 3 && sent.frame.data[1] == 0x44 && sent.frame.data[2] == 1)); // Rebase preserves the completion-paced hold and periodic identity queries.
    assert(service.press_key(1, true)); run_one_command(); run_idle(1); // An unchanged down is inert; the owner continues its existing hold.
    config = rebased_config;
    fixture_sent.clear(); assert(service.begin_maintenance() == ESP_OK && service.maintenance_ready());
    fixture_tick = normal_tick;
    assert(!service.press_key(2) && service.request_activity(5) != ESP_OK && service.release_key());
    for (const auto& sent : fixture_sent) assert(sent.frame.size < 2 || sent.frame.data[1] == 0x45);
    fixture_sent.clear(); service.end_maintenance(); run_idle(3);
    assert(!service.maintenance_ready());
    for (const auto& sent : fixture_sent) assert(sent.frame.size < 2 || sent.frame.data[1] == 0x83 || sent.frame.data[1] == 0x46 || sent.frame.data[1] == 0x8c); // Only resumed read-only discovery, never a held stream.
    for(unsigned pass=0;pass<200&&service.snapshot().discovering;++pass)run_idle(1);
    assert(!service.snapshot().discovering);
    assert(service.request_activity(5) == ESP_OK); run_one_command(); run_idle(30);
    fixture_sent.clear();
    assert(service.press_key(1, true)); run_one_command(); run_idle(2);
    assert(std::any_of(fixture_sent.begin(),fixture_sent.end(),[](const auto& sent){return sent.frame.size==3&&sent.frame.data[1]==0x44;}));
    fixture_sent.clear(); auto failed_config = config; failed_config.topology.config_revision = 3; failed_config.executable_hash = "failed-durable-config";
    bool persisted_after_drain = false;
    assert(service.configure_v2(failed_config, [&] {
        persisted_after_drain = true;
        assert(std::any_of(fixture_sent.begin(), fixture_sent.end(), [](const auto& sent) { return sent.frame.size == 2 && sent.frame.data[1] == 0x45; }));
        for (const auto& sent : fixture_sent) assert(sent.frame.size < 2 || sent.frame.data[1] == 0x45);
        return ESP_FAIL;
    }) == ESP_ERR_INVALID_STATE);
    assert(persisted_after_drain && !service.snapshot().ready);
    fixture_sent.clear(); assert(service.retire_membership() == ESP_OK);
    assert(!service.press_key(1) && service.request_activity(5) != ESP_OK && !service.snapshot().ready);
    }
    latest_task=saved_owner_task;fixture_tick=saved_owner_tick;
    std::puts("Live schema2 owner -> scoped scanner -> resource planner -> policy -> guarded runtime passes");
}
