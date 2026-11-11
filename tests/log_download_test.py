#!/usr/bin/env python3
"""Run actual PSRAM snapshot and streamed download against bounded device fakes."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

from consortium_gatt_callback_test import function

ROOT = Path(__file__).resolve().parents[1]
HEADER = r'''
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
using esp_err_t=int;
constexpr int ESP_OK=0,ESP_FAIL=-1,ESP_ERR_NO_MEM=1,ESP_ERR_TIMEOUT=2,ESP_ERR_INVALID_STATE=3;
struct httpd_req_t {std::string body;unsigned chunks=0,ended=0,fail_at=0;bool busy=false;};
int httpd_resp_set_type(httpd_req_t*,const char*){return ESP_OK;}
int httpd_resp_set_hdr(httpd_req_t*,const char*,const char*){return ESP_OK;}
int httpd_resp_send_chunk(httpd_req_t*,const char*,size_t);
namespace amity::system {
struct OctopusId {unsigned value=0;std::string str()const{return "device-"+std::to_string(value);}explicit operator bool()const{return value!=0;}};
inline bool operator==(OctopusId a,OctopusId b){return a.value==b.value;}
using BootId=OctopusId;
}
namespace amity::consortium {
constexpr size_t log_capacity=24576,log_chunk_bytes=1024;
struct LogSnapshot {std::shared_ptr<const char> bytes;size_t size=0;explicit operator bool()const{return bool(bytes);}};
struct RuntimeStatus {bool coordinator=false;};
struct RuntimePeerStatus {amity::system::OctopusId id;amity::system::BootId boot_id;std::string hostname;bool enrolled=true,provisional=false;};
RuntimeStatus runtime_status();
std::vector<RuntimePeerStatus> runtime_peers();
int runtime_pull_logs(amity::system::OctopusId,const std::function<bool(std::string_view)>&);
}
namespace device_settings {const char* hostname(){return "living-room";}}
namespace web {int error(httpd_req_t* req,const char*,const char*){req->busy=true;return ESP_FAIL;}}
namespace system_services {
amity::consortium::LogSnapshot log_snapshot();
int download_logs(httpd_req_t*,amity::system::OctopusId);
const char* boot_id(){return "boot-local";}
}
'''
PREFIX = r'''
#include "system_services.hpp"
constexpr unsigned MALLOC_CAP_SPIRAM=1,MALLOC_CAP_8BIT=2,portMAX_DELAY=0;
bool allocation_fails=false,ring_locked=false;
unsigned psram_live=0,psram_max=0,allocations=0,frees=0;
void* heap_caps_malloc(size_t size,unsigned caps){
 assert(!ring_locked&&size==amity::consortium::log_capacity&&caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
 if(allocation_fails)return nullptr;
 ++allocations;psram_live+=size;psram_max=std::max(psram_max,psram_live);return std::malloc(size);
}
void heap_caps_free(void* data){psram_live-=amity::consortium::log_capacity;++frees;std::free(data);}
void xSemaphoreTake(void*,unsigned){assert(!ring_locked);ring_locked=true;}
void xSemaphoreGive(void*){assert(ring_locked);ring_locked=false;}
namespace system_services {
std::array<char,amity::consortium::log_capacity> ring{};
size_t cursor=0,used=0;void* lock=&ring;
void fill(std::string_view text){assert(text.size()<=ring.size());used=text.size();cursor=7;for(size_t i=0;i<used;++i)ring[(cursor+ring.size()-used+i)%ring.size()]=text[i];}
}
'''
SUITE = r'''
#include <iostream>
bool main_device=true,nested=false,mutate=false;
unsigned pulls=0,peer_lists=0;
std::vector<unsigned> owners;
int httpd_resp_send_chunk(httpd_req_t* req,const char* bytes,size_t size){
 if(!size){++req->ended;return ESP_OK;}
 ++req->chunks;
 if(nested){nested=false;httpd_req_t other;assert(system_services::download_logs(&other,{1})==ESP_FAIL&&other.busy&&other.body.empty());}
 if(req->fail_at==req->chunks)return ESP_FAIL;
 req->body.append(bytes,size);
 if(mutate&&size==1024&&bytes[0]=='L'){mutate=false;system_services::fill("changed ring");}
 return ESP_OK;
}
namespace amity::consortium {
RuntimeStatus runtime_status(){return {main_device};}
std::vector<RuntimePeerStatus> runtime_peers(){++peer_lists;return {{{2},{12},"bedroom"},{{3},{13},"office"},{{4},{},"offline"},{{5},{},"candidate",false,true}};}
int runtime_pull_logs(amity::system::OctopusId owner,const std::function<bool(std::string_view)>& consume){
 assert(psram_live==0);++pulls;owners.push_back(owner.value);
 if(owner.value==4)return ESP_ERR_INVALID_STATE;
 if(owner.value==3)return consume("partial-secondary\n")?ESP_ERR_TIMEOUT:ESP_FAIL;
 std::array<char,log_chunk_bytes> bytes;bytes.fill('A');
 for(size_t i=0;i<log_capacity;i+=bytes.size())if(!consume({bytes.data(),bytes.size()}))return ESP_FAIL;
 return ESP_OK;
}
}
void reset(){assert(psram_live==0);main_device=true;nested=mutate=allocation_fails=false;pulls=peer_lists=0;owners.clear();system_services::fill("");}
int main(){
 reset();system_services::fill("old\nnew\n");
 {const auto snap=system_services::log_snapshot();assert(snap&&snap.size==8);system_services::fill("changed");assert(std::string(snap.bytes.get(),snap.size)=="old\nnew\n");}
 assert(psram_live==0);allocation_fails=true;assert(!system_services::log_snapshot());allocation_fails=false;
 reset();const std::string local(3000,'L');system_services::fill(local);nested=mutate=true;
 httpd_req_t all;assert(system_services::download_logs(&all,{1})==ESP_OK&&all.ended==1);
 assert(all.body.find(local)!=std::string::npos&&all.body.find("changed ring")==std::string::npos);
 assert(all.body.find(std::string(24576,'A'))!=std::string::npos);
 assert(all.body.find("bedroom")!=std::string::npos&&all.body.find("device-2")!=std::string::npos&&all.body.find("device-12")!=std::string::npos);
 assert(all.body.find("partial-secondary\n")!=std::string::npos&&all.body.find("some logs may be missing")!=std::string::npos);
 assert(all.body.find("offline")!=std::string::npos&&all.body.find("could not be reached")!=std::string::npos);
 assert(all.body.find("candidate")==std::string::npos&&owners==std::vector<unsigned>({2,3,4}));
 assert(psram_live==0&&psram_max==24576);
 reset();system_services::fill(local);httpd_req_t failed;failed.fail_at=3;
 assert(system_services::download_logs(&failed,{1})==ESP_FAIL&&failed.ended==0&&pulls==0&&psram_live==0);
 reset();httpd_req_t member_failure;member_failure.fail_at=5;
 assert(system_services::download_logs(&member_failure,{1})==ESP_FAIL&&member_failure.ended==0&&pulls==1);
 reset();main_device=false;httpd_req_t empty;
 assert(system_services::download_logs(&empty,{1})==ESP_OK&&empty.ended==1&&pulls==0&&peer_lists==0&&empty.body.find("No logs captured")!=std::string::npos);
 reset();allocation_fails=true;httpd_req_t no_memory;
 assert(system_services::download_logs(&no_memory,{1})==ESP_OK&&no_memory.ended==1&&no_memory.body.find("Local logs unavailable")!=std::string::npos&&pulls==3);
 assert(allocations==frees&&psram_live==0);
 std::cout<<"PASS PSRAM snapshots and streamed main/member logs: stable ring copy, bounded memory, offline/partial results, concurrency, browser abort and allocation cleanup\n";
}
'''


def main():
    services = (ROOT / 'components/system_services/system_services.cpp').read_text()
    snapshot = function(services, 'amity::consortium::LogSnapshot log_snapshot()')
    download = ROOT / 'components/system_services/log_download.cpp'
    with tempfile.TemporaryDirectory(prefix='amity-log-download-') as directory:
        temp = Path(directory)
        (temp / 'system_services.hpp').write_text(HEADER)
        for path in ('amity/consortium/runtime.hpp', 'device_settings.hpp', 'web.hpp'):
            target = temp / path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text('#pragma once\n#include "system_services.hpp"\n')
        cpp, binary = temp / 'test.cpp', temp / 'test'
        cpp.write_text(PREFIX + '\nnamespace system_services {\n' + snapshot + '\n}\n' +
                       '#include "' + str(download) + '"\n' + SUITE)
        subprocess.run(shlex.split(os.environ.get('CXX', 'c++')) + [
            '-std=c++17', '-Wall', '-Wextra', '-Werror', '-g', '-fsanitize=address,undefined',
            '-I' + str(temp), str(cpp), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
