#!/usr/bin/env python3
"""Actual retired-wire rejection and persisted-state passive-startup regressions."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile
from consortium_gatt_callback_test import function

ROOT = Path(__file__).resolve().parents[1]
PREFIX = r'''
#include "amity/consortium/session.hpp"
#include "amity/consortium/transfer.hpp"
#include "controller_policy.hpp"
#include "amity/consortium/hostname.hpp"
#include <algorithm>
#include <cassert>
#include <iostream>
#include <map>
using namespace amity::consortium;
using esp_err_t=int;
constexpr int ESP_OK=0,ESP_ERR_INVALID_STATE=1,ESP_ERR_NVS_NOT_FOUND=2,ESP_ERR_INVALID_SIZE=3,ESP_ERR_INVALID_ARG=4,PSA_SUCCESS=0;
int psa_crypto_init(){return PSA_SUCCESS;}
template<class T>T ident(unsigned n){char text[33];snprintf(text,sizeof(text),"%032x",n);return *T::parse(text);}
std::string replace(std::string s,std::string_view a,std::string_view b){auto at=s.find(a);assert(at!=std::string::npos);s.replace(at,a.size(),b);return s;}
std::map<std::string,std::string> records;std::string fail_write;std::vector<std::string> writes;
namespace amity::consortium::platform {
std::string hex(std::string_view v){std::string out;for(unsigned char c:v){out+="0123456789abcdef"[c>>4];out+="0123456789abcdef"[c&15];}return out;}
bool unhex(std::string_view v,std::string& out,size_t cap){if(v.size()%2||v.size()>cap*2)return false;out.clear();for(size_t i=0;i<v.size();i+=2){auto digit=[](char c){return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:-1;};const auto a=digit(v[i]),b=digit(v[i+1]);if(a<0||b<0)return false;out+=char(a*16+b);}return true;}
int read_record(const char* space,const char* key,std::string& out,size_t cap){auto it=records.find(std::string(space)+"/"+key);if(it==records.end())return ESP_ERR_NVS_NOT_FOUND;if(it->second.size()>cap)return ESP_ERR_INVALID_SIZE;out=it->second;return ESP_OK;}
int write_record(const char* space,const char* key,std::string_view value){writes.emplace_back(key);if(fail_write==key)return ESP_ERR_INVALID_STATE;records[std::string(space)+"/"+key]=value;return ESP_OK;}
// Certificate/secret generation is outside this storage/parser regression.
bool certificate(std::string_view leaf,model::OctopusId id,std::string& pin){pin=std::string(64,'a');return id&&!leaf.empty();}
int load_identity(model::OctopusId id,Identity& out){out.id=id;return ESP_OK;}
'''
MIDDLE = r'''
}
using namespace amity::consortium::platform;
constexpr size_t maximum_roster_peers=2;
struct RuntimeStartup {
 enum Role{unclaimed,coordinator,member,prepared,retired,recovery_required};Role role=unclaimed;
 model::ConsortiumId consortium_id;model::OctopusId authority_id;uint64_t epoch=0,membership_revision=0;
 bool passive()const{return role==member||role==prepared||role==retired||role==recovery_required;}
};
'''
SUITE = r'''
std::string packet_name(std::string_view name,std::string_view payload){return "{\"protocol_version\":1,\"consortium_id\":\""+ident<model::ConsortiumId>(1).str()+"\",\"message_id\":\""+ident<model::MessageId>(2).str()+"\",\"origin_octopus_id\":\""+ident<model::OctopusId>(3).str()+"\",\"origin_boot_id\":\""+ident<model::BootId>(4).str()+"\",\"destination_octopus_id\":\""+ident<model::OctopusId>(5).str()+"\",\"destination_boot_id\":\""+ident<model::BootId>(6).str()+"\",\"coordinator_epoch\":\"1\",\"config_revision\":\"1\",\"kind\":\""+std::string(name)+"\",\"ttl_ms\":1000,\"payload\":"+std::string(payload)+"}";}
std::string packet(Kind kind,std::string_view payload){return packet_name(kind_name(kind),payload);}
int main(){
 Envelope envelope;for(unsigned number:{2u,3u,4u,5u,6u,12u,13u,18u,19u,20u,21u,22u,23u,24u,25u,26u,33u,34u,35u,36u}){const auto kind=static_cast<Kind>(number);assert(retired_kind(kind)&&std::string_view(kind_name(kind)).empty());}for(const auto& retired:std::initializer_list<std::pair<unsigned,const char*>>{{2,"ingress_lease"},{4,"hold_renew"},{6,"integration_state"},{26,"intent"},{35,"remote_management"},{36,"remote_management_result"}}){const auto kind=static_cast<Kind>(retired.first);assert(retired_kind(kind)&&std::string_view(kind_name(kind)).empty()&&parse_envelope(packet_name(retired.second,"{}"),envelope)==Error::unsupported_kind);}
 const auto retirement="{\"mutation_id\":\""+ident<model::MutationId>(7).str()+"\",\"proposal_digest\":\""+std::string(64,'a')+"\",\"phase\":\"factory_reset\",\"membership_revision\":\"1\",\"expected_boot_id\":\"00000000000000000000000000000007\"}";
 assert(parse_envelope(packet(Kind::membership_event,retirement),envelope)==Error::none);
 for(auto phase:{"handover_prepare","handover_fence","handover_commit","status","promote","retire"})assert(parse_envelope(packet(Kind::membership_event,replace(retirement,"factory_reset",phase)),envelope)==Error::invalid_message);
 HelloFrame hello;hello.consortium_id=ident<model::ConsortiumId>(1);hello.octopus_id=ident<model::OctopusId>(3);hello.boot_id=ident<model::BootId>(4);hello.coordinator_epoch=1;hello.config_revision=1;hello.config_hash=std::string(64,'a');hello.firmware_identity=std::string(64,'b');hello.required_capabilities=known_capabilities;hello.optional_capabilities=4096|0x80000000u;
 HelloFrame parsed;auto body=hello_json(hello);assert(parse_hello(body,parsed)==Error::none); // Unknown optional bits never re-enable their retired feature.
 auto extra=body;extra.pop_back();extra+=",\"handover_id\":\""+ident<model::MessageId>(8).str()+"\",\"proposal_digest\":\""+std::string(64,'c')+"\"}";assert(parse_hello(extra,parsed)==Error::invalid_message);
 hello.required_capabilities|=4096;assert(parse_hello(hello_json(hello),parsed)!=Error::none);
 LinkScope scope{ident<model::LinkId>(9),hello.consortium_id,ident<model::OctopusId>(5),hello.octopus_id,hello.octopus_id,ident<model::BootId>(6),hello.boot_id,1,1};Session session;hello.required_capabilities=known_capabilities;assert(session.authenticate(scope,scope.peer_id,PeerRole::coordinator,true)==Error::none&&session.hello(hello,known_capabilities)==Error::none);assert((session.negotiated_capabilities()&4096)==0);scope.epoch=2;assert(session.authenticate(scope,scope.peer_id,PeerRole::coordinator,true)==Error::stale_epoch);
 SnapshotManifest manifest;manifest.transfer_id=ident<model::MessageId>(8);manifest.owner_octopus_id=scope.local_id;manifest.owner_boot_id=scope.local_boot_id;manifest.config_revision=1;manifest.total_length=1;manifest.payload_hash=std::string(64,'d');manifest.document_kind="handover";assert(snapshot_manifest_json(manifest,"begin",0).empty());
 const auto local=ident<model::OctopusId>(5),coordinator=ident<model::OctopusId>(3);RuntimeStartup startup;
 records.clear();assert(runtime_read_startup(local,startup)==ESP_OK&&startup.role==RuntimeStartup::unclaimed);
 for(const auto* key:{"active","proposal0","proposal1"}){records.clear();records[std::string("amity_handover/")+key]=std::string(200,'x');assert(runtime_read_startup(local,startup)!=ESP_OK&&startup.passive()&&startup.role==RuntimeStartup::recovery_required);}
 Group group;group.role=Group::member;group.consortium=hello.consortium_id;group.authority=coordinator;group.epoch=1;group.revision=1;group.authority_host="192.0.2.1";group.join_id=ident<model::JoinId>(10);group.joining_id=local;group.join_digest=std::string(64,'e');group.join_revision=1;group.peers.push_back({coordinator,"leaf",std::string(64,'a'),true});
 records.clear();assert(save_group(group)==ESP_OK);const auto clean=records["amity_group/state"];assert(clean.find("input_gateway")==std::string::npos&&clean.find("handover")==std::string::npos);
 records["amity_group/identity"]=records["amity_group/identity_tmp"]="same";
 assert(runtime_read_startup(local,startup)==ESP_OK&&startup.role==RuntimeStartup::member&&startup.authority_id==coordinator);
 records["amity_group/state"]=replace(clean,"\"active\":true","\"active\":true,\"input_gateway\":true");Group loaded;assert(load_group(loaded)==ESP_OK&&loaded.peers.size()==1&&save_group(loaded)==ESP_OK&&records["amity_group/state"]==clean);
 for(auto field:{std::string("\"handover_id\":null"),std::string("\"handover_digest\":\"bad\"")}){auto old=clean;old.pop_back();records["amity_group/state"]=old+","+field+"}";assert(runtime_read_startup(local,startup)!=ESP_OK&&startup.role==RuntimeStartup::recovery_required);}
 records["amity_group/state"]=replace(clean,"\"epoch\":\"1\"","\"epoch\":\"2\"");assert(runtime_read_startup(local,startup)!=ESP_OK&&startup.role==RuntimeStartup::recovery_required);
 group.join_hostname="living-room";records.clear();writes.clear();assert(save_group(group)==ESP_OK&&writes==std::vector<std::string>({"join_name","state"}));assert(records["amity_group/state"].find("hostname")==std::string::npos);assert(load_group(loaded)==ESP_OK&&loaded.join_hostname=="living-room");
 records["amity_group/join_name"]=replace(records["amity_group/join_name"],group.join_digest,std::string(64,'f'));assert(load_group(loaded)==ESP_OK&&loaded.join_hostname.empty()); // Stale metadata cannot rename another join.
 records.clear();fail_write="state";assert(save_group(group)!=ESP_OK&&records.count("amity_group/join_name")&&!records.count("amity_group/state"));assert(load_group(loaded)==ESP_OK&&loaded.role==Group::unclaimed); // Cut after name metadata never grants membership.
 records.clear();fail_write="join_name";assert(save_group(group)!=ESP_OK&&records.empty());fail_write.clear();
 group.epoch=2;assert(save_group(group)==ESP_ERR_INVALID_ARG);
 std::cout<<"PASS retired features: reserved wire tags/old transfer payloads rejected; optional bits ignored, required retired bit denied; actual old-marker/orphan/epoch startup passive; legacy input flag discarded on rewrite\n";
}
'''


def main():
    credentials = (ROOT / 'components/consortium/credentials.cpp').read_text()
    runtime = (ROOT / 'components/consortium/runtime.cpp').read_text()
    actual = function(credentials, 'esp_err_t load_group(Group &out)') + function(credentials, 'esp_err_t save_group(const Group &g)')
    startup = function(runtime, 'esp_err_t runtime_read_startup(model::OctopusId local,RuntimeStartup &out)')
    with tempfile.TemporaryDirectory(prefix='amity-retired-features-') as directory:
        cpp, binary = Path(directory) / 'test.cpp', Path(directory) / 'test'
        cpp.write_text(PREFIX + actual + MIDDLE + startup + SUITE)
        sources = sorted((ROOT / 'components/consortium').glob('*.cpp')) + [ROOT / 'components/amity_system/model.cpp']
        subprocess.run(shlex.split(os.environ.get('CXX', 'c++')) + ['-std=c++17', '-Wall', '-Wextra', '-Werror', '-g', '-fsanitize=address,undefined'] +
                       ['-I' + str(ROOT / p) for p in ('components/consortium', 'components/consortium/include', 'components/amity_system/include')] +
                       [str(cpp)] + [str(p) for p in sources] + ['-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
