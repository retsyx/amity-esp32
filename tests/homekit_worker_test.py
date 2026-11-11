#!/usr/bin/env python3
"""Run actual HomeKit worker creation/lifecycle code with native RTOS seams."""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

from consortium_gatt_callback_test import function

ROOT = Path(__file__).resolve().parents[1]

PREFIX = r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>
using esp_err_t=int;
using QueueHandle_t=void*;
using SemaphoreHandle_t=void*;
using TaskHandle_t=void*;
using TaskFunction_t=void(*)(void*);
constexpr int ESP_OK=0,ESP_FAIL=-1,ESP_ERR_NO_MEM=1,ESP_ERR_INVALID_STATE=2,ESP_ERR_TIMEOUT=3,ESP_ERR_NVS_NOT_FOUND=4;
constexpr int HAP_SUCCESS=0,pdPASS=1,pdTRUE=1,portMAX_DELAY=-1;
constexpr unsigned MALLOC_CAP_SPIRAM=1,MALLOC_CAP_8BIT=2;
#define pdMS_TO_TICKS(value) (value)
#define ESP_LOGW(...) do {} while(0)
struct StopWorker {};
unsigned creates=0,deletes=0,task_bodies=0,startup_waits=0,startup_signals=0;
unsigned init_calls=0,stop_calls=0,refresh_calls=0,remove_calls=0,value_refreshes=0;
bool create_success=true,startup_ready_now=true,network_up=true;
bool task_frame_alive=false;
int hap_init_result=HAP_SUCCESS,hap_stop_result=HAP_SUCCESS,stored_result=ESP_ERR_NVS_NOT_FOUND;
unsigned paired_controllers=0;
int64_t clock_us=1000000,last_deadline=0;
std::vector<int> requested;
TaskFunction_t created_entry=nullptr;
void* created_argument=reinterpret_cast<void*>(1);
int64_t esp_timer_get_time(){return clock_us;}
SemaphoreHandle_t xSemaphoreCreateMutex(){return reinterpret_cast<void*>(1);}
SemaphoreHandle_t xSemaphoreCreateBinary(){return reinterpret_cast<void*>(2);}
QueueHandle_t xQueueCreate(unsigned count,size_t bytes){assert(count==1&&bytes==sizeof(int));return reinterpret_cast<void*>(3);}
int xSemaphoreTake(SemaphoreHandle_t handle,unsigned wait){
    if(handle==reinterpret_cast<void*>(2)){assert(wait==30000);++startup_waits;return startup_ready_now;}
    assert(handle==reinterpret_cast<void*>(1));return pdTRUE;
}
int xSemaphoreGive(SemaphoreHandle_t handle){assert(handle==reinterpret_cast<void*>(2));++startup_signals;return pdTRUE;}
int xQueueSend(QueueHandle_t handle,const void* item,unsigned wait){assert(handle==reinterpret_cast<void*>(3)&&wait==0);int command;std::memcpy(&command,item,sizeof(command));requested.push_back(command);return pdTRUE;}
int xQueueReceive(QueueHandle_t handle,void* item,unsigned wait){
    assert(handle==reinterpret_cast<void*>(3)&&wait==1000);
    if(requested.empty())throw StopWorker{};
    std::memcpy(item,&requested.front(),sizeof(int));requested.erase(requested.begin());return pdTRUE;
}
int xTaskCreateWithCaps(TaskFunction_t entry,const char* name,unsigned bytes,void* argument,unsigned priority,TaskHandle_t* handle,unsigned caps){
    ++creates;assert(!std::strcmp(name,"homekit")&&bytes==8192&&priority==3&&!argument&&!handle);
    assert(caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    if(!create_success)return 0;
    created_entry=entry;created_argument=argument;return pdPASS;
}
void vTaskDeleteWithCaps(TaskHandle_t handle){assert(!handle&&!task_frame_alive);++deletes;}
namespace amity {struct SystemControl {int marker=17;};}
struct amity_matter_callbacks_t {int marker=23;};
namespace device_settings {const char* hostname(){return "fixture-homekit";}}
namespace config_store {
int read(const char* key,std::string& value){assert(!std::strcmp(key,"hap_setup"));if(stored_result==ESP_OK)value="saved-setup";return stored_result;}
}
int hap_amity_init(const char* name){++init_calls;assert(!std::strcmp(name,"fixture-homekit"));return hap_init_result;}
int hap_amity_stop(){++stop_calls;return hap_stop_result;}
int hap_amity_refresh_mdns(){++refresh_calls;return HAP_SUCCESS;}
int hap_amity_remove_pairings(){++remove_calls;paired_controllers=0;return HAP_SUCCESS;}
void hap_amity_set_pairing_deadline(int64_t value){last_deadline=value;}
unsigned hap_get_paired_controller_count(){return paired_controllers;}
char* esp_hap_get_setup_payload(const char* code,const char* id,bool combined,int category){
    assert(!std::strcmp(code,"111-22-333")&&!std::strcmp(id,"TEST")&&!combined&&category==31);
    auto* result=static_cast<char*>(std::malloc(12));assert(result);std::strcpy(result,"fixture-qr");return result;
}
namespace amity_homekit {
'''

GLOBALS = r'''
namespace {
constexpr int64_t pairing_duration=300LL*1000000;
constexpr int television_category=31;
amity::SystemControl* hub=nullptr;
amity_matter_callbacks_t callbacks;
QueueHandle_t commands=nullptr;
SemaphoreHandle_t status_lock=nullptr,startup_ready=nullptr;
std::mutex lifecycle_mutex;
std::atomic<bool> disabled{false};
bool initialized=false,running=false;
int64_t deadline=0,published_deadline=0;
int startup_result=ESP_OK;
Status current_status;
struct HapLock {};
struct StatusLock {};
bool network_available(){return network_up;}
void publish_status(bool available,int error=ESP_OK){current_status.available=available;current_status.error=error;}
void refresh_values(){++value_refreshes;}
int initialize(const std::string& record){assert(record=="saved-setup"||record=="TEST-setup");initialized=running=true;return ESP_OK;}
int prepare_setup(char* code,std::string& record){std::strcpy(code,"111-22-333");record="TEST-setup";return ESP_OK;}
// The real entry wrapper is exercised with a normally returning work frame;
// the unchanged production loop is separately executed below through commands.
void worker_loop(){struct Frame {Frame(){task_frame_alive=true;}~Frame(){task_frame_alive=false;}} frame;++task_bodies;}
'''

SUITE = r'''
void reset(){
    using namespace amity_homekit;
    creates=deletes=task_bodies=startup_waits=startup_signals=init_calls=stop_calls=refresh_calls=remove_calls=value_refreshes=0;
    create_success=startup_ready_now=network_up=true;task_frame_alive=false;hap_init_result=hap_stop_result=HAP_SUCCESS;
    stored_result=ESP_ERR_NVS_NOT_FOUND;paired_controllers=0;clock_us=1000000;last_deadline=0;requested.clear();
    created_entry=nullptr;created_argument=reinterpret_cast<void*>(1);
    status_lock=startup_ready=commands=nullptr;hub=nullptr;disabled=false;initialized=running=false;
    deadline=published_deadline=0;startup_result=ESP_OK;current_status={};
}
void run_loop(){try{worker_body::worker_loop();assert(false);}catch(const StopWorker&) {}}
int main(){
    using namespace amity_homekit;
    amity::SystemControl service;amity_matter_callbacks_t handlers;
    reset();assert(start(service,handlers)==ESP_OK&&creates==1&&init_calls==1&&startup_waits==1);
    assert(hub==&service&&callbacks.marker==handlers.marker&&created_entry&&!created_argument);
    created_entry(created_argument);assert(task_bodies==1&&deletes==1);
    assert(start(service,handlers)==ESP_ERR_INVALID_STATE&&creates==1);
    reset();create_success=false;
    assert(start(service,handlers)==ESP_ERR_NO_MEM&&creates==1&&!created_entry&&!startup_waits&&!deletes);
    reset();hap_init_result=ESP_FAIL;
    assert(start(service,handlers)==ESP_ERR_NO_MEM&&creates==0&&!startup_waits&&!deletes);
    reset();startup_ready_now=false;
    assert(start(service,handlers)==ESP_ERR_TIMEOUT&&creates==1&&startup_waits==1&&!deletes);
    reset();startup_result=ESP_FAIL;
    assert(start(service,handlers)==ESP_FAIL&&creates==1&&startup_waits==1);
    reset();assert(stop()==ESP_OK&&disabled&&!stop_calls);assert(start(service,handlers)==ESP_ERR_INVALID_STATE&&!creates);
    reset();assert(start(service,handlers)==ESP_OK);stored_result=ESP_OK;run_loop();
    assert(initialized&&running&&startup_signals==1&&current_status.available);
    reset();assert(start(service,handlers)==ESP_OK);current_status.available=true;
    assert(open_pairing()==ESP_OK);run_loop();
    assert(initialized&&running&&last_deadline==clock_us+pairing_duration&&refresh_calls==1&&value_refreshes==1);
    assert(!std::strcmp(current_status.manual_code,"111-22-333")&&!std::strcmp(current_status.qr_payload,"fixture-qr"));
    assert(!current_status.busy);
    assert(remove_pairings()==ESP_OK);run_loop();assert(remove_calls==1&&last_deadline==0&&!current_status.busy);
    assert(close_pairing()==ESP_OK);run_loop();assert(remove_calls==1&&refresh_calls==2&&last_deadline==0);
    current_status.started=current_status.available=current_status.pairing=current_status.busy=true;published_deadline=99;
    assert(stop()==ESP_OK&&disabled&&stop_calls==1&&!running&&!published_deadline);
    assert(!current_status.started&&!current_status.available&&!current_status.pairing&&!current_status.busy);
    assert(!current_status.manual_code[0]&&!current_status.qr_payload[0]&&open_pairing()==ESP_ERR_INVALID_STATE);
    requested.push_back(static_cast<int>(Command::Open));run_loop();
    assert(remove_calls==1&&refresh_calls==2&&!current_status.available&&!current_status.busy);
    reset();assert(start(service,handlers)==ESP_OK);running=true;hap_stop_result=ESP_FAIL;
    assert(stop()==ESP_FAIL&&disabled&&running&&current_status.error==ESP_FAIL);
    std::puts("PASS actual HomeKit app worker: external 8192-byte creation, cap-allocation/init/startup errors, matched self-delete; restored setup/open/close/remove/disabled behavior preserved");
}
'''


def main():
    source = (ROOT / 'components/amity_homekit/homekit.cpp').read_text()
    header = (ROOT / 'components/amity_homekit/include/homekit.hpp').read_text()
    status = re.search(r'struct Status \{.*?\n\};', header, re.S)
    command = re.search(r'enum class Command \{[^}]+\};', source)
    assert status and command
    lifecycle = ''.join(function(source, declaration) for declaration in (
        'void worker(void *)', 'esp_err_t submit(Command command)'))
    public = ''.join(function(source, declaration) for declaration in (
        'esp_err_t start(amity::SystemControl &service,', 'esp_err_t stop()',
        'esp_err_t open_pairing()', 'esp_err_t close_pairing()', 'esp_err_t remove_pairings()'))
    actual_loop = function(source, 'void worker_loop()')
    fixture = (PREFIX + status.group() + command.group() + GLOBALS + lifecycle + '\n}\n'
               + public + '\n}\nnamespace worker_body {using namespace amity_homekit;\n'
               + actual_loop + '\n}\n' + SUITE)
    with tempfile.TemporaryDirectory(prefix='amity-homekit-worker-') as directory:
        cpp, binary = Path(directory) / 'test.cpp', Path(directory) / 'test'
        cpp.write_text(fixture)
        subprocess.run(shlex.split(os.environ.get('CXX', 'c++')) + [
            '-std=c++17', '-Wall', '-Wextra', '-Werror', '-Wno-unused-variable', '-g',
            '-fsanitize=address,undefined', str(cpp), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
