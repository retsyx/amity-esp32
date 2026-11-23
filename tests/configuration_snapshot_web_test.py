#!/usr/bin/env python3
"""Exercise deferred configuration activation once an updated image is healthy."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

from slice_support import function

ROOT = Path(__file__).resolve().parents[1]

PREFIX = r'''
#include "intent_configuration_fixture.hpp"
#include <functional>
#include <iostream>
#include <memory>
#include <utility>
constexpr esp_err_t ESP_ERR_NVS_NOT_FOUND=0x1102;
namespace fixture {
Memory memory;
std::unique_ptr<ConfigurationStore> journal;
ConfigurationSnapshot saved,staged,loaded;
unsigned loads=0,stages=0,commits=0,serializations=0;
esp_err_t read_error=ESP_OK,stage_error=ESP_OK,commit_error=ESP_OK;
bool healthy=true,pending_boot=false;
std::vector<std::string> events;
}
namespace config_store {
std::string actual_serialize_configuration(const Configuration&,bool);
std::string serialize_configuration(const Configuration& value,bool executable){++fixture::serializations;return actual_serialize_configuration(value,executable);}
esp_err_t load_system_configuration(ConfigurationSnapshot& out,std::string&){
 ++fixture::loads;if(fixture::read_error!=ESP_OK)return fixture::read_error;
 out=fixture::loaded=fixture::saved;return out?ESP_OK:ESP_ERR_NVS_NOT_FOUND;
}
esp_err_t stage_system_configuration(Configuration value,RevisionPair expected,std::string& error){
 ++fixture::stages;if(fixture::stage_error!=ESP_OK)return fixture::stage_error;
 const auto result=fixture::journal->stage(std::move(value),expected,error);
 fixture::staged=fixture::journal->staged();return result==ConfigResult::ok?ESP_OK:ESP_FAIL;
}
esp_err_t staged_system_configuration(ConfigurationSnapshot& out){out=fixture::journal->staged();return out?ESP_OK:ESP_ERR_INVALID_STATE;}
esp_err_t activate_system_configuration(ConfigurationSnapshot& out,std::string& error){
 ++fixture::commits;fixture::events.push_back("commit");if(fixture::commit_error!=ESP_OK)return fixture::commit_error;
 if(fixture::journal->commit(out,error)!=ConfigResult::ok)return ESP_FAIL;
 assert(out==fixture::staged);fixture::saved=out;return ESP_OK;
}
}
namespace amity {
class SystemControl {
public:
 config_store::ConfigurationSnapshot published;
 uint8_t capacity=1;bool authority=true;
 esp_err_t configure_error=ESP_OK,passive_error=ESP_OK,authority_error=ESP_OK;
 system::OctopusId octopus_id()const{return id<system::OctopusId>(2);}
 system::ConsortiumId consortium_id()const{return id<system::ConsortiumId>(1);}
 uint8_t supported_tentacles()const{return capacity;}
 config_store::ConfigurationSnapshot configuration_snapshot()const{return published;}
 std::shared_ptr<const config_store::Configuration> active_configuration()const{
  return published?std::shared_ptr<const config_store::Configuration>(published,&published->configuration):nullptr;
 }
 esp_err_t configure(config_store::ConfigurationSnapshot candidate,std::function<esp_err_t()> commit={}){
  fixture::events.push_back("reserve");if(configure_error!=ESP_OK)return configure_error;
  const auto previous=published;const auto result=commit?commit():ESP_OK;
  assert(published==previous);if(result!=ESP_OK)return result;
  assert(candidate==fixture::saved);published=std::move(candidate);fixture::events.push_back("publish");return ESP_OK;
 }
 esp_err_t configure_passive_metadata(config_store::ConfigurationSnapshot candidate){
  assert(!authority);if(passive_error!=ESP_OK)return passive_error;
  assert(candidate==fixture::staged);published=std::move(candidate);fixture::events.push_back("passive");return ESP_OK;
 }
 esp_err_t transition_coordinator_authority(bool on){
  if(authority_error!=ESP_OK)return authority_error;
  authority=on;fixture::events.push_back(on?"enable":"disable");return ESP_OK;
 }
};
}
namespace system_services {bool firmware_ready(){return fixture::healthy;}}
enum esp_ota_img_states_t {ESP_OTA_IMG_VALID,ESP_OTA_IMG_PENDING_VERIFY};
const int* esp_ota_get_running_partition(){static int partition=1;return &partition;}
esp_err_t esp_ota_get_state_partition(const int*,esp_ota_img_states_t* state){*state=fixture::pending_boot?ESP_OTA_IMG_PENDING_VERIFY:ESP_OTA_IMG_VALID;return ESP_OK;}
namespace web_admin {
amity::SystemControl* control=nullptr;
bool configuration_passive=false;
config_store::ConfigurationSnapshot deferred_configuration;
'''

SUITE = r'''
}
using namespace web_admin;
void reset(bool existing=true){
 control=nullptr;deferred_configuration.reset();configuration_passive=false;
 fixture::saved.reset();fixture::staged.reset();fixture::loaded.reset();fixture::journal.reset();fixture::memory=Memory{};
 fixture::journal=std::make_unique<ConfigurationStore>(fixture::memory);
 if(existing){std::string error;assert(fixture::journal->stage(direct_configuration(),{},error)==ConfigResult::ok);assert(fixture::journal->commit(fixture::saved,error)==ConfigResult::ok);}
 fixture::loads=fixture::stages=fixture::commits=fixture::serializations=0;
 fixture::read_error=fixture::stage_error=fixture::commit_error=ESP_OK;
 fixture::healthy=true;fixture::pending_boot=false;fixture::events.clear();
}
void ota_health_defers_the_same_candidate(){
 reset();amity::SystemControl service;service.capacity=4;const auto old=fixture::saved;
 fixture::pending_boot=true;fixture::healthy=false;
 assert(load_configuration(service,false)==ESP_OK&&fixture::stages==1&&!fixture::commits);
 const auto candidate=fixture::staged;
 assert(configuration_passive&&!service.authority&&deferred_configuration==candidate&&service.published==candidate&&fixture::saved==old);
 assert(finish_configuration_activation()==ESP_ERR_INVALID_STATE&&!fixture::commits&&fixture::saved==old);
 fixture::healthy=true;fixture::commit_error=ESP_FAIL;
 assert(finish_configuration_activation()==ESP_FAIL&&fixture::commits==1&&fixture::saved==old);
 assert(configuration_passive&&!service.authority&&deferred_configuration==candidate&&service.published==candidate);
 fixture::commit_error=ESP_OK;
 assert(finish_configuration_activation()==ESP_OK&&fixture::commits==2&&fixture::saved==candidate&&service.published==candidate);
 assert(!configuration_passive&&service.authority&&!deferred_configuration);
 assert(old->configuration.octopuses.front().supported_tentacles==1);
 assert(finish_configuration_activation()==ESP_OK&&fixture::commits==2);
}
int main(){ota_health_defers_the_same_candidate();std::cout<<"PASS deferred configuration activation: refused while unhealthy, retained after a failed commit, activated once when healthy\n";}
'''


def main():
    source = (ROOT / 'components/web_admin/system_admin.cpp').read_text()
    actual = ''.join(function(source, signature) for signature in (
        'esp_err_t load_configuration(', 'esp_err_t finish_configuration_activation()'))
    cjson = ROOT / 'managed_components/espressif__cjson/cJSON'
    if not (cjson / 'cJSON.c').exists():
        cjson = cjson.parent
    openssl = Path('/opt/homebrew/opt/openssl@3')
    openssl_include = ['-I' + str(openssl / 'include')] if openssl.is_dir() else []
    openssl_lib = ['-L' + str(openssl / 'lib')] if openssl.is_dir() else []
    with tempfile.TemporaryDirectory(prefix='amity-config-web-') as directory:
        temp = Path(directory)
        (temp / 'test.cpp').write_text(PREFIX + actual + SUITE)
        includes = ['-I' + str(cjson)] + ['-I' + str(ROOT / name) for name in (
            'tests', 'tests/reset_fakes', 'components/config_store/include', 'components/amity_system/include')]
        flags = ['-std=c++17', '-Wall', '-Wextra', '-Werror', '-g', '-fsanitize=address,undefined']
        cxx = shlex.split(os.environ.get('CXX', 'c++'))
        subprocess.run(['cc', '-g', '-Wno-deprecated-declarations', '-fsanitize=address,undefined', '-I' + str(cjson),
                        '-c', str(cjson / 'cJSON.c'), '-o', str(temp / 'cjson.o')], check=True)
        subprocess.run(cxx + flags + openssl_include + includes + ['-Dserialize_configuration=actual_serialize_configuration',
            '-c', str(ROOT / 'components/config_store/system_configuration.cpp'), '-o', str(temp / 'configuration.o')], check=True)
        subprocess.run(cxx + flags + openssl_include + includes + [str(temp / 'test.cpp'),
            str(ROOT / 'components/config_store/configuration_journal.cpp'), str(ROOT / 'components/amity_system/model.cpp'),
            str(temp / 'configuration.o'), str(temp / 'cjson.o')] + openssl_lib + ['-lcrypto', '-o', str(temp / 'test')], check=True)
        subprocess.run([str(temp / 'test')], check=True)


if __name__ == '__main__':
    main()
