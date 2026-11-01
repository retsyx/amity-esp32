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
static void context_recapture_preserves_pending_setup(){
    fixture_present=true;fixture_tick=100000;fixture_async_autocomplete=true;
    fixture_peer_address=[](int gpio)->uint8_t{return gpio%2?0:4;};
    HubService primary;primary.set_system_identity(group,controller);primary.set_boot_identity(boot);
    assert(primary.start(1,2)==ESP_OK);auto* primary_task=latest_task;
    struct Owner {
        m::OctopusId id;m::BootId boot;std::unique_ptr<HubService> hub;FakeTask* task;
        uint64_t revision=1,deferred_sequence=0,context_sequence=0;
        uint32_t deferred_generation=0,context_generation=0;unsigned contexts=0;
    };
    std::vector<Owner> owners;
    for(unsigned n=0;n<2;++n){
        Owner owner;owner.id=*m::OctopusId::parse(std::string(32,'7'+n));owner.boot=*m::BootId::parse(std::string(32,'9'-n));
        owner.hub=std::make_unique<HubService>();owner.hub->set_system_identity(group,owner.id);owner.hub->set_boot_identity(owner.boot);
        owner.hub->set_coordinator_authority(false,1);assert(owner.hub->start(3+2*n,4+2*n)==ESP_OK);owner.task=latest_task;
        assert(owner.hub->bind_owned_controller(controller,1)==ESP_OK);fixture_finish_discovery(*owner.hub);
        owner.hub->set_owned_observer([&](const auto& report){assert(primary.update_owned_snapshot(report));});
        owners.push_back(std::move(owner));
    }
    const auto pump=[&](unsigned passes){
        for(unsigned pass=0;pass<passes;++pass){
            for(auto& owner:owners){latest_task=owner.task;run_idle(1);}
            latest_task=primary_task;run_idle(1);
        }
    };
    const auto sender=[&](const m::OwnedIntent& intent){
        auto owner=std::find_if(owners.begin(),owners.end(),[&](const auto& value){return value.id==intent.target.port.octopus_id;});
        assert(owner!=owners.end());
        if(intent.kind==m::OwnedIntentKind::context){
            ++owner->contexts;owner->context_generation=intent.setup_generation;owner->context_sequence=intent.sequence;
            if(intent.config_revision>owner->revision){
                // The real runtime acknowledges future-revision context as a
                // notification to recapture after synchronization. Its old
                // payload is never queued or subsequently delivered.
                if(intent.setup_generation){owner->deferred_generation=intent.setup_generation;owner->deferred_sequence=intent.sequence;}
                return true;
            }
        }
        assert(intent.config_revision==owner->revision);
        auto* previous=latest_task;latest_task=owner->task;
        const auto result=owner->hub->submit_owned(intent,{controller,boot,owner->boot,link,owner->revision,0});
        latest_task=previous;
        return result.code==m::AdmissionCode::accepted||result.code==m::AdmissionCode::pending;
    };
    const std::vector<Activity> activities{{"Main","localtv","localsource","localsource","",0},
        {"Member 2","tv0","source0","source0","",0},{"Member 3","tv1","source1","source1","",0}};
    std::vector<m::ScopedSelector> selectors{{"localtv",named(controller,m::Side::front,"TV")},{"localsource",named(controller,m::Side::back,"Source")}};
    for(unsigned n=0;n<owners.size();++n){
        selectors.push_back({"tv"+std::to_string(n),named(owners[n].id,m::Side::front,"TV")});
        selectors.push_back({"source"+std::to_string(n),named(owners[n].id,m::Side::back,"Source")});
    }
    latest_task=primary_task;assert(primary.configure_scoped(activities,selectors,sender,controller,1)==ESP_OK);
    run_one_command();fixture_finish_discovery(primary);pump(10);
    assert(primary.configure_scoped(activities,selectors,sender,controller,2)==ESP_OK);run_one_command();pump(10);
    assert(primary.applied_configuration_revision()==2);
    fixture_sent.clear();fixture_tick=0;
    assert(primary.request_activity(0)==ESP_OK);run_one_command();pump(30);
    assert(primary.snapshot().current_activity==0&&primary.snapshot().activity_settling);
    for(const auto& owner:owners)assert(owner.deferred_generation&&owner.deferred_sequence);
    const auto original_frames=fixture_sent.size();
    assert(std::count_if(fixture_sent.begin(),fixture_sent.end(),[](const auto& tx){return tx.frame.size>1&&tx.frame.data[1]==0x04;})==1);
    assert(std::count_if(fixture_sent.begin(),fixture_sent.end(),[](const auto& tx){return tx.frame.size>2&&tx.frame.data[1]==0x44&&tx.frame.data[2]==0x6d;})==1);
    fixture_time+=20000000;pump(2);assert(primary.snapshot().activity_settling);
    for(auto& owner:owners){
        latest_task=owner.task;assert(owner.hub->bind_owned_controller(controller,2)==ESP_OK);owner.revision=2;latest_task=primary_task;
        const auto old_contexts=owner.contexts;bool stale=false,published=false;
        assert(primary.refresh_owned_context(owner.id,1,[&](auto result){stale=result==m::OwnedContextResult::stale;}));
        run_one_command();assert(stale&&owner.contexts==old_contexts&&primary.snapshot().activity_settling);
        assert(primary.refresh_owned_context(owner.id,2,[&](auto result){published=result==m::OwnedContextResult::published;}));
        run_one_command();pump(3);
        assert(published&&owner.context_generation==owner.deferred_generation&&owner.context_sequence>owner.deferred_sequence);
        const auto reports=primary.owned_snapshots();
        const auto report=std::find_if(reports.begin(),reports.end(),[&](const auto& value){return value.owner==owner.id;});
        assert(report!=reports.end()&&report->setup_generation==owner.deferred_generation&&report->setup_last_sequence==owner.context_sequence&&!report->setup_pending);
        assert(primary.snapshot().activity_settling&&fixture_sent.size()==original_frames);
    }
    fixture_time+=8000000;pump(2);
    assert(!primary.snapshot().activity_settling&&primary.snapshot().current_activity==0&&fixture_sent.size()==original_frames);
    for(auto& owner:owners){
        assert(primary.refresh_owned_context(owner.id,2));run_one_command();pump(2);
        assert(owner.context_generation==0&&!primary.snapshot().activity_settling&&fixture_sent.size()==original_frames);
    }
    latest_task=primary_task;fixture_peer_address={};fixture_tick=100000;
    std::puts("Future-revision context recapture retains only pending setup generation, advances fresh owner sequences and settles without replaying power steps");
}
int main(){settling_parity(0);settling_parity(1);settling_parity(2);context_recapture_preserves_pending_setup();std::puts("Original, local qualified, and two-owner settling preserve admission and completion-paced quiet time");}
