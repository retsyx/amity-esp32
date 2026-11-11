// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#include "amity/hub.hpp"
#include <algorithm>
#include <cassert>
#include <cstdio>
using namespace amity;
namespace m=amity::system;
template<class T> static T identity(char digit){return *T::parse(std::string(32,digit));}
static const auto group=identity<m::ConsortiumId>('1');
static const auto owner=identity<m::OctopusId>('2');
static m::QualifiedDeviceName target(const char* name,m::Side side=m::Side::back){return {{group,owner,0,side},name};}
static const std::vector<m::ScopedSelector> selectors{
    {"TV",target("TV",m::Side::front)},{"Source",target("Source")},{"Other",target("Other")},{"Switch",target("Switch")}
};
struct Steps final:Transport {
    std::vector<m::OwnedIntent> actions,edges,contexts;
    bool pending=false;
    bool send(BusSide,uint8_t,const uint8_t*,size_t)override{assert(false&&"Activity sequencing dispatches to an owner");return false;}
    void request_scan()override{assert(false&&"Only the owner resolver requests discovery");}
    bool activity_step_pending()const override{return pending;}
    m::OwnedDispatchResult dispatch(m::OwnedIntent& intent){
        if(intent.kind==m::OwnedIntentKind::context){contexts.push_back(intent);return {true};}
        if(intent.kind==m::OwnedIntentKind::key_edges){edges.push_back(intent);return {true};}
        assert(!pending);actions.push_back(intent);pending=true;
        return {true,{},true};
    }
    void complete(ActivityHub& hub,bool source_present=true){
        assert(pending&&!actions.empty());pending=false;
        m::AdmissionResult result;result.code=m::AdmissionCode::accepted;
        if(actions.back().kind==m::OwnedIntentKind::source_input)result.source_present=source_present;
        hub.owned_step_result(actions.back(),result);
    }
};
static void bind(ActivityHub& hub,Steps& steps){
    hub.scoped_transport(selectors,[&](auto& intent){return steps.dispatch(intent);});
    hub.scoped_owner(owner);
}
static void drain(ActivityHub& hub,Steps& steps,int index){
    for(unsigned n=0;n<32;++n){if(hub.select_activity(index))return;assert(hub.activity_pending()&&steps.pending);steps.complete(hub);}
    assert(false);
}
static void activity_waits_for_each_owner_action(bool source_present){
    Steps steps;ActivityHub hub(steps);bind(hub,steps);
    assert(hub.configure_scoped({{"Watch","TV","Source","Source","Switch",3}}));
    unsigned ready=0;const auto begin=[&]{++ready;};
    assert(!hub.select_activity(0,begin));
    assert(steps.actions.size()==1&&steps.actions.back().kind==m::OwnedIntentKind::power&&steps.actions.back().target.osd_name=="TV"&&steps.actions.back().on);
    assert(hub.activity_pending()&&hub.snapshot().current_activity==-1);
    // A lookup or transmission pending at an owner pauses this recipe. Cached
    // later work and another selection cannot pass the current action.
    assert(!hub.select_activity(0,begin)&&!hub.standby()&&steps.actions.size()==1);
    steps.complete(hub);assert(!hub.select_activity(0,begin));
    assert(steps.actions.size()==2&&steps.actions.back().target.osd_name=="Source"&&steps.actions.back().kind==m::OwnedIntentKind::power);
    assert(!hub.select_activity(0,begin)&&steps.actions.size()==2);
    steps.complete(hub);assert(!hub.select_activity(0,begin));
    assert(steps.actions.size()==3&&steps.actions.back().target.osd_name=="Switch"&&steps.actions.back().kind==m::OwnedIntentKind::power);
    steps.complete(hub);assert(!hub.select_activity(0,begin));
    assert(steps.actions.size()==4&&steps.actions.back().kind==m::OwnedIntentKind::announce_active_source);
    steps.complete(hub);assert(!hub.select_activity(0,begin));
    assert(steps.actions.size()==5&&steps.actions.back().kind==m::OwnedIntentKind::source_input&&hub.snapshot().current_activity==-1);
    steps.complete(hub,source_present);assert(!hub.select_activity(0,begin));
    if(!source_present){
        assert(steps.actions.size()==6&&steps.actions.back().kind==m::OwnedIntentKind::switch_input&&steps.actions.back().input==3&&hub.snapshot().current_activity==-1);
        steps.complete(hub);assert(!hub.select_activity(0,begin));
    }
    assert(hub.snapshot().current_activity==0&&steps.actions.back().kind==m::OwnedIntentKind::query_audio);
    steps.complete(hub);assert(hub.select_activity(0,begin));
    assert(!hub.activity_pending()&&ready==1);
    assert(std::count_if(steps.actions.begin(),steps.actions.end(),[](const auto& action){return action.kind==m::OwnedIntentKind::power&&action.target.osd_name=="TV";})==1);
}
static void configuration_preserves_selection_and_standby_order(){
    Steps steps;ActivityHub hub(steps);bind(hub,steps);
    const Activity original{"Original","TV","Source","Source","",0,identity<m::ActivityId>('3')};
    assert(hub.configure_scoped({original}));drain(hub,steps,0);steps.actions.clear();
    assert(hub.configure_scoped({original,{"Other","TV","Other","Other","",0,identity<m::ActivityId>('4')}}));
    auto changed=original;changed.name="Changed";
    assert(!hub.configure_scoped({changed}));
    assert(hub.snapshot().current_activity==0&&hub.snapshot().activities[0].name=="Original"&&steps.actions.empty());
    unsigned ready=0;
    assert(!hub.standby([&]{++ready;}));
    assert(steps.actions.size()==1&&steps.actions.back().target.osd_name=="TV"&&!steps.actions.back().on);
    assert(!hub.configure_scoped({original}));
    steps.complete(hub);assert(!hub.standby([&]{++ready;}));
    assert(steps.actions.size()==2&&steps.actions.back().target.osd_name=="Source"&&!steps.actions.back().on);
    steps.complete(hub);assert(hub.standby([&]{++ready;})&&ready==1&&hub.snapshot().current_activity==-1);
    steps.actions.clear();ready=0;
    assert(!hub.standby([&]{++ready;}));
    assert(steps.actions.size()==1&&steps.actions.back().kind==m::OwnedIntentKind::standby_wire&&steps.actions.back().target.port.side==m::Side::front);
    steps.complete(hub);assert(!hub.standby([&]{++ready;}));
    assert(steps.actions.size()==2&&steps.actions.back().kind==m::OwnedIntentKind::standby_wire&&steps.actions.back().target.port.side==m::Side::back);
    steps.complete(hub);assert(hub.standby([&]{++ready;})&&ready==1&&!hub.activity_pending());
}
static void shared_devices_change_power_once(){
    Steps steps;ActivityHub hub(steps);bind(hub,steps);
    assert(hub.configure_scoped({{"Shared","TV","Source","Source","Source",0},{"Other","TV","Other","Other","",0}}));
    const auto power=[&](const char* name,bool on){
        return std::find_if(steps.actions.begin(),steps.actions.end(),[&](const auto& action){
            return action.kind==m::OwnedIntentKind::power&&action.target.osd_name==name&&action.on==on;})-steps.actions.begin();
    };
    const auto count=[&](const char* name,bool on){
        return std::count_if(steps.actions.begin(),steps.actions.end(),[&](const auto& action){
            return action.kind==m::OwnedIntentKind::power&&action.target.osd_name==name&&action.on==on;});
    };
    // One device in several roles is powered once. A repair powers the same
    // recipients again without Standby.
    for(unsigned attempt=0;attempt<2;++attempt){
        steps.actions.clear();drain(hub,steps,0);
        assert(count("TV",true)==1&&count("Source",true)==1&&count("TV",false)==0&&count("Source",false)==0);
    }
    // The shared display is retained. Old-only Standby precedes new-only power-on.
    steps.actions.clear();drain(hub,steps,1);
    assert(count("TV",true)==0&&count("TV",false)==0&&count("Source",false)==1&&count("Other",true)==1);
    assert(power("Source",false)<power("Other",true));
    steps.actions.clear();drain(hub,steps,-1);
    assert(count("TV",false)==1&&count("Other",false)==1&&count("Source",false)==0);
}
static void key_edges_follow_current_selection(){
    Steps steps;ActivityHub hub(steps);bind(hub,steps);
    assert(hub.configure_scoped({{"Before","TV","Source","Source","",0},{"After","TV","Other","Other","",0}}));
    drain(hub,steps,0);drain(hub,steps,1);steps.edges.clear();
    assert(hub.key_target(4)==target("Other")&&hub.key_target(0x41)==target("Other"));
    assert(hub.press_key(4,true)&&hub.press_key(0x41,true)&&hub.release_key(4)&&hub.release_key(0x41));
    assert(steps.edges.size()==4);
    for(size_t i=0;i<steps.edges.size();++i){const auto& intent=steps.edges[i];
        assert(intent.target.port==target("Other").port&&intent.edge_count==1&&intent.edges[0].key==(i%2?0x41:4)&&intent.edges[0].down==(i<2));
    }
    assert(hub.snapshot().current_activity==1&&!hub.activity_pending());
}
struct Wire final:Transport {
    struct Sent {BusSide side;uint8_t address;std::vector<uint8_t> data;};
    std::vector<Sent> sent;std::vector<BusSide> lookups;
    bool tx_pending=false,scan_allowed=false;
    bool send(BusSide side,uint8_t address,const uint8_t* data,size_t size)override{sent.push_back({side,address,{data,data+size}});tx_pending=true;return true;}
    void request_scan()override{assert(false);}
    bool request_scan(BusSide side)override{lookups.push_back(side);return scan_allowed;}
    bool activity_step_pending()const override{return tx_pending;}
    void complete(){tx_pending=false;}
    size_t count(BusSide side,uint8_t address,std::initializer_list<uint8_t> data)const{return std::count_if(sent.begin(),sent.end(),[&](const auto& frame){return frame.side==side&&frame.address==address&&frame.data==std::vector<uint8_t>(data);});}
};
static void device(Hub& hub,BusSide side,uint8_t address,uint16_t physical,uint8_t type,const char* name){
    hub.discovered(side,address);
    hub.receive(side,{{static_cast<uint8_t>((address<<4)|15),0x84,static_cast<uint8_t>(physical>>8),static_cast<uint8_t>(physical),type},5});
    cec::Frame osd;osd.data[0]=(address<<4)|(side==BusSide::Front?4:0);osd.data[1]=0x47;osd.size=2;
    while(*name)osd.data[osd.size++]=*name++;
    hub.receive(side,osd);
}
static void received_standby_is_sequential(){
    Wire wire;Hub hub(wire);hub.set_addresses(4,0);
    const cec::Frame thief{{0x8f,0x82,0x20,0},4};
    assert(hub.receive(BusSide::Back,thief)==4&&hub.receive_step()==Hub::ReceiveStep::ForceBack);
    assert(wire.count(BusSide::Front,15,{0x36})==1&&wire.count(BusSide::Back,15,{0x36})==0);
    assert(wire.count(BusSide::Back,8,{0x83})==1&&wire.count(BusSide::Back,8,{0x46})==1&&wire.count(BusSide::Back,8,{0x8c})==1);
    const auto observed=wire.sent.size();
    assert(hub.receive(BusSide::Back,thief,hub.receive_step())==4&&wire.sent.size()==observed);
    wire.complete();assert(hub.receive(BusSide::Back,thief,hub.receive_step())==4);
    assert(wire.count(BusSide::Back,15,{0x36})==1&&hub.receive_step()==Hub::ReceiveStep::Done);
    wire.complete();assert(hub.receive(BusSide::Back,thief,hub.receive_step())==0);
    assert(wire.sent.size()==observed+1&&wire.lookups.empty());
}
static void received_switch_waits_without_repeating(){
    Wire wire;Hub hub(wire);hub.set_addresses(4,0);
    device(hub,BusSide::Front,0,0,0,"TV");
    m::OwnedIntent context;context.kind=m::OwnedIntentKind::context;context.target.port={group,owner,0,m::Side::front};
    context.selected=true;context.display=target("TV",m::Side::front);context.switch_device=target("Wanted");context.input=3;
    assert(hub.execute_owned(context));
    wire.sent.clear();wire.lookups.clear();wire.scan_allowed=true;wire.complete();
    const cec::Frame thief{{0x8f,0x82,0x20,0},4};
    assert(hub.receive(BusSide::Back,thief)==4&&wire.count(BusSide::Back,8,{0x36})==1&&wire.lookups.empty());
    wire.complete();assert(hub.receive(BusSide::Back,thief,hub.receive_step())==2);
    assert(wire.lookups==std::vector<BusSide>{BusSide::Back}&&hub.receive_step()==Hub::ReceiveStep::Switch);
    const auto suspended_step=hub.receive_step();
    auto earlier=wire.sent;device(hub,BusSide::Back,5,0x1000,5,"Wanted");wire.sent=std::move(earlier);wire.complete();wire.scan_allowed=false;
    assert(hub.receive(BusSide::Back,thief,suspended_step)==4&&hub.receive_step()==Hub::ReceiveStep::Announce);
    assert(wire.count(BusSide::Back,8,{0x36})==1&&wire.count(BusSide::Back,5,{0x44,0x69,3})==1&&wire.count(BusSide::Front,15,{0x82,0x10,0})==0);
    wire.complete();assert(hub.receive(BusSide::Back,thief,hub.receive_step())==4);
    assert(wire.count(BusSide::Front,15,{0x82,0x10,0})==1);
    wire.complete();assert(hub.receive(BusSide::Back,thief,hub.receive_step())==0);
}
int main(){
    activity_waits_for_each_owner_action(true);activity_waits_for_each_owner_action(false);configuration_preserves_selection_and_standby_order();shared_devices_change_power_once();key_edges_follow_current_selection();received_standby_is_sequential();received_switch_waits_without_repeating();
    std::puts("Activity sequencing waits for each owner action, changes shared-device power once, preserves configuration and key routing, and physical source repair resumes without repeated frames");
}
