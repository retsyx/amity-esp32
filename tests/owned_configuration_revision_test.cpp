// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#include "cec_hub_fixture.hpp"
using namespace amity;
namespace m=amity::system;
template<class T> T id(char digit){return *T::parse(std::string(32,digit));}
static void settle(HubService& hub){
    for(unsigned pass=0;pass<96&&hub.activity_setup_in_progress();++pass)run_idle(1);
    assert(!hub.activity_setup_in_progress());
}
static void saved_edits_preserve_current(){
    const auto group=id<m::ConsortiumId>('1');
    const auto controller=id<m::OctopusId>('2'),member=id<m::OctopusId>('3');
    const auto boot=id<m::BootId>('4');
    fixture_present=true;HubService hub;hub.set_system_identity(group,controller);hub.set_boot_identity(boot);assert(hub.start(1,2)==ESP_OK);
    const std::vector<m::ScopedSelector> selectors{{"display",{{group,controller,0,m::Side::front},"TV"}},{"source",{{group,member,0,m::Side::back},"Source"}}};
    const Activity watch{"Watch","display","source","","",0,id<m::ActivityId>('5')};
    Activity other{"Other","display","source","","",0,id<m::ActivityId>('6')};
    std::vector<m::OwnedIntent> sent;bool publication_allowed=true;
    const auto sender=[&](const m::OwnedIntent& intent){
        assert(intent.config_revision==hub.executing_configuration_revision());sent.push_back(intent);
        return intent.kind!=m::OwnedIntentKind::context||publication_allowed;
    };
    assert(hub.configure_scoped({watch,other},selectors,sender,controller,1)==ESP_OK);
    assert(hub.executing_configuration_revision()==0);run_one_command();fixture_finish_discovery(hub);
    assert(hub.select_activity(0));run_one_command();settle(hub);
    const auto before=hub.snapshot();const auto selection=hub.activity_selection(),epoch=hub.input_epoch();
    assert(before.current_activity==0);
    int commits=0;
    const auto reject=[&](std::vector<Activity> candidate,std::vector<m::ScopedSelector> targets){
        assert(hub.configure_scoped(std::move(candidate),std::move(targets),sender,controller,2,[&]{++commits;return ESP_OK;})==ESP_ERR_INVALID_STATE);
        assert(!commits&&hub.activity_selection()==selection&&hub.input_epoch()==epoch&&!hub.activity_setup_in_progress());
    };
    sent.clear();fixture_sent.clear();
    auto changed=watch;changed.name="Changed current";reject({changed,other},selectors);
    reject({other},selectors);auto mapped=selectors;mapped[1].target.osd_name="Another source";reject({watch,other},mapped);
    assert(hub.configure_scoped({watch,other},selectors,sender,controller,2,[&]{++commits;return ESP_OK;},1,{},[](const auto&){return false;})==ESP_ERR_INVALID_STATE);
    assert(!commits&&sent.empty()&&fixture_sent.empty());
    const auto commit_owner_ipc=[&]{
        fixture_require_available_mutex=true;
        assert(hub.snapshot().current_activity==0);
        assert(hub.fence_owned(id<m::OctopusId>('7'),{}));
        assert(hub.snapshot().current_activity==0);
        assert(hub.request_activity(1)==ESP_ERR_INVALID_STATE);
        fixture_require_available_mutex=false;
    };
    bool failed_commit_called=false;
    assert(hub.configure_scoped({watch,other},selectors,sender,controller,2,[&]{
        failed_commit_called=true;commit_owner_ipc();return ESP_FAIL;
    })==ESP_FAIL);
    assert(failed_commit_called&&hub.activity_selection()==selection&&hub.input_epoch()==epoch&&!hub.activity_setup_in_progress());
    assert(hub.press_key(0x01,true));run_one_command();
    assert(sent.size()==1&&sent[0].kind==m::OwnedIntentKind::key_edges&&sent[0].edges[0].down);
    sent.clear();other.name="Edited inactive";
    bool validated=false;
    assert(hub.configure_scoped({other,watch},selectors,sender,controller,2,[&]{commit_owner_ipc();++commits;return ESP_OK;},7,{},[&](const auto& state){
        validated=true;return state.current_activity==0&&state.activities[0].activity_id==watch.activity_id&&state.applied_config_revision==1;
    })==ESP_OK);
    assert(validated&&commits==1&&hub.executing_configuration_revision()==1&&!hub.activity_setup_in_progress());
    assert(hub.configure_scoped({other,watch},selectors,sender,controller,3,[&]{++commits;return ESP_OK;})==ESP_ERR_INVALID_STATE);
    assert(hub.request_activity(1)==ESP_ERR_INVALID_STATE&&hub.activity_selection()==selection&&hub.input_epoch()==epoch);
    run_one_command();
    const auto after=hub.snapshot();
    assert(hub.executing_configuration_revision()==2&&after.applied_presentation_revision==7&&after.current_activity==1);
    assert(after.activities[1].activity_id==watch.activity_id&&after.activity_generation==before.activity_generation&&
           after.binding_generation==before.binding_generation&&after.discovery_generation==before.discovery_generation&&
           hub.activity_selection()==selection&&hub.input_epoch()==epoch&&!hub.activity_setup_in_progress());
    assert(sent.empty()&&fixture_sent.empty());
    uint32_t rejected_receipt=123;
    assert(hub.request_activity(0,&rejected_receipt,watch.activity_id)==ESP_ERR_INVALID_STATE);
    assert(rejected_receipt==123&&hub.activity_selection()==selection&&hub.input_epoch()==epoch&&!hub.activity_setup_in_progress());
    assert(hub.snapshot().current_activity==1&&sent.empty()&&fixture_sent.empty());
    assert(hub.release_key(0x01));run_one_command();
    assert(sent.size()==1&&sent[0].kind==m::OwnedIntentKind::key_edges&&!sent[0].edges[0].down&&sent[0].config_revision==2);
    sent.clear();assert(hub.refresh_owned_context(member,2));run_one_command();
    assert(sent.size()==1&&sent[0].kind==m::OwnedIntentKind::context&&sent[0].selected&&sent[0].source==selectors[1].target&&sent[0].config_revision==2);
    sent.clear();const auto commands_before=command_waits.size();
    assert(!hub.refresh_owned_context(member,0));assert(sent.empty()&&command_waits.size()==commands_before);
    assert(hub.refresh_owned_context(member,2));assert(sent.empty()&&command_waits.size()==commands_before);
    run_one_command();assert(sent.size()==1&&sent[0].kind==m::OwnedIntentKind::context);
    sent.clear();publication_allowed=false;std::vector<m::OwnedContextResult> completed;
    assert(hub.refresh_owned_context(member,2,[&](auto result){completed.push_back(result);}));run_one_command();
    assert(completed==std::vector<m::OwnedContextResult>{m::OwnedContextResult::failed}&&sent.size()==1);
    sent.clear();completed.clear();publication_allowed=true;
    assert(hub.refresh_owned_context(member,1,[&](auto result){completed.push_back(result);}));run_one_command();
    assert(completed==std::vector<m::OwnedContextResult>{m::OwnedContextResult::stale}&&sent.empty());
    assert(hub.request_activity(1,nullptr,watch.activity_id)==ESP_OK);
    const auto accepted=hub.activity_selection();
    assert(hub.configure_scoped({other,watch},selectors,sender,controller,3,[&]{++commits;return ESP_OK;})==ESP_ERR_INVALID_STATE&&commits==1);
    assert(hub.activity_selection()==accepted);run_one_command();settle(hub);
    const auto repaired_selection=hub.activity_selection(),repair_epoch=hub.input_epoch();
    assert(hub.repair_activity()==ESP_OK);
    assert(!hub.activity_setup_in_progress()&&hub.activity_selection()==repaired_selection);
    assert(hub.configure_scoped({other,watch},selectors,sender,controller,3,[&]{++commits;return ESP_OK;})==ESP_ERR_INVALID_STATE&&commits==1);
    assert(!hub.activity_setup_in_progress()&&hub.activity_selection()==repaired_selection&&hub.input_epoch()==repair_epoch+1);
    run_one_command();
    for(unsigned pass=0;pass<64;++pass)run_idle(1);
    assert(hub.standby());run_one_command();settle(hub);
    assert(hub.configure_scoped({}, {},sender,controller,3)==ESP_OK);run_one_command();sent.clear();completed.clear();fixture_sent.clear();
    assert(hub.refresh_owned_context(member,3,[&](auto result){completed.push_back(result);}));run_one_command();
    assert(completed==std::vector<m::OwnedContextResult>{m::OwnedContextResult::published}&&sent.empty()&&fixture_sent.empty());
}
static void owner_revision_preserves_admitted_press_and_hold(){
    const auto group=id<m::ConsortiumId>('1');const auto controller=id<m::OctopusId>('2'),member=id<m::OctopusId>('3');
    const auto boot=id<m::BootId>('4'),owner_boot=id<m::BootId>('5');const auto link=id<m::LinkId>('6');
    fixture_present=true;HubService hub;hub.set_system_identity(group,member);hub.set_boot_identity(owner_boot);hub.set_coordinator_authority(false,1);assert(hub.start(1,2)==ESP_OK);
    assert(hub.bind_owned_controller(controller,1)==ESP_OK);fixture_finish_discovery(hub);
    uint64_t authorized_revision=1;unsigned refreshed_authorizations=0;
    const auto authorized=[&](const auto& authorization){
        const bool current=authorization.controller_id==controller&&authorization.controller_boot==boot&&authorization.owner_boot==owner_boot&&
            authorization.link_id==link&&authorization.config_revision==authorized_revision;
        if(current&&authorized_revision==2)++refreshed_authorizations;
        return current;
    };
    hub.set_owned_authorizer(authorized);
    hub.set_owned_deadline([&](const auto& authorization){return authorized(authorization)?UINT64_MAX:uint64_t{0};});
    m::OwnedAuthorization auth{controller,boot,owner_boot,link,1,0};
    m::OwnedIntent context;context.kind=m::OwnedIntentKind::context;context.target.port={group,member,0,m::Side::front};context.generation=7;context.selected=true;
    context.source=m::QualifiedDeviceName{{group,member,0,m::Side::back},"Source"};context.audio=context.source;
    assert(hub.submit_owned(context,auth));
    fixture_tick=0;fixture_async_autocomplete=false;fixture_sent.clear();fixture_completed.clear();
    m::OwnedIntent press;press.kind=m::OwnedIntentKind::key_edges;press.target.port={group,member,0,m::Side::front};press.generation=7;press.edge_count=1;press.edges[0]={0x01,true};
    auth.authority=m::OwnedAuthority::input;assert(hub.submit_owned(press,auth));
    assert(fixture_pending.size()==1&&fixture_pending.front().frame.data[1]==0x44);const auto generation=hub.snapshot().activity_generation;
    authorized_revision=2;assert(hub.bind_owned_controller(controller,2)==ESP_OK);
    assert(fixture_pending.size()==1&&fixture_complete_next(cec::TxResult::Success));run_idle(3);
    assert(hub.snapshot().activity_generation==generation&&hub.snapshot().current_activity==0);
    assert(std::none_of(fixture_sent.begin(),fixture_sent.end(),[](const auto& tx){return tx.frame.size>=2&&tx.frame.data[1]==0x45;}));
    fixture_time+=1000000;run_idle(3);assert(!fixture_pending.empty());
    assert(refreshed_authorizations>0);
    assert(fixture_pending.front().frame.data[1]==0x44);assert(fixture_complete_next(cec::TxResult::Success));run_idle(2);
    press.edges[0].down=false;auth.config_revision=2;auth.authority=m::OwnedAuthority::cleanup;
    assert(hub.submit_owned(press,auth));fixture_complete_all(cec::TxResult::Success);run_idle(3);
    assert(std::count_if(fixture_sent.begin(),fixture_sent.end(),[](const auto& tx){return tx.frame.size==2&&tx.frame.data[1]==0x45;})==1);
    fixture_tick=100000;fixture_async_autocomplete=true;
}
static void scoped_save_preserves_admitted_frames_and_timed_release(){
    const auto group=id<m::ConsortiumId>('1');const auto controller=id<m::OctopusId>('2');const auto boot=id<m::BootId>('4');
    fixture_present=true;HubService hub;hub.set_system_identity(group,controller);hub.set_boot_identity(boot);assert(hub.start(1,2)==ESP_OK);
    const Activity watch{"Watch","display","source","","",0,id<m::ActivityId>('5')};
    Activity other{"Other","display","source","","",0,id<m::ActivityId>('6')};
    const std::vector<m::ScopedSelector> selectors{{"display",{{group,controller,0,m::Side::front},"TV"}},{"source",{{group,controller,0,m::Side::back},"Source"}}};
    assert(hub.configure_scoped({watch,other},selectors,{},controller,1)==ESP_OK);run_one_command();fixture_finish_discovery(hub);
    assert(hub.select_activity(0));run_one_command();settle(hub);
    fixture_tick=0;fixture_async_autocomplete=false;fixture_sent.clear();fixture_completed.clear();
    const auto generation=hub.activity_generation(),selection=hub.activity_selection(),epoch=hub.input_epoch();
    assert(hub.press_timed_key(0x01,generation,selection,epoch));run_one_command();
    assert(fixture_pending.size()==1&&fixture_pending.front().frame.data[1]==0x44);
    other.name="Edited other";assert(hub.configure_scoped({other,watch},selectors,{},controller,2)==ESP_OK);run_one_command();
    assert(hub.snapshot().current_activity==1&&hub.activity_generation()==generation&&hub.activity_selection()==selection&&hub.input_epoch()==epoch);
    assert(fixture_pending.size()==1&&fixture_sent.size()==1);
    assert(hub.release_timed_key(0x01,generation,selection,epoch));run_one_command();
    assert(fixture_pending.size()==2&&fixture_pending[0].frame.data[1]==0x44&&fixture_pending[1].frame.data[1]==0x45);
    fixture_complete_all(cec::TxResult::Success);run_idle(3);
    assert(fixture_completed.size()==2&&fixture_completed[0].data[1]==0x44&&fixture_completed[1].data[1]==0x45);
    fixture_tick=100000;fixture_async_autocomplete=true;
}
int main(){saved_edits_preserve_current();owner_revision_preserves_admitted_press_and_hold();scoped_save_preserves_admitted_frames_and_timed_release();std::puts("PASS scoped saved edits preserve the stable current activity, generation, input and owner holds; current/in-flight edits reject before persistence; revision context is nonemitting");}
