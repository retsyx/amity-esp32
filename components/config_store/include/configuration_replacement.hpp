#pragma once
#include "system_configuration.hpp"
namespace config_store {
// Saved equipment locations only. Replacement does not carry membership,
// ecosystem identity, live addressing or execution authority.
struct ReplacementPort {
    model::CecPortRef previous, replacement;
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
bool propose_octopus_replacement(const Configuration&, const ReplacementApproval&,
    Configuration& candidate,
    std::vector<model::ActivityId>& affected, std::string& error);
// Manual recovery onto a freshly claimed original controller. Only portable
// logical configuration and equipment selectors are retained. Availability is
// resolved by the owning device; membership/security data is never imported.
bool restore_main_configuration(const Configuration& backup, const Configuration& current,
    bool previous_controller_isolated, Configuration& candidate,
    std::vector<std::string>& assignments, std::string& error);

}
