// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#include "cec_hub_fixture.hpp"
static void finish_selection(amity::HubService& hub,int target){
    for(unsigned pass=0;pass<96;++pass){
        run_idle(1);
        if(hub.snapshot().current_activity==target&&!hub.activity_setup_in_progress()&&fixture_pending.empty())return;
    }
    assert(false&&"Accepted activity did not finish its original TX steps");
}
int main(){
    fixture_present=true;amity::HubService hub;assert(hub.start(1,2)==ESP_OK);
    assert(hub.configure({{"First","TV","Source","Source","",0},{"Second","TV","Source","Source","",0}}));
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
    for(int n=0;n<32;++n)assert(hub.release_key());
    assert(hub.request_activity(-1)==ESP_ERR_NO_MEM&&hub.activity_target()==0);
    for(int n=0;n<32;++n)run_one_command();
    assert(hub.request_activity(-1)==ESP_OK);finish_selection(hub,-1);
    fixture_tick=100000;
    std::puts("Activity admission and input readiness wait only for actual lookup and TX completion; no peer status or quiet-time dependency");
}
