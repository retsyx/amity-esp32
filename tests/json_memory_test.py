#!/usr/bin/env python3
"""Exercise actual startup JSON allocation policy with pinned real cJSON."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
CJSON = ROOT / 'managed_components/espressif__cjson/cJSON'
SUITE = r'''
#include "config_store.hpp"
#include "cJSON.h"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>

struct Allocation {void* pointer=nullptr;size_t bytes=0;bool external=false;};
Allocation allocations[4096];
size_t normal_calls=0,realloc_calls=0,external_calls=0,legacy_freed_by_caps=0,total_queries=0;
size_t remaining=std::numeric_limits<size_t>::max();bool available=AVAILABLE;
constexpr uint32_t external_caps=(1u<<10)|(1u<<2);
size_t slot(void* pointer){size_t i=0;while(i<4096&&allocations[i].pointer!=pointer)++i;assert(i<4096);return i;}
size_t live(){size_t count=0;for(const auto& item:allocations)if(item.pointer)++count;return count;}
void* allocate(size_t bytes,bool external){
 void* pointer=std::malloc(bytes);assert(pointer);size_t i=0;while(i<4096&&allocations[i].pointer)++i;assert(i<4096);allocations[i]={pointer,bytes,external};return pointer;
}
void release(void* pointer){if(!pointer)return;allocations[slot(pointer)]={};std::free(pointer);}
extern "C" void* tracked_malloc(size_t bytes){++normal_calls;return allocate(bytes,false);}
extern "C" void tracked_free(void* pointer){release(pointer);}
extern "C" void* tracked_realloc(void* pointer,size_t bytes){
 ++realloc_calls;if(!pointer)return tracked_malloc(bytes);if(!bytes){release(pointer);return nullptr;}
 const auto i=slot(pointer);void* replacement=std::realloc(pointer,bytes);assert(replacement);allocations[i].pointer=replacement;allocations[i].bytes=bytes;return replacement;
}
extern "C" size_t heap_caps_get_total_size(uint32_t caps){assert(caps==external_caps);++total_queries;return available?8u*1024*1024:0;}
extern "C" void* heap_caps_malloc(size_t bytes,uint32_t caps){
 assert(caps==external_caps);++external_calls;if(!remaining)return nullptr;
 if(remaining!=std::numeric_limits<size_t>::max())--remaining;
 return allocate(bytes,true);
}
extern "C" void heap_caps_free(void* pointer){if(pointer&&!allocations[slot(pointer)].external)++legacy_freed_by_caps;release(pointer);}
int main(){
 cJSON_InitHooks(nullptr);
 const char* document="{\"name\":\"living-room\",\"code\":\"00123456\",\"authority\":{\"id\":\"1234567890abcdef1234567890abcdef\",\"ready\":true},\"values\":[1,2.5,null,false],\"escaped\":\"a\\\"b\"}";
 cJSON* prior=cJSON_Parse(document);assert(prior);char large[4097];memset(large,'x',sizeof(large)-1);large[sizeof(large)-1]='\0';assert(cJSON_AddStringToObject(prior,"large",large));
 char* baseline=cJSON_PrintUnformatted(prior);assert(baseline);const auto before=normal_calls,queries_before=total_queries;
 config_store::initialize_json_memory();
 assert(total_queries==queries_before+(PSRAM_CONFIGURED?1u:0u));
 const bool selected=PSRAM_CONFIGURED&&AVAILABLE;
 // Initialization is one-time even if a later caller changes its view of
 // availability. The global cJSON allocator never toggles during requests.
 available=!available;config_store::initialize_json_memory();assert(total_queries==queries_before+(PSRAM_CONFIGURED?1u:0u));
 for(size_t bytes:{size_t{1},size_t{2},size_t{16},size_t{32},size_t{33},size_t{512},size_t{8192}}){void* pointer=cJSON_malloc(bytes);assert(pointer&&allocations[slot(pointer)].external==selected);cJSON_free(pointer);}
 if(selected)assert(normal_calls==before&&external_calls==7);else assert(external_calls==0&&normal_calls==before+7);
 const auto parse_calls=external_calls;cJSON* current=cJSON_Parse(baseline);assert(current&&cJSON_Compare(prior,current,true));
 const auto parse_allocations=external_calls-parse_calls,print_calls=external_calls,old_reallocs=realloc_calls;
 char* printed=cJSON_PrintUnformatted(current);assert(printed&&strcmp(printed,baseline)==0);const auto print_allocations=external_calls-print_calls;
 if(selected)assert(normal_calls==before&&realloc_calls==old_reallocs);else assert(realloc_calls>old_reallocs);
 cJSON_free(printed);cJSON_Delete(current);
 if(selected){
  const auto retained=live();
  // Every parser allocation boundary must return failure and release all
  // temporary ownership; no fallback may borrow the internal/DMA pool.
  for(size_t fail=0;fail<parse_allocations;++fail){remaining=fail;assert(cJSON_Parse(baseline)==nullptr&&live()==retained&&normal_calls==before);}
  remaining=std::numeric_limits<size_t>::max();current=cJSON_Parse(baseline);assert(current);const auto with_tree=live();
  for(size_t fail=0;fail<print_allocations;++fail){remaining=fail;assert(cJSON_PrintUnformatted(current)==nullptr&&live()==with_tree&&normal_calls==before);}
  remaining=0;assert(cJSON_malloc(1)==nullptr&&normal_calls==before);remaining=std::numeric_limits<size_t>::max();
  cJSON_Delete(current);
 }
 // Pointers and an entire DOM created before hook installation remain safe
 // under the selected heap_caps_free-compatible allocator.
 cJSON_free(baseline);cJSON_Delete(prior);assert(live()==0);
 if(selected)assert(legacy_freed_by_caps>0);else assert(legacy_freed_by_caps==0);
 std::cout<<"PASS actual pinned-cJSON memory: configured="<<PSRAM_CONFIGURED<<" available="<<AVAILABLE<<"; all sizes use selected owner, startup immutable, exact JSON bytes/auth fields, pre-hook frees compatible, allocation failures cannot fall back or leak\n";
}
'''


def main():
    with tempfile.TemporaryDirectory(prefix='amity-json-memory-') as directory:
        temp = Path(directory)
        (temp / 'esp_err.h').write_text('#pragma once\nusing esp_err_t=int;\n')
        (temp / 'nvs.h').write_text('#pragma once\n')
        (temp / 'esp_heap_caps.h').write_text('''#pragma once
#include <stddef.h>
#include <stdint.h>
#define MALLOC_CAP_SPIRAM (1u<<10)
#define MALLOC_CAP_8BIT (1u<<2)
extern "C" size_t heap_caps_get_total_size(uint32_t);
extern "C" void* heap_caps_malloc(size_t,uint32_t);
extern "C" void heap_caps_free(void*);
''')
        cpp = temp / 'test.cpp'
        cpp.write_text(SUITE)
        cjson_object = temp / 'cjson.o'
        subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + [
            '-std=c11', '-g', '-Wno-deprecated-declarations', '-fsanitize=address,undefined',
            '-Dmalloc=tracked_malloc', '-Dfree=tracked_free', '-Drealloc=tracked_realloc',
            '-I' + str(CJSON), '-c', str(CJSON / 'cJSON.c'), '-o', str(cjson_object)], check=True)
        for mode, configured, available in (('psram', 1, 1), ('absent', 1, 0), ('unconfigured', 0, 1), ('host', 0, 0)):
            (temp / 'sdkconfig.h').write_text(f'#pragma once\n#define CONFIG_SPIRAM {configured}\n')
            binary = temp / mode
            platform = [] if mode == 'host' else ['-DESP_PLATFORM']
            subprocess.run(shlex.split(os.environ.get('CXX', 'c++')) + [
                '-std=c++17', '-Wall', '-Wextra', '-Werror', '-g', '-fsanitize=address,undefined',
                '-DPSRAM_CONFIGURED=' + str(configured), '-DAVAILABLE=' + str(available),
                '-I' + str(temp), '-I' + str(CJSON), '-I' + str(ROOT / 'components/config_store/include'),
                *platform, str(ROOT / 'components/config_store/json_memory.cpp'), str(cpp), str(cjson_object), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
