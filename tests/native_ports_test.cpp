#include "cec_hub_fixture.hpp"
#include <array>
#include <iostream>
using namespace amity::system;
template<class T>T live_id(unsigned n){char text[33];std::snprintf(text,sizeof(text),"%032x",n);return T::parse(text).value();}
static void adapter_settings_owner_tests() {
    fixture_tick = 1000; fixture_send = {}; fixture_transmit = {};
    fixture_incoming.clear(); fixture_sent.clear(); fixture_present = false;
    const auto boot = live_id<BootId>(3);
    auto setup = [&](amity::HubService& owner) {
        owner.set_system_identity(live_id<ConsortiumId>(1), live_id<OctopusId>(2));
        owner.set_boot_identity(boot);
    };
    auto deadline = [] { return uint64_t(fixture_time) / 1000 + 5000; };
    {
        amity::HubService owner; setup(owner);
        assert(owner.start(cec::legacy_buttonless_profile, std::array<uint16_t,4>{{0x2000,0x3000,0x4000,0x5000}}) == ESP_OK);
        assert(owner.bind_owned_controller(live_id<OctopusId>(2),1)==ESP_OK);fixture_finish_discovery(owner);
        const auto initial = owner.snapshot();
        const auto slots = owner.adapter_runtime_snapshot(); assert(slots.size() == 4);
        for (unsigned i=0;i<4;++i) assert(slots[i].tentacle_id == i && slots[i].configured == (i==0));
        assert(!slots[1].effective_front_physical && !slots[0].observed_front_physical);
        unsigned writes = 0;
        assert(owner.commit_adapter_settings(live_id<BootId>(4), {{0,0x6000}}, deadline(), [&]{++writes;return ESP_OK;}) == ESP_ERR_INVALID_STATE);
        assert(owner.commit_adapter_settings(boot, {{0,0x6000}}, deadline(), [&]{++writes;return ESP_FAIL;}) == ESP_FAIL);
        assert(writes == 1 && !owner.maintenance_ready()); // Failed persistence restores fresh admission.
        assert(owner.adapter_runtime_snapshot()[0].effective_front_physical == slots[0].effective_front_physical);
        assert(owner.snapshot().discovery_generation == initial.discovery_generation && owner.snapshot().devices.size() == initial.devices.size());
        assert(owner.commit_adapter_settings(boot, {{0,0x6000}}, deadline(), [&]{
            ++writes; fixture_time += 6000000; fixture_late_reply = true; return ESP_OK; // A started bounded flash write may cross the request deadline.
        }) == ESP_OK);
        assert(writes == 2 && !owner.maintenance_ready() && !fixture_late_reply);
        auto applied = owner.adapter_runtime_snapshot();
        assert(applied[0].effective_front_physical == 0x6000 && applied[0].selected_from_default);
        assert(!applied[0].observed_front_physical);
        assert(owner.snapshot().discovery_generation == initial.discovery_generation);
        // A fresh same-boot save and normal work remain available immediately.
        assert(owner.commit_adapter_settings(boot, {{0,0x7000}}, deadline(), [&]{++writes;return ESP_OK;}) == ESP_OK);
        assert(writes == 3 && owner.adapter_runtime_snapshot()[0].effective_front_physical == 0x7000);
        assert(owner.configure({})); run_one_command();
        assert(owner.commit_adapter_settings(boot, {{4,0x6000}}, deadline(), [&]{++writes;return ESP_OK;}) == ESP_ERR_INVALID_ARG);
        assert(owner.commit_adapter_settings(boot, {{0,0x6000},{0,0x5000}}, deadline(), [&]{++writes;return ESP_OK;}) == ESP_ERR_INVALID_ARG);
    }
    auto sparse = cec::legacy_buttonless_profile; sparse.id = cec::BoardProfileId::Development4Tentacle; sparse.tentacle_count = 4;
    for(unsigned i=0;i<8;++i)sparse.ports[i]={static_cast<uint8_t>(i/2),i%2?cec::Side::Back:cec::Side::Front,-1};
    {
        amity::HubService owner; setup(owner); assert(owner.start(sparse)==ESP_OK);assert(owner.bind_owned_controller(live_id<OctopusId>(2),1)==ESP_OK);run_idle(2);
        for(const auto& slot:owner.adapter_runtime_snapshot())assert(!slot.configured&&!slot.effective_front_physical);
        unsigned writes=0;assert(owner.commit_adapter_settings(boot,{{3,0x5200}},deadline(),[&]{++writes;return ESP_OK;})==ESP_ERR_INVALID_STATE);assert(!writes);
    }
    sparse.ports[6].gpio=9;sparse.ports[7].gpio=44;
    {
        amity::HubService owner;setup(owner);
        assert(owner.start(sparse,std::array<uint16_t,4>{{0x1000,0x2000,0x3000,0x4000}})==ESP_OK);
        assert(owner.bind_owned_controller(live_id<OctopusId>(2),1)==ESP_OK);run_idle(2);
        assert(owner.adapter_runtime_snapshot()[3].configured);
        // A configured wire is editable without a responding device. Its
        // communication attempts still get their real CEC result normally.
        unsigned writes=0;
        const auto too_short=uint64_t(fixture_time)/1000+3;
        assert(owner.commit_adapter_settings(boot,{{3,0x5200}},too_short,[&]{++writes;return ESP_OK;})==ESP_ERR_TIMEOUT);run_idle(3);assert(!writes);
        assert(owner.commit_adapter_settings(boot,{{3,0x5200}},deadline(),[&]{++writes;return ESP_OK;})==ESP_OK);assert(writes==1);
        // Saving while startup scanning is paused must resume that same scan
        // using the new fallback, without a reboot or a new scan request.
        for(unsigned pass=0;pass<2000&&!owner.adapter_runtime_snapshot()[3].effective_front_physical;++pass)run_idle(1);
        const auto applied=owner.adapter_runtime_snapshot()[3];
        assert(applied.effective_front_physical==0x5200&&applied.selected_from_default&&!applied.observed_front_physical);
    }
    fixture_tick=100000;fixture_present=true;
}

static void pending_activity_rejects_settings(bool repair) {
    fixture_present=true;fixture_tick=100000;fixture_async_autocomplete=true;
    fixture_send={};fixture_transmit={};fixture_incoming.clear();fixture_sent.clear();
    const auto boot=live_id<BootId>(3);
    amity::HubService owner;owner.set_boot_identity(boot);assert(owner.start(1,2,0x5000)==ESP_OK);
    assert(owner.configure({{"Watch","TV","Wanted","Source","",0}}));run_one_command();fixture_finish_discovery(owner);
    const auto boot_scan=owner.snapshot().discovery_generation;
    fixture_tick=0;fixture_time+=130000000;fixture_sent.clear();fixture_async_autocomplete=false;
    assert((repair?owner.repair_activity():owner.request_activity(0))==ESP_OK);run_one_command();run_idle(2);
    assert(owner.snapshot().current_activity==-1&&!fixture_pending.empty());
    if(repair){
        // Standby repair does not arm the user-input setup gate, but it still
        // owns an ordered activity sequence until its Standby frames finish.
        assert(!owner.activity_setup_in_progress());
    }else{
        assert(fixture_complete_next(cec::TxResult::Success,1));run_idle(2);
        assert(owner.activity_setup_in_progress());
        assert(std::any_of(fixture_pending.begin(),fixture_pending.end(),[](const auto& tx){return tx.gpio==2&&tx.frame.size==1;}));
    }
    const auto before=owner.snapshot();const auto address=owner.adapter_runtime_snapshot()[0];
    unsigned writes=0;
    assert(owner.commit_adapter_settings(boot,{{0,0x6000}},uint64_t(fixture_time)/1000+5000,[&]{++writes;return ESP_OK;})==ESP_ERR_INVALID_STATE);
    assert(!writes&&!owner.maintenance_ready());
    const auto after=owner.snapshot();const auto unchanged=owner.adapter_runtime_snapshot()[0];
    assert(after.current_activity==before.current_activity&&after.discovery_generation==before.discovery_generation&&after.devices.size()==before.devices.size());
    for(size_t n=0;n<before.devices.size();++n){const auto& old=before.devices[n];const auto& current=after.devices[n];
        assert(old.name==current.name&&old.bus==current.bus&&old.address==current.address&&old.physical_address==current.physical_address);
    }
    assert(unchanged.effective_front_physical==address.effective_front_physical&&unchanged.selected_from_default==address.selected_from_default);
    fixture_complete_all();fixture_async_autocomplete=true;
    if(!repair)fixture_finish_discovery(owner,boot_scan);
    run_idle(32);
    assert(owner.snapshot().current_activity==(repair?-1:0)&&!owner.activity_setup_in_progress()&&fixture_pending.empty());
    if(!repair){assert(owner.standby());run_one_command();run_idle(32);}
    assert(owner.commit_adapter_settings(boot,{{0,0x6000}},uint64_t(fixture_time)/1000+5000,[&]{++writes;return ESP_OK;})==ESP_OK&&writes==1);
    fixture_tick=100000;
}
int main(){adapter_settings_owner_tests();pending_activity_rejects_settings(false);pending_activity_rejects_settings(true);std::cout<<"PASS per-adapter settings owner: exact boot, configured sparse wires without CEC responses, activity/repair busy fences, quiescence, bounded deadline and durable persistence\n";}
