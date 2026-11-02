// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#include "cec_hub_fixture.hpp"
using namespace amity;
namespace m=amity::system;

static void reset_fixture(bool present=false){
    assert(fixture_pending.empty());fixture_tick=100000;fixture_time=1000000;
    fixture_present=present;fixture_async_autocomplete=true;fixture_peer_address={};
    fixture_send={};fixture_incoming.clear();fixture_sent.clear();fixture_completed.clear();
}
static void start(HubService& hub){
    assert(hub.start(1,2)==ESP_OK);fixture_finish_discovery(hub);
    fixture_tick=0;fixture_time+=300000000;fixture_sent.clear();
}
static void receive(cec::Frame frame,int gpio=2){
    fixture_incoming.push_back({gpio,frame});run_idle(12);
}
static std::optional<Device> peer(const HubService& hub,uint8_t address=8){
    for(const auto& d:hub.snapshot().devices)if(d.bus==BusSide::Back&&d.address==address)return d;
    return {};
}
static size_t first(uint8_t op,uint8_t destination=15){
    for(size_t i=0;i<fixture_sent.size();++i){const auto& f=fixture_sent[i];
        if(f.gpio==2&&f.frame.size>=2&&f.frame.data[1]==op&&f.frame.destination()==destination)return i;
    }
    return fixture_sent.size();
}
static void no_enumeration(const HubService& hub,uint32_t generation){
    assert(!hub.snapshot().discovering&&hub.snapshot().discovery_generation==generation);
    assert(std::none_of(fixture_sent.begin(),fixture_sent.end(),[](const auto& sent){return sent.frame.size==1;}));
}
static void only_targeted_identity_and_standby(){
    for(const auto& sent:fixture_sent){
        const auto& frame=sent.frame;assert(frame.size>=2);
        if(frame.data[1]==0x36)continue;
        assert(sent.gpio==2&&frame.destination()==8&&frame.size==2&&
            (frame.data[1]==0x83||frame.data[1]==0x46||frame.data[1]==0x8c));
    }
}
static void metadata(HubService& hub){
    const auto destination=hub.snapshot().back_address;assert(destination<15);
    receive({{static_cast<uint8_t>(0x80|destination),0x47,'P','l','a','y','e','r'},8});
    receive({{0x8f,0x84,0x20,0,4},5});
    receive({{0x8f,0x87,0x12,0x34,0x56},5});
    const auto d=peer(hub);assert(d&&d->name=="Player"&&d->physical_address==0x2000&&
        d->type==4&&d->vendor_id==0x123456&&d->resolved);
}
static void active_announcement_without_configuration(){
    reset_fixture();HubService hub;start(hub);assert(!peer(hub)&&hub.snapshot().activities.empty());
    const auto generation=hub.snapshot().discovery_generation;
    receive({{0x8f,0x82,0x20,0},4});
    const auto announced=peer(hub);assert(announced&&announced->physical_address==0x2000&&announced->name.empty()&&
        announced->type==0xff&&!announced->resolved);
    // Cache ingestion starts the announcing peer's metadata lookup before
    // applying the ordinary no-activity Standby policy. That policy may run
    // immediately; it must not discard the newly observed endpoint.
    assert(first(0x83,8)<first(0x36)&&first(0x46,8)<first(0x36)&&first(0x8c,8)<first(0x36));
    assert(first(0x36)<fixture_sent.size()&&hub.snapshot().current_activity==-1);
    only_targeted_identity_and_standby();no_enumeration(hub,generation);
    metadata(hub);no_enumeration(hub,generation);
    for(unsigned repeat=0;repeat<3;++repeat){
        fixture_time+=300000000;fixture_sent.clear();receive({{0x8f,0x82,0x20,0},4});
        const auto retained=peer(hub);assert(retained&&retained->name=="Player"&&retained->vendor_id==0x123456&&retained->physical_address==0x2000);
        assert(first(0x36)<fixture_sent.size());only_targeted_identity_and_standby();no_enumeration(hub,generation);
    }
    // Saving and then starting a normal activity can use the announcement's
    // actual observed identity without another discovery sweep.
    fixture_sent.clear();assert(hub.configure({{"Watch","","Player","","",0}}));run_one_command();run_idle(4);
    assert(peer(hub)&&hub.request_activity(0)==ESP_OK);run_one_command();run_idle(16);
    assert(hub.snapshot().current_activity==0&&first(0x44,8)<fixture_sent.size());
    const auto power=fixture_sent[first(0x44,8)].frame;assert(power.size==3&&power.data[2]==0x6d);
    const auto route=first(0x86);assert(route<fixture_sent.size()&&fixture_sent[route].frame.data[2]==0x20&&fixture_sent[route].frame.data[3]==0);
    assert(peer(hub)->name=="Player");no_enumeration(hub,generation);
}
static void metadata_can_introduce_a_device(){
    for(const auto opcode:{0x47,0x84,0x87}){
        reset_fixture();HubService hub;start(hub);const auto generation=hub.snapshot().discovery_generation;
        const auto destination=hub.snapshot().back_address;
        if(opcode==0x47)receive({{static_cast<uint8_t>(0x80|destination),0x47,'P','l','a','y','e','r'},8});
        else if(opcode==0x84)receive({{0x8f,0x84,0x20,0,4},5});
        else receive({{0x8f,0x87,0x12,0x34,0x56},5});
        const auto d=peer(hub);assert(d);
        if(opcode==0x47)assert(d->name=="Player"&&d->resolved);
        if(opcode==0x84)assert(d->physical_address==0x2000&&d->type==4);
        if(opcode==0x87)assert(d->vendor_id==0x123456);
        assert(first(0x83,8)<fixture_sent.size()&&first(0x46,8)<fixture_sent.size()&&first(0x8c,8)<fixture_sent.size());
        only_targeted_identity_and_standby();metadata(hub);no_enumeration(hub,generation);
    }
}
static void unselected_player_retains_its_observation(){
    reset_fixture(true);HubService hub;start(hub);
    assert(hub.configure({{"Watch","TV","Source","Source","",0}}));run_one_command();
    assert(hub.request_activity(0)==ESP_OK);run_one_command();run_idle(16);assert(hub.snapshot().current_activity==0);
    fixture_present=false;fixture_sent.clear();const auto generation=hub.snapshot().discovery_generation;
    receive({{0x8f,0x82,0x20,0},4});
    assert(peer(hub)&&peer(hub)->physical_address==0x2000&&hub.snapshot().current_activity==0);
    assert(first(0x36,8)<fixture_sent.size()); // Ordinary thief rejection is independent of caching.
    const auto route=first(0x86);assert(route<fixture_sent.size()&&fixture_sent[route].frame.data[2]==0x10&&fixture_sent[route].frame.data[3]==0);
    metadata(hub);assert(peer(hub,4)&&peer(hub,4)->name=="Source");no_enumeration(hub,generation);
}
static void foreign_activity_still_caches_local_player(){
    reset_fixture();HubService hub;
    const auto group=*m::ConsortiumId::parse("11111111111111111111111111111111");
    const auto controller=*m::OctopusId::parse("22222222222222222222222222222222");
    const auto owner=*m::OctopusId::parse("33333333333333333333333333333333");
    const auto boot=*m::BootId::parse("44444444444444444444444444444444");
    const auto controller_boot=*m::BootId::parse("55555555555555555555555555555555");
    const auto link=*m::LinkId::parse("66666666666666666666666666666666");
    hub.set_system_identity(group,owner);hub.set_boot_identity(boot);hub.set_coordinator_authority(false,1);
    assert(hub.start(1,2)==ESP_OK);
    assert(hub.bind_owned_controller(controller,1)==ESP_OK);
    fixture_finish_discovery(hub);fixture_tick=0;fixture_time+=300000000;fixture_sent.clear();
    m::OwnedIntent context;context.kind=m::OwnedIntentKind::context;context.generation=1;
    context.selected=true;context.target.port={group,owner,0,m::Side::front};
    context.source=m::QualifiedDeviceName{{group,controller,0,m::Side::back},"Elsewhere"};
    assert(hub.submit_owned(context,{controller,controller_boot,boot,link,1,0}));
    fixture_sent.clear();const auto generation=hub.snapshot().discovery_generation;
    receive({{0x8f,0x82,0x20,0},4});
    assert(first(0x36,8)<fixture_sent.size()&&peer(hub)&&peer(hub)->physical_address==0x2000);
    assert(first(0x86)==fixture_sent.size());metadata(hub);no_enumeration(hub,generation);
    const auto reports=hub.owned_snapshots();assert(reports.size()==1);
    assert(std::any_of(reports[0].devices.begin(),reports[0].devices.end(),[](const auto& d){
        return d.side==m::Side::back&&d.logical_address==8&&d.osd_name=="Player"&&d.physical_address==0x2000&&d.vendor_id==0x123456;
    }));
}
static void malformed_announcements_are_not_cached(){
    reset_fixture();HubService hub;start(hub);const auto generation=hub.snapshot().discovery_generation;
    const auto destination=hub.snapshot().back_address;
    const std::vector<cec::Frame> invalid={
        {{0xff,0x82,0x20,0},4},{{static_cast<uint8_t>(0xf0|destination),0x47,'X'},3},
        {{0xff,0x84,0x20,0,4},5},{{0xff,0x87,0x12,0x34,0x56},5},
        {{0x8f,0x82,0x20},3},{{static_cast<uint8_t>(0x80|destination),0x47},2},
        {{0x8f,0x84,0x20,0},4},{{0x8f,0x87,0x12,0x34},4},
        {{0x8f,0x82,0xff,0xff},4},{{0x8f,0x82,0x20,1},4},
        {{static_cast<uint8_t>(0x80|destination),0x82,0x20,0},4}
    };
    for(const auto& frame:invalid){receive(frame);assert(!peer(hub));}
    assert(fixture_sent.empty());no_enumeration(hub,generation);
}
int main(){
    active_announcement_without_configuration();metadata_can_introduce_a_device();
    unselected_player_retains_its_observation();foreign_activity_still_caches_local_player();malformed_announcements_are_not_cached();
    std::puts("Actual worker caches valid player announcements independently of activities/Standby, accepts targeted metadata, preserves observations for later starts, rejects malformed sources and avoids enumeration");
}
