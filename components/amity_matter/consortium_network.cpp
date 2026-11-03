// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#include "amity/consortium/runtime.hpp"
#include "candidate_network.hpp"
#include "config_store.hpp"
#include "device_settings.hpp"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_matter.h"
#include "platform/ESP32/NetworkCommissioningDriver.h"
#include "platform/KeyValueStoreManager.h"
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
namespace model=amity::system;
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
bool configured(std::string& ssid,std::string& password,wifi_config_t* snapshot=nullptr){
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);wifi_config_t configuration{};const auto result=esp_wifi_get_config(WIFI_IF_STA,&configuration);
    if(result==ESP_OK){ssid.assign(reinterpret_cast<char*>(configuration.sta.ssid),strnlen(reinterpret_cast<char*>(configuration.sta.ssid),32));password.assign(reinterpret_cast<char*>(configuration.sta.password),strnlen(reinterpret_cast<char*>(configuration.sta.password),64));if(snapshot)*snapshot=configuration;}
    mbedtls_platform_zeroize(&configuration,sizeof(configuration));return result==ESP_OK;
}
bool export_credentials(std::string& ssid,std::string& password){return usable_ip()&&configured(ssid,password);}
// The pinned driver supports one network, always index0. Removal changes only
// staging; its own backup remains until authenticated transaction commitment.
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
esp_err_t restore_previous(const config_store::CandidateNetwork& saved){
    if(saved.old_index!=0||saved.old_driver_configuration.size()!=sizeof(wifi_config_t)||pending.load())return ESP_ERR_INVALID_STATE;
    if(xSemaphoreTake(transaction,0)!=pdTRUE)return ESP_ERR_INVALID_STATE;
    const auto result=driver_call([&]{auto& driver=nc::ESPWiFiDriver::GetInstance();if(driver.RevertConfiguration()!=CHIP_NO_ERROR||!remove_other(driver,saved.old_ssid))return ESP_FAIL;
        if(!saved.old_ssid.empty()){char debug[128];chip::MutableCharSpan text(debug);uint8_t index=0;if(driver.AddOrUpdateNetwork(chip::ByteSpan(reinterpret_cast<const uint8_t*>(saved.old_ssid.data()),saved.old_ssid.size()),chip::ByteSpan(reinterpret_cast<const uint8_t*>(saved.old_password.data()),saved.old_password.size()),text,index)!=nc::Status::kSuccess||index!=saved.old_index)return ESP_FAIL;}
        wifi_config_t previous{};memcpy(&previous,saved.old_driver_configuration.data(),sizeof(previous));esp_wifi_disconnect();const auto restored=esp_wifi_set_config(WIFI_IF_STA,&previous);mbedtls_platform_zeroize(&previous,sizeof(previous));if(restored!=ESP_OK)return restored;
        if(driver.CommitConfiguration()!=CHIP_NO_ERROR)return ESP_FAIL;
        if(!saved.old_ssid.empty())esp_wifi_connect();
        return ESP_OK;});
    staged=false;xSemaphoreGive(transaction);return result;
}
esp_err_t stage_transaction(model::JoinId join,const std::string& digest,const std::string& ssid,const std::string& password){
    config_store::CandidateNetwork saved;std::string error;auto loaded=config_store::load_candidate_network(saved,error);
    if(loaded==ESP_ERR_NVS_NOT_FOUND){wifi_config_t previous{};if(!configured(saved.old_ssid,saved.old_password,&previous))return ESP_FAIL;saved.old_driver_configuration.assign(reinterpret_cast<const char*>(&previous),sizeof(previous));mbedtls_platform_zeroize(&previous,sizeof(previous));saved.join_id=join;saved.proposal_digest=digest;saved.ssid=ssid;saved.password=password;if(config_store::stage_candidate_network(saved,error)!=ESP_OK)return ESP_FAIL;}
    else if(loaded!=ESP_OK||saved.join_id!=join||saved.proposal_digest!=digest||saved.ssid!=ssid||saved.password!=password||saved.decided)return ESP_ERR_INVALID_STATE;
    return stage_connect(ssid,password);
}
esp_err_t commit_transaction(model::JoinId join,const std::string& digest){
    config_store::CandidateNetwork saved;std::string error;const auto loaded=config_store::load_candidate_network(saved,error);if(loaded==ESP_ERR_NVS_NOT_FOUND)return ESP_OK;if(loaded!=ESP_OK||saved.join_id!=join||saved.proposal_digest!=digest)return ESP_ERR_INVALID_STATE;
    std::string ssid,password;if(!configured(ssid,password))return ESP_FAIL;const bool candidate=ssid==saved.ssid&&password==saved.password;mbedtls_platform_zeroize(password.data(),password.size());
    if(!usable_ip()||(!candidate&&ssid!=saved.old_ssid))return ESP_ERR_INVALID_STATE;
    if(config_store::decide_candidate_network(join,digest,candidate,error)!=ESP_OK)return ESP_FAIL;
    // Only an exact authenticated LAN commit reaches this function. The durable
    // decision precedes deletion of the driver's prior-network rollback backup.
    if(candidate){if(!staged)return driver_call([]{return nc::ESPWiFiDriver::GetInstance().CommitConfiguration()==CHIP_NO_ERROR?ESP_OK:ESP_FAIL;});return commit();}
    return restore_previous(saved);
}
esp_err_t finish_transaction(model::JoinId join,const std::string& digest){std::string error;return config_store::finish_candidate_network(join,digest,error);}
esp_err_t abort_transaction(model::JoinId join,const std::string& digest){
    config_store::CandidateNetwork saved;std::string error;const auto loaded=config_store::load_candidate_network(saved,error);
    if(loaded==ESP_ERR_NVS_NOT_FOUND)return ESP_OK;
    if(loaded!=ESP_OK||saved.join_id!=join||saved.proposal_digest!=digest||saved.decided)return ESP_ERR_INVALID_STATE;
    // The membership adapter durably retires this exact prepared transaction
    // first. Keep the rollback snapshot until the complete old driver state is
    // restored, so a power cut repeats this without reactivating any authority.
    const auto restored=restore_previous(saved);if(restored!=ESP_OK)return restored;
    return config_store::abandon_candidate_network(join,digest,error);
}
esp_err_t disconnect_for_recovery(){
    const auto result=esp_wifi_disconnect();if(result!=ESP_OK&&result!=ESP_ERR_WIFI_NOT_CONNECT)return result;
    for(unsigned n=0;n<100;++n){if(!usable_ip())return ESP_OK;vTaskDelay(pdMS_TO_TICKS(20));}return ESP_ERR_TIMEOUT;
}
esp_err_t recover_transaction(model::JoinId join,const std::string& digest,bool member){
    config_store::CandidateNetwork saved;std::string error;const auto loaded=config_store::load_candidate_network(saved,error);if(loaded==ESP_ERR_NVS_NOT_FOUND)return ESP_OK;if(loaded!=ESP_OK)return loaded;
    if(saved.join_id!=join||saved.proposal_digest!=digest){if(saved.decided)return ESP_ERR_INVALID_STATE;const auto restored=restore_previous(saved);return restored==ESP_OK?config_store::abandon_candidate_network(saved.join_id,saved.proposal_digest,error):restored;}
    if(member&&saved.decided){const auto restored=saved.use_candidate?ESP_OK:restore_previous(saved);if(restored!=ESP_OK)return restored;if(saved.use_candidate){std::string ssid,password;if(!configured(ssid,password))return ESP_FAIL;const bool candidate=ssid==saved.ssid&&password==saved.password;mbedtls_platform_zeroize(password.data(),password.size());if(!candidate){if(usable_ip()&&disconnect_for_recovery()!=ESP_OK)return ESP_ERR_TIMEOUT;const auto result=stage_connect(saved.ssid,saved.password);if(result!=ESP_OK)return result;}const auto committed=driver_call([]{return nc::ESPWiFiDriver::GetInstance().CommitConfiguration()==CHIP_NO_ERROR?ESP_OK:ESP_FAIL;});if(committed!=ESP_OK)return committed;}return finish_transaction(join,digest);}
    if(!saved.decided){const auto restored=restore_previous(saved);if(restored!=ESP_OK)return restored;if(usable_ip())return ESP_OK;return stage_connect(saved.ssid,saved.password);}
    if(!saved.use_candidate)return restore_previous(saved);
    std::string ssid,password;if(!configured(ssid,password))return ESP_FAIL;const bool candidate=ssid==saved.ssid&&password==saved.password;mbedtls_platform_zeroize(password.data(),password.size());if(candidate&&usable_ip())return ESP_OK;if(usable_ip()&&disconnect_for_recovery()!=ESP_OK)return ESP_ERR_TIMEOUT;return stage_connect(saved.ssid,saved.password);
}
}
esp_err_t install_consortium_network_hooks(){transaction=xSemaphoreCreateMutex();completed=xSemaphoreCreateBinary();if(!transaction||!completed)return ESP_ERR_NO_MEM;
    amity::consortium::RuntimeNetworkHooks hooks{export_credentials,stage_connect,commit,rollback};hooks.stage_transaction=stage_transaction;hooks.commit_transaction=commit_transaction;hooks.finish_transaction=finish_transaction;hooks.recover_transaction=recover_transaction;hooks.abort_transaction=abort_transaction;
    hooks.get_hostname=[]{return std::string(device_settings::hostname());};
    hooks.set_hostname=[](const std::string& name){
        return device_settings::apply_enrollment_hostname(name);
    };
    amity::consortium::runtime_network_hooks(std::move(hooks));return ESP_OK;
}

// Called before Matter initializes, only after the physical reclaim marker and
// original board identity have been validated. Preserve current station data;
// remove only the pinned SDK driver's pending fail-safe rollback snapshots.
extern "C" esp_err_t amity_matter_finish_network_reclaim(){
    esp_err_t result=ESP_FAIL;
    const bool dispatched=config_store::with_internal_flash_stack([&]{
        auto& storage=chip::DeviceLayer::PersistedStorage::KeyValueStoreMgr();
        for(const auto* key:{"wifi-ssid","wifi-pass"}){
            const auto erased=storage.Delete(key);if(erased!=CHIP_NO_ERROR&&erased!=CHIP_ERROR_PERSISTED_STORAGE_VALUE_NOT_FOUND)return;
            const auto checked=storage.Get(key,nullptr,0);if(checked!=CHIP_ERROR_PERSISTED_STORAGE_VALUE_NOT_FOUND)return;
        }
        result=ESP_OK;
    });
    return dispatched?result:ESP_FAIL;
}
