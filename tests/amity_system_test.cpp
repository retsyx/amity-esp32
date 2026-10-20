#include "system_wire.hpp"
#include "amity/system/discovery.hpp"
#include "amity/system/planner.hpp"
#include "amity/system/policy.hpp"
#include <cassert>
#include <iostream>
#include <limits>
#include <type_traits>

using namespace amity::system;
template<class T> T id(unsigned n) {
    char text[33]; std::snprintf(text, sizeof(text), "%032x", n); return T::parse(text).value();
}
CecPortRef port(unsigned slot = 0, Side side = Side::back, unsigned board = 2) {
    return {id<ConsortiumId>(1), id<OctopusId>(board), static_cast<uint8_t>(slot), side};
}
Component component(unsigned n, const QualifiedDeviceName& name) { return {id<ComponentId>(n), "Label", {name}}; }
ComponentBinding native(unsigned n, const Component& c, const QualifiedDeviceName& name, uint8_t la, Capability capability = Capability::navigation) {
    ComponentBinding b;
    b.binding_id = id<BindingId>(n); b.component_id = c.component_id; b.capability = capability;
    b.device_name = name; b.endpoint = {name.port, la}; b.owner_octopus_id = name.port.octopus_id;
    b.domain_id = id<DomainId>(4); b.domain_revision = 1; b.binding_revision = 1;
    b.role = PortRole::tv_controller; b.status = BindingStatus::verified; b.physical_address_evidence_ref = id<EvidenceId>(22);
    return b;
}
PhysicalAddressEvidence pa(const CecPortRef& p, uint16_t value = 0x2000) {
    return {p, id<DomainId>(4), 1, value, PaProvenance::user_confirmed_fallback, {}, true};
}
void codecs() {
    static_assert(!std::is_convertible<QualifiedDeviceName, FullyQualifiedCecAddress>::value);
    static_assert(!std::is_assignable<OctopusId&, ConsortiumId>::value);
    assert(!OctopusId::parse("00000000000000000000000000000000"));
    assert(!OctopusId::parse("A0000000000000000000000000000001"));
    assert(!OctopusId::parse("0000000000000000000000000000001"));
    assert(parse_uint64("18446744073709551615") == std::numeric_limits<uint64_t>::max());
    for (auto bad : {"18446744073709551616", "00", "01", "-1", "+1", " 1", "1.0", "1e1", ""}) assert(!parse_uint64(bad));
    QualifiedDeviceName name{port(), "A/B TV"};
    assert(parse_name_json(name_json(name)) == name);
    assert(!parse_address_json(name_json(name)));
    for (uint8_t t = 0; t < 5; ++t) for (auto side : {Side::front, Side::back}) for (uint8_t la = 0; la < 15; ++la) {
        FullyQualifiedCecAddress a{port(t, side), la};
        assert(parse_cec_uri(cec_uri(a)) == a); assert(parse_address_json(address_json(a)) == a);
        assert(!parse_name_json(address_json(a))); assert(!parse_cec_uri(cec_uri(a) + "/extra"));
    }
    auto a = cec_uri({port(), 4});
    assert(!parse_cec_uri(a.substr(0, a.size() - 1) + "04"));
    auto json = name_json(name);
    assert(!parse_name_json(json.substr(0, json.size() - 1) + ",\"logical_address\":4}"));
    assert(!parse_name_json(json.substr(0, json.size() - 1) + ",\"side\":\"front\"}"));
    assert(!parse_name_json("{}"));
    assert((QualifiedDeviceName{port(), " Apple TV "}.valid()));
    assert((!QualifiedDeviceName{port(), "bad\nname"}.valid()));
    assert(valid_physical_address(0x2100)); assert(!valid_physical_address(0x2010)); assert(!valid_physical_address(0xffff));
}
CecObservation observation(ScanId scan, uint8_t la, std::string name, CecPortRef p = port()) {
    return {{p, la}, name, id<BootId>(3), 1, id<DomainId>(4), 1, ObservationKind::native_appliance, 0x2000, scan, 10, false};
}
void discovery() {
    QualifiedDeviceName name{port(), "Apple TV"};
    DiscoverySnapshot s{id<ScanId>(9), 1, id<DomainId>(4), 1, ScanStatus::complete, {}, {}};
    s.observations.push_back(observation(s.scan_id, 8, "Apple TV"));
    s.observations.push_back(observation(s.scan_id, 4, "Game Console"));
    auto resolved = resolve_native_name(name, s, 20);
    assert(resolved.status == ResolutionStatus::resolved && resolved.endpoint->logical_address == 8);
    s.observations.push_back(observation(s.scan_id, 8, "Apple TV", port(1)));
    assert(resolve_native_name(name, s, 20).status == ResolutionStatus::resolved); // domain/LA alias
    auto proxy = observation(s.scan_id, 11, "Apple TV"); proxy.kind = ObservationKind::proxy_representation;
    s.observations.push_back(proxy);
    assert(resolve_native_name(name, s, 20).status == ResolutionStatus::resolved);
    auto old = observation(id<ScanId>(8), 4, "Apple TV"); s.observations.push_back(old);
    assert(resolve_native_name(name, s, 20).status == ResolutionStatus::resolved);
    auto c = component(7, name); auto power = native(5, c, name, 4, Capability::power); auto navigation = native(6, c, name, 4);
    unsigned fenced = 0;
    auto refresh = propose_native_refresh(name, resolved, s, {power, navigation}, [&](BindingId, uint64_t r) { assert(r == 1); ++fenced; });
    assert(refresh.changed && fenced == 2 && refresh.proposed_bindings.size() == 2);
    for (const auto& b : refresh.proposed_bindings) { assert(b.endpoint.logical_address == 8 && b.binding_revision == 2 && b.status == BindingStatus::tentative); assert(b.device_name == name); }
    auto exhausted = navigation; exhausted.binding_revision = std::numeric_limits<uint64_t>::max(); fenced = 0;
    auto cannot_refresh = propose_native_refresh(name, resolved, s, {power, exhausted}, [&](BindingId, uint64_t) { ++fenced; });
    assert(!cannot_refresh.changed && fenced == 2); // One invalid capability must fence all siblings.
    s.observations.push_back(observation(s.scan_id, 11, "Apple TV"));
    assert(resolve_native_name(name, s, 20).status == ResolutionStatus::ambiguous);
    s.status = ScanStatus::timeout;
    assert(resolve_native_name(name, s, 20).status == ResolutionStatus::incomplete);
    assert(!propose_native_refresh(name, resolve_native_name(name, s, 20), s, {power}, [&](BindingId, uint64_t) { assert(false); }).changed);
    DiscoveryService scanner;
    ScanRequest request; request.scan_id = id<ScanId>(25); request.domain_id = id<DomainId>(4); request.domain_revision = 1;
    request.scanner_port = port(); request.scanner_boot_id = id<BootId>(3);
    assert(scanner.request_scan(request, 0) == request.scan_id);
    auto second = request; second.scan_id = id<ScanId>(26);
    assert(scanner.request_scan(second, 0) == request.scan_id);
    auto immutable = scanner.snapshot(request.scan_id);
    for (int i = 0; i < 15; ++i) {
        auto probe = scanner.advance_scan(request.scan_id, i); assert(probe && probe->destination == i && probe->initiator == 15 && probe->kind == ProbeKind::presence_poll);
    }
    assert(immutable->status == ScanStatus::queued);
    assert(scanner.observe(request.scan_id, observation(request.scan_id, 4, "Apple TV"), 20));
    auto stale_boot = observation(request.scan_id, 8, "Other"); stale_boot.owner_boot_id = id<BootId>(99);
    assert(!scanner.observe(request.scan_id, stale_boot, 21));
    assert(!scanner.observe(request.scan_id, observation(request.scan_id, 8, "Other"), 19));
    auto query = scanner.advance_scan(request.scan_id, 21); assert(query && query->kind == ProbeKind::physical_address && query->initiator == 15);
    auto vendor_query = scanner.advance_scan(request.scan_id, 22); assert(vendor_query && vendor_query->kind == ProbeKind::vendor_id);
    scanner.advance_scan(request.scan_id, 23);
    scanner.advance_scan(request.scan_id, 24); scanner.advance_scan(request.scan_id, 1124);
    assert(scanner.snapshot(request.scan_id)->status == ScanStatus::complete);
    assert(!scanner.observe(request.scan_id, observation(request.scan_id, 4, "Changed"), 1123));
    request.scan_id = id<ScanId>(27); assert(scanner.request_scan(request, 2000));
    scanner.advance_scan(request.scan_id, 32000); assert(scanner.snapshot(request.scan_id)->status == ScanStatus::timeout);
    auto hint = pa(port()); hint.provenance = PaProvenance::legacy_minimum_hint; hint.confirmed = false;
    auto physical = resolve_physical_address(port(), id<DomainId>(4), 1, {hint}); assert(!physical.selected && physical.hint);
    physical = resolve_physical_address(port(1), id<DomainId>(4), 1, {pa(port())}); assert(!physical.selected);
    physical = resolve_physical_address(port(), id<DomainId>(4), 1, {pa(port())}); assert(physical.selected);
    physical = resolve_physical_address(port(), id<DomainId>(4), 1, {pa(port()), pa(port(), 0x3000)}); assert(!physical.selected);
    std::vector<IdentityRequest> requests;
    for (unsigned i = 0; i < 3; ++i) requests.push_back({port(i, Side::front), PortRole::playback_proxy, pa(port(i, Side::front))});
    unsigned polls = 0;
    auto vacant = [&](const CecPortRef&, uint8_t) { ++polls; return PollOutcome::vacant; };
    auto allocation = allocate_domain_identities(id<DomainId>(4), 1, true, requests, false, vacant);
    assert(allocation.accepted && allocation.allocations.size() == 3 && polls == 6);
    assert(allocation.allocations[0].logical_address == 4 && allocation.allocations[1].logical_address == 8 && allocation.allocations[2].logical_address == 11);
    requests.push_back({port(3, Side::front), PortRole::playback_proxy, pa(port(3, Side::front))});
    assert(!allocate_domain_identities(id<DomainId>(4), 1, true, requests, false, vacant).accepted);
    assert(allocate_domain_identities(id<DomainId>(4), 1, true, requests, true, vacant).accepted);
    assert(!allocate_domain_identities(id<DomainId>(4), 1, false, requests, true, vacant).accepted);
    assert(!allocate_domain_identities(id<DomainId>(4), 1, true, {{port(), PortRole::tv_controller, {}}}, false,
        [](const CecPortRef&, uint8_t) { return PollOutcome::wire_error; }).accepted);
}
PortPolicySnapshot local_policy() {
    QualifiedDeviceName name{port(), "Apple TV"}; auto c = component(7, name);
    auto b = native(5, c, name, 4);
    PortPolicySnapshot p; p.port = port(); p.owner_boot_id = id<BootId>(3); p.domain_id = id<DomainId>(4);
    p.domain_revision = 1; p.coordinator_epoch = 1; p.config_revision = 7; p.policy_generation = 1;
    p.authority_expires_at_ms = 10000; p.role = PortRole::tv_controller; p.owned_logical_address = 0;
    p.components = {c}; p.bindings = {{b, false, false, false, false}};
    p.route_grants = {{b.binding_id, id<SessionId>(10), 9, 10000, {0x2000}}}; return p;
}
AuthorizedCecOperation press(const PortPolicySnapshot& p, unsigned command = 20, unsigned token = 30) {
    AuthorizedCecOperation o; o.command_id = id<CommandId>(command); o.kind = OperationKind::press_key;
    o.component_id = p.bindings[0].binding.component_id; o.binding_id = p.bindings[0].binding.binding_id;
    o.target = p.bindings[0].binding.endpoint; o.execution_expires_at_ms = 9000;
    o.fence = {p.owner_boot_id, p.coordinator_epoch, p.config_revision, p.domain_revision, p.bindings[0].binding.binding_revision, id<SessionId>(10), 9, p.policy_generation};
    o.arguments.hold_token = id<HoldToken>(token); o.arguments.key = 1; o.arguments.repeat = true; return o;
}
void policy() {
    auto p = local_policy(); LocalCecAgent agent; TestWire driver; auto operation = press(p);
    assert(agent.install_policy(p,0));assert(!agent.execute(operation,10)); // Missing actual driver fails closed.
    driver.bind(agent);assert(driver.fifo.empty());
    for(int field=0;field<9;++field){auto stale=operation;switch(field){
        case 0:stale.fence.owner_boot_id=id<BootId>(99);break;case 1:++stale.fence.coordinator_epoch;break;
        case 2:++stale.fence.config_revision;break;case 3:++stale.fence.domain_revision;break;case 4:++stale.fence.binding_revision;break;
        case 5:stale.fence.session_id=id<SessionId>(99);break;case 6:++stale.fence.route_generation;break;
        case 7:++stale.fence.policy_generation;break;case 8:stale.target.logical_address=8;break;}assert(!agent.execute(stale,10));}
    for(unsigned opcode=0;opcode<256;++opcode){assert(agent.observe({port(),4,15,{static_cast<uint8_t>(opcode),0,0}},10));assert(driver.fifo.empty());}
    auto query=operation;query.kind=OperationKind::query_state;
    for(unsigned n=0;n<8;++n){query.command_id=id<CommandId>(100+n);assert(agent.execute(query,10));}
    assert(driver.fifo.size()==8&&agent.pending_work()==8);query.command_id=id<CommandId>(108);assert(!agent.execute(query,10));
    agent.fence_binding(query.binding_id,1,false);assert(!agent.quiescent(id<SessionId>(10),9));
    assert(!driver.take(11));assert(agent.pending_work()==0&&agent.quiescent(id<SessionId>(10),9));
    LocalCecAgent churn;TestWire changed;changed.bind(churn);assert(churn.install_policy(p,0));query=operation;query.kind=OperationKind::query_state;
    assert(churn.execute(query,10));churn.fence_binding(query.binding_id,1,false);auto next=p;++next.policy_generation;
    next.bindings[0].binding.endpoint.logical_address=8;++next.bindings[0].binding.binding_revision;
    assert(churn.install_policy(next,11));assert(!changed.take(12));query=press(next);query.kind=OperationKind::query_state;
    assert(churn.execute(query,13));auto wire=changed.take(14);assert(wire&&wire->frame.destination==8);changed.complete(true,15);
    LocalCecAgent expiration;TestWire late;late.bind(expiration);assert(expiration.install_policy(p,0));query=operation;query.kind=OperationKind::query_state;
    assert(expiration.execute(query,10));assert(!late.take(9000)&&late.rejected_before_start==1&&expiration.pending_work()==0);
    LocalCecAgent barrier;TestWire busy;busy.bind(barrier);assert(barrier.install_policy(p,0));assert(barrier.execute(operation,10));assert(busy.take(11));
    barrier.close_route(id<SessionId>(10),9);assert(!barrier.quiescent(id<SessionId>(10),9));busy.complete(true,12);
    assert(!barrier.quiescent(id<SessionId>(10),9));barrier.tick(13,false);assert(busy.take(13)->emergency_release);busy.complete(true,14);assert(barrier.quiescent(id<SessionId>(10),9));
    LocalCecAgent reuse;TestWire reused;reused.bind(reuse);assert(reuse.install_policy(p,0));assert(reuse.execute(operation,10));assert(reused.take(11));reused.complete(true,12);
    reuse.fence_binding(operation.binding_id,1,false);assert(!reused.take(13)&&reuse.uncertain_releases()==1);
    LocalCecAgent rebased;TestWire rebased_driver;rebased_driver.bind(rebased);assert(rebased.install_policy(p,0));assert(rebased.execute(operation,10));assert(rebased_driver.take(11));rebased_driver.complete(true,12);
    query=operation;query.command_id=id<CommandId>(192);query.kind=OperationKind::query_state;assert(rebased.execute(query,20));
    assert(rebased.rebase_revision(7,8));assert(!rebased_driver.take(21));rebased.tick(22);assert(rebased_driver.take(22)->frame.payload[0]==0x44);rebased_driver.complete(true,23);
    auto release=operation;release.kind=OperationKind::release_key;assert(rebased.execute(release,24));assert(rebased_driver.fifo.empty());rebased.tick(24,false);assert(rebased_driver.take(24)->emergency_release);rebased_driver.complete(true,25);
    auto oversized=p;oversized.components.resize(65);LocalCecAgent bounded;assert(!bounded.install_policy(oversized,0));
    auto proxy=p;proxy.port=port(1,Side::front);proxy.role=PortRole::playback_proxy;proxy.owned_logical_address=8;proxy.physical_address=pa(proxy.port);
    auto representation=p.bindings[0];representation.binding.kind=BindingKind::proxy_representation;representation.binding.device_name.reset();
    representation.binding.role=PortRole::playback_proxy;representation.binding.capability=Capability::proxy_activation;representation.binding.endpoint={proxy.port,8};
    representation.binding.proxy_identity_resource_id=id<ResourceId>(101);proxy.bindings={representation};
    LocalCecAgent active;TestWire responses;responses.bind(active);assert(active.install_policy(proxy,0));assert(active.observe({proxy.port,0,15,{0x85}},10));
    active.close_route(id<SessionId>(10),9);assert(!responses.take(11));auto inactive=proxy;++inactive.policy_generation;inactive.route_grants.clear();assert(active.install_policy(inactive,12));
    assert(active.observe({inactive.port,0,15,{0x85}},13));assert(responses.fifo.empty());
    assert(active.observe({inactive.port,0,8,{0x83}},15));wire=responses.take(16);assert(wire&&wire->frame.payload==std::vector<uint8_t>({0x84,0x20,0,4}));responses.complete(true,17);
}
Topology topology() {
    Topology t; t.config_revision = 7; t.ready_owners = {id<OctopusId>(2)};
    auto source = component(7, {port(), "Apple TV"}); auto tv = component(8, {port(1, Side::front), "TV"}); auto projector = component(9, {port(2, Side::front), "Projector"});
    t.components = {source, tv, projector};
    auto b = native(5, source, source.qualified_names[0], 4); t.bindings = {b};
    auto input = id<ResourceId>(40), power = id<ResourceId>(41);
    t.resources = {{input, id<OctopusId>(2), ResourceCompatibility::same_mode, {"X", "Y"}}, {power, id<OctopusId>(2), ResourceCompatibility::same_mode, {"on"}}};
    SignalPort from{source.component_id, 0, SignalDirection::output}, tvport{tv.component_id, 0, SignalDirection::input}, pjport{projector.component_id, 0, SignalDirection::input};
    t.signal_edges = {{from, tvport, SignalEdgeKind::hdmi_cable, Support::supported}, {from, pjport, SignalEdgeKind::hdmi_cable, Support::supported}};
    RouteDefinition a; a.route_id = id<RouteId>(50); a.source = source.component_id; a.display = tv.component_id; a.physical_path = {from, tvport};
    a.demands = {{input, "X", {}}, {power, "on", {}}}; a.required_bindings = {b.binding_id};
    a.steps = {{b.binding_id, OperationKind::query_state, {}, {}, true}, {b.binding_id, OperationKind::query_state, {}, {0}, false}};
    auto second = a; second.route_id = id<RouteId>(51); second.display = projector.component_id; second.physical_path = {from, pjport};
    auto conflict = a; conflict.route_id = id<RouteId>(52); conflict.demands[0].mode = "Y";
    t.routes = {a, second, conflict};
    t.activities = {{id<ActivityId>(60), id<ZoneId>(70), a.route_id}, {id<ActivityId>(61), id<ZoneId>(71), second.route_id}, {id<ActivityId>(62), id<ZoneId>(72), conflict.route_id}};
    return t;
}
SelectRequest select(unsigned request, unsigned zone, unsigned activity) {
    return {id<MessageId>(request), id<ZoneId>(zone), id<ActivityId>(activity), id<SessionId>(request + 100), id<PlanId>(request + 200), {}};
}
void planning() {
    ResourcePlanner planner(topology()); auto request = select(80, 70, 60);
    assert(planner.suggested_activities(id<ZoneId>(70)).size() == 1 && planner.sessions().empty());
    auto first = planner.select_activity(request); assert(first.status == PlanningStatus::proposed && first.plan);
    assert(planner.select_activity(request).status == PlanningStatus::duplicate);
    auto duplicate = request; duplicate.activity_id = id<ActivityId>(62); assert(planner.select_activity(duplicate).status == PlanningStatus::stale);
    PlanExecution execution(*first.plan); assert(execution.ready_steps().empty());
    assert(execution.prepared(id<OctopusId>(2), true)); assert(execution.ready_steps().empty());
    assert(execution.barrier_complete(id<OctopusId>(2))); assert(execution.ready_steps() == std::vector<size_t>{0});
    assert(!execution.record(1, StepResult::applied)); assert(execution.record(0, StepResult::applied)); assert(execution.ready_steps().empty());
    assert(execution.record(0, StepResult::observed)); assert(execution.ready_steps() == std::vector<size_t>{1});
    assert(execution.record(1, StepResult::applied)); assert(execution.status() == TransitionStatus::active);
    assert(planner.commit_plan(first.plan->plan_id, true));
    auto second = planner.select_activity(select(81, 71, 61)); assert(second.status == PlanningStatus::proposed); assert(planner.commit_plan(second.plan->plan_id, false));
    auto conflict = planner.select_activity(select(82, 72, 62)); assert(conflict.status == PlanningStatus::conflict && conflict.conflicting_sessions.size() == 2);
    assert(planner.stop_session(first.plan->proposed_session.session_id).empty()); // receiver/source remain shared
    auto released = planner.stop_session(second.plan->proposed_session.session_id); assert(released.size() == 2);
    auto broken = topology(); broken.signal_edges.pop_back(); ResourcePlanner no_route(broken);
    assert(no_route.select_activity(select(83, 71, 61)).status == PlanningStatus::unsupported && no_route.sessions().empty());
    auto return_audio = topology(); return_audio.routes[0].requires_return_audio = true; ResourcePlanner no_arc(return_audio);
    assert(no_arc.select_activity(select(84, 70, 60)).status == PlanningStatus::unsupported);
    auto address_change = topology(); address_change.routes[0].changes_physical_addresses = true; ResourcePlanner revalidate(address_change);
    assert(revalidate.select_activity(select(85, 70, 60)).status == PlanningStatus::unresolved);
    ResourcePlanner restart(topology()); auto running = restart.select_activity(select(86, 70, 60)); restart.commit_plan(running.plan->plan_id, true); restart.suspend_after_restart();
    assert(restart.sessions()[0].status == SessionStatus::suspended_after_restart && restart.sessions()[0].demands.empty());
    auto too_large = topology(); too_large.routes[0].physical_path.resize(33); ResourcePlanner bounded(too_large);
    assert(bounded.select_activity(select(87, 70, 60)).status == PlanningStatus::unresolved);
    ResourcePlanner reservation_cap(topology(), 1); auto one = reservation_cap.select_activity(select(88, 70, 60)); assert(one.plan);
    assert(reservation_cap.select_activity(select(89, 71, 61)).status == PlanningStatus::overloaded);
    auto malformed = *first.plan; malformed.steps[0].dependencies = {0}; PlanExecution bad_graph(malformed);
    assert(bad_graph.status() == TransitionStatus::aborted && bad_graph.ready_steps().empty());
}
// Two electrical domains and two independent local monotonic clocks. This is a
// deterministic semantic adapter fixture, not a substitute for TLS or GPIO tests.
struct Simulation {
    struct Node { LocalCecAgent agent; uint64_t clock_ms=0; std::vector<WireWork> capture; TestWire driver; };
    struct Packet { unsigned receiver; uint64_t delivery_time; AuthorizedCecOperation operation; bool lost; };
    Node nodes[2];
    Simulation(){nodes[0].clock_ms=100;nodes[1].clock_ms=500000;for(auto& node:nodes)node.driver.bind(node.agent);}
    uint64_t world_time = 0;
    std::vector<Packet> packets;
    std::vector<AdmissionCode> outcomes;
    bool send(unsigned receiver, const AuthorizedCecOperation& operation, uint64_t delay = 0, bool lost = false) {
        if (receiver > 1 || packets.size() >= 64 || delay > UINT64_MAX - world_time) return false;
        packets.push_back({receiver, world_time + delay, operation, lost}); return true;
    }
    void network_until(uint64_t time) {
        assert(time >= world_time); world_time = time;
        for (size_t i = 0; i < packets.size();) {
            if (packets[i].delivery_time > time) { ++i; continue; }
            auto packet = packets[i]; packets.erase(packets.begin() + static_cast<std::ptrdiff_t>(i));
            if (!packet.lost) outcomes.push_back(nodes[packet.receiver].agent.execute(packet.operation, nodes[packet.receiver].clock_ms).code);
        }
    }
    void advance(unsigned node, uint64_t elapsed_ms) {
        assert(elapsed_ms <= UINT64_MAX - nodes[node].clock_ms); nodes[node].clock_ms += elapsed_ms;
        nodes[node].agent.tick(nodes[node].clock_ms);
    }
    void drain(unsigned node) {
        for (unsigned i = 0; i < 128; ++i) {
            auto frame = nodes[node].driver.take(nodes[node].clock_ms); if (!frame) return;
            assert(nodes[node].capture.size() < 256); nodes[node].capture.push_back(*frame); nodes[node].driver.complete(true,nodes[node].clock_ms);
        }
        assert(false); // Every bounded workload must terminate.
    }
    void power_cut(unsigned node) { nodes[node].agent = LocalCecAgent{}; nodes[node].driver = TestWire{};nodes[node].driver.bind(nodes[node].agent);nodes[node].clock_ms = 0; }
};
void multi_node_simulation() {
    Simulation sim;
    auto isolated = local_policy(); isolated.authority_expires_at_ms = 600; isolated.route_grants[0].expires_at_ms = 600;
    auto protected_domain = local_policy(); protected_domain.port = port(0, Side::front, 3);
    protected_domain.domain_id = id<DomainId>(44); protected_domain.owner_boot_id = id<BootId>(33);
    protected_domain.authority_expires_at_ms = 510000; protected_domain.role = PortRole::playback_proxy;
    protected_domain.owned_logical_address = 8; protected_domain.physical_address = pa(protected_domain.port);
    protected_domain.physical_address->domain_id = protected_domain.domain_id;
    auto tv = component(8, {protected_domain.port, "Screen"}); auto tv_binding = native(6, tv, tv.qualified_names[0], 0, Capability::power);
    tv_binding.role = PortRole::playback_proxy; tv_binding.domain_id = protected_domain.domain_id;
    protected_domain.components = {tv}; protected_domain.bindings = {{tv_binding, false, true, true, false}};
    protected_domain.route_grants = {{tv_binding.binding_id, id<SessionId>(11), 1, 510000, {}}};
    assert(sim.nodes[0].agent.install_policy(isolated, sim.nodes[0].clock_ms));
    assert(sim.nodes[1].agent.install_policy(protected_domain, sim.nodes[1].clock_ms));
    // Incoming source frames stay on their isolated native domain even when
    // another board controls a display and its clock has an unrelated origin.
    for (unsigned opcode = 0; opcode < 256; ++opcode) {
        sim.nodes[0].agent.observe({isolated.port, 4, 15, {static_cast<uint8_t>(opcode), 0x20, 0}}, sim.nodes[0].clock_ms);
        sim.drain(0); sim.drain(1);
    }
    assert(sim.nodes[1].capture.empty());
    auto command = press(isolated); command.execution_expires_at_ms = 600;
    assert(sim.send(0, command)); sim.network_until(0);
    auto first_press = sim.nodes[0].driver.take(sim.nodes[0].clock_ms); assert(first_press); sim.nodes[0].capture.push_back(*first_press); sim.nodes[0].driver.complete(true,sim.nodes[0].clock_ms); assert(sim.nodes[0].agent.held_keys() == 1);
    auto release = command; release.kind = OperationKind::release_key;
    assert(sim.send(0, release, 10, true)); sim.network_until(10); // Lost network release.
    const auto protected_clock = sim.nodes[1].clock_ms;
    sim.advance(0, 751); sim.drain(0);
    assert(sim.nodes[0].agent.held_keys() == 0 && sim.nodes[0].capture.back().emergency_release);
    assert(sim.nodes[1].clock_ms == protected_clock); // A local watchdog never compares peer clocks.
    auto delayed = press(isolated, 120, 121); delayed.kind = OperationKind::query_state; delayed.execution_expires_at_ms = 5000;
    assert(sim.send(0, delayed, 200));
    sim.nodes[0].agent.close_route(id<SessionId>(10), 9);
    auto next = isolated; ++next.policy_generation; next.authority_expires_at_ms = 10000; next.route_grants[0].expires_at_ms = 10000; next.route_grants[0].route_generation = 10;
    assert(sim.nodes[0].agent.install_policy(next, sim.nodes[0].clock_ms));
    auto fresh = press(next, 122, 123); fresh.kind = OperationKind::query_state; fresh.fence.route_generation = 10;
    assert(sim.send(0, fresh)); sim.network_until(20); assert(sim.outcomes.back() == AdmissionCode::accepted); sim.drain(0);
    sim.network_until(210); assert(sim.outcomes.back() == AdmissionCode::stale);
    assert(sim.send(0, fresh, 20)); sim.power_cut(0); sim.network_until(230);
    assert(sim.outcomes.back() == AdmissionCode::unauthorized); // Cached desired state does not restore a live executor.
    next.owner_boot_id = id<BootId>(34); assert(sim.nodes[0].agent.install_policy(next, 0));
    assert(sim.send(0, fresh)); sim.network_until(231); assert(sim.outcomes.back() == AdmissionCode::stale); // Old boot packet.
    // A sender's clock cannot be used as the actuator's receipt-based expiry.
    fresh.fence.owner_boot_id = next.owner_boot_id; fresh.execution_expires_at_ms = sim.nodes[1].clock_ms + 500;
    assert(sim.send(0, fresh)); sim.network_until(232); assert(sim.outcomes.back() == AdmissionCode::expired);
}
int main() {
    codecs(); discovery(); policy(); planning(); multi_node_simulation();
    std::cout << "amity_system: codecs, discovery, identity allocation, isolation, binding fences, holds, barriers, resources and two-node faults passed\n";
}
