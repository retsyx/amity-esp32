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
        LocalHubService owner; setup(owner);
        assert(owner.start(cec::legacy_buttonless_profile, std::array<uint16_t,4>{{0x2000,0x3000,0x4000,0x5000}}) == ESP_OK);
        assert(owner.bind_owned_controller(live_id<OctopusId>(2),1,boot)==ESP_OK);fixture_finish_discovery(owner);
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
        assert(applied[0].effective_front_physical == 0x6000);
        assert(!applied[0].observed_front_physical);
        assert(owner.snapshot().discovery_generation == initial.discovery_generation);
        // A fresh same-boot save and normal work remain available immediately.
        assert(owner.commit_adapter_settings(boot, {{0,0x7000}}, deadline(), [&]{++writes;return ESP_OK;}) == ESP_OK);
        assert(writes == 3 && owner.adapter_runtime_snapshot()[0].effective_front_physical == 0x7000);
        assert(fixture_configure(owner,{})); run_one_command();
        assert(owner.commit_adapter_settings(boot, {{4,0x6000}}, deadline(), [&]{++writes;return ESP_OK;}) == ESP_ERR_INVALID_ARG);
        assert(owner.commit_adapter_settings(boot, {{0,0x6000},{0,0x5000}}, deadline(), [&]{++writes;return ESP_OK;}) == ESP_ERR_INVALID_ARG);
    }
    auto sparse = cec::legacy_buttonless_profile; sparse.id = cec::BoardProfileId::Development4Tentacle; sparse.tentacle_count = 4;
    for(unsigned i=0;i<8;++i)sparse.ports[i]={static_cast<uint8_t>(i/2),i%2?cec::Side::Back:cec::Side::Front,-1};
    {
        LocalHubService owner; setup(owner); assert(fixture_start(owner,sparse)==ESP_OK);assert(owner.bind_owned_controller(live_id<OctopusId>(2),1,boot)==ESP_OK);run_idle(2);
        for(const auto& slot:owner.adapter_runtime_snapshot())assert(!slot.configured&&!slot.effective_front_physical);
        unsigned writes=0;assert(owner.commit_adapter_settings(boot,{{3,0x5200}},deadline(),[&]{++writes;return ESP_OK;})==ESP_ERR_INVALID_STATE);assert(!writes);
    }
    sparse.ports[6].gpio=9;sparse.ports[7].gpio=44;
    {
        LocalHubService owner;setup(owner);
        assert(owner.start(sparse,std::array<uint16_t,4>{{0x1000,0x2000,0x3000,0x4000}})==ESP_OK);
        assert(owner.bind_owned_controller(live_id<OctopusId>(2),1,boot)==ESP_OK);run_idle(2);
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
        assert(applied.effective_front_physical==0x5200&&!applied.observed_front_physical);
        // An explicit scan reaches every configured local adapter, including
        // one that no saved activity references.
        const auto report=[&]{for(const auto& r:owner.owned_snapshots())if(r.adapter==3)return r;assert(false&&"Adapter report missing");return OwnedAdapterSnapshot{};};
        for(unsigned pass=0;pass<2000&&report().discovering;++pass)run_idle(1);
        const auto scanned=report().discovery_generation;assert(!report().discovering);
        assert(owner.discover());run_one_command();
        for(unsigned pass=0;pass<2000&&report().discovery_generation==scanned;++pass)run_idle(1);
        assert(report().discovery_generation>scanned);
    }
    fixture_tick=100000;fixture_present=true;
}

static void pending_activity_rejects_settings(bool repair) {
    fixture_present=true;fixture_tick=100000;fixture_async_autocomplete=true;
    fixture_send={};fixture_transmit={};fixture_incoming.clear();fixture_sent.clear();
    const auto boot=live_id<BootId>(3);
    LocalHubService owner;owner.set_boot_identity(boot);assert(fixture_start(owner,1,2,0x5000)==ESP_OK);
    assert(fixture_configure(owner,{{"Watch","TV","Wanted","Source","",0}}));run_one_command();fixture_finish_discovery(owner);
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
    assert(unchanged.effective_front_physical==address.effective_front_physical&&unchanged.observed_front_physical==address.observed_front_physical);
    fixture_complete_all();fixture_async_autocomplete=true;
    if(!repair)fixture_finish_discovery(owner,boot_scan);
    run_idle(32);
    assert(owner.snapshot().current_activity==(repair?-1:0)&&!owner.activity_setup_in_progress()&&fixture_pending.empty());
    if(!repair){assert(owner.standby());run_one_command();run_idle(32);}
    assert(owner.commit_adapter_settings(boot,{{0,0x6000}},uint64_t(fixture_time)/1000+5000,[&]{++writes;return ESP_OK;})==ESP_OK&&writes==1);
    fixture_tick=100000;
}
static void passive_start_can_become_main() {
    fixture_present=true;fixture_tick=100000;fixture_async_autocomplete=true;
    fixture_send={};fixture_transmit={};fixture_incoming.clear();fixture_sent.clear();
    LocalHubService owner;owner.set_coordinator_authority(false);
    assert(fixture_start(owner,1,2)==ESP_OK);
    assert(owner.transition_coordinator_authority(true)==ESP_OK);
    assert(fixture_configure(owner,{{"Watch","TV","Source","Source","",0}}));
    run_one_command();fixture_finish_discovery(owner);
    assert(fixture_select(owner,0));run_one_command();
    for(unsigned n=0;n<100&&owner.snapshot().activity_settling;++n)run_idle(1);
    assert(owner.snapshot().current_activity==0&&!owner.snapshot().activity_settling);
    fixture_tick=0;fixture_sent.clear();
    assert(owner.press_key(4,true));run_one_command();
    assert(owner.release_key(4));run_one_command();
    const auto press=std::find_if(fixture_sent.begin(),fixture_sent.end(),[](const auto& tx){
        return tx.gpio==2&&tx.frame.destination()==4&&tx.frame.size==3&&tx.frame.data[1]==0x44&&tx.frame.data[2]==4;
    });
    assert(press!=fixture_sent.end());
    assert(std::any_of(press,fixture_sent.end(),[](const auto& tx){
        return tx.gpio==2&&tx.frame.destination()==4&&tx.frame.size==2&&tx.frame.data[1]==0x45;
    }));
    fixture_tick=100000;
}
// The development carrier's four configured adapters. Only adapter 0 has an
// HDMI adapter attached. Lines 1-3 never go high: every poll there ends at the
// CEC engine's line timeout, no address is claimed, and between scans those
// adapters keep repeating claim-only cycles.
static const int carrier_gpios[8]={1,2,5,6,7,8,9,44};
static void start_carrier(amity::HubService& owner){
    fixture_present=true;fixture_tick=100000;fixture_async_autocomplete=true;
    fixture_send={};fixture_incoming.clear();fixture_sent.clear();
    fixture_peer_address=[](int gpio)->uint8_t{return gpio==1?0:gpio==2?4:15;};
    fixture_transmit=[](int gpio,const cec::Frame& frame){
        if(gpio!=1&&gpio!=2)return cec::TxResult::Timeout;
        return frame.destination()==fixture_peer(gpio)?cec::TxResult::Success:cec::TxResult::Nack;
    };
    cec::BoardProfile profile{cec::BoardProfileId::Development4Tentacle,1,amity::max_tentacles,{},-1,-1};
    for(unsigned i=0;i<8;++i)profile.ports[i]={static_cast<uint8_t>(i/2),i%2?cec::Side::Back:cec::Side::Front,carrier_gpios[i]};
    assert(owner.start(profile,std::array<uint16_t,4>{{0x1000,0x2000,0x3000,0x4000}})==ESP_OK);
}
static std::array<OwnedAdapterSnapshot,4> carrier_reports(const amity::HubService& owner){
    std::array<OwnedAdapterSnapshot,4> out{};unsigned found=0;
    for(const auto& r:owner.owned_snapshots())if(r.owner==fixture_local::owner()&&r.adapter<4){out[r.adapter]=r;++found;}
    assert(found==4&&"Every configured adapter reports");return out;
}
static bool claiming(uint8_t adapter){
    return std::any_of(fixture_pending.begin(),fixture_pending.end(),[&](const auto& p){
        return p.frame.size==1&&(p.gpio==carrier_gpios[2*adapter]||p.gpio==carrier_gpios[2*adapter+1]);});
}
static bool claiming(uint8_t adapter,cec::Side side){
    const int gpio=carrier_gpios[2*adapter+(side==cec::Side::Back)];
    return std::any_of(fixture_pending.begin(),fixture_pending.end(),[&](const auto& p){return p.frame.size==1&&p.gpio==gpio;});
}
// One owner pass: adapter 0's wire answers at once. An unwired line's pending
// poll ends at its timeout only when released.
static void advance_carrier(bool release_unwired){
    for(int gpio:{1,2})while(fixture_complete_next(cec::TxResult::Pending,gpio)){}
    if(release_unwired)for(int gpio:{5,6,7,8,9,44})fixture_complete_next(cec::TxResult::Timeout,gpio);
    fixture_time+=100000;run_idle(1);
}
// Boot scans every adapter. Then the unwired adapters start a claim-only cycle
// whose polls stay unresolved, as the engine holds each until its timeout.
static void hold_unwired_claims(amity::HubService& owner){
    fixture_finish_discovery(owner);
    for(const auto& r:carrier_reports(owner))assert(r.discovery_generation==1&&!r.discovering&&(r.adapter==0)==(r.back_address<15));
    fixture_tick=0;fixture_async_autocomplete=false;
    // Finish any claim cycle already under way, then wait for the next one.
    for(unsigned pass=0;pass<100;++pass){advance_carrier(true);if(!claiming(1)&&!claiming(2)&&!claiming(3))break;}
    for(unsigned pass=0;pass<100&&!(claiming(1)&&claiming(2)&&claiming(3));++pass)advance_carrier(false);
    assert(claiming(1,cec::Side::Back)&&claiming(2,cec::Side::Back)&&claiming(3,cec::Side::Back)&&!claiming(0));
    // A claim-only cycle is not a scan. The owner pass that started this
    // cycle has published it.
    for(const auto& r:carrier_reports(owner))assert(!r.discovering);
    assert(!owner.snapshot().discovering);
}
static void release_carrier(){
    fixture_complete_all(cec::TxResult::Timeout);
    fixture_transmit={};fixture_peer_address={};fixture_async_autocomplete=true;fixture_tick=100000;
}
static void explicit_scan_reports_unwired_adapters_until_they_scan(){
    LocalHubService owner;start_carrier(owner);
    assert(owner.bind_owned_controller(fixture_local::owner(),1,fixture_local::id<BootId>(3))==ESP_OK);
    hold_unwired_claims(owner);
    const auto before=carrier_reports(owner);const auto aggregate=owner.snapshot().discovery_generation;
    // What the UI and the main read after the request and after every later owner pass.
    struct Sample{bool discovering;std::array<bool,4> adapter_discovering,scanned;};
    std::vector<Sample> samples;
    const auto sample=[&]{
        Sample s{owner.snapshot().discovering,{},{}};const auto now=carrier_reports(owner);
        for(unsigned a=0;a<4;++a){s.adapter_discovering[a]=now[a].discovering;s.scanned[a]=now[a].discovery_generation!=before[a].discovery_generation;}
        samples.push_back(s);return s;
    };
    assert(owner.discover());run_one_command();
    // Adapter 0 scans first. The unwired adapters are still finishing their
    // claim cycle, so the requested scan has not started there yet.
    auto s=sample();
    for(unsigned pass=0;pass<4000&&!s.scanned[0];++pass){advance_carrier(false);s=sample();}
    assert(s.scanned[0]&&!s.scanned[1]&&!s.scanned[2]&&!s.scanned[3]);
    for(unsigned pass=0;pass<4000&&!(s.scanned[1]&&s.scanned[2]&&s.scanned[3]);++pass){advance_carrier(true);s=sample();}
    // The scan is busy from the request until the last adapter has scanned,
    // and never reports completion before that.
    for(const auto& each:samples)assert(each.discovering==!(each.scanned[0]&&each.scanned[1]&&each.scanned[2]&&each.scanned[3]));
    for(const auto& each:samples)for(unsigned a=0;a<4;++a)assert(each.adapter_discovering[a]==!each.scanned[a]);
    // One request scans each adapter once; later claim cycles scan nothing.
    for(const auto& r:carrier_reports(owner))assert(r.discovery_generation==before[r.adapter].discovery_generation+1);
    for(unsigned pass=0;pass<300;++pass){advance_carrier(true);assert(!owner.snapshot().discovering);}
    assert(owner.snapshot().discovery_generation==aggregate+4);
    release_carrier();
}
static void owner_reports_scan_requests_on_unwired_adapters(){
    const auto controller=live_id<OctopusId>(7);
    const OwnedAuthorization auth{controller,live_id<BootId>(8),live_id<BootId>(3),live_id<LinkId>(9),1,0};
    LocalHubService owner;owner.set_coordinator_authority(false);start_carrier(owner);
    assert(owner.bind_owned_controller(controller,1,auth.controller_boot)==ESP_OK);
    hold_unwired_claims(owner);
    const auto submit=[&](OwnedIntentKind kind,const char* name){
        OwnedIntent intent;intent.kind=kind;intent.on=true;intent.generation=1;
        intent.target={{fixture_local::group(),fixture_local::owner(),1,Side::back},name};
        assert(owner.submit_owned(intent,auth,0));run_one_command();
    };
    // Adapter 1's report to the main shows the requested scan from the
    // request until that scan has run. The other adapters show none.
    const auto scan_reported=[&](const std::array<OwnedAdapterSnapshot,4>& before){
        for(unsigned pass=0;pass<4000;++pass){
            const auto now=carrier_reports(owner);const bool done=now[1].discovery_generation!=before[1].discovery_generation;
            assert(now[1].discovering==!done&&owner.snapshot().discovering==!done);
            for(uint8_t a:{0,2,3})assert(!now[a].discovering&&now[a].discovery_generation==before[a].discovery_generation);
            if(done){assert(now[1].discovery_generation==before[1].discovery_generation+1);return;}
            advance_carrier(true);
        }
        assert(false&&"The requested scan did not finish");
    };
    // The main's explicit scan reaches adapter 1 during its claim cycle.
    const auto booted=carrier_reports(owner);
    submit(OwnedIntentKind::discover,"");scan_reported(booted);
    // A cache miss inside that side's 120 s cooldown neither scans nor
    // reports a scan.
    const auto scanned=carrier_reports(owner);submit(OwnedIntentKind::power,"Missing");
    for(unsigned pass=0;pass<20;++pass){
        for(const auto& r:carrier_reports(owner))assert(!r.discovering&&r.discovery_generation==scanned[r.adapter].discovery_generation);
        assert(!owner.snapshot().discovering);advance_carrier(true);
    }
    // After the cooldown a miss requests the lookup scan, again while a claim
    // cycle is finishing. Its retry after that scan is inside the new cooldown.
    fixture_time+=130000000;
    for(unsigned pass=0;pass<100&&!claiming(1);++pass)advance_carrier(false);
    assert(claiming(1));
    submit(OwnedIntentKind::power,"Missing");scan_reported(scanned);
    const auto looked_up=carrier_reports(owner);
    for(unsigned pass=0;pass<300;++pass){advance_carrier(true);assert(!owner.snapshot().discovering);}
    for(const auto& r:carrier_reports(owner))assert(r.discovery_generation==looked_up[r.adapter].discovery_generation);
    release_carrier();
}
int main(){passive_start_can_become_main();adapter_settings_owner_tests();pending_activity_rejects_settings(false);pending_activity_rejects_settings(true);
    explicit_scan_reports_unwired_adapters_until_they_scan();owner_reports_scan_requests_on_unwired_adapters();
    std::cout<<"PASS per-adapter settings owner: exact boot, configured sparse wires without CEC responses, activity/repair busy fences, quiescence, bounded deadline, durable persistence and scan status until every requested adapter has scanned\n";}
