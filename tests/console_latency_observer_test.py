#!/usr/bin/env python3
"""Execute the real log hook against an instrumented console and ring lock."""
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
#include <cassert>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
std::array<char,512> ring{};
size_t cursor=0,used=0;
int lock_value=0;
int *lock=&lock_value;
constexpr uint32_t portMAX_DELAY=UINT32_MAX;
bool ring_locked=false;
int64_t clock_us=0,ring_delay_us=0,output_delay_us=0;
unsigned clock_reads=0,original_calls=0;
int original_result=0;
char forwarded[2048]{};
using vprintf_like_t=int(*)(const char*,va_list);
int64_t esp_timer_get_time(){++clock_reads;return clock_us;}
void xSemaphoreTake(int *value,uint32_t timeout){
    assert(value==lock&&timeout==portMAX_DELAY&&!ring_locked);
    clock_us+=ring_delay_us;ring_locked=true;
}
void xSemaphoreGive(int *value){assert(value==lock&&ring_locked);ring_locked=false;}
int output(const char *format,va_list arguments){
    assert(!ring_locked);++original_calls;
    const int size=std::vsnprintf(forwarded,sizeof(forwarded),format,arguments);
    assert(size>=0&&size<static_cast<int>(sizeof(forwarded)));
    clock_us+=output_delay_us;
    return original_result;
}
vprintf_like_t original=output;
namespace amity::network_diagnostics {
struct Call {uint32_t began,duration;};
std::array<Call,8> calls{};
unsigned count=0;
void record_console_call(uint32_t began,uint32_t duration){
    assert(!ring_locked&&original_calls>count&&count<calls.size());
    calls[count++]={began,duration};
}
}
'''
SUITE = r'''
int emit(const char *format,...){
    va_list arguments;va_start(arguments,format);
    const int result=capture(format,arguments);
    va_end(arguments);return result;
}
void reset(){
    ring.fill(0);cursor=used=0;ring_locked=false;
    clock_us=ring_delay_us=output_delay_us=0;clock_reads=original_calls=0;
    original_result=17;forwarded[0]=0;original=output;
    amity::network_diagnostics::count=0;
}
int main(){
    using namespace amity::network_diagnostics;
    reset();clock_us=1'234'567;ring_delay_us=75'000;output_delay_us=1'500'900;
    assert(emit("key %u owner=%s",3u,"member")==17);
    assert(std::strcmp(forwarded,"key 3 owner=member")==0);
    assert(std::string(ring.data(),used)==forwarded);
    assert(original_calls==1&&clock_reads==2&&count==1);
    assert(calls[0].began==1309&&calls[0].duration==1500);
    // The ring-format copy must not consume the original variadic arguments,
    // and a truncated ring fragment must not truncate the original console.
    reset();const std::string long_text(500,'x');output_delay_us=23'456;
    assert(emit("%s %d",long_text.c_str(),41)==17);
    assert(std::string(forwarded)==long_text+" 41"&&used==383);
    assert(count==1&&calls[0].duration==23);
    reset();original_result=-7;output_delay_us=100'000;
    assert(emit("console error %d",19)==-7&&count==1&&calls[0].duration==100);
    reset();original=nullptr;output_delay_us=9'000'000;
    assert(emit("ring only %d",27)==12&&used==12);
    assert(original_calls==0&&clock_reads==0&&count==0);
    reset();clock_us=(int64_t(UINT32_MAX)-5)*1000+500;output_delay_us=16'700;
    assert(emit("wrapped")==17&&count==1);
    assert(calls[0].began==UINT32_MAX-5&&calls[0].duration==16);
    reset();output_delay_us=-1'000;
    assert(emit("clock anomaly")==17&&count==1&&calls[0].duration==0);
    std::cout<<"PASS actual console capture: exact variadic forwarding/return, released ring lock, forward-only timing, truncated ring/full console, null sink and clock boundaries\n";
}
'''


def main():
    actual = function((ROOT / 'components/system_services/system_services.cpp').read_text(), 'int capture(')
    with tempfile.TemporaryDirectory(prefix='amity-console-latency-') as directory:
        cpp, binary = Path(directory) / 'test.cpp', Path(directory) / 'test'
        cpp.write_text(PREFIX + actual + SUITE)
        subprocess.run(shlex.split(os.environ.get('CXX', 'c++')) + [
            '-std=c++17', '-Wall', '-Wextra', '-Werror', '-g', '-fsanitize=address,undefined',
            str(cpp), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
