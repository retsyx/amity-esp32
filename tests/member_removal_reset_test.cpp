#include "member_removal.hpp"
#include "amity/system_control.hpp"
#include "device_settings.hpp"
#include "config_store.hpp"
#include "nvs_flash.h"
#include <cassert>
#include <iostream>
#include <string>
#include <vector>
namespace m=amity::system;
namespace {
template<class T>T id(unsigned n){char bytes[33];snprintf(bytes,sizeof(bytes),"%032x",n);return *T::parse(bytes);}
bool broker_available=true,on_flash_stack=false;
unsigned retired=0,broker_calls=0,open_handles=0,opens=0,commits=0,reboots=0;
unsigned fail_at=0,flash_step=0;bool marker=false;
std::vector<std::string> events;
bool failure(){return ++flash_step==fail_at;}
}
namespace config_store {
bool with_internal_flash_stack(std::function<void()> callback){++broker_calls;if(!broker_available)return false;assert(!on_flash_stack);on_flash_stack=true;callback();on_flash_stack=false;return true;}
}
namespace system_services {void reboot_later(){assert(marker&&device_settings::reset_pending());++reboots;events.push_back("restart_after_reply_attempt");}}
esp_err_t nvs_open_from_partition(const char* partition,const char* space,nvs_open_mode_t mode,nvs_handle_t* out){
    assert(on_flash_stack&&std::string(partition)=="amity_cfg"&&std::string(space)=="amity_reset"&&mode==NVS_READWRITE);++opens;
    if(failure())return ESP_FAIL;
    *out=1;++open_handles;return ESP_OK;
}
esp_err_t nvs_set_u8(nvs_handle_t h,const char* key,uint8_t value){assert(on_flash_stack&&h==1&&std::string(key)=="pending"&&value==1);return failure()?ESP_FAIL:ESP_OK;}
esp_err_t nvs_commit(nvs_handle_t h){assert(on_flash_stack&&h==1);if(failure())return ESP_FAIL;++commits;marker=true;events.push_back("reset_marker_committed");return ESP_OK;}
void nvs_close(nvs_handle_t h){assert(h==1&&open_handles);--open_handles;}
esp_err_t nvs_open(const char*,nvs_open_mode_t,nvs_handle_t*){assert(false);return ESP_FAIL;}
esp_err_t nvs_get_blob(nvs_handle_t,const char*,void*,size_t*){assert(false);return ESP_FAIL;}
esp_err_t nvs_set_blob(nvs_handle_t,const char*,const void*,size_t){assert(false);return ESP_FAIL;}
esp_err_t nvs_get_u8(nvs_handle_t,const char*,uint8_t*){assert(false);return ESP_FAIL;}
esp_err_t nvs_erase_key(nvs_handle_t,const char*){assert(false);return ESP_FAIL;}
esp_err_t nvs_erase_all(nvs_handle_t){assert(false);return ESP_FAIL;}
esp_err_t nvs_flash_init(){assert(false);return ESP_FAIL;}
esp_err_t nvs_flash_deinit(){assert(false);return ESP_FAIL;}
esp_err_t nvs_flash_erase(){assert(false);return ESP_FAIL;}
int main(){
    using system_services::prepare_removed_device_reset;
    const auto main=id<m::OctopusId>(1),member=id<m::OctopusId>(2);
    amity::SystemControl owner;owner.local=member;owner.configuration=amity::RemovalConfiguration{main};
    owner.retire=[&]{++retired;return ESP_OK;};
    system_services::removed_device_reset_response_attempted();assert(!reboots);
    // No other-controller or self/main path can create a reset decision.
    assert(prepare_removed_device_reset(owner,{})==ESP_ERR_INVALID_STATE);
    assert(prepare_removed_device_reset(owner,id<m::OctopusId>(3))==ESP_ERR_INVALID_STATE);
    owner.local=main;assert(prepare_removed_device_reset(owner,main)==ESP_ERR_INVALID_STATE);owner.local=member;
    owner.configuration=amity::RemovalConfiguration{id<m::OctopusId>(3)};
    assert(prepare_removed_device_reset(owner,main)==ESP_ERR_INVALID_STATE);
    owner.configuration=amity::RemovalConfiguration{};assert(prepare_removed_device_reset(owner,main)==ESP_ERR_INVALID_STATE);
    assert(!broker_calls&&!opens&&!marker);
    owner.configuration=amity::RemovalConfiguration{main};
    broker_available=false;assert(prepare_removed_device_reset(owner,main)==ESP_FAIL&&!opens&&!marker);broker_available=true;
    for(unsigned step=1;step<=3;++step){fail_at=step;flash_step=0;
        assert(prepare_removed_device_reset(owner,main)==ESP_FAIL&&!open_handles&&!marker&&!device_settings::reset_pending());
        system_services::removed_device_reset_response_attempted();assert(!reboots);
    }
    fail_at=0;flash_step=0;events.clear();
    // A committed management-only member may lack executable configuration.
    owner.configuration.reset();
    assert(prepare_removed_device_reset(owner,main)==ESP_OK&&marker&&device_settings::reset_pending()&&!open_handles&&!reboots);
    // The runtime's lifecycle retire hook fences the board before this call.
    assert((events==std::vector<std::string>{"reset_marker_committed"})&&!retired);
    const auto old_opens=opens,old_commits=commits;
    assert(prepare_removed_device_reset(owner,main)==ESP_OK&&opens==old_opens&&commits==old_commits);
    // The transmission may fail, but the deliberate durable reset is never
    // stranded. Repeated replies cannot schedule duplicate restart tasks.
    events.push_back("reply_transmission_attempt_failed");system_services::removed_device_reset_response_attempted();system_services::removed_device_reset_response_attempted();
    assert(reboots==1&&events.back()=="restart_after_reply_attempt");
    std::cout<<"PASS actual removed-device reset service/journal: main/other-controller rejection, management-only member, broker lifetime, storage failures, durable marker before reply attempt, lost-reply restart and duplicate suppression\n";
}
