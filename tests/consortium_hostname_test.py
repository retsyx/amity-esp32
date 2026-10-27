#!/usr/bin/env python3
"""Exercise hostname induction/storage and actual authenticated metadata admission."""
from pathlib import Path
import os,shlex,subprocess,tempfile
from consortium_gatt_callback_test import function
ROOT=Path(__file__).resolve().parents[1]
PREFIX=r'''
#include "hostname_registry.hpp"
#include "amity/consortium/enrollment.hpp"
#include "amity/consortium/credential_types.hpp"
#include "amity/consortium/session.hpp"
#include <cassert>
#include <cstdio>
#include <iostream>
#include <mutex>
using namespace amity::consortium;using namespace amity::consortium::platform;
using esp_err_t=int;constexpr int ESP_OK=0,ESP_ERR_INVALID_ARG=1,ESP_ERR_INVALID_STATE=2,ESP_ERR_NOT_SUPPORTED=3,ESP_ERR_TIMEOUT=4,ESP_FAIL=5;
std::mutex lock;Group group,durable;Identity identity;HostnameRegistry hostnames;
uint64_t candidate_deadline=1000;bool candidate_passive=false;unsigned saves=0;bool save_failure=false,registry_failure=false;std::string current_name="factory-name",proposal_bytes;std::vector<std::string> effects;
uint64_t now_ms(){return 100;}
struct Network {std::function<esp_err_t(const std::string&)> set_hostname;std::function<std::string()> get_hostname;std::function<esp_err_t(model::JoinId,const std::string&,const std::string&,const std::string&)> stage_transaction;std::function<esp_err_t(model::JoinId,const std::string&)> commit_transaction,finish_transaction;std::function<void()> rollback;}network;
using RuntimeNetworkHooks=Network;
struct {std::function<esp_err_t()> retire;}lifecycle;
struct {bool member=false,authority_ready=false;std::string phase;}status;
#define pdMS_TO_TICKS(x) (x)
void vTaskDelay(unsigned){}
int ble_remote_set_controller_enabled(bool enabled){assert(!enabled);effects.push_back("remote_off");return ESP_OK;}
int save_group(const Group& value){++saves;effects.push_back("membership");if(save_failure)return ESP_FAIL;durable=value;return ESP_OK;}
std::string host_ip(){return {};}
std::string sha256(std::string_view value){return std::string(64,value==proposal_bytes?'a':'b');} // Inject only this primitive boundary; actual codecs/dispatcher are below.
std::string hex(std::string_view value){std::string out;for(unsigned char c:value){out+="0123456789abcdef"[c>>4];out+="0123456789abcdef"[c&15];}return out;}
bool unhex(std::string_view,std::string&,size_t){return false;}
bool certificate(std::string_view leaf,model::OctopusId,std::string& pin){if(leaf!="authority")return false;pin=std::string(64,'c');return true;}
template<class T>T id(unsigned n){char value[33];snprintf(value,sizeof(value),"%032x",n);return *T::parse(value);}
std::string escaped(std::string_view value){std::string out;for(char c:value){if(c=='"'||c=='\\')out+='\\';out+=c;}return out;}
std::string prepare(){return "{\"phase\":\"prepare\",\"proposal\":\""+escaped(proposal_bytes)+"\",\"digest\":\""+std::string(64,'a')+"\"}";}
JoinProposal proposal(){JoinProposal p;p.join_id=id<model::JoinId>(1);p.consortium_id=id<model::ConsortiumId>(2);p.coordinator_id=id<model::OctopusId>(3);p.candidate_id=id<model::OctopusId>(4);p.epoch=1;p.revision=2;p.coordinator_leaf="authority";p.candidate_leaf="candidate";p.coordinator_pin=std::string(64,'c');p.candidate_pin=std::string(64,'d');p.host="192.0.2.1";p.candidate_hostname="living-room";return p;}
void reset(){group={};durable={};identity={id<model::OctopusId>(4),"candidate","",std::string(64,'d')};current_name="factory-name";status={};candidate_passive=false;saves=0;save_failure=registry_failure=false;effects.clear();hostnames={};network={};network.set_hostname=[](const std::string& name){effects.push_back("persist_hostname");current_name=name;return ESP_OK;};network.get_hostname=[]{return current_name;};lifecycle.retire=[]{assert(group.role==Group::prepared);effects.push_back("retire");return ESP_OK;};proposal_bytes=encode_join_proposal(proposal());assert(!proposal_bytes.empty());}
struct FakeTls{std::string incoming;bool receive(std::string& out){out=incoming;return !out.empty();}};
struct Active {Session session;FakeTls storage;FakeTls* tls=&storage;model::OctopusId peer;std::mutex mutex;struct{std::string hostname;}status;};
unsigned sent=0;bool send(Active&,Kind kind,std::string_view payload,uint32_t){assert(kind==Kind::device_status&&payload.find(current_name)!=std::string_view::npos);++sent;return true;}
bool current_peer_credential(const Active& active,const Group&){return active.peer==id<model::OctopusId>(3);}
Error remember_hostname(model::OctopusId owner,const std::string& name){return hostnames.remember(owner,name,[](std::string_view){return !registry_failure;});}
LinkScope scope(){return {id<model::LinkId>(5),id<model::ConsortiumId>(2),id<model::OctopusId>(4),id<model::OctopusId>(3),id<model::OctopusId>(4),id<model::BootId>(6),id<model::BootId>(7),1,1};}
void start(Active& active,bool capable=true){auto sc=scope();active.peer=sc.peer_id;assert(active.session.authenticate(sc,sc.peer_id,PeerRole::actuator,true)==Error::none);HelloFrame h;h.consortium_id=sc.consortium_id;h.octopus_id=sc.peer_id;h.boot_id=sc.peer_boot_id;h.coordinator_epoch=1;h.config_revision=1;h.required_capabilities=known_capabilities&~(capable?0u:hostname_metadata);assert(active.session.hello(h,known_capabilities)==Error::none);}
std::string packet(std::string name="remote-name",unsigned destination=4){auto sc=scope();return "{\"protocol_version\":1,\"consortium_id\":\""+sc.consortium_id.str()+"\",\"message_id\":\""+id<model::MessageId>(8).str()+"\",\"origin_octopus_id\":\""+sc.peer_id.str()+"\",\"origin_boot_id\":\""+sc.peer_boot_id.str()+"\",\"destination_octopus_id\":\""+id<model::OctopusId>(destination).str()+"\",\"destination_boot_id\":\""+sc.local_boot_id.str()+"\",\"coordinator_epoch\":\"1\",\"config_revision\":\"1\",\"kind\":\"device_status\",\"ttl_ms\":1000,\"payload\":{\"hostname\":\""+name+"\"}}";}
'''
SUITE=r'''
int main(){
 for(auto good:{"a","living-room","123","a-b","abcdefghijklmnopqrstuvwxyz123456"})assert(valid_hostname(good));
 for(auto bad:{"","Living-room","amity.local","-room","room-","room name","a_b","abcdefghijklmnopqrstuvwxyz1234567"})assert(!valid_hostname(bad));
 reset();std::string response;assert(bootstrap_request("{\"phase\":\"identify\"}",response)==ESP_OK&&response.find("\"bootstrap_profile\":2")!=std::string::npos);
 assert(bootstrap_request(prepare(),response)==ESP_OK&&current_name=="living-room"&&group.role==Group::prepared&&durable.join_hostname=="living-room"&&candidate_passive);
 assert(effects==std::vector<std::string>({"persist_hostname","membership","remote_off","retire"}));
 Json prepared;JsonError detail;assert(parse_json(response,prepared,detail)&&prepared.get("hostname")->string=="living-room");
 assert(bootstrap_request(prepare(),response)==ESP_OK&&group.join_hostname=="living-room");
 reset();network.set_hostname={};assert(bootstrap_request(prepare(),response)==ESP_ERR_NOT_SUPPORTED&&!saves&&group.role==Group::unclaimed);
 reset();network.set_hostname=[](const std::string&){return ESP_FAIL;};assert(bootstrap_request(prepare(),response)==ESP_FAIL&&!saves&&group.role==Group::unclaimed);
 reset();network.set_hostname=[](const std::string&){return ESP_OK;};assert(bootstrap_request(prepare(),response)==ESP_ERR_INVALID_STATE&&!saves);
 reset();save_failure=true;assert(bootstrap_request(prepare(),response)==ESP_FAIL&&group.role==Group::unclaimed&&current_name=="living-room"&&!candidate_passive); // A cut after name storage never invents membership.
 reset();auto old=proposal_bytes;old.replace(old.find("\"bootstrap_profile\":2"),21,"\"bootstrap_profile\":1");JoinProposal out;assert(!decode_join_proposal(old,out));
 auto invalid=proposal();invalid.candidate_hostname="Wrong Name";assert(encode_join_proposal(invalid).empty());
 reset();group.role=Group::coordinator;assert(bootstrap_request(prepare(),response)==ESP_ERR_INVALID_STATE&&effects.empty());
 std::cout<<"PASS actual induction: profile2 immutable DNS name, persist/apply before membership and network, exact ACK, unsupported/storage/apply errors and role rejection\n";
 HostnameRegistry names;std::string stored;auto save=[&](std::string_view bytes){stored=bytes;return true;};
 for(unsigned n=1;n<=64;++n)assert(names.remember(id<model::OctopusId>(n),"device-"+std::to_string(n),save)==Error::none);
 assert(names.remember(id<model::OctopusId>(65),"full",save)==Error::queue_full);HostnameRegistry reboot;assert(reboot.restore(stored)&&reboot.lookup(id<model::OctopusId>(4))=="device-4");
 const auto original=stored;assert(names.remember(id<model::OctopusId>(4),"renamed",[](std::string_view){return false;})==Error::storage_failure&&names.lookup(id<model::OctopusId>(4))=="device-4");
 assert(!reboot.restore("{\"version\":1,\"devices\":[{\"octopus_id\":\""+id<model::OctopusId>(4).str()+"\",\"hostname\":\"bad.local\"}]}"));assert(reboot.lookup(id<model::OctopusId>(4))=="device-4");
 std::cout<<"PASS names: bounded64 identities, full-cap rejection, exact reboot retention independent of roster/removal, no mutation after failed storage or malformed record\n";
 reset();sent=0;{Active active;start(active);active.storage.incoming=packet();assert(exchange_hostname(active,{})&&active.status.hostname=="remote-name"&&hostnames.lookup(active.peer)=="remote-name"&&sent==1&&active.session.state()==ConnectionState::synchronizing);}
 reset();sent=0;{Active active;start(active,false);assert(exchange_hostname(active,{})&&sent==0&&hostnames.entries().empty());assert(!exchange_hostname(active,"required-name")&&sent==0);}
 for(unsigned failure=0;failure<4;++failure){reset();Active active;start(active);active.storage.incoming=packet(failure==0?"invalid.name":"remote-name",failure==1?99:4);if(failure==2)registry_failure=true;assert(!exchange_hostname(active,failure==3?"other-name":"")&&hostnames.entries().empty()&&active.status.hostname.empty());}
 std::cout<<"PASS actual metadata exchange: negotiated-only legacy compatibility, exact authenticated destination/expected name, invalid-name/storage failure rejects without readiness or authority\n";
}
'''
def main():
 source=(ROOT/'components/consortium/runtime.cpp').read_text()
 actual=function(source,'esp_err_t bootstrap_request(')+function(source,'bool exchange_hostname(')
 with tempfile.TemporaryDirectory(prefix='amity-hostname-') as directory:
  cpp=Path(directory)/'test.cpp';binary=Path(directory)/'test';cpp.write_text(PREFIX+actual+SUITE)
  sources=[ROOT/'components/consortium'/name for name in ['json.cpp','wire.cpp','session.cpp','owned_intent.cpp','settings.cpp','update.cpp','relay.cpp','transfer.cpp','enrollment.cpp']]+[ROOT/'components/amity_system/model.cpp']
  subprocess.run(shlex.split(os.environ.get('CXX','c++'))+['-std=c++17','-Wall','-Wextra','-Werror','-g','-fsanitize=address,undefined']+['-I'+str(ROOT/path) for path in ['components/consortium','components/consortium/include','components/amity_system/include']]+[str(cpp)]+list(map(str,sources))+['-o',str(binary)],check=True)
  subprocess.run([str(binary)],check=True)
if __name__=='__main__':main()
