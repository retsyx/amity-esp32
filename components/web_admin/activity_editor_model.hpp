#pragma once
#include "system_configuration.hpp"
#include <map>
struct cJSON;

namespace web_admin::activity_editor {
namespace m = amity::system;
struct Choice {
    std::string value, title, location;
    std::vector<std::string> roles;
    std::optional<m::ComponentId> component_id;
    std::optional<m::QualifiedDeviceName> native_name;
    std::optional<m::CecObservation> observation;
};
struct Row {
    std::optional<m::ActivityId> activity_id;
    std::string name, display, source, audio, switch_device;
    uint8_t switch_input = 0;
};
struct Submission { m::MessageId request_id; config_store::RevisionPair expected; std::vector<Row> rows; };
// Presentation only. Populate from the local device setting and authenticated
// retained peer names, never from a configured friendly label or DNS discovery.
using OwnerHostnames=std::map<m::OctopusId,std::string>;
using OwnerCapacities=std::map<m::OctopusId,uint8_t>;
struct ComponentOwner { m::OctopusId id; std::string name; uint8_t supported_tentacles = 0; };
struct ComponentSubmission {
    m::MessageId request_id;
    config_store::RevisionPair expected;
    m::QualifiedDeviceName name;
    std::string role;
};
std::vector<ComponentOwner> component_owners(const config_store::Configuration&,const OwnerHostnames&,
    const std::vector<m::OctopusId>& enrolled,const OwnerCapacities& = {});
bool parse_component_submission(const cJSON*,const config_store::Configuration&,ComponentSubmission&,std::string& error);
bool add_component(const config_store::Configuration&,const ComponentSubmission&,const std::vector<ComponentOwner>&,
    const std::function<bool(uint8_t*,size_t)>&,config_store::Configuration&,m::ComponentId&,std::string& error);
bool parse_submission(const std::string&,const config_store::Configuration&,Submission&,std::string& error);
bool parse_restore_request(const cJSON*,const config_store::Configuration&,m::BootId current_boot,
    config_store::Configuration& backup,std::string& error);
// Count enrolled members even while offline; retained configuration and names
// alone do not imply that another Amity is still added.
// Current compiled capacities override retained configuration capacity.
std::vector<Choice> choices(const config_store::Configuration&, const std::vector<m::CecObservation>&,
    const OwnerHostnames& = {}, bool has_added_amities = false, const OwnerCapacities& = {});
std::vector<m::ActivityId> projected_activities(const config_store::Configuration&);
std::string component_value(std::optional<m::ComponentId>);
bool edit(const config_store::Configuration&, const std::vector<Row>&, const std::vector<Choice>&,
          const std::function<bool(uint8_t*, size_t)>&, config_store::Configuration&,
          std::vector<std::string>& unresolved, std::string& error);
}
