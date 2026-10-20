#pragma once
#include "system_configuration.hpp"
namespace config_store {
// Public topology only. No operational key, ecosystem identity, bond, saved LA,
// live plan or execution authority belongs to a replacement proposal.
struct ReplacementPort {
    model::CecPortRef previous, replacement;
    bool electrical_isolation_confirmed = false;
    std::optional<uint16_t> confirmed_attachment_pa;
};
struct ReplacementEquipment {
    model::ComponentId component;
    model::QualifiedDeviceName previous, replacement;
    bool same_equipment_confirmed = false;
};
struct ReplacementApproval {
    model::OctopusId previous, replacement;
    bool previous_revoked = false, previous_isolated = false, replacement_enrolled = false;
    std::vector<ReplacementPort> ports;
    std::vector<ReplacementEquipment> equipment;
};
bool propose_octopus_replacement(const ConfigurationV2&, const ReplacementApproval&,
    const std::vector<model::CecObservation>& fresh_native,
    const std::function<bool(uint8_t*,size_t)>& random, ConfigurationV2& candidate,
    std::vector<model::ActivityId>& affected, std::string& error);
// Manual recovery onto a freshly claimed original controller. Only portable
// logical configuration is retained; routes, PA evidence and remote assignment
// are quarantined until deliberate local repair. No membership/security import.
bool restore_main_configuration(const ConfigurationV2& backup, const ConfigurationV2& current,
    bool previous_controller_isolated, ConfigurationV2& candidate,
    std::vector<std::string>& assignments, std::string& error);

}
