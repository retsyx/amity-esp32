#!/usr/bin/env python3
"""Exercise the production metadata-only rename and HomeKit discovery rollback."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]


def function(source, signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


main = (root / 'main/main.cpp').read_text()
hap = (root / 'third_party/esp-homekit-sdk/components/esp_hap_core/src/amity_hap.c').read_text()
ip = (root / 'third_party/esp-homekit-sdk/components/esp_hap_core/src/esp_hap_ip_services.c').read_text()
production = '\n'.join([
    function(ip, 'int hap_mdns_deannounce_for_rename(void)'),
    function(hap, 'int hap_amity_rename_hostname(const char *name)'),
    'namespace amity_homekit {esp_err_t rename_hostname(const char* name){return hap_amity_rename_hostname(name)==HAP_SUCCESS?ESP_OK:ESP_FAIL;}}',
    function(main, 'esp_err_t rename_hostname(const std::string& previous,const std::string& hostname)'),
])
fixture = r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstring>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>
using esp_err_t=int;
constexpr int ESP_OK=0,ESP_FAIL=-1,ESP_ERR_INVALID_STATE=1,ESP_ERR_INVALID_ARG=2,ESP_ERR_NO_MEM=3,HAP_SUCCESS=0,HAP_FAIL=-1;
#define ESP_LOGE(...) ((void)0)
#define pdMS_TO_TICKS(value) (value)
void vTaskDelay(unsigned){}
std::atomic_bool original_interfaces_enabled{true};
std::mutex admin_mdns_mutex;
bool admin_mdns_registered=true;
std::string admin_mdns_hostname="old",network_name="old";
std::set<std::string> aliases{"old"},https{"old"};
std::vector<std::string> events;
char hostname[33]="old";
bool first_announce_done=true,advertise=true;
struct {int hap_mdns_handle;}hap_priv{};
std::string fail_add,fail_https,fail_remove,fail_home;
bool fail_net=false;
int locks=0,network;
void hap_amity_lock(){++locks;}
void hap_amity_unlock(){assert(locks==1);--locks;}
int hap_mdns_serv_stop(int*){events.push_back(std::string("home_remove:")+hostname);return HAP_SUCCESS;}
int hap_mdns_announce(bool){
    if(!advertise)return HAP_SUCCESS;
    events.push_back(std::string("home_add:")+hostname);
    if(fail_home==hostname)return HAP_FAIL;
    assert(aliases.count(hostname));first_announce_done=true;return HAP_SUCCESS;
}
int* esp_netif_get_handle_from_ifkey(const char*){return &network;}
esp_err_t esp_netif_set_hostname(int*,const char* name){events.push_back(std::string("network:")+name);if(fail_net)return ESP_FAIL;network_name=name;return ESP_OK;}
esp_err_t mdns_delegate_hostname_add(const char* name,void*){events.push_back(std::string("alias_add:")+name);if(fail_add==name)return ESP_ERR_NO_MEM;aliases.insert(name);return ESP_OK;}
esp_err_t refresh_admin_mdns_for_host(const char* name){events.push_back(std::string("address:")+name);assert(aliases.count(name));return ESP_OK;}
esp_err_t mdns_service_add_for_host(const char*,const char*,const char*,const char* name,int,void*,int){
    events.push_back(std::string("https_add:")+name);if(fail_https==name)return ESP_ERR_NO_MEM;
    assert(aliases.count(name));https.insert(name);return ESP_OK;
}
esp_err_t mdns_delegate_hostname_remove(const char* name){events.push_back(std::string("alias_remove:")+name);if(fail_remove==name)return ESP_ERR_NO_MEM;aliases.erase(name);https.erase(name);return ESP_OK;}
'''
cases = r'''
void reset(){
    original_interfaces_enabled=true;admin_mdns_registered=true;admin_mdns_hostname="old";network_name="old";
    aliases={"old"};https={"old"};events.clear();strcpy(hostname,"old");first_announce_done=true;advertise=true;
    fail_add.clear();fail_https.clear();fail_remove.clear();fail_home.clear();fail_net=false;assert(!locks);
}
void old_state(){assert(network_name=="old"&&std::string(hostname)=="old"&&admin_mdns_hostname=="old"&&aliases==std::set<std::string>{"old"}&&https==aliases&&!locks);}
int main(){
    reset();assert(rename_hostname("old","new")==ESP_OK);
    assert(network_name=="new"&&std::string(hostname)=="new"&&admin_mdns_hostname=="new"&&aliases==std::set<std::string>{"new"}&&https==aliases);
    assert((events==std::vector<std::string>{"network:new","alias_add:new","address:new","https_add:new","home_remove:old","home_add:new","alias_remove:old"}));
    // Failed persistence invokes this reverse operation. It restores all three
    // metadata consumers without changing an HTTP listener or any HAP session.
    assert(rename_hostname("new","old")==ESP_OK);old_state();
    reset();fail_add="new";assert(rename_hostname("old","new")==ESP_ERR_NO_MEM);old_state();
    reset();fail_https="new";assert(rename_hostname("old","new")==ESP_ERR_NO_MEM);old_state();
    reset();fail_home="new";assert(rename_hostname("old","new")==ESP_FAIL);old_state();assert(first_announce_done);
    reset();fail_remove="old";assert(rename_hostname("old","new")==ESP_ERR_NO_MEM);old_state();
    reset();fail_net=true;assert(rename_hostname("old","new")==ESP_FAIL);old_state();assert(events==std::vector<std::string>{"network:new"});
    reset();original_interfaces_enabled=false;assert(rename_hostname("old","new")==ESP_OK);
    assert(network_name=="new"&&std::string(hostname)=="old"&&events==std::vector<std::string>{"network:new"});
    // An unpaired, closed HomeKit service stays unannounced after renaming.
    reset();first_announce_done=false;advertise=false;assert(rename_hostname("old","new")==ESP_OK);
    assert(std::string(hostname)=="new"&&!first_announce_done);
    assert(hap_amity_rename_hostname("")==HAP_FAIL&&hap_amity_rename_hostname(std::string(33,'x').c_str())==HAP_FAIL);
    std::cout<<"PASS production live hostname services: network metadata, staged delegated HTTPS/HomeKit discovery, unchanged listeners/sessions, rollback at every service failure, reverse rename, headless member path and closed pairing window\n";
}
'''
with tempfile.TemporaryDirectory(prefix='amity-hostname-services-') as temporary:
    source = Path(temporary) / 'test.cpp'
    binary = Path(temporary) / 'test'
    source.write_text(fixture + production + cases)
    subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
