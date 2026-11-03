#!/usr/bin/env python3
"""Execute the production integration lifecycle bodies against an instrumented SDK.

The source bodies are compiled, not reimplemented by the fixture. This isolates
network/service ownership from the large generated Matter/HAP cluster model.
Target compilation checks the real pinned SDK API; physical tests remain needed.
"""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[1]

def body(path, signature):
    source=(ROOT/path).read_text();start=source.index(signature);opening=source.index('{',start);depth=1;end=opening+1
    while depth:
        if source[end]=='{':depth+=1
        elif source[end]=='}':depth-=1
        end+=1
    return source[start:end]

matter='components/amity_matter/amity_matter.cpp'
functions='\n'.join(body(matter,s) for s in [
    'esp_err_t dispatch_key(', 'esp_err_t start_actuator_substrate(',
    'void finish_controller_enrollment(',
    'extern "C" bool amity_matter_controller_enrollment_complete(',
    'extern "C" esp_err_t amity_matter_set_controller_enabled(',
    'extern "C" void __wrap__ZN4chip3app11DnssdServer11StartServerEv(',
    'extern "C" void __wrap__ZN4chip3app11DnssdServer11StartServerENS_5Dnssd17CommissioningModeE('])
# The member branch must occur before credentials, node and endpoint creation.
s=(ROOT/matter).read_text();start=s[s.index('extern "C" esp_err_t amity_matter_start('):]
assert start.index('if (!controller_enabled.load()) return start_actuator_substrate();')<start.index('amity_matter_prepare_credentials()')<start.index('node::create(')
assert 'Server::GetInstance().Init' not in body(matter,'esp_err_t start_actuator_substrate(')
assert 'PlatformMgr().StopEventLoopTask' not in body(matter,'extern "C" esp_err_t amity_matter_set_controller_enabled(')

homekit='components/amity_homekit/homekit.cpp'
hap='third_party/esp-homekit-sdk/components/esp_hap_core/src/amity_hap.c'
home_functions='\n'.join(body(homekit,s) for s in ['esp_err_t start(', 'esp_err_t stop(', 'esp_err_t submit(', 'void refresh_remote_battery('])+'\n'+'\n'.join(body(hap,s) for s in ['void hap_amity_lock(', 'void hap_amity_unlock(', 'int hap_amity_stop('])
assert 'primary_remote_id' not in (ROOT/homekit).read_text()
assert 'amity::remote_status(' not in (ROOT/homekit).read_text()
# The same monotonic fence also suppresses queued HAP-loop republishing.
s=(ROOT/'third_party/esp-homekit-sdk/components/esp_hap_core/src/esp_hap_ip_services.c').read_text()
assert 'if (!hap_amity_enabled()) return HAP_SUCCESS;' in s[s.index('int hap_mdns_announce(bool first)'):][:130]
hostname_hook=body('components/amity_matter/consortium_network.cpp','hooks.set_hostname=[]')

source=r'''
#include <atomic>
#include <cassert>
#include <mutex>
#include <cstring>
#include <thread>
#include <condition_variable>
#include <iostream>
#include <algorithm>
#include <map>
#include <functional>
#include <vector>
using esp_err_t=int;
constexpr int ESP_OK=0,ESP_FAIL=-1,ESP_ERR_INVALID_STATE=1,ESP_ERR_NO_MEM=2,ESP_ERR_TIMEOUT=3;
constexpr int CHIP_NO_ERROR=0,HAP_SUCCESS=0,HAP_FAIL=-1;
constexpr unsigned portMAX_DELAY=~0u;
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define pdMS_TO_TICKS(v) (v)
constexpr int pdPASS=1,pdTRUE=1;
namespace M {
std::atomic<bool> started{false},stack_started{false},controller_enabled{true},controller_revoked{false};std::mutex lifecycle_mutex;
std::atomic<bool>controller_enrollment_complete{false},enrollment_check_pending{false};
int host_inits=0,wifi_inits=0,driver_inits=0,hooks=0,shutdowns=0,unregisters=0,dns=0,dispatches=0,memrefs=0;
int fabrics=0,commits=0,commit_error=0;bool failsafe=false,station_connected=false,station_provisioned=false;
void(*scheduled)(intptr_t)=nullptr;
bool udp=false,sessions=false,host=false,wifi=false;
std::mutex barrier;std::condition_variable cv;bool in_init=false,release_init=true;
const char*tag="test";int observed_onoff=0,observed_volume=0;
struct {int(*key)(unsigned char,void*)=nullptr;void*context=nullptr;} callbacks;
namespace chip {
namespace Dnssd {enum class CommissioningMode {kDisabled};}
namespace Platform {int MemoryInit(){++memrefs;return 0;}}
namespace DeviceLayer {
struct Platform {int InitChipStack(){++host_inits;host=true;std::unique_lock<std::mutex>l(barrier);in_init=true;cv.notify_all();cv.wait(l,[]{return release_init;});return 0;}
int AddEventHandler(void(*)(const void*,long),int){return 0;}int StartEventLoopTask(){return 0;}
int ScheduleWork(void(*work)(intptr_t),intptr_t){assert(!scheduled);scheduled=work;return 0;}};
Platform&PlatformMgr(){static Platform p;return p;}
struct Connectivity {int SetBLEAdvertisingEnabled(bool on){assert(!on);return 0;}bool IsWiFiStationConnected(){return station_connected;}bool IsWiFiStationProvisioned(){return station_provisioned;}};
Connectivity&ConnectivityMgr(){static Connectivity c;return c;}
namespace Internal {struct ESP32Utils{static int InitWiFiStack(){++wifi_inits;wifi=true;return 0;}};}
namespace NetworkCommissioning {struct ESPWiFiDriver{static ESPWiFiDriver&GetInstance(){static ESPWiFiDriver d;return d;}int Init(void*p){assert(!p);++driver_inits;return 0;}int CommitConfiguration(){++commits;return commit_error;}};}
}
namespace app {struct DnssdServer{static DnssdServer&Instance(){static DnssdServer d;return d;}void StopServer(){dns=0;}};
struct AttributeAccessInterfaceRegistry{static AttributeAccessInterfaceRegistry&Instance(){static AttributeAccessInterfaceRegistry r;return r;}void Unregister(int*){++unregisters;}};}
struct Server{struct FabricTable{int begin(){return 0;}int end(){return fabrics;}}table;
struct FailSafe{bool IsFailSafeFullyDisarmed(){return !failsafe;}}fail;
static Server&GetInstance(){static Server s;return s;}FabricTable&GetFabricTable(){return table;}FailSafe&GetFailSafeContext(){return fail;}
void Shutdown(){++shutdowns;udp=sessions=false;--memrefs;}};
}
namespace esp_matter::lock {struct ScopedChipStackLock{explicit ScopedChipStackLock(unsigned){} };}
namespace amity_matter {int stops=0;void stop_apple_fabric_cleanup(){assert(stops==shutdowns);++stops;}}
void event_callback(const void*,long){}int initialize_station_interface(){return ESP_OK;}int install_consortium_network_hooks(){++hooks;return 0;}
extern "C" void __real__ZN4chip3app11DnssdServer11StartServerEv(void*){++dns;}
extern "C" void __real__ZN4chip3app11DnssdServer11StartServerENS_5Dnssd17CommissioningModeE(void*,chip::Dnssd::CommissioningMode){++dns;}
''' + functions + r'''
void tests(){
 assert(!amity_matter_controller_enrollment_complete()&&!scheduled);
 // A pending/failed or network-less commissioning cannot choose the original.
 started=stack_started=true;fabrics=1;failsafe=true;station_connected=station_provisioned=true;
 auto drain=[](){assert(scheduled);auto work=scheduled;scheduled=nullptr;work(0);};
 assert(!amity_matter_controller_enrollment_complete()&&scheduled);
 assert(!amity_matter_controller_enrollment_complete());drain();assert(commits==0&&!controller_enrollment_complete);
 failsafe=false;station_connected=false;assert(!amity_matter_controller_enrollment_complete());drain();assert(commits==0);
 station_connected=true;fabrics=0;assert(!amity_matter_controller_enrollment_complete());drain();assert(commits==0);
 fabrics=1;assert(!amity_matter_controller_enrollment_complete());controller_enabled=false;drain();assert(commits==0&&!controller_enrollment_complete);controller_enabled=true;
 station_connected=true;commit_error=1;assert(!amity_matter_controller_enrollment_complete());drain();assert(commits==1&&!controller_enrollment_complete);
 commit_error=0;assert(!amity_matter_controller_enrollment_complete());drain();assert(commits==2&&amity_matter_controller_enrollment_complete());
 // Role evidence is not later derived from the current fabric count.
 fabrics=0;assert(amity_matter_controller_enrollment_complete());
 started=stack_started=false;
 assert(amity_matter_set_controller_enabled(false)==0);
 assert(!amity_matter_controller_enrollment_complete()&&!scheduled);
 assert(amity_matter_set_controller_enabled(true)==ESP_ERR_INVALID_STATE&&!controller_enabled);
 assert(start_actuator_substrate()==0&&host&&wifi&&stack_started&&!started&&hooks==1&&driver_inits==1&&shutdowns==0);
 __wrap__ZN4chip3app11DnssdServer11StartServerEv(nullptr);
 __wrap__ZN4chip3app11DnssdServer11StartServerENS_5Dnssd17CommissioningModeE(nullptr,chip::Dnssd::CommissioningMode::kDisabled);assert(dns==0);
 callbacks.key=[](unsigned char,void*){++dispatches;return 0;};assert(dispatch_key(1)==ESP_ERR_INVALID_STATE&&dispatches==0);
 assert(amity_matter_set_controller_enabled(true)==ESP_ERR_INVALID_STATE);
 // Independently reset the process fixture to model a previously commissioned controller.
 controller_revoked=false;stack_started=started=controller_enabled=true;udp=sessions=true;memrefs=2;dns=3;
 assert(amity_matter_set_controller_enabled(false)==0&&shutdowns==1&&unregisters==2);
 assert(host&&wifi&&!udp&&!sessions&&memrefs==1&&!started);
 assert(amity_matter_set_controller_enabled(false)==0&&shutdowns==1);
 __wrap__ZN4chip3app11DnssdServer11StartServerEv(nullptr);assert(dns==0);
 // Disabling during startup revokes callbacks before waiting for its lifecycle lock.
 controller_revoked=false;stack_started=started=false;controller_enabled=true;release_init=false;in_init=false;
 std::thread launch([]{std::lock_guard<std::mutex>l(lifecycle_mutex);assert(start_actuator_substrate()==0);started=true;memrefs=2;udp=sessions=true;});
 {std::unique_lock<std::mutex>l(barrier);cv.wait(l,[]{return in_init;});}
 std::atomic<bool>done{false};std::thread revoke([&]{assert(amity_matter_set_controller_enabled(false)==0);done=true;});
 while(controller_enabled.load())std::this_thread::yield();assert(!done);
 {std::lock_guard<std::mutex>l(barrier);release_init=true;}cv.notify_all();launch.join();revoke.join();
 assert(done&&!started&&!udp&&!sessions&&host&&wifi&&memrefs==1);
}
}
namespace H {
std::atomic<bool>disabled{false},service_enabled{true};std::mutex lifecycle_mutex;
void*status_lock=nullptr,*commands=nullptr,*startup_ready=nullptr;bool initialized=false,running=false;
int64_t deadline=100,published_deadline=100;int startup_result=0,depth=0,listener=0,paired=2,http_failure=0,server_stops=0,starts=0;
struct{void*server=nullptr;}hap_priv;void*lock=reinterpret_cast<void*>(1);
struct Status {bool started=true,available=true,pairing=true,busy=true,paired=true;int error=0;char manual_code[11]{},qr_payload[32]{};}current_status;
struct ble_remote_status_t{bool bonded=false,connected=false;int battery_percent=-1,charging=-1;}local_remote;
void ble_remote_get_status(ble_remote_status_t*s){*s=local_remote;}
bool battery_known=false,charging_known=false;int battery_level=1,battery_low=2,battery_charging=3;
std::map<int,int>characteristics;void update(int characteristic,int value){characteristics[characteristic]=value;}
struct StatusLock{StatusLock(){}~StatusLock(){}};void hap_amity_lock();void hap_amity_unlock();
int xSemaphoreTakeRecursive(void*p,unsigned){assert(p);++depth;return 1;}int xSemaphoreGiveRecursive(void*p){assert(p);--depth;return 1;}
struct HapLock{HapLock(){hap_amity_lock();}~HapLock(){hap_amity_unlock();}};
void hap_amity_set_pairing_deadline(long v){deadline=v;}void hap_close_all_sessions(){}int hap_mdns_deannounce(){return 0;}int hap_ip_services_stop(){return 0;}
int hap_platform_httpd_stop(void**){assert(depth==0);++server_stops;if(http_failure)return -1;listener=0;return 0;}
int hap_amity_stop();
namespace amity {struct SystemControl{};}
struct amity_matter_callbacks_t{};amity::SystemControl*hub=nullptr;amity_matter_callbacks_t callbacks;
namespace device_settings {const char*hostname(){return "amity";}}
void* xSemaphoreCreateMutex(){return reinterpret_cast<void*>(1);}void*xQueueCreate(int,int){return reinterpret_cast<void*>(2);}void*xSemaphoreCreateBinary(){return reinterpret_cast<void*>(3);}
int hap_amity_init(const char*){return service_enabled?0:-1;}
void worker(void*){}int xTaskCreate(void(*)(void*),const char*,int,void*,int,void*){++starts;initialized=running=true;listener=1;hap_priv.server=reinterpret_cast<void*>(9);return 1;}
int xSemaphoreTake(void*,unsigned){return 1;}
enum class Command{Open,Close,Remove};int xQueueSend(void*,const void*,int){return 1;}
''' + home_functions + r'''
void tests(){
 refresh_remote_battery();assert(battery_known&&charging_known&&characteristics[battery_level]==100&&characteristics[battery_charging]==0);
 local_remote={true,false,75,1};refresh_remote_battery();assert(!battery_known&&!charging_known);
 local_remote.connected=true;refresh_remote_battery();assert(battery_known&&charging_known&&characteristics[battery_level]==75&&characteristics[battery_charging]==1);
 local_remote.battery_percent=10;local_remote.charging=-1;refresh_remote_battery();assert(battery_known&&!charging_known&&characteristics[battery_low]==1);
 local_remote.battery_percent=-1;local_remote.charging=0;refresh_remote_battery();assert(!battery_known&&charging_known&&characteristics[battery_charging]==0);
 assert(stop()==0);amity::SystemControl h;assert(start(h,{})==ESP_ERR_INVALID_STATE&&starts==0);assert(submit(Command::Open)==ESP_ERR_INVALID_STATE);
 // An allocation failure can leave the outer status mutex without the HAP mutex.
 status_lock=reinterpret_cast<void*>(1);lock=nullptr;assert(stop()==0&&depth==0);
 status_lock=nullptr;lock=reinterpret_cast<void*>(1);
 // New-process controller fixture, with two preexisting paired controllers.
 disabled=service_enabled=false;service_enabled=true;
 assert(start(h,{})==0&&listener==1&&starts==1);
 http_failure=1;assert(stop()==ESP_FAIL&&disabled&&!service_enabled&&listener==1&&paired==2);
 assert(submit(Command::Remove)==ESP_ERR_INVALID_STATE&&start(h,{})==ESP_ERR_INVALID_STATE);
 http_failure=0;assert(stop()==0&&listener==0&&paired==2&&!running&&!current_status.started);
 assert(depth==0&&hap_priv.server==nullptr);const auto stops=server_stops;assert(stop()==0&&server_stops==stops);
}
}
namespace N {
struct Netif{}station;
int service_result=ESP_OK,service_calls=0,netif_calls=0,lookups=0,hostname_reads=0;
Netif*esp_netif_get_handle_from_ifkey(const char*){++lookups;return &station;}
namespace device_settings {
int apply_enrollment_hostname(const std::string& name){
 assert(name=="Amity-Den");++service_calls;return service_result;
}
const char*hostname(){++hostname_reads;return "unused";}
}
int esp_netif_set_hostname(Netif*,const char*){++netif_calls;return ESP_FAIL;}
struct Hooks{std::function<int(const std::string&)>set_hostname;};
std::function<int(const std::string&)>actual_hook(){Hooks hooks;
''' + hostname_hook + r''';return hooks.set_hostname;}
void tests(){
 const auto hook=actual_hook();
 // Enrollment delegates to the shared live settings service exactly once.
 // It must not duplicate network changes after that service commits.
 for(const auto outcome:{ESP_OK,ESP_FAIL,ESP_ERR_INVALID_STATE,ESP_ERR_TIMEOUT}){
  service_result=outcome;service_calls=0;
  assert(hook("Amity-Den")==outcome&&service_calls==1);
  assert(!lookups&&!netif_calls&&!hostname_reads);
 }
}
}
int main(){M::tests();H::tests();N::tests();std::cout<<"PASS actual Matter/HAP lifecycle and enrollment hostname hook: single shared live-service delegation, no duplicate hostname/network access and exact error propagation\n";}
'''
with tempfile.TemporaryDirectory() as directory:
    path=Path(directory);file=path/'controller_integration.cpp';binary=path/'test';file.write_text(source)
    subprocess.run([os.environ.get('CXX','c++'),'-std=c++17','-Wall','-Wextra','-Werror','-g','-fsanitize=address,undefined','-pthread',str(file),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
