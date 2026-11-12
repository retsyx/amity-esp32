#!/usr/bin/env python3
"""Compile the shipping Wi-Fi signal sampler with fake SDK and clock boundaries."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

HEADERS = {
    'esp_attr.h': '#pragma once\n#define DRAM_ATTR\n',
    'esp_timer.h': '#pragma once\n#include <cstdint>\nint64_t esp_timer_get_time();\n',
    'esp_log.h': '#pragma once\nvoid signal_log(const char*,const char*,...);\n#define ESP_LOGW(tag,...) signal_log(tag,__VA_ARGS__)\n',
    'esp_wifi.h': '#pragma once\n#include <cstdint>\nusing esp_err_t=int;constexpr int ESP_OK=0,ESP_ERR_WIFI_NOT_CONNECT=0x300f;\n'
                  'struct wifi_ap_record_t {uint8_t primary;int8_t rssi;};\nesp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t*);\n',
}

SUITE = r'''
#include "wifi_signal.hpp"
#include "esp_wifi.h"
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>
using namespace amity::network_diagnostics;
int64_t clock_ms=5000;unsigned sdk_calls=0;bool connected=true;wifi_ap_record_t access_point{6,-55};std::vector<std::string> logs;
int64_t esp_timer_get_time(){return clock_ms*1000;}
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t* out){++sdk_calls;if(!connected)return ESP_ERR_WIFI_NOT_CONNECT;*out=access_point;return ESP_OK;}
void signal_log(const char* tag,const char* format,...){
    assert(std::string(tag)=="network_diag");char line[160];va_list values;va_start(values,format);std::vsnprintf(line,sizeof(line),format,values);va_end(values);logs.emplace_back(line);
}
void policy(){
    LowSignalLog log;uint32_t now=0;
    assert(!log.observe(-50,now)&&!log.observe(-70,now)); // The threshold itself is not low.
    auto logged=log.observe(-71,now);assert(logged&&*logged==-71);
    now+=10000;assert(!log.observe(-75,now));now+=10000;assert(!log.observe(-69,now)); // Still low within the recovery margin.
    now=LowSignalLog::repeat_interval_ms-1;assert(!log.observe(-72,now));
    now+=1;logged=log.observe(-71,now);assert(logged&&*logged==-75); // Lowest sample since the previous log.
    assert(!log.observe(-67,now+10000)); // Recovered.
    now+=20000;assert(!log.observe(-72,now)); // A new episode within a minute of the last log waits.
    now+=LowSignalLog::episode_interval_ms-20000;logged=log.observe(-73,now);assert(logged&&*logged==-73);
    std::cout<<"PASS low-signal policy: threshold, 3 dB recovery margin, one-minute episode and ten-minute repeat limits, minimum since the previous log\n";
}
void sampler(){
    assert(!wifi_signal().known);
    poll_wifi_signal();auto signal=wifi_signal();assert(sdk_calls==1&&signal.known&&signal.rssi==-55&&signal.channel==6&&logs.empty());
    clock_ms+=9999;access_point={11,-80};poll_wifi_signal();assert(sdk_calls==1&&wifi_signal().rssi==-55);
    clock_ms+=1;poll_wifi_signal();signal=wifi_signal();assert(sdk_calls==2&&signal.rssi==-80&&signal.channel==11);
    assert(logs.size()==1&&logs.back()=="Wi-Fi signal low rssi=-80 dBm min=-80 dBm channel=11");
    clock_ms+=10000;access_point={11,-78};poll_wifi_signal();assert(sdk_calls==3&&logs.size()==1);
    clock_ms+=10000;connected=false;poll_wifi_signal();assert(sdk_calls==4&&!wifi_signal().known&&logs.size()==1);
    std::cout<<"PASS sampler: ten-second SDK sampling, negative dBm and channel in one word, unknown while disconnected, low signal logged once\n";
}
int main(){policy();sampler();}
'''


def main():
    with tempfile.TemporaryDirectory(prefix='amity-wifi-signal-') as directory:
        temp = Path(directory)
        for name, text in HEADERS.items():
            (temp / name).write_text(text)
        cpp, binary = temp / 'test.cpp', temp / 'test'
        cpp.write_text(SUITE)
        subprocess.run(shlex.split(os.environ.get('CXX', 'c++')) + [
            '-std=c++17', '-Wall', '-Wextra', '-Werror', '-g', '-fsanitize=address,undefined',
            '-I' + str(temp), '-I' + str(ROOT / 'components/network_diagnostics/include'),
            str(cpp), str(ROOT / 'components/network_diagnostics/wifi_signal.cpp'), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
