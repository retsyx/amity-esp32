#!/usr/bin/env python3
"""Execute the actual Wi-Fi policy and its startup position for both board roles.

SDK Wi-Fi calls and network startup are controlled seams. The production policy
and the unchanged surrounding startup statements execute together, so a policy
applied before Wi-Fi initialization, after transport startup, or only on the
main cannot satisfy the assertions. No radio, network or DUT is accessed.
"""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

from consortium_gatt_callback_test import function

ROOT = Path(__file__).resolve().parents[1]

PREFIX = r'''
#include <cassert>
#include <cstdio>
#include <iostream>
#include <optional>
#include <string>
#include <vector>
using esp_err_t=int;
constexpr int ESP_OK=0,ESP_ERR_INVALID_STATE=2;
enum wifi_ps_type_t {WIFI_PS_NONE,WIFI_PS_MIN_MODEM,WIFI_PS_MAX_MODEM};
struct Failure {int error;};
#define ESP_ERROR_CHECK(expression) do {const auto error=(expression);if(error!=ESP_OK)throw Failure{error};} while(0)
std::vector<std::string> calls,logs;
bool passive_member=false,initialized=false,ignore_set=false;
int network_error=0,before_error=0,set_error=0,after_error=0;
unsigned get_calls=0,set_calls=0,start_calls=0;
wifi_ps_type_t mode=WIFI_PS_MIN_MODEM;
struct amity_matter_callbacks_t {} callbacks;
int consortium_id=7,octopus_id=8,boot_value=9;
const int *boot=&boot_value;
esp_err_t amity_matter_start(amity_matter_callbacks_t *value) {
    assert(value==&callbacks&&!initialized);calls.push_back(passive_member?"member-network":"main-network");
    if(network_error)return network_error;
    initialized=true;return ESP_OK;
}
esp_err_t esp_wifi_get_ps(wifi_ps_type_t *result) {
    assert(initialized);++get_calls;calls.push_back("get");
    const auto error=get_calls==1?before_error:after_error;if(error)return error;
    *result=mode;return ESP_OK;
}
esp_err_t esp_wifi_set_ps(wifi_ps_type_t value) {
    assert(initialized&&get_calls==1);++set_calls;calls.push_back("set");
    assert(value==WIFI_PS_NONE);
    if(set_error)return set_error;
    if(!ignore_set)mode=value;
    return ESP_OK;
}
void log(const char *tag,const char *format,int previous,int current) {
    assert(std::string(tag)=="amity"&&get_calls==2&&set_calls==1);
    char line[128];const int size=std::snprintf(line,sizeof(line),format,previous,current);
    assert(size>0&&size<static_cast<int>(sizeof(line)));logs.emplace_back(line);calls.push_back("log");
}
#define ESP_LOGI(...) log(__VA_ARGS__)
namespace amity::network_diagnostics {
void start() {
    assert(initialized&&mode==WIFI_PS_NONE&&get_calls==2&&set_calls==1);
    calls.push_back("network-diagnostics");
}
}
struct Configuration {uint64_t config_revision=11;};
struct Hub {
    std::optional<Configuration> active_configuration(){return Configuration{};}
    unsigned supported_tentacles(){return 4;}
    unsigned configured_tentacles_mask(){return 15;}
} hub;
namespace amity::consortium {
esp_err_t runtime_set_hardware_profile(unsigned tentacles,unsigned mask) {
    assert(tentacles==4&&mask==15&&initialized&&mode==WIFI_PS_NONE&&get_calls==2&&set_calls==1);
    calls.push_back("profile");return ESP_OK;
}
esp_err_t runtime_start(int consortium,int owner,int actual_boot,uint64_t revision) {
    assert(consortium==consortium_id&&owner==octopus_id&&actual_boot==boot_value&&revision==11);
    assert(initialized&&mode==WIFI_PS_NONE&&get_calls==2&&set_calls==1);++start_calls;calls.push_back("transport");return ESP_OK;
}
}
'''

SUITE = r'''
void reset(bool member) {
    calls.clear();logs.clear();passive_member=member;initialized=ignore_set=false;
    network_error=before_error=set_error=after_error=0;get_calls=set_calls=start_calls=0;mode=WIFI_PS_MIN_MODEM;
}
void expect_failure(int expected) {
    try{startup();assert(false&&"Startup silently ignored a Wi-Fi policy failure");}
    catch(const Failure& failure){assert(failure.error==expected);}
    assert(start_calls==0&&std::find(calls.begin(),calls.end(),"profile")==calls.end());
}
int main() {
    for(bool member:{false,true}) {
        reset(member);assert(startup()==ESP_OK&&start_calls==1);
        assert((calls==std::vector<std::string>{member?"member-network":"main-network","get","set","get","log","network-diagnostics","profile","transport"}));
        assert(logs==std::vector<std::string>{"Wi-Fi power save: previous=1 current=0"});
        // One startup application, with no event callback or packet-path work.
        assert(mode==WIFI_PS_NONE&&get_calls==2&&set_calls==1);
        reset(member);mode=WIFI_PS_NONE;assert(startup()==ESP_OK&&start_calls==1);
        assert(logs==std::vector<std::string>{"Wi-Fi power save: previous=0 current=0"});
        reset(member);mode=WIFI_PS_MAX_MODEM;assert(startup()==ESP_OK&&start_calls==1);
        assert(logs==std::vector<std::string>{"Wi-Fi power save: previous=2 current=0"});
        reset(member);network_error=10;expect_failure(10);assert(get_calls==0&&set_calls==0&&logs.empty());
        reset(member);before_error=11;expect_failure(11);assert(get_calls==1&&set_calls==0&&logs.empty());
        reset(member);set_error=12;expect_failure(12);assert(get_calls==1&&set_calls==1&&logs.empty());
        reset(member);after_error=13;expect_failure(13);assert(get_calls==2&&set_calls==1&&logs.empty());
        reset(member);ignore_set=true;expect_failure(ESP_ERR_INVALID_STATE);
        assert(get_calls==2&&set_calls==1&&logs==std::vector<std::string>{"Wi-Fi power save: previous=1 current=1"});
    }
    std::cout<<"PASS actual Wi-Fi latency policy: all roles, initialization/order, observed modes and startup error propagation\n";
}
'''


def main():
    source = (ROOT / 'main/main.cpp').read_text()
    policy = function(source, 'esp_err_t configure_wifi_latency_policy(')
    start = source.index('    ESP_ERROR_CHECK(amity_matter_start(&callbacks));')
    end = source.index(';', source.index('    const auto group_start=', start)) + 1
    startup = 'int startup() {\n' + source[start:end] + '\nreturn group_start;\n}\n'
    with tempfile.TemporaryDirectory(prefix='amity-wifi-latency-') as directory:
        cpp, binary = Path(directory) / 'test.cpp', Path(directory) / 'test'
        cpp.write_text('#include <algorithm>\n' + PREFIX + policy + startup + SUITE)
        subprocess.run(shlex.split(os.environ.get('CXX', 'c++')) + [
            '-std=c++17', '-Wall', '-Wextra', '-Werror', '-g', '-fsanitize=address,undefined',
            str(cpp), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
