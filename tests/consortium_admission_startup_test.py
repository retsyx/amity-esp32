#!/usr/bin/env python3
"""Exercise admission-independent startup and member connection-loop ordering."""
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
#include "controller_policy.hpp"
#include <map>
#include <algorithm>
#include <atomic>
#include <functional>
#include <future>
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
unsigned full_checks=0,profile_checks=0,created_tasks=0,recoveries=0,group_reads=0,identity_loads=0,erases=0,remote_shutdowns=0;
bool crypto_ok=true,identity_ok=true,psa_ok=true,group_ok=true,remote_ok=true;
std::map<std::string,std::string> records;std::string fail_read;
AdmissionMode tested_mode=AdmissionMode::button;
AdmissionMode configured_admission_mode(){return selected_mode;}
bool admission_crypto_self_test(){++full_checks;return crypto_ok;}
bool admission_crypto_self_test(AdmissionMode mode){++profile_checks;tested_mode=mode;return crypto_ok;}
constexpr int PSA_SUCCESS=0,pdPASS=1,MALLOC_CAP_SPIRAM=1,MALLOC_CAP_8BIT=2;
constexpr int ESP_ERR_NVS_NOT_FOUND=0x1102;
int psa_crypto_init(){return psa_ok?PSA_SUCCESS:-1;}
#define ESP_LOGW(...) do {} while(0)
std::mutex lock,connection_lock;
std::atomic_bool initialized{false};
Group group,seed;Identity identity;RuntimeStatus status;HostnameRegistry hostnames;
void error(std::string value){status.last_error=std::move(value);}
model::ConsortiumId singleton;model::BootId boot;uint64_t configuration=0;
bool candidate_passive=false;
RuntimeRadioHooks radio;uint64_t pending_radio_release=0,candidate_deadline=0,candidate_generation=0;
struct ConnectionHealth{bool available=true;bool usable()const{return available;}};
struct Active{bool cancelled=false;model::OctopusId peer,connection_peer;std::shared_ptr<ConnectionHealth> tls=std::make_shared<ConnectionHealth>();};std::vector<std::shared_ptr<Active>> links;
esp_err_t load_group(Group& value){++group_reads;if(!group_ok)return ESP_FAIL;value=seed;return ESP_OK;}
namespace platform{esp_err_t load_identity(model::OctopusId owner,Identity& value){++identity_loads;if(!identity_ok)return ESP_FAIL;value={owner,"validated-leaf","private-key-"+std::to_string(identity_loads),std::string(64,'a')};return ESP_OK;}}
esp_err_t read_record(const char* space,const char* key,std::string& value,size_t cap){const auto name=std::string(space)+"/"+key;if(name==fail_read)return ESP_FAIL;const auto found=records.find(name);if(found==records.end())return ESP_ERR_NVS_NOT_FOUND;if(found->second.size()>cap)return ESP_ERR_INVALID_SIZE;value=found->second;return ESP_OK;}
esp_err_t erase_record(const char*,const char*){++erases;return ESP_OK;}
// Recovery task creation is a separate service seam; created_tasks counts
// operational workers only, while the public recovery status matches firmware.
esp_err_t recovery_usb(model::OctopusId,esp_err_t result,const char*){++recoveries;status.initialized=true;status.credential_ready=false;status.authority_ready=false;status.coordinator=false;status.member=true;status.phase="recovery_required";return result;}
int ble_remote_set_controller_enabled(bool enabled){check(!enabled,"headless role enabled remote");++remote_shutdowns;return remote_ok?ESP_OK:ESP_FAIL;}
void usb_worker(void*){}
int xTaskCreateWithCaps(void(*)(void*),const char*,unsigned,void*,unsigned,void*,unsigned){++created_tasks;return pdPASS;}
uint64_t clock_ms=0,netif_exists_at=0,netif_up_at=0,ip_ready_at=0,ip_query_success_at=0;
unsigned early_connects=0,enrollment_closes=0;
std::vector<uint64_t> discovery_times;std::function<void()> on_loop_delay;
struct esp_netif_t{} local_station;
struct esp_netif_ip_info_t{struct{uint32_t addr=0;}ip;};
esp_netif_t* esp_netif_get_handle_from_ifkey(const char* key){check(std::string(key)=="WIFI_STA_DEF","wrong local interface checked");return clock_ms>=netif_exists_at?&local_station:nullptr;}
bool esp_netif_is_netif_up(esp_netif_t* station){check(station==&local_station,"wrong interface checked");return clock_ms>=netif_up_at;}
esp_err_t esp_netif_get_ip_info(esp_netif_t* station,esp_netif_ip_info_t* info){check(station==&local_station,"wrong interface queried");info->ip.addr=clock_ms>=ip_ready_at?1:0;return clock_ms>=ip_query_success_at?ESP_OK:ESP_FAIL;}

uint64_t transport_idle_at=0;
uint64_t now_ms(){return clock_ms;}
unsigned pdMS_TO_TICKS(unsigned value){return value;}
std::vector<unsigned> delays;
void vTaskDelay(unsigned value){delays.push_back(value);clock_ms+=value;if(on_loop_delay)on_loop_delay();if(clock_ms>30000)throw std::runtime_error("connection loop exceeded existing bounded schedule");}
bool recover_candidate_network(){return true;}
bool gatt_candidate_transport_idle(){return clock_ms>=transport_idle_at;}
bool gatt_quiescent(){return true;}
void advertise_group_hint(const Group&){}
void reap_closed_links(){links.erase(std::remove_if(links.begin(),links.end(),[](const auto& active){return active->cancelled;}),links.end());}
void runtime_close_enrollment(){++enrollment_closes;candidate_deadline=candidate_generation=0;}
unsigned discoveries=0,connect_failures=0,setup_failures=0,launch_failures=0,launches=0;bool retain_launched=false;
std::vector<std::string> attempts;
std::vector<uint64_t> attempt_times;
std::string resolved;
std::vector<std::string> discover_group_hints(const Group&){++discoveries;discovery_times.push_back(clock_ms);clock_ms+=2000;return {"192.0.2.2"};}
std::string random_hex(size_t){return "0";}
int listener=-1;
int fake_socket(int,int,int){return 42;}
int fake_close(int){return 0;}
int fake_getaddrinfo(const char* host,const char* port,const addrinfo* hints,addrinfo** out){
    check(std::string(port)=="7443"&&hints->ai_family==AF_INET&&hints->ai_flags==AI_NUMERICHOST,"endpoint lookup became DNS or changed service");
    resolved=host;static sockaddr address{};static addrinfo result{};result.ai_addr=&address;result.ai_addrlen=sizeof(address);result.ai_family=AF_INET;*out=&result;return 0;
}
void fake_freeaddrinfo(addrinfo*){}
bool connect_bounded(int,const sockaddr*,socklen_t){attempts.push_back(resolved);attempt_times.push_back(clock_ms);if(clock_ms<ip_ready_at||clock_ms<netif_exists_at||clock_ms<netif_up_at||clock_ms<ip_query_success_at){++early_connects;return false;}if(connect_failures){--connect_failures;clock_ms+=5000;return false;}return true;}
struct TlsLink{
    bool setup(const Identity& local,const std::vector<Peer>& peers,bool server,int,model::OctopusId expected={}){
        check(local.der=="validated-leaf"&&!server&&peers.size()==1&&peers[0].id==group.authority&&expected==group.authority,"known endpoint bypassed exact peer setup");
        if(setup_failures){--setup_failures;return false;}return true;
    }
};
struct Connected{};
void launch(std::shared_ptr<TlsLink>,std::string endpoint={},model::OctopusId expected={}){++launches;check(endpoint==attempts.back()&&expected==group.authority,"launch lost selected endpoint or owner");if(launch_failures){--launch_failures;return;}if(retain_launched){auto active=std::make_shared<Active>();active->connection_peer=expected;std::lock_guard<std::mutex> guard(lock);links.push_back(active);return;}throw Connected{};}
}
namespace config_store{bool production_storage_self_test(){return true;}}
namespace amity::consortium {
'''
SUITE = r'''
void reset(){
    initialized=false;status={};identity={};group={};seed={};hostnames={};singleton={};boot={};configuration=0;candidate_passive=false;
    full_checks=profile_checks=created_tasks=recoveries=group_reads=identity_loads=erases=remote_shutdowns=0;crypto_ok=identity_ok=psa_ok=group_ok=remote_ok=true;records.clear();fail_read.clear();platform::recorded_setup_timings.clear();
    clock_ms=transport_idle_at=netif_exists_at=netif_up_at=ip_ready_at=ip_query_success_at=0;early_connects=enrollment_closes=0;discovery_times.clear();on_loop_delay={};delays.clear();discoveries=connect_failures=setup_failures=launch_failures=launches=0;retain_launched=false;attempts.clear();attempt_times.clear();links.clear();candidate_deadline=candidate_generation=pending_radio_release=0;radio={};
}
void seed_role(Group::Role role){
 seed.role=role;if(role==Group::unclaimed)return;
 seed.consortium=ident<model::ConsortiumId>(1);seed.epoch=seed.revision=1;seed.authority=ident<model::OctopusId>(role==Group::coordinator?2:9);
 seed.joining_id=ident<model::OctopusId>(2);if(role==Group::member||role==Group::prepared)seed.peers={{seed.authority,"validated-authority","validated-pin",role==Group::member}};
 records["amity_group/identity"]=records["amity_group/identity_tmp"]="same-exact-record";
}
esp_err_t start_runtime(){return runtime_start(ident<model::ConsortiumId>(1),ident<model::OctopusId>(2),ident<model::BootId>(3),1);}
void check_phases(){
 for(const char* stage:{"total","load_group","startup_validation","identity_load"}){
  const auto matches=std::count_if(platform::recorded_setup_timings.begin(),platform::recorded_setup_timings.end(),[&](const auto& row){return std::string(row.area)=="startup"&&std::string(row.entry.stage)==stage&&std::string(row.entry.result)=="ok"&&row.entry.count==(std::string(stage)=="total"?0u:1u);});
  check(matches==1,"startup phase missing, duplicated or falsely counted");
 }
 check(platform::recorded_setup_timings.size()==4,"startup diagnostic capacity/phase count changed");
}
void startup_tests(){
 reset();seed_role(Group::coordinator);
 for(unsigned n=0;n<3;++n)seed.peers.push_back({ident<model::OctopusId>(10+n),"validated-peer",std::string(64,'a'+n),true});
 check(start_runtime()==ESP_OK&&group.peers.size()==3&&status.authority_ready&&created_tasks==2,"main with three secondary Amities failed operational startup");
 for(auto mode:{AdmissionMode::button,AdmissionMode::setup_code}){
  for(auto role:{Group::unclaimed,Group::coordinator}){
   reset();seed_role(role);selected_mode=mode;
   check(start_runtime()==ESP_OK,"admission-capable startup failed");
   std::cout<<"admission_role="<<unsigned(role)<<" group_reads="<<group_reads<<" identity_loads="<<identity_loads<<" profile_tests="<<profile_checks<<'\n';
   check(group_reads==1&&identity_loads==1&&identity.key_der=="private-key-1","runtime_start repeated validation or lost exact verified identity");
   check(profile_checks==0&&full_checks==0&&status.initialized&&status.credential_ready&&created_tasks==2,"startup ran an admission test or lost operational readiness");
   check(status.authority_ready==(role==Group::coordinator)&&status.coordinator==(role==Group::coordinator)&&!status.member,"main/unclaimed authority changed");check_phases();
   check(runtime_crypto_self_test()==ESP_OK&&full_checks==1,"explicit USB diagnostic stopped checking both profiles");
   reset();seed_role(role);selected_mode=mode;crypto_ok=false;
   check(start_runtime()==ESP_OK&&status.initialized&&status.credential_ready&&created_tasks==2&&group_reads==1&&identity_loads==1&&profile_checks==0&&full_checks==0,"unused admission crypto failure blocked startup");
   check(status.authority_ready==(role==Group::coordinator)&&status.coordinator==(role==Group::coordinator)&&!status.member,"unused admission crypto failure changed authority");check_phases();
   check(runtime_crypto_self_test()!=ESP_OK&&full_checks==1&&status.initialized&&status.credential_ready,"explicit diagnostic failure was skipped or changed operational readiness");
  }
  for(auto role:{Group::prepared,Group::member,Group::retired}){
   reset();seed_role(role);selected_mode=mode;crypto_ok=false;
   check(start_runtime()==ESP_OK,"headless operational boot depended on unused admission test");
   check(group_reads==1&&identity_loads==1&&identity.key_der=="private-key-1"&&profile_checks==0&&full_checks==0,"headless startup repeated credential/admission work");
   check(status.initialized&&status.credential_ready&&!status.authority_ready&&!status.coordinator&&created_tasks==2&&remote_shutdowns==1,"headless startup changed authority/passivity");
   check(status.member==(role!=Group::retired)&&candidate_passive==(role!=Group::retired),"stored headless role changed");check_phases();
   check(runtime_crypto_self_test()!=ESP_OK&&full_checks==1&&status.initialized&&status.credential_ready,"explicit diagnostic failure was skipped or changed operational readiness");
  }
 }
 // Failure before operational readiness remains fail closed for every role.
 for(auto role:{Group::unclaimed,Group::coordinator,Group::prepared,Group::member,Group::retired}){
  for(unsigned failure=0;failure<8;++failure){
   reset();seed_role(role);
   if(failure==0)psa_ok=false;
   if(failure==1)group_ok=false;
   if(failure==2)identity_ok=false;
   if(failure==3)records["amity_handover/active"]="marker";
   if(failure==4)fail_read="amity_handover/proposal0";
   if(failure==5){if(role==Group::unclaimed)continue;records["amity_group/identity_tmp"]="different";}
   if(failure==6){if(role==Group::unclaimed)continue;records.erase("amity_group/identity");}
   if(failure==7){if(role==Group::unclaimed)continue;seed.epoch=2;}
   check(start_runtime()!=ESP_OK&&recoveries==1&&status.initialized&&status.phase=="recovery_required"&&!status.credential_ready&&!created_tasks&&!profile_checks,"invalid PSA/group/journal/identity reached operational readiness");
  }
 }
 for(unsigned failure=0;failure<4;++failure){
  reset();seed_role(failure==1?Group::coordinator:Group::member);
  if(failure==0)seed.joining_id=ident<model::OctopusId>(99);
  if(failure==1)seed.authority=ident<model::OctopusId>(99);
  if(failure==2)seed.peers.resize(maximum_roster_peers+1);
  if(failure==3)remote_ok=false;
  check(start_runtime()!=ESP_OK&&recoveries==1&&!created_tasks&&!status.credential_ready,"headless authority/localID/capacity/remote shutdown check lost");
 }
}
void headless_only(){reset();seed_role(Group::member);crypto_ok=false;const auto result=start_runtime();std::cout<<"headless_group_reads="<<group_reads<<" identity_loads="<<identity_loads<<" admission_tests="<<profile_checks<<" result="<<result<<'\n';check(result==ESP_OK&&status.initialized&&status.credential_ready&&profile_checks==0&&created_tasks==2,"unused admission self-test blocked headless operational startup");}
void fresh_public_preflight(){
 const auto local=ident<model::OctopusId>(2);RuntimeStartup startup;
 reset();identity_ok=false;const auto empty=records;
 check(runtime_read_startup(local,startup)==ESP_OK&&startup.role==RuntimeStartup::unclaimed&&group_reads==1&&identity_loads==0&&records==empty&&!erases,"unclaimed preflight generated identity or changed storage");
 reset();seed_role(Group::member);auto stored=records;
 check(runtime_read_startup(local,startup)==ESP_OK&&startup.role==RuntimeStartup::member&&group_reads==1&&identity_loads==1&&records==stored&&!erases,"public member preflight failed exact validation");
 seed.role=Group::prepared;
 check(runtime_read_startup(local,startup)==ESP_OK&&startup.role==RuntimeStartup::prepared&&group_reads==2&&identity_loads==2,"public preflight reused an old boot/call cache");
 records["amity_group/identity_tmp"]="changed";
 check(runtime_read_startup(local,startup)!=ESP_OK&&startup.passive()&&startup.role==RuntimeStartup::recovery_required&&group_reads==3&&identity_loads==2,"changed public journal skipped fresh validation");
 reset();seed_role(Group::member);check(runtime_read_startup(local,startup)==ESP_OK,"pre-start preflight failed");
 check(group_reads==1&&identity_loads==1&&start_runtime()==ESP_OK&&group_reads==2&&identity_loads==2&&identity.key_der=="private-key-2","runtime_start reused a prior public preflight instead of validating its own records");
}
void setup_member(bool hint=true){reset();group.role=Group::member;group.authority=ident<model::OctopusId>(9);group.authority_host=hint?"192.0.2.1":"";group.peers={{group.authority,"exact-leaf","exact-pin",true}};identity={ident<model::OctopusId>(8),"validated-leaf","key","pin"};}
void run_network(){try{network_loop(nullptr);}catch(const Connected&){return;}catch(const std::exception& error){throw std::runtime_error(std::string(error.what())+" attempts="+std::to_string(attempts.size())+" discoveries="+std::to_string(discoveries)+" launches="+std::to_string(launches));}throw std::runtime_error("loop returned without authenticated transport setup");}
void local_ipv4_tests(){
 for(auto role:{Group::member,Group::prepared}){
  setup_member();group.role=role;candidate_passive=role==Group::prepared;ip_ready_at=300;
  unsigned released=0;pending_radio_release=7;radio.release=[&](uint64_t generation){check(generation==7,"wrong radio released");++released;};candidate_generation=8;candidate_deadline=150;
  run_network();
  std::cout<<"local_ipv4_role="<<unsigned(role)<<" early_tcp="<<early_connects<<" first_tcp_ms="<<attempt_times.front()<<" last_tcp_ms="<<attempt_times.back()<<" mdns="<<discoveries<<'\n';
  check(early_connects==0&&attempts==std::vector<std::string>{"192.0.2.1"}&&attempt_times.front()==300&&discoveries==0,"missing local IPv4 consumed known hint/backoff/discovery");
  check(released==1&&enrollment_closes==1&&delays==std::vector<unsigned>({100,100,100}),"waiting for address blocked radio/deadline work or added a retry schedule");
  const auto spans=std::count_if(platform::recorded_setup_timings.begin(),platform::recorded_setup_timings.end(),[&](const auto& row){return std::string(row.area)=="network"&&std::string(row.entry.stage)=="local_ipv4_wait"&&row.entry.at_ms==300&&row.entry.elapsed_ms==300&&row.peer==group.authority&&std::string(row.entry.result)=="ok";});
  check(spans==1,"address wait did not emit exactly one bounded truthful completion span");
 }
 for(unsigned missing=0;missing<3;++missing){
  setup_member();if(missing==0)netif_exists_at=300;if(missing==1)netif_up_at=300;if(missing==2)ip_query_success_at=300;
  run_network();check(attempt_times.front()==300&&attempts.size()==1&&!early_connects&&!discoveries,"missing/down/query-failed local interface reached TCP");
 }
 setup_member(false);ip_ready_at=300;run_network();
 check(!early_connects&&discovery_times==std::vector<uint64_t>{300}&&attempt_times.front()==2300&&attempts==std::vector<std::string>{"192.0.2.2"},"no-hint member queried before local address or lost bounded discovery");
 setup_member();ip_ready_at=300;connect_failures=1;run_network();
 check(!early_connects&&attempts==std::vector<std::string>({"192.0.2.1","192.0.2.2"})&&attempt_times.front()==300&&attempt_times.back()==8448&&discoveries==1,"genuine post-address connection failure lost original backoff/discovered fallback");
 struct Stopped{};
 for(bool connected:{false,true}){
  setup_member();ip_ready_at=UINT64_MAX;if(connected){auto active=std::make_shared<Active>();active->connection_peer=group.authority;links.push_back(active);}
  unsigned released=0;pending_radio_release=7;radio.release=[&](uint64_t generation){check(generation==7,"wrong radio release while no address");++released;};candidate_generation=8;candidate_deadline=150;
  on_loop_delay=[&]{if(clock_ms>=500)throw Stopped{};};
  try{network_loop(nullptr);}catch(const Stopped&){}
  check(clock_ms==500&&attempts.empty()&&discoveries==0&&released==1&&enrollment_closes==1,"absent IP blocked bookkeeping or generated TCP/mDNS work");
  check(links.size()==(connected?1u:0u)&&platform::recorded_setup_timings.empty(),"address wait changed existing connection or produced periodic logs");
 }
}
void implicit_connection_acquisition(){
 setup_member();retain_launched=true;
 check(attempts.empty()&&links.empty(),"connection opened without data demand");
 auto first=std::async(std::launch::async,[&]{ensure_connection(identity,group,"192.0.2.1");});
 auto second=std::async(std::launch::async,[&]{ensure_connection(identity,group,"192.0.2.1");});first.get();second.get();
 check(attempts==std::vector<std::string>{"192.0.2.1"}&&launches==1&&links.size()==1,"simultaneous TLS demand created competing sockets");
 const auto existing=links.front();ensure_connection(identity,group,"192.0.2.2");
 check(attempts.size()==1&&launches==1&&links.front()==existing&&!existing->cancelled,"data demand replaced the pending or established connection");
 existing->peer=group.authority;existing->connection_peer={};ensure_connection(identity,group,"192.0.2.2");
 check(attempts.size()==1&&launches==1,"authenticated connection was not reused after pending ownership cleared");
 existing->cancelled=true;ensure_connection(identity,group,"192.0.2.2");
 check(attempts==std::vector<std::string>({"192.0.2.1","192.0.2.2"})&&launches==2&&links.size()==2,"dead connection prevented implicit acquisition");
 setup_member();retain_launched=true;ensure_connection(identity,group,"192.0.2.1");const auto broken=links.front();broken->tls->available=false;
 check(!broken->cancelled&&!connection_available(group.authority),"known TLS failure remained available until worker cancellation");
 ensure_connection(identity,group,"192.0.2.2");
 check(attempts==std::vector<std::string>({"192.0.2.1","192.0.2.2"})&&launches==2&&links.size()==2&&!broken->cancelled,"known TLS failure prevented implicit acquisition");
 setup_member();retain_launched=true;auto unrelated=std::make_shared<Active>();unrelated->peer=ident<model::OctopusId>(99);links.push_back(unrelated);ensure_connection(identity,group,"192.0.2.1");
 check(attempts.size()==1&&launches==1&&links.size()==2&&!unrelated->cancelled,"unrelated peer was treated as the requested peer's connection");
}
void connection_tests(){
    implicit_connection_acquisition();local_ipv4_tests();
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
int main(){using namespace amity::consortium;try{startup_tests();fresh_public_preflight();connection_tests();std::cout<<"PASS actual startup/connection loop: once-per-call group+identity verification; fresh read-only preflight; role and storage failure fences; no boot admission tests; full explicit self-test; unchanged pinned IP/discovery fallback\n";return 0;}catch(const std::exception& error){std::cerr<<"FAIL "<<error.what()<<'\n';return 1;}}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT / 'components/consortium/runtime.cpp')
    parser.add_argument('--only', choices=['startup', 'network', 'headless', 'ipv4'])
    args = parser.parse_args()
    source = args.source.read_text()
    actual = ('#define socket amity::consortium::fake_socket\n#define close amity::consortium::fake_close\n#define getaddrinfo amity::consortium::fake_getaddrinfo\n#define freeaddrinfo amity::consortium::fake_freeaddrinfo\n'
              + function(source, 'bool connection_available(')
              + function(source, 'void ensure_connection(')
              + (function(source, 'bool local_ipv4_ready(') if 'bool local_ipv4_ready(' in source else '')
              + function(source, 'void network_loop(')
              + '\n#undef socket\n#undef close\n#undef getaddrinfo\n#undef freeaddrinfo\n'
              + (function(source, 'esp_err_t validate_startup(') if 'esp_err_t validate_startup(' in source else '')
              + function(source, 'esp_err_t runtime_read_startup(')
              + function(source, 'esp_err_t runtime_start(')
              + function(source, 'esp_err_t runtime_crypto_self_test('))
    suite = SUITE
    if args.only == 'startup':
        suite = suite.replace('startup_tests();fresh_public_preflight();connection_tests();', 'startup_tests();fresh_public_preflight();')
    if args.only == 'headless':
        suite = suite.replace('startup_tests();fresh_public_preflight();connection_tests();', 'headless_only();')
    if args.only == 'ipv4':
        suite = suite.replace('startup_tests();fresh_public_preflight();connection_tests();', 'local_ipv4_tests();')
    if args.only == 'network':
        suite = suite.replace('startup_tests();fresh_public_preflight();connection_tests();', 'connection_tests();')
    with tempfile.TemporaryDirectory(prefix='amity-admission-startup-') as directory:
        cpp, binary = Path(directory) / 'test.cpp', Path(directory) / 'test'
        cpp.write_text(timing_support() + PREFIX + actual + suite)
        includes = ['tests/reset_fakes', 'components/consortium', 'components/consortium/include', 'components/amity_system/include']
        subprocess.run(shlex.split(os.environ.get('CXX', 'c++')) + [
            '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pthread', '-g', '-fsanitize=address,undefined',
            *['-I' + str(ROOT / path) for path in includes], str(cpp),
            str(ROOT / 'components/consortium/json.cpp'), str(ROOT / 'components/amity_system/model.cpp'), '-o', str(binary)], check=True)
        return subprocess.run([str(binary)], check=False).returncode


if __name__ == '__main__':
    raise SystemExit(main())
