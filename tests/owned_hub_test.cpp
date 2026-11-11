// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#include "cec_hub_fixture.hpp"
static bool measure_request_allocations=false;
static size_t request_allocations=0,largest_request_allocation=0;
void* operator new(size_t size,const std::nothrow_t&)noexcept{
    if(measure_request_allocations){++request_allocations;largest_request_allocation=std::max(largest_request_allocation,size);}
    try{return ::operator new(size);}catch(...){return nullptr;}
}
void operator delete(void* memory,const std::nothrow_t&)noexcept{::operator delete(memory);}
using namespace amity;
namespace m=amity::system;
static const auto group=*m::ConsortiumId::parse("11111111111111111111111111111111");
static const auto controller=*m::OctopusId::parse("22222222222222222222222222222222");
static const auto member=*m::OctopusId::parse("33333333333333333333333333333333");
static const auto boot=*m::BootId::parse("44444444444444444444444444444444");
static const auto controller_boot=*m::BootId::parse("55555555555555555555555555555555");
static const auto link=*m::LinkId::parse("66666666666666666666666666666666");
static m::QualifiedDeviceName named(m::OctopusId owner,m::Side side,const char* name,uint8_t adapter=0){return {{group,owner,adapter,side},name};}
static void finish_local(HubService& hub){
    for(unsigned pass=0;pass<96;++pass){run_idle(1);if(!hub.activity_setup_in_progress()&&fixture_pending.empty())return;}
    assert(false&&"Local activity did not finish actual TX steps");
}
static void compact_keys_and_quiet_publication(){
    fixture_present=true;fixture_tick=100000;fixture_async_autocomplete=true;
    HubService hub;assert(hub.start(1,2)==ESP_OK);
    assert(hub.configure({{"Watch","TV","Source","Source","",0}}));run_one_command();fixture_finish_discovery(hub);
    assert(hub.select_activity(0));run_one_command();finish_local(hub);run_idle(4);
    fixture_tick=0;fixture_async_autocomplete=false;
    unsigned publications=0,observations=0;
    hub.on_change([&](const auto&){++publications;});hub.set_owned_observer([&](const auto&){++observations;});
    request_allocations=largest_request_allocation=0;measure_request_allocations=true;
    assert(hub.press_key(4,true));measure_request_allocations=false;
    assert(request_allocations==1&&largest_request_allocation<=96); // Host pointer width is larger than the target's.
    run_one_command();
    for(unsigned repeat=0;repeat<4;++repeat){
        assert(fixture_complete_next(cec::TxResult::Success,2));
        request_allocations=0;measure_request_allocations=true;run_idle(1);measure_request_allocations=false;
        assert(request_allocations==0&&"A cached held-key repeat must not allocate an IPC command");
    }
    assert(publications==0&&observations==0&&"Unchanged key traffic must not rebuild/publish observations");
    assert(hub.release_key(4));run_one_command();fixture_complete_all();fixture_async_autocomplete=true;run_idle(4);
    fixture_tick=100000;
}
static void member_selection_refreshes_projection_without_discovery(){
    fixture_present=true;fixture_tick=100000;fixture_async_autocomplete=true;
    HubService hub;hub.set_system_identity(group,member);hub.set_boot_identity(boot);hub.set_coordinator_authority(false,1);
    assert(hub.start(1,2)==ESP_OK&&hub.bind_owned_controller(controller,1)==ESP_OK);fixture_finish_discovery(hub);
    fixture_tick=0;const auto scanned=hub.snapshot().discovery_generation;
    m::OwnedAuthorization auth{controller,controller_boot,boot,link,1,0};
    m::OwnedIntent selection;selection.kind=m::OwnedIntentKind::activity;selection.target.port={group,member,0,m::Side::front};selection.selected=true;
    selection.generation=1;selection.source=named(member,m::Side::back,"Source");
    assert(hub.submit_owned(selection,auth));finish_local(hub);
    const auto first=hub.snapshot();assert(first.devices.size()==1&&first.devices.front().bus==BusSide::Back);
    selection.generation=2;selection.source=named(member,m::Side::front,"TV");
    assert(hub.submit_owned(selection,auth));finish_local(hub);
    const auto second=hub.snapshot();assert(second.discovery_generation==scanned);
    assert(std::any_of(second.devices.begin(),second.devices.end(),[](const auto& device){return device.bus==BusSide::Front&&device.name.find(":TV")!=std::string::npos;}));
    fixture_tick=100000;
}
static std::vector<SentFrame> select_trace(bool scoped){
    // Compare explicit activity commands without crossing an incidental
    // periodic audio-query deadline as synthetic clock reads advance.
    const auto prior_tick=fixture_tick;fixture_tick=1000;
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
    fixture_sent.clear();assert(hub.select_activity(0));run_one_command();finish_local(hub);
    assert(hub.snapshot().current_activity==0);
    auto trace=fixture_sent;
    fixture_sent.clear();assert(hub.press_key(0x41,true));run_one_command();
    assert(hub.release_key(0x41));run_one_command();run_idle(4);
    fixture_sent.clear();assert(hub.standby());run_one_command();finish_local(hub);
    assert(hub.snapshot().current_activity==-1);
    assert(std::count_if(fixture_sent.begin(),fixture_sent.end(),[](const auto& frame){return frame.frame.size==2&&frame.frame.data[1]==0x36;})==2);
    trace.insert(trace.end(),fixture_sent.begin(),fixture_sent.end());
    fixture_tick=prior_tick;return trace;
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
    assert(owner.execute_owned(intent));
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
    // The setup-time down was ignored before IPC/pressed registration; only
    // the selection and unmatched (no-op) release were queued.
    run_one_command();run_one_command();fixture_finish_discovery(hub);finish_local(hub);
    const auto sent=fixture_sent;
    const auto key=std::count_if(sent.begin(),sent.end(),[](const auto& frame){return frame.frame.size==3&&frame.frame.data[1]==0x44&&frame.frame.data[2]==4;});
    assert(key==0);return sent;
}
static void startup_parity(){
    auto select_frames=[](const auto& sent){std::vector<SentFrame> result;for(const auto& f:sent)if(f.frame.size>1&&(f.frame.data[1]==0x44||f.frame.data[1]==0x45||f.frame.data[1]==0x86||f.frame.data[1]==0x82||f.frame.data[1]==0x04||f.frame.data[1]==0x71))result.push_back(f);return result;};
    const auto baseline=select_frames(startup_trace(false)),owned=select_frames(startup_trace(true));
    if(baseline.size()!=owned.size())for(const auto* trace:{&baseline,&owned}){std::fprintf(stderr,"startup trace size=%zu\n",trace->size());for(const auto& sent:*trace){std::fprintf(stderr,"gpio%d:",sent.gpio);for(unsigned n=0;n<sent.frame.size;++n)std::fprintf(stderr," %02x",sent.frame.data[n]);std::fprintf(stderr,"\n");}}
    if(baseline.size()!=owned.size()){
        for(const auto& group:{baseline,owned}){std::fprintf(stderr,"trace %zu\n",group.size());for(const auto& f:group){std::fprintf(stderr,"gpio=%d attempts=%u frame=",f.gpio,unsigned(f.attempts));for(unsigned n=0;n<f.frame.size;++n)std::fprintf(stderr,"%02x:",f.frame.data[n]);std::fprintf(stderr,"\n");}}
    }
    assert(baseline.size()==owned.size());
    for(size_t i=0;i<baseline.size();++i){const auto& a=baseline[i];const auto& b=owned[i];assert(a.gpio==b.gpio&&a.attempts==b.attempts&&a.frame.size==b.frame.size&&std::memcmp(a.frame.data,b.frame.data,a.frame.size)==0);}
}
static void observation_pressure(){
    fixture_present=true;HubService hub;hub.set_system_identity(group,controller);hub.set_boot_identity(boot);assert(hub.start(1,2)==ESP_OK);
    assert(hub.configure_scoped({{"Watch","display","source","source","",0}},{{"display",named(controller,m::Side::front,"TV")},{"source",named(controller,m::Side::back,"Source")}},{},controller,1)==ESP_OK);run_one_command();fixture_finish_discovery(hub);
    for(unsigned n=0;n<32;++n)assert(hub.press_key(4));
    m::OwnedAdapterSnapshot report;report.owner=member;report.boot=boot;report.ready=true;
    for(unsigned n=0;n<100;++n){report.volume=n;assert(hub.update_owned_snapshot(report));}
    run_one_command();const auto reports=hub.owned_snapshots();const auto found=std::find_if(reports.begin(),reports.end(),[](const auto& state){return state.owner==member;});assert(found!=reports.end()&&found->volume==99);
    for(unsigned n=1;n<32;++n)run_one_command();
}
static void removed_owner_snapshots_release_capacity(){
    fixture_present=true;fixture_tick=100000;fixture_async_autocomplete=true;
    HubService hub;hub.set_system_identity(group,controller);hub.set_boot_identity(boot);assert(hub.start(1,2)==ESP_OK);
    assert(hub.configure_scoped({{"Watch","display","source","source","",0}},{{"display",named(controller,m::Side::front,"TV")},{"source",named(controller,m::Side::back,"Source")}},{},controller,1)==ESP_OK);
    run_one_command();fixture_finish_discovery(hub);fixture_tick=0;
    const auto survivor=*m::OctopusId::parse("77777777777777777777777777777777");
    m::OwnedAdapterSnapshot removed;removed.owner=member;removed.boot=boot;removed.ready=true;removed.devices={{m::Side::back,"Source",4}};
    auto retained=removed;retained.owner=survivor;retained.devices[0].logical_address=8;
    assert(hub.update_owned_snapshot(removed)&&hub.update_owned_snapshot(retained));run_idle(1);
    const auto local_generation=hub.snapshot().discovery_generation;
    const auto local_devices=hub.snapshot().devices;
    const auto has_owner=[&](m::OctopusId owner){const auto reports=hub.owned_snapshots();return std::any_of(reports.begin(),reports.end(),[&](const auto& report){return report.owner==owner;});};
    assert(has_owner(member)&&has_owner(survivor)&&has_owner(controller));

    // Purge both a stored report and pending updates/new adapter reports for
    // only the removed owner, before the worker can consume pending state.
    removed.devices[0].logical_address=11;assert(hub.update_owned_snapshot(removed));
    removed.adapter=1;assert(hub.update_owned_snapshot(removed));
    retained.adapter=1;assert(hub.update_owned_snapshot(retained));
    const auto sent=fixture_sent.size();hub.forget_owned_snapshots(member);
    assert(!has_owner(member)&&has_owner(survivor)&&has_owner(controller)&&fixture_sent.size()==sent);
    run_idle(1);
    assert(!has_owner(member)&&has_owner(survivor)&&has_owner(controller));
    const auto remaining=hub.owned_snapshots();
    assert(std::count_if(remaining.begin(),remaining.end(),[&](const auto& report){return report.owner==survivor&&report.devices[0].logical_address==8;})==2);
    assert(hub.snapshot().discovery_generation==local_generation&&hub.snapshot().devices.size()==local_devices.size());
    const auto current_devices=hub.snapshot().devices;
    for(size_t i=0;i<local_devices.size();++i){const auto& current=current_devices[i];assert(current.name==local_devices[i].name&&current.bus==local_devices[i].bus&&current.address==local_devices[i].address);}

    // Readding the same physical board accepts its new boot and observations.
    removed.adapter=0;removed.boot=controller_boot;removed.devices[0].logical_address=6;
    assert(hub.update_owned_snapshot(removed));run_idle(1);
    const auto readded=hub.owned_snapshots();
    const auto fresh=std::find_if(readded.begin(),readded.end(),[&](const auto& report){return report.owner==member;});
    assert(fresh!=readded.end()&&fresh->boot==controller_boot&&fresh->devices[0].logical_address==6);
    hub.forget_owned_snapshots(member);

    // More than 64 distinct four-adapter boards used to exhaust the facade
    // despite each previous board having been explicitly removed.
    for(unsigned n=1;n<=70;++n){
        const auto owner=*m::OctopusId::generate([n](uint8_t* bytes,size_t size){std::fill(bytes,bytes+size,0);bytes[0]=0xa1;bytes[size-1]=static_cast<uint8_t>(n);return true;});
        for(uint8_t adapter=0;adapter<amity::max_tentacles;++adapter){removed.owner=owner;removed.adapter=adapter;assert(hub.update_owned_snapshot(removed));}
        run_idle(1);assert(has_owner(owner));
        hub.forget_owned_snapshots(owner);assert(!has_owner(owner));
    }
    const auto final=hub.owned_snapshots();assert(final.size()==remaining.size()&&has_owner(survivor)&&has_owner(controller));
    fixture_tick=100000;
}
static void owner_keys(){
    fixture_present=true;HubService hub;hub.set_system_identity(group,member);hub.set_boot_identity(boot);hub.set_coordinator_authority(false,1);assert(hub.start(1,2)==ESP_OK);
    assert(hub.bind_owned_controller(controller,1)==ESP_OK);fixture_finish_discovery(hub);
    bool authority=true;hub.set_owned_authorizer([&](const auto&){return authority;});
    m::OwnedAuthorization auth{controller,controller_boot,boot,link,1,0};
    auto issue=[&](m::OwnedIntent& intent){if(!intent.generation)intent.generation=1;auto scoped_auth=auth;scoped_auth.authority=intent.kind==m::OwnedIntentKind::key_edges?(intent.edges[0].down?m::OwnedAuthority::input:m::OwnedAuthority::cleanup):m::OwnedAuthority::control;return hub.submit_owned(intent,scoped_auth);};
    m::OwnedIntent context;context.kind=m::OwnedIntentKind::context;context.target.port={group,member,0,m::Side::front};context.selected=true;context.source=context.audio=named(member,m::Side::back,"Source");assert(issue(context));
    auto down=[&](uint8_t key){m::OwnedIntent intent;intent.kind=m::OwnedIntentKind::key_edges;intent.target.port={group,member,0,m::Side::front};intent.edges[0]={key,true};intent.edge_count=1;return intent;};
    auto right=down(4),volume=down(0x41);
    fixture_async_autocomplete=false;fixture_sent.clear();
    assert(issue(right));assert(issue(volume));
    assert(std::count_if(fixture_sent.begin(),fixture_sent.end(),[](const auto& f){return f.frame.size>=3&&f.frame.data[1]==0x44;})==2);
    auto up=right;up.edges[0].down=false;assert(issue(up));
    assert(std::none_of(fixture_sent.begin(),fixture_sent.end(),[](const auto& f){return f.frame.size==2&&f.frame.data[1]==0x45;}));
    fixture_complete_all(cec::TxResult::Nack);run_idle(4);
    const auto stream_count=[](){return std::count_if(fixture_sent.begin(),fixture_sent.end(),[](const auto& frame){return frame.frame.size>1&&(frame.frame.data[1]==0x44||frame.frame.data[1]==0x45);});};
    const auto count=stream_count();assert(issue(volume));assert(stream_count()==count);
    up=volume;up.edges[0].down=false;assert(issue(up));assert(stream_count()==count);
    auto wrong=auth;wrong.authority=m::OwnedAuthority::input;wrong.owner_boot=controller_boot;auto bad=down(3);bad.generation=1;assert(!hub.submit_owned(bad,wrong));
    authority=false;auto denied_auth=auth;denied_auth.authority=m::OwnedAuthority::input;assert(!hub.submit_owned(bad,denied_auth));authority=true;
    fixture_async_autocomplete=true;fixture_complete_all();run_idle(2);
}
static void audio_keys_use_audio_or_display(bool remote_owner){
    fixture_present=true;fixture_tick=100000;fixture_async_autocomplete=true;
    fixture_peer_address=[](int gpio)->uint8_t{return gpio==1||gpio==5?0:4;};
    fixture_peer_name=[](int gpio){return std::string(gpio==1||gpio==5?"TV":gpio==6?"Receiver":"Source");};
    const cec::BoardProfile profile{cec::BoardProfileId::Development4Tentacle,1,amity::max_tentacles,
        {{0,cec::Side::Front,1},{0,cec::Side::Back,2},{1,cec::Side::Front,5},{1,cec::Side::Back,6},
         {2,cec::Side::Front,-1},{2,cec::Side::Back,-1},{3,cec::Side::Front,-1},{3,cec::Side::Back,-1}},-1,-1};
    const auto owner=remote_owner?member:controller;
    const auto display=named(owner,m::Side::front,"TV"),source=named(owner,m::Side::back,"Source"),
        receiver=named(owner,m::Side::back,"Receiver",1);
    HubService hub;hub.set_system_identity(group,owner);hub.set_boot_identity(boot);
    if(remote_owner)hub.set_coordinator_authority(false,1);
    assert(hub.start(profile)==ESP_OK);
    m::OwnedAuthorization auth{controller,controller_boot,boot,link,1,0};
    if(remote_owner){
        assert(hub.bind_owned_controller(controller,1)==ESP_OK);
        hub.set_owned_authorizer([&](const auto& current){return current.controller_id==auth.controller_id&&
            current.controller_boot==auth.controller_boot&&current.owner_boot==auth.owner_boot&&
            current.link_id==auth.link_id&&current.config_revision==auth.config_revision;});
    }else{
        assert(hub.configure_scoped({{"TV audio","display","source","","",0},
            {"Receiver audio","display","source","receiver","",0},{"Source only","","source","","",0}},
            {{"display",display},{"source",source},{"receiver",receiver}},{},controller,1)==ESP_OK);
        run_one_command();
    }
    fixture_finish_discovery(hub);
    for(unsigned phase=0;phase<(remote_owner?5u:3u);++phase){
        const uint64_t generation=phase+1;
        fixture_tick=1000;
        if(remote_owner){
            m::OwnedIntent context;context.kind=m::OwnedIntentKind::context;context.target.port=display.port;
            context.generation=generation;context.selected=true;context.source=source;
            if(phase!=2)context.display=display;
            if(phase==1)context.audio=receiver;
            if(phase==3)context.audio=named(owner,m::Side::back,"MissingReceiver");
            if(phase==4)context.audio=named(controller,m::Side::back,"Receiver",1);
            assert(hub.submit_owned(context,auth));
        }else{
            assert(hub.select_activity(static_cast<int>(phase)));run_one_command();finish_local(hub);
            assert(hub.snapshot().current_activity==static_cast<int>(phase));
        }
        auto edge=[&](uint8_t raw,bool down){
            if(!remote_owner){const bool admitted=down?hub.press_key(raw,true):hub.release_key(raw);run_one_command();return admitted;}
            m::OwnedIntent input;input.kind=m::OwnedIntentKind::key_edges;input.target.port=display.port;
            input.generation=generation;input.edges[0]={raw,down};input.edge_count=1;
            auto permitted=auth;permitted.authority=down?m::OwnedAuthority::input:m::OwnedAuthority::cleanup;
            return bool(hub.submit_owned(input,permitted));
        };
        const int audio_gpio=phase==0?1:6;
        const uint8_t audio_address=phase==0?0:4;
        const auto count=[](int gpio,uint8_t address,uint8_t opcode,uint8_t key=0){
            return std::count_if(fixture_sent.begin(),fixture_sent.end(),[&](const auto& tx){return tx.gpio==gpio&&
                tx.frame.destination()==address&&tx.frame.size>1&&tx.frame.data[1]==opcode&&
                (opcode!=0x44||(tx.frame.size==3&&tx.frame.data[2]==key));});
        };
        const auto keys=[](){return std::count_if(fixture_sent.begin(),fixture_sent.end(),[](const auto& tx){
            return tx.frame.size>1&&(tx.frame.data[1]==0x44||tx.frame.data[1]==0x45);});};
        fixture_tick=0;fixture_async_autocomplete=false;fixture_sent.clear();
        assert(edge(4,true)&&edge(0x41,true));
        assert(count(2,4,0x44,4)==1);
        if(phase>=2){
            // No display/audio, an unresolved explicit audio target, and an
            // explicit audio target on another owner never redirect volume.
            assert(keys()==1);assert(edge(0x41,false)&&keys()==1);
            assert(edge(4,false)&&count(2,4,0x45)==1&&keys()==2);
        }else{
            assert(count(audio_gpio,audio_address,0x44,0x41)==1&&keys()==2);
            fixture_time+=600000;run_idle(2);
            assert(count(audio_gpio,audio_address,0x71)==0);
            // A TV/receiver ACK repeats its volume hold while the independent
            // source navigation frame remains outstanding on the back wire.
            assert(fixture_complete_next(cec::TxResult::Success,audio_gpio));run_idle(1);
            assert(count(audio_gpio,audio_address,0x44,0x41)==2&&count(2,4,0x44,4)==1);
            assert(edge(4,false)&&count(2,4,0x45)==1&&count(audio_gpio,audio_address,0x45)==0);
            assert(fixture_complete_next(cec::TxResult::Success,2));run_idle(1);
            assert(count(2,4,0x44,4)==1);
            assert(fixture_complete_next(cec::TxResult::Success,audio_gpio));run_idle(1);
            assert(count(audio_gpio,audio_address,0x44,0x41)==3);
            assert(edge(0x41,false)&&count(audio_gpio,audio_address,0x45)==1);
            assert(keys()==6);
        }
        fixture_complete_all();run_idle(4);
        const auto ended=keys();fixture_time+=600000;run_idle(2);assert(keys()==ended);
        // Every audio key follows the same recipient rule, including mute.
        for(uint8_t raw:{0x42,0x43,0x65,0x66}){
            const auto before=keys();assert(edge(raw,true)&&edge(raw,false));
            assert(keys()==before+(phase>=2?0:2));
            if(phase<2)assert(count(audio_gpio,audio_address,0x44,raw)==1);
            fixture_complete_all();run_idle(2);
        }
        if(phase>=2)assert(count(1,0,0x71)==0);
        for(const auto& tx:fixture_sent)if(tx.frame.size==3&&tx.frame.data[1]==0x44)assert(tx.attempts==1);
        fixture_complete_all();fixture_async_autocomplete=true;run_idle(2);
    }
    fixture_peer_address={};fixture_peer_name={};fixture_tick=100000;
}
static void scoped_audio_fallback_chooses_display_owner(){
    struct NoWire:Transport{
        bool send(BusSide,uint8_t,const uint8_t*,size_t)override{assert(false&&"Scoped keys must dispatch to their owner");return false;}
        void request_scan()override{assert(false&&"Owner dispatch must not preflight discovery");}
    } wire;
    Hub hub(wire);
    const auto display=named(member,m::Side::front,"TV"),source=named(controller,m::Side::back,"Source"),
        receiver=named(controller,m::Side::back,"Receiver",1);
    std::vector<m::OwnedIntent> edges;
    hub.scoped_transport({{"display",display},{"source",source},{"receiver",receiver}},
        [&](auto& intent){if(intent.kind==m::OwnedIntentKind::key_edges)edges.push_back(intent);return m::OwnedDispatchResult{true,true};},
        [](const auto& target)->std::optional<Device>{return Device{target.osd_name,
            target.port.side==m::Side::front?BusSide::Front:BusSide::Back,
            static_cast<uint8_t>(target.port.side==m::Side::front?0:4),0x1000,4,0,true};});
    assert(hub.configure({{"TV audio","display","source","","",0},
        {"Receiver audio","display","source","receiver","",0},{"Source only","","source","","",0}}));
    for(int phase=0;phase<3;++phase){
        assert(hub.select_activity(phase));edges.clear();
        assert(hub.press_key(4,true)&&hub.release_key(4));
        assert(edges.size()==2&&edges[0].target.port==source.port&&edges[1].target.port==source.port);
        edges.clear();
        if(phase==2){assert(!hub.press_key(0x41,true)&&edges.empty());continue;}
        const auto expected=phase==0?display:receiver;
        assert(hub.press_key(0x41,true)&&hub.release_key(0x41));
        assert(edges.size()==2&&edges[0].target.port==expected.port&&edges[1].target.port==expected.port);
        assert(edges[0].edges[0].down&&!edges[1].edges[0].down);
    }
}
static void cec_ack_controls_hold(){
    fixture_present=true;
    cec::BoardProfile profile{cec::BoardProfileId::Development4Tentacle,1,amity::max_tentacles,
        {{0,cec::Side::Front,1},{0,cec::Side::Back,2},{1,cec::Side::Front,5},{1,cec::Side::Back,6},
         {2,cec::Side::Front,7},{2,cec::Side::Back,8},{3,cec::Side::Front,9},{3,cec::Side::Back,44}},-1,-1};
    HubService hub;hub.set_system_identity(group,controller);hub.set_boot_identity(boot);
    assert(hub.start(profile)==ESP_OK);
    assert(hub.configure_scoped({{"Watch","display","source","source","",0}},
        {{"display",named(controller,m::Side::front,"TV")},{"source",named(controller,m::Side::back,"Source")}},{},controller,1)==ESP_OK);
    run_one_command();fixture_finish_discovery(hub);assert(hub.select_activity(0));run_one_command();finish_local(hub);
    fixture_async_autocomplete=false;
    // A configured CEC wire has no separate adapter-presence permission.
    // Submit to the cached recipient even when it no longer answers polls;
    // that key's actual wire result decides its continuation.
    fixture_present=false;run_idle(1);fixture_sent.clear();
    assert(hub.press_key(0x41,true));run_one_command();
    const auto presses=[](){return std::count_if(fixture_sent.begin(),fixture_sent.end(),[](const auto& f){
        return f.gpio==2&&f.frame.size==3&&f.frame.data[1]==0x44&&f.frame.data[2]==0x41;});};
    assert(presses()==1);
    assert(hub.press_key(0x41,true));run_one_command();assert(presses()==1);
    // A consumed down still retains its key state on a wire failure: unchanged
    // down reports cannot restart it, and the eventual up emits no new TX.
    fixture_complete_all(cec::TxResult::Nack);run_idle(1);
    const auto failed_count=fixture_sent.size();
    assert(hub.press_key(0x41,true));run_one_command();assert(fixture_sent.size()==failed_count);
    assert(hub.release_key(0x41));run_one_command();assert(fixture_sent.size()==failed_count);
    assert(hub.press_key(0x41,true));run_one_command();assert(presses()==2);
    assert(hub.release_key(0x41));run_one_command();fixture_complete_all();fixture_async_autocomplete=true;fixture_present=true;
}
static void queued_request_lifetime(){
    fixture_present=true;HubService hub;
    hub.set_system_identity(group,member);hub.set_boot_identity(boot);hub.set_coordinator_authority(false,1);
    assert(hub.start(1,2)==ESP_OK);assert(hub.bind_owned_controller(controller,1)==ESP_OK);fixture_finish_discovery(hub);
    for(const bool on:{true,false}){
        fixture_sent.clear();
        {
            m::OwnedIntent intent;intent.kind=m::OwnedIntentKind::power;intent.target=named(member,m::Side::front,"TV");
            intent.generation=1;intent.on=on;
            m::OwnedAuthorization auth{controller,controller_boot,boot,link,1,0};
            assert(hub.submit_owned(intent,auth,0));
            // The caller may reuse and destroy both arguments before the
            // owner task consumes the queued command.
            intent={};auth={};
        }
        assert(fixture_sent.empty());run_one_command();
        assert(fixture_sent.size()==1);
        const auto& frame=fixture_sent.front();
        assert(frame.gpio==1&&frame.frame.destination()==0&&frame.frame.size==2);
        assert(frame.frame.data[1]==(on?0x04:0x36));
    }
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
        latest_task=primary_task;
        assert(primary.configure_scoped({{"Watch","display","player","receiver","receiver",3}},
            {{"display",named(member,m::Side::front,"TV")},{"player",named(member,m::Side::back,"Player")},{"receiver",named(member,m::Side::back,"Receiver")}},
            [&](const m::OwnedIntent& intent){
                assert(intent.target.port.octopus_id==member);auto* previous=latest_task;latest_task=other_task;
                m::OwnedAuthorization auth{controller,boot,other_boot,link,1,0};auth.authority=intent.kind==m::OwnedIntentKind::key_edges?(intent.edges[0].down?m::OwnedAuthority::input:m::OwnedAuthority::cleanup):m::OwnedAuthority::control;const auto admitted=other->submit_owned(intent,auth);latest_task=previous;
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
    const auto finish=[&]{for(unsigned pass=0;pass<96;++pass){
        if(remote){latest_task=other_task;run_idle(1);}latest_task=primary_task;run_idle(1);
        if(!primary.activity_setup_in_progress()&&(!other||!other->activity_setup_in_progress())&&fixture_pending.empty())return;
    }assert(false&&"Two-owner activity did not finish actual TX steps");};
    drain(0);std::vector<std::vector<SentFrame>> transcript;
    for(int phase=0;phase<3;++phase){
        if(phase){player=phase==1?8:11;const auto previous=primary.snapshot().discovery_generation;latest_task=primary_task;assert(primary.discover());run_one_command();drain(previous);}
        // The coordinator's remote report can be missing while the emitting
        // owner still knows its devices. This never vetoes power/key intents.
        if(remote)for(auto report:primary.owned_snapshots())if(report.owner==member){report.devices.clear();report.ready=false;primary.update_owned_snapshot(report);latest_task=primary_task;run_idle(1);}
        fixture_sent.clear();latest_task=primary_task;assert(primary.select_activity(0));run_one_command();finish();
        auto frames=fixture_sent;
        for(auto& f:frames)if(remote){assert(f.gpio==3||f.gpio==4);f.gpio=f.gpio==3?1:2;}
        const bool switched=std::any_of(frames.begin(),frames.end(),[](const auto& f){return f.frame.size==4&&f.frame.data[1]==0x44&&f.frame.data[2]==0x69&&f.frame.data[3]==3;});
        const bool source_input=std::any_of(frames.begin(),frames.end(),[](const auto& f){return f.frame.size==4&&f.frame.data[1]==0x86&&f.frame.data[2]==0x20;});
        assert(phase?source_input&&!switched:switched&&!source_input);
        transcript.push_back(std::move(frames));
        if(phase==0){
            fixture_sent.clear();latest_task=primary_task;assert(primary.select_activity(0));assert(primary.select_activity(0));run_one_command();run_one_command();finish();
            auto repairs=fixture_sent;for(auto& f:repairs)if(remote){assert(f.gpio==3||f.gpio==4);f.gpio=f.gpio==3?1:2;}
            assert(std::count_if(repairs.begin(),repairs.end(),[](const auto& f){return f.frame.size==4&&f.frame.data[1]==0x44&&f.frame.data[2]==0x69;})==2);
            transcript.push_back(std::move(repairs));
        }
    }
    fixture_sent.clear();latest_task=primary_task;assert(primary.standby());run_one_command();finish();
    auto standby=fixture_sent;
    for(auto& frame:standby)if(remote){assert(frame.gpio==3||frame.gpio==4);frame.gpio=frame.gpio==3?1:2;}
    assert(std::count_if(standby.begin(),standby.end(),[](const auto& frame){return frame.frame.size==2&&frame.frame.data[1]==0x36;})==3);
    transcript.push_back(std::move(standby));
    latest_task=primary_task;fixture_send={};fixture_transmit={};return transcript;
}
static void remote_receiver_parity(){
    const auto local=receiver_transcript(false),remote=receiver_transcript(true);assert(local.size()==remote.size());
    for(size_t phase=0;phase<local.size();++phase){if(local[phase].size()!=remote[phase].size()){for(const auto* trace:{&local[phase],&remote[phase]}){std::fprintf(stderr,"phase=%zu trace=%zu\n",phase,trace->size());for(const auto& f:*trace){std::fprintf(stderr,"gpio%d attempts%u:",f.gpio,f.attempts);for(unsigned j=0;j<f.frame.size;++j)std::fprintf(stderr," %02x",f.frame.data[j]);std::fprintf(stderr,"\n");}}}assert(local[phase].size()==remote[phase].size());for(size_t i=0;i<local[phase].size();++i){const auto& a=local[phase][i];const auto& b=remote[phase][i];assert(a.gpio==b.gpio&&a.attempts==b.attempts&&a.frame.size==b.frame.size);assert(std::memcmp(a.frame.data,b.frame.data,a.frame.size)==0);}}
}
static void configuration_preserves_discovery(){
    struct Wire:Transport{
        bool send(BusSide,uint8_t,const uint8_t*,size_t)override{return true;}
        void request_scan()override{assert(false&&"Activity configuration requested discovery");}
    } wire;
    Hub original(wire);original.set_addresses(4,0);
    original.discovered(BusSide::Back,4);
    original.receive(BusSide::Back,{{0x4f,0x84,0x10,0,4},5});
    original.receive(BusSide::Back,{{0x40,0x47,'S','o','u','r','c','e'},8});
    original.receive(BusSide::Back,{{0x4f,0x87,1,2,3},5});
    const auto original_devices=original.snapshot().devices;
    assert(original_devices.size()==1&&original_devices[0].physical_address==0x1000&&original_devices[0].vendor_id==0x010203);
    const auto same_native_cache=[&]{
        const auto current=original.snapshot().devices;assert(current.size()==original_devices.size());
        for(size_t n=0;n<current.size();++n){const auto& a=current[n];const auto& b=original_devices[n];assert(a.name==b.name&&a.bus==b.bus&&a.address==b.address&&a.physical_address==b.physical_address&&a.type==b.type&&a.vendor_id==b.vendor_id&&a.resolved==b.resolved);}
    };
    original.configure({{"Changed","","Source","Source","",0}});
    same_native_cache();
    assert(original.select_activity(0));
    // An independently requested scan remains in progress across an edit; it
    // alone replaces its staged device set when it actually finishes.
    original.begin_discovery();original.discovered(BusSide::Back,8);
    original.receive(BusSide::Back,{{0x80,0x47,'N','e','w'},5});
    original.configure({{"Another edit","","Source","Source","",0}});
    same_native_cache();
    original.finish_discovery();const auto scanned=original.snapshot().devices;assert(scanned.size()==1&&scanned[0].name=="New");

    fixture_present=true;HubService hub;hub.set_system_identity(group,controller);hub.set_boot_identity(boot);assert(hub.start(1,2)==ESP_OK);
    const auto local_display=named(controller,m::Side::front,"TV"),local_source=named(controller,m::Side::back,"Source");
    const auto remote_source=named(member,m::Side::back,"Remote");
    const Activity retained{"Watch","display","source","source","",0,*m::ActivityId::parse("77777777777777777777777777777777")};
    assert(hub.configure_scoped({retained},
        {{"display",local_display},{"source",local_source},{"remote",remote_source}},[](const auto&){return true;},controller,1)==ESP_OK);
    run_one_command();fixture_finish_discovery(hub);
    m::OwnedAdapterSnapshot remote;remote.owner=member;remote.boot=controller_boot;remote.adapter=0;remote.ready=true;remote.discovery_generation=7;
    remote.front_address=4;remote.back_address=0;remote.front_physical_address=0x2000;remote.audio_status_known=true;remote.volume=33;
    remote.devices={{m::Side::back,"Remote",4,4,0x2100,0x010203,true}};
    auto unselected=remote;unselected.adapter=1;unselected.discovery_generation=11;unselected.devices[0].osd_name="Unselected";
    assert(hub.update_owned_snapshot(remote)&&hub.update_owned_snapshot(unselected));run_idle(1);
    assert(hub.select_activity(0));run_one_command();finish_local(hub);run_idle(1);
    const auto fingerprint=[](const m::OwnedAdapterSnapshot& report){
        std::string value=report.owner.str()+report.boot.str()+":"+std::to_string(report.adapter)+":"+
            std::to_string(report.front_address)+":"+std::to_string(report.back_address)+":"+std::to_string(report.front_physical_address)+":"+
            std::to_string(report.ready)+":"+std::to_string(report.discovering)+":"+std::to_string(report.discovery_generation)+":"+
            std::to_string(report.audio_status_known)+":"+std::to_string(report.muted)+":"+std::to_string(report.volume);
        for(const auto& d:report.devices)value+=":"+std::to_string(int(d.side))+":"+d.osd_name+":"+
            std::to_string(d.logical_address)+":"+std::to_string(d.device_type)+":"+std::to_string(d.physical_address)+":"+
            std::to_string(d.vendor_id)+":"+std::to_string(d.resolved);
        return value;
    };
    const auto before=hub.owned_snapshots();const auto scan_generation=hub.snapshot().discovery_generation;
    assert(before.size()==3&&!hub.snapshot().discovering);
    const auto verify=[&]{
        const auto after=hub.owned_snapshots();assert(after.size()==before.size());
        for(const auto& prior:before){const auto found=std::find_if(after.begin(),after.end(),[&](const auto& current){return current.owner==prior.owner&&current.adapter==prior.adapter;});assert(found!=after.end()&&fingerprint(*found)==fingerprint(prior));}
        assert(!hub.snapshot().discovering&&hub.snapshot().discovery_generation==scan_generation);
        assert(std::none_of(fixture_sent.begin(),fixture_sent.end(),[](const auto& tx){return tx.frame.size==1||(tx.frame.size>1&&(tx.frame.data[1]==0x83||tx.frame.data[1]==0x46||tx.frame.data[1]==0x8c));}));
    };
    fixture_sent.clear();
    // Removing the last saved reference to an owner/adapter does not remove
    // that owner's physical observations, or force any native discovery.
    assert(hub.configure_scoped({retained},
        {{"display",local_display},{"source",local_source}},[](const auto&){return true;},controller,2)==ESP_OK);
    run_one_command();run_idle(2);verify();
    assert(hub.configure_scoped({}, {},[](const auto&){return true;},controller,3)==ESP_ERR_INVALID_STATE);
    verify();
    std::puts("Activity configuration preserves native device records/discovery and selected/unselected remote HDMI snapshots without rediscovery");
}
static void activity_commits_after_source_tx_without_peer_status(bool scoped){
    fixture_present=true;fixture_tick=100000;fixture_async_autocomplete=true;
    HubService hub;if(scoped){hub.set_system_identity(group,controller);hub.set_boot_identity(boot);}
    assert(hub.start(1,2)==ESP_OK);
    if(scoped)assert(hub.configure_scoped({{"Watch","display","source","source","",0}},
        {{"display",named(controller,m::Side::front,"TV")},{"source",named(controller,m::Side::back,"Source")}},{},controller,1)==ESP_OK);
    else assert(hub.configure({{"Watch","TV","Source","Source","",0}}));
    run_one_command();fixture_finish_discovery(hub);
    fixture_tick=0;fixture_sent.clear();fixture_async_autocomplete=false;
    const auto started=fixture_time;
    assert(hub.request_activity(0)==ESP_OK);run_one_command();run_idle(2);
    assert(hub.snapshot().current_activity==-1&&hub.activity_setup_in_progress());
    assert(fixture_pending.size()==1&&fixture_pending.front().gpio==1&&fixture_pending.front().frame.data[1]==0x04);
    bool route=false,finished=false;
    for(unsigned pass=0;pass<48;++pass){
        if(!fixture_pending.empty()){
            const auto pending=fixture_pending.front();
            if(pending.frame.size>1&&pending.frame.data[1]==0x86){
                assert(hub.snapshot().current_activity==-1);route=true;
            }
            assert(fixture_complete_next(cec::TxResult::Success));
        }
        run_idle(2);
        if(hub.snapshot().current_activity==0&&!hub.activity_setup_in_progress()&&fixture_pending.empty()){finished=true;break;}
    }
    // The fixture provides no Active Source, Report Power Status or Audio
    // Status responses here. TX completion finishes setup with a frozen clock.
    assert(route&&finished&&fixture_time==started&&!hub.snapshot().activity_settling);
    assert(std::count_if(fixture_sent.begin(),fixture_sent.end(),[](const auto& tx){return tx.frame.size==4&&tx.frame.data[1]==0x86;})==1);
    fixture_async_autocomplete=true;fixture_tick=100000;
}
static void cold_lookup_blocks_following_activity(bool scoped,int next){
    fixture_present=true;fixture_tick=100000;fixture_async_autocomplete=true;
    HubService hub;if(scoped){hub.set_system_identity(group,controller);hub.set_boot_identity(boot);}
    assert(hub.start(1,2)==ESP_OK);
    if(scoped)assert(hub.configure_scoped({{"First","display","wanted","audio","",0},{"Later","display","audio","audio","",0}},
        {{"display",named(controller,m::Side::front,"TV")},{"wanted",named(controller,m::Side::back,"Wanted")},
         {"audio",named(controller,m::Side::back,"Source")}},{},controller,1)==ESP_OK);
    else assert(hub.configure({{"First","TV","Wanted","Source","",0},{"Later","TV","Source","Source","",0}}));
    run_one_command();fixture_finish_discovery(hub);const auto boot_scan=hub.snapshot().discovery_generation;
    fixture_tick=0;fixture_time+=130000000;fixture_sent.clear();fixture_async_autocomplete=false;
    assert(hub.request_activity(0)==ESP_OK);run_one_command();run_idle(2);
    assert(hub.snapshot().current_activity==-1&&fixture_sent.size()==1);
    assert(hub.request_activity(next)==ESP_OK);run_one_command();run_idle(2);
    assert(fixture_sent.size()==1&&hub.snapshot().current_activity==-1);
    assert(fixture_complete_next(cec::TxResult::Success,1));run_idle(2);
    assert(hub.snapshot().current_activity==-1&&hub.activity_setup_in_progress());
    assert(std::any_of(fixture_pending.begin(),fixture_pending.end(),[](const auto& tx){return tx.gpio==2&&tx.frame.size==1;}));
    assert(hub.press_key(0x41,true));run_idle(2);
    assert(std::none_of(fixture_sent.begin(),fixture_sent.end(),[](const auto& tx){
        return tx.frame.size>2&&tx.frame.data[1]==0x44&&(tx.frame.data[2]==0x6d||tx.frame.data[2]==0x41);
    }));
    // Finish the actual owner-local miss scan. Missing equipment follows the
    // baseline fallback; neither this lookup nor a later action is replayed.
    fixture_complete_all();fixture_async_autocomplete=true;fixture_finish_discovery(hub,boot_scan);run_idle(32);
    assert(hub.snapshot().current_activity==next&&!hub.activity_setup_in_progress());
    assert(std::none_of(fixture_sent.begin(),fixture_sent.end(),[](const auto& tx){return tx.frame.size==3&&tx.frame.data[1]==0x44&&tx.frame.data[2]==0x41;}));
    const auto first_audio=std::find_if(fixture_sent.begin(),fixture_sent.end(),[](const auto& tx){return tx.frame.size==2&&tx.frame.data[1]==0x71;});
    assert(first_audio!=fixture_sent.end());
    if(next==1){
        const auto route=std::find_if(fixture_sent.begin(),fixture_sent.end(),[](const auto& tx){return tx.frame.size==4&&tx.frame.data[1]==0x86;});
        assert(route!=fixture_sent.end()&&first_audio<route);
    }else if(next==-1){
        const auto off=std::find_if(fixture_sent.begin(),fixture_sent.end(),[](const auto& tx){return tx.frame.size==2&&tx.frame.data[1]==0x36;});
        assert(off!=fixture_sent.end()&&first_audio<off);
    }else{
        const auto repeated=std::find_if(fixture_sent.begin()+1,fixture_sent.end(),[](const auto& tx){return tx.gpio==1&&tx.frame.size==2&&tx.frame.data[1]==0x04;});
        assert(repeated!=fixture_sent.end()&&first_audio<repeated);
    }
    fixture_tick=100000;
}
static void cold_lookup_waiters_resume_in_arrival_order(){
    fixture_tick=100000;fixture_present=true;fixture_async_autocomplete=true;
    fixture_peer_address=[](int gpio)->uint8_t{return gpio%2?0:4;};
    cec::BoardProfile profile{cec::BoardProfileId::Development4Tentacle,1,amity::max_tentacles,
        {{0,cec::Side::Front,1},{0,cec::Side::Back,2},{1,cec::Side::Front,5},{1,cec::Side::Back,6},
         {2,cec::Side::Front,-1},{2,cec::Side::Back,-1},{3,cec::Side::Front,-1},{3,cec::Side::Back,-1}},-1,-1};
    HubService hub;hub.set_system_identity(group,member);hub.set_boot_identity(boot);hub.set_coordinator_authority(false,1);
    assert(hub.start(profile)==ESP_OK&&hub.bind_owned_controller(controller,1)==ESP_OK);fixture_finish_discovery(hub);
    m::OwnedAuthorization auth{controller,controller_boot,boot,link,1,0};
    m::OwnedIntent context;context.kind=m::OwnedIntentKind::context;context.target.port={group,member,0,m::Side::front};context.generation=1;context.selected=true;context.audio=named(member,m::Side::back,"Source",1);assert(hub.submit_owned(context,auth));
    auto power=[&](m::Side side,const char* name,uint8_t adapter){m::OwnedIntent i;i.kind=m::OwnedIntentKind::power;i.target=named(member,side,name,adapter);i.generation=1;i.on=true;return i;};
    fixture_send=[](int gpio,const cec::Frame& frame){
        if(frame.size<2||frame.destination()!=fixture_peer(gpio))return true;
        const auto peer=frame.destination();cec::Frame reply;reply.data[0]=(peer<<4)|frame.source();
        switch(frame.data[1]){
        case 0x83:reply.size=5;reply.data[0]=(peer<<4)|15;reply.data[1]=0x84;reply.data[2]=peer?0x10:0;reply.data[3]=0;reply.data[4]=peer?4:0;break;
        case 0x46:{reply.size=2;reply.data[1]=0x47;const char* text=gpio==2?"WantedBack":gpio==5?"WantedFront":peer?"Source":"TV";while(*text)reply.data[reply.size++]=*text++;break;}
        case 0x8c:reply.size=5;reply.data[0]=(peer<<4)|15;reply.data[1]=0x87;break;
        default:return true;
        }
        fixture_incoming.push_back({gpio,reply});return true;
    };
    fixture_tick=0;fixture_time+=130000000;fixture_sent.clear();fixture_async_autocomplete=false;
    assert(hub.submit_owned(power(m::Side::back,"WantedBack",0),auth).code==m::AdmissionCode::pending);
    assert(hub.submit_owned(power(m::Side::front,"WantedFront",1),auth).code==m::AdmissionCode::pending);
    assert(std::any_of(fixture_pending.begin(),fixture_pending.end(),[](const auto& tx){return tx.gpio==2&&tx.frame.size==1;}));
    const auto presses=[&](uint8_t raw){return std::count_if(fixture_sent.begin(),fixture_sent.end(),[&](const auto& tx){return raw==1?(tx.gpio==2&&tx.frame.size==3&&tx.frame.data[1]==0x44&&tx.frame.data[2]==0x6d):raw==2?(tx.gpio==5&&tx.frame.size==2&&tx.frame.data[1]==0x04):(tx.gpio==6&&tx.frame.size==3&&tx.frame.data[1]==0x44&&tx.frame.data[2]==raw);});};
    // This recipient is already cached on the unscanned opposite side. New
    // independent input and ordinary RX remain responsive past the waiters.
    m::OwnedIntent key;key.kind=m::OwnedIntentKind::key_edges;key.target.port={group,member,0,m::Side::front};key.generation=1;key.edges[0]={0x41,true};key.edges[1]={0x41,false};key.edge_count=2;auto input_auth=auth;input_auth.authority=m::OwnedAuthority::input;assert(hub.submit_owned(key,input_auth));assert(presses(0x41)==1);
    fixture_incoming.push_back({1,{{0x04,0x46},2}});run_idle(2);
    assert(std::any_of(fixture_sent.begin(),fixture_sent.end(),[](const auto& tx){return tx.gpio==1&&tx.frame.size>1&&tx.frame.data[1]==0x47;}));
    bool front_ready=false;
    for(unsigned pass=0;pass<400&&!front_ready;++pass){
        fixture_complete_next(cec::TxResult::Pending,5);fixture_time+=100000;run_idle(1);
        for(const auto& report:hub.owned_snapshots())if(report.owner==member&&report.adapter==1)front_ready=report.discovery_generation>1&&!report.discovering;
    }
    if(!front_ready||!hub.snapshot().discovering||presses(1)||presses(2))std::fprintf(stderr,"lookup order: front_ready=%d discovering=%d first=%td later=%td cached=%td pending=%zu\n",front_ready,hub.snapshot().discovering,presses(1),presses(2),presses(0x41),fixture_pending.size());
    assert(front_ready&&hub.snapshot().discovering&&presses(1)==0&&presses(2)==0);
    fixture_complete_all();fixture_async_autocomplete=true;fixture_finish_discovery(hub,2);run_idle(8);
    assert(presses(1)==1&&presses(2)==1&&presses(0x41)==1);
    const auto first=std::find_if(fixture_sent.begin(),fixture_sent.end(),[](const auto& tx){return tx.gpio==2&&tx.frame.size==3&&tx.frame.data[1]==0x44&&tx.frame.data[2]==0x6d;});
    const auto later=std::find_if(fixture_sent.begin(),fixture_sent.end(),[](const auto& tx){return tx.gpio==5&&tx.frame.size==2&&tx.frame.data[1]==0x04;});
    assert(first<later&&first->gpio==2&&later->gpio==5);
    fixture_send={};fixture_peer_address={};fixture_tick=100000;
}
static void transmission_only_rx_does_not_block_a_ready_lookup(){
    fixture_present=true;fixture_tick=100000;fixture_async_autocomplete=true;
    fixture_peer_address=[](int gpio)->uint8_t{return gpio%2?0:4;};
    cec::BoardProfile profile{cec::BoardProfileId::Development4Tentacle,1,amity::max_tentacles,
        {{0,cec::Side::Front,1},{0,cec::Side::Back,2},{1,cec::Side::Front,5},{1,cec::Side::Back,6},
         {2,cec::Side::Front,-1},{2,cec::Side::Back,-1},{3,cec::Side::Front,-1},{3,cec::Side::Back,-1}},-1,-1};
    HubService hub;hub.set_system_identity(group,member);hub.set_boot_identity(boot);hub.set_coordinator_authority(false,1);
    assert(hub.start(profile)==ESP_OK&&hub.bind_owned_controller(controller,1)==ESP_OK);fixture_finish_discovery(hub);
    m::OwnedAuthorization auth{controller,controller_boot,boot,link,1,0};
    m::OwnedIntent context;context.kind=m::OwnedIntentKind::context;context.target.port={group,member,0,m::Side::front};context.generation=1;context.selected=true;context.source=named(member,m::Side::back,"Source");context.audio=named(member,m::Side::back,"Source",1);assert(hub.submit_owned(context,auth));
    fixture_tick=0;fixture_time+=130000000;fixture_async_autocomplete=false;fixture_sent.clear();
    // This real source-theft caller sends its directed Standby first and then
    // waits only for that transmission before restoring the selected input.
    fixture_incoming.push_back({2,{{0x8f,0x82,0x20,0},4}});run_idle(2);
    const auto pending_standby=[](){return std::any_of(fixture_pending.begin(),fixture_pending.end(),[](const auto& tx){return tx.gpio==2&&tx.frame.destination()==8&&tx.frame.size==2&&tx.frame.data[1]==0x36;});};
    assert(pending_standby());
    fixture_send=[](int gpio,const cec::Frame& frame){
        if(gpio!=5||frame.size<2||frame.destination()!=0)return true;
        cec::Frame reply;reply.data[0]=frame.source();
        switch(frame.data[1]){
        case 0x83:reply={{0x0f,0x84,0,0,0},5};break;
        case 0x46:{reply.size=2;reply.data[1]=0x47;const char* text="WantedFront";while(*text)reply.data[reply.size++]=*text++;break;}
        case 0x8c:reply={{0x0f,0x87,0,0,0},5};break;
        default:return true;
        }
        fixture_incoming.push_back({gpio,reply});return true;
    };
    m::OwnedIntent key;key.kind=m::OwnedIntentKind::power;key.target=named(member,m::Side::front,"WantedFront",1);key.generation=1;key.on=true;
    assert(hub.submit_owned(key,auth).code==m::AdmissionCode::pending);
    m::OwnedIntent cached;cached.kind=m::OwnedIntentKind::key_edges;cached.target.port={group,member,0,m::Side::front};cached.generation=1;cached.edges[0]={0x41,true};cached.edges[1]={0x41,false};cached.edge_count=2;auto input_auth=auth;input_auth.authority=m::OwnedAuthority::input;assert(hub.submit_owned(cached,input_auth));
    fixture_incoming.push_back({1,{{0x04,0x46},2}});run_idle(2);
    assert(std::any_of(fixture_sent.begin(),fixture_sent.end(),[](const auto& tx){return tx.gpio==1&&tx.frame.size>1&&tx.frame.data[1]==0x47;}));
    bool ready=false;
    for(unsigned pass=0;pass<400&&!ready;++pass){
        fixture_complete_next(cec::TxResult::Pending,5);fixture_time+=100000;run_idle(1);
        for(const auto& report:hub.owned_snapshots())if(report.owner==member&&report.adapter==1)ready=report.discovery_generation>1&&!report.discovering;
    }
    assert(ready&&pending_standby());
    assert(std::count_if(fixture_sent.begin(),fixture_sent.end(),[](const auto& tx){return tx.gpio==5&&tx.frame.size==2&&tx.frame.data[1]==0x04;})==1);
    assert(std::count_if(fixture_sent.begin(),fixture_sent.end(),[](const auto& tx){return tx.gpio==6&&tx.frame.size==3&&tx.frame.data[1]==0x44&&tx.frame.data[2]==0x41;})==1);
    fixture_complete_all();fixture_async_autocomplete=true;run_idle(16);assert(fixture_pending.empty());
    fixture_send={};fixture_peer_address={};fixture_tick=100000;
}
static void raw_keys_resolve_current_role_on_sparse_adapter(){
    fixture_present=true;fixture_tick=100000;fixture_async_autocomplete=true;
    fixture_peer_address=[](int gpio)->uint8_t{return gpio==5?0:4;};
    const cec::BoardProfile profile{cec::BoardProfileId::Development4Tentacle,1,amity::max_tentacles,
        {{0,cec::Side::Front,-1},{0,cec::Side::Back,-1},{1,cec::Side::Front,5},{1,cec::Side::Back,6},
         {2,cec::Side::Front,-1},{2,cec::Side::Back,-1},{3,cec::Side::Front,-1},{3,cec::Side::Back,-1}},-1,-1};
    HubService hub;hub.set_system_identity(group,member);hub.set_boot_identity(boot);hub.set_coordinator_authority(false,1);
    assert(hub.start(profile)==ESP_OK&&hub.bind_owned_controller(controller,1)==ESP_OK);fixture_finish_discovery(hub);
    m::OwnedAuthorization auth{controller,controller_boot,boot,link,1,0};
    m::OwnedIntent context;context.kind=m::OwnedIntentKind::context;context.target.port={group,member,1,m::Side::front};context.generation=1;context.selected=true;context.source=context.audio=named(member,m::Side::back,"Source",1);assert(hub.submit_owned(context,auth));
    auto edge=[&](uint8_t raw,bool down){m::OwnedIntent input;input.kind=m::OwnedIntentKind::key_edges;input.target.port={group,member,0,m::Side::front};input.generation=1;input.edges[0]={raw,down};input.edge_count=1;auto permitted=auth;permitted.authority=down?m::OwnedAuthority::input:m::OwnedAuthority::cleanup;return hub.submit_owned(input,permitted);};
    const auto count=[&](uint8_t address,uint8_t opcode,uint8_t key=0){return std::count_if(fixture_sent.begin(),fixture_sent.end(),[&](const auto& tx){return tx.gpio==6&&tx.frame.destination()==address&&tx.frame.size>1&&tx.frame.data[1]==opcode&&(opcode!=0x44||(tx.frame.size==3&&tx.frame.data[2]==key));});};
    fixture_tick=0;fixture_async_autocomplete=false;fixture_sent.clear();
    assert(edge(0x61,true)&&count(4,0x44,0x44)==1);assert(edge(0x61,true)&&count(4,0x44,0x44)==1);
    assert(fixture_complete_next(cec::TxResult::Success,6));
    fixture_incoming.push_back({6,{{0x8f,0x84,0x20,0,4},5}});
    fixture_incoming.push_back({6,{{0x80,0x47,'S','o','u','r','c','e'},8}});run_idle(2);
    assert(count(4,0x44,0x44)==1&&count(8,0x44,0x44)==1&&count(8,0x44,0x46)==0);
    assert(edge(0x61,false)&&count(4,0x45)==0&&count(8,0x45)==1);
    fixture_complete_all();run_idle(2);
    assert(edge(0x61,true)&&count(8,0x44,0x46)==1);
    fixture_complete_all(cec::TxResult::Nack);run_idle(4);
    const auto presses=count(8,0x44,0x46),releases=count(8,0x45);
    assert(edge(0x61,true)&&count(8,0x44,0x46)==presses);assert(edge(0x61,false)&&count(8,0x45)==releases);
    fixture_complete_all();run_idle(2);
    assert(edge(4,true)&&edge(0x41,true));const auto before_audio_up=count(8,0x45);
    assert(edge(0x41,false)&&count(8,0x45)==before_audio_up);assert(edge(0x42,true));
    const auto queries=count(8,0x71);fixture_time+=600000;run_idle(3);assert(count(8,0x71)==queries);
    assert(edge(0x42,false)&&count(8,0x45)==before_audio_up);fixture_time+=600000;run_idle(3);assert(count(8,0x71)==queries+1);
    assert(edge(4,false)&&count(8,0x45)==before_audio_up+1);
    fixture_complete_all();fixture_async_autocomplete=true;run_idle(4);fixture_peer_address={};fixture_tick=100000;
}
static void releasing_one_key_does_not_authorize_another_repeat(){
    fixture_present=true;fixture_tick=100000;fixture_async_autocomplete=true;
    HubService hub;hub.set_system_identity(group,member);hub.set_boot_identity(boot);hub.set_coordinator_authority(false,1);assert(hub.start(1,2)==ESP_OK&&hub.bind_owned_controller(controller,1)==ESP_OK);fixture_finish_discovery(hub);
    m::OwnedAuthorization auth{controller,controller_boot,boot,link,1,0};
    m::OwnedIntent context;context.kind=m::OwnedIntentKind::context;context.target.port={group,member,0,m::Side::front};context.generation=1;context.selected=true;context.source=context.audio=named(member,m::Side::back,"Source");assert(hub.submit_owned(context,auth));
    bool positive=true;hub.set_owned_authorizer([&](const auto& current){return current.authority!=m::OwnedAuthority::input||positive;});
    auto edge=[&](uint8_t raw,bool down){m::OwnedIntent input;input.kind=m::OwnedIntentKind::key_edges;input.target.port={group,member,0,m::Side::front};input.generation=1;input.edges[0]={raw,down};input.edge_count=1;auto permitted=auth;permitted.authority=down?m::OwnedAuthority::input:m::OwnedAuthority::cleanup;return hub.submit_owned(input,permitted);};
    const auto volume=[](){return std::count_if(fixture_sent.begin(),fixture_sent.end(),[](const auto& tx){return tx.gpio==2&&tx.frame.size==3&&tx.frame.data[1]==0x44&&tx.frame.data[2]==0x41;});};
    fixture_tick=0;fixture_async_autocomplete=false;fixture_sent.clear();assert(edge(4,true)&&edge(0x41,true)&&volume()==1);
    assert(edge(4,false));positive=false;fixture_complete_all();run_idle(4);assert(volume()==1);
    assert(!edge(0x41,true)&&volume()==1);assert(edge(0x41,false)&&volume()==1);
    fixture_complete_all();fixture_async_autocomplete=true;run_idle(4);fixture_tick=100000;
}
static void native_key_lookup_has_no_partial_side_effects(){
    struct ResolverWire:amity::Transport{
        struct Sent{BusSide side;uint8_t address;std::vector<uint8_t> bytes;};std::vector<Sent> sent;std::vector<BusSide> scans;
        bool send(BusSide side,uint8_t address,const uint8_t* data,size_t size)override{sent.push_back({side,address,{data,data+size}});return true;}
        void request_scan()override{assert(false&&"Recipient lookup must choose its isolated side");}
        bool request_scan(BusSide side)override{scans.push_back(side);return true;}
        size_t count(uint8_t address,uint8_t opcode,uint8_t key=0)const{return std::count_if(sent.begin(),sent.end(),[&](const auto& tx){return tx.address==address&&!tx.bytes.empty()&&tx.bytes[0]==opcode&&(opcode!=0x44||(tx.bytes.size()>1&&tx.bytes[1]==key));});}
    } wire;
    Hub hub(wire);hub.set_addresses(4,0);assert(hub.configure({{"Watch","","Source","Audio","",0}}));
    const auto observe=[&](uint8_t address,uint16_t pa,const char* name){
        hub.discovered(BusSide::Back,address);hub.receive(BusSide::Back,cec::Frame{{static_cast<uint8_t>((address<<4)|15),0x84,static_cast<uint8_t>(pa>>8),static_cast<uint8_t>(pa),4},5});
        cec::Frame label{{static_cast<uint8_t>(address<<4),0x47},2};while(*name)label.data[label.size++]=*name++;hub.receive(BusSide::Back,label);
    };
    observe(4,0x1000,"Source");observe(5,0x2000,"Audio");assert(hub.select_activity(0));
    assert(hub.press_key(0x41,true)&&hub.press_key(4,true));observe(5,0x2000,"Other");wire.sent.clear();
    assert(!hub.press_key(0)&&hub.pending_resolution()==2&&wire.count(4,0x44,0)==0&&wire.count(4,0x45)==0);
    observe(8,0x3000,"Audio");assert(hub.press_key(0));assert(wire.count(4,0x44,0)==1&&wire.count(4,0x45)==0);
    observe(8,0x3000,"Other");wire.sent.clear();
    assert(!hub.release_key(4)&&hub.pending_resolution()==2&&wire.count(4,0x45)==0);
    observe(11,0x4000,"Audio");assert(hub.release_key(4)&&hub.release_key(4));assert(wire.count(4,0x45)==1);
    observe(11,0x4000,"Other");wire.sent.clear();
    assert(!hub.release_key()&&hub.pending_resolution()==2&&wire.count(11,0x45)==0);
    assert(!hub.select_activity(-1)&&hub.pending_resolution()==2&&wire.count(4,0x36)==0);
    observe(5,0x2000,"Audio");assert(hub.select_activity(-1));assert(wire.count(5,0x45)==1&&wire.count(4,0x36)==1&&wire.count(5,0x36)==1);
    const auto cleanup=std::find_if(wire.sent.begin(),wire.sent.end(),[](const auto& tx){return tx.address==5&&tx.bytes==std::vector<uint8_t>{0x45};});
    const auto standby=std::find_if(wire.sent.begin(),wire.sent.end(),[](const auto& tx){return tx.bytes==std::vector<uint8_t>{0x36};});assert(cleanup<standby);
    assert(hub.release_key()&&wire.count(5,0x45)==1);
}
static void cold_audio_lookup_precedes_finite_switch_transmission(){
    fixture_present=true;fixture_tick=100000;fixture_async_autocomplete=true;
    HubService hub;hub.set_system_identity(group,member);hub.set_boot_identity(boot);hub.set_coordinator_authority(false,1);assert(hub.start(1,2)==ESP_OK&&hub.bind_owned_controller(controller,1)==ESP_OK);fixture_finish_discovery(hub);
    m::OwnedAuthorization auth{controller,controller_boot,boot,link,1,0};
    m::OwnedIntent context;context.kind=m::OwnedIntentKind::context;context.target.port={group,member,0,m::Side::front};context.generation=1;context.selected=true;context.audio=named(member,m::Side::back,"Source");context.switch_device=named(member,m::Side::back,"WarmSwitch");context.input=3;assert(hub.submit_owned(context,auth));
    fixture_transmit=[](int gpio,const cec::Frame& frame){return (gpio==1?frame.destination()==0:frame.destination()==4||frame.destination()==8)?cec::TxResult::Success:cec::TxResult::Nack;};
    fixture_send=[](int gpio,const cec::Frame& frame){
        const auto peer=frame.destination();if(frame.size<2||(gpio==1?peer!=0:peer!=4&&peer!=8))return true;
        cec::Frame reply;reply.data[0]=(peer<<4)|frame.source();
        switch(frame.data[1]){
        case 0x83:reply.size=5;reply.data[0]=(peer<<4)|15;reply.data[1]=0x84;reply.data[2]=peer==8?0x20:peer?0x10:0;reply.data[3]=0;reply.data[4]=peer?4:0;break;
        case 0x46:{reply.size=2;reply.data[1]=0x47;const char* text=peer==8?"Source":peer?"WarmSwitch":"TV";while(*text)reply.data[reply.size++]=*text++;break;}
        case 0x8c:reply.size=5;reply.data[0]=(peer<<4)|15;reply.data[1]=0x87;break;
        default:return true;
        }fixture_incoming.push_back({gpio,reply});return true;
    };
    const auto input_count=[](){return std::count_if(fixture_sent.begin(),fixture_sent.end(),[](const auto& tx){return tx.gpio==2&&tx.frame.destination()==4&&tx.frame.size==4&&tx.frame.data[1]==0x44&&tx.frame.data[2]==0x69&&tx.frame.data[3]==3;});};
    fixture_tick=0;fixture_time+=130000000;fixture_async_autocomplete=false;fixture_sent.clear();
    m::OwnedIntent down;down.kind=m::OwnedIntentKind::key_edges;down.target.port={group,member,0,m::Side::front};down.generation=1;down.edges[0]={0x41,true};down.edge_count=1;auto input=auth;input.authority=m::OwnedAuthority::input;
    assert(hub.submit_owned(down,input));run_idle(1);
    // A valid OSD update replaces the formerly named audio record while its
    // raw hold remains registered. The explicit switch is now warm, while
    // its independent audio role must resolve through a cold lookup.
    fixture_incoming.push_back({2,{{0x40,0x47,'W','a','r','m','S','w','i','t','c','h'},12}});run_idle(2);
    m::OwnedIntent sw;sw.kind=m::OwnedIntentKind::switch_input;sw.target=*context.switch_device;sw.generation=1;sw.input=3;
    assert(hub.submit_owned(sw,auth).code==m::AdmissionCode::pending);
    if(input_count())std::fprintf(stderr,"finite switch sent before aggregate lookup: %td\n",input_count());
    assert(input_count()==0);
    fixture_async_autocomplete=true;fixture_complete_all();fixture_finish_discovery(hub,1);run_idle(8);assert(input_count()==1);
    down.edges[0].down=false;input.authority=m::OwnedAuthority::cleanup;assert(hub.submit_owned(down,input));run_idle(4);assert(input_count()==1);
    fixture_send={};fixture_transmit={};fixture_tick=100000;
}
static void source_theft_switch_repair_preserves_audio_hold(){
    fixture_present=true;fixture_tick=100000;fixture_async_autocomplete=true;
    HubService hub;hub.set_system_identity(group,member);hub.set_boot_identity(boot);hub.set_coordinator_authority(false,1);assert(hub.start(1,2)==ESP_OK&&hub.bind_owned_controller(controller,1)==ESP_OK);fixture_finish_discovery(hub);
    m::OwnedAuthorization auth{controller,controller_boot,boot,link,1,0};
    m::OwnedIntent context;context.kind=m::OwnedIntentKind::context;context.target.port={group,member,0,m::Side::front};context.generation=1;context.selected=true;context.display=named(member,m::Side::front,"TV");context.audio=context.switch_device=named(member,m::Side::back,"Source");context.input=3;assert(hub.submit_owned(context,auth));
    auto edge=[&](bool down){m::OwnedIntent input;input.kind=m::OwnedIntentKind::key_edges;input.target.port={group,member,0,m::Side::front};input.generation=1;input.edges[0]={0x41,down};input.edge_count=1;auto permitted=auth;permitted.authority=down?m::OwnedAuthority::input:m::OwnedAuthority::cleanup;return hub.submit_owned(input,permitted);};
    const auto count=[](uint8_t opcode,uint8_t key=0){return std::count_if(fixture_sent.begin(),fixture_sent.end(),[&](const auto& tx){return tx.gpio==2&&tx.frame.destination()==4&&tx.frame.size>1&&tx.frame.data[1]==opcode&&(opcode!=0x44||(tx.frame.size>2&&tx.frame.data[2]==key));});};
    fixture_tick=0;fixture_async_autocomplete=false;fixture_sent.clear();assert(edge(true)&&count(0x44,0x41)==1);run_idle(1);
    fixture_incoming.push_back({2,{{0x8f,0x82,0x20,0},4}});run_idle(2);
    // Complete this owner's directed Standby; the independent held audio
    // press can remain outstanding while original switch repair proceeds.
    for(unsigned pass=0;pass<8&&!count(0x44,0x69);++pass){fixture_complete_next(cec::TxResult::Success,2);run_idle(1);}
    if(count(0x44,0x69)!=1||count(0x45))std::fprintf(stderr,"audio hold switch repair: input=%td releases=%td\n",count(0x44,0x69),count(0x45));
    assert(count(0x44,0x69)==1&&count(0x45)==0);
    assert(edge(false)&&count(0x45)==1);fixture_complete_all();fixture_async_autocomplete=true;run_idle(4);fixture_tick=100000;
}
static void member_maintenance_finishes_key_cleanup(){
    fixture_present=true;fixture_tick=100000;fixture_async_autocomplete=true;
    HubService hub;hub.set_system_identity(group,member);hub.set_boot_identity(boot);hub.set_coordinator_authority(false,1);assert(hub.start(1,2)==ESP_OK&&hub.bind_owned_controller(controller,1)==ESP_OK);fixture_finish_discovery(hub);
    m::OwnedAuthorization auth{controller,controller_boot,boot,link,1,0};
    m::OwnedIntent context;context.kind=m::OwnedIntentKind::context;context.target.port={group,member,0,m::Side::front};context.generation=1;context.selected=true;context.source=context.audio=named(member,m::Side::back,"Source");assert(hub.submit_owned(context,auth));
    m::OwnedIntent down;down.kind=m::OwnedIntentKind::key_edges;down.target.port={group,member,0,m::Side::front};down.generation=1;down.edges[0]={0x41,true};down.edge_count=1;auto input=auth;input.authority=m::OwnedAuthority::input;
    const auto count=[](uint8_t opcode){return std::count_if(fixture_sent.begin(),fixture_sent.end(),[&](const auto& tx){return tx.gpio==2&&tx.frame.size>1&&tx.frame.data[1]==opcode;});};
    fixture_tick=0;fixture_async_autocomplete=false;fixture_sent.clear();assert(hub.submit_owned(down,input)&&count(0x44)==1);
    fixture_delay=[](){fixture_complete_next(cec::TxResult::Success);};
    assert(hub.begin_maintenance()==ESP_OK);fixture_delay={};fixture_complete_all();run_idle(4);
    hub.end_maintenance();fixture_async_autocomplete=true;run_idle(4);
    assert(count(0x44)==1);const auto releases=count(0x45);
    auto next=context;next.kind=m::OwnedIntentKind::activity;next.generation=2;assert(hub.submit_owned(next,auth));run_idle(32);
    if(releases!=1||hub.activity_setup_in_progress()||count(0x86)!=1)std::fprintf(stderr,"member maintenance: releases=%td setup=%d routes=%td ready=%d\n",releases,hub.activity_setup_in_progress(),count(0x86),hub.maintenance_ready());
    assert(releases==1&&!hub.activity_setup_in_progress()&&count(0x86)==1);
    fixture_complete_all();fixture_tick=100000;
}
static void distinct_activity_ids_with_identical_equipment(unsigned mode){
    fixture_present=true;fixture_tick=100000;fixture_async_autocomplete=true;
    fixture_peer_address=[](int gpio)->uint8_t{return gpio%2?0:4;};
    HubService primary;primary.set_system_identity(group,controller);primary.set_boot_identity(boot);assert(primary.start(1,2)==ESP_OK);auto* primary_task=latest_task;
    std::unique_ptr<HubService> other;FakeTask* other_task=nullptr;
    const auto other_boot=*m::BootId::parse("77777777777777777777777777777777");
    std::vector<uint64_t> delivered_generations;
    if(mode==2){
        other=std::make_unique<HubService>();other->set_system_identity(group,member);other->set_boot_identity(other_boot);other->set_coordinator_authority(false,1);
        assert(other->start(3,4)==ESP_OK);other_task=latest_task;assert(other->bind_owned_controller(controller,1)==ESP_OK);fixture_finish_discovery(*other);
        other->set_owned_observer([&](const auto& report){assert(primary.update_owned_snapshot(report));});
    }
    const auto sender=[&](const m::OwnedIntent& intent){
        assert(other&&intent.target.port.octopus_id==member);if(intent.kind==m::OwnedIntentKind::activity)delivered_generations.push_back(intent.generation);
        auto* previous=latest_task;latest_task=other_task;m::OwnedAuthorization auth{controller,boot,other_boot,link,1,0};
        const auto result=other->submit_owned(intent,auth);latest_task=previous;
        return result.code==m::AdmissionCode::accepted||result.code==m::AdmissionCode::pending;
    };
    latest_task=primary_task;
    const std::vector<Activity> activities{{"First activity",mode?"display":"TV",mode?"source":"Source",mode?"source":"Source","",0},
                                           {"Second activity",mode?"display":"TV",mode?"source":"Source",mode?"source":"Source","",0}};
    if(mode)assert(primary.configure_scoped(activities,{{"display",named(mode==2?member:controller,m::Side::front,"TV")},{"source",named(mode==2?member:controller,m::Side::back,"Source")}},sender,controller,1)==ESP_OK);
    else assert(primary.configure(activities));
    run_one_command();fixture_finish_discovery(primary);
    const auto finish=[&](int selected){for(unsigned pass=0;pass<96;++pass){
        if(other){latest_task=other_task;run_idle(1);}latest_task=primary_task;run_idle(1);
        if(primary.snapshot().current_activity==selected&&!primary.activity_setup_in_progress()&&(!other||!other->activity_setup_in_progress())&&fixture_pending.empty())return;
    }assert(false&&"Identical-equipment selection did not finish its original recipe");};
    const auto count=[&](uint8_t opcode,uint8_t operand=0){return std::count_if(fixture_sent.begin(),fixture_sent.end(),[&](const auto& tx){return tx.frame.size>1&&tx.frame.data[1]==opcode&&(opcode!=0x44||(tx.frame.size==3&&tx.frame.data[2]==operand));});};
    fixture_tick=0;assert(primary.request_activity(0)==ESP_OK);finish(0);
    const auto first_generation=primary.activity_generation();
    const auto owner_first_generation=other?other->activity_generation():first_generation;
    const auto first_wire_generation=delivered_generations.empty()?0:delivered_generations.back();
    fixture_sent.clear();assert(primary.request_activity(1)==ESP_OK);finish(1);
    const auto second_generation=primary.activity_generation();
    assert(second_generation!=first_generation&&count(0x04)==0&&count(0x44,0x6d)==0&&count(0x86)==1);
    if(other)assert(other->activity_generation()!=owner_first_generation&&delivered_generations.back()!=first_wire_generation);
    const auto second_wire_generation=delivered_generations.empty()?0:delivered_generations.back();
    fixture_sent.clear();assert(primary.request_activity(1)==ESP_OK);finish(1);
    assert(primary.activity_generation()==second_generation&&count(0x04)==1&&count(0x44,0x6d)==1&&count(0x86)==1);
    if(other)assert(delivered_generations.back()!=second_wire_generation);
    assert(primary.request_activity(-1)==ESP_OK);finish(-1);latest_task=primary_task;fixture_peer_address={};fixture_tick=100000;
}
int main(){compact_keys_and_quiet_publication();member_selection_refreshes_projection_without_discovery();scoped_audio_fallback_chooses_display_owner();for(bool remote_owner:{false,true})audio_keys_use_audio_or_display(remote_owner);removed_owner_snapshots_release_capacity();native_key_lookup_has_no_partial_side_effects();cold_audio_lookup_precedes_finite_switch_transmission();source_theft_switch_repair_preserves_audio_hold();member_maintenance_finishes_key_cleanup();raw_keys_resolve_current_role_on_sparse_adapter();releasing_one_key_does_not_authorize_another_repeat();for(unsigned mode:{0u,1u,2u})distinct_activity_ids_with_identical_equipment(mode);cold_lookup_waiters_resume_in_arrival_order();transmission_only_rx_does_not_block_a_ready_lookup();local_parity();first_matching_osd();startup_parity();observation_pressure();owner_keys();cec_ack_controls_hold();queued_request_lifetime();every_adapter();remote_receiver_parity();configuration_preserves_discovery();activity_commits_after_source_tx_without_peer_status(false);activity_commits_after_source_tx_without_peer_status(true);for(bool scoped:{false,true})for(int next:{-1,0,1})cold_lookup_blocks_following_activity(scoped,next);std::puts("FIFO cold-lookup resumption with responsive cached keys/RX, original local/owned start/standby frame parity, queued request lifetime, CEC ACK-based hold lifecycle, first OSD match, startup taps, observation pressure, removed-owner snapshot cleanup/capacity reuse, independent owner keys, authorization, and all-adapter discovery passed");}
