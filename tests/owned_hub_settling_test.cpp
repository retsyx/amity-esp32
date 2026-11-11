// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#include "cec_hub_fixture.hpp"
using namespace amity;
namespace m=amity::system;
static const auto group=*m::ConsortiumId::parse("11111111111111111111111111111111");
static const auto controller=*m::OctopusId::parse("22222222222222222222222222222222");
static const auto member=*m::OctopusId::parse("33333333333333333333333333333333");
static const auto boot=*m::BootId::parse("44444444444444444444444444444444");
static const auto other_boot=*m::BootId::parse("55555555555555555555555555555555");
static const auto link=*m::LinkId::parse("66666666666666666666666666666666");
static m::QualifiedDeviceName named(m::OctopusId owner,m::Side side,const char* name){return {{group,owner,0,side},name};}
struct Owners {
    HubService primary;std::unique_ptr<HubService> other;FakeTask* primary_task=nullptr;FakeTask* other_task=nullptr;
    std::vector<m::OwnedIntentKind> delivered;
    explicit Owners(bool remote){
        fixture_present=true;fixture_tick=100000;fixture_async_autocomplete=true;
        fixture_peer_address=[](int gpio)->uint8_t{return gpio%2?0:4;};
        primary.set_system_identity(group,controller);primary.set_boot_identity(boot);
        assert(fixture_start(primary,1,2)==ESP_OK);primary_task=latest_task;
        if(remote){
            other=std::make_unique<HubService>();other->set_system_identity(group,member);other->set_boot_identity(other_boot);
            other->set_coordinator_authority(false,1);assert(fixture_start(*other,3,4)==ESP_OK);other_task=latest_task;
            assert(other->bind_owned_controller(controller,1)==ESP_OK);fixture_finish_discovery(*other);
            other->set_owned_observer([&](const auto& report){assert(primary.update_owned_snapshot(report));});
        }
        latest_task=primary_task;
    }
    ~Owners(){latest_task=primary_task;fixture_peer_address={};fixture_before_start={};fixture_async_autocomplete=true;fixture_tick=100000;}
    bool send(const m::OwnedIntent& intent){
        delivered.push_back(intent.kind);
        assert(other&&intent.target.port.octopus_id==member);auto* previous=latest_task;latest_task=other_task;
        m::OwnedAuthorization auth{controller,boot,other_boot,link,1,0};

        const auto result=(intent.kind==m::OwnedIntentKind::key_edges?other->submit_key_edges(intent.edges,intent.edge_count,intent.generation,link,0):other->submit_owned(intent,auth));latest_task=previous;
        return result.code==m::AdmissionCode::accepted||result.code==m::AdmissionCode::pending||result.code==m::AdmissionCode::overloaded||result.code==m::AdmissionCode::unresolved;
    }
    void configure(std::vector<Activity> activities,std::vector<m::ScopedSelector> selectors){
        latest_task=primary_task;assert(primary.configure_scoped(std::move(activities),std::move(selectors),[&](const auto& intent){return send(intent);},controller,1,{},1,other?std::vector<m::OctopusId>{member}:std::vector<m::OctopusId>{})==ESP_OK);
        run_one_command();fixture_finish_discovery(primary);pump(8);
    }
    void pump(unsigned passes=1){for(unsigned n=0;n<passes;++n){if(other){latest_task=other_task;run_idle(1);}latest_task=primary_task;run_idle(1);}}
    void finish(int selected){for(unsigned n=0;n<96;++n){pump();if(primary.snapshot().current_activity==selected&&!primary.activity_setup_in_progress()&&(!other||!other->activity_setup_in_progress())&&fixture_pending.empty())return;}assert(false&&"Ordered activity did not finish");}
};
static void completion_readiness(bool remote){
    Owners owners(remote);const auto source_owner=remote?member:controller;auto& local=remote?*owners.other:owners.primary;
    owners.configure({{"First","display","source","source","",0},{"Second","display","source","source","",0}},
        {{"display",named(source_owner,m::Side::front,"TV")},{"source",named(source_owner,m::Side::back,"Source")}});
    fixture_tick=0;const auto began=fixture_time;fixture_async_autocomplete=false;fixture_sent.clear();
    bool echoed=false;
    fixture_before_start=[&](int gpio,const cec::Frame& frame){if(frame.size==4&&frame.data[1]==0x86){
        echoed=true;cec::Frame echo;echo.size=4;echo.data[0]=0x4f;echo.data[1]=0x82;echo.data[2]=0x10;echo.data[3]=0;
        fixture_incoming.push_back({gpio,echo});
    }};
    assert(owners.primary.request_activity(0)==ESP_OK);owners.pump(2);
    assert(local.snapshot().current_activity==-1&&local.activity_setup_in_progress());
    assert(fixture_pending.size()==1&&fixture_pending.front().gpio==(remote?3:1)&&fixture_pending.front().frame.data[1]==0x04);
    assert(owners.primary.press_key(0x41,true));owners.pump(2);assert(fixture_sent.size()==1);
    for(unsigned pass=0;pass<96&&local.activity_setup_in_progress();++pass){
        if(!fixture_pending.empty()){
            if(fixture_pending.front().frame.size==4&&fixture_pending.front().frame.data[1]==0x86)assert(local.snapshot().current_activity==-1);
            assert(fixture_complete_next(cec::TxResult::Success));
        }
        owners.pump(2);
    }
    assert(echoed&&local.snapshot().current_activity>=0&&!local.activity_setup_in_progress()&&fixture_pending.empty());
    assert(!local.snapshot().activity_settling&&fixture_time==began);
    assert(std::none_of(fixture_sent.begin(),fixture_sent.end(),[](const auto& tx){return tx.frame.size==2&&tx.frame.data[1]==0x36;}));
    // TX completion and the source's causal Active Source echo suffice. No
    // Power Status or Audio Status response and no elapsed time is required.
    fixture_before_start={};fixture_async_autocomplete=true;
    assert(owners.primary.release_key(0x41));owners.pump(2);
    assert(owners.primary.request_activity(1)==ESP_OK);owners.finish(1);
    if(remote){
        owners.delivered.clear();fixture_sent.clear();fixture_async_autocomplete=false;
        const auto edges=[&]{return std::count(owners.delivered.begin(),owners.delivered.end(),m::OwnedIntentKind::key_edges);};
        const auto presses=[](){return std::count_if(fixture_sent.begin(),fixture_sent.end(),[](const auto& tx){return tx.gpio==4&&tx.frame.size==3&&tx.frame.data[1]==0x44&&tx.frame.data[2]==4;});};
        assert(owners.primary.press_key(4,true));owners.pump(2);assert(edges()==1&&presses()==1);
        assert(fixture_complete_next(cec::TxResult::Success,4));owners.pump(2);assert(edges()==1&&presses()==2);
        assert(owners.primary.release_key(4));owners.pump(2);assert(edges()==2);
        fixture_complete_all();fixture_async_autocomplete=true;owners.pump(4);
    }
    assert(owners.primary.request_activity(-1)==ESP_OK);owners.finish(-1);
    assert(fixture_time==began&&!owners.primary.snapshot().activity_settling);
}
static void remote_cold_lookup_blocks_later_selection(int next){
    Owners owners(true);
    owners.configure({{"First","display","wanted","audio","",0},{"Later","display","audio","audio","",0}},
        {{"display",named(member,m::Side::front,"TV")},{"wanted",named(member,m::Side::back,"Wanted")},
         {"audio",named(member,m::Side::back,"Source")}});
    const auto scanned=owners.other->snapshot().discovery_generation;
    fixture_tick=0;fixture_time+=130000000;fixture_async_autocomplete=false;fixture_sent.clear();
    assert(owners.primary.request_activity(0)==ESP_OK);owners.pump(2);
    assert(fixture_pending.size()==1&&fixture_pending.front().gpio==3&&fixture_pending.front().frame.data[1]==0x04);
    assert(owners.primary.request_activity(next)==ESP_OK);owners.pump(2);assert(fixture_sent.size()==1);
    assert(fixture_complete_next(cec::TxResult::Success));owners.pump(3);
    assert(owners.other->snapshot().current_activity==-1&&owners.other->activity_setup_in_progress());
    assert(std::any_of(fixture_pending.begin(),fixture_pending.end(),[](const auto& tx){return tx.gpio==4&&tx.frame.size==1;}));
    assert(owners.primary.press_key(0x41,true));owners.pump(2);
    assert(std::none_of(fixture_sent.begin(),fixture_sent.end(),[](const auto& tx){return tx.frame.size>2&&tx.frame.data[1]==0x44;}));
    fixture_complete_all();fixture_async_autocomplete=true;
    latest_task=owners.other_task;fixture_finish_discovery(*owners.other,scanned);owners.finish(next);
    assert(std::none_of(fixture_sent.begin(),fixture_sent.end(),[](const auto& tx){return tx.frame.size==3&&tx.frame.data[1]==0x44&&tx.frame.data[2]==0x41;}));
    const auto audio=std::find_if(fixture_sent.begin(),fixture_sent.end(),[](const auto& tx){return tx.frame.size==2&&tx.frame.data[1]==0x71;});
    assert(audio!=fixture_sent.end());
    if(next==-1){const auto off=std::find_if(fixture_sent.begin(),fixture_sent.end(),[](const auto& tx){return tx.frame.size==2&&tx.frame.data[1]==0x36;});assert(off!=fixture_sent.end()&&audio<off);}
    else if(next==1){const auto route=std::find_if(fixture_sent.begin(),fixture_sent.end(),[](const auto& tx){return tx.frame.size==4&&tx.frame.data[1]==0x86;});assert(route!=fixture_sent.end()&&audio<route);}
    else{const auto repair=std::find_if(fixture_sent.begin()+1,fixture_sent.end(),[](const auto& tx){return tx.gpio==3&&tx.frame.size==2&&tx.frame.data[1]==0x04;});assert(repair!=fixture_sent.end()&&audio<repair);}
}
static void owners_progress_independently(bool pause_lookup,bool paused_member){
    const int paused_back=paused_member?4:2,progress_front=paused_member?1:3,progress_back=paused_member?2:4;
    const auto paused_owner=paused_member?member:controller,progress_owner=paused_member?controller:member;
    fixture_transmit=[=](int gpio,const cec::Frame& frame){return fixture_present&&(frame.destination()==fixture_peer(gpio)||(gpio==progress_back&&frame.destination()==8))?cec::TxResult::Success:cec::TxResult::Nack;};
    fixture_send=[=](int gpio,const cec::Frame& frame){
        if(frame.size<2)return true;
        const auto peer=frame.destination();if(peer!=fixture_peer(gpio)&&!(gpio==progress_back&&peer==8))return true;
        cec::Frame reply;reply.data[0]=(peer<<4)|frame.source();
        switch(frame.data[1]){
        case 0x83:reply.size=5;reply.data[0]=(peer<<4)|15;reply.data[1]=0x84;reply.data[2]=peer==8?0x20:peer?0x10:0;reply.data[3]=0;reply.data[4]=peer?4:0;break;
        case 0x46:{reply.size=2;reply.data[1]=0x47;const char* text=peer==8?"Player":peer?"Source":"TV";while(*text)reply.data[reply.size++]=*text++;break;}
        case 0x8c:reply.size=5;reply.data[0]=(peer<<4)|15;reply.data[1]=0x87;break;
        default:return true;
        }
        fixture_incoming.push_back({gpio,reply});return true;
    };
    {
        Owners owners(true);auto& paused=paused_member?*owners.other:owners.primary;auto& progress=paused_member?owners.primary:*owners.other;
        owners.configure({{"Old","tv","oldsource","oldaudio","",0},{"New","tv","newsource","missing","",0}},
            {{"tv",named(progress_owner,m::Side::front,"TV")},{"oldsource",named(progress_owner,m::Side::back,"Source")},
             {"newsource",named(progress_owner,m::Side::back,"Player")},{"oldaudio",named(paused_owner,m::Side::back,"Source")},
             {"missing",named(paused_owner,m::Side::back,"Wanted")}});
        assert(owners.primary.request_activity(0)==ESP_OK);owners.finish(0);
        const auto scanned=paused.snapshot().discovery_generation;
        fixture_tick=0;fixture_time+=130000000;fixture_async_autocomplete=false;fixture_sent.clear();owners.delivered.clear();
        assert(owners.primary.request_activity(1)==ESP_OK);owners.pump(3);
        assert(std::any_of(fixture_pending.begin(),fixture_pending.end(),[&](const auto& tx){return tx.gpio==paused_back&&tx.frame.size==2&&tx.frame.data[1]==0x36;}));
        if(pause_lookup){
            assert(fixture_complete_next(cec::TxResult::Success,paused_back));owners.pump(3);
            assert(std::any_of(fixture_pending.begin(),fixture_pending.end(),[&](const auto& tx){return tx.gpio==paused_back&&tx.frame.size==1;}));
        }
        const auto finish_progress=[&](bool standby){
            for(unsigned pass=0;pass<128;++pass){
                fixture_complete_next(cec::TxResult::Success,progress_front);fixture_complete_next(cec::TxResult::Success,progress_back);owners.pump(2);
                const auto& state=progress.snapshot();
                if(!progress.activity_setup_in_progress()&&(standby?state.current_activity==-1:state.current_activity>=0))return;
            }
            assert(false&&"An independent owner's local activity waited for another owner's CEC work");
        };
        finish_progress(false);
        assert(paused.activity_setup_in_progress()&&std::any_of(fixture_pending.begin(),fixture_pending.end(),[&](const auto& tx){return tx.gpio==paused_back;}));
        const auto old_off=std::find_if(fixture_sent.begin(),fixture_sent.end(),[&](const auto& tx){return tx.gpio==progress_back&&tx.frame.destination()==4&&tx.frame.size==2&&tx.frame.data[1]==0x36;});
        const auto new_on=std::find_if(fixture_sent.begin(),fixture_sent.end(),[&](const auto& tx){return tx.gpio==progress_back&&tx.frame.destination()==8&&tx.frame.size==3&&tx.frame.data[1]==0x44&&tx.frame.data[2]==0x6d;});
        const auto route=std::find_if(fixture_sent.begin(),fixture_sent.end(),[&](const auto& tx){return tx.gpio==progress_back&&tx.frame.size==4&&tx.frame.data[1]==0x86&&tx.frame.data[2]==0x20;});
        assert(old_off!=fixture_sent.end()&&new_on!=fixture_sent.end()&&route!=fixture_sent.end()&&old_off<new_on&&new_on<route);
        // Shared Standby is delivered immediately even while the paused owner's
        // local program counter remains blocked on lookup or transmission.
        assert(owners.primary.request_activity(-1)==ESP_OK);owners.pump(3);finish_progress(true);
        assert(paused.activity_setup_in_progress()&&std::any_of(fixture_pending.begin(),fixture_pending.end(),[&](const auto& tx){return tx.gpio==paused_back;}));
        assert(std::any_of(fixture_sent.begin(),fixture_sent.end(),[&](const auto& tx){return tx.gpio==progress_back&&tx.frame.destination()==8&&tx.frame.size==2&&tx.frame.data[1]==0x36;}));
        assert(std::all_of(owners.delivered.begin(),owners.delivered.end(),[](auto kind){return kind==m::OwnedIntentKind::activity||kind==m::OwnedIntentKind::context||kind==m::OwnedIntentKind::key_edges||kind==m::OwnedIntentKind::discover;}));
        fixture_complete_all();fixture_async_autocomplete=true;latest_task=paused_member?owners.other_task:owners.primary_task;fixture_finish_discovery(paused,scanned);owners.finish(-1);
    }
    fixture_send={};fixture_transmit={};
}
static void owner_without_saved_roles_keeps_shared_current_activity(){
    Owners owners(true);
    owners.configure({{"Main only","display","source","source","",0}},
        {{"display",named(controller,m::Side::front,"TV")},{"source",named(controller,m::Side::back,"Source")}});
    fixture_sent.clear();assert(owners.primary.request_activity(0)==ESP_OK);owners.finish(0);
    assert(owners.other->snapshot().current_activity>=0&&!owners.other->activity_setup_in_progress());
    assert(std::none_of(fixture_sent.begin(),fixture_sent.end(),[](const auto& tx){return tx.gpio==3||tx.gpio==4;}));
    fixture_sent.clear();fixture_incoming.push_back({4,{{0x4f,0x82,0x10,0},4}});owners.pump(3);
    assert(std::count_if(fixture_sent.begin(),fixture_sent.end(),[](const auto& tx){return tx.gpio==4&&tx.frame.destination()==4&&tx.frame.size==2&&tx.frame.data[1]==0x36;})==1);
    assert(std::none_of(fixture_sent.begin(),fixture_sent.end(),[](const auto& tx){return tx.gpio!=4||(tx.frame.size==2&&tx.frame.data[1]==0x36&&tx.frame.destination()==15);}));
    assert(owners.other->snapshot().current_activity>=0);
    assert(owners.primary.request_activity(-1)==ESP_OK);owners.finish(-1);assert(owners.other->snapshot().current_activity==-1);
    fixture_sent.clear();fixture_incoming.push_back({4,{{0x4f,0x82,0x10,0},4}});owners.pump(4);
    for(const int gpio:{3,4})assert(std::count_if(fixture_sent.begin(),fixture_sent.end(),[&](const auto& tx){return tx.gpio==gpio&&tx.frame.destination()==15&&tx.frame.size==2&&tx.frame.data[1]==0x36;})==1);
}
int main(){owner_without_saved_roles_keeps_shared_current_activity();for(bool paused_member:{false,true})for(bool pause_lookup:{false,true})owners_progress_independently(pause_lookup,paused_member);for(int next:{-1,0,1})remote_cold_lookup_blocks_later_selection(next);completion_readiness(false);completion_readiness(true);std::puts("Owners execute independent local activity sequences through lookup/TX waits, preserve local Standby-before-On order, and need no remote CEC completion or device-status reply");}
