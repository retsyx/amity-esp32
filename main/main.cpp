/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#include "board_pins.h"
#include "amity/system_control.hpp"
#include "amity_matter.h"
#include "homekit.hpp"
#include "atomic_controller.hpp"
#include "atomic_remote.h"
#include "siri_remote.hpp"
#include "volume_sequence.hpp"
#include "swipe_sequence.hpp"
#include <atomic>
#include <algorithm>
#include <mutex>
#include "ble_remote.h"
#include "config_store.hpp"
#include "system_identity.hpp"
#include "amity/consortium/runtime.hpp"
#include "amity/system_sync.hpp"
#include "radio_control.hpp"
#include "device_settings.hpp"
#include "adapter_settings.hpp"
#include "member_removal.hpp"
#include "system_services.hpp"
#include "firmware_update.hpp"
#include "group_firmware_update.hpp"
#include "web_admin.hpp"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "mdns.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <cstdio>
#include <cstring>
namespace {
amity::SystemControl hub;
enum class InputType { Atomic, SiriButtons, SiriTouch, Reset };
struct Input {
    InputType type=InputType::Atomic;
    uint16_t page=0,usage=0;
    bool down=false;
    uint8_t size=0,data[18]{};
    uint64_t timestamp=0;
    uint32_t activity_generation=0;
    int activity_target=-1;
};
std::atomic_bool input_lost{false};
std::atomic_bool original_interfaces_enabled{false};
std::mutex admin_mdns_mutex;
bool admin_mdns_registered=false;
std::string admin_mdns_hostname;
QueueHandle_t input_queue;
void startup_memory(const char* phase){
    ESP_LOGI("startup_memory","%s: internal=%u psram=%u",phase,
        unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)),
        unsigned(heap_caps_get_free_size(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)));
}
esp_err_t refresh_admin_mdns_for_host(const char* hostname) {
    // Matter owns the responder's global hostname. Keep the admin hostname as a
    // delegated alias so a Matter republish cannot rename the HTTPS endpoint.
    mdns_ip_addr_t addresses[1+CONFIG_LWIP_IPV6_NUM_ADDRESSES]{};
    size_t count=0;
    auto *netif=esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if(netif&&esp_netif_is_netif_up(netif)) {
        esp_netif_ip_info_t ipv4{};
        if(esp_netif_get_ip_info(netif,&ipv4)==ESP_OK&&ipv4.ip.addr) {
            addresses[count].addr.type=ESP_IPADDR_TYPE_V4;
            addresses[count++].addr.u_addr.ip4=ipv4.ip;
        }
        esp_ip6_addr_t ipv6[CONFIG_LWIP_IPV6_NUM_ADDRESSES]{};
        const int num=esp_netif_get_all_preferred_ip6(netif,ipv6);
        for(int i=0;i<num&&count<sizeof(addresses)/sizeof(addresses[0]);++i) {
            addresses[count].addr.type=ESP_IPADDR_TYPE_V6;
            addresses[count++].addr.u_addr.ip6=ipv6[i];
        }
    }
    for(size_t i=1;i<count;++i)addresses[i-1].next=&addresses[i];
    // The mDNS API deep-copies this list before returning.
    return mdns_delegate_hostname_set_address(hostname,count?addresses:nullptr);
}
void refresh_admin_mdns() {
    std::lock_guard<std::mutex> guard(admin_mdns_mutex);
    if(!original_interfaces_enabled.load()||admin_mdns_hostname.empty())return;
    const auto err=refresh_admin_mdns_for_host(admin_mdns_hostname.c_str());
    if(err!=ESP_OK)ESP_LOGW("amity","Administration mDNS address update: %s",esp_err_to_name(err));
}
esp_err_t rename_hostname(const std::string& previous,const std::string& hostname) {
    auto* netif=esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if(!netif)return ESP_ERR_INVALID_STATE;
    std::lock_guard<std::mutex> guard(admin_mdns_mutex);
    auto err=esp_netif_set_hostname(netif,hostname.c_str());
    if(err!=ESP_OK)return err;
    if(!original_interfaces_enabled.load())return ESP_OK;
    bool added=false,home_changed=false;
    err=mdns_delegate_hostname_add(hostname.c_str(),nullptr);
    if(err==ESP_OK){added=true;err=refresh_admin_mdns_for_host(hostname.c_str());}
    if(err==ESP_OK){
        err=ESP_ERR_INVALID_ARG;
        for(unsigned attempt=0;attempt<100&&err==ESP_ERR_INVALID_ARG;++attempt){
            err=mdns_service_add_for_host("Amity Home Theater","_https","_tcp",hostname.c_str(),443,nullptr,0);
            if(err==ESP_ERR_INVALID_ARG)vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
    if(err==ESP_OK){err=amity_homekit::rename_hostname(hostname.c_str());home_changed=err==ESP_OK;}
    if(err==ESP_OK)err=mdns_delegate_hostname_remove(previous.c_str());
    if(err==ESP_OK){admin_mdns_hostname=hostname;admin_mdns_registered=true;return ESP_OK;}
    if(home_changed&&amity_homekit::rename_hostname(previous.c_str())!=ESP_OK)
        ESP_LOGE("amity","Could not restore HomeKit hostname after rename failure");
    if(added&&mdns_delegate_hostname_remove(hostname.c_str())!=ESP_OK)
        ESP_LOGE("amity","Could not remove incomplete administration hostname");
    if(esp_netif_set_hostname(netif,previous.c_str())!=ESP_OK)
        ESP_LOGE("amity","Could not restore network hostname after rename failure");
    return err;
}
void remote_key(uint16_t page,uint16_t usage,bool down,void *) {
    if(!original_interfaces_enabled.load())return;
    Input input{InputType::Atomic,page,usage,down};
    input.timestamp=esp_timer_get_time();
    input.activity_generation=hub.remote_activity_generation();
    input.activity_target=hub.remote_activity_target();
    if(xQueueSend(input_queue,&input,0)!=pdTRUE) input_lost.store(true);
}
void remote_siri(ble_remote_siri_event_t event,const uint8_t *data,size_t size,void *) {
    if(!original_interfaces_enabled.load())return;
    Input input{};
    if(!data||size>sizeof(input.data)) {input_lost.store(true);return;}
    if(event==BLE_REMOTE_SIRI_BUTTONS)input.type=InputType::SiriButtons;
    else if(event==BLE_REMOTE_SIRI_TOUCH)input.type=InputType::SiriTouch;
    else return;
    input.timestamp=esp_timer_get_time();
    input.activity_generation=hub.remote_activity_generation();
    input.activity_target=hub.remote_activity_target();
    input.size=static_cast<uint8_t>(size);
    std::memcpy(input.data,data,size);
    if(xQueueSend(input_queue,&input,0)!=pdTRUE)input_lost.store(true);
}
void remote_disconnect(void *) {
    Input reset{};reset.type=InputType::Reset;
    if(xQueueSend(input_queue,&reset,0)!=pdTRUE)input_lost.store(true);
}
esp_err_t open_boot_matter_setup() {
    if(!system_services::firmware_ready())return ESP_ERR_INVALID_STATE;
    const auto group=amity::consortium::runtime_status();
    // Current Matter fabrics determine whether setup is needed. A retained
    // original keeps its role; an already enrolled headless member never opens
    // Matter. Neither a button nor saved activity configuration gates boot setup.
    if(!group.initialized||group.member||
       !((group.coordinator&&group.authority_ready)||amity::consortium::runtime_original_setup_eligible()))return ESP_ERR_INVALID_STATE;
    amity_matter_status_t matter{};
    const auto read=amity_matter_get_status(&matter);
    if(read!=ESP_OK)return read;
    if(matter.fabric_count)return ESP_ERR_INVALID_STATE;
    return amity_matter_open_commissioning(300);
}
void start_boot_matter_setup() {
    esp_err_t result=ESP_ERR_INVALID_STATE;
    // Shared-radio cancellation can finish asynchronously. Retry only this
    // startup request, rechecking role and fabrics each time; expiry does not
    // create a periodic commissioning loop.
    for(unsigned attempt=0;attempt<50;++attempt){
        result=open_boot_matter_setup();
        if(result==ESP_OK){ESP_LOGI("amity","Matter setup opened automatically; no Matter pairing is saved");return;}
        if(result==ESP_ERR_INVALID_STATE)return;
        if(attempt+1<50)vTaskDelay(pdMS_TO_TICKS(100));
    }
    ESP_LOGE("amity","Could not open Matter setup automatically: %s",esp_err_to_name(result));
}
// Keep the full Matter fabric/status buffer and activity snapshot off the Add
// gesture's stack, including when the compiler optimizes these one-call helpers.
[[gnu::noinline]] void setup_matter(const amity::consortium::RuntimeStatus &group) {
    amity_matter_status_t matter{};const auto config=hub.active_configuration();
    const bool configured=config?(!config->activities.empty()||!config->components.empty()):!hub.snapshot().activities.empty();
    if(amity::consortium::runtime_original_setup_eligible()&&!group.member&&!group.coordinator&&!configured&&
       amity_matter_get_status(&matter)==ESP_OK&&matter.fabric_count==0)
        amity_matter_open_commissioning(300);
}
void setup_button(bool consortium_setup,void*) {
    if(!system_services::firmware_ready())return;
    const auto group=amity::consortium::runtime_status();
    if(!group.initialized)return;
    if(consortium_setup){
        uint64_t generation=0;
        const auto admitted=amity::radio::reserve_consortium(false,generation);
        if(admitted!=ESP_OK){
            ESP_LOGW("amity","Could not reserve radio for Add Amity: %s",esp_err_to_name(admitted));
            return;
        }
        const auto opened=amity::consortium::runtime_open_enrollment(generation);
        if(opened!=ESP_OK){
            amity::radio::release(generation);
            ESP_LOGW("amity","Could not open Add Amity window: %s",esp_err_to_name(opened));
        }else ESP_LOGI("amity","Add Amity window opened");
    }else setup_matter(group);
    ESP_LOGI("amity","Setup button handled; minimum stack free: %u bytes",
             unsigned(uxTaskGetStackHighWaterMark(nullptr)));
}
esp_err_t power(bool on,void *) {
    if(!on)return hub.request_activity(-1);
    const int target=hub.activity_target();
    const auto state=hub.snapshot();
    if(target>=0)return hub.request_remote_activity(static_cast<unsigned>(target));
    for(size_t index=0;index<state.activities.size();++index)
        if(!state.activities[index].name.empty())return hub.request_remote_activity(static_cast<unsigned>(index));
    return ESP_ERR_INVALID_STATE;
}
esp_err_t activity(uint8_t index,void *) {return hub.request_activity(index);}
esp_err_t key(uint8_t value,void *) {if(hub.snapshot().current_activity<0)return ESP_ERR_INVALID_STATE;return hub.press_key(value)?ESP_OK:ESP_ERR_NO_MEM;}
esp_err_t mute(bool muted,void *) {return key(muted?0x65:0x66,nullptr);}
QueueHandle_t volume_queue;
esp_err_t volume(int16_t steps,void *) {
    const auto state=hub.snapshot();
    if(state.current_activity<0)return ESP_ERR_INVALID_STATE;
    if(steps>100||steps< -100)return ESP_ERR_INVALID_ARG;
    amity::VolumeAdjustment v{steps,state.activity_generation};
    return xQueueSend(volume_queue,&v,0)==pdTRUE?ESP_OK:ESP_ERR_NO_MEM;
}
void firmware_update_task(void *) {
    for (;;) {
        system_services::updates::poll_group_update();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
void remote_battery_task(void *) {
    for (;;) {
        system_services::log_memory_if_due();
        system_services::updates::poll_peer_timeout();
        ble_remote_status_t remote{};
        ble_remote_get_status(&remote);
        const auto err = amity_matter_update_remote(remote.bonded,remote.connected,remote.battery_percent,remote.charging);
        if (err != ESP_OK) ESP_LOGW("amity", "Remote battery Matter update failed: %s", esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
void log_siri_buttons(const Input &input,uint16_t &previous) {
    if(!input.size||input.size>4) {
        ESP_LOGW("remote_input","Siri button report ignored: %u bytes",static_cast<unsigned>(input.size));
        return;
    }
    const uint16_t buttons=input.data[0]|(input.size>1?static_cast<uint16_t>(input.data[1])<<8:0);
    const uint16_t changed=buttons^previous;
    constexpr const char *names[]={"Home","Volume Up","Volume Down","Select","Power","Siri",
                                  "Back","Mute","Play/Pause","Up","Right","Down","Left"};
    for(size_t i=0;i<sizeof(names)/sizeof(names[0]);++i) {
        if(changed&(1u<<i))
            ESP_LOGI("remote_input","Siri: %s %s (buttons=0x%04x)",names[i],
                     buttons&(1u<<i)?"pressed":"released",static_cast<unsigned>(buttons));
    }
    if(changed&0xe000)
        ESP_LOGI("remote_input","Siri: unknown button bits changed (buttons=0x%04x)",static_cast<unsigned>(buttons));
    previous=buttons;
}
void log_atomic_key(const Input &input) {
    atomic_action_t action{};
    const char *name="Unknown";
    if(atomic_remote_translate(input.page,input.usage,&action)) {
        if(action.type==ATOMIC_ACTIVITY) {
            ESP_LOGI("remote_input","Atomic: F%u %s",static_cast<unsigned>(action.value+1),input.down?"pressed":"released");
            return;
        }
        switch(action.value) {
        case 0x00:name="Select";break;case 0x01:name="Up";break;case 0x02:name="Down";break;
        case 0x03:name="Left";break;case 0x04:name="Right";break;case 0x09:name="Home";break;
        case 0x0d:name="Back";break;case 0x40:name="Power";break;case 0x41:name="Volume Up";break;
        case 0x42:name="Volume Down";break;case 0x43:name="Mute";break;
        }
    }
    ESP_LOGI("remote_input","Atomic: %s %s (page=0x%04x usage=0x%04x)",name,input.down?"pressed":"released",
             static_cast<unsigned>(input.page),static_cast<unsigned>(input.usage));
}
void input_task(void *) {
    amity::SwipeSequence swipes;
    ble::RemoteInputController controller([](ble::RemoteCommand command,void *context) {
        auto &pending_swipes=*static_cast<amity::SwipeSequence *>(context);
        if(command.type==ble::RemoteCommandType::Release) {
            pending_swipes.cancel();
            return hub.release_remote_key(static_cast<uint8_t>(command.value));
        }
        // Check the reservation as well as the published generation. The hub
        // checks generation again on dequeue for a transition racing this call.
        if(command.stamp.generation!=hub.remote_activity_generation()||command.stamp.target!=hub.remote_activity_target())return false;
        switch(command.type) {
        case ble::RemoteCommandType::Press:
            pending_swipes.cancel();
            return hub.press_remote_key_for_activity(static_cast<uint8_t>(command.value),command.stamp.generation,true);
        case ble::RemoteCommandType::Activity:
            pending_swipes.cancel();return hub.request_remote_activity(command.value,command.stamp.generation)==ESP_OK;
        case ble::RemoteCommandType::PowerShort:
            pending_swipes.cancel();return (command.stamp.target<0?hub.request_remote_activity(0,command.stamp.generation):hub.stop_remote_zone(command.stamp.generation))==ESP_OK;
        case ble::RemoteCommandType::PowerLong:
            pending_swipes.cancel();return hub.repair_remote_activity(command.stamp.generation)==ESP_OK;
        case ble::RemoteCommandType::Swipe:
            pending_swipes.add(static_cast<uint8_t>(command.value),command.count,command.stamp.generation);return true;
        case ble::RemoteCommandType::Release:break;
        }
        return false;
    },&swipes);
    controller.set_wait_for_release(esp_timer_get_time());
    controller.activity_selected(hub.activity_selection(),esp_timer_get_time());
    ble::AtomicInputAdapter atomic(controller);
    ble::SiriInputAdapter siri(controller);
    amity::VolumeSequence volume_sequence;
    uint16_t siri_logged_buttons=0;
    ESP_LOGI("remote_input","Key press/release logging enabled");
    auto sync_activity=[&] {
        const bool setup=controller.activity_setup(hub.activity_setup_in_progress(),esp_timer_get_time());
        const bool selected=controller.activity_selected(hub.activity_selection(),esp_timer_get_time());
        if(controller.synchronize(hub.remote_activity_generation(),hub.remote_activity_target())||selected||setup) {
            swipes.cancel();siri.activity_changed();
        }
    };
    for(;;) {
        if(input_lost.exchange(false)) {
            ESP_LOGW("remote_input","Input lost; pending input cancelled");
            siri_logged_buttons=0;
            xQueueReset(input_queue);controller.cancel();siri.cancel();swipes.cancel();
        }
        sync_activity();
        Input event{};
        if(xQueueReceive(input_queue,&event,pdMS_TO_TICKS(20))==pdTRUE) {
            sync_activity();
            const ble::InputStamp stamp{event.timestamp,event.activity_generation,event.activity_target};
            switch(event.type) {
            case InputType::Atomic:
                log_atomic_key(event);atomic.input(event.page,event.usage,event.down,stamp);break;
            case InputType::SiriButtons:
                // Log received physical transitions before activity/gesture suppression.
                // Repeated full-state reports produce no duplicate entries.
                log_siri_buttons(event,siri_logged_buttons);
                siri.buttons(event.data,event.size,stamp);
                break;
            case InputType::SiriTouch:
                siri.touches(event.data,event.size,stamp);
                break;
            case InputType::Reset:
                ESP_LOGI("remote_input","Remote input reset");
                siri_logged_buttons=0;
                controller.cancel();siri.cancel();swipes.cancel();break;
            }
        }
        // A reserved target changes before the hub publishes its snapshot.
        // Suppress held keys and touches as soon as a remote transition is admitted.
        sync_activity();
        auto now=esp_timer_get_time();
        swipes.tick(now,hub.remote_activity_generation(),[](uint8_t key,uint32_t generation) {
            return hub.press_remote_key_for_activity(key,generation);
        });
        amity::VolumeAdjustment v;
        if(volume_sequence.idle()&&xQueueReceive(volume_queue,&v,0)==pdTRUE)volume_sequence.start(v);
        // A complete CEC press/release needs about 165 ms of clear bus time.
        // Leave room for replies, and retain a step if hub admission is full.
        if(!volume_sequence.idle())volume_sequence.tick(now,hub.activity_generation(),[](uint8_t key,uint32_t generation) {
            return hub.press_key_for_activity(key,generation);
        });
    }
}

esp_err_t retire_original_interfaces() {
    original_interfaces_enabled.store(false);
    const auto fenced=hub.retire_membership();
    const auto web=web_admin::stop();
    {
        // Drain an in-flight publication/refresh before Matter can free the
        // shared responder. Headless members never registered this service.
        std::lock_guard<std::mutex> guard(admin_mdns_mutex);
        if(admin_mdns_registered){
            admin_mdns_registered=false;
            const auto removed=mdns_service_remove_for_host("Amity Home Theater","_https","_tcp",admin_mdns_hostname.c_str());
            if(removed!=ESP_OK)ESP_LOGW("amity","Administration mDNS removal: %s",esp_err_to_name(removed));
        }
    }
    const auto matter=amity_matter_set_controller_enabled(false);
    const auto home=amity_homekit::stop();
    if(fenced!=ESP_OK)return fenced;
    if(web!=ESP_OK)return web;
    if(matter!=ESP_OK)return matter;
    return home;
}
void start_admin_mdns() {
    std::lock_guard<std::mutex> guard(admin_mdns_mutex);
    if(!original_interfaces_enabled.load())return;
    admin_mdns_hostname=device_settings::hostname();
    const auto octopus_id=hub.octopus_id();
    // CONFIG_USE_MINIMAL_MDNS=n makes Matter and HTTPS share this responder.
    ESP_ERROR_CHECK(mdns_init());
    char responder_hostname[65]{};
    if(mdns_hostname_get(responder_hostname)!=ESP_OK||!responder_hostname[0]){
        const auto temporary=std::string("amity-")+octopus_id.str().substr(24);
        ESP_ERROR_CHECK(mdns_hostname_set(temporary.c_str()));
    }
    if(original_interfaces_enabled.load())ESP_ERROR_CHECK(mdns_delegate_hostname_add(admin_mdns_hostname.c_str(),nullptr));
    // mDNS hostname changes are queued, even when initialization succeeds.
    // Wait for the responder to process them before registering the alias's
    // service; a startup timing difference must not cause an OTA rollback.
    esp_err_t admin_mdns=ESP_ERR_INVALID_STATE;
    if(original_interfaces_enabled.load())admin_mdns=ESP_ERR_INVALID_ARG;
    for(unsigned attempt=0;attempt<100&&admin_mdns==ESP_ERR_INVALID_ARG&&original_interfaces_enabled.load();++attempt){
        admin_mdns=mdns_service_add_for_host("Amity Home Theater","_https","_tcp",admin_mdns_hostname.c_str(),443,nullptr,0);
        if(admin_mdns==ESP_ERR_INVALID_ARG)vTaskDelay(pdMS_TO_TICKS(20));
    }
    if(admin_mdns==ESP_OK)admin_mdns_registered=true;
    else if(original_interfaces_enabled.load())ESP_LOGW("amity","Administration discovery is pending; use the DHCP address");
}
void start_original_interfaces() {
    if(!original_interfaces_enabled.load())return;
    const amity_matter_callbacks_t callbacks{power,activity,key,mute,volume,nullptr};
    start_admin_mdns();
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT,ESP_EVENT_ANY_ID,[](void *,esp_event_base_t,int32_t id,void *data){
        if(id==IP_EVENT_STA_GOT_IP) {
            auto *event=static_cast<ip_event_got_ip_t *>(data);
            ESP_LOGI("amity","Administration: https://" IPSTR,IP2STR(&event->ip_info.ip));
        }
        if(id==IP_EVENT_STA_GOT_IP||id==IP_EVENT_STA_LOST_IP||id==IP_EVENT_GOT_IP6)
            refresh_admin_mdns();
    },nullptr));
    refresh_admin_mdns();
    if(original_interfaces_enabled.load()){
        const auto admin=web_admin::start(hub,device_settings::hostname(),device_settings::initial_admin_auth());
        if(admin!=ESP_OK&&original_interfaces_enabled.load())ESP_ERROR_CHECK(admin);
    }
    ESP_ERROR_CHECK(xTaskCreate(input_task,"remote_input",8192,nullptr,4,nullptr)==pdPASS?ESP_OK:ESP_ERR_NO_MEM);
    ble_remote_set_siri_callback(remote_siri,nullptr);
    // esp_matter::start starts NimBLE asynchronously. Attach only once the host syncs.
    esp_err_t ble=ESP_ERR_INVALID_STATE;
    for(unsigned attempt=0;attempt<100&&ble==ESP_ERR_INVALID_STATE;++attempt) {ble=ble_remote_start(remote_key,nullptr);if(ble==ESP_ERR_INVALID_STATE)vTaskDelay(pdMS_TO_TICKS(100));}
    ESP_ERROR_CHECK(ble);
    ble_remote_set_disconnect_callback(remote_disconnect,nullptr);
    startup_memory("before HomeKit");
    if(original_interfaces_enabled.load()){
        const auto home=amity_homekit::start(hub,callbacks);
        if(home!=ESP_OK&&original_interfaces_enabled.load())ESP_ERROR_CHECK(home);
    }
    startup_memory("HomeKit");
}
bool station_has_address() {
    auto* station=esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");esp_netif_ip_info_t address{};
    return station&&esp_netif_is_netif_up(station)&&esp_netif_get_ip_info(station,&address)==ESP_OK&&address.ip.addr;
}
void await_original_work() {
    for(;;){
        const auto role=amity::consortium::runtime_status();
        if(role.member||role.phase=="retired"||role.phase=="recovery_required")return;
        if(station_has_address()&&amity_matter_controller_enrollment_complete()){
            const auto claimed=amity::consortium::runtime_claim_original_controller();
            if(claimed==ESP_OK){
                ESP_ERROR_CHECK(hub.transition_coordinator_authority(true,1));
                ESP_ERROR_CHECK(web_admin::load(hub,false));
                ESP_ERROR_CHECK(web_admin::finish_configuration_activation());
                const auto config=hub.active_configuration();
                if(config)amity::consortium::runtime_configuration_changed(config->config_revision,config_store::executable_policy_hash(*config));
                original_interfaces_enabled.store(true);
                start_original_interfaces();
                ESP_LOGI("amity","Original Amity setup complete. Administration: https://%s.local",device_settings::hostname());
                return;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}
void await_original_task(void*) {
    // Return from the C++ work frame before FreeRTOS deletes its stack.
    await_original_work();vTaskDelete(nullptr);
}

}
extern "C" void app_main() {
#if CONFIG_SPIRAM_USE_MALLOC
    // Planner snapshots, canonical JSON and protocol buffers are task data.
    // Keep them in external RAM; wire engines, frame-start guards, controller
    // allocations explicitly reserve internal memory. Network/config workers
    // use external stacks with flash operations owned by internal-stack tasks.
    heap_caps_malloc_extmem_enable(32);
#endif
    config_store::initialize_json_memory();
    // Never erase NVS automatically: that would destroy Wi-Fi credentials and fabrics.
    ESP_ERROR_CHECK(config_store::init());
    ESP_ERROR_CHECK(device_settings::finish_factory_reset());
#if !CONFIG_AMITY_PRODUCTION_UPDATES
    ESP_ERROR_CHECK(nvs_flash_init());
#endif
    ESP_ERROR_CHECK(amity::consortium::runtime_finish_reclaim(amity_matter_finish_network_reclaim));
    ESP_ERROR_CHECK(device_settings::init());
    device_settings::set_hostname_handler(rename_hostname);
    system_services::start_log_capture();
    auto startup_profile=amity::board_profile;
    const auto passive_profile=[&]{
        for(size_t i=0;i<2u*startup_profile.tentacle_count;++i)startup_profile.ports[i].gpio=-1;
        startup_profile.reset_button=-1;startup_profile.status_led=-1;
    };
    constexpr int reserved_pins[] = {0, 3, 19, 20, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 45, 46};
    const auto profile_error = cec::validate_board_profile(amity::board_profile,
        [](int gpio) { return GPIO_IS_VALID_OUTPUT_GPIO(gpio); }, reserved_pins, sizeof(reserved_pins)/sizeof(reserved_pins[0]),
        [](int gpio) { return GPIO_IS_VALID_GPIO(gpio); });
    if (profile_error != cec::ProfileError::None) {
        ESP_LOGE("amity", "Unsupported or unsafe board profile (%u); CEC outputs remain passive", unsigned(profile_error));
        if(startup_profile.id!=cec::BoardProfileId::Development4Tentacle)return;
        hub.set_hardware_error("HDMI control could not start. Check the device wiring or install firmware intended for this hardware. Administration remains available.");
        passive_profile();
    }
    device_settings::set_setup_button_handler(setup_button);
    if (startup_profile.reset_button >= 0) ESP_ERROR_CHECK(device_settings::start_reset_button(startup_profile.reset_button));
    input_queue=xQueueCreate(32,sizeof(Input));volume_queue=xQueueCreate(8,sizeof(amity::VolumeAdjustment));
    ESP_ERROR_CHECK(input_queue&&volume_queue?ESP_OK:ESP_ERR_NO_MEM);
    amity::system::OctopusId octopus_id;
    amity::system::ConsortiumId consortium_id;
    if (config_store::load_system_identity(octopus_id, consortium_id) != ESP_OK) {
        ESP_LOGE("amity", "Stored origin/system identity needs USB recovery; CEC outputs remain passive");
        return;
    }
    amity::consortium::RuntimeStartup membership;
    const auto membership_read=amity::consortium::runtime_read_startup(octopus_id,membership);
    const bool passive_member=membership_read!=ESP_OK||membership.passive();
    const bool awaiting_first_setup=!passive_member&&membership.role==amity::consortium::RuntimeStartup::unclaimed;
    const bool passive_startup=passive_member||awaiting_first_setup;
    original_interfaces_enabled.store(false);
    if(passive_member)ble_remote_set_controller_enabled(false);
    // Fresh boards need the shared Matter BLE setup host to choose the main
    // role. HTTP/HomeKit/remote input remain absent until enrollment commits.
    ESP_ERROR_CHECK(amity_matter_set_controller_enabled(!passive_member));
    if(membership.consortium_id)consortium_id=membership.consortium_id;
    hub.set_identity(consortium_id, octopus_id);
    hub.set_coordinator_authority(!passive_startup,membership.epoch?membership.epoch:1);
    const auto boot=amity::system::BootId::parse(system_services::boot_id());
    if(!boot)return;
    hub.set_boot_identity(*boot);
    auto cec_start=hub.start(startup_profile,device_settings::adapter_defaults().front_physical);
    if(cec_start!=ESP_OK&&startup_profile.id==cec::BoardProfileId::Development4Tentacle){
        ESP_LOGE("amity","HDMI adapter startup failed (%s); retaining administration with all CEC outputs disabled",esp_err_to_name(cec_start));
        hub.set_hardware_error("HDMI control could not start. Check the device wiring or install firmware intended for this hardware. Administration remains available.");
        passive_profile();cec_start=hub.start(startup_profile,device_settings::adapter_defaults().front_physical);
    }
    ESP_ERROR_CHECK(cec_start);
    system_services::adapter_settings::install(hub);
    amity::consortium::runtime_settings_hooks({system_services::adapter_settings::snapshot,
        system_services::adapter_settings::apply});
    const auto configuration_load=web_admin::load(hub,passive_startup);
    if(configuration_load!=ESP_OK){
        ESP_LOGE("amity","Equipment configuration needs recovery; retaining storage and passive CEC isolation");
        ESP_ERROR_CHECK(hub.retire_membership());
        ESP_ERROR_CHECK(web_admin::load(hub,true));
    }
    if(membership.role==amity::consortium::RuntimeStartup::retired)ESP_ERROR_CHECK(hub.retire_membership());
    amity::consortium::runtime_lifecycle_hooks({retire_original_interfaces,[](amity::system::OctopusId owner){amity::forget_owner(owner);},
    [](amity::system::OctopusId coordinator){return system_services::prepare_removed_device_reset(hub,coordinator);},
    system_services::removed_device_reset_response_attempted});
    system_services::updates::maintenance_hooks([]{
        const auto entered=hub.begin_maintenance();if(entered!=ESP_OK)return entered;
        for(unsigned n=0;n<100;++n){if(hub.maintenance_ready())return ESP_OK;vTaskDelay(pdMS_TO_TICKS(20));}
        return ESP_ERR_TIMEOUT;
    },[]{hub.end_maintenance();});
    startup_memory("configuration");
    ESP_ERROR_CHECK(amity::radio::start());
    ESP_ERROR_CHECK(amity::consortium::runtime_register_gatt());
    amity_matter_callbacks_t callbacks{power,activity,key,mute,volume,nullptr};
    ESP_ERROR_CHECK(amity_matter_start(&callbacks));
    const auto installed=hub.active_configuration();
    ESP_ERROR_CHECK(amity::consortium::runtime_set_hardware_profile(hub.supported_tentacles(),hub.configured_tentacles_mask()));
    const auto group_start=amity::consortium::runtime_start(consortium_id,octopus_id,*boot,installed?installed->config_revision:1);
    if(group_start!=ESP_OK)ESP_LOGE("amity","Consortium credentials need physical recovery; network actuation remains unavailable");
    if(group_start==ESP_OK){
        system_services::updates::start_group_updates(octopus_id,{
            [octopus_id](amity::system::OctopusId owner,bool maintenance){
                if(owner==octopus_id){if(maintenance){if(hub.execution_snapshot().applied_config_revision)hub.begin_maintenance();}else hub.end_maintenance();}
                else amity::set_owner_maintenance(owner,maintenance);
            },[]{return hub.execution_snapshot().applied_config_revision;},[]{return hub.execution_snapshot().applied_policy_hash;}});
        if(installed)amity::consortium::runtime_configuration_changed(installed->config_revision,config_store::executable_policy_hash(*installed));
    }
    startup_memory("Matter and consortium");
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT,ESP_EVENT_ANY_ID,[](void *,esp_event_base_t,int32_t id,void *data){
        if(id==WIFI_EVENT_STA_DISCONNECTED) {
            auto *event=static_cast<wifi_event_sta_disconnected_t *>(data);
            ESP_LOGW("amity","Wi-Fi disconnected: reason=%u RSSI=%d dBm",event->reason,event->rssi);
        } else if(id==WIFI_EVENT_STA_CONNECTED) {
            auto *event=static_cast<wifi_event_sta_connected_t *>(data);
            ESP_LOGI("amity","Wi-Fi associated: channel=%u",event->channel);
        }
    },nullptr));
    hub.on_change([](const amity::Snapshot &s) {
        if(!original_interfaces_enabled.load())return;
        amity_matter_state_t state{};state.power=s.current_activity>=0;const auto input=hub.external_activity_index(s);state.current_input=input.value_or(0);
        state.mute_known=s.audio_status_known;state.muted=s.muted;
        state.volume_known=s.audio_status_known&&s.volume<=100;state.volume=state.volume_known?(s.volume*254+50)/100:0;
        amity_matter_update_state(&state);
    });
    if(group_start==ESP_OK&&membership_read==ESP_OK&&membership.role==amity::consortium::RuntimeStartup::coordinator&&amity::consortium::runtime_status().authority_ready){
        original_interfaces_enabled.store(true);
        start_original_interfaces();
    }
    ESP_ERROR_CHECK(xTaskCreateWithCaps(firmware_update_task,"firmware_update",12288,nullptr,3,nullptr,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)==pdPASS?ESP_OK:ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(xTaskCreate(remote_battery_task,"remote_battery",4096,nullptr,3,nullptr)==pdPASS?ESP_OK:ESP_ERR_NO_MEM);
    amity_matter_status_t status{};
    if(!passive_member&&amity_matter_get_status(&status)==ESP_OK&&status.fabric_count==0)
        printf("\nAMITY MATTER SETUP CODE: %s\nAMITY MATTER QR PAYLOAD: %s\n",status.manual_code,status.qr_payload);
    // Confirm protected local boot without requiring a first user enrollment.
    // A proven original also starts its UI/remote/integrations before this point;
    // fresh and member boards intentionally have no such services.
    ESP_ERROR_CHECK(system_services::confirm_firmware());
    const auto activated=web_admin::finish_configuration_activation();
    if(activated!=ESP_OK)ESP_LOGE("amity","Configuration activation needs recovery; active records retained");
    if(group_start==ESP_OK&&activated==ESP_OK){
        const auto synchronization=amity::start_configuration_synchronization(hub);
        if(synchronization!=ESP_OK)ESP_LOGE("amity","Consortium synchronization could not start; peers remain fenced");
    }
    if(group_start==ESP_OK&&activated==ESP_OK){
        start_boot_matter_setup();
    }
    if(awaiting_first_setup)ESP_ERROR_CHECK(xTaskCreate(await_original_task,"await_setup",12288,nullptr,2,nullptr)==pdPASS?ESP_OK:ESP_ERR_NO_MEM);
    if(original_interfaces_enabled.load())ESP_LOGI("amity","Ready. Administration: https://%s.local (or the DHCP address)",device_settings::hostname());
    else ESP_LOGI("amity","%s",awaiting_first_setup?"Ready for BLE setup; no user interface is running":"Ready as a managed device; administration is on the original Amity");
}
