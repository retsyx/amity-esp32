#include "device_settings.hpp"
#include "config_store.hpp"
#include "esp_partition.h"
#include "psa/crypto.h"
#include <cassert>
#include <functional>
#include <iostream>
#include <map>
#include <thread>
#include <vector>

std::map<std::string,std::string> records;
std::vector<std::string> write_keys;
esp_err_t write_result=ESP_OK;
std::function<void()> during_write;
namespace config_store {
esp_err_t read(const char* key,std::string& value){const auto found=records.find(key);if(found==records.end())return ESP_ERR_NVS_NOT_FOUND;value=found->second;return ESP_OK;}
esp_err_t write(const char* key,const std::string& value){write_keys.emplace_back(key);if(during_write)during_write();if(write_result!=ESP_OK)return write_result;records[key]=value;return ESP_OK;}
}
const esp_partition_t* esp_partition_find_first(int,int,const char*){return nullptr;}
esp_err_t esp_partition_read(const esp_partition_t*,size_t,void*,size_t){assert(false);return ESP_FAIL;}
psa_status_t psa_crypto_init(){assert(false);return -1;}
psa_status_t psa_hash_compute(unsigned,const uint8_t*,size_t,uint8_t*,size_t,size_t*){assert(false);return -1;}
int main(){
    assert(device_settings::init()==ESP_OK&&std::string(device_settings::hostname())=="amity");
    auto defaults=device_settings::adapter_defaults();auto name=device_settings::hostname_settings();
    assert(defaults.revision==1&&name.revision==1&&name.hostname=="amity");
    for(auto address:defaults.front_physical)assert(address==0x1000);
    std::string live_name="amity";
    std::vector<std::pair<std::string,std::string>> renames;
    bool reject_rename=false;
    device_settings::set_hostname_handler([&](const std::string& before,const std::string& after){
        assert(before==live_name);renames.emplace_back(before,after);
        if(reject_rename)return ESP_FAIL;
        live_name=after;return ESP_OK;
    });
    during_write=[]{const auto value=device_settings::hostname_settings();assert(value.revision==1&&value.hostname=="amity");};
    assert(device_settings::save_hostname(1,"Living-Room")==ESP_OK);
    name=device_settings::hostname_settings();
    assert(name.revision==2&&name.hostname=="living-room"&&live_name==name.hostname);
    assert(records.at("hostname")=="AMITYHN1\n2\nliving-room\n"&&records.count("front_defaults")==0);
    assert(device_settings::adapter_defaults().revision==1&&write_keys==std::vector<std::string>{"hostname"});
    const auto durable_name=records.at("hostname");const auto rename_count=renames.size();
    during_write={};
    assert(device_settings::save_adapter_defaults(1,{{3,0x5200},{1,0x2100}})==ESP_OK);
    defaults=device_settings::adapter_defaults();
    assert(defaults.revision==2&&defaults.front_physical[0]==0x1000&&defaults.front_physical[1]==0x2100&&defaults.front_physical[3]==0x5200);
    assert(device_settings::hostname_settings().revision==2&&records.at("hostname")==durable_name&&renames.size()==rename_count);
    assert(records.at("front_defaults")=="AMITYPA1\n2\n1.0.0.0\n2.1.0.0\n1.0.0.0\n5.2.0.0\n"&&write_keys.back()=="front_defaults");
    const auto durable_defaults=records.at("front_defaults");
    // A caller's hostname snapshot survives a rename on another task, and
    // enrollment changes only the hostname record and its own revision.
    const auto* previous=device_settings::hostname();
    std::thread rename([&]{assert(device_settings::apply_enrollment_hostname("Amity-Den")==ESP_OK);});rename.join();
    assert(std::string(previous)=="living-room"&&std::string(device_settings::hostname())=="amity-den");
    assert(device_settings::hostname_settings().revision==3&&live_name=="amity-den"&&device_settings::adapter_defaults().revision==2&&records.at("front_defaults")==durable_defaults);
    const auto saved_writes=write_keys.size(),saved_renames=renames.size();
    assert(device_settings::save_hostname(2,"other")==ESP_ERR_INVALID_STATE);
    assert(device_settings::save_adapter_defaults(1,{{0,0x4000}})==ESP_ERR_INVALID_STATE);
    for(const auto& invalid:std::vector<std::vector<device_settings::AdapterDefaultChange>>{{},{{4,0x1000}},{{1,0x1000},{1,0x2000}},{{2,0x1010}},{{2,0xffff}}})
        assert(device_settings::save_adapter_defaults(2,invalid)==ESP_ERR_INVALID_ARG);
    for(const auto& invalid:std::vector<std::string>{"","-room","room-","living room","room.local","room_name",std::string(33,'x'),std::string("room\0hidden",11)})
        assert(device_settings::save_hostname(3,invalid)==ESP_ERR_INVALID_ARG);
    assert(write_keys.size()==saved_writes&&renames.size()==saved_renames);
    const auto before_failure=records;
    // Failed live service changes never reach storage. A failed hostname write
    // restores the old service name without altering adapter defaults.
    reject_rename=true;assert(device_settings::save_hostname(3,"other")==ESP_FAIL&&write_keys.size()==saved_writes);
    reject_rename=false;write_result=ESP_FAIL;
    during_write=[]{const auto value=device_settings::hostname_settings();assert(value.revision==3&&value.hostname=="amity-den"&&device_settings::adapter_defaults().revision==2);};
    assert(device_settings::save_hostname(3,"other")==ESP_FAIL);
    assert(live_name=="amity-den"&&renames.back()==std::make_pair(std::string("other"),std::string("amity-den")));
    assert(device_settings::hostname_settings().revision==3&&records==before_failure&&device_settings::adapter_defaults().front_physical==defaults.front_physical);
    // Adapter persistence failure cannot rename services or touch hostname state.
    during_write={};const auto failed_rename_count=renames.size();
    assert(device_settings::save_adapter_defaults(2,{{0,0x4000},{3,0x6000}})==ESP_FAIL);
    assert(device_settings::adapter_defaults().revision==2&&device_settings::hostname_settings().revision==3&&renames.size()==failed_rename_count&&records==before_failure);
    write_result=ESP_OK;
    // A same-name hostname save advances only its own revision, without a
    // discovery republish; a subsequent adapter save uses its original revision.
    assert(device_settings::save_hostname(3,"AMITY-DEN")==ESP_OK&&device_settings::hostname_settings().revision==4&&renames.size()==failed_rename_count);
    const auto hostname_after_save=records.at("hostname");
    assert(device_settings::save_adapter_defaults(2,{{3,0}})==ESP_OK);
    assert(device_settings::adapter_defaults().revision==3&&records.at("hostname")==hostname_after_save);
    assert(device_settings::init()==ESP_OK&&device_settings::adapter_defaults().front_physical[3]==0&&device_settings::adapter_defaults().revision==3&&device_settings::hostname_settings().revision==4&&std::string(device_settings::hostname())=="amity-den");
    auto valid_defaults=records.at("front_defaults"),valid_hostname=records.at("hostname");
    // Each record has one current format and rejects extra fields or malformed
    // revisions independently of the other valid record.
    for(const auto& malformed:std::vector<std::string>{valid_defaults+"5.0.0.0\n",valid_defaults+"extra","AMITYPA1\n0\n1.0.0.0\n1.0.0.0\n1.0.0.0\n1.0.0.0\n","AMITYPA1\n1\n1.0.1.0\n1.0.0.0\n1.0.0.0\n1.0.0.0\n"}){
        records["front_defaults"]=malformed;assert(device_settings::init()==ESP_ERR_INVALID_STATE);
    }
    records["front_defaults"]=valid_defaults;
    for(const auto& malformed:std::vector<std::string>{valid_hostname+"extra","AMITYHN1\n0\namity\n","AMITYHN1\n01\namity\n","AMITYHN1\n18446744073709551616\namity\n","AMITYHN1\n1\nbad.local\n","AMITYHN1\n1\nMixedCase\n","amity"}){
        records["hostname"]=malformed;assert(device_settings::init()==ESP_ERR_INVALID_STATE);
    }
    records["hostname"]=valid_hostname;
    records["front_defaults"]="AMITYPA1\n18446744073709551615\n1.0.0.0\n1.0.0.0\n1.0.0.0\n1.0.0.0\n";
    assert(device_settings::init()==ESP_OK&&device_settings::save_adapter_defaults(UINT64_MAX,{{0,0x2000}})==ESP_ERR_INVALID_STATE);
    const std::string maximum(32,'x');assert(device_settings::save_hostname(4,maximum)==ESP_OK&&device_settings::hostname_settings().revision==5&&std::string(device_settings::hostname())==maximum);
    records["front_defaults"]=valid_defaults;records["hostname"]="AMITYHN1\n18446744073709551615\n"+maximum+"\n";
    assert(device_settings::init()==ESP_OK&&device_settings::save_hostname(UINT64_MAX,"other")==ESP_ERR_INVALID_STATE);
    assert(device_settings::save_adapter_defaults(3,{{0,0x2000}})==ESP_OK&&device_settings::adapter_defaults().revision==4&&device_settings::hostname_settings().revision==UINT64_MAX);
    std::cout<<"PASS independent hostname and adapter settings: separate records/revisions, atomic sparse defaults, live service/storage rollback, task-local hostname lifetime, stale/invalid rejection, durable reload and independent revision exhaustion\n";
}
