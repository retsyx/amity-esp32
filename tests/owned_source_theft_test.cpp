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
    other.set_owned_input_observer([&](const auto& intent,const auto&,const auto& result){if(result.source_present)assert(primary.owned_input_result(member,intent.generation,intent.sequence,*result.source_present));});
    latest_task=primary_task;
    assert(primary.configure_scoped({{"Main","tv-a","player-a","","",0},{"Member","tv-b","player-b","","",0}},
        {{"tv-a",name(controller,m::Side::front,"TV")},{"player-a",name(controller,m::Side::back,"Source")},{"tv-b",name(member,m::Side::front,"TV")},{"player-b",name(member,m::Side::back,"Source")}},
        [&](const m::OwnedIntent& intent){
            assert(intent.target.port.octopus_id==member);auto* previous=latest_task;latest_task=other_task;
            m::OwnedAuthorization auth{controller,controller_boot,member_boot,link,1,0};auth.authority=intent.kind==m::OwnedIntentKind::key_up?m::OwnedAuthority::cleanup:intent.kind==m::OwnedIntentKind::key_down?m::OwnedAuthority::realtime:m::OwnedAuthority::control;
            const auto result=other.submit_owned(intent,auth);latest_task=previous;
            if(intent.kind==m::OwnedIntentKind::source_input&&result.source_present)assert(primary.owned_input_result(member,intent.generation,intent.sequence,*result.source_present));
            return result.code==m::AdmissionCode::accepted||result.code==m::AdmissionCode::pending||result.code==m::AdmissionCode::overloaded||result.code==m::AdmissionCode::unresolved;
        },controller,1)==ESP_OK);
    run_one_command();fixture_finish_discovery(primary);latest_task=other_task;fixture_finish_discovery(other);latest_task=primary_task;run_idle(2);
    auto count=[](int gpio,uint8_t destination,uint8_t opcode){return std::count_if(fixture_sent.begin(),fixture_sent.end(),[&](const auto& sent){return sent.gpio==gpio&&sent.frame.destination()==destination&&sent.frame.size>=2&&sent.frame.data[1]==opcode;});};
    for(int selected:{0,1}){
        assert(primary.select_activity(selected));run_one_command();run_idle(2);const auto generation=primary.activity_generation();
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
    fixture_sent.clear();assert(primary.standby());run_one_command();run_idle(1);
    assert(count(3,0,0x36)==1&&count(4,4,0x36)==1&&count(1,0,0x36)==0&&count(2,4,0x36)==0);
    fixture_sent.clear();fixture_incoming.push_back({2,active(4)});run_idle(1);
    assert(count(1,15,0x36)==1&&count(2,15,0x36)==1); // Existing global standby response.
    fixture_peer_address={};
}
int main(){original_and_context_source_comparison();distributed_source_comparison();std::puts("PASS assigned-source comparison: same-name/LA cross-owner/adapter/side suppression, no foreign route repair, local absence/fallback and original global standby");}
