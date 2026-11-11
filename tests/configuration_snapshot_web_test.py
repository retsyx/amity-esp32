#!/usr/bin/env python3
"""Exercise web startup, OTA deferral and reads with shared real configuration snapshots."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

from consortium_gatt_callback_test import function

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
unsigned loads=0,stages=0,commits=0,serializations=0,input_publications=0;
esp_err_t read_error=ESP_OK,stage_error=ESP_OK,commit_error=ESP_OK,startup_error=ESP_OK;
bool healthy=true,pending_boot=false;
std::vector<std::string> events,input_names;
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
 esp_err_t transition_coordinator_authority(bool on,uint64_t epoch){
  assert(epoch==1);if(authority_error!=ESP_OK)return authority_error;
  authority=on;fixture::events.push_back(on?"enable":"disable");return ESP_OK;
 }
};
namespace consortium {
struct RuntimeStartup {system::OctopusId authority_id=id<system::OctopusId>(2);uint64_t epoch=1;bool is_passive=false;bool passive()const{return is_passive;}};
RuntimeStartup startup;
esp_err_t runtime_read_startup(system::OctopusId,RuntimeStartup& out){out=startup;return fixture::startup_error;}
struct RuntimeStatus {bool coordinator=true,authority_ready=true;};
RuntimeStatus runtime_status(){return {};}
}
}
namespace system_services {bool firmware_ready(){return fixture::healthy;}}
void esp_fill_random(void* bytes,size_t size){static auto random=random_source();assert(random(static_cast<uint8_t*>(bytes),size));}
enum esp_ota_img_states_t {ESP_OTA_IMG_VALID,ESP_OTA_IMG_PENDING_VERIFY};
const int* esp_ota_get_running_partition(){static int partition=1;return &partition;}
esp_err_t esp_ota_get_state_partition(const int*,esp_ota_img_states_t* state){*state=fixture::pending_boot?ESP_OTA_IMG_PENDING_VERIFY:ESP_OTA_IMG_VALID;return ESP_OK;}
struct amity_matter_input_t {unsigned index=0;char name[129]{};};
void amity_matter_set_inputs(const amity_matter_input_t* inputs,size_t count){
 ++fixture::input_publications;fixture::input_names.clear();for(size_t n=0;n<count;++n)fixture::input_names.emplace_back(inputs[n].name);
}
struct httpd_req_t {std::string response,disposition;std::function<void()> during_send;};
esp_err_t httpd_resp_set_hdr(httpd_req_t* req,const char* name,const char* value){assert(std::string(name)=="Content-Disposition");req->disposition=value;return ESP_OK;}
namespace web {esp_err_t json(httpd_req_t* req,const std::string& bytes){if(req->during_send)req->during_send();req->response=bytes;return ESP_OK;}}
namespace web_admin {
amity::SystemControl* control=nullptr;
config_store::RevisionPair published_input_revisions;
bool configuration_passive=false;
config_store::ConfigurationSnapshot deferred_configuration;
uint64_t deferred_epoch=1;
esp_err_t error(httpd_req_t* req,const char*,const std::string& message,const char*){req->response=message;return ESP_FAIL;}
'''

SUITE = r'''
}
using namespace web_admin;
void reset(bool existing=true){
 control=nullptr;deferred_configuration.reset();configuration_passive=false;published_input_revisions={};deferred_epoch=1;
 fixture::saved.reset();fixture::staged.reset();fixture::loaded.reset();fixture::journal.reset();fixture::memory=Memory{};
 fixture::journal=std::make_unique<ConfigurationStore>(fixture::memory);
 if(existing){std::string error;assert(fixture::journal->stage(direct_configuration(),{},error)==ConfigResult::ok);assert(fixture::journal->commit(fixture::saved,error)==ConfigResult::ok);}
 fixture::loads=fixture::stages=fixture::commits=fixture::serializations=fixture::input_publications=0;
 fixture::read_error=fixture::stage_error=fixture::commit_error=fixture::startup_error=ESP_OK;
 fixture::healthy=true;fixture::pending_boot=false;fixture::events.clear();fixture::input_names.clear();amity::consortium::startup={};
}
void retained_configuration_is_shared(){
 for(bool pending:{false,true}){
  reset();fixture::pending_boot=pending;amity::SystemControl service;const auto current=fixture::saved;
  assert(load_configuration(service,false)==ESP_OK&&fixture::loads==1&&!fixture::stages&&!fixture::commits);
  assert(service.published==current&&fixture::loaded==current&&!deferred_configuration&&!configuration_passive);
  assert(service.active_configuration().get()==&current->configuration&&fixture::serializations==0);
  assert(fixture::input_publications==1&&fixture::input_names==std::vector<std::string>{"Watch"});
 }
}
void changed_hardware_commits_before_publication(){
 reset();amity::SystemControl service;service.capacity=4;const auto old=fixture::saved;
 assert(load_configuration(service,false)==ESP_OK&&fixture::stages==1&&fixture::commits==1);
 assert((fixture::events==std::vector<std::string>{"reserve","commit","publish"}));
 assert(service.published==fixture::staged&&service.published==fixture::saved&&service.published!=old);
 assert(old->configuration.octopuses.front().supported_tentacles==1&&service.published->configuration.octopuses.front().supported_tentacles==4);
 assert(fixture::input_publications==1&&!deferred_configuration);
}
void initial_configuration_uses_the_staged_snapshot(){
 reset(false);amity::SystemControl service;
 assert(load_configuration(service,false)==ESP_OK&&fixture::stages==1&&fixture::commits==1);
 assert(service.published&&service.published==fixture::saved&&service.published==fixture::staged);
 assert(service.published->configuration.controller_octopus_id==service.octopus_id()&&fixture::input_publications==1&&fixture::input_names.empty());
}
void ota_health_defers_the_same_candidate(){
 reset();amity::SystemControl service;service.capacity=4;const auto old=fixture::saved;
 fixture::pending_boot=true;fixture::healthy=false;
 assert(load_configuration(service,false)==ESP_OK&&fixture::stages==1&&!fixture::commits&&!fixture::input_publications);
 const auto candidate=fixture::staged;
 assert(configuration_passive&&!service.authority&&deferred_configuration==candidate&&service.published==candidate&&fixture::saved==old);
 assert(finish_configuration_activation()==ESP_ERR_INVALID_STATE&&!fixture::commits&&fixture::saved==old);
 fixture::healthy=true;fixture::commit_error=ESP_FAIL;
 assert(finish_configuration_activation()==ESP_FAIL&&fixture::commits==1&&fixture::saved==old);
 assert(configuration_passive&&!service.authority&&deferred_configuration==candidate&&service.published==candidate&&!fixture::input_publications);
 fixture::commit_error=ESP_OK;
 assert(finish_configuration_activation()==ESP_OK&&fixture::commits==2&&fixture::saved==candidate&&service.published==candidate);
 assert(!configuration_passive&&service.authority&&!deferred_configuration&&fixture::input_publications==1);
 assert(old->configuration.octopuses.front().supported_tentacles==1);
 assert(finish_configuration_activation()==ESP_OK&&fixture::commits==2&&fixture::input_publications==1);
}
void failures_preserve_the_previous_snapshot(){
 for(bool fail_reservation:{false,true}){
  reset();amity::SystemControl service;const auto old=fixture::saved;service.published=old;service.capacity=4;
  if(fail_reservation)service.configure_error=ESP_FAIL;else fixture::commit_error=ESP_FAIL;
  assert(load_configuration(service,false)==ESP_FAIL&&service.published==old&&fixture::saved==old);
  assert(fixture::commits==(fail_reservation?0u:1u)&&!fixture::input_publications);
 }
 reset();amity::SystemControl service;fixture::read_error=ESP_FAIL;
 assert(load_configuration(service,false)==ESP_FAIL&&!service.published&&!fixture::stages&&!fixture::commits);
 reset();amity::consortium::startup.is_passive=true;
 assert(load_configuration(service,false)==ESP_ERR_INVALID_STATE&&!fixture::loads);
 reset();assert(load_configuration(service,true)==ESP_OK&&configuration_passive&&!fixture::loads&&!service.published);
}
void reads_pin_one_revision_and_reuse_canonical_bytes(){
 reset();amity::SystemControl service;assert(load_configuration(service,false)==ESP_OK);
 auto replacement=fixture::saved->configuration;replacement.activities[0].label="Renamed";++replacement.presentation_revision;
 std::string error;const auto next=prepare_configuration(std::move(replacement),error);assert(next);
 auto previous=service.published;const auto bytes=previous->canonical;std::weak_ptr<const PreparedConfiguration> retained=previous;
 fixture::serializations=0;
 httpd_req_t req;esp_err_t result=ESP_FAIL;
 req.during_send=[&]{service.published=next;fixture::saved.reset();fixture::loaded.reset();fixture::journal.reset();previous.reset();assert(!retained.expired());};
 assert(read_configuration(&req,"/api/config",result)&&result==ESP_OK&&req.response==bytes&&fixture::serializations==0&&retained.expired());
 req={};assert(read_configuration(&req,"/api/backup",result)&&result==ESP_OK&&req.response==next->canonical&&fixture::serializations==0);
 assert(req.disposition=="attachment; filename=amity-config.json"&&fixture::input_names==std::vector<std::string>{"Renamed"});
 const auto publications=fixture::input_publications;
 assert(read_configuration(&req,"/api/config",result)&&fixture::input_publications==publications);
 service.published.reset();req={};assert(read_configuration(&req,"/api/config",result)&&result==ESP_FAIL);
}
int main(){retained_configuration_is_shared();changed_hardware_commits_before_publication();initial_configuration_uses_the_staged_snapshot();ota_health_defers_the_same_candidate();failures_preserve_the_previous_snapshot();reads_pin_one_revision_and_reuse_canonical_bytes();std::cout<<"PASS web configuration snapshots: shared startup/staging, OTA health deferral and retry, commit-before-publication, unchanged old readers, pinned canonical API/backup reads and revision-only input publication\n";}
'''


def main():
    source = (ROOT / 'components/web_admin/system_admin.cpp').read_text()
    actual = ''.join(function(source, signature) for signature in (
        'void publish_inputs()', 'bool configuration_is_passive()',
        'esp_err_t load_configuration(', 'esp_err_t finish_configuration_activation()'))
    router = source[source.index('bool admin_request('):]
    # Exercise the real snapshot capture and read branches without unrelated UI routes.
    capture = router[router.index('    const auto published='):router.index('    if(req->method==HTTP_GET)')]
    config = function(router, '        if(path=="/api/config")')
    backup = function(router, '        if(path=="/api/backup")')
    actual += '\nbool read_configuration(httpd_req_t* req,const std::string& path,esp_err_t& result){\n' + capture + config + backup + '\nreturn false;\n}\n'
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
