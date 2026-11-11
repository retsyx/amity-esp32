// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#include "cec_hub_fixture.hpp"
using namespace amity;
namespace m=amity::system;
static const auto group=*m::ConsortiumId::parse("11111111111111111111111111111111");
static const auto controller=*m::OctopusId::parse("22222222222222222222222222222222");
static const auto owner=*m::OctopusId::parse("33333333333333333333333333333333");
static const auto boot=*m::BootId::parse("44444444444444444444444444444444");
static const auto controller_boot=*m::BootId::parse("55555555555555555555555555555555");
static const auto link=*m::LinkId::parse("66666666666666666666666666666666");

static void reset_fixture(){
    fixture_tick=0;fixture_time=1000000;fixture_present=true;
    fixture_sent.clear();fixture_incoming.clear();fixture_transmit={};fixture_send={};fixture_peer_address={};
}
static size_t polls(int gpio){
    return std::count_if(fixture_sent.begin(),fixture_sent.end(),[&](const auto& frame){return frame.gpio==gpio&&frame.frame.size==1;});
}
static void missing(HubService& hub,m::Side side,uint8_t adapter=0){
    m::OwnedIntent intent;intent.kind=m::OwnedIntentKind::power;intent.on=true;intent.generation=1;
    intent.target={{group,owner,adapter,side},"Missing"};
    m::OwnedAuthorization auth{controller,controller_boot,boot,link,1,0};
    assert(hub.submit_owned(intent,auth,0));run_one_command();
}
static void explicit_ui_discovery(HubService& hub){
    // The main's UI request reaches a headless owner through authenticated
    // delivery, rather than that owner's coordinator-only dispatch helper.
    m::OwnedIntent intent;intent.kind=m::OwnedIntentKind::discover;intent.generation=1;
    intent.target.port={group,owner,0,m::Side::front};
    const m::OwnedAuthorization auth{controller,controller_boot,boot,link,1,0};
    assert(hub.submit_owned(intent,auth,0));run_one_command();
}
static m::OwnedAdapterSnapshot observed(const HubService& hub){
    const auto reports=hub.owned_snapshots();
    const auto found=std::find_if(reports.begin(),reports.end(),[](const auto& report){return report.adapter==0;});
    assert(found!=reports.end());return *found;
}
static void setup(HubService& hub){
    hub.set_system_identity(group,owner);hub.set_boot_identity(boot);hub.set_coordinator_authority(false,1);
    assert(fixture_start(hub,1,2)==ESP_OK);assert(hub.bind_owned_controller(controller,1)==ESP_OK);
    fixture_finish_discovery(hub);
    assert(hub.snapshot().discovery_generation==1&&observed(hub).devices.size()==2);
}
static Device cached(const HubService& hub,BusSide side){
    const auto devices=observed(hub).devices;
    const auto wanted=side==BusSide::Front?m::Side::front:m::Side::back;
    const auto found=std::find_if(devices.begin(),devices.end(),[&](const auto& d){return d.side==wanted;});
    assert(found!=devices.end());
    return {found->osd_name,side,found->logical_address,found->physical_address,found->device_type,found->vendor_id,found->resolved};
}
static void same_device(const Device& a,const Device& b){
    assert(a.name==b.name&&a.bus==b.bus&&a.address==b.address&&a.physical_address==b.physical_address&&
           a.type==b.type&&a.vendor_id==b.vendor_id&&a.resolved==b.resolved);
}
static void side_local_cooldown(){
    reset_fixture();LocalHubService hub;setup(hub);
    const auto front=cached(hub,BusSide::Front),back=cached(hub,BusSide::Back);
    assert(polls(1)&&polls(2)); // The actual boot enumerates both sides.
    fixture_sent.clear();
    fixture_time=120999999;missing(hub,m::Side::back);run_idle(3);
    assert(hub.snapshot().discovery_generation==1&&fixture_sent.empty());
    fixture_time=121000000;missing(hub,m::Side::back);
    fixture_finish_discovery(hub,1);run_idle(1);
    assert(polls(2)&&!polls(1));same_device(front,cached(hub,BusSide::Front));

    // A back lookup cannot charge the independent front-side cooldown.
    fixture_sent.clear();const auto back_scan=hub.snapshot().discovery_generation;
    missing(hub,m::Side::front);fixture_finish_discovery(hub,back_scan);run_idle(1);
    assert(polls(1)&&!polls(2));same_device(back,cached(hub,BusSide::Back));

    fixture_sent.clear();const auto completed=hub.snapshot().discovery_generation;
    fixture_time=181000000;missing(hub,m::Side::back);run_idle(3);
    assert(hub.snapshot().discovery_generation==completed&&fixture_sent.empty());
    // Expiry alone cannot queue/defer another scan or replay the failed work.
    fixture_time=400000000;run_idle(30);
    assert(hub.snapshot().discovery_generation==completed&&fixture_sent.empty());
    missing(hub,m::Side::back);fixture_finish_discovery(hub,completed);
    assert(polls(2)&&!polls(1));

    // Explicit UI discovery bypasses both cooldowns, and its fresh scan
    // prevents another lookup enumeration immediately afterwards.
    fixture_sent.clear();const auto lookup=hub.snapshot().discovery_generation;
    explicit_ui_discovery(hub);fixture_finish_discovery(hub,lookup);
    assert(polls(1)&&polls(2));
    fixture_sent.clear();const auto manual=hub.snapshot().discovery_generation;
    missing(hub,m::Side::front);missing(hub,m::Side::back);run_idle(3);
    assert(hub.snapshot().discovery_generation==manual&&fixture_sent.empty());
    explicit_ui_discovery(hub);fixture_finish_discovery(hub,manual);
    assert(polls(1)&&polls(2));
}
static void no_configuration_or_rebind_scan(){
    reset_fixture();LocalHubService hub;hub.set_system_identity(group,controller);hub.set_boot_identity(boot);assert(fixture_start(hub,1,2)==ESP_OK);
    const std::vector<m::ScopedSelector> selectors{{"display",{{group,controller,0,m::Side::front},"TV"}},
                                                {"source",{{group,controller,0,m::Side::back},"Source"}}};
    assert(hub.configure_scoped({{"Watch","display","source","source","",0}},selectors,{},controller,1)==ESP_OK);
    run_one_command();fixture_finish_discovery(hub);
    const auto front=cached(hub,BusSide::Front),back=cached(hub,BusSide::Back);
    const auto generation=hub.snapshot().discovery_generation;
    fixture_time+=300000000;fixture_sent.clear();
    for(uint64_t revision=2;revision<5;++revision){
        assert(hub.configure_scoped({{"Edited","display","source","source","",0}},selectors,{},controller,revision)==ESP_OK);
        run_one_command();run_idle(3);
        assert(hub.snapshot().discovery_generation==generation&&!hub.snapshot().discovering);
        same_device(front,cached(hub,BusSide::Front));same_device(back,cached(hub,BusSide::Back));
    }
    assert(fixture_sent.empty());
    // Same-runtime controller/scope rebinding is not a boot or a UI scan.
    assert(hub.bind_owned_controller(controller,5)==ESP_OK);run_idle(10);
    assert(hub.snapshot().discovery_generation==generation&&fixture_sent.empty());
}
static void unrelated_scan_does_not_defer_cooldown_miss(){
    reset_fixture();LocalHubService hub;setup(hub);fixture_time=200000000;
    missing(hub,m::Side::back);fixture_finish_discovery(hub,1);
    const auto generation=hub.snapshot().discovery_generation;
    fixture_sent.clear();missing(hub,m::Side::front);
    m::OwnedIntent intent;intent.kind=m::OwnedIntentKind::source_input;intent.generation=1;
    intent.target={{group,owner,0,m::Side::back},"Missing"};
    const m::OwnedAuthorization auth{controller,controller_boot,boot,link,1,0};
    // The front scan begins while this synchronous request is consumed. A
    // back-side cooldown miss must resolve now, not join the unrelated scan.
    const auto result=hub.submit_owned(intent,auth);
    assert(result.code==m::AdmissionCode::unresolved&&result.source_present==std::optional<bool>{false});
    assert(hub.snapshot().discovering&&!polls(2));
    fixture_finish_discovery(hub,generation);
    assert(polls(1)&&!polls(2));
}
static void independent_adapters(){
    reset_fixture();
    fixture_peer_address=[](int gpio)->uint8_t{return gpio==1||gpio==5?0:4;};
    cec::BoardProfile profile{cec::BoardProfileId::Development4Tentacle,1,amity::max_tentacles,
        {{0,cec::Side::Front,1},{0,cec::Side::Back,2},{1,cec::Side::Front,5},{1,cec::Side::Back,6},
         {2,cec::Side::Front,-1},{2,cec::Side::Back,-1},{3,cec::Side::Front,-1},{3,cec::Side::Back,-1}},-1,-1};
    LocalHubService hub;hub.set_system_identity(group,owner);hub.set_boot_identity(boot);hub.set_coordinator_authority(false,1);
    assert(fixture_start(hub,profile)==ESP_OK);assert(hub.bind_owned_controller(controller,1)==ESP_OK);fixture_finish_discovery(hub);
    const auto generation=hub.snapshot().discovery_generation;assert(generation==2);
    fixture_time=200000000;fixture_sent.clear();missing(hub,m::Side::back,0);fixture_finish_discovery(hub,generation);
    assert(polls(2)&&!polls(1)&&!polls(5)&&!polls(6));
    const auto first=hub.snapshot().discovery_generation;fixture_sent.clear();
    missing(hub,m::Side::back,1);fixture_finish_discovery(hub,first);
    assert(polls(6)&&!polls(1)&&!polls(2)&&!polls(5));
    fixture_sent.clear();missing(hub,m::Side::back,0);missing(hub,m::Side::back,1);run_idle(3);
    assert(fixture_sent.empty());fixture_peer_address={};
}
static void claim_maintenance_is_not_enumeration(){
    reset_fixture();bool occupied=true;
    fixture_transmit=[&](int gpio,const cec::Frame& frame){
        if(gpio==2&&frame.source()==frame.destination())return occupied?cec::TxResult::Success:cec::TxResult::Nack;
        return fixture_present&&frame.destination()==fixture_peer(gpio)?cec::TxResult::Success:cec::TxResult::Nack;
    };
    LocalHubService hub;setup(hub);assert(observed(hub).back_address==15);
    const auto generation=hub.snapshot().discovery_generation;
    occupied=false;fixture_sent.clear();fixture_time+=6000000;
    for(unsigned n=0;n<20&&observed(hub).back_address==15;++n)run_idle(1);
    assert(observed(hub).back_address<15);run_idle(30);
    assert(hub.snapshot().discovery_generation==generation&&!hub.snapshot().discovering);
    assert(std::all_of(fixture_sent.begin(),fixture_sent.end(),[](const auto& sent){
        // Claim polls and the normal identity announcement are allowed. A
        // poll of a different address would enumerate a device unsolicited.
        return sent.frame.size!=1||sent.frame.source()==sent.frame.destination();
    }));
    fixture_transmit={};
}
static void resolve_renamed_source(int gpio,const cec::Frame& frame){
    if(gpio!=2||frame.size<2||frame.destination()!=4)return;
    cec::Frame reply;reply.data[0]=0x40;
    if(frame.data[1]==0x83){reply={{0x4f,0x84,0x10,0,4},5};}
    else if(frame.data[1]==0x46){reply={{0x40,0x47,'W','a','n','t','e','d'},8};}
    else if(frame.data[1]==0x8c){reply={{0x4f,0x87,0,0,0},5};}
    else return;
    fixture_incoming.push_back({gpio,reply});
}
static void every_caller_uses_one_resolver(){
    for(const auto opcode:{0x7a,0x8f,0x82,0x04}){
        reset_fixture();LocalHubService hub;assert(fixture_start(hub,1,2)==ESP_OK);
        assert(fixture_configure(hub,{{"Watch","TV","Wanted","Wanted","",0}}));run_one_command();fixture_finish_discovery(hub);
        assert(fixture_select(hub,0));run_one_command();run_idle(3);
        const auto generation=hub.snapshot().discovery_generation;
        fixture_time+=300000000;fixture_sent.clear();
        fixture_send=[](int gpio,const cec::Frame& frame){resolve_renamed_source(gpio,frame);return true;};
        cec::Frame incoming;
        if(opcode==0x7a)incoming={{0x40,0x7a,50},3};
        else if(opcode==0x8f)incoming={{0x40,0x8f},2};
        else if(opcode==0x82)incoming={{0x8f,0x82,0x20,0},4};
        else incoming={{0x80,0x04},2};
        fixture_incoming.push_back({2,incoming});run_idle(1);
        assert(hub.snapshot().discovering);
        // Resolving the cold miss must suspend this operation, while ordinary
        // RX and commands continue on the same owner task.
        assert(std::none_of(fixture_sent.begin(),fixture_sent.end(),[](const auto& tx){return tx.frame.size>1&&(tx.frame.data[1]==0x90||tx.frame.data[1]==0x36);}));
        fixture_incoming.push_back({1,{{0x04,0x46},2}});run_idle(1);
        assert(std::any_of(fixture_sent.begin(),fixture_sent.end(),[](const auto& tx){return tx.gpio==1&&tx.frame.size>1&&tx.frame.data[1]==0x47;}));
        fixture_finish_discovery(hub,generation);run_idle(2);
        assert(polls(2)&&!polls(1));
        // No second incoming event is sent: completion re-reads the populated
        // cache and finishes the original status/comparison operation once.
        if(opcode==0x7a){assert(hub.snapshot().audio_status_known&&hub.snapshot().volume==50);}
        else if(opcode==0x8f){
            assert(std::count_if(fixture_sent.begin(),fixture_sent.end(),[](const auto& tx){return tx.gpio==2&&tx.frame.size==3&&tx.frame.data[1]==0x90&&tx.frame.data[2]==0;})==1);
        }else{
            assert(std::count_if(fixture_sent.begin(),fixture_sent.end(),[](const auto& tx){return tx.gpio==2&&tx.frame.destination()==8&&tx.frame.size==2&&tx.frame.data[1]==0x36;})==1);
        }
        fixture_sent.clear();fixture_time+=300000000;run_idle(10);
        assert(fixture_sent.empty()&&hub.snapshot().discovery_generation==generation+1);
        fixture_send={};
    }
}
static void cold_miss_still_absent_and_stale_rx(){
    reset_fixture();LocalHubService hub;assert(fixture_start(hub,1,2)==ESP_OK);
    assert(fixture_configure(hub,{{"Watch","TV","Missing","Missing","",0}}));run_one_command();fixture_finish_discovery(hub);
    assert(fixture_select(hub,0));run_one_command();run_idle(3);
    const auto generation=hub.snapshot().discovery_generation;fixture_time+=300000000;fixture_sent.clear();
    fixture_incoming.push_back({2,{{0x40,0x8f},2}});run_idle(1);fixture_finish_discovery(hub,generation);run_idle(2);
    assert(std::count_if(fixture_sent.begin(),fixture_sent.end(),[](const auto& tx){return tx.frame.size==3&&tx.frame.data[1]==0x90&&tx.frame.data[2]==1;})==1);
    const auto scanned=hub.snapshot().discovery_generation;fixture_sent.clear();
    fixture_incoming.push_back({2,{{0x40,0x8f},2}});run_idle(2);
    assert(!polls(2)&&hub.snapshot().discovery_generation==scanned&&fixture_sent.size()==1);
    // A later accepted activity intent fences the original pending input;
    // completing its lookup must not answer it under a different activity.
    fixture_time+=300000000;fixture_sent.clear();fixture_incoming.push_back({2,{{0x40,0x8f},2}});run_idle(1);
    assert(hub.standby());run_one_command();fixture_finish_discovery(hub,scanned);run_idle(2);
    assert(std::none_of(fixture_sent.begin(),fixture_sent.end(),[](const auto& tx){return tx.frame.size>1&&tx.frame.data[1]==0x90;}));
    assert(hub.snapshot().current_activity==-1);
}
static void owned_context_fences_pending_rx(){
    reset_fixture();LocalHubService hub;setup(hub);
    const m::OwnedAuthorization auth{controller,controller_boot,boot,link,1,0};
    m::OwnedIntent context;context.kind=m::OwnedIntentKind::context;context.generation=1;context.selected=true;
    context.target.port={group,owner,0,m::Side::front};context.source=m::QualifiedDeviceName{{group,owner,0,m::Side::back},"Missing"};
    assert(hub.submit_owned(context,auth));
    // A new activity's earlier power request has arrived, while the previous
    // assigned-source context is still installed on this owner.
    m::OwnedIntent power;power.kind=m::OwnedIntentKind::power;power.on=true;power.generation=2;
    power.target={{group,owner,0,m::Side::back},"Source"};assert(hub.submit_owned(power,auth));
    fixture_time+=300000000;fixture_sent.clear();fixture_incoming.push_back({2,{{0x40,0x8f},2}});run_idle(1);
    const auto generation=hub.snapshot().discovery_generation;assert(hub.snapshot().discovering);
    context.generation=2;context.source->osd_name="Source";assert(hub.submit_owned(context,auth));
    fixture_finish_discovery(hub,generation);run_idle(2);
    assert(std::none_of(fixture_sent.begin(),fixture_sent.end(),[](const auto& tx){return tx.frame.size>1&&tx.frame.data[1]==0x90;}));
    fixture_sent.clear();fixture_incoming.push_back({2,{{0x40,0x8f},2}});run_idle(1);
    assert(fixture_sent.size()==1&&fixture_sent[0].frame.data[1]==0x90&&fixture_sent[0].frame.data[2]==0);
}
static void rejected_owner_work_does_not_resolve(){
    reset_fixture();LocalHubService hub;setup(hub);fixture_time+=300000000;fixture_sent.clear();
    m::OwnedIntent intent;intent.kind=m::OwnedIntentKind::power;intent.on=true;intent.generation=1;
    intent.target={{group,owner,0,m::Side::back},"Missing"};
    m::OwnedAuthorization auth{controller,controller_boot,boot,link,1,0};
    auto wrong=auth;wrong.owner_boot=controller_boot;assert(hub.submit_owned(intent,wrong).code==m::AdmissionCode::unauthorized);
    intent.target.port.octopus_id=controller;assert(hub.submit_owned(intent,auth).code==m::AdmissionCode::invalid);
    intent.target.port.octopus_id=owner;hub.set_owned_authorizer([](const auto&){return false;});
    assert(hub.submit_owned(intent,auth).code==m::AdmissionCode::unauthorized);run_idle(3);
    assert(fixture_sent.empty()&&hub.snapshot().discovery_generation==1);
}
static void received_continuation_uses_bounded_existing_ipc(){
    reset_fixture();LocalHubService hub;assert(fixture_start(hub,1,2)==ESP_OK);
    assert(fixture_configure(hub,{{"Watch","TV","Missing","Missing","",0}}));run_one_command();fixture_finish_discovery(hub);
    assert(fixture_select(hub,0));run_one_command();run_idle(3);
    const auto generation=hub.snapshot().discovery_generation;fixture_time+=300000000;
    fixture_async_autocomplete=false;
    for(unsigned n=0;n<40;++n)fixture_incoming.push_back({2,{{0x40,0x8f},2}});
    run_idle(2);assert(!hub.press_key(4)); // The existing 32 ordinary reservations bound all typed work.
    fixture_time+=120000001;run_idle(2);
    assert(hub.press_key(4)); // Expired RX releases reservations even while the scan is still pending.
    fixture_async_autocomplete=true;fixture_complete_all();fixture_finish_discovery(hub,generation);run_idle(3);
    assert(hub.release_key(4));run_one_command();
}
static void unregistered_received_source_is_bounded(){
    reset_fixture();LocalHubService hub;assert(fixture_start(hub,1,2)==ESP_OK);
    assert(fixture_configure(hub,{{"Watch","TV","Missing","Missing","",0}}));run_one_command();fixture_finish_discovery(hub);
    assert(fixture_select(hub,0));run_one_command();run_idle(3);
    const auto generation=hub.snapshot().discovery_generation;fixture_time+=300000000;
    fixture_incoming.push_back({2,{{0xf0,0x7a,50},3}});
    fixture_incoming.push_back({2,{{0xff,0x82,0x20,0},4}});run_idle(1);
    fixture_finish_discovery(hub,generation);run_idle(2);
    assert(!hub.snapshot().audio_status_known);
}
static void cold_switch_resumes_after_single_suppression(){
    for(unsigned scenario=0;scenario<3;++scenario){
        const bool found=scenario!=1,dropped=scenario==2;
        reset_fixture();LocalHubService hub;assert(fixture_start(hub,1,2)==ESP_OK);
        assert(fixture_configure(hub,{{"Switch","TV","","Source","Wanted",3}}));run_one_command();fixture_finish_discovery(hub);
        assert(fixture_select(hub,0));run_one_command();run_idle(3);
        const auto generation=hub.snapshot().discovery_generation;fixture_time+=300000000;fixture_sent.clear();
        if(found)fixture_send=[](int gpio,const cec::Frame& frame){resolve_renamed_source(gpio,frame);return true;};
        unsigned standby_attempts=0;
        fixture_admission=[&](int,const cec::Frame& frame){
            if(frame.size==2&&frame.destination()==8&&frame.data[1]==0x36){++standby_attempts;return !dropped;}
            return true;
        };
        fixture_incoming.push_back({2,{{0x8f,0x82,0x20,0},4}});run_idle(1);
        assert(standby_attempts==1&&hub.snapshot().discovering);
        fixture_finish_discovery(hub,generation);run_idle(2);
        assert(standby_attempts==1); // Resumption must not repeat an admitted or dropped36.
        const auto standby=std::find_if(fixture_sent.begin(),fixture_sent.end(),[](const auto& tx){return tx.gpio==2&&tx.frame.destination()==8&&tx.frame.size==2&&tx.frame.data[1]==0x36;});
        const auto poll=std::find_if(fixture_sent.begin(),fixture_sent.end(),[](const auto& tx){return tx.gpio==2&&tx.frame.size==1;});
        const auto input=std::find_if(fixture_sent.begin(),fixture_sent.end(),[](const auto& tx){return tx.gpio==2&&tx.frame.destination()==4&&tx.frame.size==4&&tx.frame.data[1]==0x44&&tx.frame.data[2]==0x69&&tx.frame.data[3]==3;});
        const auto release=std::find_if(input,fixture_sent.end(),[](const auto& tx){return tx.gpio==2&&tx.frame.destination()==4&&tx.frame.size==2&&tx.frame.data[1]==0x45;});
        const auto active=std::find_if(fixture_sent.begin(),fixture_sent.end(),[](const auto& tx){return tx.gpio==1&&tx.frame.size==4&&tx.frame.data[1]==0x82;});
        assert(poll!=fixture_sent.end());
        assert(standby!=fixture_sent.end()&&standby<poll);
        if(found)assert(input!=fixture_sent.end()&&poll<input&&input<release&&release<active&&active!=fixture_sent.end());
        else assert(input==fixture_sent.end()&&active==fixture_sent.end());
        fixture_admission={};fixture_send={};
    }
}
static void audio_query_finishes_after_opaque_lookup(){
    reset_fixture();LocalHubService hub;assert(fixture_start(hub,1,2)==ESP_OK);
    assert(fixture_configure(hub,{{"Watch","TV","Source","Source","",0}}));run_one_command();fixture_finish_discovery(hub);
    bool blocked=true;unsigned queries=0;
    fixture_admission=[&](int,const cec::Frame& frame){if(frame.size==2&&frame.data[1]==0x71){++queries;return !blocked;}return true;};
    assert(fixture_select(hub,0));run_one_command();
    for(unsigned n=0;n<100&&hub.snapshot().activity_settling;++n)run_idle(1);
    assert(hub.snapshot().current_activity==0&&!hub.snapshot().activity_settling&&queries==1);
    // Preserve the original failed-query continuation, then replace the
    // audio record passively without performing a scan or changing activity.
    fixture_time+=300000000;fixture_incoming.push_back({2,{{0x8f,0x84,0x10,0,4},5}});run_idle(1);assert(queries==2);
    blocked=false;fixture_time+=500001;fixture_sent.clear();const auto generation=hub.snapshot().discovery_generation;
    run_idle(1);assert(hub.snapshot().discovering&&queries==2);
    fixture_finish_discovery(hub,generation);run_idle(2);
    assert(queries==3);
    assert(std::count_if(fixture_sent.begin(),fixture_sent.end(),[](const auto& tx){return tx.gpio==2&&tx.frame.destination()==4&&tx.frame.size==2&&tx.frame.data[1]==0x71;})==1);
    fixture_admission={};
}

int main(){
    side_local_cooldown();no_configuration_or_rebind_scan();unrelated_scan_does_not_defer_cooldown_miss();independent_adapters();claim_maintenance_is_not_enumeration();every_caller_uses_one_resolver();cold_miss_still_absent_and_stale_rx();owned_context_fences_pending_rx();rejected_owner_work_does_not_resolve();received_continuation_uses_bounded_existing_ipc();unregistered_received_source_is_bounded();cold_switch_resumes_after_single_suppression();audio_query_finishes_after_opaque_lookup();
    std::puts("Boot/UI/cache-miss-only HDMI discovery, independent side/adapter cooldowns, retained opposite-side cache, no deferred retry or config/rebind/claim enumeration, uniform resolver and same-event cold-miss completion passed");
}
