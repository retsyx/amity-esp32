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
        assert(owner.bind_owned_controller(live_id<OctopusId>(2),1)==ESP_OK);run_idle(2);
        const auto slots = owner.adapter_runtime_snapshot(); assert(slots.size() == 4);
        for (unsigned i=0;i<4;++i) assert(slots[i].tentacle_id == i && slots[i].configured == (i==0));
        assert(!slots[1].effective_front_physical && !slots[0].observed_front_physical);
        unsigned writes = 0;
        assert(owner.commit_adapter_settings(live_id<BootId>(4), {0}, deadline(), [&]{++writes;return ESP_OK;}) == ESP_ERR_INVALID_STATE);
        assert(owner.commit_adapter_settings(boot, {0}, deadline(), [&]{++writes;return ESP_FAIL;}) == ESP_FAIL);
        assert(writes == 1 && !owner.maintenance_ready()); // Failed persistence restores fresh admission.
        assert(owner.commit_adapter_settings(boot, {0}, deadline(), [&]{
            ++writes; fixture_time += 6000000; fixture_late_reply = true; return ESP_OK; // A started bounded flash write may cross the request deadline.
        }) == ESP_OK);
        assert(writes == 2 && owner.maintenance_ready() && !fixture_late_reply);
        assert(owner.commit_adapter_settings(boot, {0}, deadline(), [&]{++writes;return ESP_OK;}) == ESP_ERR_INVALID_STATE);
        assert(writes == 2); // Durable success stays quiescent for its acknowledged restart.
        assert(owner.commit_adapter_settings(boot, {4}, deadline(), [&]{++writes;return ESP_OK;}) == ESP_ERR_INVALID_ARG);
        assert(owner.commit_adapter_settings(boot, {0,0}, deadline(), [&]{++writes;return ESP_OK;}) == ESP_ERR_INVALID_ARG);
    }
    auto sparse = cec::legacy_buttonless_profile; sparse.id = cec::BoardProfileId::Development4Tentacle; sparse.tentacle_count = 4;
    for(unsigned i=0;i<8;++i)sparse.ports[i]={static_cast<uint8_t>(i/2),i%2?cec::Side::Back:cec::Side::Front,-1};
    {
        amity::HubService owner; setup(owner); assert(owner.start(sparse)==ESP_OK);assert(owner.bind_owned_controller(live_id<OctopusId>(2),1)==ESP_OK);run_idle(2);
        for(const auto& slot:owner.adapter_runtime_snapshot())assert(!slot.configured&&!slot.effective_front_physical);
        unsigned writes=0;assert(owner.commit_adapter_settings(boot,{3},deadline(),[&]{++writes;return ESP_OK;})==ESP_ERR_INVALID_STATE);assert(!writes);
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
        assert(owner.commit_adapter_settings(boot,{3},too_short,[&]{++writes;return ESP_OK;})==ESP_ERR_TIMEOUT);run_idle(3);assert(!writes);
        assert(owner.commit_adapter_settings(boot,{3},deadline(),[&]{++writes;return ESP_OK;})==ESP_OK);assert(writes==1);
    }
    fixture_tick=100000;fixture_present=true;
}
int main(){adapter_settings_owner_tests();std::cout<<"PASS per-adapter settings owner: exact boot, configured sparse wires without CEC responses, quiescence, bounded deadline and durable persistence\n";}
