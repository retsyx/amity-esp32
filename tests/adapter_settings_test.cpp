#include "adapter_settings.hpp"
#include "amity/system_control.hpp"
#include "amity/consortium/settings.hpp"
#include "device_settings.hpp"
#include "config_store.hpp"
#include "esp_partition.h"
#include "psa/crypto.h"
#include <cassert>
#include <iostream>
#include <map>

namespace c=amity::consortium;namespace m=amity::system;namespace a=system_services::adapter_settings;
namespace {
std::map<std::string,std::string> records;uint64_t clock_ms=100;unsigned writes=0,restarts=0,calls=0;
bool authorized=true;esp_err_t write_result=ESP_OK;
template<class T>T id(unsigned n){char bytes[33];snprintf(bytes,sizeof(bytes),"%032x",n);return *T::parse(bytes);}
c::Json json(const std::string& bytes){c::Json out;c::JsonError detail;assert(c::parse_json(bytes,out,detail));return out;}
std::string request(m::BootId boot,uint64_t revision=1,std::string changes="{\"tentacle_id\":3,\"default_front_physical_address\":\"5.2.0.0\"}"){
    return "{\"expected_boot_id\":\""+boot.str()+"\",\"expected_settings_revision\":\""+std::to_string(revision)+"\",\"changes\":["+changes+"]}";
}
}
int64_t esp_timer_get_time(){return static_cast<int64_t>(clock_ms*1000);}
namespace config_store {
esp_err_t read(const char* key,std::string& value){const auto found=records.find(key);if(found==records.end())return ESP_ERR_NVS_NOT_FOUND;value=found->second;return ESP_OK;}
esp_err_t write(const char* key,const std::string& value){++writes;if(write_result==ESP_OK)records[key]=value;return write_result;}
}
namespace amity::consortium {bool runtime_device_settings_authorized(m::MessageId){return authorized;}}
namespace system_services {void reboot_later(){++restarts;}}
const esp_partition_t* esp_partition_find_first(int,int,const char*){return nullptr;}
esp_err_t esp_partition_read(const esp_partition_t*,size_t,void*,size_t){assert(false);return ESP_FAIL;}
psa_status_t psa_crypto_init(){assert(false);return -1;}
psa_status_t psa_hash_compute(unsigned,const uint8_t*,size_t,uint8_t*,size_t,size_t*){assert(false);return -1;}
int main(){
    records["front_physical"]="3.0.0.0";assert(device_settings::init()==ESP_OK);
    amity::SystemControl owner;owner.boot=id<m::BootId>(1);
    owner.rows={{0,true,0x2300,0x2300},{1,false,std::nullopt,std::nullopt},{2,true,std::nullopt,std::nullopt},{3,true,std::nullopt,0x1000,true}};
    owner.gate=[&](m::BootId boot,std::vector<amity::AdapterAddressChange> adapters,uint64_t deadline,std::function<esp_err_t()> persist){
        ++calls;assert(boot==owner.boot&&adapters.size()==1&&adapters[0].tentacle_id==3&&adapters[0].default_physical==0x5200&&deadline==1000);return persist();
    };
    a::install(owner);const auto before=json(a::snapshot());assert(c::valid_settings_snapshot(before));const auto* rows=before.get("adapters");
    assert(rows&&rows->array.size()==4&&rows->array[0].get("observed_front_physical_address")->string=="2.3.0.0"&&rows->array[0].get("default_front_physical_address")->string=="3.0.0.0");
    assert(!rows->array[1].get("configured")->boolean&&rows->array[1].get("effective_front_physical_address")->type==c::Json::Type::null_value);
    assert(rows->array[2].get("configured")->boolean&&rows->array[3].get("name")->string=="HDMI adapter 4");
    std::string response;assert(!restarts);
    unsigned serial=10;
    const auto submit=[&](std::string body,uint64_t deadline=1000){return a::apply(id<m::MessageId>(serial++),json(body),deadline,response);};
    auto bad=request(owner.boot);bad.insert(bad.size()-1,",\"confirm_restart\":true");assert(submit(bad)==c::Error::invalid_message);
    bad=request(owner.boot);bad.insert(bad.size()-1,",\"extra\":true");assert(submit(bad)==c::Error::invalid_message);
    const std::string duplicate="{\"tentacle_id\":3,\"default_front_physical_address\":\"1.0.0.0\"}";
    assert(submit(request(owner.boot,1,duplicate+","+duplicate))==c::Error::invalid_message);
    for(auto changes:{"","{\"tentacle_id\":4,\"default_front_physical_address\":\"1.0.0.0\"}","{\"tentacle_id\":3,\"extra\":true,\"default_front_physical_address\":\"1.0.0.0\"}","{\"tentacle_id\":3,\"default_front_physical_address\":\"1.0.1.0\"}"})
        assert(submit(request(owner.boot,1,changes))==c::Error::invalid_message);
    assert(submit(request(id<m::BootId>(2)))==c::Error::stale_boot);
    assert(submit(request(owner.boot,2))==c::Error::transaction_conflict);
    assert(submit(request(owner.boot),100)==c::Error::expired&&calls==0&&writes==0);
    authorized=false;assert(submit(request(owner.boot))==c::Error::not_ready&&calls==0);authorized=true;
    owner.gate=[&](auto,auto,uint64_t,std::function<esp_err_t()> persist){++calls;clock_ms=1000;return persist();};
    assert(submit(request(owner.boot))==c::Error::expired&&writes==0);clock_ms=100;
    owner.gate=[&](auto,auto,uint64_t,std::function<esp_err_t()> persist){++calls;authorized=false;return persist();};
    assert(submit(request(owner.boot))==c::Error::not_ready&&writes==0);authorized=true;
    // Native owner revalidation still fences boot, configured adapter IDs and
    // quiescence before invoking persistence. The service cannot bypass it.
    owner.gate=[&](auto,auto,uint64_t,std::function<esp_err_t()>){++calls;return ESP_ERR_INVALID_STATE;};
    assert(submit(request(owner.boot))==c::Error::not_ready&&writes==0);
    owner.gate=[&](auto,auto,uint64_t,std::function<esp_err_t()> persist){++calls;return persist();};
    write_result=ESP_FAIL;assert(submit(request(owner.boot))==c::Error::storage_failure&&writes==1&&device_settings::adapter_defaults().revision==1);write_result=ESP_OK;
    const auto successful=id<m::MessageId>(serial++);const auto body=json(request(owner.boot));
    assert(a::apply(successful,body,1000,response)==c::Error::none&&writes==2&&device_settings::adapter_defaults().revision==2);
    assert(device_settings::adapter_defaults().front_physical[3]==0x5200&&device_settings::adapter_defaults().front_physical[0]==0x3000);
    const auto applied=response;const auto finished_calls=calls;clock_ms=1100;
    assert(a::apply(successful,body,1000,response)==c::Error::none&&response==applied&&calls==finished_calls&&writes==2);
    assert(a::apply(successful,json(request(owner.boot,2)),2000,response)==c::Error::transaction_conflict);
    assert(!restarts);
    // A fresh save is available immediately on the same owner boot.
    assert(submit(request(owner.boot,2),2000)==c::Error::none&&writes==3&&device_settings::adapter_defaults().revision==3);
    // Old same-boot revisions and old-boot approvals remain invalid after the
    // bounded receipt cache is gone. Replaying 100 approvals cannot resave.
    a::install(owner);for(unsigned n=0;n<100;++n)assert(submit(request(owner.boot),2000)==c::Error::transaction_conflict);
    owner.boot=id<m::BootId>(3);a::install(owner);
    for(unsigned n=0;n<100;++n)assert(submit(request(id<m::BootId>(1),3),2000)==c::Error::stale_boot);
    assert(writes==3&&!restarts);
    assert(submit(request(owner.boot,3,"{\"tentacle_id\":3,\"default_front_physical_address\":\"\"}"),2000)==c::Error::none);
    assert(writes==4&&device_settings::adapter_defaults().front_physical[3]==0x1000&&c::valid_settings_snapshot(json(a::snapshot())));
    std::cout<<"PASS actual adapter settings service: stable snapshot/provenance, strict parser, original deadline and authority recheck, owner gate, atomic save, idempotence, stale-cache/boot replay, same-boot consecutive saves without restart\n";
}
