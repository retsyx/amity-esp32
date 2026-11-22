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
#include "setup_timing.hpp"
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
bool certificate(std::string_view leaf,model::OctopusId id,std::string& pin){pin=std::string(64,leaf.size()==1024?leaf.front():'a');return id&&!leaf.empty();}
bool pinned_leaf(std::string_view leaf,model::OctopusId id,std::string_view pin){std::string derived;return certificate(leaf,id,derived)&&derived==pin;}
int load_identity(model::OctopusId id,Identity& out,bool=false){out.id=id;return ESP_OK;}
'''
MIDDLE = r'''
}
using namespace amity::consortium::platform;
struct RuntimeStartup {
 enum Role{unclaimed,coordinator,member,prepared,retired,recovery_required};Role role=unclaimed;
 model::ConsortiumId consortium_id;model::OctopusId authority_id;uint64_t membership_revision=0;
 bool passive()const{return role==member||role==prepared||role==retired||role==recovery_required;}
};
'''
SUITE = r'''
std::string packet_name(std::string_view name,std::string_view payload){return "{\"config_revision\":\"1\",\"kind\":\""+std::string(name)+"\",\"ttl_ms\":1000,\"payload\":"+std::string(payload)+"}";}
std::string packet(Kind kind,std::string_view payload){return packet_name(kind_name(kind),payload);}
int main(){
 Envelope envelope;for(const auto* retired:{"ingress_lease","hold_renew","integration_state","intent","remote_management","remote_management_result"})assert(parse_envelope(packet_name(retired,"{}"),envelope)==Error::unsupported_kind);
 const std::string retirement="{\"phase\":\"factory_reset\"}";
 assert(parse_envelope(packet(Kind::membership_event,retirement),envelope)==Error::none);
 for(auto phase:{"handover_prepare","handover_fence","handover_commit","status","promote","retire"})assert(parse_envelope(packet(Kind::membership_event,replace(retirement,"factory_reset",phase)),envelope)==Error::invalid_message);
 HelloFrame hello;hello.consortium_id=ident<model::ConsortiumId>(1);hello.octopus_id=ident<model::OctopusId>(3);hello.boot_id=ident<model::BootId>(4);hello.config_revision=1;hello.firmware_identity=std::string(64,'b');
 HelloFrame parsed;auto body=hello_json(hello);assert(parse_hello(body,parsed)==Error::none);
 auto extra=body;extra.pop_back();extra+=",\"handover_id\":\""+ident<model::MessageId>(8).str()+"\",\"proposal_digest\":\""+std::string(64,'c')+"\"}";assert(parse_hello(extra,parsed)==Error::invalid_message);
 LinkScope scope{ident<model::LinkId>(9),hello.consortium_id,ident<model::OctopusId>(5),hello.octopus_id,hello.octopus_id,ident<model::BootId>(6),hello.boot_id,1};Session session;assert(session.authenticate(scope,scope.peer_id,PeerRole::coordinator,true)==Error::none&&session.hello(hello)==Error::none);
 SnapshotManifest manifest;manifest.transfer_id=ident<model::MessageId>(8);manifest.owner_octopus_id=scope.local_id;manifest.owner_boot_id=scope.local_boot_id;manifest.config_revision=1;manifest.total_length=1;manifest.payload_hash=std::string(64,'d');manifest.document_kind="handover";assert(snapshot_manifest_json(manifest,"begin",0).empty());
 const auto local=ident<model::OctopusId>(5),coordinator=ident<model::OctopusId>(3);RuntimeStartup startup;
 records.clear();assert(runtime_read_startup(local,startup)==ESP_OK&&startup.role==RuntimeStartup::unclaimed);
 Group group;group.role=Group::member;group.consortium=hello.consortium_id;group.authority=coordinator;group.revision=1;group.authority_host="192.0.2.1";group.join_id=ident<model::JoinId>(10);group.joining_id=local;group.join_digest=std::string(64,'e');group.join_revision=1;group.peers.push_back({coordinator,"leaf",std::string(64,'a'),true});
 records.clear();assert(save_group(group)==ESP_OK);const auto clean=records["amity_group/state"];Group loaded;
 records["amity_group/identity"]="same";
 assert(runtime_read_startup(local,startup)==ESP_OK&&startup.role==RuntimeStartup::member&&startup.authority_id==coordinator);
 group.join_hostname="living-room";records.clear();writes.clear();assert(save_group(group)==ESP_OK&&writes==std::vector<std::string>({"state"}));assert(records["amity_group/state"].find("\"join_hostname\":\"living-room\"")!=std::string::npos);assert(load_group(loaded)==ESP_OK&&loaded.join_hostname=="living-room");
 records["amity_group/state"]=replace(records["amity_group/state"],"living-room","Living Room");assert(load_group(loaded)==ESP_ERR_INVALID_STATE);
 group={};group.role=Group::coordinator;group.consortium=hello.consortium_id;group.authority=coordinator;group.revision=1;
 for(unsigned n=0;n<3;++n)group.peers.push_back({ident<model::OctopusId>(20+n),std::string(1024,'b'+n),std::string(64,'b'+n),true});
 records.clear();assert(save_group(group)==ESP_OK);const auto full=records["amity_group/state"];
 assert(full.size()>6144&&load_group(loaded)==ESP_OK&&loaded.peers.size()==3);
 for(size_t n=0;n<3;++n)assert(loaded.peers[n].id==group.peers[n].id&&loaded.peers[n].der==group.peers[n].der&&loaded.peers[n].pin==group.peers[n].pin);
 group.peers.push_back({ident<model::OctopusId>(23),"leaf",std::string(64,'a'),true});
 assert(save_group(group)==ESP_ERR_INVALID_ARG&&records["amity_group/state"]==full);
 auto overfull=full;overfull.pop_back();overfull.pop_back();
 overfull+=",{\"id\":\""+group.peers.back().id.str()+"\",\"leaf\":\"6c656166\",\"pin\":\""+std::string(64,'a')+"\",\"active\":true}]}";
 records["amity_group/state"]=overfull;assert(load_group(loaded)==ESP_ERR_INVALID_STATE);
 std::cout<<"PASS membership storage: three maximum-size peer leaves round-trip; fourth rejected without overwriting committed roster\n";
 std::cout<<"PASS retired features: reserved wire tags/old transfer payloads rejected; old handover fields rejected\n";
}
'''


def main():
    credentials = (ROOT / 'components/consortium/credentials.cpp').read_text()
    runtime = (ROOT / 'components/consortium/runtime.cpp').read_text()
    actual = function(credentials, 'esp_err_t load_group(Group &out)') + function(credentials, 'esp_err_t save_group(const Group &g)')
    startup = (function(runtime, 'esp_err_t validate_startup(') if 'esp_err_t validate_startup(' in runtime else '') + function(runtime, 'esp_err_t runtime_read_startup(model::OctopusId local,RuntimeStartup &out)')
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
