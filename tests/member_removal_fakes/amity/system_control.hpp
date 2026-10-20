#pragma once
#include "amity/system/model.hpp"
#include "esp_err.h"
#include <functional>
#include <optional>
namespace amity {
struct RemovalConfiguration {std::optional<system::OctopusId> controller_octopus_id;};
class SystemControl {
public:
    system::OctopusId local;std::optional<RemovalConfiguration> configuration;
    std::function<esp_err_t()> retire;
    system::OctopusId octopus_id() const{return local;}
    std::optional<RemovalConfiguration> active_configuration() const{return configuration;}
    esp_err_t retire_membership(){return retire();}
};
}
