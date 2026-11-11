#!/usr/bin/env python3
"""Run the actual original-controller claim/CAS code against atomic store faults."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile
from consortium_gatt_callback_test import function
ROOT=Path(__file__).resolve().parents[1]
PREFIX=r'''
#include "controller_policy.hpp"
#include "amity/consortium/bootstrap.hpp"
#include "runtime_persistence.hpp"
#include <cassert>
#include <chrono>
#include <future>
#include <iostream>
#include <mutex>
using namespace amity::consortium;using namespace amity::consortium::platform;
using namespace std::chrono_literals;
using esp_err_t=int;constexpr int ESP_OK=0,ESP_ERR_INVALID_STATE=1,ESP_FAIL=2;
std::mutex lock,persistence_lock;Group group,durable;Identity identity;model::ConsortiumId singleton;bool candidate_passive=false;
uint64_t candidate_generation=0,candidate_deadline=0,released=0;unsigned closed_count=0,opened_count=0;
uint64_t now_ms(){return 100;}
int bootstrap_request(std::string_view,std::string&){return ESP_OK;}
template<class Callback>int gatt_server_open(uint64_t,model::OctopusId,Callback){++opened_count;return ESP_OK;}
void gatt_server_close(){++closed_count;}void defer_radio_release(uint64_t generation){released=generation;}
struct{bool initialized=true,credential_ready=true,coordinator=false,authority_ready=false,member=false,enrollment_open=false;uint64_t epoch=0,membership_revision=0;std::string phase="awaiting_setup",last_error;}status;
unsigned writes=0;unsigned fault=0;
std::function<void()> during_save;
int save_group(const Group&next){if(during_save)during_save();++writes;if(fault==1)return ESP_FAIL;durable=next;return fault==2?ESP_FAIL:ESP_OK;}
template<class T>T id(unsigned n){char text[33];snprintf(text,sizeof(text),"%032x",n);return *T::parse(text);}
void reset(){group={};durable={};identity={};identity.id=id<model::OctopusId>(1);singleton=id<model::ConsortiumId>(2);status={};candidate_passive=false;candidate_generation=candidate_deadline=released=0;closed_count=opened_count=0;writes=0;fault=0;during_save={};}
'''
SUITE=r'''
void claim_and_open_are_ordered(){
 reset();std::promise<void> entered,release,opening;auto released=release.get_future().share();
 during_save=[&]{entered.set_value();released.wait();};
 auto claim=std::async(std::launch::async,[]{return runtime_claim_original_controller();});
 assert(entered.get_future().wait_for(2s)==std::future_status::ready);
 auto open=std::async(std::launch::async,[&]{opening.set_value();return runtime_open_enrollment(7);});opening.get_future().wait();
 // An in-progress durable claim must not expose the old unclaimed role to a
 // competing Open, while unrelated runtime readers still get their mutex.
 auto read=std::async(std::launch::async,[]{std::lock_guard<std::mutex> guard(lock);return group.role;});
 assert(read.wait_for(2s)==std::future_status::ready&&read.get()==Group::unclaimed);
 assert(open.wait_for(50ms)==std::future_status::timeout);
 release.set_value();assert(claim.get()==ESP_OK&&open.get()==ESP_ERR_INVALID_STATE);
 assert(group.role==Group::coordinator&&durable.role==Group::coordinator&&!opened_count&&!candidate_generation);
}
int main(){
 reset();assert(!writes&&!status.authority_ready);
 assert(runtime_original_setup_eligible());
 assert(runtime_claim_original_controller()==ESP_OK&&writes==1&&group.role==Group::coordinator&&durable.authority==identity.id&&durable.epoch==1&&status.authority_ready&&status.coordinator&&!status.member);
 assert(runtime_claim_original_controller()==ESP_OK&&writes==1);
 for(auto role:{Group::member,Group::prepared,Group::retired,Group::coordinator}){reset();group.role=role;group.authority=id<model::OctopusId>(3);group.epoch=1;assert(runtime_claim_original_controller()==ESP_ERR_INVALID_STATE&&!writes&&!status.authority_ready);}
 reset();group.role=Group::prepared;group.join_id=id<model::JoinId>(5);candidate_passive=true;assert(runtime_claim_original_controller()==ESP_ERR_INVALID_STATE&&!writes); // Durable member preparation wins a late Matter completion.
 reset();candidate_passive=true;assert(runtime_claim_original_controller()==ESP_ERR_INVALID_STATE&&!writes);
 reset();group.join_id=id<model::JoinId>(5);assert(runtime_claim_original_controller()==ESP_ERR_INVALID_STATE&&!writes);
 for(bool generation_only:{false,true}){reset();if(generation_only)candidate_generation=7;else candidate_deadline=42;assert(!runtime_original_setup_eligible()&&runtime_claim_original_controller()==ESP_ERR_INVALID_STATE&&!writes);}
 reset();candidate_generation=7;candidate_deadline=42;status.phase="enrollment_open";status.enrollment_open=true;assert(!runtime_original_setup_eligible());runtime_close_enrollment();assert(closed_count==1&&released==7&&!status.enrollment_open&&status.phase=="awaiting_setup"&&runtime_original_setup_eligible()&&!writes);assert(runtime_claim_original_controller()==ESP_OK); // Cancelling one uncommitted choice permits physical Matter setup again.
 reset();group.role=Group::prepared;candidate_passive=true;candidate_generation=7;candidate_deadline=42;status.phase="prepared";runtime_close_enrollment();assert(status.phase=="prepared"&&!runtime_original_setup_eligible()&&runtime_claim_original_controller()==ESP_ERR_INVALID_STATE);
 for(unsigned cut:{1u,2u}){reset();fault=cut;assert(runtime_claim_original_controller()==ESP_FAIL&&!status.authority_ready&&group.role==Group::unclaimed);assert((durable.role==Group::coordinator)==(cut==2));assert(candidate_passive&&!runtime_original_setup_eligible()&&runtime_claim_original_controller()==ESP_ERR_INVALID_STATE);group=durable;candidate_passive=false;fault=0;assert(runtime_claim_original_controller()==ESP_OK);if(cut==1)assert(status.authority_ready);} // Only a reboot/reload can resolve uncertain durable outcome.
 for(unsigned unavailable=0;unavailable<2;++unavailable){reset();if(!unavailable)status.initialized=false;else status.credential_ready=false;assert(runtime_claim_original_controller()==ESP_ERR_INVALID_STATE&&!writes);}
 claim_and_open_are_ordered();
 std::cout<<"PASS actual original claim: no implicit singleton authority, durable epoch1 identity, idempotence, member/retired rejection, late Matter race, before/after-commit failure and concurrent Open ordered behind blocked flash\n";
}
'''

def main():
 source=(ROOT/'components/consortium/runtime.cpp').read_text()
 actual=''.join(function(source,name) for name in ('esp_err_t runtime_claim_original_controller()','bool runtime_original_setup_eligible()','esp_err_t runtime_open_enrollment(','void runtime_close_enrollment()'))
 with tempfile.TemporaryDirectory(prefix='amity-original-claim-') as directory:
  cpp=Path(directory)/'test.cpp';binary=Path(directory)/'test';cpp.write_text(PREFIX+actual+SUITE)
  subprocess.run(shlex.split(os.environ.get('CXX','c++'))+['-std=c++17','-pthread','-Wall','-Wextra','-Werror','-g','-fsanitize=address,undefined']+['-I'+str(ROOT/path) for path in ('components/consortium','components/consortium/include','components/amity_system/include')]+[str(cpp),str(ROOT/'components/amity_system/model.cpp'),'-o',str(binary)],check=True)
  subprocess.run([str(binary)],check=True)
if __name__=='__main__':main()
