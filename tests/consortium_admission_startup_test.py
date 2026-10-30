#!/usr/bin/env python3
"""Exercise actual crypto startup dispatch and member connection-loop ordering."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

from consortium_gatt_callback_test import function
from setup_timing_support import timing_support

ROOT = Path(__file__).resolve().parents[1]
PREFIX = r'''
#include "amity/consortium/runtime.hpp"
#include "amity/consortium/credential_types.hpp"
#include "hostname_registry.hpp"
#include <algorithm>
#include <atomic>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>
namespace amity::consortium {
using namespace platform;
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
template<class T>T ident(unsigned value){char text[33];std::snprintf(text,sizeof(text),"%032x",value);return *T::parse(text);}
enum class AdmissionMode{button,setup_code};
AdmissionMode selected_mode=AdmissionMode::button;
unsigned full_checks=0,profile_checks=0,created_tasks=0,recoveries=0;
bool crypto_ok=true,identity_ok=true;
AdmissionMode tested_mode=AdmissionMode::button;
AdmissionMode configured_admission_mode(){return selected_mode;}
bool admission_crypto_self_test(){++full_checks;return crypto_ok;}
bool admission_crypto_self_test(AdmissionMode mode){++profile_checks;tested_mode=mode;return crypto_ok;}
constexpr int PSA_SUCCESS=0,pdPASS=1,MALLOC_CAP_SPIRAM=1,MALLOC_CAP_8BIT=2;
constexpr int ESP_ERR_NVS_NOT_FOUND=0x1102;
int psa_crypto_init(){return PSA_SUCCESS;}
#define ESP_LOGW(...) do {} while(0)
std::mutex lock;
std::atomic_bool initialized{false};
Group group,seed;Identity identity;RuntimeStatus status;HostnameRegistry hostnames;
void error(std::string value){status.last_error=std::move(value);}
model::ConsortiumId singleton;model::BootId boot;uint64_t configuration=0;
bool candidate_passive=false;
RuntimeRadioHooks radio;uint64_t pending_radio_release=0,candidate_deadline=0,candidate_generation=0;
struct Active{bool cancelled=false;};std::vector<std::shared_ptr<Active>> links;
esp_err_t load_group(Group& value){value=seed;return ESP_OK;}
namespace platform{esp_err_t load_identity(model::OctopusId owner,Identity& value){if(!identity_ok)return ESP_FAIL;value={owner,"validated-leaf","private-key",std::string(64,'a')};return ESP_OK;}}
esp_err_t runtime_read_startup(model::OctopusId,RuntimeStartup& value){value={};return ESP_OK;}
esp_err_t read_record(const char*,const char*,std::string&,size_t){return ESP_ERR_NVS_NOT_FOUND;}
esp_err_t erase_record(const char*,const char*){return ESP_OK;}
bool original_controller(const Group& value,model::OctopusId owner){return value.role==Group::coordinator&&value.authority==owner;}
esp_err_t recovery_usb(model::OctopusId,esp_err_t result,const char*){++recoveries;return result;}
int ble_remote_set_controller_enabled(bool){return ESP_OK;}
void usb_worker(void*){}
int xTaskCreateWithCaps(void(*)(void*),const char*,unsigned,void*,unsigned,void*,unsigned){++created_tasks;return pdPASS;}
uint64_t clock_ms=0;
uint64_t transport_idle_at=0;
uint64_t now_ms(){return clock_ms;}
unsigned pdMS_TO_TICKS(unsigned value){return value;}
std::vector<unsigned> delays;
void vTaskDelay(unsigned value){delays.push_back(value);clock_ms+=value;if(clock_ms>30000)throw std::runtime_error("connection loop exceeded existing bounded schedule");}
bool recover_candidate_network(){return true;}
bool gatt_candidate_transport_idle(){return clock_ms>=transport_idle_at;}
bool gatt_quiescent(){return true;}
void advertise_group_hint(const Group&){}
void reap_closed_links(){links.erase(std::remove_if(links.begin(),links.end(),[](const auto& active){return active->cancelled;}),links.end());}
void runtime_close_enrollment(){}
unsigned discoveries=0,connect_failures=0,setup_failures=0,launch_failures=0,launches=0;
std::vector<std::string> attempts;
std::vector<uint64_t> attempt_times;
std::string resolved;
std::vector<std::string> discover_group_hints(const Group&){++discoveries;clock_ms+=2000;return {"192.0.2.2"};}
std::string random_hex(size_t){return "0";}
int listener=-1;
int fake_socket(int,int,int){return 42;}
int fake_close(int){return 0;}
int fake_getaddrinfo(const char* host,const char* port,const addrinfo* hints,addrinfo** out){
    check(std::string(port)=="7443"&&hints->ai_family==AF_INET&&hints->ai_flags==AI_NUMERICHOST,"endpoint lookup became DNS or changed service");
    resolved=host;static sockaddr address{};static addrinfo result{};result.ai_addr=&address;result.ai_addrlen=sizeof(address);result.ai_family=AF_INET;*out=&result;return 0;
}
void fake_freeaddrinfo(addrinfo*){}
bool connect_bounded(int,const sockaddr*,socklen_t){attempts.push_back(resolved);attempt_times.push_back(clock_ms);if(connect_failures){--connect_failures;clock_ms+=5000;return false;}return true;}
struct TlsLink{
    bool setup(const Identity& local,const std::vector<Peer>& peers,bool server,int,model::OctopusId expected={}){
        check(local.der=="validated-leaf"&&!server&&peers.size()==1&&peers[0].id==group.authority&&expected==group.authority,"known endpoint bypassed exact peer setup");
        if(setup_failures){--setup_failures;return false;}return true;
    }
};
struct Connected{};
void launch(std::shared_ptr<TlsLink>,std::string endpoint={}){++launches;check(endpoint==attempts.back(),"launch lost selected endpoint");if(launch_failures){--launch_failures;return;}throw Connected{};}
}
namespace config_store{bool production_storage_self_test(){return true;}}
namespace amity::consortium {
'''
SUITE = r'''
void reset(){
    initialized=false;status={};identity={};group={};seed={};hostnames={};singleton={};boot={};configuration=0;candidate_passive=false;
    full_checks=profile_checks=created_tasks=recoveries=0;crypto_ok=identity_ok=true;
    clock_ms=transport_idle_at=0;delays.clear();discoveries=connect_failures=setup_failures=launch_failures=launches=0;attempts.clear();attempt_times.clear();links.clear();candidate_deadline=candidate_generation=pending_radio_release=0;radio={};
}
void startup_tests(){
    for(auto mode:{AdmissionMode::button,AdmissionMode::setup_code}){
        reset();selected_mode=mode;
        check(runtime_start(ident<model::ConsortiumId>(1),ident<model::OctopusId>(2),ident<model::BootId>(3),1)==ESP_OK,"valid crypto startup failed");
        check(profile_checks==1&&tested_mode==mode&&full_checks==0&&status.crypto_ready&&created_tasks==2,"actual runtime_start ran disabled profile or launched before crypto proof");
        check(runtime_crypto_self_test()==ESP_OK&&full_checks==1,"explicit USB diagnostic stopped checking both profiles");
        reset();selected_mode=mode;crypto_ok=false;
        check(runtime_start(ident<model::ConsortiumId>(1),ident<model::OctopusId>(2),ident<model::BootId>(3),1)!=ESP_OK&&!status.crypto_ready&&!created_tasks,"failed selected crypto started network workers");
    }
    reset();identity_ok=false;
    check(runtime_start(ident<model::ConsortiumId>(1),ident<model::OctopusId>(2),ident<model::BootId>(3),1)!=ESP_OK&&recoveries==1&&profile_checks==0&&!created_tasks,"invalid identity reached crypto-ready network startup");
}
void setup_member(bool hint=true){reset();group.role=Group::member;group.authority=ident<model::OctopusId>(9);group.authority_host=hint?"192.0.2.1":"";group.peers={{group.authority,"exact-leaf","exact-pin",true}};identity={ident<model::OctopusId>(8),"validated-leaf","key","pin"};}
void run_network(){try{network_loop(nullptr);}catch(const Connected&){return;}catch(const std::exception& error){throw std::runtime_error(std::string(error.what())+" attempts="+std::to_string(attempts.size())+" discoveries="+std::to_string(discoveries)+" launches="+std::to_string(launches));}throw std::runtime_error("loop returned without authenticated transport setup");}
void connection_tests(){
    setup_member();run_network();check(attempts==std::vector<std::string>{"192.0.2.1"}&&attempt_times.front()==0&&discoveries==0,"known authority waited for blocking mDNS before first attempt");
    setup_member(false);run_network();check(attempts==std::vector<std::string>{"192.0.2.2"}&&attempt_times.front()==2000&&discoveries==1,"missing hint lost bounded discovery");
    for(unsigned failure=0;failure<3;++failure){
        setup_member();connect_failures=failure==0;setup_failures=failure==1;launch_failures=failure==2;run_network();
        check(attempts==std::vector<std::string>({"192.0.2.1","192.0.2.2"})&&attempt_times.front()==0&&discoveries==1,"failed IP/connect/TLS attempt suppressed discovered fallback");
        check(delays.size()>=2&&delays[0]>=1000&&delays[0]<1100&&delays[1]==100,"existing backoff or task yielding changed");
    }
    setup_member();group.role=Group::prepared;candidate_passive=true;run_network();check(attempt_times.front()==0,"prepared transaction retained pre-connect discovery wait");
    setup_member();group.role=Group::prepared;candidate_passive=true;transport_idle_at=300;run_network();check(attempt_times.front()==300&&discoveries==0,"IP-first bypassed prepared GATT transport-idle fence");
}
}
int main(){using namespace amity::consortium;try{startup_tests();connection_tests();std::cout<<"PASS actual runtime startup and connection loop: selected-profile crypto failure gates; full explicit self-test; immediate known IP with exact peer TLS setup; bounded discovery/fallback after connect/setup/rejected-link failure\n";return 0;}catch(const std::exception& error){std::cerr<<"FAIL "<<error.what()<<'\n';return 1;}}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT / 'components/consortium/runtime.cpp')
    parser.add_argument('--only', choices=['startup', 'network'])
    args = parser.parse_args()
    source = args.source.read_text()
    actual = ('#define socket amity::consortium::fake_socket\n#define close amity::consortium::fake_close\n#define getaddrinfo amity::consortium::fake_getaddrinfo\n#define freeaddrinfo amity::consortium::fake_freeaddrinfo\n'
              + function(source, 'void network_loop(')
              + '\n#undef socket\n#undef close\n#undef getaddrinfo\n#undef freeaddrinfo\n'
              + function(source, 'esp_err_t runtime_start(')
              + function(source, 'esp_err_t runtime_crypto_self_test('))
    suite = SUITE
    if args.only == 'startup':
        suite = suite.replace('startup_tests();connection_tests();', 'startup_tests();')
    if args.only == 'network':
        suite = suite.replace('startup_tests();connection_tests();', 'connection_tests();')
    with tempfile.TemporaryDirectory(prefix='amity-admission-startup-') as directory:
        cpp, binary = Path(directory) / 'test.cpp', Path(directory) / 'test'
        cpp.write_text(timing_support() + PREFIX + actual + suite)
        includes = ['tests/reset_fakes', 'components/consortium', 'components/consortium/include', 'components/amity_system/include']
        subprocess.run(shlex.split(os.environ.get('CXX', 'c++')) + [
            '-std=c++17', '-Wall', '-Wextra', '-Werror', '-g', '-fsanitize=address,undefined',
            *['-I' + str(ROOT / path) for path in includes], str(cpp),
            str(ROOT / 'components/consortium/json.cpp'), str(ROOT / 'components/amity_system/model.cpp'), '-o', str(binary)], check=True)
        return subprocess.run([str(binary)], check=False).returncode


if __name__ == '__main__':
    raise SystemExit(main())
