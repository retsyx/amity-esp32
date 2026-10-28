#!/usr/bin/env python3
"""Check actual task entry wrappers destroy C++ owners before FreeRTOS deletion."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
from consortium_gatt_callback_test import function

ROOT = Path(__file__).resolve().parents[1]
PREFIX = r'''
#include <algorithm>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
int destroyed=0,at_delete=-1;
bool invitation_running=true;
struct Tracked {~Tracked(){++destroyed;}};
struct Active:Tracked{};
struct InviteRequest {std::string scan_id;};
enum class AdmissionMode {button};
AdmissionMode configured_admission_mode(){return AdmissionMode::button;}
struct AdmissionClient:Tracked {AdmissionClient(AdmissionMode,uint32_t){}};
struct Id {std::string str()const{return "public-test-id";}};
struct TaskDeleted{};
// Observe BEFORE throwing. Host unwinding only cleans up the deliberately
// failing baseline afterwards; real FreeRTOS never runs those destructors.
[[noreturn]]void vTaskDeleteWithCaps(void*){at_delete=destroyed;throw TaskDeleted{};}
'''
SUITE = r'''
int main(){
    for(const std::string name: {"link","invite"}){
        destroyed=0;at_delete=-1;
        try{
            if(name=="link"){auto value=std::make_shared<Active>();auto argument=new std::shared_ptr<Active>(value);value.reset();active_loop(argument);}
            if(name=="invite")invite_worker(new InviteRequest{"public-scan-id"});
        }catch(const TaskDeleted&){}
        if(at_delete!=1||destroyed!=1){std::cerr<<"FAIL "<<name<<" owners destroyed before task delete="<<at_delete<<'\n';return 1;}
        std::cout<<"PASS actual "<<name<<" task wrapper: owning resource destroyed before task deletion\n";
    }
    destroyed=0;at_delete=-1;
    try{auto value=std::make_shared<Active>();auto argument=new std::shared_ptr<Active>(value);value.reset();baseline_link_worker(argument);}catch(const TaskDeleted&){}
    if(at_delete!=0||destroyed!=1){std::cerr<<"FAIL baseline ownership leak was not reproduced\n";return 1;}
    std::cout<<"EXPECTED baseline resource declaration/self-delete control: owner still live at task deletion; host-only unwinding cleans it afterwards\n";
}
'''


def ownership_prefix(body):
    start=body.index('std::unique_ptr<std::shared_ptr<')
    return body[start:body.index('holder.reset();',start)+len('holder.reset();')]


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline-source',type=Path)
    args=parser.parse_args()
    runtime=(ROOT/'components/consortium/runtime.cpp').read_text()
    active_body=function(runtime,'void active_loop_work(void *arg)')
    invite_body=function(runtime,'void invite_worker_work(void *arg)')
    for body in (active_body,invite_body):
        if 'vTaskDelete' in body:
            raise AssertionError('An owning worker still bypasses C++ return/destruction')
    actual='void active_loop_work(void *arg){'+ownership_prefix(active_body)+'}\n'
    client=invite_body[invite_body.index('AdmissionClient client('):]
    client=client[:client.index(';')+1]
    actual+='void invite_worker_work(void *arg){std::unique_ptr<InviteRequest> invite(static_cast<InviteRequest*>(arg));uint32_t pin=0;'+client+'}\n'
    actual+=function(runtime,'void active_loop(void *arg)')+function(runtime,'void invite_worker(void *arg)')
    baseline=args.baseline_source.read_text() if args.baseline_source else active_body
    old=function(baseline,'void active_loop(void *arg)') if args.baseline_source else active_body
    actual+='void baseline_link_worker(void *arg){'+ownership_prefix(old)+'vTaskDeleteWithCaps(nullptr);}\n'
    with tempfile.TemporaryDirectory(prefix='amity-task-lifetime-') as directory:
        cpp=Path(directory)/'test.cpp';binary=Path(directory)/'test'
        cpp.write_text(PREFIX+actual+SUITE)
        subprocess.run(shlex.split(os.environ.get('CXX','c++'))+['-std=c++17','-Wall','-Wextra','-Werror','-g','-fsanitize=address,undefined',str(cpp),'-o',str(binary)],check=True)
        return subprocess.run([str(binary)],check=False).returncode


if __name__=='__main__':raise SystemExit(main())
