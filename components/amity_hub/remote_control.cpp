#include "amity/system_control.hpp"
namespace amity {
bool SystemControl::accepts_local_input()const{
    if(!local_coordinator_.load()||maintenance_.load())return false;
    const auto config=active_configuration();return !config||config->controller_octopus_id==octopus_;
}
esp_err_t SystemControl::request_activity(int index){
    if(!accepts_local_input())return ESP_ERR_INVALID_STATE;
    auto config=active_configuration();
    if(!config||index<0)return local_.request_activity(index);
    const auto* activity=config_store::activity_for_slot(*config,static_cast<unsigned>(index));
    if(!activity)return ESP_ERR_INVALID_ARG;
    auto order=config_store::ordered_activities(*config);auto found=std::find(order.begin(),order.end(),activity->activity_id);
    return found==order.end()?ESP_ERR_INVALID_ARG:local_.request_activity(static_cast<int>(found-order.begin()),nullptr,activity->activity_id);
}
bool SystemControl::press_key(uint8_t key,bool repeat){return accepts_local_input()&&local_.press_key(key,repeat);}
esp_err_t SystemControl::request_remote_activity(unsigned index){return request_remote_activity(index,remote_activity_generation());}
esp_err_t SystemControl::request_remote_activity(unsigned index,uint32_t generation){
    if(!accepts_local_input()||generation!=activity_generation())return ESP_ERR_INVALID_STATE;
    return local_.request_activity(static_cast<int>(index));
}
esp_err_t SystemControl::repair_remote_activity(uint32_t generation){return accepts_local_input()&&generation==activity_generation()?local_.repair_activity():ESP_ERR_INVALID_STATE;}
esp_err_t SystemControl::stop_remote_zone(uint32_t generation){return accepts_local_input()&&generation==activity_generation()?local_.request_activity(-1):ESP_ERR_INVALID_STATE;}
}
