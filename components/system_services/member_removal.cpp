// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#include "member_removal.hpp"
#include "amity/system_control.hpp"
#include "config_store.hpp"
#include "device_settings.hpp"
#include "system_services.hpp"
#include <mutex>
namespace system_services {
namespace {
std::mutex reset_gate;
enum class Reset { idle, armed, restarting };
Reset reset=Reset::idle;
}
esp_err_t prepare_removed_device_reset(amity::SystemControl& owner,
    amity::system::OctopusId coordinator){
    std::lock_guard<std::mutex> lock(reset_gate);
    const auto configuration=owner.active_configuration();
    if(!coordinator||coordinator==owner.octopus_id()||
       (configuration&&(!configuration->controller_octopus_id||*configuration->controller_octopus_id!=coordinator)))
        return ESP_ERR_INVALID_STATE;
    if(reset!=Reset::idle)return ESP_OK;
    esp_err_t saved=ESP_FAIL;
    const bool completed=config_store::with_internal_flash_stack([&]{
        saved=device_settings::reset_pending()?ESP_OK:device_settings::schedule_factory_reset();
    });
    if(!completed)return ESP_FAIL;
    if(saved==ESP_OK)reset=Reset::armed;
    return saved;
}
void removed_device_reset_response_attempted(){
    bool restart=false;
    {std::lock_guard<std::mutex> lock(reset_gate);if(reset==Reset::armed){reset=Reset::restarting;restart=true;}}
    if(restart)reboot_later();
}
}
