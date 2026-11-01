// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#include "cec_hub_fixture.hpp"
#include "remote_input.hpp"
#include "siri_remote.hpp"
using namespace amity;
namespace m=amity::system;
static const auto group=*m::ConsortiumId::parse("11111111111111111111111111111111");
static const auto controller=*m::OctopusId::parse("22222222222222222222222222222222");
static const auto owner=*m::OctopusId::parse("33333333333333333333333333333333");
static const auto other_owner=*m::OctopusId::parse("77777777777777777777777777777777");
static const auto boot=*m::BootId::parse("44444444444444444444444444444444");
static const auto controller_boot=*m::BootId::parse("55555555555555555555555555555555");
static const auto link=*m::LinkId::parse("66666666666666666666666666666666");
static void reset_fixture(){
    fixture_tick=100000;fixture_time=1000000;fixture_present=true;fixture_async_autocomplete=true;
    fixture_driver_paused=false;fixture_wire_deadlines=false;fixture_completion_ready={};fixture_submit_admission={};
    fixture_wait_outcome={};fixture_before_start={};fixture_delay={};fixture_send={};fixture_transmit={};fixture_admission={};
    fixture_peer_address=[](int gpio)->uint8_t{return gpio==1||gpio==3||gpio==5?0:4;};
    fixture_sent.clear();fixture_completed.clear();fixture_incoming.clear();assert(fixture_pending.empty());
}
static bool is_frame(const SentFrame& sent,int gpio,std::initializer_list<uint8_t> data){
    return sent.gpio==gpio&&sent.frame.size==data.size()+1&&std::equal(data.begin(),data.end(),sent.frame.data+1);
}
static size_t count(int gpio,std::initializer_list<uint8_t> data){
    return std::count_if(fixture_sent.begin(),fixture_sent.end(),[&](const auto& sent){return is_frame(sent,gpio,data);});
}
static size_t first(int gpio,std::initializer_list<uint8_t> data,size_t begin=0){
    for(size_t n=begin;n<fixture_sent.size();++n)if(is_frame(fixture_sent[n],gpio,data))return n;
    return fixture_sent.size();
}
static m::QualifiedDeviceName named(m::OctopusId id,m::Side side,const char* name,uint8_t adapter=0){return {{group,id,adapter,side},name};}
static m::OwnedAuthorization authorization(m::OwnedIntentKind kind){
    m::OwnedAuthorization auth{controller,controller_boot,boot,link,1,0};
    auth.authority=kind==m::OwnedIntentKind::key_up?m::OwnedAuthority::cleanup:kind==m::OwnedIntentKind::key_down?m::OwnedAuthority::realtime:m::OwnedAuthority::control;
    return auth;
}
static void enqueue(HubService& hub,m::OwnedIntent intent,uint64_t sequence){
    intent.generation=1;intent.sequence=sequence;intent.config_revision=1;
    assert(hub.submit_owned(intent,authorization(intent.kind),0));run_one_command();
}
static m::OwnedIntent power(m::OctopusId id,m::Side side,uint8_t adapter=0){
    m::OwnedIntent intent;intent.kind=m::OwnedIntentKind::power;intent.target=named(id,side,side==m::Side::front?"TV":"Source",adapter);
    intent.on=true;intent.setup_generation=1;return intent;
}
static void start_owner(HubService& hub,m::OctopusId id,bool two_adapters=false,int front=1,int back=2){
    hub.set_system_identity(group,id);hub.set_boot_identity(boot);hub.set_coordinator_authority(false,1);
    if(two_adapters){
        const cec::BoardProfile profile{cec::BoardProfileId::Development4Tentacle,1,amity::max_tentacles,
            {{0,cec::Side::Front,1},{0,cec::Side::Back,2},{1,cec::Side::Front,5},{1,cec::Side::Back,6},
             {2,cec::Side::Front,-1},{2,cec::Side::Back,-1},{3,cec::Side::Front,-1},{3,cec::Side::Back,-1}},-1,-1};
        assert(hub.start(profile)==ESP_OK);
    }else assert(hub.start(front,back)==ESP_OK);
    assert(hub.bind_owned_controller(controller,1)==ESP_OK);fixture_finish_discovery(hub);
    fixture_tick=0;
}
static void drain(HubService& hub,FakeTask* task=nullptr){
    if(task)latest_task=task;
    for(unsigned n=0;n<24;++n){fixture_complete_all();run_idle(1);}
    fixture_async_autocomplete=true;fixture_complete_all();run_idle(2);
    (void)hub;
}
static void owner_lane_and_independent_keys(){
    reset_fixture();HubService hub;start_owner(hub,owner,true);
    m::OwnedIntent context;context.kind=m::OwnedIntentKind::context;context.target.port={group,owner,0,m::Side::front};
    context.selected=true;context.source=context.audio=named(owner,m::Side::back,"Source");enqueue(hub,context,1);
    fixture_sent.clear();fixture_async_autocomplete=false;
    enqueue(hub,power(owner,m::Side::front),2);
    assert(count(1,{0x04})==1&&fixture_pending.size()==1);
    enqueue(hub,power(owner,m::Side::back),3);
    enqueue(hub,power(owner,m::Side::front,1),4);
    m::OwnedIntent standby;standby.kind=m::OwnedIntentKind::standby_wire;standby.target.port={group,owner,1,m::Side::back};standby.setup_generation=1;
    enqueue(hub,standby,5);run_idle(2);
    assert(fixture_sent.size()==1); // One lane across front/back and adapter IDs, even with SETTLE=0.

    m::OwnedIntent right;right.kind=m::OwnedIntentKind::key_down;right.target=named(owner,m::Side::back,"Source");right.repeat=true;right.key=4;
    right.token=*m::HoldToken::parse("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    auto volume=right;volume.key=0x41;volume.token=*m::HoldToken::parse("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
    assert(count(1,{0x04})==1&&count(2,{0x44,0x6d})==0&&count(5,{0x04})==0);
    assert(fixture_complete_next(cec::TxResult::Success,1));run_idle(2);
    assert(count(2,{0x44,0x6d})==1&&count(5,{0x04})==0&&count(6,{0x36})==0);
    // A finite power press/release is one original step. Completing only its
    // press must not submit the next adapter's activity operation.
    assert(fixture_complete_next(cec::TxResult::Success,2));run_idle(2);assert(count(5,{0x04})==0);
    assert(fixture_complete_next(cec::TxResult::Success,2));run_idle(2);assert(count(5,{0x04})==1&&count(6,{0x36})==0);
    assert(fixture_complete_next(cec::TxResult::Success,5));run_idle(2);assert(count(6,{0x36})==1);
    drain(hub);
    assert(count(2,{0x44,4})==0&&count(2,{0x44,0x41})==0);
    fixture_sent.clear();fixture_async_autocomplete=false;
    right.kind=m::OwnedIntentKind::key_down;volume.kind=m::OwnedIntentKind::key_down;
    enqueue(hub,right,13);enqueue(hub,volume,14);
    assert(count(2,{0x44,4})==1&&count(2,{0x44,0x41})==1);
    right.kind=m::OwnedIntentKind::key_up;enqueue(hub,right,15);assert(count(2,{0x45})==0);
    assert(fixture_complete_next(cec::TxResult::Success,2));run_idle(1);
    assert(fixture_complete_next(cec::TxResult::Success,2));run_idle(1);
    assert(count(2,{0x44,4})==1&&count(2,{0x44,0x41})==2);
    volume.kind=m::OwnedIntentKind::key_up;enqueue(hub,volume,16);assert(count(2,{0x45})==1);drain(hub);
}
static void cold_lookup_precedes_later_recipe(int next){
    reset_fixture();HubService hub;assert(hub.start(1,2)==ESP_OK);
    assert(hub.configure({{"First","TV","Wanted","Source","",0},{"Later","TV","Source","Source","",0}}));
    run_one_command();fixture_finish_discovery(hub);
    // Permit a genuine recipient-cache miss after boot's per-side cooldown.
    fixture_tick=0;fixture_time+=130000000;fixture_sent.clear();fixture_async_autocomplete=false;
    assert(hub.request_activity(0)==ESP_OK);run_one_command();
    assert(count(1,{0x04})==1&&fixture_sent.size()==1);
    assert(hub.request_activity(next)==ESP_OK);run_one_command();run_idle(2);
    assert(fixture_sent.size()==1); // Later start/standby/repair cannot overtake the earlier recipient's TX.
    assert(fixture_complete_next(cec::TxResult::Success,1));run_idle(2);
    assert(std::any_of(fixture_pending.begin(),fixture_pending.end(),[](const auto& pending){return pending.gpio==2&&pending.frame.size==1;}));
    assert(count(2,{0x44,0x6d})==0&&count(1,{0x36})==0&&count(2,{0x86,0x10,0})==0);
    // Finish only this legitimately admitted lookup, then let each original
    // step complete. A missing source follows the original fallback path.
    fixture_complete_all();fixture_async_autocomplete=true;fixture_finish_discovery(hub,1);run_idle(16);
    assert(hub.snapshot().current_activity==next&&!hub.snapshot().discovering);
    const auto first_audio=first(2,{0x71});assert(first_audio<fixture_sent.size());
    if(next==1){const auto later_route=first(2,{0x86,0x10,0});assert(later_route<fixture_sent.size()&&first_audio<later_route);}
    else if(next==-1){const auto later_off=first(1,{0x36});assert(later_off<fixture_sent.size()&&first_audio<later_off);}
    else{const auto repeated_on=first(1,{0x04},1);assert(repeated_on<fixture_sent.size()&&first_audio<repeated_on);}
    drain(hub);
}
static void different_owners_progress_independently(){
    reset_fixture();HubService first_owner;start_owner(first_owner,owner);auto* first_task=latest_task;
    HubService second_owner;start_owner(second_owner,other_owner,false,3,4);auto* second_task=latest_task;
    fixture_sent.clear();fixture_async_autocomplete=false;
    latest_task=first_task;enqueue(first_owner,power(owner,m::Side::front),1);enqueue(first_owner,power(owner,m::Side::back),2);
    assert(count(1,{0x04})==1&&count(2,{0x44,0x6d})==0);
    latest_task=second_task;enqueue(second_owner,power(other_owner,m::Side::front),1);enqueue(second_owner,power(other_owner,m::Side::back),2);
    assert(count(3,{0x04})==1&&count(4,{0x44,0x6d})==0); // No global owner barrier.
    assert(fixture_complete_next(cec::TxResult::Success,3));run_idle(2);
    assert(count(4,{0x44,0x6d})==1&&count(2,{0x44,0x6d})==0);
    while(fixture_complete_next(cec::TxResult::Success,4)){}run_idle(2);
    assert(std::any_of(fixture_pending.begin(),fixture_pending.end(),[](const auto& pending){return pending.gpio==1;}));
    latest_task=first_task;assert(fixture_complete_next(cec::TxResult::Success,1));run_idle(2);assert(count(2,{0x44,0x6d})==1);
    drain(first_owner,first_task);drain(second_owner,second_task);latest_task=first_task;
}
static void revision_bind_waits_for_old_recipe_cleanup(){
    reset_fixture();HubService hub;start_owner(hub,owner);
    fixture_sent.clear();fixture_completed.clear();fixture_async_autocomplete=false;
    auto front=power(owner,m::Side::front),back=power(owner,m::Side::back);front.on=back.on=false;
    enqueue(hub,front,1);enqueue(hub,back,2);
    assert(count(1,{0x36})==1&&count(2,{0x36})==0&&hub.executing_configuration_revision()==1);
    unsigned completed=0;
    fixture_delay=[&]{
        // The synchronous bind uses the fixture's real request/reply IPC
        // pump. It must wait through both old-revision recipient completions,
        // without changing their execution fences or canceling either frame.
        if(completed==0){
            assert(hub.executing_configuration_revision()==1&&count(2,{0x36})==0);
            assert(fixture_complete_next(cec::TxResult::Success,1));++completed;
        }else if(completed==1&&count(2,{0x36})){
            assert(hub.executing_configuration_revision()==1);
            assert(fixture_complete_next(cec::TxResult::Success,2));++completed;
        }
    };
    assert(hub.bind_owned_controller(controller,2)==ESP_OK);
    fixture_delay={};assert(completed==2&&hub.executing_configuration_revision()==2);
    assert(count(1,{0x36})==1&&count(2,{0x36})==1&&fixture_completed.size()==2&&fixture_pending.empty());
    auto stale=power(owner,m::Side::front);stale.sequence=3;stale.generation=1;stale.config_revision=1;
    const auto before=fixture_sent.size();assert(!hub.submit_owned(stale,authorization(stale.kind)));
    assert(fixture_sent.size()==before); // The old authorization is no longer executable after the boundary.
    auto current=stale;current.sequence=1;current.config_revision=2;auto auth=authorization(current.kind);auth.config_revision=2;
    assert(hub.submit_owned(current,auth,0));run_one_command();assert(count(1,{0x04})==1);
    drain(hub);
}
static void newer_activity_fences_unfinished_reactive_standby(){
    reset_fixture();HubService hub;assert(hub.start(1,2)==ESP_OK);
    assert(hub.configure({{"Watch","TV","Source","Source","",0}}));run_one_command();fixture_finish_discovery(hub);
    fixture_tick=0;fixture_sent.clear();fixture_async_autocomplete=false;
    // The original no-activity reaction awaits Front Standby before Back.
    // A subsequent accepted activity invalidates only the unfinished RX
    // continuation; its already admitted first frame still finishes normally.
    fixture_incoming.push_back({2,cec::Frame{{0x4f,0x82,0x10,0x00},4}});
    for(unsigned pass=0;pass<8&&!count(1,{0x36});++pass)run_idle(1);
    assert(count(1,{0x36})==1&&count(2,{0x36})==0);
    assert(hub.request_activity(0)==ESP_OK);run_one_command();run_idle(2);
    assert(count(2,{0x36})==0&&count(1,{0x04})==0);
    assert(fixture_complete_next(cec::TxResult::Success,1));fixture_async_autocomplete=true;run_idle(16);
    assert(hub.snapshot().current_activity==0&&count(1,{0x04})==1&&count(2,{0x36})==0);
    assert(count(2,{0x44,0x6d})==1&&count(2,{0x86,0x10,0})==1);
    drain(hub);
}
static void active_source_reply_follows_route_completion(bool scoped){
    reset_fixture();HubService hub;
    if(scoped){hub.set_system_identity(group,controller);hub.set_boot_identity(boot);}
    assert(hub.start(1,2)==ESP_OK);
    if(scoped){
        assert(hub.configure_scoped({{"Watch","display","source","source","",0}},
            {{"display",named(controller,m::Side::front,"TV")},{"source",named(controller,m::Side::back,"Source")}},
            [](const auto&){assert(false&&"The local owner must use its actual worker");return false;},controller,1)==ESP_OK);
    }else assert(hub.configure({{"Watch","TV","Source","Source","",0}}));
    run_one_command();fixture_finish_discovery(hub);fixture_tick=0;
    fixture_sent.clear();fixture_async_autocomplete=false;bool echoed=false;
    fixture_before_start=[&](int gpio,const cec::Frame& frame){
        if(gpio==2&&frame.size==4&&frame.data[1]==0x86&&!echoed){
            echoed=true;
            // The source's ordinary wire reply is causally after this route
            // completed. Resume the selected activity/context before handling
            // that reply; the new source must not be treated as a standby thief.
            fixture_incoming.push_back({2,cec::Frame{{0x4f,0x82,0x10,0x00},4}});
        }
    };
    assert(hub.request_activity(0)==ESP_OK);run_one_command();
    for(unsigned pass=0;pass<48;++pass){
        fixture_complete_next(cec::TxResult::Success);run_idle(2);
        assert(count(1,{0x36})==0&&count(2,{0x36})==0);
        if(echoed&&fixture_pending.empty()&&hub.snapshot().current_activity==0)break;
    }
    assert(echoed&&hub.snapshot().current_activity==0&&count(2,{0x86,0x10,0})==1);
    assert(count(1,{0x36})==0&&count(2,{0x36})==0);fixture_before_start={};drain(hub);
}
struct UserInput {
    HubService& hub;
    std::vector<ble::RemoteCommand> emitted;
    ble::RemoteInputController policy;
    ble::SiriInputAdapter siri;
    uint16_t mask=0;
    explicit UserInput(HubService& target):hub(target),policy(deliver,this),siri(policy){refresh();}
    static bool deliver(ble::RemoteCommand command,void* context){
        auto& self=*static_cast<UserInput*>(context);self.emitted.push_back(command);
        if(command.type==ble::RemoteCommandType::Press)
            return self.hub.press_key_for_activity(static_cast<uint8_t>(command.value),command.stamp.generation,true);
        if(command.type==ble::RemoteCommandType::Release)return self.hub.release_key(static_cast<uint8_t>(command.value));
        assert(false&&"This fixture emits only ordinary physical keys");return false;
    }
    void refresh(){
        const auto now=uint64_t(fixture_time);
        // Match the shipping main's order: actual setup holds the existing
        // release gate before selection/generation notifications are applied.
        policy.activity_setup(hub.activity_setup_in_progress(),now);
        policy.activity_selected(hub.activity_selection(),now);
        policy.synchronize(hub.activity_generation(),hub.activity_target());policy.tick(now);
    }
    void button(ble::RemoteButton key,bool down,uint64_t timestamp=0){
        refresh();uint16_t bit=0;
        switch(key){case ble::RemoteButton::Home:bit=1;break;case ble::RemoteButton::VolumeUp:bit=2;break;
        case ble::RemoteButton::Up:bit=0x200;break;case ble::RemoteButton::Right:bit=0x400;break;case ble::RemoteButton::Left:bit=0x1000;break;
        default:assert(false);}
        mask=down?mask|bit:mask&~bit;const uint8_t data[]={static_cast<uint8_t>(mask),static_cast<uint8_t>(mask>>8)};
        siri.buttons(data,sizeof(data),{timestamp?timestamp:uint64_t(fixture_time),hub.activity_generation(),hub.activity_target()});run_idle(1);
    }
};
static void existing_release_gate_covers_actual_worker_setup(bool all_up){
    reset_fixture();HubService hub;assert(hub.start(1,2)==ESP_OK);
    assert(hub.configure({{"Watch","TV","Source","Source","",0}}));run_one_command();fixture_finish_discovery(hub);
    fixture_tick=0;assert(hub.request_activity(0)==ESP_OK);run_one_command();drain(hub);
    assert(hub.snapshot().current_activity==0&&!hub.activity_setup_in_progress());
    UserInput input(hub);assert(!input.policy.waiting_for_release());
    fixture_sent.clear();fixture_async_autocomplete=false;
    // Only the fallback case begins with a genuinely registered physical key.
    // Discarded setup downs must not create ignored/suppressed key entries.
    if(!all_up){input.button(ble::RemoteButton::Right,true);assert(count(2,{0x44,4})==1);}
    // Admit a raw input handoff immediately before reserving selection. The
    // worker must also reject this race at dequeue, before creating key state.
    assert(hub.press_key_for_activity(9,hub.activity_generation(),true));
    assert(hub.request_activity(0)==ESP_OK);run_one_command();assert(count(2,{0x44,9})==0);
    run_one_command();input.refresh();run_idle(1);
    const auto during_setup=uint64_t(fixture_time);const auto selection_generation=hub.activity_generation();
    const auto prior_right=count(2,{0x44,4}),prior_releases=count(2,{0x45});input.emitted.clear();
    assert(hub.activity_setup_in_progress()&&input.policy.waiting_for_release()&&count(1,{0x04})==1);
    input.button(ble::RemoteButton::Right,true);input.button(ble::RemoteButton::VolumeUp,true);
    fixture_time+=2000000;input.refresh();assert(input.policy.waiting_for_release());
    assert(hub.press_key_for_activity(9,hub.activity_generation(),true));
    input.button(ble::RemoteButton::Up,true);input.button(ble::RemoteButton::Up,false);
    input.button(ble::RemoteButton::VolumeUp,false);
    assert(input.emitted.empty()&&count(2,{0x44,4})==prior_right&&count(2,{0x44,0x41})==0&&count(2,{0x45})==prior_releases);
    assert(input.policy.waiting_for_release()&&hub.activity_setup_in_progress());
    fixture_complete_all();fixture_async_autocomplete=true;run_idle(16);input.refresh();
    assert(!hub.activity_setup_in_progress()&&hub.snapshot().current_activity==0);
    const auto before_unmatched_up=fixture_sent.size();assert(hub.release_key(9));run_one_command();
    assert(fixture_sent.size()==before_unmatched_up&&count(2,{0x44,9})==0); // Neither discarded handoff registered a key.
    if(all_up){
        // The physical adapter still reports Right down, but it was discarded
        // before key registration. Therefore no registered hold delays all-up.
        assert(!input.policy.waiting_for_release()&&hub.activity_generation()==selection_generation);
        input.button(ble::RemoteButton::Right,true);assert(input.emitted.empty()); // Unchanged physical state has no new edge/replay.
        const auto before=fixture_sent.size();input.button(ble::RemoteButton::Right,false);
        assert(input.emitted.empty()&&fixture_sent.size()==before&&!input.policy.waiting_for_release()); // Unmatched up is a NOP.
        // Same-target selection keeps generation: an ingress down timestamped
        // during setup must still be discarded after the gate has cleared.
        input.button(ble::RemoteButton::Home,true,during_setup);input.button(ble::RemoteButton::Home,false,during_setup+1);
        assert(input.emitted.empty()&&count(2,{0x44,9})==0);
    }else{
        assert(input.policy.waiting_for_release()); // The pre-setup registered Right is still physically held.
        fixture_time+=999999;input.refresh();assert(input.policy.waiting_for_release());
        fixture_time+=1;input.refresh();assert(!input.policy.waiting_for_release());
        input.button(ble::RemoteButton::Right,true);assert(input.emitted.empty());
        input.button(ble::RemoteButton::Left,true);assert(count(2,{0x44,3})>=1&&count(2,{0x44,4})==prior_right);
        input.button(ble::RemoteButton::Right,false);input.button(ble::RemoteButton::Left,false);
        fixture_complete_all();run_idle(2);
    }
    fixture_time+=2;fixture_sent.clear();input.emitted.clear();fixture_async_autocomplete=false;
    input.button(ble::RemoteButton::Right,true);input.button(ble::RemoteButton::VolumeUp,true);
    assert(count(2,{0x44,4})==1&&count(2,{0x44,0x41})==1);
    input.button(ble::RemoteButton::Right,false);assert(count(2,{0x45})==0);
    assert(fixture_complete_next(cec::TxResult::Success,2));run_idle(1);
    assert(fixture_complete_next(cec::TxResult::Success,2));run_idle(1);
    assert(count(2,{0x44,4})==1&&count(2,{0x44,0x41})==2);
    input.button(ble::RemoteButton::VolumeUp,false);assert(count(2,{0x45})==1);drain(hub);
}
static void long_power_repair_preserves_normal_input(){
    reset_fixture();HubService hub;assert(hub.start(1,2)==ESP_OK);
    assert(hub.configure({{"Watch","TV","Source","Source","",0}}));run_one_command();fixture_finish_discovery(hub);
    fixture_tick=0;assert(hub.request_activity(0)==ESP_OK);run_one_command();drain(hub);
    UserInput input(hub);assert(!input.policy.waiting_for_release());
    fixture_sent.clear();fixture_async_autocomplete=false;
    assert(hub.repair_activity()==ESP_OK);run_one_command();input.refresh();
    assert(count(1,{0x04})==1&&!hub.activity_setup_in_progress()&&!input.policy.waiting_for_release());
    // Long-Power repair has never armed wait_for_release. Its sequential
    // activity TX cannot turn the ordinary key path into a new gate.
    input.button(ble::RemoteButton::Right,true);input.button(ble::RemoteButton::VolumeUp,true);
    assert(count(2,{0x44,4})==1&&count(2,{0x44,0x41})==1);
    input.button(ble::RemoteButton::Right,false);assert(count(2,{0x45})==0);
    input.button(ble::RemoteButton::VolumeUp,false);assert(count(2,{0x45})==1);
    drain(hub);assert(!input.policy.waiting_for_release());
}

static void isolated_source_input(bool nested,bool same_adapter,bool front_source=false,bool lose_authority=false,bool reject_announcement=false,bool missing=false){
    reset_fixture();fixture_present=false;
    HubService hub;hub.set_system_identity(group,owner);hub.set_boot_identity(boot);hub.set_coordinator_authority(false,1);
    const cec::BoardProfile profile{cec::BoardProfileId::Development4Tentacle,1,amity::max_tentacles,
        {{0,cec::Side::Front,1},{0,cec::Side::Back,2},{1,cec::Side::Front,5},{1,cec::Side::Back,6},
         {2,cec::Side::Front,-1},{2,cec::Side::Back,-1},{3,cec::Side::Front,-1},{3,cec::Side::Back,-1}},-1,-1};
    std::array<uint16_t,amity::max_tentacles> physical;physical.fill(0x1000);physical[1]=nested?0x1200:0x1000;
    assert(hub.start(profile,physical)==ESP_OK);assert(hub.bind_owned_controller(controller,1)==ESP_OK);fixture_finish_discovery(hub);
    fixture_tick=0;
    const int source_gpio=front_source?5:6;const uint8_t address=front_source?8:4;
    // Boot had no peers, so the configured Front PA remains independent of
    // this source's later PA. The announcement must use the adapter's PA.
    fixture_incoming.push_back({source_gpio,{{static_cast<uint8_t>((address<<4)|15),0x84,0x12,0x10,4},5}});
    fixture_incoming.push_back({source_gpio,{{static_cast<uint8_t>((address<<4)|(front_source?4:0)),0x47,'S','o','u','r','c','e'},8}});
    run_idle(2);fixture_sent.clear();fixture_async_autocomplete=false;
    bool authority=true;hub.set_owned_authorizer([&](const auto&){return authority;});
    m::OwnedIntent input;input.kind=m::OwnedIntentKind::source_input;
    input.target=named(owner,front_source?m::Side::front:m::Side::back,"Source",1);
    input.display=named(owner,m::Side::front,"TV",same_adapter?1:0);input.setup_generation=1;
    if(missing)input.target.osd_name="Missing";
    if(reject_announcement)fixture_admission=[](int gpio,const auto& frame){return !(gpio==5&&frame.size>1&&frame.data[1]==0x82);};
    enqueue(hub,input,1);
    const bool announces=nested&&!same_adapter&&!front_source;
    if(missing){
        run_idle(2);assert(count(5,{0x82,0x12,0})==0&&count(source_gpio,{0x86,0x12,0x10})==0);
        drain(hub);return; // An unresolved source never selects the upstream input.
    }
    assert(count(source_gpio,{0x86,0x12,0x10})==size_t(!announces||reject_announcement));
    assert(count(5,{0x82,0x12,0})==size_t(announces));
    m::OwnedIntent next;next.kind=m::OwnedIntentKind::standby_wire;next.target.port={group,owner,0,m::Side::front};next.setup_generation=1;
    enqueue(hub,next,2);run_idle(2);assert(count(1,{0x36})==0);
    if(lose_authority)authority=false;
    if(announces){
        if(!reject_announcement){
            assert(count(source_gpio,{0x86,0x12,0x10})==0);
            assert(fixture_complete_next(cec::TxResult::Success,5));run_idle(2);
        }
        assert(count(source_gpio,{0x86,0x12,0x10})==size_t(!lose_authority));
        assert(count(1,{0x36})==0); // The final Back route remains completion paced.
    }
    if(!lose_authority){assert(fixture_complete_next(cec::TxResult::Success,source_gpio));run_idle(2);}
    assert(count(5,{0x82,0x12,0})==size_t(announces));
    assert(count(1,{0x82,0x10,0})==0); // Never announce on the display adapter.
    assert(count(1,{0x36})==size_t(!lose_authority));
    authority=true;drain(hub);
    assert(count(source_gpio,{0x86,0x12,0x10})==size_t(!lose_authority));
    assert(count(5,{0x82,0x12,0})==size_t(announces)); // Once only; no retry backlog.
}

int main(){
    owner_lane_and_independent_keys();
    for(const int next:{1,-1,0})cold_lookup_precedes_later_recipe(next);
    different_owners_progress_independently();
    revision_bind_waits_for_old_recipe_cleanup();
    newer_activity_fences_unfinished_reactive_standby();
    active_source_reply_follows_route_completion(false);active_source_reply_follows_route_completion(true);
    existing_release_gate_covers_actual_worker_setup(true);existing_release_gate_covers_actual_worker_setup(false);
    long_power_repair_preserves_normal_input();
    isolated_source_input(true,false);
    isolated_source_input(false,false); // Dual displays: leave the unselected TV alone.
    isolated_source_input(true,true); // Original same-adapter transcript.
    isolated_source_input(true,false,true); // An upstream source needs no paired announcement.
    isolated_source_input(true,false,false,true); // Recheck authority before the second step.
    isolated_source_input(true,false,false,false,true); // Dropped upstream work never blocks the original route.
    isolated_source_input(true,false,false,false,false,true); // Lookup must find the actual source first.
    std::puts("Actual worker activity sequencing: cold lookup/TX order at SETTLE=0, per-owner lanes, gated setup input with no hold replay, normal independent keys/owners, completed cleanup before bind, fenced RX and causal replies");
}
