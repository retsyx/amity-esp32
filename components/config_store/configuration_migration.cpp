#include "system_configuration.hpp"
#include <map>
#include <set>

namespace config_store {
namespace {
struct IdSource {
    std::function<bool(uint8_t*, size_t)> random;
    std::vector<std::string> pool;
    size_t next = 0;
    bool failed = false;
    template<class T> T make() {
        std::optional<T> result;
        if (!pool.empty()) { if (next < pool.size()) result = T::parse(pool[next++]); }
        else result = T::generate(random);
        if (!result) { failed = true; return {}; } return *result;
    }
};
bool write_checked(ConfigBackend& backend, const char* key, const std::string& value) {
    if (!backend.write(key, value)) return false;
    std::string reread;
    return backend.read(key, reread) == ReadResult::found && reread == value;
}
const LegacyEvidence* unique(const std::vector<LegacyEvidence>& evidence, const std::string& name, model::Side side, model::ConsortiumId consortium) {
    const LegacyEvidence* selected = nullptr;
    for (const auto& item : evidence) if (item.name.valid() && item.name.osd_name == name && item.name.port.side == side && item.name.port.consortium_id == consortium && item.native) {
        if (!item.unique || selected) return nullptr;
        selected = &item;
    }
    return selected;
}
void capability(ConfiguredComponent& c, model::Capability cap) {
    for (const auto& item : c.capabilities) if (item.capability == cap) return;
    c.capabilities.push_back({cap, model::Support::supported});
}
ConfiguredComponent* component(ConfigurationV2& c, model::ComponentId id) {
    for (auto& item : c.components) if (item.record.component_id == id) return &item;
    return nullptr;
}
ConfiguredDomain* domain(ConfigurationV2& c, const model::CecPortRef& port, model::PortRole role, IdSource& ids, bool allow_monitor_upgrade) {
    for (auto& d : c.domains) for (auto& p : d.ports) if (p.port == port) {
        if (p.role == role && d.confirmed) return &d;
        if (allow_monitor_upgrade && !d.confirmed && d.ports.size() == 1 && d.owner_octopus_id == port.octopus_id &&
            (p.role == model::PortRole::monitor || p.role == model::PortRole::disabled) && d.domain_revision != UINT64_MAX) {
            ++d.domain_revision; d.confirmed = true; p.role = role; p.physical_address.reset(); return &d;
        }
        return nullptr;
    }
    ConfiguredDomain d; d.domain_id = ids.make<model::DomainId>(); d.owner_octopus_id = port.octopus_id; d.confirmed = true;
    d.ports.push_back({port, role, {}}); c.domains.push_back(d); return &c.domains.back();
}
model::ResourceId resource(ConfigurationV2& c, model::OctopusId owner, model::ResourceCompatibility compatibility,
    const std::string& mode, const std::string& key, std::map<std::string, model::ResourceId>& known, IdSource& ids) {
    auto found = known.find(key);
    if (found != known.end()) {
        for (auto& r : c.resources) if (r.resource_id == found->second && std::find(r.supported_modes.begin(), r.supported_modes.end(), mode) == r.supported_modes.end()) r.supported_modes.push_back(mode);
        return found->second;
    }
    const auto id = ids.make<model::ResourceId>(); c.resources.push_back({id, owner, compatibility, {mode}}); known[key] = id; return id;
}
model::BindingId binding(ConfigurationV2& c, const model::QualifiedDeviceName& name, model::ComponentId component_id,
    model::Capability cap, model::DomainId domain_id, IdSource& ids) {
    for (const auto& b : c.binding_policies) if (b.component_id == component_id && b.capability == cap && b.device_name && *b.device_name == name) return b.binding_id;
    BindingPolicy b; b.binding_id = ids.make<model::BindingId>(); b.component_id = component_id; b.capability = cap;
    b.domain_id = domain_id; b.port = name.port; b.device_name = name;
    b.discrete_power_on = cap == model::Capability::power && name.port.side == model::Side::back;
    b.discrete_power_off = cap == model::Capability::power;
    b.image_view_on = cap == model::Capability::power && name.port.side == model::Side::front;
    c.binding_policies.push_back(b); capability(*component(c, component_id), cap); return b.binding_id;
}
void edge(ConfigurationV2& c, const model::SignalPort& from, const model::SignalPort& to) {
    for (auto& existing : c.signal_edges) if (existing.from == from && existing.to == to) {
        existing.kind = model::SignalEdgeKind::hdmi_cable; existing.support = model::Support::supported; return;
    }
    c.signal_edges.push_back({from, to, model::SignalEdgeKind::hdmi_cable, model::Support::supported});
}
bool same_route(const model::RouteDefinition& a, const model::RouteDefinition& b) {
    if (a.source != b.source || a.display != b.display || a.physical_path != b.physical_path || a.required_bindings != b.required_bindings || a.demands.size() != b.demands.size() || a.steps.size() != b.steps.size() || a.requires_return_audio != b.requires_return_audio || a.return_audio_support != b.return_audio_support || a.changes_physical_addresses != b.changes_physical_addresses || a.qualified_physical_address_stability != b.qualified_physical_address_stability) return false;
    for (size_t n = 0; n < a.demands.size(); ++n) if (a.demands[n].resource_id != b.demands[n].resource_id || a.demands[n].mode != b.demands[n].mode || a.demands[n].control_owner != b.demands[n].control_owner) return false;
    for (size_t n = 0; n < a.steps.size(); ++n) {
        const auto& x = a.steps[n]; const auto& y = b.steps[n];
        if (x.binding_id != y.binding_id || x.operation != y.operation || x.dependencies != y.dependencies || x.require_observation != y.require_observation || x.arguments.on != y.arguments.on || x.arguments.key != y.arguments.key || x.arguments.physical_path != y.arguments.physical_path || x.arguments.query_opcode != y.arguments.query_opcode || x.arguments.hold_token != y.arguments.hold_token || x.arguments.repeat != y.arguments.repeat) return false;
    }
    return true;
}
struct Builder {
    ConfigurationV2& config;
    const std::vector<LegacyEvidence>& evidence;
    IdSource& ids;
    std::vector<std::string>& tasks;
    std::map<std::string, model::ComponentId> named;
    std::map<std::string, model::ResourceId> resources;
    bool legacy_semantics = false;
    model::ComponentId equipment(const std::string& reported_name, model::Side side, const char* kind) {
        if (reported_name.empty()) return {};
        const std::string key = model::side_name(side) + std::string(":") + reported_name;
        auto existing = named.find(key); if (existing != named.end()) return existing->second;
        const auto* observed = unique(evidence, reported_name, side, config.consortium_id);
        if (observed) for (const auto& c : config.components)
            if (std::find(c.record.qualified_names.begin(), c.record.qualified_names.end(), observed->name) != c.record.qualified_names.end()) { named[key] = c.record.component_id; return c.record.component_id; }
        ConfiguredComponent c; c.record.component_id = ids.make<model::ComponentId>(); c.record.label = reported_name; c.generated_label = reported_name;
        c.kind = observed ? kind : "placeholder"; if (observed) c.record.qualified_names.push_back(observed->name);
        else tasks.push_back("Assign " + reported_name + ": actual unique OSD evidence is missing or ambiguous on the expected connection");
        config.components.push_back(c); named[key] = c.record.component_id; return c.record.component_id;
    }
    bool activity(const LegacyActivity& legacy) {
        ConfiguredActivity activity; activity.activity_id = ids.make<model::ActivityId>(); activity.route_id = ids.make<model::RouteId>();
        activity.zone_id = config.primary_zone_id; activity.label = legacy.name; activity.generated_label = legacy.name; activity.enabled = false; activity.switch_input = legacy.switch_input;
        if (!legacy.source.empty()) activity.source = equipment(legacy.source, model::Side::back, "source");
        if (!legacy.display.empty()) activity.display = equipment(legacy.display, model::Side::front, "display");
        if (!legacy.audio.empty()) {
            if (legacy.audio == legacy.display && activity.display) activity.audio = activity.display;
            else activity.audio = equipment(legacy.audio, model::Side::back, legacy.audio == legacy.source ? "source" : "receiver");
        }
        if (!legacy.switch_device.empty()) activity.switch_device = equipment(legacy.switch_device, model::Side::back, "switch");
        const auto* source = unique(evidence, legacy.source, model::Side::back, config.consortium_id);
        const auto* display = unique(evidence, legacy.display, model::Side::front, config.consortium_id);
        if (source && display && activity.source && activity.display && legacy.switch_device.empty() && (legacy.audio.empty() || legacy.audio == legacy.source || legacy.audio == legacy.display) &&
            source->name.port.octopus_id == display->name.port.octopus_id && source->name.port.tentacle_id == display->name.port.tentacle_id &&
            (legacy_semantics || (source->direct_cable_confirmed && display->direct_cable_confirmed)) &&
            (source->physical_address_confirmed || (!legacy_semantics && source->pa_user_confirmed_fallback)) && model::valid_physical_address(source->physical_address, false)) {
            const auto source_port = source->name.port, display_port = display->name.port;
            auto* source_domain = domain(config, source_port, model::PortRole::tv_controller, ids, !legacy_semantics); if (!source_domain) return false;
            const auto source_domain_id = source_domain->domain_id;
            auto* display_domain = domain(config, display_port, model::PortRole::playback_proxy, ids, !legacy_semantics); if (!display_domain) return false;
            const auto display_domain_id = display_domain->domain_id;
            // A raw native PA is retained only as a legacy hint, never promoted
            // into a user-confirmed static fallback or a restored live binding.
            if (!display_domain->ports[0].physical_address) display_domain->ports[0].physical_address = model::PhysicalAddressEvidence{
                display_port, display_domain_id, display_domain->domain_revision, source->physical_address, !legacy_semantics && source->pa_user_confirmed_fallback ? model::PaProvenance::user_confirmed_fallback : model::PaProvenance::legacy_minimum_hint, {}, !legacy_semantics && source->pa_user_confirmed_fallback};
            const auto source_power = binding(config, source->name, *activity.source, model::Capability::power, source_domain_id, ids);
            const auto display_power = binding(config, display->name, *activity.display, model::Capability::power, display_domain_id, ids);
            const auto source_navigation = binding(config, source->name, *activity.source, model::Capability::navigation, source_domain_id, ids);
            std::optional<model::BindingId> audio_binding;
            if (activity.audio) {
                if (activity.audio == activity.display) audio_binding = binding(config, display->name, *activity.display, model::Capability::audio, display_domain_id, ids);
                else audio_binding = binding(config, source->name, *activity.source, model::Capability::audio, source_domain_id, ids);
            }
            const std::string proxy_key = "proxy:" + display_port.octopus_id.str() + ":" + std::to_string(display_port.tentacle_id);
            for (const auto& b : config.binding_policies) if (b.kind == model::BindingKind::proxy_representation && b.port == display_port && b.proxy_identity_resource_id) resources[proxy_key] = *b.proxy_identity_resource_id;
            const auto identity = resource(config, display_port.octopus_id, model::ResourceCompatibility::same_mode, activity.source->str(), proxy_key, resources, ids);
            model::BindingId proxy;
            for (const auto& b : config.binding_policies) if (b.kind == model::BindingKind::proxy_representation && b.component_id == *activity.source && b.port == display_port) proxy = b.binding_id;
            if (!proxy) { BindingPolicy b; b.binding_id = ids.make<model::BindingId>(); b.component_id = *activity.source; b.capability = model::Capability::proxy_activation;
                b.kind = model::BindingKind::proxy_representation; b.domain_id = display_domain_id; b.port = display_port; b.proxy_identity_resource_id = identity; proxy = b.binding_id; config.binding_policies.push_back(b); }
            model::RouteDefinition route; route.route_id = activity.route_id; route.source = *activity.source; route.display = *activity.display;
            model::SignalPort from{*activity.source, 0, model::SignalDirection::output}, to{*activity.display, 0, model::SignalDirection::input}; edge(config, from, to); route.physical_path = {from, to};
            route.required_bindings = {display_power, source_power, proxy, source_navigation};
            if (audio_binding) route.required_bindings.push_back(*audio_binding);
            for (const auto& id : {*activity.source, *activity.display}) {
                const std::string key = "power:" + id.str();
                // Reuse preexisting component power resources from matching route demands.
                if (!resources.count(key)) for (const auto& prior : config.routes) if (prior.demands.size() == 3 && prior.steps.size() == 4 &&
                    prior.steps[0].operation == model::OperationKind::ensure_power && prior.steps[1].operation == model::OperationKind::ensure_power &&
                    prior.steps[2].operation == model::OperationKind::announce_active_source && (prior.source == id || prior.display == id)) {
                    const auto& demand = prior.demands[prior.source == id ? 0 : 1];
                    if (demand.mode == "on") resources[key] = demand.resource_id;
                }
                route.demands.push_back({resource(config, source_port.octopus_id, model::ResourceCompatibility::same_mode, "on", key, resources, ids), "on", {}});
            }
            route.demands.push_back({identity, activity.source->str(), {}});
            model::PlanStep wake_display; wake_display.binding_id = display_power; wake_display.operation = model::OperationKind::ensure_power;
            model::PlanStep wake_source; wake_source.binding_id = source_power; wake_source.operation = model::OperationKind::ensure_power; wake_source.dependencies = {0};
            model::PlanStep activate; activate.binding_id = proxy; activate.operation = model::OperationKind::announce_active_source; activate.dependencies = {1};
            model::PlanStep verify; verify.binding_id = display_power; verify.operation = model::OperationKind::query_state; verify.dependencies = {2}; verify.require_observation = true;
            route.steps = {wake_display, wake_source, activate, verify};
            const auto existing = std::find_if(config.routes.begin(), config.routes.end(), [&](const auto& prior) { return same_route(prior, route); });
            if (existing != config.routes.end()) activity.route_id = existing->route_id;
            else config.routes.push_back(route);
            activity.enabled = true;
        } else tasks.push_back("Complete the verified source-to-screen path for " + legacy.name + "; receiver switching, return audio, or missing attachment evidence needs assignment");
        config.activities.push_back(activity);
        for (auto& zone : config.zones) if (zone.zone_id == activity.zone_id) zone.activity_order.push_back(activity.activity_id);
        if (config.external_slots.size() < 16) {
            uint16_t available = 0;
            while (std::any_of(config.external_slots.begin(), config.external_slots.end(), [&](const auto& slot) { return slot.slot == available; })) ++available;
            config.external_slots.push_back({available, activity.activity_id, false, false, {}});
        }
        return !ids.failed;
    }
};
}

bool propose_local_activity(const ConfigurationV2& previous, const LegacyActivity& desired, const std::vector<LegacyEvidence>& evidence,
    const std::function<bool(uint8_t*, size_t)>& random, ConfigurationV2& candidate, std::vector<std::string>& tasks, std::string& error) {
    if (previous.activities.size() >= 64 || desired.name.empty() || desired.name.size() > 128) { error = "Activity capacity exhausted or invalid name"; return false; }
    const auto* source = unique(evidence, desired.source, model::Side::back, previous.consortium_id);
    const auto* screen = unique(evidence, desired.display, model::Side::front, previous.consortium_id);
    if (source && screen && source->direct_cable_confirmed && screen->direct_cable_confirmed) {
        if (!screen->confirmed_screen_input || screen->confirmed_screen_input > 15) { error = "Choose the explicitly connected screen HDMI input"; return false; }
        if (source->physical_address_confirmed && model::valid_physical_address(source->physical_address, false) && (source->physical_address >> 12) != screen->confirmed_screen_input) { error = "Fresh native physical-address evidence disagrees with the chosen screen input"; return false; }
        if (source->pa_user_confirmed_fallback && (source->physical_address_confirmed || source->physical_address != (screen->confirmed_screen_input << 12))) { error = "Explicit fallback must match the chosen input and cannot override fresh native PA"; return false; }
    }
    candidate = previous; tasks.clear(); IdSource ids{random, {}, 0, false}; Builder builder{candidate, evidence, ids, tasks, {}, {}};
    if (!builder.activity(desired) || ids.failed) { error = "Secure activity identifier allocation failed"; return false; }
    auto& created = candidate.activities.back();
    if (created.display) {
        std::optional<model::ZoneId> zone;
        for (const auto& a : previous.activities) if (a.display == created.display) { zone = a.zone_id; break; }
        if (!zone && previous.activities.empty()) zone = candidate.primary_zone_id;
        if (!zone) {
            auto new_zone = ids.make<model::ZoneId>(); auto* screen = component(candidate, *created.display);
            candidate.zones.push_back({new_zone, screen ? screen->record.label : "Screen", screen ? screen->record.label : "Screen", false, {}}); zone = new_zone;
        }
        if (created.zone_id != *zone) {
            for (auto& z : candidate.zones) if (z.zone_id == created.zone_id) z.activity_order.erase(std::remove(z.activity_order.begin(), z.activity_order.end(), created.activity_id), z.activity_order.end());
            created.zone_id = *zone; for (auto& z : candidate.zones) if (z.zone_id == *zone) z.activity_order.push_back(created.activity_id);
        }
    }
    if (!validate_configuration_v2(candidate, error)) return false;
    return true;
}

ConfigResult migrate_legacy(ConfigurationStore& store, ConfigBackend& backend, const std::string& original, const LegacyConfiguration& legacy,
    model::ConsortiumId consortium, model::OctopusId octopus, const std::vector<LegacyEvidence>& evidence,
    const std::function<bool(uint8_t*, size_t)>& random, ConfigurationV2& out, std::vector<std::string>& tasks, std::string& error) {
    ConfigurationV2 active; const auto existing = store.recover(active, error);
    if (existing == ConfigResult::ok) { out = active; return ConfigResult::ok; }
    if (existing != ConfigResult::missing) return existing;
    if (!consortium || !octopus || original.size() > configuration_compatibility_limit || original.find('\0') != std::string::npos || legacy.activities.size() > 16 || legacy.quirks.size() > 32) { error = "Invalid legacy migration input"; return ConfigResult::invalid; }
    const auto digest = sha256_hex(original);
    std::string backup; auto backup_status = backend.read("legacy_backup", backup);
    if (backup_status == ReadResult::error || (backup_status == ReadResult::found && backup != original)) { error = "Legacy backup conflict or storage error"; return ConfigResult::recovery_required; }
    if (backup_status == ReadResult::missing && !write_checked(backend, "legacy_backup", original)) return ConfigResult::storage_error;
    std::string map; auto mapping = backend.read("migration_ids", map); const std::string prefix = "2:" + digest + ":" + consortium.str() + ":" + octopus.str() + ":";
    constexpr size_t count = 512;
    if (mapping == ReadResult::error) return ConfigResult::storage_error;
    if (mapping == ReadResult::missing) {
        // Journal the CSPRNG seed rather than 512 expanded random IDs. This is
        // an ID-generation stream, never an identity derived from a MAC/name
        // or an array index alone. The seed is nonsecret but has 256 random bits.
        const auto first = model::MutationId::generate(random), second = model::MutationId::generate(random);
        if (!first || !second) { error = "Secure migration ID generation failed"; return ConfigResult::invalid; }
        map = prefix + first->str() + second->str();
        if (!write_checked(backend, "migration_ids", map)) return ConfigResult::storage_error;
    }
    if (map.size() != prefix.size() + 64 || map.compare(0, prefix.size(), prefix)) { error = "Migration mapping is corrupt or belongs to another original/system"; return ConfigResult::recovery_required; }
    IdSource ids{random, {}, 0, false}; std::set<std::string> unique_ids;
    for (size_t n = 0; n < count; ++n) {
        auto text = sha256_hex("Amity migration ID stream v1:" + map.substr(prefix.size()) + ":" + std::to_string(n)).substr(0, 32);
        if (!model::MutationId::parse(text) || !unique_ids.insert(text).second) return ConfigResult::recovery_required;
        ids.pool.push_back(text);
    }
    ConfigurationV2 candidate; std::string retained; const auto retained_status = backend.read("migration_map", retained);
    tasks.clear();
    if (retained_status == ReadResult::error) return ConfigResult::storage_error;
    if (retained_status == ReadResult::found) {
        if (!parse_configuration_v2(retained, candidate, error) || candidate.consortium_id != consortium) return ConfigResult::recovery_required;
        for (const auto& a : candidate.activities) if (!a.enabled) tasks.push_back("Complete equipment/connection assignment for " + a.label);
    } else {
        candidate.consortium_id = consortium; candidate.primary_zone_id = ids.make<model::ZoneId>(); candidate.controller_octopus_id = octopus;
        candidate.octopuses.push_back({octopus, "Amity 1", "Amity 1", false, 1}); candidate.zones.push_back({candidate.primary_zone_id, "Main screen", "Main screen", false, {}});
        Builder builder{candidate, evidence, ids, tasks, {}, {}}; builder.legacy_semantics = true;
        for (size_t n = 0; n < legacy.activities.size(); ++n) {
            ids.next = 1 + n * 31; // Stable activity/component ID segment, independent of earlier evidence.
            if (!builder.activity(legacy.activities[n]) || ids.next > 1 + (n + 1) * 31) { error = "Migration identity pool exhausted"; return ConfigResult::capacity; }
        }
        for (const auto& alias : legacy.aliases) if (alias.slot < candidate.external_slots.size()) { candidate.external_slots[alias.slot].alias = alias.alias; candidate.external_slots[alias.slot].hidden = alias.hidden; }
        if (!legacy.quirks.empty()) {
            tasks.push_back("Confirm and scope legacy vendor power recipes before enabling them; raw unqualified recipes were retained in the original backup");
            for (auto& a : candidate.activities) a.enabled = false;
        }
        if (!validate_configuration_v2(candidate, error)) return ConfigResult::invalid;
        retained = serialize_configuration_v2(candidate);
        if (!write_checked(backend, "migration_map", retained)) return ConfigResult::storage_error;
    }
    bool controller_changed = false;
    if (!bind_original_controller(candidate, octopus, controller_changed, error)) return ConfigResult::recovery_required;
    if (controller_changed && !write_checked(backend, "migration_map", serialize_configuration_v2(candidate))) return ConfigResult::storage_error;
    if (serialize_configuration_v2(candidate).size() > configuration_payload_limit) { error = "Migrated candidate exceeds this board16KiB admission profile; original configuration is preserved"; return ConfigResult::capacity; }
    auto staged = store.stage(candidate, {}, error); if (staged != ConfigResult::ok) return staged;
    // Migration activation remains a staged decision until the live owner queue
    // reserves capacity and acknowledges exactly this digest. No CEC is emitted.
    out = *store.staged(); return ConfigResult::ok;
}
} // namespace config_store
