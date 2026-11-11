#!/usr/bin/env python3
"""Exercise effective protocol memory placement with actual pinned SDK functions.

Wi-Fi checks cover its public configuration, open-source allocator preference
and netif dispatch. The precompiled driver's TX-cache behavior is not modeled.
"""
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
#include <cstdarg>
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
#define MALLOC_CAP_DEFAULT (1u<<12)
#define IRAM_ATTR
#define HEAP_IRAM_ATTR
#define ESP_LOGI(tag,...) ((void)(tag))
#define ESP_LOGE(tag,...) ((void)(tag))
#define ESP_EARLY_LOGI(tag,...) ((void)(tag))
struct Allocation {void* pointer;size_t bytes;uint32_t caps;};
std::vector<Allocation> allocations;
std::vector<uint32_t> requests;
size_t ordinary_allocations=0;bool external_available=true,internal_available=true;
void* heap_caps_malloc(size_t bytes,uint32_t caps){
    requests.push_back(caps);
    if((caps&MALLOC_CAP_SPIRAM)&&!external_available)return nullptr;
    if((caps&MALLOC_CAP_INTERNAL)&&!internal_available)return nullptr;
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

WIFI_PREFIX = r'''
// Only the allocation device seam is modeled. Preference/fallback order comes
// from the actual SDK heap helpers compiled below, not a replacement policy.
unsigned allocation_failures=0;
void* heap_caps_malloc_base(size_t size,uint32_t caps){return heap_caps_malloc(size,caps);}
void* heap_caps_calloc_base(size_t count,size_t size,uint32_t caps){return heap_caps_calloc(count,size,caps);}
void* heap_caps_realloc_base(void* pointer,size_t size,uint32_t caps){
    auto found=std::find_if(allocations.begin(),allocations.end(),[&](const auto& a){return a.pointer==pointer;});
    CHECK(!pointer||found!=allocations.end());const auto previous=pointer?found->bytes:0;
    if(!size){heap_caps_free(pointer);return nullptr;}
    void* result=heap_caps_malloc(size,caps);if(!result)return nullptr;
    if(pointer){std::memcpy(result,pointer,std::min(size,previous));heap_caps_free(pointer);}return result;
}
void* heap_caps_realloc(void* pointer,size_t size,uint32_t caps){return heap_caps_realloc_base(pointer,size,caps);}
void heap_caps_alloc_failed(size_t bytes,uint32_t caps,const char*){
    CHECK(bytes>0&&caps==(MALLOC_CAP_DEFAULT|MALLOC_CAP_INTERNAL));++allocation_failures;
}
struct wifi_osi_funcs_t {};
struct wpa_crypto_funcs_t {};
wifi_osi_funcs_t g_wifi_osi_funcs;
const wpa_crypto_funcs_t g_wifi_default_wpa_crypto_funcs{};
constexpr int ESP_ERR_NOT_SUPPORTED=-2;
struct wifi_netif_driver {int wifi_if=7;};
using wifi_netif_driver_t=wifi_netif_driver*;
unsigned plain_tx_calls=0,by_ref_tx_calls=0;
void* expected_buffer=nullptr,*expected_reference=nullptr;
size_t expected_bytes=0;
int transmit_result=0;
int esp_wifi_internal_tx(int,void*,size_t){++plain_tx_calls;return transmit_result;}
int esp_wifi_internal_tx_by_ref(int interface,void* bytes,size_t size,void* reference){
    CHECK(interface==7&&bytes==expected_buffer&&size==expected_bytes&&reference==expected_reference);
    ++by_ref_tx_calls;return transmit_result;
}
'''

WIFI_SUITE = r'''
void wifi_policy_tests(){
    constexpr uint32_t external=MALLOC_CAP_DEFAULT|MALLOC_CAP_SPIRAM,internal=MALLOC_CAP_DEFAULT|MALLOC_CAP_INTERNAL;
    for(const auto bytes:{size_t{1},size_t{16},size_t{32},size_t{512},size_t{4096}}){
        const auto before=requests.size();void* value=wifi_malloc(bytes);
        CHECK(value&&external_pointer(value)&&requests.size()==before+1&&requests.back()==external);
        std::memset(value,0x5a,bytes);value=wifi_realloc(value,bytes+9);
        CHECK(value&&external_pointer(value)&&requests.back()==external);
        CHECK(std::all_of(static_cast<uint8_t*>(value),static_cast<uint8_t*>(value)+bytes,[](auto b){return b==0x5a;}));heap_caps_free(value);
        value=wifi_calloc(2,bytes);CHECK(value&&external_pointer(value)&&requests.back()==external);
        CHECK(std::all_of(static_cast<uint8_t*>(value),static_cast<uint8_t*>(value)+2*bytes,[](auto b){return b==0;}));heap_caps_free(value);
    }
    // Unlike NimBLE's explicit external-only mode, this supported Wi-Fi mode
    // deliberately falls back internally when its first preference is full.
    external_available=false;
    for(const auto bytes:{size_t{1},size_t{32},size_t{4096}}){
        auto before=requests.size();void* value=wifi_malloc(bytes);
        CHECK(value&&!external_pointer(value)&&requests.size()==before+2&&requests[before]==external&&requests.back()==internal);
        std::memset(value,0x63,bytes);before=requests.size();value=wifi_realloc(value,bytes+9);
        CHECK(value&&!external_pointer(value)&&requests.size()==before+2&&requests[before]==external&&requests.back()==internal);
        CHECK(std::all_of(static_cast<uint8_t*>(value),static_cast<uint8_t*>(value)+bytes,[](auto b){return b==0x63;}));heap_caps_free(value);
        before=requests.size();value=wifi_calloc(2,bytes);
        CHECK(value&&!external_pointer(value)&&requests.size()==before+2&&requests[before]==external&&requests.back()==internal);
        CHECK(std::all_of(static_cast<uint8_t*>(value),static_cast<uint8_t*>(value)+2*bytes,[](auto b){return b==0;}));heap_caps_free(value);
    }
    external_available=true;void* preserved=wifi_malloc(32);std::memset(preserved,0x37,32);
    external_available=internal_available=false;
    CHECK(wifi_malloc(32)==nullptr&&wifi_calloc(2,16)==nullptr&&wifi_realloc(preserved,64)==nullptr&&allocation_failures==3);
    CHECK(std::all_of(static_cast<uint8_t*>(preserved),static_cast<uint8_t*>(preserved)+32,[](auto b){return b==0x37;}));heap_caps_free(preserved);
    external_available=internal_available=true;
    constexpr uint32_t dma=MALLOC_CAP_8BIT|MALLOC_CAP_DMA|MALLOC_CAP_INTERNAL;
    void* value=calloc_internal_wrapper(2,32);CHECK(value&&!external_pointer(value)&&requests.back()==dma);
    value=realloc_internal_wrapper(value,128);CHECK(value&&!external_pointer(value)&&requests.back()==dma);heap_caps_free(value);
    value=zalloc_internal_wrapper(64);CHECK(value&&!external_pointer(value)&&requests.back()==dma);heap_caps_free(value);
    wifi_init_config_t config=WIFI_INIT_CONFIG_DEFAULT();
    CHECK(config.tx_buf_type==0&&config.static_tx_buf_num==16&&config.dynamic_tx_buf_num==0&&config.cache_tx_buf_num==64);
    CHECK(config.static_rx_buf_num==10&&config.dynamic_rx_buf_num==32&&config.rx_ba_win==6);
    CHECK((config.feature_caps&CONFIG_FEATURE_CACHE_TX_BUF_BIT)!=0&&esp_wifi_psram_check(&config)==ESP_OK);
    CHECK(zero_cache_check(&config)==ESP_ERR_NOT_SUPPORTED);config.feature_caps&=~uint64_t(CONFIG_FEATURE_CACHE_TX_BUF_BIT);
    CHECK(zero_cache_check(&config)==ESP_OK);
    wifi_netif_driver driver;uint8_t bytes[8]{};int reference=0;
    expected_buffer=bytes;expected_bytes=sizeof(bytes);expected_reference=&reference;
    CHECK(wifi_transmit_wrap(&driver,bytes,sizeof(bytes),&reference)==0&&by_ref_tx_calls==1&&!plain_tx_calls);
    transmit_result=257;CHECK(wifi_transmit_wrap(&driver,bytes,sizeof(bytes),&reference)==257&&by_ref_tx_calls==2&&!plain_tx_calls);
    CHECK(allocations.empty()&&ordinary_allocations==0);
}
'''

SUITE = r'''
constexpr uint32_t external_caps=MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT;
void expect_external_request(){CHECK(!requests.empty()&&requests.back()==external_caps);}
void expect_external(void* pointer){CHECK(pointer&&external_pointer(pointer));expect_external_request();}
void expect_single_external_failure(size_t before){CHECK(requests.size()==before+1);expect_external_request();}
int main(){
    wifi_policy_tests();
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
    std::puts("PASS actual SDK policy: Wi-Fi PSRAM preference/internal fallback, static16/cache64 configuration and by-reference netif dispatch; NimBLE/mDNS external memory; controller DMA/kernel allocations remain internal");
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
    # Fix the native S3 Wi-Fi policy explicitly without importing the different
    # defaults implied by this switch for RX, Block Ack or TCP out-of-order data.
    for key, expected in {'CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP': '1',
                          'CONFIG_ESP_WIFI_STATIC_TX_BUFFER': '1', 'CONFIG_ESP_WIFI_TX_BUFFER_TYPE': '0',
                          'CONFIG_ESP_WIFI_STATIC_TX_BUFFER_NUM': '16', 'CONFIG_ESP_WIFI_CACHE_TX_BUFFER_NUM': '64',
                          'CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM': '10',
                          'CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM': '32', 'CONFIG_ESP_WIFI_RX_BA_WIN': '6',
                          'CONFIG_ESP_WIFI_TX_BA_WIN': '6', 'CONFIG_LWIP_TCP_OOSEQ_MAX_PBUFS': '4',
                          'CONFIG_CHIP_MEM_ALLOC_MODE_DEFAULT': '1'}.items():
        assert values.get(key) == expected, f'Unrelated memory policy changed: {key}'
    for key in ('CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER', 'CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM',
                'CONFIG_SPIRAM_IGNORE_NOTFOUND'):
        assert key not in values, f'Unexpected native Wi-Fi policy: {key}'
    assert values.get('CONFIG_IDF_TARGET_ESP32S3') == '1'
    # ESP-IDF uses this shared controller implementation for the S3. Confirm
    # the actual target's build selects it before testing its internal heap.
    controller = IDF / 'components/bt/controller/esp32c3/bt.c'
    commands = json.loads((ROOT / 'build/compile_commands.json').read_text())
    assert any(Path(entry['file']) == controller for entry in commands)
    wifi_adapter = IDF / 'components/esp_wifi/esp32s3/esp_adapter.c'
    assert any(Path(entry['file']) == wifi_adapter for entry in commands)
    defaults = (ROOT / 'sdkconfig.defaults').read_text()
    for key in selected[:3]:
        assert f'{key}=y' in defaults, f'Missing saved allocation choice: {key}'
    for setting in ('CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y', 'CONFIG_ESP_WIFI_STATIC_TX_BUFFER_NUM=16',
                    'CONFIG_ESP_WIFI_CACHE_TX_BUFFER_NUM=64', 'CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=10',
                    'CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM=32', 'CONFIG_ESP_WIFI_RX_BA_WIN=6',
                    'CONFIG_ESP_WIFI_TX_BA_WIN=6', 'CONFIG_LWIP_TCP_OOSEQ_MAX_PBUFS=4'):
        assert setting in defaults.splitlines(), f'Missing saved Wi-Fi constraint: {setting}'
    return header


def main() -> None:
    header = configuration()
    nimble = (IDF / 'components/bt/host/nimble/port/src/esp_nimble_mem.c').read_text()
    mdns = (MDNS / 'mdns_mem_caps.c').read_text()
    service = (MDNS / 'mdns_service.c').read_text()
    controller = (IDF / 'components/bt/controller/esp32c3/bt.c').read_text()
    kernel = (IDF / 'components/freertos/heap_idf.c').read_text()
    wifi = (IDF / 'components/esp_wifi/esp32s3/esp_adapter.c').read_text()
    wifi_header = (IDF / 'components/esp_wifi/include/esp_wifi.h').read_text()
    wifi_init = (IDF / 'components/esp_wifi/src/wifi_init.c').read_text()
    wifi_netif = (IDF / 'components/esp_wifi/src/wifi_netif.c').read_text()
    heap = (IDF / 'components/heap/heap_caps.c').read_text()
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
    actual_heap = ''.join(function(heap, name) for name in (
        'void *heap_caps_malloc_prefer(', 'void *heap_caps_realloc_prefer(', 'void *heap_caps_calloc_prefer('))
    actual_wifi = ''.join(function(wifi, name) for name in (
        'void *wifi_malloc(', 'void *wifi_realloc(', 'void *wifi_calloc(',
        'void * IRAM_ATTR realloc_internal_wrapper(', 'void * IRAM_ATTR calloc_internal_wrapper(',
        'void * IRAM_ATTR zalloc_internal_wrapper('))
    config_type = re.search(r'typedef struct \{[^}]+\} wifi_init_config_t;', wifi_header, re.S).group()
    macro_start = wifi_header.index('#ifdef CONFIG_ESP_WIFI_STATIC_TX_BUFFER_NUM')
    macro_end = wifi_header.index('\n}', wifi_header.index('#define WIFI_INIT_CONFIG_DEFAULT()')) + 2
    actual_wifi_macros = wifi_header[macro_start:macro_end]
    psram_check = function(wifi_init, 'static esp_err_t esp_wifi_psram_check(')
    zero_check = ('\n#undef WIFI_CACHE_TX_BUFFER_NUM\n#define WIFI_CACHE_TX_BUFFER_NUM 0\n'
                  + psram_check.replace('esp_wifi_psram_check(', 'zero_cache_check(', 1)
                  + '\n#undef WIFI_CACHE_TX_BUFFER_NUM\n#define WIFI_CACHE_TX_BUFFER_NUM CONFIG_ESP_WIFI_CACHE_TX_BUFFER_NUM\n')
    transmit = function(wifi_netif, 'static esp_err_t wifi_transmit_wrap(').replace(
        'wifi_netif_driver_t driver = h;', 'wifi_netif_driver_t driver = static_cast<wifi_netif_driver_t>(h);')
    source = '\n'.join((PREFIX, actual_host, actual_mdns, TASK_PREFIX, task_macros, actual_task, actual_internal,
                       WIFI_PREFIX, actual_heap, actual_wifi, config_type, actual_wifi_macros,
                       psram_check, zero_check, transmit, WIFI_SUITE, SUITE))
    with tempfile.TemporaryDirectory(prefix='amity-protocol-memory-') as directory:
        cpp, binary = Path(directory) / 'test.cpp', Path(directory) / 'test'
        cpp.write_text(source)
        subprocess.run(shlex.split(os.environ.get('CXX', 'c++')) + [
            '-std=c++17', '-Wall', '-Wextra', '-Werror', '-g', '-fsanitize=address,undefined',
            '-I' + str(header.parent), str(cpp), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
