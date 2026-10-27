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
bool parse_submission(const std::string&,const config_store::ConfigurationV2&,Submission&,std::string& error);
bool parse_restore_request(const cJSON*,const config_store::ConfigurationV2&,m::BootId current_boot,
    config_store::ConfigurationV2& backup,std::string& error);
std::vector<Choice> choices(const config_store::ConfigurationV2&, const std::vector<m::CecObservation>&,
    const OwnerHostnames& = {});
std::vector<m::ActivityId> projected_activities(const config_store::ConfigurationV2&);
std::string component_value(std::optional<m::ComponentId>);
bool edit(const config_store::ConfigurationV2&, const std::vector<Row>&, const std::vector<Choice>&,
          const std::function<bool(uint8_t*, size_t)>&, config_store::ConfigurationV2&,
          std::vector<std::string>& unresolved, std::string& error);
}
