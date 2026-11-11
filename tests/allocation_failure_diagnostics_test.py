#!/usr/bin/env python3
"""Exercise actual bounded allocator-failure capture without logging in the hook."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

from consortium_gatt_callback_test import function

ROOT = Path(__file__).resolve().parents[1]
PREFIX = r'''
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <new>
#include <string>
#include <vector>
bool allocations_forbidden=false;
void* operator new(size_t size){assert(!allocations_forbidden);if(auto* p=std::malloc(size))return p;throw std::bad_alloc();}
void operator delete(void* p)noexcept{std::free(p);}
void operator delete(void* p,std::size_t)noexcept{::operator delete(p);}
constexpr int ESP_OK=0;
constexpr uint32_t MALLOC_CAP_DMA=8,MALLOC_CAP_INTERNAL=2048,MALLOC_CAP_8BIT=4,MALLOC_CAP_SPIRAM=1024;
struct multi_heap_info_t{size_t total_free_bytes=0,minimum_free_bytes=0,largest_free_block=0;};
using esp_alloc_failed_hook_t=void(*)(size_t,uint32_t,const char*);
esp_alloc_failed_hook_t registered_failure_hook=nullptr;
int heap_caps_register_failed_alloc_callback(esp_alloc_failed_hook_t hook){registered_failure_hook=hook;return ESP_OK;}
uint64_t current_ms=777;
int64_t esp_timer_get_time(){return static_cast<int64_t>(current_ms*1000);}
bool in_isr=false,recurse=false,max_stats=false;
unsigned heap_queries=0,minimum_queries=0,task_queries=0;
char task_name[64]="group_link";
bool xPortInIsrContext(){return in_isr;}
const char* pcTaskGetName(void*){++task_queries;return task_name;}
void heap_caps_get_info(multi_heap_info_t*,uint32_t){assert(false&&"Heap walks mask CEC timer interrupts");}
size_t heap_caps_get_free_size(uint32_t caps){
 ++heap_queries;
 if(recurse){recurse=false;assert(registered_failure_hook);registered_failure_hook(99,MALLOC_CAP_DMA,"nested_failure");}
 if(max_stats)return UINT32_MAX;
 if(caps==(MALLOC_CAP_DMA|MALLOC_CAP_8BIT))return 111;
 if(caps==MALLOC_CAP_DMA)return 512;
 if(caps==(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT))return 12345;
 return 987654;
}
size_t heap_caps_get_minimum_free_size(uint32_t caps){
 ++minimum_queries;
 return caps==(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)?3:4;
}
std::vector<std::string> lines;
template<class... Args>void record_log(const char* level,const char* tag,const char* format,Args... args){
 assert(!allocations_forbidden);char message[384];int n;
 if constexpr(sizeof...(args)==0)n=snprintf(message,sizeof(message),"%s",format);else n=snprintf(message,sizeof(message),format,args...);
 assert(n>=0&&n<static_cast<int>(sizeof(message)));
 lines.push_back(std::string(level)+":"+tag+" "+message);
}
#define ESP_LOGE(...) record_log("E",__VA_ARGS__)
#define ESP_LOGW(...) record_log("W",__VA_ARGS__)
#define ESP_LOGI(...) record_log("I",__VA_ARGS__)
namespace system_services {
bool ready=false;
bool firmware_ready(){return ready;}
using vprintf_like_t=int(*)(const char*,va_list);
vprintf_like_t original=nullptr,installed=nullptr;
char boot_identifier[33]{};
void* lock=nullptr;
void bootloader_random_enable(){}
void bootloader_random_disable(){}
void esp_fill_random(void* bytes,size_t size){assert(size==16);const uint32_t entropy[]={1,2,3,4};std::memcpy(bytes,entropy,size);}
void* xSemaphoreCreateMutex(){return reinterpret_cast<void*>(1);}
int capture(const char*,va_list){return 0;}
vprintf_like_t esp_log_set_vprintf(vprintf_like_t callback){installed=callback;return nullptr;}
}
'''
SUITE = r'''
int main(){using namespace system_services;
 start_log_capture();assert(registered_failure_hook==capture_allocation_failure&&installed==capture);
 assert(std::string(boot_identifier)=="00000001000000020000000300000004");
 allocations_forbidden=true;recurse=true;
 registered_failure_hook(512,MALLOC_CAP_DMA|MALLOC_CAP_8BIT,"heap_caps_aligned_alloc");
 allocations_forbidden=false;assert(lines.empty()&&heap_queries==3&&allocation_failure_count==1);
 current_ms=999;log_memory_if_due(); // Allocation observations are emitted even before boot readiness.
 assert(lines.size()==2&&lines[0].find("at_ms=777 bytes=512 caps=0x0000000c fn=heap_caps_aligned_alloc task=group_link")!=std::string::npos);
 assert(lines[0].find("matched_free=111 dma_free=512 internal_free=12345")!=std::string::npos);
 assert(lines[1].find("suppressed=1 reason=concurrent_or_full")!=std::string::npos);
 lines.clear();log_memory_if_due();assert(lines.empty()); // No event, no extra periodic output.
 allocations_forbidden=true;for(unsigned n=0;n<9;++n)registered_failure_hook(100+n,MALLOC_CAP_DMA,"bounded");allocations_forbidden=false;
 assert(allocation_failure_count==8);log_memory_if_due();assert(lines.size()==9);
 for(unsigned n=0;n<8;++n)assert(lines[n].find("bytes="+std::to_string(100+n)+" ")!=std::string::npos);
 assert(lines.back().find("suppressed=1")!=std::string::npos&&allocation_failure_count==0);
 lines.clear();char name[80];memset(name,'f',sizeof(name));name[sizeof(name)-1]='\0';memset(task_name,'t',sizeof(task_name));task_name[sizeof(task_name)-1]='\0';max_stats=true;
 allocations_forbidden=true;registered_failure_hook(UINT32_MAX,UINT32_MAX,name);allocations_forbidden=false;
 memset(name,'x',sizeof(name));strcpy(task_name,"changed");log_memory_if_due();assert(lines.size()==1);
 assert(lines[0].find("fn="+std::string(31,'f')+" task="+std::string(15,'t'))!=std::string::npos&&lines[0].size()<384);
 lines.clear();in_isr=true;const auto before=task_queries;
 allocations_forbidden=true;registered_failure_hook(48,MALLOC_CAP_DMA,"interrupt_allocation");allocations_forbidden=false;
 log_memory_if_due();assert(task_queries==before&&lines.size()==1&&lines[0].find("task=ISR")!=std::string::npos);
 lines.clear();log_memory_if_due();assert(lines.empty());
 in_isr=false;max_stats=false;ready=true;current_ms=1204000;const auto baseline_queries=heap_queries;
 log_memory_if_due();assert(lines.size()==1&&heap_queries==baseline_queries+2&&minimum_queries==2);
 assert(lines[0].find("uptime=1204s bytes: internal free=12345 min=3; psram free=987654 min=4")!=std::string::npos);
 lines.clear();current_ms+=599999;log_memory_if_due();assert(lines.empty()&&heap_queries==baseline_queries+2&&minimum_queries==2);
 ++current_ms;log_memory_if_due();assert(lines.size()==1&&heap_queries==baseline_queries+4&&minimum_queries==4);
 std::cout<<"PASS actual memory diagnostics: no heap walks; allocation hook allocates/logs nothing, captures original size/caps/function/task/time and matching/DMA/internal free counters, bounds recursion/capacity/names, emits pending events outside allocator context; periodic free/min counters retain ten-minute cadence\n";
}
'''


def main():
    source = (ROOT / 'components/system_services/system_services.cpp').read_text()
    actual = source[source.index('struct AllocationFailure {'):source.index('int capture(const char *fmt,')]
    actual += function(source, 'void start_log_capture()')
    actual += function(source, 'void log_memory_if_due()')
    with tempfile.TemporaryDirectory(prefix='amity-allocation-failure-') as directory:
        cpp, binary = Path(directory) / 'test.cpp', Path(directory) / 'test'
        cpp.write_text(PREFIX + 'namespace system_services {\n' + actual + '\n}\n' + SUITE)
        subprocess.run(shlex.split(os.environ.get('CXX', 'c++')) + [
            '-std=c++17', '-Wall', '-Wextra', '-Werror', '-g', '-fsanitize=address,undefined',
            str(cpp), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
