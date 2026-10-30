#!/usr/bin/env python3
"""Exercise actual roster projection while authenticated links are in flight."""

import os
from pathlib import Path
import shlex
import subprocess
import tempfile

from consortium_gatt_callback_test import function
from consortium_removal_test import PREFIX

ROOT=Path(__file__).resolve().parents[1]

SUITE=r'''
namespace amity::consortium {
HostnameRegistry hostnames;
std::shared_ptr<Active> link(model::OctopusId owner,unsigned boot,bool ready){
    auto value=std::make_shared<Active>();value->peer=owner;value->tls->authenticated=owner;value->tls->allowed=group.peers;
    LinkScope scope{ident<model::LinkId>(100+boot),ident<model::ConsortiumId>(1),ident<model::OctopusId>(2),owner,ident<model::OctopusId>(2),ident<model::BootId>(4),ident<model::BootId>(boot),1,1};
    assert(value->session.authenticate(scope,owner,PeerRole::actuator,true)==Error::none);
    value->ready=value->status.ready=ready;value->status.id=owner;value->status.boot_id=scope.peer_boot_id;return value;
}
void checks(){
    const auto owner=ident<model::OctopusId>(3),other=ident<model::OctopusId>(9);
    group.peers={{owner,"certificate","pin",true}};links.clear();
    assert(hostnames.remember(owner,"member",[](std::string_view){return true;})==Error::none);
    auto output=runtime_peers();assert(output.size()==1&&output.front().id==owner&&output.front().enrolled&&!output.front().ready);
    // Before hello, the connection knows its peer but has not populated status.
    auto connecting=link(owner,5,false);connecting->status={};links={connecting};
    connecting->session.handshaking=true;
    output=runtime_peers();assert(output.size()==1&&output.front().id==owner&&output.front().hostname=="member"&&output.front().enrolled&&!output.front().boot_id&&!output.front().ready);
    connecting->session.handshaking=false;
    auto ready=link(owner,6,true);links={connecting,ready};output=runtime_peers();assert(output.front().id==owner&&output.front().ready&&output.front().boot_id==ident<model::BootId>(6));
    // A partial newer link never hides the still-current ready snapshot.
    connecting=link(owner,7,false);links={connecting,ready};output=runtime_peers();assert(output.front().ready&&output.front().boot_id==ident<model::BootId>(6));
    ready->cancelled=true;output=runtime_peers();assert(!output.front().ready&&output.front().boot_id==ident<model::BootId>(7));
    connecting->status.id=other;output=runtime_peers();assert(output.front().id==owner&&!output.front().boot_id);
    connecting->status.id=owner;connecting->status.boot_id=ident<model::BootId>(99);output=runtime_peers();assert(!output.front().boot_id);
    connecting->status.boot_id=ident<model::BootId>(7);connecting->tls->authenticated=other;output=runtime_peers();assert(!output.front().boot_id);
    connecting->tls->authenticated=owner;group.peers.front().der="replacement credential";output=runtime_peers();assert(!output.front().boot_id);
    connecting->tls->allowed=group.peers;group.peers.front().active=false;output=runtime_peers();assert(output.front().id==owner&&!output.front().enrolled&&output.front().provisional);
    connecting->retiring=true;output=runtime_peers();assert(output.front().id==owner&&!output.front().boot_id);
    std::cout<<"PASS actual runtime peer projection: stable roster identity, authenticated exact-boot metadata, ready snapshot preferred, partial/revoked/stale links cannot replace it\n";
}
}
int main(){amity::consortium::checks();}
'''


def main():
    source=(ROOT/'components/consortium/runtime.cpp').read_text()
    actual=function(source,'std::vector<RuntimePeerStatus> runtime_peers(')
    suite=SUITE.replace('void checks(){',actual+'\nvoid checks(){')
    with tempfile.TemporaryDirectory(prefix='amity-peer-status-') as directory:
        directory=Path(directory)
        (directory/'esp_err.h').write_text('#pragma once\nusing esp_err_t=int;constexpr int ESP_OK=0,ESP_ERR_INVALID_ARG=1,ESP_ERR_INVALID_STATE=2,ESP_ERR_NO_MEM=3,ESP_ERR_TIMEOUT=4,ESP_FAIL=5,ESP_ERR_NOT_FOUND=6,ESP_ERR_NOT_SUPPORTED=7;\n')
        cpp,binary=directory/'test.cpp',directory/'test'
        # A partial link publishes its peer before authenticate()/hello() finish.
        # Trap any scope read until initialized status is published: the actual
        # projection must avoid that concurrently written handshake state.
        prefix=PREFIX.replace('struct Active{Session session;', '''struct ProjectionSession:Session {
 bool handshaking=false;
 const LinkScope& scope()const{assert(!handshaking);return Session::scope();}
};
struct Active{ProjectionSession session;''')
        cpp.write_text('#include "hostname_registry.hpp"\n'+prefix+suite)
        sources=[ROOT/'components/consortium'/name for name in ('json.cpp','wire.cpp','session.cpp','owned_intent.cpp','settings.cpp','update.cpp','transfer.cpp')]
        sources.append(ROOT/'components/amity_system/model.cpp')
        subprocess.run(shlex.split(os.environ.get('CXX','c++'))+['-std=c++17','-Wall','-Wextra','-Werror','-g','-fsanitize=address,undefined','-I'+str(directory)]+['-I'+str(ROOT/path) for path in ('components/consortium','components/consortium/include','components/amity_system/include')]+[str(cpp),*map(str,sources),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True)


if __name__=='__main__':main()
