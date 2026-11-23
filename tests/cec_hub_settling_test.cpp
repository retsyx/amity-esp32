// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#include "cec_hub_fixture.hpp"
static void finish_selection(amity::HubService& hub,int target){
    for(unsigned pass=0;pass<96;++pass){
        run_idle(1);
        if(hub.snapshot().current_activity==target&&!hub.activity_setup_in_progress()&&fixture_pending.empty())return;
    }
    assert(false&&"Accepted activity did not finish its original TX steps");
}
// A firmware update cancels an activity whose first frame is still on the
// wire. That admitted frame is activity work until it has finished.
static void canceled_activity_frame_is_settling(){
    fixture_present=true;fixture_tick=100000;fixture_async_autocomplete=true;
    LocalHubService hub;assert(fixture_start(hub,1,2)==ESP_OK);
    assert(fixture_configure(hub,{{"Watch","TV","Source","Source","",0}}));run_one_command();fixture_finish_discovery(hub);
    fixture_tick=0;fixture_async_autocomplete=false;
    assert(hub.request_activity(0)==ESP_OK);run_one_command();run_idle(2);
    assert(fixture_pending.size()==1&&fixture_pending.front().frame.data[1]==0x04&&hub.snapshot().activity_settling);
    assert(hub.begin_maintenance()==ESP_OK);run_idle(2);
    assert(!hub.maintenance_ready()&&hub.snapshot().current_activity==-1&&hub.snapshot().activity_settling);
    fixture_complete_all();run_idle(2);
    assert(hub.maintenance_ready()&&!hub.snapshot().activity_settling);
    hub.end_maintenance();fixture_async_autocomplete=true;fixture_tick=100000;
}
// Amity answers received traffic during an activity on that traffic's
// adapter. On a two-adapter board the activity's last frame on adapter 0 can
// finish while such an answer on adapter 1 is still on the wire. The activity
// is settling until that answer has finished too.
static void answer_during_activity_is_settling(){
    fixture_present=true;fixture_tick=100000;fixture_async_autocomplete=true;
    fixture_peer_address=[](int gpio)->uint8_t{return gpio==1?0:gpio==2?4:gpio==6?8:15;};
    cec::BoardProfile profile{cec::BoardProfileId::Development4Tentacle,1,amity::max_tentacles,{},-1,-1};
    const int gpios[8]={1,2,5,6,-1,-1,-1,-1};
    for(unsigned i=0;i<8;++i)profile.ports[i]={static_cast<uint8_t>(i/2),i%2?cec::Side::Back:cec::Side::Front,gpios[i]};
    LocalHubService hub;assert(fixture_start(hub,profile)==ESP_OK);
    assert(fixture_configure(hub,{{"Watch","TV","Source","Source","",0}}));run_one_command();fixture_finish_discovery(hub);
    uint8_t own=15;for(const auto& report:hub.owned_snapshots())if(report.adapter==1)own=report.back_address;
    assert(own<15);
    const auto answers=[]{return std::count_if(fixture_pending.begin(),fixture_pending.end(),[](const auto& tx){
        return tx.gpio==6&&tx.frame.size==5&&tx.frame.data[1]==0x87;});};
    fixture_tick=0;fixture_async_autocomplete=false;
    assert(hub.request_activity(0)==ESP_OK);run_one_command();run_idle(2);
    for(unsigned step=0;step<16;++step){
        // The player at 8 on adapter 1 asks for Amity's vendor ID as the next
        // activity frame on adapter 0 completes.
        fixture_incoming.push_back({6,{{static_cast<uint8_t>(0x80|own),0x8c},2}});
        if(!fixture_complete_next(cec::TxResult::Pending,1))fixture_complete_next(cec::TxResult::Pending,2);
        run_idle(1);
        assert(answers()==1&&hub.snapshot().activity_settling);
        if(hub.snapshot().current_activity==0&&!hub.activity_setup_in_progress()){
            // The activity has finished; its last answer has not.
            fixture_complete_all();run_idle(2);
            assert(!hub.snapshot().activity_settling);
            fixture_peer_address={};fixture_async_autocomplete=true;fixture_tick=100000;return;
        }
        while(fixture_complete_next(cec::TxResult::Pending,6)){}
        run_idle(1);
    }
    assert(false&&"The activity did not finish");
}
int main(){
    fixture_present=true;LocalHubService hub;assert(fixture_start(hub,1,2)==ESP_OK);
    assert(fixture_configure(hub,{{"First","TV","Source","Source","",0},{"Second","TV","Source","Source","",0}}));
    run_one_command();fixture_finish_discovery(hub);
    fixture_tick=0;const auto started=fixture_time;fixture_async_autocomplete=false;fixture_sent.clear();
    assert(hub.request_activity(0)==ESP_OK&&hub.activity_target()==0);
    assert(hub.press_key(0x40)); // Setup discards new downs before pressed-state registration.
    assert(hub.request_activity(0)==ESP_OK); // An accepted same-target repair remains behind setup.
    run_one_command();run_idle(2);
    assert(hub.snapshot().current_activity==-1&&hub.snapshot().activity_settling);
    assert(fixture_pending.size()==1&&fixture_pending.front().gpio==1&&fixture_pending.front().frame.data[1]==0x04);
    assert(hub.request_activity(1)==ESP_OK);run_one_command();run_idle(2);
    assert(fixture_sent.size()==1); // No ignored Power or repair can overtake this completion.
    fixture_complete_all();fixture_async_autocomplete=true;finish_selection(hub,1);
    assert(fixture_time==started&&!hub.snapshot().activity_settling);
    // The peers never sent Active Source, Audio Status or Power Status. No
    // response timer or quiet period is a prerequisite for the next selection.
    assert(hub.request_activity(-1)==ESP_OK);finish_selection(hub,-1);
    assert(!hub.snapshot().activity_settling&&fixture_time==started);
    assert(hub.request_activity(0)==ESP_OK);finish_selection(hub,0);
    for(int n=0;n<32;++n)assert(hub.release_key(0));
    assert(hub.request_activity(-1)==ESP_ERR_NO_MEM&&hub.activity_target()==0);
    for(int n=0;n<32;++n)run_one_command();
    assert(hub.request_activity(-1)==ESP_OK);finish_selection(hub,-1);
    fixture_tick=100000;
    canceled_activity_frame_is_settling();answer_during_activity_is_settling();
    std::puts("Activity admission and input readiness wait only for actual lookup and TX completion; no peer status or quiet-time dependency; a canceled activity's admitted frame and an answer sent during an activity settle when they finish");
}
