#pragma once
#include "system_configuration.hpp"
#include "amity/system/execution.hpp"
namespace amity {
class SystemControl;
struct ConfigurationActivationStatus {
    system::MessageId request_id;
    uint64_t config_revision=0,presentation_revision=0;
    std::string phase="idle",error;
};
esp_err_t start_configuration_synchronization(SystemControl&);
// Explicit membership mutations may run only after staged readback and owner
// command reservation, immediately before the configuration marker commits.
// A later storage failure is reported; it cannot roll back a roster revocation.
esp_err_t activate_distributed_configuration(config_store::Configuration,config_store::RevisionPair,system::MessageId,
    std::function<esp_err_t()> before_commit = {});
ConfigurationActivationStatus configuration_activation_status();
std::vector<system::CecObservation> discovered_equipment();
esp_err_t request_equipment_scan();
void set_owner_maintenance(system::OctopusId,bool);
void forget_owner(system::OctopusId);
}
