#pragma once
#include "amity/system/model.hpp"
#include "esp_err.h"
#include <functional>
#include <optional>
#include <utility>
#include <vector>
namespace amity {
struct AdapterRuntimeState {uint8_t tentacle_id=0;bool configured=false;std::optional<uint16_t> observed_front_physical,effective_front_physical;};
struct AdapterAddressChange {uint8_t tentacle_id=0;uint16_t default_physical=0x1000;};
class SystemControl {
public:
    system::BootId boot;uint8_t capacity=4;
    std::vector<AdapterRuntimeState> rows;
    std::function<esp_err_t(system::BootId,std::vector<AdapterAddressChange>,uint64_t,std::function<esp_err_t()>)> gate;
    system::BootId boot_identity() const{return boot;}
    uint8_t supported_tentacles() const{return capacity;}
    std::vector<AdapterRuntimeState> adapter_runtime_snapshot() const{return rows;}
    esp_err_t commit_adapter_settings(system::BootId b,std::vector<AdapterAddressChange> p,uint64_t deadline,std::function<esp_err_t()> persist){return gate(b,std::move(p),deadline,std::move(persist));}
};
}
