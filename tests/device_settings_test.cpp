#include "device_settings.hpp"
#include "config_store.hpp"
#include "esp_partition.h"
#include "psa/crypto.h"
#include <cassert>
#include <functional>
#include <iostream>
#include <map>
#include <vector>

std::map<std::string,std::string> records;
unsigned writes=0;
esp_err_t write_result=ESP_OK;
std::function<void()> during_write;
namespace config_store {
esp_err_t read(const char* key,std::string& value){const auto found=records.find(key);if(found==records.end())return ESP_ERR_NVS_NOT_FOUND;value=found->second;return ESP_OK;}
esp_err_t write(const char* key,const std::string& value){++writes;if(during_write)during_write();if(write_result!=ESP_OK)return write_result;records[key]=value;return ESP_OK;}
}
const esp_partition_t* esp_partition_find_first(int,int,const char*){return nullptr;}
esp_err_t esp_partition_read(const esp_partition_t*,size_t,void*,size_t){assert(false);return ESP_FAIL;}
psa_status_t psa_crypto_init(){assert(false);return -1;}
psa_status_t psa_hash_compute(unsigned,const uint8_t*,size_t,uint8_t*,size_t,size_t*){assert(false);return -1;}
int main(){
    assert(device_settings::init()==ESP_OK&&std::string(device_settings::hostname())=="amity");
    records["hostname"]="amity-dut";assert(device_settings::init()==ESP_OK&&std::string(device_settings::hostname())=="amity-dut");
    // The existing Advanced setting retains its reboot-applied behavior.
    assert(device_settings::save_hostname("Living-Room")==ESP_OK&&records.at("hostname")=="living-room"&&std::string(device_settings::hostname())=="amity-dut");
    assert(device_settings::init()==ESP_OK&&std::string(device_settings::hostname())=="living-room");
    during_write=[]{assert(std::string(device_settings::hostname())=="living-room");};
    assert(device_settings::apply_enrollment_hostname("Amity-Den")==ESP_OK&&records.at("hostname")=="amity-den"&&std::string(device_settings::hostname())=="amity-den");
    during_write=[]{assert(std::string(device_settings::hostname())=="amity-den");};write_result=ESP_FAIL;
    assert(device_settings::apply_enrollment_hostname("Other-Room")==ESP_FAIL&&records.at("hostname")=="amity-den"&&std::string(device_settings::hostname())=="amity-den");
    during_write={};write_result=ESP_OK;const auto before=writes;
    for(const auto& invalid:std::vector<std::string>{"","-room","room-","living room","room.local","room_name",std::string(33,'x'),std::string("room\0hidden",11)}){
        assert(device_settings::apply_enrollment_hostname(invalid)==ESP_ERR_INVALID_ARG&&writes==before&&std::string(device_settings::hostname())=="amity-den"&&records.at("hostname")=="amity-den");
    }
    const std::string maximum(32,'x');assert(device_settings::apply_enrollment_hostname(maximum)==ESP_OK&&std::string(device_settings::hostname())==maximum&&records.at("hostname")==maximum);
    assert(device_settings::init()==ESP_OK&&std::string(device_settings::hostname())==maximum);
    records.erase("front_defaults");records["front_physical"]="3.0.0.0";
    assert(device_settings::init()==ESP_OK);
    auto defaults=device_settings::adapter_defaults();
    assert(defaults.revision==1&&defaults.front_physical[0]==0x3000);
    for(size_t n=1;n<5;++n)assert(defaults.front_physical[n]==0x1000);
    const auto initial_writes=writes;
    assert(device_settings::save_adapter_defaults(1,{{4,0x5200},{1,0x2100}})==ESP_OK&&writes==initial_writes+1);
    defaults=device_settings::adapter_defaults();assert(defaults.revision==2&&defaults.front_physical[0]==0x3000&&defaults.front_physical[1]==0x2100&&defaults.front_physical[4]==0x5200);
    assert(device_settings::startup_adapter_defaults().revision==1&&device_settings::startup_adapter_defaults().front_physical[4]==0x1000);
    assert(device_settings::front_physical_address()==0x3000);
    auto durable=records.at("front_defaults");const auto saved_writes=writes;
    assert(device_settings::save_adapter_defaults(1,{{0,0x4000}})==ESP_ERR_INVALID_STATE);
    for(const auto& invalid:std::vector<std::vector<device_settings::AdapterDefaultChange>>{{},{{5,0x1000}},{{1,0x1000},{1,0x2000}},{{2,0x1010}},{{2,0xffff}}})
        assert(device_settings::save_adapter_defaults(2,invalid)==ESP_ERR_INVALID_ARG);
    assert(writes==saved_writes&&records.at("front_defaults")==durable);
    write_result=ESP_FAIL;assert(device_settings::save_adapter_defaults(2,{{0,0x4000},{4,0x6000}})==ESP_FAIL);
    assert(device_settings::adapter_defaults().revision==2&&records.at("front_defaults")==durable);
    write_result=ESP_OK;
    // A reboot adopts the batch without compacting sparse slots. The legacy
    // scalar cannot override a newer record or leak into the other slots.
    records["front_physical"]="7.0.0.0";assert(device_settings::init()==ESP_OK);
    assert(device_settings::startup_adapter_defaults().front_physical==defaults.front_physical);
    assert(device_settings::save_adapter_defaults(2,{{3,0}})==ESP_OK);
    assert(device_settings::init()==ESP_OK&&device_settings::adapter_defaults().front_physical[3]==0);
    durable=records.at("front_defaults");records["front_defaults"]+="extra";
    assert(device_settings::init()==ESP_ERR_INVALID_STATE);records["front_defaults"]=durable;
    records["front_defaults"]="AMITYPA1\n18446744073709551615\n1.0.0.0\n1.0.0.0\n1.0.0.0\n1.0.0.0\n1.0.0.0\n";
    assert(device_settings::init()==ESP_OK&&device_settings::save_adapter_defaults(UINT64_MAX,{{0,0x2000}})==ESP_ERR_INVALID_STATE);
    std::cout<<"PASS actual enrollment hostname: normalized durable commit precedes live name, write failure preserves old name, invalid names make no writes, Advanced remains reboot-applied\n";
    std::cout<<"PASS stable adapter defaults: scalar migrates only slot0, atomic sparse batch, revision CAS/exhaustion, invalid/duplicate rejection, write failure, reboot-only application\n";
}
