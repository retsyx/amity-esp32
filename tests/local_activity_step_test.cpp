// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
// Real recipe steps on the owner that runs them: admission, IPC capacity,
// per-Amity sequencing across adapters, authority loss, resolver FIFO order
// and the source context commit. Each scenario runs in its own process.
#include "cec_hub_fixture.hpp"
#include <string>
using namespace amity;
namespace m=amity::system;
static const auto group=*m::ConsortiumId::parse("11111111111111111111111111111111");
static const auto controller=*m::OctopusId::parse("22222222222222222222222222222222");
static const auto member=*m::OctopusId::parse("33333333333333333333333333333333");
static const auto boot=*m::BootId::parse("44444444444444444444444444444444");
static const auto controller_boot=*m::BootId::parse("55555555555555555555555555555555");
static const auto link=*m::LinkId::parse("66666666666666666666666666666666");
static const cec::BoardProfile two_adapters{cec::BoardProfileId::Development4Tentacle,1,amity::max_tentacles,
    {{0,cec::Side::Front,1},{0,cec::Side::Back,2},{1,cec::Side::Front,5},{1,cec::Side::Back,6},
     {2,cec::Side::Front,-1},{2,cec::Side::Back,-1},{3,cec::Side::Front,-1},{3,cec::Side::Back,-1}},-1,-1};
static bool frame_is(const cec::Frame& frame,std::initializer_list<uint8_t> data){
    return frame.size==data.size()+1&&std::equal(data.begin(),data.end(),frame.data+1);
}
static size_t count(int gpio,std::initializer_list<uint8_t> data){
    return std::count_if(fixture_sent.begin(),fixture_sent.end(),[&](const auto& tx){return tx.gpio==gpio&&frame_is(tx.frame,data);});
}
static long index_of(int gpio,std::initializer_list<uint8_t> data){
    for(size_t i=0;i<fixture_sent.size();++i)if(fixture_sent[i].gpio==gpio&&frame_is(fixture_sent[i].frame,data))return static_cast<long>(i);
    return -1;
}
static bool pending(int gpio,std::initializer_list<uint8_t> data){
    return std::any_of(fixture_pending.begin(),fixture_pending.end(),[&](const auto& tx){return tx.gpio==gpio&&frame_is(tx.frame,data);});
}
static void finish_recipe(HubService& hub,int target){
    fixture_complete_all();fixture_async_autocomplete=true;
    for(unsigned pass=0;pass<64&&(hub.snapshot().current_activity!=target||hub.activity_setup_in_progress()||!fixture_pending.empty());++pass)run_idle(1);
}

// A settings commit closes ordinary owner IPC until the owner rejects it. The
// recipe's next steps are dispatched inside that window; they must still run
// and the activity must finish.
static void step_during_settings_commit(){
    fixture_present=true;fixture_tick=100000;fixture_async_autocomplete=true;
    const auto local_boot=fixture_local::id<m::BootId>(3);
    LocalHubService hub;assert(fixture_start(hub,1,2)==ESP_OK);
    assert(fixture_configure(hub,{{"Watch","TV","Source","Source","",0}}));run_one_command();fixture_finish_discovery(hub);
    fixture_tick=0;fixture_sent.clear();fixture_async_autocomplete=false;
    assert(hub.request_activity(0)==ESP_OK);run_one_command();run_idle(2);
    assert(fixture_pending.size()==1&&fixture_pending.front().gpio==1&&fixture_pending.front().frame.data[1]==0x04);
    // The TV power completes. The owner resumes the recipe while the commit
    // below holds the settings gate, before it dequeues and rejects it.
    assert(fixture_complete_next(cec::TxResult::Success,1));
    unsigned writes=0;
    assert(hub.commit_adapter_settings(local_boot,{{0,0x6000}},uint64_t(fixture_time)/1000+5000,[&]{++writes;return ESP_OK;})==ESP_ERR_INVALID_STATE);
    assert(!writes);
    finish_recipe(hub,0);
    const auto state=hub.snapshot();
    std::printf("current=%d setup=%d settling=%d source_on=%zu announce=%zu route=%zu\n",state.current_activity,hub.activity_setup_in_progress(),
        state.activity_settling,count(2,{0x44,0x6d}),count(1,{0x82,0x10,0}),count(2,{0x86,0x10,0}));
    assert(state.current_activity==0&&!hub.activity_setup_in_progress()&&!state.activity_settling);
    assert(count(2,{0x44,0x6d})==1&&count(1,{0x82,0x10,0})==1&&count(2,{0x86,0x10,0})==1);
    std::puts("A recipe step dispatched during a rejected settings commit still runs, and the activity finishes");
}

// Every ordinary IPC slot is taken when the selection dispatches its steps.
// The steps must still run once each, and the activity must finish.
static void steps_with_full_ipc(){
    fixture_present=true;fixture_tick=100000;fixture_async_autocomplete=true;
    LocalHubService hub;assert(fixture_start(hub,1,2)==ESP_OK);
    assert(fixture_configure(hub,{{"Watch","TV","Source","Source","",0}}));run_one_command();fixture_finish_discovery(hub);
    fixture_tick=0;fixture_sent.clear();
    assert(hub.request_activity(0)==ESP_OK);
    for(unsigned n=0;n<31;++n)assert(hub.release_key(0));
    assert(!hub.discover()); // The Select and 31 no-op releases hold all 32 slots.
    finish_recipe(hub,0);
    const auto state=hub.snapshot();
    std::printf("current=%d setup=%d settling=%d tv_on=%zu source_on=%zu announce=%zu route=%zu\n",state.current_activity,
        hub.activity_setup_in_progress(),state.activity_settling,count(1,{0x04}),count(2,{0x44,0x6d}),count(1,{0x82,0x10,0}),count(2,{0x86,0x10,0}));
    assert(state.current_activity==0&&!hub.activity_setup_in_progress()&&!state.activity_settling);
    assert(count(1,{0x04})==1&&count(2,{0x44,0x6d})==1&&count(1,{0x82,0x10,0})==1&&count(2,{0x86,0x10,0})==1);
    std::puts("Recipe steps run while ordinary owner IPC is full, and the activity finishes");
}

// A recipe step holds no ordinary IPC capacity: completing a step neither
// takes a command slot nor returns one it never took.
static void step_holds_no_ipc_slot(){
    fixture_present=true;fixture_tick=100000;fixture_async_autocomplete=true;
    LocalHubService hub;assert(fixture_start(hub,1,2)==ESP_OK);
    assert(fixture_configure(hub,{{"Watch","TV","Source","Source","",0}}));run_one_command();fixture_finish_discovery(hub);
    fixture_tick=0;fixture_sent.clear();fixture_async_autocomplete=false;
    assert(hub.request_activity(0)==ESP_OK);run_one_command();run_idle(2);
    assert(fixture_pending.size()==1&&fixture_pending.front().gpio==1);
    const unsigned queued=28;
    for(unsigned n=0;n<queued;++n)assert(hub.release_key(0));
    // Completes the TV step, starts the source power step and dequeues one
    // Release. The deferred Select and the other Releases hold their slots.
    assert(fixture_complete_next(cec::TxResult::Success,1));
    run_one_command();
    unsigned admitted=0;while(hub.discover())++admitted;
    std::printf("admitted=%u expected=%u source_power_started=%d\n",admitted,32u-1u-(queued-1u),pending(2,{0x44,0x6d}));
    assert(admitted==32u-1u-(queued-1u));
    // The next step started at once instead of queueing behind the Releases.
    assert(pending(2,{0x44,0x6d}));
    std::puts("Recipe steps take and return no IPC command slot");
}

// A main recipe with its display on adapter 1: the next step, on adapter 0,
// waits for adapter 1's TX, which the planner's own predicate cannot see.
static void display_tx_on_other_adapter_holds_next_step(){
    fixture_present=true;fixture_tick=100000;fixture_async_autocomplete=true;
    fixture_peer_address=[](int gpio)->uint8_t{return gpio%2?0:4;};
    LocalHubService hub;assert(fixture_start(hub,two_adapters)==ESP_OK);
    const auto group=fixture_local::group();const auto owner=fixture_local::owner();
    assert(hub.configure_scoped({{"Watch","display","source","source","",0,fixture_local::id<m::ActivityId>(100)}},
        {{"display",{{group,owner,1,m::Side::front},"TV"}},{"source",{{group,owner,0,m::Side::back},"Source"}}},{},owner,1)==ESP_OK);
    run_one_command();fixture_finish_discovery(hub);
    fixture_tick=0;fixture_sent.clear();fixture_async_autocomplete=false;
    assert(hub.request_activity(0)==ESP_OK);run_one_command();run_idle(4);
    assert(count(5,{0x04})==1&&count(2,{0x44,0x6d})==0&&fixture_pending.size()==1);
    run_idle(4);assert(count(2,{0x44,0x6d})==0);
    assert(fixture_complete_next(cec::TxResult::Success,5));run_idle(2);
    assert(count(2,{0x44,0x6d})==1);
    finish_recipe(hub,0);
    assert(hub.snapshot().current_activity==0&&!hub.activity_setup_in_progress());
    assert(count(5,{0x82,0x10,0})==1&&count(2,{0x86,0x10,0})==1);
    std::puts("A recipe step's TX on another adapter holds the next step");
}

// A main recipe with its source on adapter 1: each action there holds the
// next until all of its frames complete, although adapter 0 is idle.
static void source_frames_on_other_adapter_hold_next_step(){
    fixture_present=true;fixture_tick=100000;fixture_async_autocomplete=true;
    fixture_peer_address=[](int gpio)->uint8_t{return gpio==1||gpio==5?0:4;};
    HubService hub;hub.set_system_identity(group,controller);hub.set_boot_identity(boot);
    assert(fixture_start(hub,two_adapters)==ESP_OK);
    assert(hub.configure_scoped({{"Watch","display","source","","",0}},
        {{"display",{{group,controller,0,m::Side::front},"TV"}},{"source",{{group,controller,1,m::Side::back},"Source"}}},{},controller,1)==ESP_OK);
    run_one_command();fixture_finish_discovery(hub);
    fixture_tick=0;fixture_sent.clear();fixture_async_autocomplete=false;
    assert(hub.request_activity(0)==ESP_OK);run_one_command();run_idle(2);
    assert(count(1,{0x04})==1&&fixture_pending.size()==1);
    assert(fixture_complete_next(cec::TxResult::Success,1));run_idle(2);
    // Adapter 1 owns the source's finite power press and release.
    assert(count(6,{0x44,0x6d})==1&&count(6,{0x45})==1&&count(1,{0x82,0x10,0})==0);
    assert(fixture_complete_next(cec::TxResult::Success,6));run_idle(2);
    assert(count(1,{0x82,0x10,0})==0); // Its release is still transmitting.
    assert(fixture_complete_next(cec::TxResult::Success,6));run_idle(2);
    assert(count(1,{0x82,0x10,0})==1&&count(6,{0x86,0x10,0})==0); // Announce, then route.
    assert(fixture_complete_next(cec::TxResult::Success,1));run_idle(2);
    assert(count(6,{0x86,0x10,0})==1&&hub.snapshot().current_activity==-1);
    assert(fixture_complete_next(cec::TxResult::Success,6));
    for(unsigned pass=0;pass<16&&hub.activity_setup_in_progress();++pass){while(fixture_complete_next(cec::TxResult::Success)){}run_idle(2);}
    assert(hub.snapshot().current_activity==0&&count(6,{0x86,0x10,0})==1);
    std::puts("Recipe actions on another adapter hold the next action until all of their frames complete");
}

// A paced source-policy reaction frame in flight on adapter 1 holds the first
// recipe step on adapter 0 until that frame completes.
static void reaction_frame_holds_first_step(){
    fixture_present=true;fixture_tick=100000;fixture_async_autocomplete=true;
    fixture_peer_address=[](int gpio)->uint8_t{return gpio%2?0:4;};
    LocalHubService hub;assert(fixture_start(hub,two_adapters)==ESP_OK);
    const auto group=fixture_local::group();const auto owner=fixture_local::owner();
    assert(hub.configure_scoped({{"Watch","display","source","source","",0,fixture_local::id<m::ActivityId>(100)}},
        {{"display",{{group,owner,0,m::Side::front},"TV"}},{"source",{{group,owner,0,m::Side::back},"Source"}}},{},owner,1)==ESP_OK);
    run_one_command();fixture_finish_discovery(hub);
    fixture_tick=0;fixture_sent.clear();fixture_async_autocomplete=false;
    // No activity is current: adapter 1's source policy sends Front Standby,
    // an activity-paced reaction frame, and waits for its TX.
    fixture_incoming.push_back({6,cec::Frame{{0x4f,0x82,0x10,0x00},4}});
    for(unsigned pass=0;pass<8&&!count(5,{0x36});++pass)run_idle(1);
    assert(count(5,{0x36})==1&&fixture_pending.size()==1);
    assert(hub.request_activity(0)==ESP_OK);run_one_command();run_idle(4);
    std::printf("tv_on_while_reaction_pending=%zu\n",count(1,{0x04}));
    assert(count(1,{0x04})==0);
    assert(fixture_complete_next(cec::TxResult::Success,5));run_idle(2);
    assert(count(1,{0x04})==1);
    finish_recipe(hub,0);
    assert(hub.snapshot().current_activity==0);
    std::puts("A reaction frame on another adapter holds the first recipe step until its TX completes");
}

// A member's nested source route. Authority lost after the upstream
// announcement completes pauses the recipe and then cancels it: the Back
// route is never sent. Without the bind, the route is sent.
static void authority_loss_between_source_phases(bool rebind){
    fixture_present=false;fixture_tick=100000;fixture_async_autocomplete=true;fixture_peer_address={};
    const auto group=fixture_local::group();
    const auto member=fixture_local::id<m::OctopusId>(7);const auto controller=fixture_local::id<m::OctopusId>(8);
    const auto other=fixture_local::id<m::OctopusId>(9);
    const auto boot=fixture_local::id<m::BootId>(3);const auto controller_boot=fixture_local::id<m::BootId>(5);
    const auto link=fixture_local::id<m::LinkId>(6);
    HubService hub;hub.set_system_identity(group,member);hub.set_boot_identity(boot);hub.set_coordinator_authority(false);
    std::array<uint16_t,amity::max_tentacles> physical;physical.fill(0x1000);physical[1]=0x1200;
    assert(hub.start(two_adapters,physical)==ESP_OK);assert(hub.bind_owned_controller(controller,1)==ESP_OK);fixture_finish_discovery(hub);
    fixture_tick=0;
    fixture_incoming.push_back({6,{{0x4f,0x84,0x12,0x10,4},5}});
    fixture_incoming.push_back({6,{{0x40,0x47,'S','o','u','r','c','e'},8}});
    run_idle(2);fixture_sent.clear();fixture_async_autocomplete=false;
    m::OwnedIntent activity;activity.kind=m::OwnedIntentKind::activity;activity.target.port={group,member,0,m::Side::front};
    activity.generation=1;activity.config_revision=1;activity.selected=true;
    activity.source={{group,member,1,m::Side::back},"Source"};activity.display={{group,member,0,m::Side::front},"TV"};
    m::OwnedAuthorization auth{controller,controller_boot,boot,link,1,0};
    assert(hub.submit_owned(activity,auth));
    // Complete each activity TX until the upstream announcement is pending.
    for(unsigned pass=0;pass<32&&!count(5,{0x82,0x12,0});++pass){fixture_complete_all();run_idle(1);}
    assert(count(5,{0x82,0x12,0})==1&&count(6,{0x86,0x12,0x10})==0);
    // The announcement completes; a new controller may bind before the owner
    // resumes the source step's second phase.
    fixture_complete_all();
    if(rebind)assert(hub.bind_owned_controller(other,1)==ESP_OK);
    run_idle(8);fixture_complete_all();run_idle(8);
    std::printf("rebind=%d route=%zu settling=%d current=%d\n",rebind,count(6,{0x86,0x12,0x10}),hub.snapshot().activity_settling,hub.snapshot().current_activity);
    assert(count(6,{0x86,0x12,0x10})==(rebind?0u:1u)&&!hub.snapshot().activity_settling);
    fixture_tick=100000;fixture_async_autocomplete=true;
    std::puts(rebind?"Authority loss between nested source phases cancels the recipe without the Back route":
                     "Without authority loss the nested source route is sent");
}

// A real member recipe step's cache-miss lookup joins the resolver FIFO when
// it misses. A ready later waiter must not pass an earlier pending one, in
// either order, and the recipe must not repeat frames after the lookup.
static void lookup_fifo(bool step_first){
    const auto named=[](m::Side side,const char* name,uint8_t adapter){return m::QualifiedDeviceName{{group,member,adapter,side},name};};
    fixture_tick=100000;fixture_present=true;fixture_async_autocomplete=true;fixture_sent.clear();
    fixture_peer_address=[](int gpio)->uint8_t{return gpio%2?0:4;};
    HubService hub;hub.set_system_identity(group,member);hub.set_boot_identity(boot);hub.set_coordinator_authority(false);
    assert(fixture_start(hub,two_adapters)==ESP_OK&&hub.bind_owned_controller(controller,1)==ESP_OK);fixture_finish_discovery(hub);
    m::OwnedAuthorization auth{controller,controller_boot,boot,link,1,0};
    fixture_send=[](int gpio,const cec::Frame& frame){
        if(frame.size<2||frame.destination()!=fixture_peer(gpio))return true;
        const auto peer=frame.destination();cec::Frame reply;reply.data[0]=(peer<<4)|frame.source();
        switch(frame.data[1]){
        case 0x83:reply.size=5;reply.data[0]=(peer<<4)|15;reply.data[1]=0x84;reply.data[2]=peer?0x10:0;reply.data[3]=0;reply.data[4]=peer?4:0;break;
        case 0x46:{reply.size=2;reply.data[1]=0x47;const char* text=gpio==2?"WantedBack":gpio==5?"WantedFront":peer?"Source":"TV";while(*text)reply.data[reply.size++]=*text++;break;}
        case 0x8c:reply.size=5;reply.data[0]=(peer<<4)|15;reply.data[1]=0x87;break;
        default:return true;
        }
        fixture_incoming.push_back({gpio,reply});return true;
    };
    fixture_tick=0;fixture_time+=130000000;fixture_sent.clear();fixture_async_autocomplete=false;
    // A remote waiter: one power on a named recipient.
    m::OwnedIntent remote;remote.kind=m::OwnedIntentKind::power;remote.generation=1;remote.on=true;
    // A real member recipe: its first On step powers its display.
    m::OwnedIntent activity;activity.kind=m::OwnedIntentKind::activity;activity.target.port={group,member,0,m::Side::front};
    activity.generation=1;activity.config_revision=1;activity.selected=true;
    if(!step_first){
        remote.target=named(m::Side::back,"WantedBack",0);
        activity.display=named(m::Side::front,"WantedFront",1);
        assert(hub.submit_owned(remote,auth).code==m::AdmissionCode::pending);
        assert(hub.submit_owned(activity,auth).code==m::AdmissionCode::accepted);
        run_idle(2);
    }else{
        activity.display=named(m::Side::back,"WantedBack",0);
        remote.target=named(m::Side::front,"WantedFront",1);
        assert(hub.submit_owned(activity,auth).code==m::AdmissionCode::accepted);
        run_idle(2);
        assert(hub.submit_owned(remote,auth).code==m::AdmissionCode::pending);
    }
    // Both scans are pending. Finish only the adapter-1 front scan.
    bool front_ready=false;
    for(unsigned pass=0;pass<400&&!front_ready;++pass){
        fixture_complete_next(cec::TxResult::Pending,5);fixture_time+=100000;run_idle(1);
        for(const auto& report:hub.owned_snapshots())if(report.owner==member&&report.adapter==1)front_ready=report.discovery_generation>1&&!report.discovering;
    }
    const auto later_tx=count(5,{0x04});
    std::printf("step_first=%d front_ready=%d later_tx_before_earlier_scan=%zu earlier_tx=%zu\n",step_first,front_ready,later_tx,count(2,{0x44,0x6d}));
    assert(front_ready&&later_tx==0&&count(2,{0x44,0x6d})==0);
    // Finish everything; the earlier waiter's frame precedes the later one's.
    fixture_complete_all();fixture_async_autocomplete=true;fixture_finish_discovery(hub,2);
    for(unsigned pass=0;pass<64;++pass){fixture_complete_all();run_idle(1);}
    const auto earlier=index_of(2,{0x44,0x6d}),later=index_of(5,{0x04});
    std::printf("step_first=%d earlier=%ld later=%ld earlier_count=%zu later_count=%zu setup=%d\n",step_first,earlier,later,
        count(2,{0x44,0x6d}),count(5,{0x04}),hub.activity_setup_in_progress());
    assert(earlier>=0&&later>=0&&earlier<later);
    assert(count(2,{0x44,0x6d})==1&&count(5,{0x04})==1); // No duplicates after the lookup.
    assert(!hub.activity_setup_in_progress());
    fixture_send={};fixture_peer_address={};fixture_tick=100000;
    std::puts(step_first?"A recipe step's lookup keeps its FIFO place ahead of a later waiter":
                         "A recipe step's lookup waits behind an earlier waiter");
}

// A source-policy continuation created on the source adapter while the new
// route's 0x86 is in flight must not resume with the previous activity's
// context: the source step's completion commits the new context first.
static void route_completion_commits_before_policy(){
    fixture_present=false;fixture_tick=100000;fixture_async_autocomplete=true;
    LocalHubService hub;assert(fixture_start(hub,two_adapters)==ESP_OK);
    const auto group=fixture_local::group();const auto owner=fixture_local::owner();
    assert(hub.configure_scoped({{"One","display","one","one","",0,fixture_local::id<m::ActivityId>(100)},
                                 {"Two","display","two","two","",0,fixture_local::id<m::ActivityId>(101)}},
        {{"display",{{group,owner,0,m::Side::front},"TV"}},{"one",{{group,owner,1,m::Side::back},"One"}},{"two",{{group,owner,1,m::Side::back},"Two"}}},
        {},owner,1)==ESP_OK);
    run_one_command();fixture_finish_discovery(hub);
    // Announce the TV and two players on adapter 1's back wire.
    fixture_incoming.push_back({1,{{0x0f,0x84,0x00,0x00,0x00},5}});
    fixture_incoming.push_back({6,{{0x4f,0x84,0x11,0x00,4},5}});
    fixture_incoming.push_back({6,{{0x40,0x47,'O','n','e'},5}});
    fixture_incoming.push_back({6,{{0x8f,0x84,0x12,0x00,4},5}});
    fixture_incoming.push_back({6,{{0x80,0x47,'T','w','o'},5}});
    run_idle(4);
    assert(hub.request_activity(0)==ESP_OK);run_one_command();
    for(unsigned pass=0;pass<64&&(hub.snapshot().current_activity!=0||hub.activity_setup_in_progress());++pass)run_idle(1);
    assert(hub.snapshot().current_activity==0&&count(6,{0x86,0x11,0x00})>=1);
    run_idle(4);
    fixture_tick=0;fixture_sent.clear();fixture_async_autocomplete=false;
    assert(hub.request_activity(1)==ESP_OK);run_one_command();
    for(unsigned pass=0;pass<64&&!pending(6,{0x86,0x12,0x00});++pass){fixture_complete_next(cec::TxResult::Success);run_idle(1);}
    assert(pending(6,{0x86,0x12,0x00}));
    // While the new route is in flight, player Two announces itself. The old
    // context (One) would treat it as a thief and restore One afterwards.
    fixture_incoming.push_back({6,{{0x8f,0x82,0x12,0x00},4}});
    run_idle(1);
    fixture_complete_all();fixture_async_autocomplete=true;
    for(unsigned pass=0;pass<32;++pass){fixture_complete_all();run_idle(1);}
    const auto restored=count(6,{0x86,0x11,0x00});
    std::printf("current=%d standby=%zu new_route=%zu old_route=%zu\n",hub.snapshot().current_activity,count(6,{0x36}),count(6,{0x86,0x12,0x00}),restored);
    assert(hub.snapshot().current_activity==1&&count(6,{0x86,0x12,0x00})==1);
    assert(restored==0); // The previous activity's source is never routed again.
    std::puts("A policy continuation created during the new route does not restore the previous source");
}

int main(int argc,char** argv){
    const std::string scenario=argc>1?argv[1]:"";
    if(scenario=="settings-commit")step_during_settings_commit();
    else if(scenario=="ipc-full")steps_with_full_ipc();
    else if(scenario=="ipc-slots")step_holds_no_ipc_slot();
    else if(scenario=="display-other-adapter")display_tx_on_other_adapter_holds_next_step();
    else if(scenario=="source-other-adapter")source_frames_on_other_adapter_hold_next_step();
    else if(scenario=="reaction-frame")reaction_frame_holds_first_step();
    else if(scenario=="authority-loss")authority_loss_between_source_phases(true);
    else if(scenario=="authority-kept")authority_loss_between_source_phases(false);
    else if(scenario=="lookup-waiter-first")lookup_fifo(false);
    else if(scenario=="lookup-step-first")lookup_fifo(true);
    else if(scenario=="route-commit")route_completion_commits_before_policy();
    else{std::fprintf(stderr,"unknown scenario '%s'\n",scenario.c_str());return 2;}
}
