#include "amity/system_control.hpp"
#ifdef ESP_PLATFORM
#include "esp_random.h"
#else
#include <random>
#endif

namespace amity {
bool SystemControl::compile_v2(const config_store::ConfigurationV2& config, system::LocalConfiguration& slice, std::string& error) const {
    if(!config.legacy_remote_configuration.empty()){error="Upgrade the saved configuration before activating it";return false;}
    if (!config_store::validate_configuration_v2(config, error) || !config.controller_octopus_id || config.consortium_id != consortium_) return false;
    slice.topology = config_store::configured_topology(config); slice.primary_zone_id = config.primary_zone_id;
    slice.presentation_revision = config.presentation_revision; slice.executable_hash = config_store::executable_policy_hash(config);
    for (const auto& a : config.activities) if (a.enabled) slice.activities.push_back({a.activity_id, a.zone_id, a.label, a.source, a.display, a.audio});
    for (const auto& slot : config.external_slots) slice.external_slots.push_back({slot.slot, slot.activity_id, slot.hidden, slot.tombstone, slot.alias});
    for (const auto& domain : config.domains) {
        for (const auto& port : domain.ports) if (port.port.octopus_id == octopus_) {
            if (port.port.tentacle_id >= amity::max_tentacles || port.port.consortium_id != consortium_) {
                error = "local attachment is outside the supported HDMI adapter indices"; return false;
            }
            if (std::any_of(slice.port_templates.begin(), slice.port_templates.end(), [&](const auto& p) { return p.port == port.port; })) {
                error = "local attachment is assigned more than one domain"; return false;
            }
            if (!domain.confirmed && (port.role == system::PortRole::tv_controller || port.role == system::PortRole::playback_proxy)) { error = "active CEC domain is not confirmed"; return false; }
            system::PortPolicySnapshot policy;
            policy.port = port.port; policy.domain_id = domain.domain_id; policy.domain_revision = domain.domain_revision;
            policy.role = connector_enabled(port.port.tentacle_id) ? port.role : system::PortRole::disabled;
            policy.physical_address = port.physical_address; policy.components = slice.topology.components;
            for (const auto& binding : config.binding_policies) if (binding.port == port.port && binding.domain_id == domain.domain_id) {
                system::ComponentBinding record;
                record.binding_id = binding.binding_id; record.component_id = binding.component_id; record.capability = binding.capability;
                record.kind = binding.kind; record.device_name = binding.device_name; record.endpoint.port = binding.port;
                record.owner_octopus_id = octopus_; record.domain_id = domain.domain_id; record.domain_revision = domain.domain_revision;
                record.binding_revision = 1; record.role = policy.role; record.proxy_identity_resource_id = binding.proxy_identity_resource_id;
                record.physical_address_evidence_ref = system::EvidenceId::generate([](uint8_t* data, size_t size) {
#ifdef ESP_PLATFORM
                    esp_fill_random(data, size);
#else
                    std::random_device random; for (size_t i = 0; i < size; ++i) data[i] = static_cast<uint8_t>(random());
#endif
                    return true;
                }).value_or(system::EvidenceId{});
                slice.topology.bindings.push_back(record);
                policy.bindings.push_back({record, binding.discrete_power_on, binding.discrete_power_off, binding.image_view_on, binding.exact_path_selection});
            }
            if (policy.bindings.size() > 64) { error = "local port exceeds bounded binding capacity"; return false; }
            slice.port_templates.push_back(std::move(policy));
        }
    }
    if (slice.port_templates.size() > amity::max_cec_ports || slice.topology.bindings.size() > 128) { error = "local runtime capacity exceeded"; return false; }
    for (size_t i = 0; i < slice.port_templates.size(); ++i) for (size_t j = 0; j < i; ++j)
        if (slice.port_templates[i].domain_id == slice.port_templates[j].domain_id && slice.port_templates[i].role == system::PortRole::tv_controller && slice.port_templates[j].role == system::PortRole::tv_controller) {
            error = "electrical domain cannot allocate two local TV-controller identities"; return false;
        }
    for (uint8_t id = 0; id < supported_tentacles(); ++id) if (connector_enabled(id))
    for (auto side : {system::Side::front, system::Side::back}) if (std::none_of(slice.port_templates.begin(), slice.port_templates.end(), [&](const auto& p) { return p.port.tentacle_id == id && p.port.side == side; })) {
        // Unknown attachments start in a private runtime quarantine domain.
        // This grants observation only; configuration must explicitly confirm
        // a domain before installing an active control role.
        system::PortPolicySnapshot policy;
        policy.port = {consortium_, octopus_, id, side}; policy.role = system::PortRole::monitor; policy.domain_revision = 1;
        policy.domain_id = system::DomainId::generate([](uint8_t* data, size_t size) {
#ifdef ESP_PLATFORM
            esp_fill_random(data, size);
#else
            std::random_device random; for (size_t i = 0; i < size; ++i) data[i] = static_cast<uint8_t>(random());
#endif
            return true;
        }).value_or(system::DomainId{});
        slice.port_templates.push_back(std::move(policy));
    }
    return true;
}
esp_err_t SystemControl::configure_v2(config_store::ConfigurationV2 config, std::function<esp_err_t()> commit) {
    if (!local_coordinator_.load() || maintenance_.load()) return ESP_ERR_INVALID_STATE;
    if (!config.controller_octopus_id || *config.controller_octopus_id != octopus_) return ESP_ERR_INVALID_ARG;
    system::LocalConfiguration slice; std::string error;
    if (!compile_v2(config, slice, error)) return ESP_ERR_INVALID_ARG;
    auto stored = std::make_shared<const config_store::ConfigurationV2>(std::move(config));
    const auto result = local_.configure_v2(std::move(slice), std::move(commit));
    if (result == ESP_OK) std::atomic_store(&active_config_, std::move(stored));
    return result;
}
esp_err_t SystemControl::configure_passive_metadata(const config_store::ConfigurationV2& config) {
    std::string error;
    if (local_coordinator_.load() || maintenance_.load()) return ESP_ERR_INVALID_STATE;
    const auto previous = std::atomic_load(&active_config_);
    if (!config.controller_octopus_id || (previous && previous->controller_octopus_id && previous->controller_octopus_id != config.controller_octopus_id) || config.consortium_id != consortium_ || !config_store::validate_configuration_v2(config, error)) return ESP_ERR_INVALID_ARG;
    std::atomic_store(&active_config_, std::make_shared<const config_store::ConfigurationV2>(config));
    return ESP_OK;
}
esp_err_t SystemControl::configure_member_slice(config_store::ConfigurationV2 config, system::OctopusId coordinator, uint64_t epoch) {
    if (!coordinator || coordinator == octopus_ || epoch != 1 || !config.controller_octopus_id || *config.controller_octopus_id != coordinator) return ESP_ERR_INVALID_ARG;
    const auto previous = std::atomic_load(&active_config_);
    if (previous && previous->controller_octopus_id && previous->controller_octopus_id != config.controller_octopus_id) return ESP_ERR_INVALID_ARG;
    system::LocalConfiguration slice; std::string error;
    if (!compile_v2(config, slice, error)) return ESP_ERR_INVALID_ARG;
    slice.member = true; slice.coordinator_id = coordinator; slice.coordinator_epoch = epoch;
    auto stored = std::make_shared<const config_store::ConfigurationV2>(std::move(config));
    const auto result = local_.configure_member_v2(std::move(slice), [stored, owner = octopus_, coordinator, epoch] {
        std::string reason; return config_store::persist_owner_configuration(*stored, owner, coordinator, epoch, reason);
    });
    if (result == ESP_OK) std::atomic_store(&active_config_, std::move(stored));
    return result;
}
system::AdmissionResult SystemControl::prepare_member_configuration(config_store::ConfigurationV2 config, system::OctopusId coordinator, uint64_t epoch, std::string hash) {
    if (!coordinator || coordinator == octopus_ || epoch != 1 || !config.controller_octopus_id || *config.controller_octopus_id != coordinator || hash.size() != 64 ||
        std::any_of(hash.begin(), hash.end(), [](char c) { return (c < '0' || c > '9') && (c < 'a' || c > 'f'); }))
        return {system::AdmissionCode::invalid, "invalid member configuration correlation"};
    const auto previous = std::atomic_load(&active_config_);
    if (previous && previous->controller_octopus_id && previous->controller_octopus_id != config.controller_octopus_id)
        return {system::AdmissionCode::unauthorized, "the original controller cannot be changed"};
    system::LocalConfiguration slice; std::string error;
    if (!compile_v2(config, slice, error)) return {system::AdmissionCode::invalid, error};
    slice.member = true; slice.coordinator_id = coordinator; slice.coordinator_epoch = epoch;
    auto prepared = std::make_shared<const PreparedMember>(PreparedMember{hash, std::move(config)});
    const auto result = local_.prepare_member_v2(std::move(slice), std::move(hash), [prepared, owner = octopus_, coordinator, epoch] {
        std::string reason; return config_store::persist_owner_configuration(prepared->configuration, owner, coordinator, epoch, reason);
    });
    if (result) std::atomic_store(&prepared_member_, std::move(prepared));
    return result;
}
esp_err_t SystemControl::activate_member_configuration(const std::string& hash) {
    auto prepared = std::atomic_load(&prepared_member_);
    if (!prepared || prepared->payload_hash != hash) return ESP_ERR_INVALID_STATE;
    const auto result = local_.activate_member_v2(hash);
    if (result == ESP_OK) {
        std::atomic_store(&active_config_, std::make_shared<const config_store::ConfigurationV2>(prepared->configuration));
        std::atomic_store(&prepared_member_, std::shared_ptr<const PreparedMember>{});
    }
    return result;
}
void SystemControl::abort_member_configuration(const std::string& hash) {
    local_.abort_member_v2(hash);
    auto prepared = std::atomic_load(&prepared_member_);
    if (prepared && prepared->payload_hash == hash) std::atomic_store(&prepared_member_, std::shared_ptr<const PreparedMember>{});
}
system::AdmissionResult SystemControl::update_remote_owner(system::OctopusId owner, std::vector<system::PortPolicySnapshot> policies, bool ready, uint32_t ttl) {
    auto config = std::atomic_load(&active_config_);
    if (!config || !owner || owner == octopus_) return {system::AdmissionCode::unauthorized, "owner is not in an active Amity system configuration"};
    for (const auto& policy : policies) {
        const auto domain = std::find_if(config->domains.begin(), config->domains.end(), [&](const auto& d) { return d.domain_id == policy.domain_id && d.domain_revision == policy.domain_revision && d.owner_octopus_id == owner; });
        const bool passive = (policy.role == system::PortRole::monitor || policy.role == system::PortRole::disabled) && !policy.owned_logical_address && policy.bindings.empty();
        const bool unavailable = (policy.role == system::PortRole::disabled || policy.role == system::PortRole::monitor) && !policy.owned_logical_address &&
            std::none_of(policy.bindings.begin(),policy.bindings.end(),[](const auto& p){return p.binding.status==system::BindingStatus::verified;});
        if(domain==config->domains.end()&&passive){
            const auto board=std::find_if(config->octopuses.begin(),config->octopuses.end(),[&](const auto& b){return b.octopus_id==owner;});
            if(board!=config->octopuses.end()&&policy.port.consortium_id==consortium_&&policy.port.octopus_id==owner&&policy.port.tentacle_id<board->supported_tentacles&&policy.config_revision==config->config_revision)continue;
        }
        if (domain == config->domains.end() || (!domain->confirmed && !passive && !unavailable) || policy.config_revision != config->config_revision ||
            std::none_of(domain->ports.begin(), domain->ports.end(), [&](const auto& p) { return p.port == policy.port && (p.role == policy.role || unavailable); }))
            return {system::AdmissionCode::unauthorized, "remote policy is outside its committed attachment domain"};
        for (const auto& permission : policy.bindings) {
            const auto& binding = permission.binding;
            const auto expected = std::find_if(config->binding_policies.begin(), config->binding_policies.end(), [&](const auto& p) { return p.binding_id == binding.binding_id; });
            if (expected == config->binding_policies.end() || expected->component_id != binding.component_id || expected->domain_id != binding.domain_id ||
                expected->port != binding.endpoint.port || expected->capability != binding.capability || expected->kind != binding.kind || expected->device_name != binding.device_name ||
                expected->proxy_identity_resource_id != binding.proxy_identity_resource_id ||
                (binding.status==system::BindingStatus::verified?(expected->discrete_power_on!=permission.discrete_power_on||expected->discrete_power_off!=permission.discrete_power_off||expected->image_view_on!=permission.image_view_on||expected->exact_path_selection!=permission.exact_path_selection):
                 ((permission.discrete_power_on&&!expected->discrete_power_on)||(permission.discrete_power_off&&!expected->discrete_power_off)||(permission.image_view_on&&!expected->image_view_on)||(permission.exact_path_selection&&!expected->exact_path_selection))))
                return {system::AdmissionCode::unauthorized, "remote binding changes committed component or semantic permissions"};
        }
    }
    return local_.update_remote_owner(owner, std::move(policies), ready, ttl);
}
std::optional<config_store::ConfigurationV2> SystemControl::active_configuration() const {
    auto config = std::atomic_load(&active_config_); if (config) return *config; return {};
}
std::optional<config_store::ConfiguredActivity> SystemControl::external_activity(unsigned slot) const {
    auto config = std::atomic_load(&active_config_); if (!config) return {};
    const auto* activity = config_store::activity_for_slot(*config, slot); if (!activity || !activity->enabled || activity->zone_id != config->primary_zone_id) return {}; return *activity;
}
std::optional<config_store::ExternalSlot> SystemControl::external_slot(unsigned index) const {
    auto config = std::atomic_load(&active_config_); if (!config) return {};
    for (const auto& slot : config->external_slots) if (slot.slot == index && (slot.tombstone || std::any_of(config->activities.begin(), config->activities.end(), [&](const auto& activity) { return activity.activity_id == slot.activity_id && activity.zone_id == config->primary_zone_id; }))) return slot;
    return {};
}
}
