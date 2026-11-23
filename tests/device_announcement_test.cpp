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
    assert(fixture_start(hub,1,2)==ESP_OK);assert(hub.bind_owned_controller(fixture_local::owner(),1,fixture_local::id<m::BootId>(3))==ESP_OK);fixture_finish_discovery(hub);
    fixture_tick=0;fixture_time+=300000000;fixture_sent.clear();
}
static void receive(cec::Frame frame,int gpio=2){
    fixture_incoming.push_back({gpio,frame});run_idle(12);
}
static std::optional<Device> peer(const HubService& hub,uint8_t address=8){
    for(const auto& report:hub.owned_snapshots())for(const auto& d:report.devices)
        if(report.adapter==0&&d.side==m::Side::back&&d.logical_address==address)
            return Device{d.osd_name,BusSide::Back,d.logical_address,d.physical_address,d.device_type,d.vendor_id,d.resolved};
    return {};
}
static uint8_t back_address(const HubService& hub){
    const auto reports=hub.owned_snapshots();
    const auto found=std::find_if(reports.begin(),reports.end(),[](const auto& report){return report.adapter==0;});
    assert(found!=reports.end());return found->back_address;
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
    const auto destination=back_address(hub);assert(destination<15);
    receive({{static_cast<uint8_t>(0x80|destination),0x47,'P','l','a','y','e','r'},8});
    receive({{0x8f,0x84,0x20,0,4},5});
    receive({{0x8f,0x87,0x12,0x34,0x56},5});
    const auto d=peer(hub);assert(d&&d->name=="Player"&&d->physical_address==0x2000&&
        d->type==4&&d->vendor_id==0x123456&&d->resolved);
}
static void active_announcement_without_configuration(){
    reset_fixture();LocalHubService hub;start(hub);assert(!peer(hub)&&hub.snapshot().activities.empty());
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
    fixture_sent.clear();assert(fixture_configure(hub,{{"Watch","","Player","","",0}}));run_one_command();run_idle(4);
    assert(peer(hub)&&hub.request_activity(0)==ESP_OK);run_one_command();run_idle(16);
    assert(hub.snapshot().current_activity==0&&first(0x44,8)<fixture_sent.size());
    const auto power=fixture_sent[first(0x44,8)].frame;assert(power.size==3&&power.data[2]==0x6d);
    const auto route=first(0x86);assert(route<fixture_sent.size()&&fixture_sent[route].frame.data[2]==0x20&&fixture_sent[route].frame.data[3]==0);
    assert(peer(hub)->name=="Player");no_enumeration(hub,generation);
}
static void metadata_can_introduce_a_device(){
    for(const auto opcode:{0x47,0x84,0x87}){
        reset_fixture();LocalHubService hub;start(hub);const auto generation=hub.snapshot().discovery_generation;
        const auto destination=back_address(hub);
        if(opcode==0x47)receive({{static_cast<uint8_t>(0x80|destination),0x47,'P','l','a','y','e','r'},8});
        else if(opcode==0x84)receive({{0x8f,0x84,0x20,0,4},5});
        else receive({{0x8f,0x87,0x12,0x34,0x56},5});
        const auto d=peer(hub);assert(d);
        if(opcode==0x47)assert(d->name=="Player"&&d->resolved);
        if(opcode==0x84)assert(d->physical_address==0x2000&&d->type==4);
        if(opcode==0x87)assert(d->vendor_id==0x123456);
        // The peer is asked for its full identity, whatever its introducing report carried.
        assert(first(0x83,8)<fixture_sent.size()&&first(0x46,8)<fixture_sent.size()&&first(0x8c,8)<fixture_sent.size());
        only_targeted_identity_and_standby();metadata(hub);no_enumeration(hub,generation);
    }
}
static void unselected_player_retains_its_observation(){
    reset_fixture(true);LocalHubService hub;start(hub);
    assert(fixture_configure(hub,{{"Watch","TV","Source","Source","",0}}));run_one_command();
    assert(hub.request_activity(0)==ESP_OK);run_one_command();run_idle(16);assert(hub.snapshot().current_activity==0);
    fixture_present=false;fixture_sent.clear();const auto generation=hub.snapshot().discovery_generation;
    receive({{0x8f,0x82,0x20,0},4});
    assert(peer(hub)&&peer(hub)->physical_address==0x2000&&hub.snapshot().current_activity==0);
    assert(first(0x36,8)<fixture_sent.size()); // Ordinary thief rejection is independent of caching.
    const auto route=first(0x86);assert(route<fixture_sent.size()&&fixture_sent[route].frame.data[2]==0x10&&fixture_sent[route].frame.data[3]==0);
    metadata(hub);assert(peer(hub,4)&&peer(hub,4)->name=="Source");no_enumeration(hub,generation);
}
static void foreign_activity_still_caches_local_player(){
    reset_fixture();LocalHubService hub;
    const auto group=*m::ConsortiumId::parse("11111111111111111111111111111111");
    const auto controller=*m::OctopusId::parse("22222222222222222222222222222222");
    const auto owner=*m::OctopusId::parse("33333333333333333333333333333333");
    const auto boot=*m::BootId::parse("44444444444444444444444444444444");
    const auto controller_boot=*m::BootId::parse("55555555555555555555555555555555");
    const auto link=*m::LinkId::parse("66666666666666666666666666666666");
    hub.set_system_identity(group,owner);hub.set_boot_identity(boot);hub.set_coordinator_authority(false);
    assert(fixture_start(hub,1,2)==ESP_OK);
    assert(hub.bind_owned_controller(controller,1,controller_boot)==ESP_OK);
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
    reset_fixture();LocalHubService hub;start(hub);const auto generation=hub.snapshot().discovery_generation;
    const auto destination=back_address(hub);
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
static size_t sent(uint8_t op,uint8_t destination=8){
    return static_cast<size_t>(std::count_if(fixture_sent.begin(),fixture_sent.end(),[&](const auto& f){
        return f.gpio==2&&f.frame.size>=2&&f.frame.data[1]==op&&f.frame.destination()==destination;}));
}
static void known_peer_is_asked_again_for_a_missing_name(){
    reset_fixture();LocalHubService hub;start(hub);const auto generation=hub.snapshot().discovery_generation;
    receive({{0x8f,0x82,0x20,0},4});
    assert(peer(hub)&&peer(hub)->name.empty()&&sent(0x46)==1);
    // The reply to Amity's own 0x83 does not repeat the outstanding request.
    fixture_time+=100000;fixture_sent.clear();receive({{0x8f,0x84,0x20,0,4},5});assert(sent(0x46)==0);
    // The peer did not answer. Each later announcement asks again for only the
    // missing name; a power-on burst asks once.
    for(const auto& burst:std::vector<std::vector<cec::Frame>>{{{{0x8f,0x84,0x20,0,4},5}},{{{0x8f,0x82,0x20,0},4}},
            {{{0x8f,0x84,0x20,0,4},5},{{0x8f,0x87,0x12,0x34,0x56},5},{{0x8f,0x82,0x20,0},4}}}){
        fixture_time+=300000000;fixture_sent.clear();
        for(const auto& frame:burst)receive(frame);
        assert(sent(0x46)==1&&sent(0x83)==0&&sent(0x8c)==0&&peer(hub)->name.empty());
        only_targeted_identity_and_standby();no_enumeration(hub,generation);
    }
    metadata(hub);
    fixture_time+=300000000;fixture_sent.clear();receive({{0x8f,0x84,0x20,0,4},5});
    assert(sent(0x46)==0&&peer(hub)->name=="Player");no_enumeration(hub,generation);
}
static void refused_name_request_is_asked_again(){
    reset_fixture();LocalHubService hub;start(hub);const auto generation=hub.snapshot().discovery_generation;
    // A full CEC FIFO refuses the first two name requests. A refused request is
    // not outstanding, so the next announcement asks again even within 1 s.
    unsigned asked=0;
    fixture_admission=[&](int gpio,const cec::Frame& f){
        return !(gpio==2&&f.size==2&&f.data[1]==0x46&&f.destination()==8&&++asked<=2);};
    receive({{0x8f,0x82,0x20,0},4});
    assert(peer(hub)&&peer(hub)->name.empty()&&sent(0x46)==1&&asked==1);
    // The reply to Amity's own 0x83 arrives well inside 1 s.
    fixture_time+=100000;fixture_sent.clear();receive({{0x8f,0x84,0x20,0,4},5});
    assert(sent(0x46)==1&&asked==2);
    fixture_time+=100000;fixture_sent.clear();receive({{0x8f,0x82,0x20,0},4});
    assert(sent(0x46)==1&&asked==3);
    // The admitted request stands for CEC's 1 s reply time.
    fixture_time+=100000;fixture_sent.clear();receive({{0x8f,0x84,0x20,0,4},5});
    assert(sent(0x46)==0&&asked==3&&peer(hub)->name.empty());
    fixture_admission={};
    only_targeted_identity_and_standby();no_enumeration(hub,generation);
}
static void known_peer_is_asked_again_for_a_missing_type(){
    reset_fixture();LocalHubService hub;start(hub);const auto generation=hub.snapshot().discovery_generation;
    // The peer introduces itself by name and never answers Give Physical Address.
    receive({{static_cast<uint8_t>(0x80|back_address(hub)),0x47,'P','l','a','y','e','r'},8});
    assert(peer(hub)&&peer(hub)->name=="Player"&&peer(hub)->type==0xff&&sent(0x83)==1);
    // Active Source carries no device type, so a later one asks again for it;
    // a repeat inside CEC's 1 s reply time does not.
    fixture_time+=300000000;fixture_sent.clear();
    receive({{0x8f,0x82,0x20,0},4});receive({{0x8f,0x82,0x20,0},4});
    assert(sent(0x83)==1&&sent(0x46)==0&&sent(0x8c)==0&&peer(hub)->type==0xff);
    only_targeted_identity_and_standby();no_enumeration(hub,generation);
    // A request the full FIFO refused does not count; the next one asks.
    fixture_time+=300000000;fixture_sent.clear();
    fixture_admission=[](int gpio,const cec::Frame& f){return !(gpio==2&&f.size==2&&f.data[1]==0x83);};
    receive({{0x8f,0x82,0x20,0},4});fixture_admission={};receive({{0x8f,0x82,0x20,0},4});
    assert(sent(0x83)==2&&peer(hub)->type==0xff);
    // Report Physical Address carries the type itself, and a known type is not
    // asked for.
    fixture_time+=300000000;fixture_sent.clear();receive({{0x8f,0x84,0x20,0,4},5});
    assert(sent(0x83)==0&&peer(hub)->type==4);
    fixture_time+=300000000;fixture_sent.clear();receive({{0x8f,0x82,0x20,0},4});
    assert(sent(0x83)==0&&peer(hub)->name=="Player");no_enumeration(hub,generation);
}
static void scan_asks_a_peer_learned_from_a_late_reply_for_its_identity(){
    reset_fixture();LocalHubService hub;start(hub);const auto generation=hub.snapshot().discovery_generation;
    // An explicit scan starts while a Set OSD Name reply to a request sent
    // before it is still on its way. The reply returns the peer to the scan's
    // list, and the scan still asks that peer for its full identity itself.
    assert(hub.discover());run_one_command();assert(hub.snapshot().discovering);
    fixture_sent.clear();
    receive({{static_cast<uint8_t>(0x80|back_address(hub)),0x47,'P','l','a','y','e','r'},8});
    assert(first(0x83,8)<fixture_sent.size()&&first(0x46,8)<fixture_sent.size()&&first(0x8c,8)<fixture_sent.size());
    fixture_finish_discovery(hub,generation);
    assert(peer(hub)&&peer(hub)->name=="Player");
}
static size_t pending(uint8_t op,uint8_t destination=8){
    return static_cast<size_t>(std::count_if(fixture_pending.begin(),fixture_pending.end(),[&](const auto& p){
        return p.gpio==2&&p.frame.size>=2&&p.frame.data[1]==op&&p.frame.destination()==destination;}));
}
static void identity_queries_are_not_activity_work(){
    reset_fixture();LocalHubService hub;start(hub);
    // Every TX stays on the wire, as on a busy bus.
    fixture_async_autocomplete=false;
    // A new peer reports its physical address and is asked for its identity.
    receive({{0x8f,0x84,0x20,0,4},5});
    assert(pending(0x83)==1&&pending(0x46)==1&&pending(0x8c)==1);
    assert(hub.snapshot().current_activity==-1&&!hub.snapshot().activity_settling);
    fixture_complete_all();run_idle(4);
    // More than 1 s later the still nameless peer reports again and is asked
    // again for its name.
    fixture_time+=1500000;receive({{0x8f,0x84,0x20,0,4},5});
    assert(pending(0x46)==1&&pending(0x83)==0&&pending(0x8c)==0);
    assert(hub.snapshot().current_activity==-1&&!hub.snapshot().activity_settling);
    fixture_complete_all();fixture_async_autocomplete=true;run_idle(4);
    assert(peer(hub)&&peer(hub)->name.empty()&&!hub.snapshot().activity_settling);
}
static void activity_settles_only_after_its_own_work(){
    reset_fixture(true);LocalHubService hub;start(hub);
    assert(fixture_configure(hub,{{"Watch","TV","Source","Source","",0}}));run_one_command();run_idle(4);
    fixture_async_autocomplete=false;
    assert(hub.request_activity(0)==ESP_OK);run_one_command();run_idle(2);
    assert(!fixture_pending.empty()&&hub.snapshot().current_activity==-1&&hub.snapshot().activity_settling);
    // A new peer reports its physical address while the activity's first
    // frame is still on the wire. Its identity queries queue behind it.
    receive({{0x8f,0x84,0x20,0,4},5});
    assert(pending(0x83)==1&&pending(0x46)==1&&pending(0x8c)==1);
    // The activity is settling until it has committed and finished,
    // whichever frames are on the wire meanwhile.
    for(unsigned pass=0;pass<200;++pass){
        const auto state=hub.snapshot();
        if(state.current_activity==0&&!state.activity_settling)break;
        assert(state.activity_settling);
        fixture_complete_next();run_idle(2);
    }
    assert(hub.snapshot().current_activity==0&&!hub.snapshot().activity_settling&&fixture_pending.empty());
    // Asking the still nameless peer again later is not activity work.
    fixture_time+=1500000;receive({{0x8f,0x84,0x20,0,4},5});
    assert(pending(0x46)==1&&hub.snapshot().current_activity==0&&!hub.snapshot().activity_settling);
    fixture_complete_all();fixture_async_autocomplete=true;run_idle(4);
}
int main(){
    scan_asks_a_peer_learned_from_a_late_reply_for_its_identity();
    active_announcement_without_configuration();metadata_can_introduce_a_device();
    unselected_player_retains_its_observation();foreign_activity_still_caches_local_player();malformed_announcements_are_not_cached();
    known_peer_is_asked_again_for_a_missing_name();refused_name_request_is_asked_again();known_peer_is_asked_again_for_a_missing_type();
    identity_queries_are_not_activity_work();activity_settles_only_after_its_own_work();
    std::puts("Actual worker caches valid player announcements independently of activities/Standby, accepts targeted metadata, preserves observations for later starts, rejects malformed sources, avoids enumeration and never counts identity queries as activity work");
}
