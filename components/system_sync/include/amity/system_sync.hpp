#pragma once
#include "system_configuration.hpp"
#include "amity/system/policy.hpp"
#include "esp_err.h"
namespace amity {
class SystemControl;
struct ConfigurationActivationStatus {
    system::MessageId request_id;
    uint64_t config_revision = 0, presentation_revision = 0;
    std::string phase = "idle", error;
    std::vector<system::OctopusId> pending_owners;
};
esp_err_t start_configuration_synchronization(SystemControl&);
esp_err_t activate_distributed_configuration(config_store::ConfigurationV2, config_store::RevisionPair expected, system::MessageId request);
ConfigurationActivationStatus configuration_activation_status();
std::vector<system::CecObservation> discovered_equipment();
// Bounded identity-only refresh on local and current authenticated owners.
esp_err_t request_equipment_scan();
void set_owner_maintenance(system::OctopusId, bool);
namespace synchronization {
config_store::ConfigurationV2 owner_slice(const config_store::ConfigurationV2&, system::OctopusId);
std::string owner_state_json(const config_store::ConfigurationV2&, system::OctopusId, system::BootId, uint64_t epoch,
                             const std::vector<system::PortPolicySnapshot>&, const std::vector<system::CecObservation>& observations = {}, uint64_t sender_now_ms = 0);
bool parse_owner_state(std::string_view, const config_store::ConfigurationV2&, system::OctopusId, system::BootId, uint64_t epoch,
                       std::vector<system::PortPolicySnapshot>&, std::string& error, std::vector<system::CecObservation>* observations = nullptr, uint64_t receiver_now_ms = 0);
}
}
