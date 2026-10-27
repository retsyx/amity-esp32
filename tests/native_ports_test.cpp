#include "cec_hub_fixture.hpp"
#include "amity/live_system.hpp"
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

#include <array>

static LocalConfiguration four_connectors() {
    const auto base = configuration(); auto result = base;
    result.topology = {}; result.topology.config_revision = 1;
    result.activities.clear(); result.port_templates.clear();
    const auto group = live_id<ConsortiumId>(1); const auto owner = live_id<OctopusId>(2);
    for (unsigned id = 0; id < 4; ++id) {
        auto part = base; const unsigned offset = 100 * id;
        const CecPortRef front{group, owner, static_cast<uint8_t>(id), Side::front}, back{group, owner, static_cast<uint8_t>(id), Side::back};
        for (auto& component : part.topology.components) {
            component.component_id = live_id<ComponentId>((component.component_id == live_id<ComponentId>(10) ? 10 : 11) + offset);
            for (auto& name : component.qualified_names) name.port = name.port.side == Side::front ? front : back;
        }
        for (auto& policy : part.port_templates) {
            policy.port = policy.port.side == Side::front ? front : back;
            policy.domain_id = live_id<DomainId>((policy.port.side == Side::front ? 20 : 21) + offset);
            policy.components = part.topology.components; policy.bindings.clear();
            if (policy.physical_address) {
                policy.physical_address->attachment = policy.port; policy.physical_address->domain_id = policy.domain_id;
                policy.physical_address->physical_address = static_cast<uint16_t>((id + 1) << 12);
                if (id == 2) { policy.physical_address->confirmed = false; policy.physical_address->provenance = PaProvenance::legacy_minimum_hint; }
            }
        }
        for (unsigned j = 0; j < part.topology.bindings.size(); ++j) {
            auto& binding = part.topology.bindings[j]; binding.binding_id = live_id<BindingId>(30 + offset + j);
            binding.component_id = live_id<ComponentId>((j == 0 ? 11 : 10) + offset);
            auto& policy = part.port_templates[j == 0 || j == 4 ? 0 : 1];
            binding.endpoint.port = policy.port; binding.domain_id = policy.domain_id;
            if (binding.device_name) binding.device_name->port = policy.port;
            if (binding.proxy_identity_resource_id) binding.proxy_identity_resource_id = live_id<ResourceId>(90 + offset);
            policy.bindings.push_back({binding, true, true, policy.role == PortRole::playback_proxy && j == 0, j == 2});
        }
        auto& route = part.topology.routes[0]; route.route_id = live_id<RouteId>(50 + offset);
        route.source = live_id<ComponentId>(10 + offset); route.display = live_id<ComponentId>(11 + offset);
        route.physical_path[0].component_id = route.source; route.physical_path[1].component_id = route.display;
        for (unsigned j = 0; j < route.required_bindings.size(); ++j) route.required_bindings[j] = live_id<BindingId>(30 + offset + j);
        for (auto& step : route.steps) {
            unsigned index = step.binding_id == live_id<BindingId>(30) ? 0 : step.binding_id == live_id<BindingId>(31) ? 1 : step.binding_id == live_id<BindingId>(32) ? 2 : 4;
            step.binding_id = live_id<BindingId>(30 + offset + index);
            if (step.operation == OperationKind::select_path) step.arguments.physical_path = static_cast<uint16_t>((id + 1) << 12);
        }
        route.demands[0].resource_id = live_id<ResourceId>(90 + offset);
        part.topology.resources[0].resource_id = live_id<ResourceId>(90 + offset);
        for (unsigned j = 0; j < part.topology.signal_edges.size(); ++j) {
            part.topology.signal_edges[j].from.component_id = route.source;
            part.topology.signal_edges[j].to.component_id = route.display;
        }
        result.topology.components.insert(result.topology.components.end(), part.topology.components.begin(), part.topology.components.end());
        result.topology.bindings.insert(result.topology.bindings.end(), part.topology.bindings.begin(), part.topology.bindings.end());
        result.topology.routes.push_back(route); result.topology.resources.push_back(part.topology.resources[0]);
        result.topology.signal_edges.push_back(part.topology.signal_edges[0]);
        result.topology.activities.push_back({live_id<ActivityId>(70 + offset), live_id<ZoneId>(60 + offset), route.route_id});
        result.activities.push_back({live_id<ActivityId>(70 + offset), live_id<ZoneId>(60 + offset), "Watch", route.source, route.display, {}});
        result.port_templates.insert(result.port_templates.end(), part.port_templates.begin(), part.port_templates.end());
    }
    return result;
}
static void adapter_settings_owner_tests() {
    fixture_tick = 1000; fixture_send = {}; fixture_transmit = {};
    fixture_incoming.clear(); fixture_sent.clear(); fixture_present = false;
    const auto boot = live_id<BootId>(3);
    auto setup = [&](amity::HubService& owner) {
        owner.set_system_identity(live_id<ConsortiumId>(1), live_id<OctopusId>(2));
        owner.set_boot_identity(boot);
    };
    auto deadline = [] { return uint64_t(fixture_time) / 1000 + 5000; };
    {
        amity::HubService owner; setup(owner);
        assert(owner.start(cec::legacy_buttonless_profile, std::array<uint16_t,4>{{0x2000,0x3000,0x4000,0x5000}}) == ESP_OK);
        run_idle(2); auto slots = owner.adapter_runtime_snapshot(); assert(slots.size() == 4);
        for (unsigned i=0;i<4;++i) assert(slots[i].tentacle_id == i && slots[i].configured == (i==0));
        assert(slots[0].present == std::optional<bool>{true} && slots[0].effective_front_physical == 0x2000);
        assert(!slots[1].present && !slots[1].effective_front_physical);
        assert(owner.configure_v2(configuration()) == ESP_OK); run_idle(30);
        assert(owner.execution_snapshot().applied_config_revision == 1 && owner.snapshot().ready);
        slots = owner.adapter_runtime_snapshot();
        assert(!slots[0].observed_front_physical && slots[0].effective_front_physical == 0x2000);
        unsigned writes = 0; const auto expected = slots[0].presence_generation;
        assert(owner.commit_adapter_settings(live_id<BootId>(4), {{0,expected}}, deadline(), [&]{++writes;return ESP_OK;}) == ESP_ERR_INVALID_STATE);
        assert(owner.commit_adapter_settings(boot, {{0,expected}}, deadline(), [&]{++writes;return ESP_FAIL;}) == ESP_FAIL);
        assert(writes == 1 && !owner.maintenance_ready()); // Failed persistence restores fresh admission.
        assert(owner.commit_adapter_settings(boot, {{0,expected}}, deadline(), [&]{
            ++writes; fixture_time += 6000000; fixture_late_reply = true; return ESP_OK; // A started bounded flash write may cross the request deadline.
        }) == ESP_OK);
        assert(writes == 2 && owner.maintenance_ready() && !fixture_late_reply);
        assert(owner.commit_adapter_settings(boot, {{0,expected}}, deadline(), [&]{++writes;return ESP_OK;}) == ESP_ERR_INVALID_STATE);
        assert(writes == 2); // Durable success stays quiescent for its acknowledged restart.
        assert(owner.commit_adapter_settings(boot, {{4,expected}}, deadline(), [&]{++writes;return ESP_OK;}) == ESP_ERR_INVALID_ARG);
    }
    auto sparse = cec::legacy_buttonless_profile; sparse.id = cec::BoardProfileId::Development4Tentacle; sparse.tentacle_count = 4;
    for(unsigned i=0;i<8;++i)sparse.ports[i]={static_cast<uint8_t>(i/2),i%2?cec::Side::Back:cec::Side::Front,-1};
    {
        amity::HubService owner; setup(owner); assert(owner.start(sparse)==ESP_OK);run_idle(2);
        for(const auto& slot:owner.adapter_runtime_snapshot())assert(!slot.configured&&!slot.present&&!slot.effective_front_physical);
        unsigned writes=0;assert(owner.commit_adapter_settings(boot,{{3,1}},deadline(),[&]{++writes;return ESP_OK;})==ESP_ERR_INVALID_STATE);assert(!writes);
    }
    sparse.ports[6].gpio=9;sparse.ports[7].gpio=44;
    {
        amity::HubService owner;setup(owner);assert(owner.start(sparse)==ESP_OK);run_idle(2);
        const auto slot=owner.adapter_runtime_snapshot()[3];assert(slot.configured&&!slot.present); // CEC silence never becomes physical presence.
        unsigned writes=0;assert(owner.commit_adapter_settings(boot,{{3,slot.presence_generation}},deadline(),[&]{++writes;return ESP_OK;})==ESP_ERR_INVALID_STATE);assert(!writes);
    }
    sparse.presence[3]={20,true,false,false};
    {
        int detector=1;unsigned reads=0;bool bounce=false;
        amity::HubService owner;setup(owner);assert(owner.set_presence_reader([&](int gpio){assert(gpio==20);if(bounce&&++reads==1)return 0;return detector;})==ESP_OK);
        assert(owner.start(sparse,std::array<uint16_t,4>{{0x1000,0x2000,0x3000,0x4000}})==ESP_OK);run_idle(2);
        auto slot=owner.adapter_runtime_snapshot()[3];assert(slot.present==std::optional<bool>{true}&&slot.effective_front_physical==0x4000);
        unsigned writes=0;bounce=true;reads=0;
        assert(owner.commit_adapter_settings(boot,{{3,slot.presence_generation}},deadline(),[&]{++writes;return ESP_OK;})==ESP_ERR_INVALID_STATE);assert(!writes);
        bounce=false;run_idle(2);auto replacement=owner.adapter_runtime_snapshot()[3];
        assert(replacement.present==std::optional<bool>{true}&&replacement.presence_generation>slot.presence_generation); // Removal/reinsert ABA is not the dialog's original attachment.
        detector=0;run_idle(2);slot=owner.adapter_runtime_snapshot()[3];assert(slot.present==std::optional<bool>{false}&&!slot.effective_front_physical);
        detector=-1;run_idle(2);slot=owner.adapter_runtime_snapshot()[3];assert(!slot.present&&!slot.effective_front_physical);
        detector=1;run_idle(2);slot=owner.adapter_runtime_snapshot()[3];
        const auto too_short=uint64_t(fixture_time)/1000+3;
        assert(owner.commit_adapter_settings(boot,{{3,slot.presence_generation}},too_short,[&]{++writes;return ESP_OK;})==ESP_ERR_TIMEOUT);run_idle(3);assert(!writes);
        slot=owner.adapter_runtime_snapshot()[3];assert(owner.commit_adapter_settings(boot,{{3,slot.presence_generation}},deadline(),[&]{++writes;return ESP_OK;})==ESP_OK);assert(writes==1);
    }
    fixture_tick=100000;fixture_present=true;
}
int main() {
    fixture_present = true;
    fixture_transmit = [](int gpio, const cec::Frame& frame) {
        const unsigned id = static_cast<unsigned>(gpio - 1) / 2;
        if (id == 1) return cec::TxResult::LineError;
        const uint8_t peer = gpio % 2 ? 0 : 4;
        return id != 2 && frame.destination() == peer ? cec::TxResult::Success : cec::TxResult::Nack;
    };
    fixture_send = [](int gpio, const cec::Frame& frame) {
        const unsigned id = static_cast<unsigned>(gpio - 1) / 2;
        if (id == 1 || id == 2) return false;
        const uint8_t peer = gpio % 2 ? 0 : 4;
        if (frame.size < 2 || frame.destination() != peer) return true;
        cec::Frame reply; reply.data[0] = static_cast<uint8_t>(peer << 4 | frame.source());
        if (frame.data[1] == 0x83) { reply = {{static_cast<uint8_t>(peer << 4 | 15), 0x84, static_cast<uint8_t>(peer ? (id + 1) << 4 : 0), 0, static_cast<uint8_t>(peer ? 4 : 0)}, 5}; }
        else if (frame.data[1] == 0x46) { const char* name = peer ? "Source" : "TV"; reply.data[1] = 0x47; reply.size = 2; while (*name) reply.data[reply.size++] = *name++; }
        else if (frame.data[1] == 0x8c) reply = {{static_cast<uint8_t>(peer << 4 | 15), 0x87, 0, 0, 0}, 5};
        else return true;
        fixture_incoming.push_back({gpio, reply}); return true;
    };
    auto config = four_connectors(); std::array<cec::Bus, 8> buses;
    amity::LiveSystem live(live_id<ConsortiumId>(1), live_id<OctopusId>(2), live_id<BootId>(3), [](uint8_t* data, size_t size) { static uint8_t value=1; std::fill(data,data+size,value++); return true; });
    for (unsigned i = 0; i < buses.size(); ++i) {
        assert(buses[i].start(i + 1) == ESP_OK);
        live.attach(config.port_templates[i].port, buses[i], 15, i % 2 ? 0 : static_cast<uint16_t>((i / 2 + 6) << 12));
    }
    live.set_tentacle_present(1, false, 0); // An explicit separate detector; silence on 2 stays unknown.
    std::string error; assert(live.configure(config, 100, error));
    assert(live.snapshot({}).ready && live.execution_snapshot().applied_config_revision == 1);
    for (uint64_t now = 200; now < 18000; now += 50) live.tick(now);
    const auto policies = live.local_policy_snapshot(); assert(policies.size() == 8);
    size_t binding_count = 0;
    for (const auto& policy : policies) {
        binding_count += policy.bindings.size();
        const unsigned id = policy.port.tentacle_id;
        if (id == 1 || (id == 2 && policy.port.side == Side::front)) assert(policy.role == PortRole::monitor && !policy.owned_logical_address && std::all_of(policy.bindings.begin(), policy.bindings.end(), [](const auto& binding) { return binding.binding.status != BindingStatus::verified; }));
        for (const auto& permission : policy.bindings) if (permission.binding.status != BindingStatus::verified) {
            assert(permission.binding.binding_revision && permission.binding.physical_address_evidence_ref);
            assert(permission.binding.role == policy.role && !permission.discrete_power_on && !permission.discrete_power_off && !permission.image_view_on && !permission.exact_path_selection);
        }
    }
    assert(binding_count == config.topology.bindings.size()); // Unavailable records remain synchronizable diagnostics.
    const auto observations = live.execution_snapshot().native_observations;
    for (unsigned id : {0u,3u}) assert(std::count_if(observations.begin(), observations.end(), [&](const auto& o) { return o.endpoint.port.tentacle_id == id && o.osd_name; }) == 2);
    assert(live.front_physical_addresses(0).first == 0x1000 && live.front_physical_addresses(0).second == 0x1000);
    assert(!live.front_physical_addresses(1).first && !live.front_physical_addresses(1).second);
    assert(!live.front_physical_addresses(2).first && live.front_physical_addresses(2).second == 0x8000);
    assert(live.front_physical_addresses(3).first == 0x4000 && live.front_physical_addresses(3).second == 0x4000);
    fixture_sent.clear();
    assert(live.select(live_id<ZoneId>(60), live_id<ActivityId>(70), live_id<MessageId>(700), {}, 18000, error));
    for (uint64_t now = 18050; now < 22000; now += 50) live.tick(now);
    assert(live.select(live_id<ZoneId>(360), live_id<ActivityId>(370), live_id<MessageId>(701), {}, 22000, error));
    for (uint64_t now = 22050; now < 26000; now += 50) live.tick(now);
    assert(live.execution_snapshot().sessions.size() == 2 && live.snapshot({}).current_activity == 5);
    assert(!live.select(live_id<ZoneId>(260), live_id<ActivityId>(270), live_id<MessageId>(702), {}, 26000, error));
    assert(!live.select(live_id<ZoneId>(160), live_id<ActivityId>(170), live_id<MessageId>(703), {}, 26000, error));
    InputIntent hold; hold.action = InputAction::key_press; hold.zone_id = live_id<ZoneId>(360); hold.key = 1; hold.repeat = true;
    hold.hold_token = live_id<HoldToken>(800); hold.expected_state_revision = live.execution_snapshot().state_revision;
    assert(live.input(hold, 26001));
    fixture_sent.clear(); live.set_tentacle_present(3, false, 26002); // Queue canceled before dispatch, including unsafe cleanup.
    for (uint64_t now = 26050; now < 28000; now += 50) live.tick(now);
    assert(live.execution_snapshot().sessions.size() == 1 && live.snapshot({}).current_activity == 5 && live.snapshot({}).ready);
    for (const auto& sent : fixture_sent) assert(sent.gpio != 7 && sent.gpio != 8);
    fixture_sent.clear(); live.set_tentacle_present(3, true, 28000);
    for (uint64_t now = 28050; now < 44000; now += 50) live.tick(now);
    assert(live.execution_snapshot().sessions.size() == 1); // No route/session is recreated by reinsertion or discovery.
    for (const auto& sent : fixture_sent) assert(sent.frame.size < 2 || (sent.frame.data[1] != 0x44 && sent.frame.data[1] != 0x45 && sent.frame.data[1] != 0x04 && sent.frame.data[1] != 0x82 && sent.frame.data[1] != 0x86));
    hold.action = InputAction::hold_release; hold.expected_state_revision = live.execution_snapshot().state_revision;
    assert(!live.input(hold, 44000));
    assert(live.select(live_id<ZoneId>(360), live_id<ActivityId>(370), live_id<MessageId>(704), {}, 44000, error));
    for (uint64_t now = 44050; now < 48000; now += 50) live.tick(now);
    if(live.execution_snapshot().sessions.size()!=2)std::fprintf(stderr,"reinsert plan failed: %s (snapshot %s)\n",error.c_str(),live.execution_snapshot().error.c_str());
    assert(live.execution_snapshot().sessions.size() == 2);
    // Unknown physical presence is distinct from stale native evidence. A
    // stalled replacement scan must not preserve old binding authority forever.
    fixture_completion_ready=[](int gpio){return gpio!=7&&gpio!=8;};
    for(uint64_t now=48000;now<80000;now+=50)live.tick(now);
    const auto expired=live.local_policy_snapshot();
    for(const auto& policy:expired)if(policy.port.tentacle_id==3)
        for(const auto& permission:policy.bindings)if(permission.binding.kind==BindingKind::native_appliance)assert(permission.binding.status!=BindingStatus::verified);
    assert(!live.select(live_id<ZoneId>(360),live_id<ActivityId>(370),live_id<MessageId>(705),{},80000,error));
    fixture_completion_ready={};
    // Reuse the detached lane to retain noisy-wire coverage within four slots.
    live.set_tentacle_present(1, true, 80001);
    for(uint64_t now=80050;now<96000;now+=50)live.tick(now);
    for(const auto& policy:live.local_policy_snapshot())if(policy.port.tentacle_id==1)
        assert(policy.role==PortRole::monitor&&!policy.owned_logical_address);
    assert(!live.select(live_id<ZoneId>(160),live_id<ActivityId>(170),live_id<MessageId>(706),{},96000,error));
    auto profile = cec::legacy_buttonless_profile; profile.id = cec::BoardProfileId::Development4Tentacle; profile.tentacle_count = 4;
    for (unsigned id=0;id<4;++id) for(unsigned side=0;side<2;++side) profile.ports[id*2+side]={static_cast<uint8_t>(id),side?cec::Side::Back:cec::Side::Front,static_cast<int>(id*2+side+1)};
    { amity::HubService owner; owner.set_system_identity(live_id<ConsortiumId>(1),live_id<OctopusId>(2));
      assert(owner.start(profile)==ESP_OK && !owner.connector_presence(3));
      assert(owner.configure_v2(config)==ESP_OK);run_idle(100);
      assert(owner.local_policy_snapshot().size()==8 && owner.execution_snapshot().applied_config_revision==1 && owner.snapshot().ready);
      assert(owner.begin_maintenance()==ESP_OK && owner.maintenance_ready());owner.end_maintenance();
      assert(owner.retire_membership()==ESP_OK && !owner.snapshot().ready);
    }
    {
        amity::HubService owner; owner.set_system_identity(live_id<ConsortiumId>(1),live_id<OctopusId>(2));
        fixture_stopped_buses.clear(); fixture_start_bus=[](int gpio){return gpio==5?ESP_ERR_NO_MEM:ESP_OK;};
        assert(owner.start(profile)==ESP_ERR_NO_MEM);
        for(int gpio=1;gpio<5;++gpio) assert(std::find(fixture_stopped_buses.begin(),fixture_stopped_buses.end(),gpio)!=fixture_stopped_buses.end());
        fixture_start_bus={}; auto empty=profile; for(auto& pin:empty.ports)pin.gpio=-1;
        assert(owner.start(empty)==ESP_OK); auto unavailable=config;
        for(auto& policy:unavailable.port_templates){policy.role=PortRole::disabled;policy.owned_logical_address.reset();policy.bindings.clear();}
        fixture_sent.clear(); fixture_transmitted.clear();
        assert(owner.configure_v2(unavailable)==ESP_OK);run_idle(3);
        assert(owner.snapshot().ready&&owner.execution_snapshot().applied_config_revision==1);
        assert(fixture_sent.empty()&&fixture_transmitted.empty());
        assert(owner.request_activity(5)==ESP_OK);run_one_command();
        assert(owner.execution_snapshot().sessions.empty());
    }
    {
        // A monitor earlier in the port list must not steal a shared-domain
        // scanner's directed queries or use another port's source identity.
        fixture_incoming.clear(); fixture_sent.clear(); fixture_transmitted.clear();
        fixture_transmit=[](int, const cec::Frame& frame) { return frame.destination()==4 ? cec::TxResult::Success : cec::TxResult::Nack; };
        fixture_send=[](int, const cec::Frame& frame) {
            if(frame.size<2 || frame.destination()!=4)return true;
            cec::Frame reply;
            if(frame.data[1]==0x83)reply={{0x4f,0x84,0x10,0,4},5};
            else if(frame.data[1]==0x46)reply={{static_cast<uint8_t>(0x40|frame.source()),0x47,'S','o','u','r','c','e'},8};
            else if(frame.data[1]==0x8c)reply={{0x4f,0x87,0,0,0},5};
            else return true;
            fixture_incoming.push_back({1,reply});fixture_incoming.push_back({2,reply});return true;
        };
        auto shared=configuration(); auto& monitor=shared.port_templates[0];
        monitor.role=PortRole::monitor;monitor.physical_address.reset();monitor.bindings.clear();
        monitor.domain_id=shared.port_templates[1].domain_id;
        for(auto& binding:shared.topology.bindings)if(binding.endpoint.port==monitor.port)binding.domain_id=monitor.domain_id;
        cec::Bus a,b;assert(a.start(1)==ESP_OK&&b.start(2)==ESP_OK);
        amity::LiveSystem domain(live_id<ConsortiumId>(1),live_id<OctopusId>(2),live_id<BootId>(3),[](uint8_t* data,size_t size){static uint8_t v=1;std::fill(data,data+size,v++);return true;});
        domain.attach(monitor.port,a,15,0xffff);domain.attach(shared.port_templates[1].port,b,15,0);
        assert(domain.configure(shared,100,error));for(uint64_t now=150;now<10000;now+=50)domain.tick(now);
        assert(std::any_of(fixture_sent.begin(),fixture_sent.end(),[](const auto& sent){return sent.gpio==2&&sent.frame.size>1&&sent.frame.data[1]==0x46&&sent.frame.source()==0;}));
        for(const auto& sent:fixture_sent)assert(sent.gpio!=1||sent.frame.size<2||sent.frame.data[1]!=0x46);
        const auto scoped=domain.local_policy_snapshot();
        assert(std::any_of(scoped[1].bindings.begin(),scoped[1].bindings.end(),[](const auto& permission){return permission.binding.binding_id==live_id<BindingId>(31)&&permission.binding.status==BindingStatus::verified;}));
    }
    adapter_settings_owner_tests();
    std::puts("Native four-connector owner, absent/noisy lane isolation, scoped presence fencing and explicit resume passed");
}
