// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#include "amity/hub.hpp"
#include <algorithm>
#include <cassert>
#include <cstdio>
using namespace amity;
struct Wire final:Transport {
    struct Sent {BusSide side;uint8_t address;std::vector<uint8_t> data;};
    std::vector<Sent> sent;std::vector<BusSide> lookups;
    bool tx_pending=false,scan_allowed=false;
    bool send(BusSide side,uint8_t address,const uint8_t* data,size_t size)override{
        sent.push_back({side,address,{data,data+size}});tx_pending=true;return true;
    }
    void request_scan()override{assert(false);}
    bool request_scan(BusSide side)override{lookups.push_back(side);return scan_allowed;}
    bool activity_step_pending()const override{return tx_pending;}
    void complete(){tx_pending=false;}
    size_t count(BusSide side,uint8_t address,std::initializer_list<uint8_t> data)const{
        return std::count_if(sent.begin(),sent.end(),[&](const auto& frame){return frame.side==side&&frame.address==address&&frame.data==std::vector<uint8_t>(data);});
    }
};
static void device(Hub& hub,BusSide side,uint8_t address,uint16_t physical,uint8_t type,const char* name){
    hub.discovered(side,address);
    hub.receive(side,{{static_cast<uint8_t>((address<<4)|15),0x84,static_cast<uint8_t>(physical>>8),static_cast<uint8_t>(physical),type},5});
    cec::Frame osd;osd.data[0]=(address<<4)|(side==BusSide::Front?4:0);osd.data[1]=0x47;osd.size=2;
    while(*name)osd.data[osd.size++]=*name++;
    hub.receive(side,osd);
}
static void drain(Hub& hub,Wire& wire,int index){
    for(unsigned n=0;n<32;++n){if(hub.select_activity(index))return;assert(hub.activity_pending());wire.complete();}
    assert(false);
}
static void lookup_is_just_in_time(bool found){
    Wire wire;Hub hub(wire);hub.set_addresses(4,0);
    hub.configure({{"Watch","TV","Wanted","Wanted","Switch",3}});
    device(hub,BusSide::Front,0,0,0,"TV");device(hub,BusSide::Back,5,0x1000,5,"Switch");
    wire.sent.clear();wire.lookups.clear();wire.complete();wire.scan_allowed=true;unsigned ready=0;
    const auto begin=[&]{++ready;};
    assert(!hub.select_activity(0,begin));
    assert(wire.count(BusSide::Front,0,{0x04})==1&&wire.sent.size()==1&&wire.lookups.empty());
    assert(hub.activity_pending()&&hub.snapshot().current_activity==-1);
    // The next recipient's lookup cannot start before the first recipient's
    // original command completes; another action cannot replace the PC.
    assert(!hub.standby()&&wire.sent.size()==1);
    wire.complete();assert(!hub.select_activity(0,begin));
    assert(hub.pending_resolution()==2&&wire.lookups==std::vector<BusSide>{BusSide::Back});
    assert(wire.sent.size()==1); // Even the already-cached switch waits its turn.
    wire.scan_allowed=false;
    if(found){auto earlier=wire.sent;device(hub,BusSide::Back,4,0x1100,4,"Wanted");wire.sent=std::move(earlier);}
    wire.complete();assert(!hub.select_activity(0,begin));
    assert(wire.sent.size()==3); // Exactly one original power press/release pair.
    if(found){assert(wire.count(BusSide::Back,4,{0x44,0x6d})==1&&wire.count(BusSide::Back,5,{0x44,0x6d})==0);}
    else assert(wire.count(BusSide::Back,5,{0x44,0x6d})==1);
    wire.complete();assert(!hub.select_activity(0,begin));
    if(found){
        assert(wire.count(BusSide::Back,5,{0x44,0x6d})==1&&wire.count(BusSide::Back,15,{0x86,0x11,0})==0);
        wire.complete();assert(!hub.select_activity(0,begin));
        assert(wire.count(BusSide::Front,15,{0x82,0x10,0})==1&&wire.count(BusSide::Back,15,{0x86,0x11,0})==0);
        wire.complete();assert(!hub.select_activity(0,begin));
        assert(wire.count(BusSide::Back,15,{0x86,0x11,0})==1&&hub.snapshot().current_activity==-1);
        wire.complete();assert(!hub.select_activity(0,begin));
        assert(hub.snapshot().current_activity==0&&wire.count(BusSide::Back,4,{0x71})==1);
    }else{
        assert(wire.count(BusSide::Back,5,{0x44,0x69,3})==0&&wire.count(BusSide::Front,15,{0x82,0x10,0})==1);
        wire.complete();assert(!hub.select_activity(0,begin));
        assert(wire.count(BusSide::Back,5,{0x44,0x69,3})==1&&wire.count(BusSide::Front,15,{0x82,0x10,0})==1&&hub.snapshot().current_activity==-1);
    }
    wire.complete();assert(hub.select_activity(0,begin));
    assert(!hub.activity_pending()&&hub.snapshot().current_activity==0&&ready==1);
    assert(wire.count(BusSide::Front,0,{0x04})==1);
}
static void configuration_and_global_standby_wait(){
    Wire wire;Hub hub(wire);hub.set_addresses(4,0);
    hub.configure({{"Original","TV","Source","Source","",0}});
    device(hub,BusSide::Front,0,0,0,"TV");device(hub,BusSide::Back,4,0x1000,4,"Source");
    wire.complete();drain(hub,wire,0);wire.sent.clear();unsigned ready=0;
    const auto edit=[&]{return hub.configure({{"Replacement","TV","Source","Source","",0}},[&]{++ready;});};
    assert(!edit());assert(wire.count(BusSide::Front,0,{0x36})==1&&wire.count(BusSide::Back,4,{0x36})==0);
    assert(hub.snapshot().activities[0].name=="Original");
    wire.complete();assert(!edit());assert(wire.count(BusSide::Back,4,{0x36})==1);
    wire.complete();assert(edit()&&ready==1&&hub.snapshot().activities[0].name=="Replacement");
    assert(wire.count(BusSide::Front,15,{0x82,0x10,0})==0);
    wire.sent.clear();ready=0;
    assert(!hub.standby([&]{++ready;}));assert(wire.count(BusSide::Front,15,{0x36})==1&&wire.count(BusSide::Back,15,{0x36})==0);
    wire.complete();assert(!hub.standby([&]{++ready;}));assert(wire.count(BusSide::Back,15,{0x36})==1);
    wire.complete();assert(hub.standby([&]{++ready;})&&ready==1&&!hub.activity_pending());
}
static void keys_after_activity_remain_independent(){
    Wire wire;Hub hub(wire);hub.set_addresses(4,0);
    hub.configure({{"Before","TV","Source","Source","",0},{"After","TV","Other","Other","",0}});
    device(hub,BusSide::Front,0,0,0,"TV");device(hub,BusSide::Back,4,0x1000,4,"Source");device(hub,BusSide::Back,8,0x2000,4,"Other");
    wire.complete();drain(hub,wire,0);wire.complete();drain(hub,wire,1);wire.sent.clear();
    assert(hub.press_key(4,true)&&hub.press_key(0x41,true));
    assert(wire.count(BusSide::Back,8,{0x44,4})==1&&wire.count(BusSide::Back,8,{0x44,0x41})==1);
    assert(hub.release_key(4)&&wire.count(BusSide::Back,8,{0x45})==0);
    assert(hub.release_key(0x41)&&wire.count(BusSide::Back,8,{0x45})==1);
    assert(hub.snapshot().current_activity==1&&!hub.activity_pending());
}
static void received_standby_does_not_use_user_pc(){
    Wire wire;Hub hub(wire);hub.set_addresses(4,0);hub.configure({});wire.complete();
    const cec::Frame thief{{0x8f,0x82,0x20,0},4};
    assert(hub.receive(BusSide::Back,thief)==4&&hub.receive_step()==Hub::ReceiveStep::ForceBack);
    assert(!hub.activity_pending()&&wire.count(BusSide::Front,15,{0x36})==1&&wire.count(BusSide::Back,15,{0x36})==0);
    assert(wire.count(BusSide::Back,8,{0x83})==1&&wire.count(BusSide::Back,8,{0x46})==1&&wire.count(BusSide::Back,8,{0x8c})==1);
    const auto observed=wire.sent.size();
    assert(hub.receive(BusSide::Back,thief,hub.receive_step())==4&&wire.sent.size()==observed);
    wire.complete();assert(hub.receive(BusSide::Back,thief,hub.receive_step())==4);
    assert(wire.count(BusSide::Back,15,{0x36})==1&&hub.receive_step()==Hub::ReceiveStep::Done);
    wire.complete();assert(hub.receive(BusSide::Back,thief,hub.receive_step())==0&&!hub.activity_pending());
    assert(wire.sent.size()==observed+1&&hub.snapshot().current_activity==-1&&wire.lookups.empty());
}
static void received_switch_waits_without_repeating(){
    Wire wire;Hub hub(wire);hub.set_addresses(4,0);hub.configure({{"Switch","TV","","","Wanted",3}});
    device(hub,BusSide::Front,0,0,0,"TV");wire.complete();drain(hub,wire,0);
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
    lookup_is_just_in_time(true);lookup_is_just_in_time(false);configuration_and_global_standby_wait();keys_after_activity_remain_independent();received_standby_does_not_use_user_pc();received_switch_waits_without_repeating();
    std::puts("Native activity program counters preserve recipient/lookup/TX order, switch fallback, once-only effects, configuration cleanup and independent keys after completion");
}
