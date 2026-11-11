#!/usr/bin/env python3
"""Exercise effective NimBLE/mDNS placement with actual pinned SDK allocators."""
from pathlib import Path
import json
import os
import re
import shlex
import subprocess
import tempfile

from consortium_gatt_callback_test import function

ROOT = Path(__file__).resolve().parents[1]
IDF = Path(os.environ.get('IDF_PATH', Path.home() / '.espressif/v6.1/esp-idf'))
MDNS = ROOT / 'managed_components/espressif__mdns'

PREFIX = r'''
#include "sdkconfig.h"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <vector>
#define CHECK(value) do {if(!(value)){std::fprintf(stderr,"Check failed at line %d: %s\n",__LINE__,#value);std::abort();}} while(0)
#define MALLOC_CAP_SPIRAM (1u<<10)
#define MALLOC_CAP_INTERNAL (1u<<11)
#define MALLOC_CAP_DMA (1u<<3)
#define MALLOC_CAP_8BIT (1u<<2)
#define ESP_LOGI(tag,...) ((void)(tag))
#define ESP_LOGE(tag,...) ((void)(tag))
#define ESP_EARLY_LOGI(tag,...) ((void)(tag))
struct Allocation {void* pointer;size_t bytes;uint32_t caps;};
std::vector<Allocation> allocations;
std::vector<uint32_t> requests;
size_t ordinary_allocations=0;bool external_available=true;
void* heap_caps_malloc(size_t bytes,uint32_t caps){
    requests.push_back(caps);
    if((caps&MALLOC_CAP_SPIRAM)&&!external_available)return nullptr;
    void* result=std::malloc(bytes);CHECK(result);allocations.push_back({result,bytes,caps});return result;
}
void* heap_caps_calloc(size_t count,size_t bytes,uint32_t caps){
    void* result=heap_caps_malloc(count*bytes,caps);if(result)std::memset(result,0,count*bytes);return result;
}
void heap_caps_free(void* pointer){
    if(!pointer)return;
    auto found=std::find_if(allocations.begin(),allocations.end(),[&](const auto& a){return a.pointer==pointer;});
    CHECK(found!=allocations.end());allocations.erase(found);std::free(pointer);
}
bool external_pointer(void* pointer){
    auto found=std::find_if(allocations.begin(),allocations.end(),[&](const auto& a){return a.pointer==pointer;});
    return found!=allocations.end()&&(found->caps&MALLOC_CAP_SPIRAM);
}
void* forbidden_malloc(size_t){++ordinary_allocations;throw std::runtime_error("Unexpected ordinary malloc fallback");}
void* forbidden_calloc(size_t,size_t){++ordinary_allocations;throw std::runtime_error("Unexpected ordinary calloc fallback");}
struct NimbleAllocationFailure{};
unsigned log_count=0;
#define malloc forbidden_malloc
#define calloc forbidden_calloc
#define assert(value) do {if(!(value))throw NimbleAllocationFailure{};} while(0)
'''

TASK_PREFIX = r'''
#undef malloc
#undef calloc
#undef assert
using esp_err_t=int;
using StackType_t=uint8_t;
struct StaticTask_t {uint8_t kernel_state[100];};
using TaskHandle_t=StaticTask_t*;
constexpr int ESP_OK=0,ESP_FAIL=-1;
#define ESP_TASK_PRIO_MAX 24
#define ESP_TASKD_EVENT_PRIO 20
static TaskHandle_t s_service_task_handle=nullptr;
static StackType_t* s_stack_buffer=nullptr;
static const char* TAG="mdns_service";
bool fail_task_creation=false;size_t task_creations=0,allocation_failure_hooks=0;
void service_task(void*){}
TaskHandle_t xTaskCreateStaticPinnedToCore(void(*entry)(void*),const char* name,size_t bytes,void* argument,
    unsigned priority,StackType_t* stack,StaticTask_t* tcb,int affinity){
    CHECK(entry==service_task&&std::strcmp(name,"mdns")==0&&bytes==CONFIG_MDNS_TASK_STACK_SIZE&&argument==nullptr);
    CHECK(priority==CONFIG_MDNS_TASK_PRIORITY&&affinity==CONFIG_MDNS_TASK_AFFINITY);
    CHECK(external_pointer(stack)&&!external_pointer(tcb));++task_creations;
    return fail_task_creation?nullptr:tcb;
}
#define ESP_GOTO_ON_FALSE(value,error,label,tag,...) do {(void)(tag);if(!(value)){ret=(error);goto label;}} while(0)
#define HOOK_MALLOC_FAILED (++allocation_failure_hooks)
'''

SUITE = r'''
constexpr uint32_t external_caps=MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT;
void expect_external_request(){CHECK(!requests.empty()&&requests.back()==external_caps);}
void expect_external(void* pointer){CHECK(pointer&&external_pointer(pointer));expect_external_request();}
void expect_single_external_failure(size_t before){CHECK(requests.size()==before+1);expect_external_request();}
int main(){
    for(const auto bytes:{size_t{1},size_t{16},size_t{32},size_t{124},size_t{512},size_t{4096},size_t{8192}}){
        void* pointer=nimble_mem_malloc(bytes);expect_external(pointer);nimble_mem_free(pointer);
        pointer=nimble_mem_calloc(2,bytes);expect_external(pointer);
        CHECK(std::all_of(static_cast<uint8_t*>(pointer),static_cast<uint8_t*>(pointer)+2*bytes,[](auto b){return b==0;}));nimble_mem_free(pointer);
        pointer=mdns_mem_malloc(bytes);expect_external(pointer);mdns_mem_free(pointer);
        pointer=mdns_mem_calloc(2,bytes);expect_external(pointer);mdns_mem_free(pointer);
    }
    char* text=mdns_mem_strdup("living-room.local");expect_external(text);CHECK(std::strcmp(text,"living-room.local")==0);mdns_mem_free(text);
    text=mdns_mem_strndup("living-room.local",6);expect_external(text);CHECK(std::strcmp(text,"living")==0);mdns_mem_free(text);
    CHECK(mdns_mem_strdup(nullptr)==nullptr&&mdns_mem_strndup(nullptr,1)==nullptr);
    CHECK(allocations.empty());
    CHECK(create_task_with_caps()==ESP_OK&&task_creations==1&&s_service_task_handle);
    expect_external(s_stack_buffer);mdns_mem_task_free(s_stack_buffer);s_stack_buffer=nullptr;s_service_task_handle=nullptr;
    fail_task_creation=true;CHECK(create_task_with_caps()==ESP_FAIL&&allocations.empty()&&task_creations==2);fail_task_creation=false;
    s_stack_buffer=nullptr;s_service_task_handle=nullptr;
    // Exhaustion must not borrow the controller/kernel internal pool. The SDK
    // malloc assertion remains fail-closed; calloc and mDNS propagate failure.
    external_available=false;
    auto before=requests.size();bool rejected=false;
    try {nimble_mem_malloc(124);}catch(const NimbleAllocationFailure&){rejected=true;}
    CHECK(rejected);expect_single_external_failure(before);
    before=requests.size();CHECK(nimble_mem_calloc(2,124)==nullptr);expect_single_external_failure(before);
    before=requests.size();CHECK(mdns_mem_malloc(124)==nullptr);expect_single_external_failure(before);
    before=requests.size();CHECK(mdns_mem_calloc(2,124)==nullptr);expect_single_external_failure(before);
    before=requests.size();CHECK(mdns_mem_strdup("host")==nullptr);expect_single_external_failure(before);
    before=requests.size();CHECK(mdns_mem_strndup("host",2)==nullptr);expect_single_external_failure(before);
    before=requests.size();CHECK(create_task_with_caps()==ESP_FAIL&&allocation_failure_hooks==1&&task_creations==2);
    expect_single_external_failure(before);CHECK(allocations.empty()&&ordinary_allocations==0);
    // Compile the actual controller and FreeRTOS allocation entry points too:
    // moving host state cannot move their DMA buffers or kernel structures.
    void* control=malloc_internal_wrapper(124);CHECK(control&&!external_pointer(control));
    CHECK(requests.back()==(MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA));heap_caps_free(control);
    void* kernel=pvPortMalloc(sizeof(StaticTask_t));CHECK(kernel&&!external_pointer(kernel));
    CHECK(requests.back()==(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));heap_caps_free(kernel);
    CHECK(allocations.empty()&&ordinary_allocations==0);
    std::puts("PASS effective SDK memory policy: NimBLE host/mDNS heap+stack use PSRAM, exhaustion never falls back internally, task failure cleans up, controller DMA/kernel allocations remain internal");
}
'''


def configuration() -> Path:
    header = ROOT / 'build/config/sdkconfig.h'
    if not header.exists():
        raise AssertionError('Regenerate the firmware configuration before running protocol memory policy tests')
    values = dict(re.findall(r'^#define (CONFIG_\w+) (\S+)', header.read_text(), re.MULTILINE))
    selected = ('CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL', 'CONFIG_MDNS_MEMORY_ALLOC_SPIRAM',
                'CONFIG_MDNS_TASK_CREATE_FROM_SPIRAM', 'CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM')
    for key in selected:
        assert values.get(key) == '1', f'{key} is not effective; regenerate the firmware configuration'
    for key in ('CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_INTERNAL', 'CONFIG_MDNS_MEMORY_ALLOC_INTERNAL',
                'CONFIG_MDNS_TASK_CREATE_FROM_INTERNAL'):
        assert key not in values, f'Conflicting internal allocation choice: {key}'
    # This change places protocol task data; it does not change Wi-Fi hardware
    # buffer limits or the separate CHIP allocator policy.
    for key, expected in {'CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER': '1', 'CONFIG_ESP_WIFI_TX_BUFFER_TYPE': '1',
                          'CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM': '32', 'CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM': '10',
                          'CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM': '32', 'CONFIG_ESP_WIFI_RX_BA_WIN': '6',
                          'CONFIG_CHIP_MEM_ALLOC_MODE_DEFAULT': '1'}.items():
        assert values.get(key) == expected, f'Unrelated memory policy changed: {key}'
    assert 'CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP' not in values
    assert values.get('CONFIG_IDF_TARGET_ESP32S3') == '1'
    # ESP-IDF uses this shared controller implementation for the S3. Confirm
    # the actual target's build selects it before testing its internal heap.
    controller = IDF / 'components/bt/controller/esp32c3/bt.c'
    commands = json.loads((ROOT / 'build/compile_commands.json').read_text())
    assert any(Path(entry['file']) == controller for entry in commands)
    defaults = (ROOT / 'sdkconfig.defaults').read_text()
    for key in selected[:3]:
        assert f'{key}=y' in defaults, f'Missing saved allocation choice: {key}'
    return header


def main() -> None:
    header = configuration()
    nimble = (IDF / 'components/bt/host/nimble/port/src/esp_nimble_mem.c').read_text()
    mdns = (MDNS / 'mdns_mem_caps.c').read_text()
    service = (MDNS / 'mdns_service.c').read_text()
    controller = (IDF / 'components/bt/controller/esp32c3/bt.c').read_text()
    kernel = (IDF / 'components/freertos/heap_idf.c').read_text()
    actual_host = ''.join(function(nimble, name) for name in
                          ('void *nimble_mem_malloc(', 'void *nimble_mem_calloc(', 'void nimble_mem_free('))
    actual_mdns = mdns[mdns.index('#if CONFIG_MDNS_MEMORY_CUSTOM_IMPL'):]
    task_macros = service[service.index('#define MDNS_SERVICE_STACK_DEPTH'):service.index('#define MDNS_SERVICE_LOCK')]
    controller_caps = re.search(r'^#define BLE_CONTROLLER_MALLOC_CAPS.*$', controller, re.MULTILINE).group()
    kernel_caps = re.search(r'^#define portFREERTOS_HEAP_CAPS.*$', kernel, re.MULTILINE).group()
    actual_internal = controller_caps + '\n#define BT_LOG_TAG "controller"\n' + function(controller, 'static void *malloc_internal_wrapper(size_t size)\n{')
    actual_internal += kernel_caps + '\n' + function(kernel, 'void * pvPortMalloc(')
    actual_task = function(service, 'static esp_err_t create_task_with_caps(')
    # C permits this void-pointer conversion implicitly; C++ requires the
    # explicit cast in the native fixture. No allocation/lifetime logic changes.
    actual_task = actual_task.replace('s_stack_buffer = mdns_mem_task_malloc(MDNS_SERVICE_STACK_DEPTH);',
                                     's_stack_buffer = static_cast<StackType_t*>(mdns_mem_task_malloc(MDNS_SERVICE_STACK_DEPTH));')
    source = PREFIX + actual_host + actual_mdns + TASK_PREFIX + task_macros + actual_task + actual_internal + SUITE
    with tempfile.TemporaryDirectory(prefix='amity-protocol-memory-') as directory:
        cpp, binary = Path(directory) / 'test.cpp', Path(directory) / 'test'
        cpp.write_text(source)
        subprocess.run(shlex.split(os.environ.get('CXX', 'c++')) + [
            '-std=c++17', '-Wall', '-Wextra', '-Werror', '-g', '-fsanitize=address,undefined',
            '-I' + str(header.parent), str(cpp), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
