// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#include "cec_hub_fixture.hpp"
using namespace amity;
namespace m=amity::system;
static const auto group=*m::ConsortiumId::parse("11111111111111111111111111111111");
static const auto controller=*m::OctopusId::parse("22222222222222222222222222222222");
static const auto member=*m::OctopusId::parse("33333333333333333333333333333333");
static const auto boot=*m::BootId::parse("44444444444444444444444444444444");
static const auto controller_boot=*m::BootId::parse("55555555555555555555555555555555");
static const auto link=*m::LinkId::parse("66666666666666666666666666666666");
static m::QualifiedDeviceName named(m::OctopusId owner,m::Side side,const char* name,uint8_t adapter=0){return {{group,owner,adapter,side},name};}
static std::vector<SentFrame> select_trace(bool scoped){
    fixture_present=true;
    HubService hub;
    if(scoped){hub.set_system_identity(group,controller);hub.set_boot_identity(boot);}
    assert(hub.start(1,2)==ESP_OK);
    std::vector<Activity> activities{{"Watch",scoped?"display":"TV",scoped?"source":"Source",scoped?"source":"Source","",0}};
    if(scoped){
        assert(hub.configure_scoped(activities,{{"display",named(controller,m::Side::front,"TV")},{"source",named(controller,m::Side::back,"Source")}},
            [](const auto&){assert(false&&"Local equipment must use the same direct owner entry");return false;},controller,1)==ESP_OK);
    }else assert(hub.configure(activities));
    run_one_command();fixture_finish_discovery(hub);
    fixture_sent.clear();assert(hub.select_activity(0));run_one_command();
    assert(hub.snapshot().current_activity==0);
    auto trace=fixture_sent;
    fixture_sent.clear();assert(hub.press_key(0x41,true));run_one_command();
    assert(hub.release_key(0x41));run_one_command();
    return trace;
}
static void local_parity(){
    const auto baseline=select_trace(false),owned=select_trace(true);
    assert(baseline.size()==owned.size());
    for(size_t i=0;i<baseline.size();++i){assert(baseline[i].gpio==owned[i].gpio);assert(baseline[i].frame.size==owned[i].frame.size);assert(baseline[i].attempts==owned[i].attempts);assert(std::memcmp(baseline[i].frame.data,owned[i].frame.data,baseline[i].frame.size)==0);}
}
static void first_matching_osd(){
    struct Recorder:amity::Transport{
        struct Item{BusSide side;uint8_t address;std::vector<uint8_t> bytes;};std::vector<Item> sent;
        bool send(BusSide side,uint8_t address,const uint8_t* bytes,size_t size)override{sent.push_back({side,address,{bytes,bytes+size}});return true;}
        void request_scan()override{}
    } original_wire,owner_wire;
    Hub original(original_wire),owner(owner_wire);original.set_addresses(4,0);owner.set_addresses(4,0);owner.qualified_names();
    for(auto* hub:{&original,&owner}){
        hub->configure({{"Watch","","TV","TV","",0}});
        hub->discovered(BusSide::Back,4);
        hub->receive(BusSide::Back,cec::Frame{{0x40,0x47,'T','V'},4});
        // A later unresponsive TV poll creates the original fallback record,
        // while the earlier appliance still has the same OSD name.
        hub->discovered(BusSide::Back,0);
        hub->discovered(BusSide::Front,0);
    }
    original_wire.sent.clear();owner_wire.sent.clear();assert(original.select_activity(0));
    m::OwnedIntent intent;intent.kind=m::OwnedIntentKind::power;intent.target=named(member,m::Side::back,"TV");intent.on=true;
    assert(owner.owned_target_available(intent.target));assert(owner.execute_owned(intent));
    assert(!original_wire.sent.empty()&&!owner_wire.sent.empty());
    assert(original_wire.sent.front().address==4&&owner_wire.sent.front().address==4);
    assert(original_wire.sent.front().bytes==owner_wire.sent.front().bytes);
    owner_wire.sent.clear();intent.target.port.side=m::Side::front;assert(owner.execute_owned(intent));
    assert(owner_wire.sent.front().side==BusSide::Front&&owner_wire.sent.front().address==0);

    fixture_present=true;HubService controller_hub;controller_hub.set_system_identity(group,controller);controller_hub.set_boot_identity(boot);assert(controller_hub.start(1,2)==ESP_OK);
    assert(controller_hub.configure_scoped({{"Watch","display","source","source","",0}},{{"display",named(controller,m::Side::front,"TV")},{"source",named(member,m::Side::back,"TV")}},[](const auto&){return true;},controller,1)==ESP_OK);run_one_command();fixture_finish_discovery(controller_hub);
    m::OwnedAdapterSnapshot report;report.owner=member;report.boot=boot;report.ready=true;
    report.devices={{m::Side::back,"TV",4},{m::Side::back,"TV",8},{m::Side::front,"TV",0}};
    assert(controller_hub.update_owned_snapshot(report));run_idle(1);
    const auto state=controller_hub.snapshot();const auto found=std::find_if(state.devices.begin(),state.devices.end(),[](const auto& device){return device.name=="source";});
    assert(found!=state.devices.end()&&found->bus==BusSide::Back&&found->address==4);
}
static std::vector<SentFrame> startup_trace(bool scoped){
    fixture_present=true;HubService hub;
    if(scoped){hub.set_system_identity(group,controller);hub.set_boot_identity(boot);}
    assert(hub.start(1,2)==ESP_OK);
    if(scoped)assert(hub.configure_scoped({{"Watch","display","source","source","",0}},{{"display",named(controller,m::Side::front,"TV")},{"source",named(controller,m::Side::back,"Source")}},{},controller,1)==ESP_OK);
    else assert(hub.configure({{"Watch","TV","Source","Source","",0}}));
    run_one_command();fixture_sent.clear();
    assert(hub.select_activity(0));assert(hub.press_key(4,true));assert(hub.release_key(4));
    run_one_command();run_one_command();run_one_command();fixture_finish_discovery(hub);run_idle(3);
    const auto sent=fixture_sent;
    const auto key=std::count_if(sent.begin(),sent.end(),[](const auto& frame){return frame.frame.size==3&&frame.frame.data[1]==0x44&&frame.frame.data[2]==4;});
    assert(key==0);return sent;
}
static void startup_parity(){
    auto select_frames=[](const auto& sent){std::vector<SentFrame> result;for(const auto& f:sent)if(f.frame.size>1&&(f.frame.data[1]==0x44||f.frame.data[1]==0x45||f.frame.data[1]==0x86||f.frame.data[1]==0x82||f.frame.data[1]==0x04||f.frame.data[1]==0x71))result.push_back(f);return result;};
    const auto baseline=select_frames(startup_trace(false)),owned=select_frames(startup_trace(true));assert(baseline.size()==owned.size());
    for(size_t i=0;i<baseline.size();++i){const auto& a=baseline[i];const auto& b=owned[i];assert(a.gpio==b.gpio&&a.attempts==b.attempts&&a.frame.size==b.frame.size&&std::memcmp(a.frame.data,b.frame.data,a.frame.size)==0);}
}
static void observation_pressure(){
    fixture_present=true;HubService hub;hub.set_system_identity(group,controller);hub.set_boot_identity(boot);assert(hub.start(1,2)==ESP_OK);
    assert(hub.configure_scoped({{"Watch","display","source","source","",0}},{{"display",named(controller,m::Side::front,"TV")},{"source",named(controller,m::Side::back,"Source")}},{},controller,1)==ESP_OK);run_one_command();fixture_finish_discovery(hub);
    for(unsigned n=0;n<32;++n)assert(hub.press_key(4));
    m::OwnedAdapterSnapshot report;report.owner=member;report.boot=boot;report.ready=true;
    for(unsigned n=0;n<100;++n){report.setup_last_sequence=n;report.setup_pending=100-n;assert(hub.update_owned_snapshot(report));}
    report.setup_pending=0;report.setup_replies_pending=false;assert(hub.update_owned_snapshot(report));
    run_one_command();const auto reports=hub.owned_snapshots();const auto found=std::find_if(reports.begin(),reports.end(),[](const auto& state){return state.owner==member;});assert(found!=reports.end()&&found->setup_last_sequence==99&&found->setup_pending==0&&!found->setup_replies_pending);
    for(unsigned n=1;n<32;++n)run_one_command();
}
static void owner_keys(){
    fixture_present=true;HubService hub;hub.set_system_identity(group,member);hub.set_boot_identity(boot);hub.set_coordinator_authority(false,1);assert(hub.start(1,2)==ESP_OK);
    assert(hub.bind_owned_controller(controller,1)==ESP_OK);fixture_finish_discovery(hub);
    bool authority=true;hub.set_owned_authorizer([&](const auto&){return authority;});
    m::OwnedAuthorization auth{controller,controller_boot,boot,link,1,0};
    uint64_t sequence=0;
    auto issue=[&](m::OwnedIntent intent){intent.sequence=++sequence;if(!intent.generation)intent.generation=1;auto scoped_auth=auth;scoped_auth.authority=intent.kind==m::OwnedIntentKind::key_up?m::OwnedAuthority::cleanup:intent.kind==m::OwnedIntentKind::key_down?m::OwnedAuthority::realtime:m::OwnedAuthority::control;return hub.submit_owned(intent,scoped_auth);};
    m::OwnedIntent context;context.kind=m::OwnedIntentKind::context;context.target.port={group,member,0,m::Side::front};context.selected=true;context.source=context.audio=named(member,m::Side::back,"Source");assert(issue(context));
    auto down=[&](uint8_t key,char digit){m::OwnedIntent intent;intent.kind=m::OwnedIntentKind::key_down;intent.target=named(member,m::Side::back,"Source");intent.key=key;intent.repeat=true;intent.token=*m::HoldToken::parse(std::string(32,digit));return intent;};
    auto right=down(4,'7'),volume=down(0x41,'8');
    fixture_async_autocomplete=false;fixture_sent.clear();
    assert(issue(right));assert(issue(volume));
    assert(std::count_if(fixture_sent.begin(),fixture_sent.end(),[](const auto& f){return f.frame.size>=3&&f.frame.data[1]==0x44;})==2);
    auto up=right;up.kind=m::OwnedIntentKind::key_up;assert(issue(up));
    assert(std::none_of(fixture_sent.begin(),fixture_sent.end(),[](const auto& f){return f.frame.size==2&&f.frame.data[1]==0x45;}));
    fixture_complete_all(cec::TxResult::Nack);run_idle(1);
    const auto count=fixture_sent.size();assert(issue(volume));assert(fixture_sent.size()==count);
    up=volume;up.kind=m::OwnedIntentKind::key_up;assert(issue(up));assert(fixture_sent.size()==count);
    auto wrong=auth;wrong.owner_boot=controller_boot;auto bad=down(3,'9');bad.sequence=++sequence;bad.generation=1;assert(!hub.submit_owned(bad,wrong));
    authority=false;bad.sequence=++sequence;assert(!hub.submit_owned(bad,auth));authority=true;
    fixture_async_autocomplete=true;fixture_complete_all();run_idle(2);
}
static void rejected_local_hold(){
    fixture_present=true;
    cec::BoardProfile profile{cec::BoardProfileId::Development4Tentacle,1,amity::max_tentacles,
        {{0,cec::Side::Front,1},{0,cec::Side::Back,2},{1,cec::Side::Front,5},{1,cec::Side::Back,6},
         {2,cec::Side::Front,7},{2,cec::Side::Back,8},{3,cec::Side::Front,9},{3,cec::Side::Back,44}},-1,-1};
    profile.presence[0]={20,true,false,false};int detector=1;
    HubService hub;hub.set_system_identity(group,controller);hub.set_boot_identity(boot);
    assert(hub.set_presence_reader([&](int){return detector;})==ESP_OK);assert(hub.start(profile)==ESP_OK);
    assert(hub.configure_scoped({{"Watch","display","source","source","",0}},
        {{"display",named(controller,m::Side::front,"TV")},{"source",named(controller,m::Side::back,"Source")}},{},controller,1)==ESP_OK);
    run_one_command();fixture_finish_discovery(hub);assert(hub.select_activity(0));run_one_command();
    fixture_async_autocomplete=false;
    detector=0;run_idle(1);fixture_sent.clear();
    assert(hub.press_key(0x41,true));run_one_command();
    assert(hub.release_key(0x41));run_one_command();
    assert(fixture_sent.empty());
    // Rejected local admission has no consumed hold or cleanup token. The
    // returning adapter must receive the very next physical down, once.
    detector=1;run_idle(1);assert(fixture_sent.empty());
    assert(hub.press_key(0x41,true));run_one_command();
    const auto presses=[](){return std::count_if(fixture_sent.begin(),fixture_sent.end(),[](const auto& f){
        return f.gpio==2&&f.frame.size==3&&f.frame.data[1]==0x44&&f.frame.data[2]==0x41;});};
    assert(presses()==1);
    assert(hub.press_key(0x41,true));run_one_command();assert(presses()==1);
    // A consumed down still retains its token on a wire failure: unchanged
    // down reports cannot restart it, and the eventual up emits no new TX.
    fixture_complete_all(cec::TxResult::Nack);run_idle(1);
    const auto failed_count=fixture_sent.size();
    assert(hub.press_key(0x41,true));run_one_command();assert(fixture_sent.size()==failed_count);
    assert(hub.release_key(0x41));run_one_command();assert(fixture_sent.size()==failed_count);
    assert(hub.press_key(0x41,true));run_one_command();assert(presses()==2);
    assert(hub.release_key(0x41));run_one_command();fixture_complete_all();fixture_async_autocomplete=true;
}
static void every_adapter(){
    fixture_present=true;
    fixture_peer_address=[](int gpio)->uint8_t{return gpio==1||gpio==5||gpio==7||gpio==9?0:4;};
    cec::BoardProfile profile{cec::BoardProfileId::Development4Tentacle,1,amity::max_tentacles,
        {{0,cec::Side::Front,1},{0,cec::Side::Back,2},{1,cec::Side::Front,5},{1,cec::Side::Back,6},{2,cec::Side::Front,7},{2,cec::Side::Back,8},{3,cec::Side::Front,9},{3,cec::Side::Back,44}},-1,-1};
    HubService hub;hub.set_system_identity(group,member);hub.set_boot_identity(boot);hub.set_coordinator_authority(false,1);assert(hub.start(profile)==ESP_OK);assert(hub.bind_owned_controller(controller,1)==ESP_OK);
    bool complete=false;
    for(unsigned n=0;n<2500&&!complete;++n){run_idle(1);const auto reports=hub.owned_snapshots();complete=reports.size()==amity::max_tentacles&&std::all_of(reports.begin(),reports.end(),[](const auto& r){return r.discovery_generation&& !r.discovering&&r.front_physical_address==0x1000&&r.front_address<15&&r.back_address<15&&r.devices.size()==2;});}
    if(!complete)for(const auto& r:hub.owned_snapshots())std::fprintf(stderr,"adapter=%u gen=%u scan=%d pa=%04x front=%u back=%u devices=%zu\n",r.adapter,r.discovery_generation,r.discovering,r.front_physical_address,r.front_address,r.back_address,r.devices.size());
    assert(complete);fixture_peer_address={};
}

// The original 98e64a6 Hub falls back to the configured switch when the named
// source is absent, and resolves it again after later discovery. No saved
// executable/approval state participates in either local or remote execution.
static std::vector<std::vector<SentFrame>> receiver_transcript(bool remote){
    fixture_present=true;int player=-1;
    const auto is_front=[](int gpio){return gpio==1||gpio==3;};
    fixture_transmit=[&](int gpio,const cec::Frame& frame){const bool found=is_front(gpio)?frame.destination()==0:frame.destination()==4||frame.destination()==player;return found?cec::TxResult::Success:cec::TxResult::Nack;};
    fixture_send=[&](int gpio,const cec::Frame& frame){
        if(frame.size<2)return true;
        const auto address=frame.destination();const bool front=is_front(gpio);
        if(front?address!=0:address!=4&&address!=player)return true;
        cec::Frame reply;reply.data[0]=(address<<4)|(frame.source());
        switch(frame.data[1]){
        case 0x83:reply.size=5;reply.data[0]=(address<<4)|15;reply.data[1]=0x84;reply.data[2]=front?0:address==4?0x10:0x20;reply.data[3]=0;reply.data[4]=front?0:address==4?5:4;break;
        case 0x46:{reply.size=2;reply.data[1]=0x47;const char* text=front?"TV":address==4?"Receiver":"Player";while(*text)reply.data[reply.size++]=*text++;break;}
        case 0x8c:reply.size=5;reply.data[0]=(address<<4)|15;reply.data[1]=0x87;reply.data[2]=address==4?1:0;reply.data[3]=address==4?2:0;reply.data[4]=address==4?3:0;break;
        case 0x71:reply.size=3;reply.data[1]=0x7a;reply.data[2]=50;break;
        default:return true;
        }
        fixture_incoming.push_back({gpio,reply});return true;
    };
    HubService primary;FakeTask* primary_task;
    if(remote){primary.set_system_identity(group,controller);primary.set_boot_identity(boot);}
    assert(primary.start(1,2)==ESP_OK);primary_task=latest_task;
    auto other=std::unique_ptr<HubService>{};FakeTask* other_task=nullptr;
    const auto other_boot=*m::BootId::parse("77777777777777777777777777777777");
    if(remote){
        other=std::make_unique<HubService>();other->set_system_identity(group,member);other->set_boot_identity(other_boot);other->set_coordinator_authority(false,1);assert(other->start(3,4)==ESP_OK);other_task=latest_task;
        assert(other->bind_owned_controller(controller,1)==ESP_OK);
        other->set_owned_observer([&](const auto& report){primary.update_owned_snapshot(report);});
        other->set_owned_input_observer([&](const auto& intent,const auto&,const auto& result){if(result.source_present)assert(primary.owned_input_result(member,intent.generation,intent.sequence,*result.source_present));});
        latest_task=primary_task;
        assert(primary.configure_scoped({{"Watch","display","player","receiver","receiver",3}},
            {{"display",named(controller,m::Side::front,"TV")},{"player",named(member,m::Side::back,"Player")},{"receiver",named(member,m::Side::back,"Receiver")}},
            [&](const m::OwnedIntent& intent){
                assert(intent.target.port.octopus_id==member);auto* previous=latest_task;latest_task=other_task;
                m::OwnedAuthorization auth{controller,boot,other_boot,link,1,0};auth.authority=intent.kind==m::OwnedIntentKind::key_up?m::OwnedAuthority::cleanup:intent.kind==m::OwnedIntentKind::key_down?m::OwnedAuthority::realtime:m::OwnedAuthority::control;const auto admitted=other->submit_owned(intent,auth);latest_task=previous;
                if(intent.kind==m::OwnedIntentKind::source_input&&admitted.source_present)assert(primary.owned_input_result(member,intent.generation,intent.sequence,*admitted.source_present));
                return admitted.code==m::AdmissionCode::accepted||admitted.code==m::AdmissionCode::pending||admitted.code==m::AdmissionCode::overloaded||admitted.code==m::AdmissionCode::unresolved;
            },controller,1)==ESP_OK);
    }else assert(primary.configure({{"Watch","TV","Player","Receiver","Receiver",3}}));
    latest_task=primary_task;run_one_command();
    auto drain=[&](uint32_t previous){
        for(unsigned n=0;n<2500;++n){
            if(remote){latest_task=other_task;run_idle(1);}
            latest_task=primary_task;run_idle(1);
            const auto state=primary.snapshot();
            bool done=state.discovery_generation>previous&&!state.discovering;
            if(remote){const auto reports=primary.owned_snapshots();const auto found=std::find_if(reports.begin(),reports.end(),[&](const auto& report){return report.owner==member;});done=done&&found!=reports.end()&&found->discovery_generation>previous&&!found->discovering;}
            if(done)return;
        }
        assert(false&&"Original discovery did not complete on both owners");
    };
    drain(0);std::vector<std::vector<SentFrame>> transcript;
    for(int phase=0;phase<3;++phase){
        if(phase){player=phase==1?8:11;const auto previous=primary.snapshot().discovery_generation;latest_task=primary_task;assert(primary.discover());run_one_command();drain(previous);}
        // The coordinator's remote report can be missing while the emitting
        // owner still knows its devices. This never vetoes power/key intents.
        if(remote)for(auto report:primary.owned_snapshots())if(report.owner==member){report.devices.clear();report.ready=false;primary.update_owned_snapshot(report);latest_task=primary_task;run_idle(1);}
        fixture_sent.clear();latest_task=primary_task;assert(primary.select_activity(0));run_one_command();run_idle(1);
        auto frames=fixture_sent;
        for(auto& f:frames)if(remote){assert(f.gpio==1||f.gpio==4);if(f.gpio==4)f.gpio=2;}
        const bool switched=std::any_of(frames.begin(),frames.end(),[](const auto& f){return f.frame.size==4&&f.frame.data[1]==0x44&&f.frame.data[2]==0x69&&f.frame.data[3]==3;});
        const bool source_input=std::any_of(frames.begin(),frames.end(),[](const auto& f){return f.frame.size==4&&f.frame.data[1]==0x86&&f.frame.data[2]==0x20;});
        assert(phase?source_input&&!switched:switched&&!source_input);
        transcript.push_back(std::move(frames));
        if(phase==0){
            fixture_sent.clear();latest_task=primary_task;assert(primary.select_activity(0));assert(primary.select_activity(0));run_one_command();run_one_command();run_idle(2);
            auto repairs=fixture_sent;for(auto& f:repairs)if(remote){assert(f.gpio==1||f.gpio==4);if(f.gpio==4)f.gpio=2;}
            assert(std::count_if(repairs.begin(),repairs.end(),[](const auto& f){return f.frame.size==4&&f.frame.data[1]==0x44&&f.frame.data[2]==0x69;})==2);
            transcript.push_back(std::move(repairs));
        }
    }
    latest_task=primary_task;fixture_send={};fixture_transmit={};return transcript;
}
static void remote_receiver_parity(){
    const auto local=receiver_transcript(false),remote=receiver_transcript(true);assert(local.size()==remote.size());
    for(size_t phase=0;phase<local.size();++phase){if(local[phase].size()!=remote[phase].size()){for(const auto* trace:{&local[phase],&remote[phase]}){std::fprintf(stderr,"phase=%zu trace=%zu\n",phase,trace->size());for(const auto& f:*trace){std::fprintf(stderr,"gpio%d attempts%u:",f.gpio,f.attempts);for(unsigned j=0;j<f.frame.size;++j)std::fprintf(stderr," %02x",f.frame.data[j]);std::fprintf(stderr,"\n");}}}assert(local[phase].size()==remote[phase].size());for(size_t i=0;i<local[phase].size();++i){const auto& a=local[phase][i];const auto& b=remote[phase][i];assert(a.gpio==b.gpio&&a.attempts==b.attempts&&a.frame.size==b.frame.size);assert(std::memcmp(a.frame.data,b.frame.data,a.frame.size)==0);}}
}
int main(){local_parity();first_matching_osd();startup_parity();observation_pressure();owner_keys();rejected_local_hold();every_adapter();remote_receiver_parity();std::puts("Original local/owned frame parity, first OSD match, startup taps, observation pressure, rejected local hold/reconnect, independent owner keys, authorization, and all-adapter discovery passed");}
