// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#include "cec_hub_fixture.hpp"

using namespace amity;
namespace m=amity::system;
template<class T> T identity(char digit){return *T::parse(std::string(32,digit));}
static const auto group=identity<m::ConsortiumId>('1');
static const auto controller=identity<m::OctopusId>('2');
static const auto member=identity<m::OctopusId>('3');
static const auto controller_boot=identity<m::BootId>('4');
static const auto member_boot=identity<m::BootId>('5');
static const auto link=identity<m::LinkId>('6');
static m::QualifiedDeviceName name(m::OctopusId owner,m::Side side,const char* osd,uint8_t adapter=0){return {{group,owner,adapter,side},osd};}
static cec::Frame active(uint8_t address){return {{static_cast<uint8_t>((address<<4)|15),0x82,static_cast<uint8_t>(address==4?0x10:address==8?0x20:0x30),0},4};}
struct Wire:Transport {
    struct Item{BusSide side;uint8_t destination;std::vector<uint8_t> data;};
    std::vector<Item> sent;
    bool send(BusSide side,uint8_t destination,const uint8_t* data,size_t size)override{sent.push_back({side,destination,{data,data+size}});return true;}
    void request_scan()override{}
};
static void device(Hub& hub,BusSide side,uint8_t address,const char* osd){
    hub.discovered(side,address);
    cec::Frame pa{{static_cast<uint8_t>((address<<4)|15),0x84,static_cast<uint8_t>(address==4?0x10:address==8?0x20:0x30),0,4},5};hub.receive(side,pa);
    cec::Frame label;label.data[0]=static_cast<uint8_t>((address<<4)|(side==BusSide::Back?0:4));label.data[1]=0x47;label.size=2;
    while(*osd)label.data[label.size++]=static_cast<uint8_t>(*osd++);
    hub.receive(side,label);
}
static void one_standby(const Wire& wire,uint8_t destination){
    assert(wire.sent.size()==1&&wire.sent[0].side==BusSide::Back&&wire.sent[0].destination==destination&&wire.sent[0].data==std::vector<uint8_t>{0x36});
}
static void original_and_context_source_comparison(){
    Wire wire;Hub hub(wire);hub.set_addresses(4,0);hub.qualified_names();
    device(hub,BusSide::Back,4,"Source");device(hub,BusSide::Back,8,"Other");
    device(hub,BusSide::Front,8,"Front Source");
    hub.configure({{"Watch","","Source","","",0}});assert(hub.select_activity(0));wire.sent.clear();
    hub.receive(BusSide::Back,active(8));
    assert(wire.sent.size()==2&&wire.sent[0].destination==8&&wire.sent[0].data==std::vector<uint8_t>{0x36});
    assert(wire.sent[1].side==BusSide::Back&&wire.sent[1].destination==15&&wire.sent[1].data==std::vector<uint8_t>({0x86,0x10,0}));
    wire.sent.clear();hub.receive(BusSide::Back,active(4));assert(wire.sent.empty());

    m::OwnedIntent context;context.kind=m::OwnedIntentKind::context;context.target.port={group,member,0,m::Side::front};context.selected=true;
    // Equal OSD and logical address cannot make a source on another owner or
    // adapter match this wire's source. No other wire's input needs repair.
    for(const auto& source:{name(controller,m::Side::back,"Source"),name(member,m::Side::back,"Source",1)}){
        context.source=source;assert(hub.execute_owned(context));wire.sent.clear();hub.receive(BusSide::Back,active(4));one_standby(wire,4);
        assert(hub.snapshot().current_activity==0);
    }
    context.source=name(member,m::Side::front,"Front Source");assert(hub.execute_owned(context));
    wire.sent.clear();hub.receive(BusSide::Back,active(8));one_standby(wire,8);
    context.source=name(member,m::Side::back,"Source");assert(hub.execute_owned(context));
    wire.sent.clear();hub.receive(BusSide::Back,active(4));assert(wire.sent.empty());
    hub.receive(BusSide::Back,active(8));assert(wire.sent.size()==2&&wire.sent[1].data[0]==0x86);
    // Preserve the original configured-but-unresolved local-source no-op.
    context.source=name(member,m::Side::back,"Missing");assert(hub.execute_owned(context));
    wire.sent.clear();hub.receive(BusSide::Back,active(4));assert(wire.sent.empty());
    context.source=name(member,m::Side::front,"Missing");assert(hub.execute_owned(context));
    wire.sent.clear();hub.receive(BusSide::Back,active(8));assert(wire.sent.empty());
    // A real, short OSD name sharing the reserved prefix remains ordinary.
    device(hub,BusSide::Back,11,"@foreign:Foo");context.source=name(member,m::Side::back,"@foreign:Foo");assert(hub.execute_owned(context));
    wire.sent.clear();hub.receive(BusSide::Back,active(11));assert(wire.sent.empty());
    // Empty source retains original explicit-input fallback when local.
    context.source.reset();context.switch_device=name(member,m::Side::back,"Source");context.input=3;assert(hub.execute_owned(context));
    wire.sent.clear();hub.receive(BusSide::Back,active(8));
    assert(wire.sent.size()==4&&wire.sent[0].data==std::vector<uint8_t>{0x36}&&wire.sent[1].data==std::vector<uint8_t>({0x44,0x69,3})&&wire.sent[2].data==std::vector<uint8_t>{0x45}&&wire.sent[3].side==BusSide::Front&&wire.sent[3].data[0]==0x82);
    context.selected=false;assert(hub.execute_owned(context));wire.sent.clear();hub.receive(BusSide::Back,active(4));
    assert(wire.sent.size()==2&&wire.sent[0].side==BusSide::Front&&wire.sent[1].side==BusSide::Back&&wire.sent[0].destination==15&&wire.sent[1].destination==15&&wire.sent[0].data==std::vector<uint8_t>{0x36}&&wire.sent[1].data==std::vector<uint8_t>{0x36});
}

static void distributed_source_comparison(){
    fixture_present=true;fixture_peer_address=[](int gpio)->uint8_t{return gpio==1||gpio==3?0:4;};
    HubService primary,other;
    primary.set_system_identity(group,controller);primary.set_boot_identity(controller_boot);assert(primary.start(1,2)==ESP_OK);auto* primary_task=latest_task;
    other.set_system_identity(group,member);other.set_boot_identity(member_boot);other.set_coordinator_authority(false,1);assert(other.start(3,4)==ESP_OK);auto* other_task=latest_task;
    assert(other.bind_owned_controller(controller,1)==ESP_OK);
    other.set_owned_observer([&](const auto& report){assert(primary.update_owned_snapshot(report));});
    latest_task=primary_task;
    assert(primary.configure_scoped({{"Main","tv-a","player-a","","",0},{"Member","tv-b","player-b","","",0}},
        {{"tv-a",name(controller,m::Side::front,"TV")},{"player-a",name(controller,m::Side::back,"Source")},{"tv-b",name(member,m::Side::front,"TV")},{"player-b",name(member,m::Side::back,"Source")}},
        [&](const m::OwnedIntent& intent){
            assert(intent.target.port.octopus_id==member);auto* previous=latest_task;latest_task=other_task;
            m::OwnedAuthorization auth{controller,controller_boot,member_boot,link,1,0};auth.authority=m::OwnedAuthority::control;
            const auto result=(intent.kind==m::OwnedIntentKind::key_edges?other.submit_key_edges(intent.edges,intent.edge_count,intent.generation,link,0):other.submit_owned(intent,auth));latest_task=previous;
            return result.code==m::AdmissionCode::accepted||result.code==m::AdmissionCode::pending||result.code==m::AdmissionCode::overloaded||result.code==m::AdmissionCode::unresolved;
        },controller,1)==ESP_OK);
    run_one_command();fixture_finish_discovery(primary);latest_task=other_task;fixture_finish_discovery(other);latest_task=primary_task;run_idle(2);
    auto advance=[&](unsigned passes){
        for(unsigned pass=0;pass<passes;++pass)for(auto* task:{primary_task,other_task}){latest_task=task;run_idle(1);}
        latest_task=primary_task;
    };
    auto finish_activity=[&](int selected){
        for(unsigned pass=0;pass<64&&(primary.activity_setup_in_progress()||other.activity_setup_in_progress()||primary.snapshot().current_activity!=selected);++pass)advance(1);
        assert(primary.snapshot().current_activity==selected&&!primary.activity_setup_in_progress()&&!other.activity_setup_in_progress());
    };
    auto count=[](int gpio,uint8_t destination,uint8_t opcode){return std::count_if(fixture_sent.begin(),fixture_sent.end(),[&](const auto& sent){return sent.gpio==gpio&&sent.frame.destination()==destination&&sent.frame.size>=2&&sent.frame.data[1]==opcode;});};
    for(int selected:{0,1}){
        fixture_sent.clear();
        if(selected)fixture_async_autocomplete=false;
        assert(primary.select_activity(selected));run_one_command();advance(2);
        if(selected){
            // The new owner progresses while the previous owner's Standby
            // is still physically pending. There is no cross-Amity CEC gate.
            assert(count(1,0,0x36)==1&&count(2,4,0x36)==0);
            assert(count(3,0,0x04)==1&&count(4,4,0x44)==0);
            assert(fixture_complete_next(cec::TxResult::Success,3));advance(2);
            assert(count(4,4,0x44)==1&&count(2,4,0x36)==0);
            assert(fixture_complete_next(cec::TxResult::Success,4));advance(2);
            assert(fixture_complete_next(cec::TxResult::Success,4));advance(2);
            assert(count(3,15,0x82)==1&&count(4,15,0x86)==0&&count(2,4,0x36)==0);
            assert(fixture_complete_next(cec::TxResult::Success,3));advance(2);
            assert(count(4,15,0x86)==1&&count(2,4,0x36)==0);
            assert(fixture_complete_next(cec::TxResult::Success,4));advance(2);
            assert(count(3,0,0x71)==1&&count(2,4,0x36)==0);
            assert(fixture_complete_next(cec::TxResult::Success,3));advance(2);
            assert(!other.activity_setup_in_progress()&&count(2,4,0x36)==0);
            assert(fixture_complete_next(cec::TxResult::Success,1));advance(2);
            assert(count(2,4,0x36)==1&&count(3,0,0x04)==1);
            fixture_complete_all();fixture_async_autocomplete=true;
        }
        finish_activity(selected);const auto generation=primary.activity_generation();
        const int thief_gpio=selected?2:4;const int source_gpio=selected?4:2;
        fixture_sent.clear();fixture_incoming.push_back({thief_gpio,active(4)});
        latest_task=selected?primary_task:other_task;run_idle(1);latest_task=primary_task;run_idle(1);
        assert(count(thief_gpio,4,0x36)==1&&count(source_gpio,15,0x86)==0&&count(thief_gpio,15,0x86)==0);
        assert(count(selected?1:3,15,0x82)==0);
        assert(primary.snapshot().current_activity==selected&&primary.activity_generation()==generation);
        fixture_sent.clear();fixture_incoming.push_back({source_gpio,active(4)});
        latest_task=selected?other_task:primary_task;run_idle(1);latest_task=primary_task;run_idle(1);
        assert(count(source_gpio,4,0x36)==0&&count(source_gpio,15,0x86)==0);
    }
    fixture_sent.clear();assert(primary.standby());run_one_command();finish_activity(-1);
    assert(count(3,0,0x36)==1&&count(4,4,0x36)==1&&count(1,0,0x36)==0&&count(2,4,0x36)==0);
    fixture_sent.clear();fixture_incoming.push_back({2,active(4)});run_idle(1);
    assert(count(1,15,0x36)==1&&count(2,15,0x36)==1); // Existing global standby response.
    fixture_peer_address={};
}
static void three_owner_standby_cycles(){
    const auto third=identity<m::OctopusId>('7');
    const auto third_boot=identity<m::BootId>('8');
    const auto third_link=identity<m::LinkId>('9');
    fixture_present=true;fixture_peer_address=[](int gpio)->uint8_t{return gpio%2?0:4;};
    HubService primary,second,tertiary;
    primary.set_system_identity(group,controller);primary.set_boot_identity(controller_boot);assert(primary.start(1,2)==ESP_OK);auto* primary_task=latest_task;
    second.set_system_identity(group,member);second.set_boot_identity(member_boot);second.set_coordinator_authority(false,1);assert(second.start(3,4)==ESP_OK);auto* second_task=latest_task;assert(second.bind_owned_controller(controller,1)==ESP_OK);
    tertiary.set_system_identity(group,third);tertiary.set_boot_identity(third_boot);tertiary.set_coordinator_authority(false,1);assert(tertiary.start(5,6)==ESP_OK);auto* third_task=latest_task;assert(tertiary.bind_owned_controller(controller,1)==ESP_OK);
    for(auto* owner:{&second,&tertiary})owner->set_owned_observer([&](const auto& report){assert(primary.update_owned_snapshot(report));});
    latest_task=primary_task;
    // All three activities exist before the first cycle, just as on the bench.
    assert(primary.configure_scoped({{"Main","tv-a","player-a","avr-a","",0},{"Member2","tv-b","player-b","avr-b","",0},{"Member3","tv-c","player-c","avr-c","",0}},
       {{"tv-a",name(controller,m::Side::front,"TV")},{"player-a",name(controller,m::Side::back,"Source")},{"avr-a",name(controller,m::Side::back,"AVR")},
        {"tv-b",name(member,m::Side::front,"TV")},{"player-b",name(member,m::Side::back,"Source")},{"avr-b",name(member,m::Side::back,"AVR")},
        {"tv-c",name(third,m::Side::front,"TV")},{"player-c",name(third,m::Side::back,"Source")},{"avr-c",name(third,m::Side::back,"AVR")}},
       [&](const m::OwnedIntent& intent){
          const bool is_third=intent.target.port.octopus_id==third;assert(is_third||intent.target.port.octopus_id==member);
          auto* previous=latest_task;latest_task=is_third?third_task:second_task;
          m::OwnedAuthorization auth{controller,controller_boot,is_third?third_boot:member_boot,is_third?third_link:link,1,0};auth.authority=m::OwnedAuthority::control;
          const auto result=(intent.kind==m::OwnedIntentKind::key_edges?(is_third?tertiary:second).submit_key_edges(intent.edges,intent.edge_count,intent.generation,link,0):(is_third?tertiary:second).submit_owned(intent,auth));latest_task=previous;
          return result.code==m::AdmissionCode::accepted||result.code==m::AdmissionCode::pending||result.code==m::AdmissionCode::overloaded||result.code==m::AdmissionCode::unresolved;
       },controller,1)==ESP_OK);
    run_one_command();fixture_finish_discovery(primary);
    latest_task=second_task;fixture_finish_discovery(second);
    latest_task=third_task;fixture_finish_discovery(tertiary);
    auto advance=[&](unsigned passes){
        for(unsigned i=0;i<passes;++i)for(auto* task:{primary_task,second_task,third_task}){latest_task=task;run_idle(1);}
        latest_task=primary_task;
    };
    auto finish_activity=[&](int selected){
        const auto pending=[&]{return primary.activity_setup_in_progress()||second.activity_setup_in_progress()||tertiary.activity_setup_in_progress();};
        for(unsigned pass=0;pass<96&&(pending()||primary.snapshot().current_activity!=selected);++pass)advance(1);
        assert(primary.snapshot().current_activity==selected&&!pending());
    };
    // Each isolated back bus has an AVR at 1.0.0.0 and its player at 1.1.0.0.
    for(int gpio:{2,4,6}){
        fixture_incoming.push_back({gpio,{{0x4f,0x84,0x11,0,4},5}});
        fixture_incoming.push_back({gpio,{{0x5f,0x84,0x10,0,5},5}});
        fixture_incoming.push_back({gpio,{{0x50,0x47,'A','V','R'},5}});
    }
    advance(4);assert(primary.snapshot().devices.size()==9);
    auto count=[](int gpio,uint8_t destination,uint8_t opcode){
        return std::count_if(fixture_sent.begin(),fixture_sent.end(),[&](const auto& sent){
            return sent.gpio==gpio&&sent.frame.destination()==destination&&sent.frame.size>=2&&sent.frame.data[1]==opcode;
        });
    };
    const cec::Frame player_active{{0x4f,0x82,0x11,0},4};
    for(int selected:{0,1,2}){
        fixture_sent.clear();assert(primary.select_activity(selected));run_one_command();finish_activity(selected);
        assert(primary.snapshot().current_activity==selected);
        const auto generation=primary.activity_generation();
        for(int owner:{0,1,2}){
            const int front=1+2*owner,back=2+2*owner;
            assert(count(front,0,0x04)==(owner==selected));
            assert(count(back,4,0x44)==(owner==selected));
            assert(count(back,5,0x44)==(owner==selected));
            assert(count(back,15,0x86)==(owner==selected));
        }
        for(int thief:{0,1,2})if(thief!=selected){
            fixture_sent.clear();fixture_incoming.push_back({2+2*thief,player_active});advance(2);
            for(int owner:{0,1,2}){
                assert(count(2+2*owner,4,0x36)==(owner==thief));
                assert(count(2+2*owner,15,0x86)==0);
                assert(count(1+2*owner,15,0x82)==0);
            }
            assert(primary.snapshot().current_activity==selected&&primary.activity_generation()==generation);
        }
        fixture_sent.clear();fixture_incoming.push_back({2+2*selected,player_active});advance(2);
        for(int owner:{0,1,2})assert(count(2+2*owner,4,0x36)==0&&count(2+2*owner,15,0x86)==0);
        assert(primary.snapshot().current_activity==selected&&primary.activity_generation()==generation);
        assert(primary.snapshot().devices.size()==9);
        fixture_sent.clear();assert(primary.standby());run_one_command();finish_activity(-1);
        assert(primary.snapshot().current_activity==-1);
        for(int owner:{0,1,2}){
            assert(count(1+2*owner,0,0x36)==(owner==selected));
            assert(count(2+2*owner,4,0x36)==(owner==selected));
            assert(count(2+2*owner,5,0x36)==(owner==selected));
        }
    }
    fixture_peer_address={};
}
int main(){original_and_context_source_comparison();distributed_source_comparison();three_owner_standby_cycles();std::puts("PASS assigned-source comparison: same-name/LA cross-owner/adapter/side suppression, no foreign route repair, local absence/fallback and original global standby");}
