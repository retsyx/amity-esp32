// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#include "amity/consortium/runtime.hpp"
#include "device_settings.hpp"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_matter.h"
#include "platform/ESP32/NetworkCommissioningDriver.h"
#include "platform/PlatformManager.h"
#include "esp_memory_utils.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <mbedtls/platform_util.h>
namespace {
namespace nc=chip::DeviceLayer::NetworkCommissioning;
SemaphoreHandle_t transaction=nullptr,completed=nullptr;
std::atomic_bool pending{false};bool staged=false;
class Callback final:public nc::Internal::WirelessDriver::ConnectCallback {
public:std::atomic_bool success{false};
    void OnResult(nc::Status status,chip::CharSpan,int32_t)override{success.store(status==nc::Status::kSuccess);pending.store(false);xSemaphoreGive(completed);}
} callback;
bool usable_ip(){auto* netif=esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");esp_netif_ip_info_t info{};return netif&&esp_netif_is_netif_up(netif)&&esp_netif_get_ip_info(netif,&info)==ESP_OK&&info.ip.addr;}
esp_err_t driver_call(const std::function<esp_err_t()>& fn){
    if(chip::DeviceLayer::PlatformMgr().IsChipStackLockedByCurrentThread()){int marker=0;if(!esp_ptr_internal(&marker))return ESP_ERR_INVALID_STATE;return fn();}
    struct Work{const std::function<esp_err_t()>& action;SemaphoreHandle_t done;esp_err_t result=ESP_FAIL;};
    Work work{fn,xSemaphoreCreateBinary()};if(!work.done)return ESP_ERR_NO_MEM;
    const auto queued=chip::DeviceLayer::PlatformMgr().ScheduleWork([](intptr_t argument){auto& item=*reinterpret_cast<Work*>(argument);int marker=0;if(esp_ptr_internal(&marker))item.result=item.action();xSemaphoreGive(item.done);},reinterpret_cast<intptr_t>(&work));
    // A scheduled callback must finish before references on the caller's stack
    // expire. Connection completion has its separate bounded callback below.
    if(queued==CHIP_NO_ERROR)xSemaphoreTake(work.done,portMAX_DELAY);
    vSemaphoreDelete(work.done);return queued==CHIP_NO_ERROR?work.result:ESP_FAIL;
}
bool configured(std::string& ssid,std::string& password){
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);wifi_config_t configuration{};const auto result=esp_wifi_get_config(WIFI_IF_STA,&configuration);
    if(result==ESP_OK){ssid.assign(reinterpret_cast<char*>(configuration.sta.ssid),strnlen(reinterpret_cast<char*>(configuration.sta.ssid),32));password.assign(reinterpret_cast<char*>(configuration.sta.password),strnlen(reinterpret_cast<char*>(configuration.sta.password),64));}
    mbedtls_platform_zeroize(&configuration,sizeof(configuration));return result==ESP_OK;
}
bool export_credentials(std::string& ssid,std::string& password){return usable_ip()&&configured(ssid,password);}
// The pinned driver supports one network, always index0. Removal changes only
// staging; its own backup remains until commit().
bool remove_other(nc::ESPWiFiDriver& driver,const std::string& desired){
    auto* iterator=driver.GetNetworks();if(!iterator)return false;nc::Network network{};bool valid=iterator->Count()<=1;const bool found=iterator->Next(network);iterator->Release();
    if(!valid)return false;
    if(!found||std::string(reinterpret_cast<char*>(network.networkID),network.networkIDLen)==desired)return true;
    char debug[128];chip::MutableCharSpan text(debug);uint8_t index=0;return driver.RemoveNetwork(chip::ByteSpan(network.networkID,network.networkIDLen),text,index)==nc::Status::kSuccess;
}
esp_err_t stage_connect(const std::string& ssid,const std::string& password){
    if(ssid.empty()||ssid.size()>32||password.size()>64||ssid.find('\0')!=std::string::npos||password.find('\0')!=std::string::npos)return ESP_ERR_INVALID_ARG;
    if(xSemaphoreTake(transaction,0)!=pdTRUE)return ESP_ERR_INVALID_STATE;
    if(staged||pending.load()||usable_ip()){xSemaphoreGive(transaction);return ESP_ERR_INVALID_STATE;}
    while(xSemaphoreTake(completed,0)==pdTRUE){}callback.success.store(false);pending.store(true);
    const auto deadline=esp_timer_get_time()+35000000;
    const auto remaining_ms=[&]{return std::max<int64_t>(0,(deadline-esp_timer_get_time())/1000);};
    const auto admitted=driver_call([&]{auto& driver=nc::ESPWiFiDriver::GetInstance();if(!remove_other(driver,ssid))return ESP_ERR_INVALID_STATE;char debug[128];chip::MutableCharSpan text(debug);uint8_t index=0;
        if(driver.AddOrUpdateNetwork(chip::ByteSpan(reinterpret_cast<const uint8_t*>(ssid.data()),ssid.size()),chip::ByteSpan(reinterpret_cast<const uint8_t*>(password.data()),password.size()),text,index)!=nc::Status::kSuccess||index!=0)return ESP_ERR_INVALID_STATE;
        driver.ConnectNetwork(chip::ByteSpan(reinterpret_cast<const uint8_t*>(ssid.data()),ssid.size()),&callback);return ESP_OK;});
    if(admitted!=ESP_OK){pending.store(false);xSemaphoreGive(transaction);return admitted;}
    const bool finished=xSemaphoreTake(completed,pdMS_TO_TICKS(remaining_ms()))==pdTRUE;
    // The SDK reports success at association, before DHCP. Keep the same total
    // connection budget while awaiting the IPv4 address required by admission.
    bool success=false;
    while(finished&&callback.success.load()&&esp_timer_get_time()<deadline){
        if(usable_ip()){success=true;break;}
        const auto ticks=pdMS_TO_TICKS(std::min<int64_t>(100,remaining_ms()));
        if(!ticks)break;
        vTaskDelay(ticks);
    }
    staged=success;if(!success)driver_call([]{return nc::ESPWiFiDriver::GetInstance().RevertConfiguration()==CHIP_NO_ERROR?ESP_OK:ESP_FAIL;});
    xSemaphoreGive(transaction);return success?ESP_OK:ESP_ERR_TIMEOUT;
}
esp_err_t commit(){
    if(xSemaphoreTake(transaction,0)!=pdTRUE)return ESP_ERR_INVALID_STATE;
    const auto result=!staged||!usable_ip()?ESP_ERR_INVALID_STATE:driver_call([]{return nc::ESPWiFiDriver::GetInstance().CommitConfiguration()==CHIP_NO_ERROR?ESP_OK:ESP_FAIL;});
    if(result==ESP_OK)staged=false;
    xSemaphoreGive(transaction);return result;
}
void rollback(){if(xSemaphoreTake(transaction,0)!=pdTRUE)return;driver_call([]{return nc::ESPWiFiDriver::GetInstance().RevertConfiguration()==CHIP_NO_ERROR?ESP_OK:ESP_FAIL;});staged=false;xSemaphoreGive(transaction);}
}
esp_err_t install_consortium_network_hooks(){transaction=xSemaphoreCreateMutex();completed=xSemaphoreCreateBinary();if(!transaction||!completed)return ESP_ERR_NO_MEM;
    amity::consortium::RuntimeNetworkHooks hooks{export_credentials,stage_connect,commit,rollback};
    hooks.get_hostname=[]{return std::string(device_settings::hostname());};
    hooks.set_hostname=[](const std::string& name){
        return device_settings::apply_enrollment_hostname(name);
    };
    amity::consortium::runtime_network_hooks(std::move(hooks));return ESP_OK;
}
