#!/usr/bin/env python3
"""Compile actual runtime heartbeat/retirement paths with real wire/session code."""
import os
import argparse
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
from consortium_gatt_callback_test import function

ROOT = Path(__file__).resolve().parents[1]
PREFIX = r'''
#include "amity/consortium/session.hpp"
#include "amity/consortium/owned_intent.hpp"
#include "amity/consortium/credential_types.hpp"
#include <algorithm>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>
using namespace amity::consortium;
namespace m=amity::system;
uint64_t clock_ms=100;
uint64_t now_ms(){return clock_ms++;}
struct Queued {Kind kind;std::string payload;uint32_t ttl=0;uint64_t created=0,revision=0;std::shared_ptr<const model::OwnedIntent> owned;};
struct FakeTls {
    bool server=true;model::OctopusId authenticated;
    std::vector<std::string> frames;size_t next=0;uint32_t largest_timeout=0;
    bool receive(std::string &out,uint32_t timeout){largest_timeout=std::max(largest_timeout,timeout);if(next==frames.size()){clock_ms+=timeout;return false;}out=frames[next++];return true;}
};
struct Active {
    model::OctopusId peer;model::MessageId removal_request;uint64_t removal_until=0;bool management_ready=false;
    Session session;FakeTls transport;FakeTls *tls=&transport;bool cancelled=false,ready=true,retiring=true;
    struct {bool ready=true,management_ready=false;} status;std::mutex mutex;std::vector<int> queue{1};
    std::string retirement_payload;
};
std::mutex lock;platform::Group group;struct{model::OctopusId id;}identity;struct{bool authority_ready=true;}status;uint64_t configuration=7;model::BootId boot;
// REMOVAL_OPERATION
bool original_controller(decltype(group)& value,model::OctopusId owner){return value.role==platform::Group::coordinator&&value.authority==owner;}
std::string sha256(std::string_view){return std::string(64,'a');} // Digest scope construction is tested with nonconstant vectors in consortium_removal_test.
void removal_finish(model::OctopusId,model::MessageId,bool){}
bool send(Active &,Kind kind,std::string_view,uint32_t){return kind==Kind::membership_event;}
void check(bool value,const char *message){if(!value)throw std::runtime_error(message);}
template<class T>T ident(unsigned n){char text[33];std::snprintf(text,sizeof(text),"%032x",n);return *T::parse(text);}
LinkScope scope(uint64_t config=7){return {ident<m::LinkId>(1),ident<m::ConsortiumId>(2),ident<m::OctopusId>(3),ident<m::OctopusId>(4),ident<m::OctopusId>(3),ident<m::BootId>(5),ident<m::BootId>(6),1,config};}
void start(Active &active,uint64_t config=7,uint32_t capabilities=known_capabilities){
    active.removal_request=ident<m::MessageId>(32);active.removal_until=clock_ms+2000;configuration=config;
    const auto sc=scope(config);HelloFrame hello;hello.consortium_id=sc.consortium_id;hello.octopus_id=sc.peer_id;hello.boot_id=sc.peer_boot_id;hello.coordinator_epoch=sc.epoch;hello.config_revision=sc.config_revision;hello.required_capabilities=capabilities;
    active.peer=sc.peer_id;active.tls->authenticated=sc.peer_id;identity.id=sc.local_id;boot=sc.local_boot_id;
    group={};group.role=platform::Group::coordinator;group.authority=sc.coordinator_id;group.consortium=sc.consortium_id;group.epoch=sc.epoch;group.revision=2;
    removal_operations.clear();removal_operations.push_back({active.peer,active.removal_request,active.removal_until,"pending",false,sc,2});
    check(active.session.authenticate(sc,sc.peer_id,PeerRole::actuator,true)==Error::none,"authenticate failed");
    check(active.session.hello(hello,known_capabilities)==Error::none&&active.session.synchronized(config,true,true)==Error::none,"session did not synchronize");
}
Envelope envelope(Kind kind,unsigned message=10,uint64_t config=7){const auto s=scope(config);Envelope e;e.kind=kind;e.message_id=ident<m::MessageId>(message);e.consortium_id=s.consortium_id;e.origin_octopus_id=s.peer_id;e.origin_boot_id=s.peer_boot_id;e.destination_octopus_id=s.local_id;e.destination_boot_id=s.local_boot_id;e.coordinator_epoch=s.epoch;e.config_revision=s.config_revision;e.ttl_ms=2000;return e;}
std::string packet(const Envelope&e,std::string payload){return "{\"protocol_version\":1,\"consortium_id\":\""+e.consortium_id.str()+"\",\"message_id\":\""+e.message_id.str()+"\",\"origin_octopus_id\":\""+e.origin_octopus_id.str()+"\",\"origin_boot_id\":\""+e.origin_boot_id.str()+"\",\"destination_octopus_id\":\""+e.destination_octopus_id.str()+"\",\"destination_boot_id\":\""+e.destination_boot_id.str()+"\",\"coordinator_epoch\":\""+std::to_string(e.coordinator_epoch)+"\",\"config_revision\":\""+std::to_string(e.config_revision)+"\",\"kind\":\""+kind_name(e.kind)+"\",\"ttl_ms\":2000,\"payload\":"+payload+"}";}
const std::string heartbeat="{\"sequence\":\"1\",\"observation_cursor\":\"0\",\"ready\":true}";
const std::string requested="{\"mutation_id\":\"00000000000000000000000000000020\",\"proposal_digest\":\""+std::string(64,'a')+"\",\"phase\":\"factory_reset\",\"membership_revision\":\"2\",\"expected_boot_id\":\"00000000000000000000000000000006\"}";
std::string acknowledgement(bool correct=true){auto payload=requested;if(!correct)payload.replace(payload.find(std::string(64,'a')),64,std::string(64,'b'));payload.pop_back();return packet(envelope(Kind::membership_result,20),payload+",\"result\":\"reset_scheduled\"}");}
std::string lease(Kind kind,unsigned message){const auto base="{\"lease_id\":\"00000000000000000000000000000021\",\"link_id\":\"00000000000000000000000000000001\",\"class\":\"control\",\"validity_ms\":5000}";return packet(envelope(kind,message),base);}
const std::string settings_payload="{\"request_id\":\"00000000000000000000000000000100\",\"operation\":\"read\",\"stage\":\"applied\",\"error\":\"\",\"snapshot\":{\"boot_id\":\"00000000000000000000000000000006\",\"settings_revision\":\"1\",\"adapters\":[{\"tentacle_id\":0,\"name\":\"HDMI adapter 1\",\"configured\":true,\"presence\":\"present\",\"presence_generation\":\"1\",\"default_front_physical_address\":\"1.0.0.0\",\"observed_front_physical_address\":null,\"effective_front_physical_address\":\"1.0.0.0\"}]},\"rebooting\":false}";
std::string settings_reply(unsigned message=70){return packet(envelope(Kind::settings_result,message),settings_payload);}
'''
SUITE = r'''
void heartbeat_tests(){
    Active active;start(active);const auto valid=envelope(Kind::heartbeat);
    check(dispatch_heartbeat(active,valid),"valid heartbeat rejected");
    check(!dispatch_heartbeat(active,valid),"duplicate heartbeat accepted");
    for(unsigned failure=0;failure<7;++failure){auto bad=envelope(Kind::heartbeat,30+failure);
        switch(failure){case 0:bad.origin_boot_id=ident<m::BootId>(99);break;case 1:bad.destination_boot_id=ident<m::BootId>(99);break;case 2:bad.destination_octopus_id=ident<m::OctopusId>(99);break;case 3:bad.consortium_id=ident<m::ConsortiumId>(99);break;case 4:bad.config_revision=8;break;case 5:bad.coordinator_epoch=2;break;case 6:bad.origin_octopus_id=ident<m::OctopusId>(99);break;}
        LinkTrace trace;check(!dispatch_heartbeat(active,bad,&trace),"foreign heartbeat bypassed actual runtime admission");
        check(trace.unexpected&&trace.error!=Error::none&&std::string(trace.reason)=="heartbeat_admit","heartbeat rejection lost its admission diagnostic");
    }
    // A committed new config must not relabel an existing TLS link: old-scope
    // heartbeats remain valid until reconnect; the new link accepts only8.
    check(dispatch_heartbeat(active,envelope(Kind::heartbeat,50,7)),"old scoped heartbeat rejected during transition");
    Active updated;start(updated,8);check(dispatch_heartbeat(updated,envelope(Kind::heartbeat,51,8)),"new scoped heartbeat rejected");
    check(!dispatch_heartbeat(updated,envelope(Kind::heartbeat,52,7)),"old revision accepted on new link");
    updated.session.restrict_management();check(dispatch_heartbeat(updated,envelope(Kind::heartbeat,53,8)),"valid management heartbeat rejected");
    std::cout<<"PASS actual heartbeat dispatch: exact boots/identities/group/epoch/config/replay; old/new config transition and management\n";
}
void retirement_tests(){
    {Active active;start(active);active.retirement_payload=requested;active.transport.frames={packet(envelope(Kind::heartbeat,60),heartbeat),lease(Kind::execution_lease,61),acknowledgement()};
     check(finish_retirement(active),"retirement failed behind in-flight heartbeat/leases");check(!active.ready&&!active.status.ready&&active.queue.empty(),"retirement did not fence queue");check(active.session.execution_leases().live(LeaseClass::control,clock_ms)==0,"drained frame granted authority");check(active.transport.largest_timeout<=2000,"retirement extended two-second deadline");}
    {Active active;start(active);active.retirement_payload=requested;active.transport.frames={acknowledgement(false)};check(!finish_retirement(active),"wrong retirement digest accepted");}
    {Active active;start(active);active.retirement_payload=requested;auto foreign=envelope(Kind::heartbeat,63);foreign.config_revision=8;active.transport.frames={packet(foreign,heartbeat),acknowledgement()};check(!finish_retirement(active)&&active.transport.next==1,"foreign frame was drained or acknowledged");}
    {Active active;start(active);active.retirement_payload=requested;for(unsigned i=0;i<33;++i)active.transport.frames.push_back(packet(envelope(Kind::heartbeat,100+i),heartbeat));active.transport.frames.push_back(acknowledgement());check(!finish_retirement(active)&&active.transport.next==32,"retirement drain exceeded32 frames");}
    {Active active;start(active);active.retirement_payload=requested;check(!finish_retirement(active)&&active.transport.largest_timeout<=2000,"absent ACK did not time out bounded");}
    std::cout<<"PASS actual retirement:32-frame/2s drain, no lease grants, exact ACK digest, foreign-scope and missing-ACK rejection\n";
}
void settings_retirement_tests(){
    {Active active;start(active);active.retirement_payload=requested;Envelope reply;
     check(parse_envelope(settings_reply(),reply)==Error::none&&active.session.admit(reply,clock_ms,clock_ms,false)==Error::none,"fixture is not a valid ready-session settings reply");
     active.transport.frames={settings_reply(),acknowledgement()};
     check(finish_retirement(active)&&active.transport.next==2,"in-flight settings reply hid the matching reset ACK");
     check(active.session.state()==ConnectionState::restricted&&!active.ready&&!active.status.ready&&active.queue.empty(),"drained settings reply restored work authority");
     check(active.session.execution_leases().live(LeaseClass::control,clock_ms)==0,"drained settings reply granted a lease");
     check(active.transport.largest_timeout<=2000,"settings reply drain extended the removal deadline");
     check(!dispatch_heartbeat(active,envelope(Kind::heartbeat,80)),"settings reply left the session in management mode");}
    for(unsigned failure=0;failure<7;++failure){Active active;start(active);active.retirement_payload=requested;auto foreign=envelope(Kind::settings_result,71);
     switch(failure){case 0:foreign.origin_boot_id=ident<m::BootId>(99);break;case 1:foreign.destination_boot_id=ident<m::BootId>(99);break;case 2:foreign.origin_octopus_id=ident<m::OctopusId>(99);break;case 3:foreign.destination_octopus_id=ident<m::OctopusId>(99);break;case 4:foreign.consortium_id=ident<m::ConsortiumId>(99);break;case 5:foreign.coordinator_epoch=2;break;case 6:foreign.config_revision=8;break;}
     active.transport.frames={packet(foreign,settings_payload),acknowledgement()};
     check(!finish_retirement(active)&&active.transport.next==1,"foreign settings reply was drained or acknowledged");}
    {Active active;start(active);active.retirement_payload=requested;active.transport.frames={settings_reply()};
     check(!finish_retirement(active)&&active.transport.largest_timeout<=2000,"settings reply counted as a reset ACK");}
    {Active active;start(active);active.retirement_payload=requested;Envelope seen;
     check(parse_envelope(settings_reply(),seen)==Error::none&&active.session.admit(seen,clock_ms,clock_ms)==Error::none,"could not record a prior settings reply");
     active.transport.frames={settings_reply(),acknowledgement()};
     check(!finish_retirement(active)&&active.transport.next==1,"previously received settings reply bypassed replay checks");}
    {Active active;start(active);active.retirement_payload=requested;active.transport.frames={settings_reply(),settings_reply(),acknowledgement()};
     check(!finish_retirement(active)&&active.transport.next==2,"replayed settings reply bypassed drain replay checks");}
    {Active active;start(active);active.retirement_payload=requested;active.transport.frames={settings_reply(20),acknowledgement()};
     check(!finish_retirement(active)&&active.transport.next==2,"ACK reused a drained reply message ID");}
    {Active active;start(active,7,known_capabilities&~per_adapter_settings);active.retirement_payload=requested;active.transport.frames={settings_reply(),acknowledgement()};
     check(!finish_retirement(active)&&active.transport.next==1&&active.session.state()==ConnectionState::restricted,"settings drain ignored negotiated capability or retained management permission");}
    {Active active;start(active);active.retirement_payload=requested;active.transport.frames={packet(envelope(Kind::settings_read,72),"{\"request_id\":\"00000000000000000000000000000100\"}"),acknowledgement()};
     check(!finish_retirement(active)&&active.transport.next==1,"settings work was admitted during retirement");}
    {Active active;start(active);active.retirement_payload=requested;auto malformed=settings_payload;malformed.replace(malformed.find("\"applied\""),9,"\"failed\"");active.transport.frames={packet(envelope(Kind::settings_result,73),malformed),acknowledgement()};
     check(!finish_retirement(active)&&active.transport.next==1,"malformed settings reply was drained");}
    {Active active;start(active);active.retirement_payload=requested;for(unsigned i=0;i<33;++i)active.transport.frames.push_back(settings_reply(200+i));active.transport.frames.push_back(acknowledgement());
     check(!finish_retirement(active)&&active.transport.next==32,"settings reply drain exceeded32 frames");}
    std::cout<<"PASS actual retirement behind settings replies: exact ACK required; scope/capability/replay/32-frame/2s fences retained; no settings work or lease authority\n";
}
int main(){try{heartbeat_tests();retirement_tests();settings_retirement_tests();return 0;}catch(const std::exception&e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
'''


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source',type=Path,default=ROOT/'components/consortium/runtime.cpp')
    source=parser.parse_args().source.read_text()
    # The heartbeat fallback follows the non-heartbeat control branch. Locate
    # that structural boundary, then retain the entire actual fallback body,
    # including diagnostics and break-on-rejection, regardless of line layout.
    loop = function(source, 'void active_loop_work(')
    control = re.search(r'else\s+if\s*\(\s*envelope\.kind\s*!=\s*Kind::heartbeat\s*\)\s*\{', loop)
    if control is None:
        raise ValueError('The runtime non-heartbeat control branch was not found')
    control_body = function(loop, control.group()).rstrip('\n')
    tail = loop[control.start() + len(control_body):]
    fallback = re.match(r'\s*(else\s*\{)', tail)
    if fallback is None:
        raise ValueError('The runtime heartbeat fallback was not found after control dispatch')
    branch = function(tail, fallback.group(1))
    trace = function(source, 'struct LinkTrace {').rstrip() + ';\n'
    dispatch = '\nbool dispatch_heartbeat(Active &storage,Envelope envelope,LinkTrace* diagnostic=nullptr){Active *active=&storage;LinkTrace local;auto& trace=diagnostic?*diagnostic:local;const auto received=now_ms();while(true){if(false){}' + branch + 'return true;}return false;}\n'
    actual = function(source, 'bool lifecycle_receive(Active &active,') + function(source, 'std::string retirement_hash(') + function(source, 'bool finish_retirement(Active &active)')
    operation = function((ROOT / 'components/consortium/removal_runtime.inc').read_text(), 'struct RemovalOperation {').rstrip() + ';\nstd::vector<RemovalOperation> removal_operations;\n'
    with tempfile.TemporaryDirectory(prefix='amity-runtime-admission-') as directory:
        cpp = Path(directory) / 'test.cpp'; binary = Path(directory) / 'test'
        cpp.write_text(PREFIX.replace('// REMOVAL_OPERATION', operation) + trace + dispatch + actual + SUITE)
        sources = sorted((ROOT / 'components/consortium').glob('*.cpp')) + [ROOT / 'components/amity_system/model.cpp']
        subprocess.run(shlex.split(os.environ.get('CXX', 'c++')) +
                       ['-std=c++17', '-Wall', '-Wextra', '-Werror', '-g', '-fsanitize=address,undefined',
                        '-I' + str(ROOT / 'components/consortium/include'), '-I' + str(ROOT / 'components/amity_system/include')] +
                       [str(path) for path in sources] + [str(cpp), '-o', str(binary)], check=True)
        return subprocess.run([str(binary)], check=False).returncode


if __name__ == '__main__':
    raise SystemExit(main())
