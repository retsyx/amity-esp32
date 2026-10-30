#!/usr/bin/env python3
"""Exercise actual LAN enrollment through its prepared-member restart boundary."""
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
#include <algorithm>
#include <functional>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>
using namespace amity::consortium;
using amity::consortium::platform::Group;
namespace model=amity::system;
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
template<class T>T ident(unsigned n){char value[33];std::snprintf(value,sizeof(value),"%032x",n);return *T::parse(value);}
template<class T>T random_id(){return ident<T>(90);}
uint64_t clock_ms=100;
uint64_t now_ms(){return clock_ms;}
struct FakeTls {
    std::vector<std::string> frames;size_t next=0;
    bool receive(std::string& out){if(next==frames.size())return false;out=frames[next++];return true;}
};
struct Active {Session session;FakeTls transport;FakeTls* tls=&transport;model::OctopusId peer;};
std::mutex lock,enrollment_lock;
Group group;RuntimeStatus status;RuntimeNetworkHooks network;
std::vector<std::string> events;
bool ack_fails=false,commit_fails=false,save_fails=false,finish_fails=false,handoff_fails=false,internal_stack=false;
unsigned handoffs=0,restarts=0;
bool enrollment_abort_pending(const Active&){return false;}
bool finish_enrollment_abort(Active&){return false;}
bool abort_peer_credential(const Active&,const Group&){return false;}
bool candidate_join_status(Active&,model::JoinId,const std::string&){return false;}
bool abort_candidate_enrollment(Active&,model::JoinId,const std::string&){return false;}
esp_err_t save_group(const Group& next){check(next.role==Group::member,"unexpected group write");events.push_back("group_commit");return save_fails?ESP_FAIL:ESP_OK;}
bool send(Active&,Kind kind,std::string_view payload){
    Json value;JsonError error;check(kind==Kind::enrollment_control&&parse_json(payload,value,error),"invalid enrollment send");
    const auto phase=value.get("phase")->string;events.push_back(phase);
    return phase!="ack"||!ack_fails;
}
void amity::consortium::runtime_close_enrollment(){events.push_back("close_enrollment");}
unsigned pdMS_TO_TICKS(unsigned ms){return ms;}
void vTaskDelay(unsigned ms){check(ms==500,"post-ACK delay changed");events.push_back("delay_500");clock_ms+=ms;}
struct Restart {};
void esp_restart(){check(internal_stack,"restart used the external link stack");events.push_back("restart");++restarts;throw Restart{};}
namespace config_store {
bool with_internal_flash_stack(std::function<void()> action){
    ++handoffs;events.push_back("internal_handoff");if(handoff_fails)return false;
    struct Restore {~Restore(){internal_stack=false;}} restore;
    internal_stack=true;action();return true;
}
}
'''

SUITE = r'''
void configure(Active& active,Group::Role role=Group::prepared,uint64_t commit_revision=3){
    clock_ms=100;group={};status={};events.clear();handoffs=restarts=0;internal_stack=false;
    ack_fails=commit_fails=save_fails=finish_fails=handoff_fails=false;
    group.role=role;group.consortium=ident<model::ConsortiumId>(2);group.authority=ident<model::OctopusId>(3);
    group.epoch=1;group.revision=3;group.authority_host="192.0.2.1";group.join_id=ident<model::JoinId>(22);group.joining_id=ident<model::OctopusId>(4);
    group.join_digest=std::string(64,'a');group.join_hostname="amity-candidate";group.join_revision=3;group.peers.push_back({group.authority,"leaf","pin",role==Group::member});
    active.peer=group.authority;
    LinkScope scope{ident<model::LinkId>(1),group.consortium,group.joining_id,group.authority,group.authority,ident<model::BootId>(6),ident<model::BootId>(5),1,7};
    HelloFrame hello;hello.consortium_id=scope.consortium_id;hello.octopus_id=scope.peer_id;hello.boot_id=scope.peer_boot_id;hello.coordinator_epoch=1;hello.config_revision=7;hello.required_capabilities=known_capabilities;
    check(active.session.authenticate(scope,scope.peer_id,PeerRole::provisional_candidate,false)==Error::none&&active.session.hello(hello,known_capabilities)==Error::none,"provisional session did not authenticate");
    const auto incoming=[&](unsigned id,const std::string& payload){
        Envelope e;e.kind=Kind::enrollment_control;e.message_id=ident<model::MessageId>(id);e.consortium_id=scope.consortium_id;e.origin_octopus_id=scope.peer_id;e.destination_octopus_id=scope.local_id;e.origin_boot_id=scope.peer_boot_id;e.destination_boot_id=scope.local_boot_id;e.coordinator_epoch=1;e.config_revision=7;e.ttl_ms=2000;return envelope_json(e,payload);
    };
    active.transport.frames={incoming(10,control_json(group.join_id,group.join_digest,"challenge",",\"challenge_nonce\":\""+ident<model::MessageId>(11).str()+"\"")),incoming(12,control_json(group.join_id,group.join_digest,"commit",",\"membership_revision\":\""+std::to_string(commit_revision)+"\",\"ready\":false"))};
    network={};network.commit_transaction=[](model::JoinId join,const std::string& digest){check(join==group.join_id&&digest==group.join_digest,"network commitment used a different join");events.push_back("network_commit");return commit_fails?ESP_FAIL:ESP_OK;};
    network.finish_transaction=[](model::JoinId join,const std::string& digest){check(join==group.join_id&&digest==group.join_digest,"network finish used a different join");events.push_back("network_finish");return finish_fails?ESP_FAIL:ESP_OK;};
}
bool run(Active& active,bool& rebooted){try{return enroll_lan(active,false);}catch(const Restart&){rebooted=true;return false;}}
void restart_tests(){
    {Active active;configure(active);bool rebooted=false;
     check(!run(active,rebooted)&&rebooted&&restarts==1&&handoffs==1,"prepared member did not restart through the internal handoff");
     check(group.role==Group::member&&status.member&&group.peers.front().active,"restart preceded durable member commitment");
     check(events==std::vector<std::string>({"prove","network_commit","group_commit","network_finish","ack","close_enrollment","delay_500","internal_handoff","restart"}),"commit/ACK/restart ordering changed");
     check(clock_ms==600&&!internal_stack,"post-ACK delay or broker lifetime changed");}
    {Active active;configure(active);ack_fails=true;bool rebooted=false;
     check(!run(active,rebooted)&&!rebooted&&!handoffs&&!restarts,"failed ACK authorized a restart");
     check(group.role==Group::member&&events.back()=="ack"&&clock_ms==100,"ACK failure lost durable membership or introduced a restart delay");}
    for(unsigned failure=0;failure<4;++failure){Active active;configure(active,Group::prepared,failure==0?4:3);commit_fails=failure==1;save_fails=failure==2;finish_fails=failure==3;bool rebooted=false;
     check(!run(active,rebooted)&&!rebooted&&!handoffs&&!restarts&&std::find(events.begin(),events.end(),"ack")==events.end(),"invalid or failed commitment authorized an ACK/restart");}
    {Active active;configure(active);handoff_fails=true;bool rebooted=false;
     check(!run(active,rebooted)&&!rebooted&&handoffs==1&&!restarts&&!internal_stack,"failed internal handoff used an unsafe restart fallback");
     check(group.role==Group::member&&events.back()=="internal_handoff","handoff failure undid the acknowledged durable member state");}
    {Active active;configure(active,Group::member);bool rebooted=false;
     check(run(active,rebooted)&&!rebooted&&!handoffs&&!restarts,"already committed member unnecessarily restarted");
     check(events.back()=="close_enrollment"&&clock_ms==100,"existing-member completion added a restart delay");}
    {Active active;configure(active);group.join_id={};bool rebooted=false;
     check(!run(active,rebooted)&&!rebooted&&events.empty()&&!handoffs,"unprepared membership authorized a restart");}
    std::cout<<"PASS actual LAN enrollment restart: durable network/member commit and successful ACK precede unchanged500ms delay/internal-stack handoff; failed ACK/commit/handoff and existing members never restart\n";
}
int main(){try{restart_tests();return 0;}catch(const std::exception& error){std::cerr<<"FAIL "<<error.what()<<'\n';return 1;}}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT / 'components/consortium/runtime.cpp')
    source = parser.parse_args().source.read_text()
    actual = function(source, 'std::string control_json(') + function(source, 'bool enroll_lan(')
    encoder = function((ROOT / 'components/consortium/tls_link.cpp').read_text(), 'std::string envelope_json(')
    with tempfile.TemporaryDirectory(prefix='amity-enrollment-restart-') as directory:
        directory = Path(directory)
        (directory / 'esp_err.h').write_text('#pragma once\nusing esp_err_t=int;constexpr int ESP_OK=0,ESP_FAIL=-1;\n')
        cpp = directory / 'test.cpp'
        binary = directory / 'test'
        cpp.write_text(timing_support() + PREFIX + encoder + actual + SUITE)
        sources = sorted((ROOT / 'components/consortium').glob('*.cpp')) + [ROOT / 'components/amity_system/model.cpp']
        subprocess.run(shlex.split(os.environ.get('CXX', 'c++')) +
                       ['-std=c++17', '-Wall', '-Wextra', '-Werror', '-g', '-fsanitize=address,undefined',
                        '-I' + str(directory), '-I' + str(ROOT / 'components/consortium/include'),
                        '-I' + str(ROOT / 'components/amity_system/include')] +
                       [str(path) for path in sources] + [str(cpp), '-o', str(binary)], check=True)
        return subprocess.run([str(binary)], check=False).returncode


if __name__ == '__main__':
    raise SystemExit(main())
