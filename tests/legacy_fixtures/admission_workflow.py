"""Discovery and admission fixture.

PREFIX fakes the runtime state, the task, radio and GATT seams and the candidate's side of
the exchange. RESET restores them before each scenario.
"""
PREFIX = r'''
#include "amity/consortium/runtime.hpp"
#include "amity/consortium/bootstrap.hpp"
#include "amity/consortium/credential_types.hpp"
#include "amity/consortium/enrollment.hpp"
#include "amity/consortium/settings.hpp"
#include "hostname_registry.hpp"
#include "runtime_persistence.hpp"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
namespace amity::consortium {
using namespace amity::consortium::platform;
using namespace std::chrono_literals;
constexpr int pdPASS=1,MALLOC_CAP_SPIRAM=1,MALLOC_CAP_8BIT=2;
#define pdMS_TO_TICKS(x) (x)
std::mutex lock,persistence_lock;
Group group;Identity identity;RuntimeStatus status;RuntimeRadioHooks radio;RuntimeNetworkHooks network;HostnameRegistry hostnames;
struct InviteRequest {model::MessageId scan_id;};
struct Admission {RuntimeAdmissionStatus view;uint64_t generation=0,deadline=0;std::string selected_token,hostname;bool submitted=false;} admission;
std::atomic_bool invitation_running{false},invitation_cancelled{false};
struct Active {model::OctopusId peer;bool cancelled=false,ready=false;std::mutex mutex;struct{bool ready=false;std::string hostname;}status;};
std::vector<std::shared_ptr<Active>> links;
struct SettingsLink {std::shared_ptr<Active> active;RuntimePeerStatus status;};
SettingsLink settings_link(model::OctopusId owner){for(const auto& active:links)if(active->peer==owner&&!active->cancelled&&active->ready){RuntimePeerStatus status;status.hostname=active->status.hostname;return {active,status};}return {};}
struct SettingsOperation {model::OctopusId owner;std::string core,stage="admitted";uint64_t expires=0;std::optional<std::string> hostname{};};
std::vector<SettingsOperation> settings_operations;
void settings_expire(uint64_t now){for(auto& operation:settings_operations)if(operation.stage=="admitted"&&now>=operation.expires)operation.stage="unknown";}

unsigned connections=0,handshakes=0;
std::function<void()> during_connect;
uint64_t clock_ms=100;
uint64_t now_ms(){return clock_ms;}
std::function<void()> during_wait;
void vTaskDelay(unsigned ms){clock_ms+=ms;if(during_wait)during_wait();}
bool spawn_ok=true,discover_ok=true,exchange_ok=true,networked=false,bad_identity=false,bad_certificate=false;
std::string fail_phase;
unsigned discoveries=0,reservations=0,releases=0,disconnects=0,wifi_exports=0,encrypted_exchanges=0,saves=0,certificates=0;
unsigned candidate_snapshots=0;bool early_candidate_published=false;
void* task_argument=nullptr;
void invite_worker(void*){}
int xTaskCreateWithCaps(void(*)(void*),const char*,unsigned,void* arg,unsigned,void*,unsigned){if(!spawn_ok)return 0;task_argument=arg;return pdPASS;}
void gatt_client_disconnect(){++disconnects;}
void defer_radio_release(uint64_t generation){assert(generation==77);++releases;}
int gatt_client_discover(uint64_t generation,std::vector<RuntimeAdmissionCandidate>& out){assert(generation==77);++discoveries;if(discover_ok&&discoveries>1){out={{"one","Amity-123456"}};clock_ms+=100;}else clock_ms+=20000;return ESP_OK;}
int gatt_client_candidates(uint64_t generation,std::vector<RuntimeAdmissionCandidate>& out){
 assert(generation==77);++candidate_snapshots;
 if(candidate_snapshots==1)early_candidate_published=admission.view.phase=="found"&&admission.view.candidates.size()==1&&admission.view.candidates[0].token=="one";
 out={{"one","Amity-123456"},{"two","Amity-654321"}};return ESP_OK;
}
int gatt_client_connect_candidate(const std::string& token,uint64_t generation){assert(token=="two"&&generation==77);++connections;if(during_connect)during_connect();return ESP_OK;}
std::string hex(std::string_view value){std::string out;for(uint8_t c:value){out+="0123456789abcdef"[c>>4];out+="0123456789abcdef"[c&15];}return out;}
bool unhex(std::string_view text,std::string& value,size_t){if(text==hex("candidate-leaf")){value="candidate-leaf";return true;}return false;}
std::string sha256(std::string_view){return std::string(64,'d');}
bool certificate(std::string_view leaf,model::OctopusId,std::string& pin){++certificates;pin=std::string(64,leaf=="controller-leaf"?'a':'c');return leaf=="controller-leaf"||(leaf=="candidate-leaf"&&!bad_certificate);}
template<class T>T ident(unsigned n){char text[33];snprintf(text,sizeof(text),"%032x",n);return *T::parse(text);}
template<class T>T random_id(){return ident<T>(90);}
std::string host_ip(){return "192.0.2.1";}
std::function<void()> during_save;
int save_group(const Group&){if(during_save)during_save();++saves;return ESP_OK;}
Error remember_hostname(model::OctopusId owner,const std::string& name,std::unique_lock<std::mutex>&){return hostnames.remember(owner,name,[](std::string_view){return true;});}
void mbedtls_platform_zeroize(void* data,size_t count){std::fill_n(static_cast<char*>(data),count,0);}
struct AdmissionClient {
    AdmissionClient(){}
    bool handshake(const std::function<bool(std::string_view,std::string&)>& exchange){std::string response;return exchange("public-handshake",response);}
    std::string peer_id()const{return ident<model::OctopusId>(3).str();}
    bool encrypt(std::string_view value,std::string& out){out="encrypted:"+std::string(value);return true;}
    bool decrypt(std::string_view value,std::string& out){if(value.substr(0,10)!="encrypted:")return false;out=value.substr(10);return true;}
};
bool gatt_client_exchange(bool encrypted,std::string_view input,std::string& out){
    if(!exchange_ok)return false;
    if(!encrypted){assert(input=="public-handshake");++handshakes;out="public-response";return true;}
    ++encrypted_exchanges;assert(input.substr(0,10)=="encrypted:");Json request;JsonError detail;assert(parse_json(input.substr(10),request,detail));
    const auto phase=request.get("phase")->string;
    if(phase==fail_phase)return false;
    if(phase=="identify")out="{\"phase\":\"identity\",\"octopus_id\":\""+ident<model::OctopusId>(bad_identity?4:3).str()+"\",\"leaf\":\""+hex("candidate-leaf")+"\",\"networked\":"+(networked?"true":"false")+"}";
    else if(phase=="prepare"){
        JoinProposal proposal;assert(decode_join_proposal(request.get("proposal")->string,proposal));
        assert(proposal.coordinator_id==identity.id&&proposal.coordinator_leaf==identity.der&&proposal.coordinator_pin==identity.spki);
        assert(proposal.candidate_id==ident<model::OctopusId>(3)&&proposal.candidate_leaf=="candidate-leaf"&&proposal.candidate_pin==std::string(64,'c'));
        out="{\"phase\":\"prepared\",\"join_id\":\""+group.join_id.str()+"\",\"digest\":\""+group.join_digest+"\",\"hostname\":\""+admission.hostname+"\"}";
    }
    else {assert(phase=="wifi");assert(request.get("ssid")->string==hex("private-network"));assert(request.get("password")->string==hex("private-password"));out="{\"phase\":\"networked\",\"ready\":true}";}
    out="encrypted:"+out;return true;
}
'''
RESET = r'''void reset(){
    if(task_argument){delete static_cast<InviteRequest*>(task_argument);task_argument=nullptr;}
    group={};group.role=Group::coordinator;group.consortium=ident<model::ConsortiumId>(1);group.authority=ident<model::OctopusId>(2);group.revision=1;
    identity={};identity.id=group.authority;identity.der="controller-leaf";identity.spki=std::string(64,'a');status={};status.initialized=status.credential_ready=status.authority_ready=true;
    admission={};invitation_running=invitation_cancelled=false;hostnames={};links.clear();settings_operations.clear();
    fail_phase.clear();clock_ms=100;during_wait={};spawn_ok=discover_ok=exchange_ok=true;networked=bad_identity=bad_certificate=false;
    discoveries=reservations=releases=disconnects=wifi_exports=encrypted_exchanges=saves=certificates=0;candidate_snapshots=0;early_candidate_published=false;
    connections=handshakes=0;during_connect={};during_save={};
    platform::recorded_setup_timings.clear();
    radio.reserve=[](bool inviter,uint64_t& generation){assert(inviter);++reservations;generation=77;return ESP_OK;};
    network.get_hostname=[]{return "main-amity";};
    network.export_credentials=[](std::string& ssid,std::string& password){++wifi_exports;ssid="private-network";password="private-password";return true;};
}
'''
