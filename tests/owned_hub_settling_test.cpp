// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#include "cec_hub_fixture.hpp"
using namespace amity;
namespace m=amity::system;
static const auto group=*m::ConsortiumId::parse("11111111111111111111111111111111");
static const auto controller=*m::OctopusId::parse("22222222222222222222222222222222");
static const auto member=*m::OctopusId::parse("33333333333333333333333333333333");
static const auto boot=*m::BootId::parse("44444444444444444444444444444444");
static const auto other_boot=*m::BootId::parse("55555555555555555555555555555555");
static const auto link=*m::LinkId::parse("66666666666666666666666666666666");
static m::QualifiedDeviceName named(m::OctopusId owner,m::Side side,const char* name){return {{group,owner,0,side},name};}
static void settling_parity(unsigned mode){
    fixture_tick=100000;fixture_present=true;fixture_async_autocomplete=true;
    fixture_peer_address=[](int gpio)->uint8_t{return gpio==1||gpio==3?0:4;};
    HubService primary;std::unique_ptr<HubService> other;FakeTask* other_task=nullptr;
    if(mode){primary.set_system_identity(group,controller);primary.set_boot_identity(boot);}
    assert(primary.start(1,2)==ESP_OK);auto* primary_task=latest_task;
    if(mode==2){
        other=std::make_unique<HubService>();other->set_system_identity(group,member);other->set_boot_identity(other_boot);other->set_coordinator_authority(false,1);assert(other->start(3,4)==ESP_OK);other_task=latest_task;
        assert(other->bind_owned_controller(controller,1)==ESP_OK);fixture_finish_discovery(*other);
        other->set_owned_observer([&](const auto& report){assert(primary.update_owned_snapshot(report));});
        other->set_owned_input_observer([&](const auto& intent,const auto&,const auto& result){if(result.source_present)assert(primary.owned_input_result(member,intent.generation,intent.sequence,*result.source_present));});
    }
    latest_task=primary_task;
    if(mode)assert(primary.configure_scoped({{"First","display","source","source","",0},{"Second","display","source","source","",0}},
        {{"display",named(controller,m::Side::front,"TV")},{"source",named(mode==2?member:controller,m::Side::back,"Source")}},[&](const m::OwnedIntent& intent){
            auto* previous=latest_task;latest_task=other_task;m::OwnedAuthorization auth{controller,boot,other_boot,link,1,0};
            const auto result=other->submit_owned(intent,auth);latest_task=previous;
            if(result.source_present)assert(primary.owned_input_result(member,intent.generation,intent.sequence,*result.source_present));
            return result.code==m::AdmissionCode::accepted||result.code==m::AdmissionCode::pending||result.source_present.has_value();
        },controller,1)==ESP_OK);
    else assert(primary.configure({{"First","TV","Source","Source","",0},{"Second","TV","Source","Source","",0}}));
    run_one_command();fixture_finish_discovery(primary);
    fixture_tick=0;fixture_async_autocomplete=false;
    assert(primary.request_activity(0)==ESP_OK);assert(primary.activity_target()==0);
    assert(primary.request_activity(1)==ESP_ERR_TIMEOUT);assert(primary.request_activity(-1)==ESP_ERR_TIMEOUT);
    run_one_command();run_idle(1);
    auto advance=[&]{if(other){latest_task=other_task;run_idle(1);}latest_task=primary_task;run_idle(1);};
    // One owner's source frames can finish long before the display's last
    // admitted frame. Original response timeout starts after the global last
    // completion; an earlier owner's elapsed timer cannot shorten it.
    while(fixture_complete_next(cec::TxResult::Pending,mode==2?4:2)){}
    advance();
    fixture_time+=20000000;advance();assert(primary.request_activity(1)==ESP_ERR_TIMEOUT);
    assert(!fixture_pending.empty());fixture_complete_all();fixture_async_autocomplete=true;advance();
    fixture_time+=4000000;advance();assert(primary.request_activity(1)==ESP_ERR_TIMEOUT);
    fixture_time+=2000000;advance();assert(primary.request_activity(1)==ESP_ERR_TIMEOUT);
    fixture_time+=2000000;advance();assert(primary.request_activity(1)==ESP_OK);run_one_command();advance();
    latest_task=primary_task;fixture_peer_address={};
}
int main(){settling_parity(0);settling_parity(1);settling_parity(2);std::puts("Original, local qualified, and two-owner settling preserve admission and completion-paced quiet time");}
