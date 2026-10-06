#include "board_pins.h"
#include "amity/hub_service.hpp"
#include "amity_matter.h"
#include "homekit.hpp"
#include "atomic_controller.hpp"
#include "volume_sequence.hpp"
#include <atomic>
#include "ble_remote.h"
#include "config_store.hpp"
#include "device_settings.hpp"
#include "system_services.hpp"
#include "web_admin.hpp"
#include "esp_log.h"
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
namespace {
amity::HubService hub;
struct Input { uint16_t page,usage; bool down; bool reset=false; };
std::atomic_bool input_lost{false};
QueueHandle_t input_queue;
void refresh_admin_mdns() {
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
    const auto err=mdns_delegate_hostname_set_address(device_settings::hostname(),count?addresses:nullptr);
    if(err!=ESP_OK)ESP_LOGW("amity","Administration mDNS address update: %s",esp_err_to_name(err));
}
void remote_key(uint16_t page,uint16_t usage,bool down,void *) {
    Input input{page,usage,down};
    if(xQueueSend(input_queue,&input,0)!=pdTRUE) input_lost.store(true);
}
void remote_disconnect(void *) {
    Input reset{};reset.reset=true;
    if(xQueueSend(input_queue,&reset,0)!=pdTRUE)input_lost.store(true);
}
esp_err_t power(bool on,void *) {
    if(!on)return hub.request_activity(-1);
    const int target=hub.activity_target();
    if(hub.snapshot().activities.empty())return ESP_ERR_INVALID_STATE;
    return hub.request_activity(target<0?0:target);
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
void remote_battery_task(void *) {
    for (;;) {
        system_services::log_memory_if_due();
        ble_remote_status_t remote{};
        ble_remote_get_status(&remote);
        const auto err = amity_matter_update_remote(remote.bonded, remote.connected, remote.battery_percent, remote.charging);
        if (err != ESP_OK) ESP_LOGW("amity", "Remote battery Matter update failed: %s", esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
void input_task(void *) {
    ble::AtomicController controller([](ble::AtomicCommand command,void *) {
        switch(command.type) {
        case ble::AtomicCommandType::Press: hub.press_key(command.value,true);break;
        case ble::AtomicCommandType::Release: hub.release_key();break;
        case ble::AtomicCommandType::Activity: hub.select_activity(command.value);break;
        case ble::AtomicCommandType::PowerShort: {const int target=hub.activity_target();hub.request_activity(target<0?0:-1);break;}
        case ble::AtomicCommandType::PowerLong: hub.request_activity(hub.activity_target());break;
        }
    },nullptr,4);
    amity::VolumeSequence volume_sequence;
    for(;;) {
        if(input_lost.exchange(false)) {xQueueReset(input_queue);controller.cancel();}
        Input event{};
        if(xQueueReceive(input_queue,&event,pdMS_TO_TICKS(20))==pdTRUE) {
            if(event.reset)controller.cancel();
            else controller.input(event.page,event.usage,event.down,esp_timer_get_time());
        }
        auto now=esp_timer_get_time();
        amity::VolumeAdjustment v;
        if(volume_sequence.idle()&&xQueueReceive(volume_queue,&v,0)==pdTRUE)volume_sequence.start(v);
        // A complete CEC press/release needs about 165 ms of clear bus time.
        // Leave room for replies, and retain a step if hub admission is full.
        if(!volume_sequence.idle())volume_sequence.tick(now,hub.activity_generation(),[](uint8_t key,uint32_t generation) {
            return hub.press_key_for_activity(key,generation);
        });
    }
}
}
extern "C" void app_main() {
    // Never erase NVS automatically: that would destroy Wi-Fi credentials and fabrics.
    ESP_ERROR_CHECK(config_store::init());
    ESP_ERROR_CHECK(device_settings::finish_factory_reset());
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(device_settings::init());
    system_services::start_log_capture();
    ESP_ERROR_CHECK(device_settings::start_reset_button(AMITY_FW_RESET_GPIO));
    input_queue=xQueueCreate(32,sizeof(Input));volume_queue=xQueueCreate(8,sizeof(amity::VolumeAdjustment));
    ESP_ERROR_CHECK(input_queue&&volume_queue?ESP_OK:ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(hub.start(AMITY_CEC_FRONT_GPIO,AMITY_CEC_BACK_GPIO,device_settings::front_physical_address()));
    ESP_ERROR_CHECK(web_admin::load(hub));
    amity_matter_callbacks_t callbacks{power,activity,key,mute,volume,nullptr};
    ESP_ERROR_CHECK(amity_matter_start(&callbacks));
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
        amity_matter_state_t state{};state.power=s.current_activity>=0;state.current_input=s.current_activity<0?0:s.current_activity;
        state.mute_known=s.audio_status_known;state.muted=s.muted;
        state.volume_known=s.audio_status_known&&s.volume<=100;state.volume=state.volume_known?(s.volume*254+50)/100:0;
        amity_matter_update_state(&state);
    });
    // CONFIG_USE_MINIMAL_MDNS=n makes Matter and HTTPS share this responder.
    ESP_ERROR_CHECK(mdns_init());
    ESP_ERROR_CHECK(mdns_delegate_hostname_add(device_settings::hostname(),nullptr));
    ESP_ERROR_CHECK(mdns_service_add_for_host("Amity Home Theater","_https","_tcp",device_settings::hostname(),443,nullptr,0));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT,ESP_EVENT_ANY_ID,[](void *,esp_event_base_t,int32_t id,void *data){
        if(id==IP_EVENT_STA_GOT_IP) {
            auto *event=static_cast<ip_event_got_ip_t *>(data);
            ESP_LOGI("amity","Administration: https://" IPSTR,IP2STR(&event->ip_info.ip));
        }
        if(id==IP_EVENT_STA_GOT_IP||id==IP_EVENT_STA_LOST_IP||id==IP_EVENT_GOT_IP6)
            refresh_admin_mdns();
    },nullptr));
    refresh_admin_mdns();
    ESP_ERROR_CHECK(web_admin::start(hub,device_settings::hostname(),device_settings::initial_admin_auth()));
    ESP_ERROR_CHECK(xTaskCreate(input_task,"remote_input",8192,nullptr,4,nullptr)==pdPASS?ESP_OK:ESP_ERR_NO_MEM);
    // esp_matter::start starts NimBLE asynchronously. Attach only once the host syncs.
    esp_err_t ble=ESP_ERR_INVALID_STATE;
    for(unsigned attempt=0;attempt<100&&ble==ESP_ERR_INVALID_STATE;++attempt) {ble=ble_remote_start(remote_key,nullptr);if(ble==ESP_ERR_INVALID_STATE)vTaskDelay(pdMS_TO_TICKS(100));}
    ESP_ERROR_CHECK(ble);
    ble_remote_set_disconnect_callback(remote_disconnect,nullptr);
    ESP_ERROR_CHECK(xTaskCreate(remote_battery_task,"remote_battery",4096,nullptr,3,nullptr)==pdPASS?ESP_OK:ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(amity_homekit::start(hub,callbacks));
    amity_matter_status_t status{};
    if(amity_matter_get_status(&status)==ESP_OK&&status.fabric_count==0)
        printf("\nAMITY MATTER SETUP CODE: %s\nAMITY MATTER QR PAYLOAD: %s\n",status.manual_code,status.qr_payload);
    // The update is healthy only after its CEC service, configuration, Matter, BLE and HTTPS start.
    ESP_ERROR_CHECK(system_services::confirm_firmware());
    ESP_LOGI("amity","Ready. Administration: https://%s.local (or the DHCP address)",device_settings::hostname());
}
