#!/usr/bin/env python3
"""Exercise production chunk progress and durable OTA phase recovery together."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

from consortium_gatt_callback_test import function

ROOT = Path(__file__).resolve().parents[1]

PREFIX = r'''
#include "release_manifest.hpp"
#include "amity/consortium/json.hpp"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
using esp_err_t=int;
constexpr int ESP_OK=0,ESP_FAIL=1,ESP_ERR_INVALID_STATE=2,ESP_ERR_INVALID_ARG=3;
constexpr bool CONFIG_AMITY_PRODUCTION_UPDATES=false;
using esp_ota_handle_t=int;
struct esp_partition_t {};
struct App {uint8_t app_elf_sha256[32]{};const char* version="0.1.138";} app;
const App* esp_app_get_description(){return &app;}
int64_t clock_us=1000000;
int64_t esp_timer_get_time(){return clock_us;}
bool firmware_ready(){return true;}
std::string current_boot(32,'a');
const std::string& boot_id(){return current_boot;}
unsigned metadata_writes=0,flash_writes=0,flash_depth=0,maintenance=0;
std::map<std::string,std::string> records;
namespace config_store {
int write(const char* key,const std::string& value){++metadata_writes;records[key]=value;return ESP_OK;}
int read(const char* key,std::string& value){auto found=records.find(key);if(found==records.end())return ESP_FAIL;value=found->second;return ESP_OK;}
bool with_internal_flash_stack(const std::function<void()>& call){assert(!flash_depth);++flash_depth;call();--flash_depth;return true;}
}
const esp_partition_t* esp_ota_get_next_update_partition(void*){static esp_partition_t partition;return &partition;}
int erase_partition_release(const esp_partition_t*){return ESP_OK;}
int esp_ota_begin(const esp_partition_t*,uint64_t,esp_ota_handle_t* handle){assert(flash_depth);*handle=1;return ESP_OK;}
int esp_ota_write(esp_ota_handle_t handle,const void*,size_t){assert(flash_depth&&handle);++flash_writes;return ESP_OK;}
int esp_ota_abort(esp_ota_handle_t){assert(flash_depth);return ESP_OK;}
struct psa_hash_operation_t {};
constexpr int PSA_SUCCESS=0;
int psa_hash_update(psa_hash_operation_t*,const uint8_t*,size_t){return PSA_SUCCESS;}
int psa_hash_abort(psa_hash_operation_t*){return PSA_SUCCESS;}
namespace system_services::updates {
namespace c=amity::consortium;
std::mutex mutex,gate;
std::function<esp_err_t()> begin_maintenance=[](){++maintenance;return ESP_OK;};
std::function<void()> end_maintenance=[](){assert(maintenance);--maintenance;};
std::optional<ReleaseManifest> staged,release;
uint64_t deadline=0,received=0,offset=0;
std::string job,phase="idle",reason,old_boot,release_signature;
bool installing=false;
esp_ota_handle_t handle=0;
psa_hash_operation_t digest;
int64_t last_progress_us=0;
const esp_partition_t* receiving_partition=nullptr;
bool header(std::string_view,const ReleaseManifest&){return true;}
void install_failed(const std::string&);
void install_progress(uint64_t);
'''

SUITE = r'''
std::string field(const std::string& json,const char* name){c::Json value;c::JsonError error;assert(c::parse_json(json,value,error));const auto* item=value.get(name);assert(item&&item->type==c::Json::Type::string);return item->string;}
void clear_ram(){
 staged.reset();release.reset();job.clear();phase="idle";reason.clear();old_boot.clear();
 received=offset=deadline=0;handle=0;installing=false;
 receiving_partition=nullptr;maintenance=0;
}
void prepare(uint64_t total){
 clear_ram();records.clear();metadata_writes=flash_writes=0;
 ReleaseManifest manifest;manifest.release_id="release-one";manifest.image_bytes=total;
 manifest.image_sha256=std::string(64,'b');manifest.image_elf_sha256=std::string(64,'0');manifest.firmware_version=app.version;
 staged=release=manifest;job=std::string(32,'c');phase="staged";old_boot=current_boot;deadline=100000;
 std::string error;assert(begin_install(error)==ESP_OK&&metadata_writes==1);
 assert(field(records.at("ota_job"),"phase")=="installing");
}
void chunks(uint64_t count){
 std::string error;while(offset<count){
  const auto size=size_t(std::min<uint64_t>(2048,release->image_bytes-offset));
  const auto before=offset;const std::string bytes(size,char('d'+before%17));
  const unsigned writes_before=flash_writes;
  assert(peer_chunk(release->release_id,before,bytes,error)==ESP_OK);
  assert(flash_writes==writes_before+1&&received==offset&&metadata_writes==1);
  assert(field(status_json(),"received_bytes")==std::to_string(offset));
  // The ordered stream rejects repeated chunks without another flash write.
  assert(peer_chunk(release->release_id,before,bytes,error)==ESP_ERR_INVALID_ARG&&flash_writes==writes_before+1&&metadata_writes==1);
 }
}
void lifecycle_writes_are_bounded(){
 constexpr uint64_t image_bytes=2482272;
 prepare(image_bytes);chunks(image_bytes);
 assert(flash_writes==1213&&metadata_writes==1&&field(status_json(),"received_bytes")==std::to_string(image_bytes));
 assert(install_staged()==ESP_OK&&metadata_writes==2&&!installing&&maintenance==0);
 assert(field(records.at("ota_job"),"phase")=="staged_image"&&field(records.at("ota_job"),"received_bytes")==std::to_string(image_bytes));
 clear_ram();current_boot=std::string(32,'f');
 assert(field(status_json(),"phase")=="staged_image"&&metadata_writes==2);
 assert(staged&&staged->release_id=="release-one");
 // Staging and boot-selection markers remain durable independently of chunk count.
 assert(install_selected()==ESP_OK&&metadata_writes==3&&field(records.at("ota_job"),"phase")=="boot_checking");
 std::cout<<"PASS 1213 physical chunk writes use one receiving marker; staged and boot-selection phases remain durable\n";
}
void failure_remains_durable(){
 prepare(8192);chunks(2048);install_failed("flash rejected");
 assert(metadata_writes==2&&field(records.at("ota_job"),"phase")=="failed");
 clear_ram();assert(field(status_json(),"phase")=="failed"&&metadata_writes==2);
 std::cout<<"PASS failed receiving phase remains durable without per-chunk checkpoints\n";
}
} // namespace system_services::updates
int main(){using namespace system_services::updates;lifecycle_writes_are_bounded();failure_remains_durable();assert(flash_depth==0);}
'''


def main():
    report_source = (ROOT / 'components/system_services/firmware_update.cpp').read_text()
    report = ''.join(function(report_source, declaration) for declaration in (
        'std::string quote(std::string_view value)',
        'std::string hex(const uint8_t* bytes,size_t count)',
        'uint64_t now()', 'bool production()', 'std::string image_identity()',
        'std::string report()', 'esp_err_t persist()', 'void recover()',
        'std::string status_json()', 'esp_err_t begin_install(std::string& error)',
        'void install_progress(uint64_t bytes)', 'esp_err_t install_selected()',
        'esp_err_t install_staged()', 'void install_failed(const std::string& error)'))
    receiver_source = (ROOT / 'components/system_services/peer_firmware_update.cpp').read_text()
    receiver = ''.join(function(receiver_source, declaration) for declaration in (
        'esp_err_t flash(const std::function<esp_err_t()>& call)',
        'void abort_locked(const std::string& error)',
        'esp_err_t peer_chunk(std::string_view release_id,uint64_t requested,std::string_view bytes,std::string& error)'))
    with tempfile.TemporaryDirectory(prefix='amity-firmware-progress-') as temporary:
        base = Path(temporary)
        cpp = base / 'test.cpp'
        cpp.write_text(PREFIX + report + receiver + SUITE)
        binary = base / 'test'
        subprocess.run(shlex.split(os.environ.get('CXX', 'c++')) + [
            '-std=c++17', '-Wall', '-Wextra', '-Werror', '-g', '-fsanitize=address,undefined',
            '-I' + str(ROOT / 'components/system_services/include'),
            '-I' + str(ROOT / 'components/consortium/include'),
            str(cpp), str(ROOT / 'components/consortium/json.cpp'), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
