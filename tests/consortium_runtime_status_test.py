#!/usr/bin/env python3
"""Compile the actual runtime health snapshot against bounded link fixtures."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
from consortium_gatt_callback_test import function

ROOT=Path(__file__).resolve().parents[1]
PREFIX=r'''
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>
struct RuntimeStatus{
    bool connected=false,synchronized=false,initialized=true,coordinator=true,member=false,authority_ready=true;
    uint32_t active_peers=0;uint64_t epoch=9;
};
struct Tls{int authenticated=4;};
struct Active{
    bool cancelled=false,retiring=false,ready=true,credential_current=true;int peer=4;
    std::shared_ptr<Tls> tls=std::make_shared<Tls>();std::mutex mutex;
    struct{int id=4,boot_id=6;uint64_t config_revision=7;bool ready=true;}status;
};
struct Group{}group;
std::mutex lock;RuntimeStatus status;uint64_t configuration=7;
std::vector<std::shared_ptr<Active>>links;
bool current_peer_credential(const Active&link,const Group&){return link.credential_current;}
void check(bool value,const char*message){if(!value)throw std::runtime_error(message);}
'''
SUITE=r'''
void expect(bool connected,bool synchronized,unsigned peers){
    const auto out=runtime_status();check(out.connected==connected&&out.synchronized==synchronized&&out.active_peers==peers,"incorrect current-link health aggregate");
    check(out.initialized&&out.coordinator&&!out.member&&out.authority_ready&&out.epoch==9,"health snapshot changed membership authority fields");
}
int main(){try{
    auto healthy=std::make_shared<Active>();links={healthy};status.connected=status.synchronized=false;
    const auto first=runtime_status();check(first.connected&&first.synchronized,"healthy link hidden by stale global status");expect(true,true,1);
    auto probe=std::make_shared<Active>();probe->peer=0;probe->status.id=0;probe->status.boot_id=0;probe->ready=false;links.push_back(probe);expect(true,true,1);
    links.pop_back();status.synchronized=false;expect(true,true,1);
    // A revision transition changes execution scope, not the health of this
    // same authenticated TLS connection. Preserve its ready/boot identity
    // while the link task advances in-band, then catch up without a blink.
    const auto* tls=healthy->tls.get();const auto boot=healthy->status.boot_id;
    configuration=8;expect(true,true,1);
    check(healthy->status.config_revision==7&&healthy->tls.get()==tls&&healthy->status.boot_id==boot,"health snapshot mutated the live revision transition");
    healthy->status.config_revision=8;expect(true,true,1);
    configuration=9;status.connected=status.synchronized=false;expect(true,true,1);
    healthy->status.config_revision=9;expect(true,true,1);
    auto old=std::make_shared<Active>();old->status.config_revision=6;old->cancelled=true;links.push_back(old);expect(true,true,1);
    links.pop_back();status.synchronized=false;expect(true,true,1);
    auto unready=std::make_shared<Active>();unready->ready=unready->status.ready=false;links.push_back(unready);expect(true,true,2);
    healthy->ready=healthy->status.ready=false;expect(true,false,2); // Management/sync connection has no execution readiness.
    links.clear();status.connected=status.synchronized=true;expect(false,false,0);
    // Configuration equality is tested by Session/owned-work admission, not
    // used as a second connectivity permission. Every identity/credential
    // rejection below still excludes the link irrespective of its revision.
    for(unsigned kind=0;kind<7;++kind){auto link=std::make_shared<Active>();
        switch(kind){case 0:link->cancelled=true;break;case 1:link->retiring=true;break;case 2:link->status.boot_id=0;break;case 3:link->tls->authenticated=99;break;case 4:link->credential_current=false;break;case 5:link->status.id=99;break;case 6:link->tls.reset();break;}
        links={link};expect(false,false,0);
    }
    check(status.connected&&status.synchronized,"snapshot rewrote stored authority or transition state");
    std::cout<<"PASS actual runtime health: same TLS/boot/readiness survives live revision advance and old-link close; probe/unready/cancelled/retiring/identity/auth fences and authority unchanged\n";
    return 0;
}catch(const std::exception&e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
'''


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source',type=Path,default=ROOT/'components/consortium/runtime.cpp')
    source=parser.parse_args().source.read_text()
    actual=function(source,'RuntimeStatus runtime_status()')
    with tempfile.TemporaryDirectory(prefix='amity-runtime-status-') as directory:
        cpp=Path(directory)/'test.cpp';binary=Path(directory)/'test';cpp.write_text(PREFIX+actual+SUITE)
        subprocess.run(shlex.split(os.environ.get('CXX','c++'))+['-std=c++17','-Wall','-Wextra','-Werror','-g','-fsanitize=address,undefined',str(cpp),'-o',str(binary)],check=True)
        return subprocess.run([str(binary)],check=False).returncode


if __name__=='__main__':raise SystemExit(main())
