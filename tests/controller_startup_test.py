#!/usr/bin/env python3
"""Execute actual composition-layer role/setup helpers against instrumented services."""
from pathlib import Path
import os
import subprocess
import tempfile

root=Path(__file__).resolve().parents[1]
source=(root/'main/main.cpp').read_text()
def extract(signature):
    start=source.index(signature);opening=source.index('{',start);end=opening+1;depth=1
    while depth:
        if source[end]=='{':depth+=1
        elif source[end]=='}':depth-=1
        end+=1
    return source[start:end]
functions='\n'.join(extract(s) for s in ['void refresh_admin_mdns()', '[[gnu::noinline]] void setup_matter(', 'void setup_button(', 'esp_err_t retire_original_interfaces()', 'void start_admin_mdns()', 'void start_original_interfaces(', 'bool station_has_address(', 'void await_original_work(', 'void await_original_task('])
retained=extract('if(group_start==ESP_OK&&membership_read==ESP_OK&&membership.role==amity::consortium::RuntimeStartup::coordinator')
# Actual app_main composition boundaries, in addition to executed helper bodies.
main=source[source.index('extern "C" void app_main()'):]
assert main.index('system_services::confirm_firmware()')<main.index('xTaskCreate(await_original_task,')
assert 'const bool passive_startup=passive_member||awaiting_first_setup;' in main
assert 'web_admin::load(hub,passive_startup)' in main
assert main.index('hub.set_coordinator_authority(!passive_startup,')<main.index('amity_matter_start(&callbacks)')
assert source.count('web_admin::start(')==1 and source.count('amity_homekit::start(')==1
assert source.count('xTaskCreate(input_task,')==1
assert 'runtime_lifecycle_hooks({retire_original_interfaces,' in main

fixture=r'''
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstring>
#include <condition_variable>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
using esp_err_t=int;using esp_event_base_t=int;
constexpr int ESP_OK=0,ESP_ERR_INVALID_STATE=1,ESP_ERR_INVALID_ARG=2,ESP_ERR_NO_MEM=3,ESP_FAIL=4,pdPASS=1;
const char*esp_err_to_name(esp_err_t){return "injected error";}
unsigned uxTaskGetStackHighWaterMark(void*){return 1024;}
constexpr int IP_EVENT=1,ESP_EVENT_ANY_ID=-1,IP_EVENT_STA_GOT_IP=1,IP_EVENT_STA_LOST_IP=2,IP_EVENT_GOT_IP6=3;
#define ESP_ERROR_CHECK(expr) assert((expr)==ESP_OK)
template<class...A>void log(A&&...){ }
#define ESP_LOGI(...) log(__VA_ARGS__)
#define ESP_LOGW(...) log(__VA_ARGS__)
#define IPSTR "%u"
#define IP2STR(p) (*(p))
#define pdMS_TO_TICKS(v) (v)
std::atomic<bool>original_interfaces_enabled{false};
std::mutex admin_mdns_mutex;bool admin_mdns_registered=false;
bool responder_live=false;int mdns_initializations=0,mdns_removals=0,mdns_updates=0,matter_stops=0;
int fence_result=ESP_OK,web_result=ESP_OK,matter_result=ESP_OK,home_result=ESP_OK,mdns_remove_result=ESP_OK,mdns_add_result=ESP_OK;
std::mutex interleave_mutex;std::condition_variable interleave;
bool block_mdns_add=false,mdns_add_entered=false,release_mdns_add=false,retire_at_web=false;
std::vector<std::string>events;bool address_available=false,enrolled=false,firmware_healthy=false,claim_member_wins=false;
int claims=0,delays=0,setup_windows=0,enrollment_windows=0,remote_workers=0,delete_calls=0,getter_calls=0;bool active_candidate=false;
int radio_reservations=0,radio_releases=0,reserve_result=ESP_OK,enrollment_result=ESP_OK;
std::function<void()>on_delay;
namespace amity::consortium {
struct Role {static int instances;bool initialized=true,member=false,coordinator=false,authority_ready=false;std::string phase="awaiting_setup";
Role(){++instances;}Role(const Role&r):initialized(r.initialized),member(r.member),coordinator(r.coordinator),authority_ready(r.authority_ready),phase(r.phase){++instances;}~Role(){--instances;}};
int Role::instances=0;Role role;using RuntimeStatus=Role;
struct RuntimeStartup{enum{unclaimed,coordinator,member,retired};int role=unclaimed;};
Role runtime_status(){return role;}
bool runtime_original_setup_eligible(){return role.initialized&&!role.member&&!role.coordinator&&!active_candidate&&role.phase!="retired"&&role.phase!="recovery_required";}
int runtime_open_enrollment(uint64_t generation){assert(generation==77);++enrollment_windows;events.push_back("enroll");return enrollment_result;}
int runtime_claim_original_controller(){++claims;events.push_back("claim");if(claim_member_wins){role.member=true;role.phase="member";return ESP_ERR_INVALID_STATE;}if(active_candidate)return ESP_ERR_INVALID_STATE;role.coordinator=true;role.phase="coordinator";return ESP_OK;}
void runtime_configuration_changed(uint64_t,const std::string&){events.push_back("publish-config");}
}
namespace amity::radio{
int reserve_consortium(bool inviter,uint64_t&g){assert(!inviter);++radio_reservations;events.push_back("reserve");g=reserve_result==ESP_OK?77:0;return reserve_result;}
void release(uint64_t generation){assert(generation==77);++radio_releases;events.push_back("release");}
}
namespace device_settings{const char*hostname(){return "amity-test";}const char*initial_admin_auth(){return "private-fixture";}}
struct Config{uint64_t config_revision=2;std::vector<int>activities,components;};
struct FakeHub {struct Id{std::string str()const{return std::string(32,'1');}};
std::shared_ptr<Config>config;struct Snapshot{std::vector<int>activities;};Id octopus_id(){return {};}
std::shared_ptr<Config>active_configuration(){return config;}Snapshot snapshot(){return {};}
int retire_membership(){assert(!original_interfaces_enabled);events.push_back("retire");return fence_result;}
int transition_coordinator_authority(bool on,unsigned epoch){assert(on&&epoch==1&&amity::consortium::role.coordinator);events.push_back("promote");return 0;}}hub;
namespace config_store{std::string executable_policy_hash(const Config&){return "exact-policy";}}
namespace web_admin{int load(FakeHub&,bool member){assert(!member);events.push_back("load");hub.config=std::make_shared<Config>();return 0;}
int finish_configuration_activation(){events.push_back("activate_configuration");return 0;}int start(FakeHub&,const char*,const char*){assert(original_interfaces_enabled);events.push_back("web");return 0;}
int stop(){assert(!original_interfaces_enabled);events.push_back("web-stop");if(block_mdns_add){std::lock_guard<std::mutex>guard(interleave_mutex);retire_at_web=true;interleave.notify_all();}return web_result;}}
namespace system_services{bool firmware_ready(){return firmware_healthy;}}
struct amity_matter_status_t{unsigned fabric_count=0;};
int matter_fabrics=0;
int amity_matter_get_status(amity_matter_status_t*s){s->fabric_count=matter_fabrics;return 0;}
int amity_matter_open_commissioning(unsigned seconds){assert(seconds==300);++setup_windows;return 0;}
bool amity_matter_controller_enrollment_complete(){++getter_calls;return enrolled;}
int amity_matter_set_controller_enabled(bool enabled){assert(!enabled&&!original_interfaces_enabled);events.push_back("matter-stop");++matter_stops;responder_live=false;return matter_result;}
int power(bool,void*){return 0;}int activity(uint8_t,void*){return 0;}int key(uint8_t,void*){return 0;}int mute(bool,void*){return 0;}int volume(int16_t,void*){return 0;}
struct amity_matter_callbacks_t{int(*power)(bool,void*);int(*activity)(uint8_t,void*);int(*key)(uint8_t,void*);int(*mute)(bool,void*);int(*volume)(int16_t,void*);void*context;};
struct IPv4{unsigned addr=0;};struct esp_ip6_addr_t{};
struct mdns_ip_addr_t{struct{int type=0;struct{IPv4 ip4;esp_ip6_addr_t ip6;}u_addr;}addr;mdns_ip_addr_t*next=nullptr;};
constexpr int CONFIG_LWIP_IPV6_NUM_ADDRESSES=2,ESP_IPADDR_TYPE_V4=1,ESP_IPADDR_TYPE_V6=2;
int mdns_init(){++mdns_initializations;responder_live=true;return 0;}int mdns_hostname_get(char*b){assert(responder_live);strcpy(b,"shared-host");return 0;}int mdns_hostname_set(const char*){assert(responder_live);return 0;}int mdns_delegate_hostname_add(const char*,void*){assert(responder_live);return 0;}
int mdns_service_add_for_host(const char*,const char*,const char*,const char*,int,void*,int){
 assert(responder_live);
 if(block_mdns_add){std::unique_lock<std::mutex>guard(interleave_mutex);mdns_add_entered=true;interleave.notify_all();interleave.wait(guard,[]{return release_mdns_add;});}
 if(mdns_add_result==ESP_OK)events.push_back("mdns");return mdns_add_result;
}
int mdns_service_remove_for_host(const char*,const char*,const char*,const char*){assert(responder_live);++mdns_removals;events.push_back("mdns-remove");return mdns_remove_result;}
int mdns_delegate_hostname_set_address(const char*,mdns_ip_addr_t*){assert(responder_live);++mdns_updates;return ESP_OK;}
struct ip_event_got_ip_t{struct{unsigned ip=0;}ip_info;};
template<class F>int esp_event_handler_register(int,int,F,void*){return 0;}
void input_task(void*){}void remote_siri(){}void remote_key(){}void remote_disconnect(){}
int xTaskCreate(void(*)(void*),const char*name,int,void*,int,void*){assert(original_interfaces_enabled);assert(std::string(name)=="remote_input");++remote_workers;events.push_back("remote-worker");return pdPASS;}
void ble_remote_set_siri_callback(void(*)(),void*){}int ble_remote_start(void(*)(),void*){assert(original_interfaces_enabled);events.push_back("remote-start");return 0;}void ble_remote_set_disconnect_callback(void(*)(),void*){}
void startup_memory(const char*){}
namespace amity_homekit{int start(FakeHub&,const amity_matter_callbacks_t&){assert(original_interfaces_enabled);events.push_back("homekit");return 0;}int stop(){assert(!original_interfaces_enabled);events.push_back("homekit-stop");return home_result;}}
struct esp_netif_ip_info_t{IPv4 ip;};
void*esp_netif_get_handle_from_ifkey(const char*){return reinterpret_cast<void*>(1);}bool esp_netif_is_netif_up(void*){return address_available;}int esp_netif_get_ip_info(void*,esp_netif_ip_info_t*p){p->ip.addr=address_available;return 0;}
int esp_netif_get_all_preferred_ip6(void*,esp_ip6_addr_t*){return 0;}
void vTaskDelay(unsigned){++delays;assert(delays<10);if(on_delay)on_delay();}
void vTaskDelete(void*){assert(amity::consortium::Role::instances==1);assert(!hub.config||hub.config.use_count()==1);++delete_calls;}
''' +functions+r'''
void retained_startup(amity::consortium::RuntimeStartup membership,int membership_read,int group_start=ESP_OK){
''' +retained+r'''
}
void reset(){using namespace amity::consortium;role.initialized=true;role.member=role.coordinator=role.authority_ready=false;role.phase="awaiting_setup";events.clear();hub.config.reset();original_interfaces_enabled=false;address_available=enrolled=firmware_healthy=claim_member_wins=active_candidate=false;claims=delays=setup_windows=enrollment_windows=remote_workers=delete_calls=getter_calls=matter_fabrics=radio_reservations=radio_releases=0;reserve_result=enrollment_result=ESP_OK;on_delay={};admin_mdns_registered=responder_live=false;mdns_initializations=mdns_removals=mdns_updates=matter_stops=0;fence_result=web_result=matter_result=home_result=mdns_remove_result=mdns_add_result=ESP_OK;block_mdns_add=mdns_add_entered=release_mdns_add=retire_at_web=false;}
void lifecycle_regressions(){
 const std::vector<std::string>headless={"retire","web-stop","matter-stop","homekit-stop"};
 const std::vector<std::string>published={"retire","web-stop","mdns-remove","matter-stop","homekit-stop"};
 reset();refresh_admin_mdns();start_admin_mdns();assert(retire_original_interfaces()==ESP_OK);
 assert(events==headless&&!mdns_initializations&&!mdns_removals&&!mdns_updates);
 refresh_admin_mdns();start_admin_mdns();assert(!mdns_initializations&&!mdns_removals&&!mdns_updates);
 std::cout<<"PASS actual headless retirement never enters uninitialized mDNS\n";
 reset();original_interfaces_enabled=true;start_admin_mdns();refresh_admin_mdns();
 assert(admin_mdns_registered&&mdns_initializations==1&&mdns_updates==1);events.clear();
 assert(retire_original_interfaces()==ESP_OK&&events==published&&!admin_mdns_registered&&!original_interfaces_enabled);
 refresh_admin_mdns();start_admin_mdns();assert(mdns_initializations==1&&mdns_removals==1&&mdns_updates==1);
 assert(retire_original_interfaces()==ESP_OK&&mdns_removals==1);
 std::cout<<"PASS actual registered service removed once before shared SDK shutdown\n";
 for(int*failure:{&fence_result,&web_result,&matter_result,&home_result}){
  reset();original_interfaces_enabled=true;start_admin_mdns();events.clear();*failure=ESP_FAIL;
  assert(retire_original_interfaces()==ESP_FAIL&&events==published&&mdns_removals==1&&!admin_mdns_registered);
 }
 reset();original_interfaces_enabled=true;start_admin_mdns();events.clear();mdns_remove_result=ESP_FAIL;
 assert(retire_original_interfaces()==ESP_OK&&events==published&&mdns_removals==1&&!admin_mdns_registered);
 assert(retire_original_interfaces()==ESP_OK&&mdns_removals==1);
 reset();original_interfaces_enabled=true;mdns_add_result=ESP_FAIL;start_admin_mdns();events.clear();
 assert(!admin_mdns_registered&&retire_original_interfaces()==ESP_OK&&events==headless&&!mdns_removals);
 std::cout<<"PASS actual cleanup errors preserve all retirement attempts; failed registration is never removed\n";
 reset();original_interfaces_enabled=true;block_mdns_add=true;
 std::thread publisher(start_admin_mdns);
 {std::unique_lock<std::mutex>guard(interleave_mutex);assert(interleave.wait_for(guard,std::chrono::seconds(2),[]{return mdns_add_entered;}));}
 int retired=ESP_FAIL;std::thread retiring([&]{retired=retire_original_interfaces();});
 {std::unique_lock<std::mutex>guard(interleave_mutex);assert(interleave.wait_for(guard,std::chrono::seconds(2),[]{return retire_at_web;}));}
 assert(!original_interfaces_enabled&&matter_stops==0);
 {std::lock_guard<std::mutex>guard(interleave_mutex);release_mdns_add=true;interleave.notify_all();}
 publisher.join();retiring.join();
 assert(retired==ESP_OK&&!admin_mdns_registered&&mdns_removals==1);
 assert((events==std::vector<std::string>{"retire","web-stop","mdns","mdns-remove","matter-stop","homekit-stop"}));
 std::cout<<"PASS actual in-flight mDNS publication drains before removal and SDK shutdown\n";
}
int main(){using namespace amity::consortium;
 lifecycle_regressions();
 reset();setup_button(false,nullptr);assert(setup_windows==0);firmware_healthy=true;setup_button(false,nullptr);assert(setup_windows==1);setup_button(true,nullptr);assert(enrollment_windows==1);
 active_candidate=true;setup_button(false,nullptr);assert(setup_windows==1);active_candidate=false;
 role.member=true;role.phase="member";setup_button(false,nullptr);assert(setup_windows==1);role.member=false;role.phase="awaiting_setup";matter_fabrics=1;setup_button(false,nullptr);assert(setup_windows==1);
 // Exercise the actual Add callback: acquire before opening, release only the
 // successfully acquired generation after an open failure, never open Matter.
 reset();setup_button(true,nullptr);assert(events.empty()&&!radio_reservations&&!enrollment_windows&&!setup_windows);
 firmware_healthy=true;role.initialized=false;setup_button(true,nullptr);assert(events.empty()&&!radio_reservations&&!enrollment_windows);
 reset();firmware_healthy=true;reserve_result=ESP_ERR_INVALID_STATE;setup_button(true,nullptr);assert(events==std::vector<std::string>{"reserve"}&&radio_reservations==1&&!enrollment_windows&&!radio_releases&&!setup_windows);
 reset();firmware_healthy=true;enrollment_result=ESP_ERR_INVALID_STATE;setup_button(true,nullptr);assert((events==std::vector<std::string>{"reserve","enroll","release"})&&radio_reservations==1&&enrollment_windows==1&&radio_releases==1&&!setup_windows);
 reset();firmware_healthy=true;setup_button(true,nullptr);assert((events==std::vector<std::string>{"reserve","enroll"})&&radio_reservations==1&&enrollment_windows==1&&!radio_releases&&!setup_windows);
 reset();firmware_healthy=true;enrolled=true;
 on_delay=[](){if(delays==1){assert(getter_calls==0&&!original_interfaces_enabled&&remote_workers==0);address_available=true;enrolled=false;}if(delays==2){assert(claims==0&&!original_interfaces_enabled&&remote_workers==0);enrolled=true;}};
 await_original_task(nullptr);assert(claims==1&&delays==2&&original_interfaces_enabled&&remote_workers==1&&delete_calls==1);
 const std::vector<std::string>expected={"claim","promote","load","activate_configuration","publish-config","mdns","web","remote-worker","remote-start","homekit"};assert(events==expected);
 reset();role.member=true;role.phase="member";address_available=enrolled=true;await_original_task(nullptr);assert(!claims&&events.empty()&&!remote_workers&&!original_interfaces_enabled&&delete_calls==1);
 reset();address_available=enrolled=true;claim_member_wins=true;await_original_task(nullptr);assert(claims==1&&role.member&&!original_interfaces_enabled&&remote_workers==0&&events==std::vector<std::string>{"claim"});
 reset();active_candidate=true;address_available=enrolled=true;role.phase="prepared";on_delay=[](){role.member=true;role.phase="member";};await_original_task(nullptr);assert(claims==1&&!original_interfaces_enabled&&remote_workers==0);
 reset();role.phase="retired";await_original_task(nullptr);assert(!claims&&!original_interfaces_enabled);
 reset();role.phase="recovery_required";await_original_task(nullptr);assert(!claims&&!original_interfaces_enabled);
 reset();role.coordinator=role.authority_ready=true;role.phase="coordinator";RuntimeStartup stored;stored.role=RuntimeStartup::coordinator;retained_startup(stored,ESP_OK);assert(matter_fabrics==0&&remote_workers==1&&!getter_calls);
 reset();role.coordinator=role.authority_ready=true;retained_startup(stored,ESP_OK,ESP_ERR_INVALID_STATE);assert(!original_interfaces_enabled&&events.empty());
 reset();role.coordinator=true;retained_startup(stored,ESP_OK);assert(!original_interfaces_enabled&&events.empty());
 reset();stored.role=RuntimeStartup::member;retained_startup(stored,ESP_OK);assert(events.empty()&&remote_workers==0);
 std::cout<<"PASS actual main role startup: checked same-boot enrollment, no-IP/pending/member/race gating, retained zero-fabric original, physical setup, scoped destruction\n";
}
'''
with tempfile.TemporaryDirectory() as directory:
    p=Path(directory);cpp=p/'main_role.cpp';binary=p/'test';cpp.write_text(fixture)
    subprocess.run([os.environ.get('CXX','c++'),'-std=c++17','-Wall','-Wextra','-Werror','-g','-pthread','-fsanitize=address,undefined',str(cpp),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
