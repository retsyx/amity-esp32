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
        return publication_allowed;
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
    assert(sent.size()==1&&sent[0].kind==m::OwnedIntentKind::activity&&!sent[0].repair&&sent[0].selected&&sent[0].source==selectors[1].target&&sent[0].config_revision==2);
    sent.clear();const auto commands_before=command_waits.size();
    assert(!hub.refresh_owned_context(member,0));assert(sent.empty()&&command_waits.size()==commands_before);
    assert(hub.refresh_owned_context(member,2));assert(sent.empty()&&command_waits.size()==commands_before);
    run_one_command();assert(sent.size()==1&&sent[0].kind==m::OwnedIntentKind::activity&&!sent[0].repair);
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
    assert(hub.submit_key_edges(press.edges,press.edge_count,press.generation,link,0));
    assert(fixture_pending.size()==1&&fixture_pending.front().frame.data[1]==0x44);const auto generation=hub.snapshot().activity_generation;
    authorized_revision=2;assert(hub.bind_owned_controller(controller,2)==ESP_OK);
    assert(fixture_pending.size()==1&&fixture_complete_next(cec::TxResult::Success));run_idle(3);
    assert(hub.snapshot().activity_generation==generation&&hub.snapshot().current_activity==0);
    assert(std::none_of(fixture_sent.begin(),fixture_sent.end(),[](const auto& tx){return tx.frame.size>=2&&tx.frame.data[1]==0x45;}));
    fixture_time+=1000000;run_idle(3);assert(!fixture_pending.empty());
    assert(refreshed_authorizations==0&&"An admitted hold does not revalidate copied per-key authority");
    assert(fixture_pending.front().frame.data[1]==0x44);assert(fixture_complete_next(cec::TxResult::Success));run_idle(2);
    press.edges[0].down=false;
    assert(hub.submit_key_edges(press.edges,press.edge_count,press.generation,link,0));fixture_complete_all(cec::TxResult::Success);run_idle(3);
    assert(refreshed_authorizations==0&&"Button edges reuse the established authenticated connection");
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

static void never_admitted_selection_delivery(){
    const auto group=id<m::ConsortiumId>('1');
    const auto controller=id<m::OctopusId>('2'),member=id<m::OctopusId>('3'),other_owner=id<m::OctopusId>('7');
    const auto boot=id<m::BootId>('4');
    fixture_present=true;fixture_tick=100000;fixture_async_autocomplete=true;
    HubService hub;hub.set_system_identity(group,controller);hub.set_boot_identity(boot);assert(hub.start(1,2)==ESP_OK);
    const std::vector<m::ScopedSelector> selectors{
        {"display",{{group,controller,0,m::Side::front},"TV"}},
        {"source",{{group,member,0,m::Side::back},"Source"}},
        {"next",{{group,member,0,m::Side::back},"Next"}},
        {"audio",{{group,other_owner,0,m::Side::back},"AVR"}}};
    const Activity watch{"Watch","display","source","audio","",0,id<m::ActivityId>('5')};
    Activity other{"Other","display","next","audio","",0,id<m::ActivityId>('6')};
    bool member_available=false;
    std::vector<m::OwnedIntent> attempts,admitted;
    const auto sender=[&](const m::OwnedIntent& intent){
        assert(intent.config_revision==hub.executing_configuration_revision());
        attempts.push_back(intent);
        const bool available=intent.target.port.octopus_id!=member||member_available;
        if(available)admitted.push_back(intent);
        return available;
    };
    const auto activity_count=[&](m::OctopusId owner){return std::count_if(admitted.begin(),admitted.end(),[&](const auto& intent){return intent.kind==m::OwnedIntentKind::activity&&intent.target.port.octopus_id==owner;});};
    const auto refresh=[&](m::OctopusId owner,uint64_t revision,m::OwnedContextResult expected){
        std::vector<m::OwnedContextResult> completed;
        assert(hub.refresh_owned_context(owner,revision,[&](auto result){completed.push_back(result);}));run_one_command();
        assert(completed==std::vector<m::OwnedContextResult>{expected});
    };
    assert(hub.configure_scoped({watch,other},selectors,sender,controller,1,{},1,{member,other_owner})==ESP_OK);
    run_one_command();fixture_finish_discovery(hub);attempts.clear();admitted.clear();fixture_sent.clear();
    assert(hub.select_activity(0));run_one_command();settle(hub);
    assert(hub.snapshot().current_activity==0&&!hub.activity_setup_in_progress());
    assert(activity_count(member)==0&&activity_count(other_owner)==1);
    const auto initial=std::find_if(attempts.begin(),attempts.end(),[&](const auto& intent){return intent.kind==m::OwnedIntentKind::activity&&intent.target.port.octopus_id==member;});
    assert(initial!=attempts.end()&&initial->selected&&!initial->repair);const auto generation=initial->generation;
    refresh(member,1,m::OwnedContextResult::failed);
    assert(activity_count(member)==0&&activity_count(other_owner)==1);
    // An unrelated edit/reorder advances the saved revision but cannot change
    // the current intent's qualified refs, generation or admission history.
    other.name="Edited inactive";assert(hub.configure_scoped({other,watch},selectors,sender,controller,2,{},2,{member,other_owner})==ESP_OK);run_one_command();
    assert(hub.snapshot().current_activity==1&&hub.snapshot().activities[1].activity_id==watch.activity_id);
    const auto before_stale=attempts.size();refresh(member,1,m::OwnedContextResult::stale);
    assert(std::all_of(attempts.begin()+before_stale,attempts.end(),[](const auto& intent){return intent.kind==m::OwnedIntentKind::activity&&intent.config_revision==2;}));
    member_available=true;refresh(member,2,m::OwnedContextResult::published);
    assert(activity_count(member)==1&&activity_count(other_owner)==1);
    const auto delivered=admitted.back();assert(delivered.kind==m::OwnedIntentKind::activity&&delivered.selected&&!delivered.repair&&
        delivered.generation==generation&&delivered.config_revision==2&&delivered.source==selectors[1].target);
    // A ready capture offers the same normal selection again: sender
    // admission alone cannot establish whether the owner received it.
    assert(hub.fence_owned(member,id<m::LinkId>('8')));
    refresh(member,2,m::OwnedContextResult::published);assert(admitted.back().kind==m::OwnedIntentKind::activity&&admitted.back().generation==generation&&activity_count(member)==2);
    refresh(other_owner,2,m::OwnedContextResult::published);assert(admitted.back().kind==m::OwnedIntentKind::activity&&admitted.back().generation==generation&&activity_count(other_owner)==2);
    // Rejected same-target selection is superseded by a later accepted target.
    member_available=false;assert(hub.select_activity(1));run_one_command();settle(hub);
    assert(hub.select_activity(0));run_one_command();settle(hub);
    const auto latest_generation=attempts.back().generation;
    member_available=true;refresh(member,2,m::OwnedContextResult::published);
    assert(activity_count(member)==3&&admitted.back().source==selectors[2].target&&admitted.back().generation==latest_generation&&!admitted.back().repair);
    // Standby must still reach a never-admitted owner after all role selectors
    // disappear. The saved owner roster supplies its one owner-level delivery.
    member_available=false;assert(hub.standby());run_one_command();settle(hub);
    const auto standby_generation=attempts.back().generation;
    assert(hub.configure_scoped({}, {},sender,controller,3,{},3,{member,other_owner})==ESP_OK);run_one_command();
    member_available=true;refresh(member,3,m::OwnedContextResult::published);
    assert(activity_count(member)==4&&admitted.back().kind==m::OwnedIntentKind::activity&&!admitted.back().selected&&!admitted.back().repair&&
        admitted.back().generation==standby_generation&&admitted.back().config_revision==3&&!admitted.back().source&&!admitted.back().display&&!admitted.back().audio);
    const auto no_roles=attempts.size();refresh(member,3,m::OwnedContextResult::published);refresh(other_owner,3,m::OwnedContextResult::published);assert(attempts.size()==no_roles+2);
    assert(!admitted.back().selected&&!admitted.back().repair&&admitted.back().generation==standby_generation);
    // A rejected long-Power repair is one-shot; readiness must not retry it.
    assert(hub.configure_scoped({watch,other},selectors,sender,controller,4,{},4,{member,other_owner})==ESP_OK);run_one_command();
    assert(hub.select_activity(0));run_one_command();settle(hub);const auto before_repair=activity_count(member);
    member_available=false;assert(hub.repair_activity()==ESP_OK);run_one_command();for(unsigned n=0;n<16;++n)run_idle(1);
    assert(std::any_of(attempts.begin(),attempts.end(),[&](const auto& intent){return intent.target.port.octopus_id==member&&intent.kind==m::OwnedIntentKind::activity&&intent.repair;}));
    member_available=true;refresh(member,4,m::OwnedContextResult::published);
    assert(activity_count(member)==before_repair+1&&admitted.back().kind==m::OwnedIntentKind::activity&&!admitted.back().repair);
}
static void never_admitted_selection_is_not_current(){
    const auto group=id<m::ConsortiumId>('1');const auto controller=id<m::OctopusId>('2'),member=id<m::OctopusId>('3');
    fixture_present=true;fixture_tick=100000;fixture_async_autocomplete=true;
    HubService hub;hub.set_system_identity(group,controller);hub.set_boot_identity(id<m::BootId>('4'));assert(hub.start(1,2)==ESP_OK);
    const std::vector<m::ScopedSelector> selectors{{"display",{{group,controller,0,m::Side::front},"TV"}},{"source",{{group,member,0,m::Side::back},"Source"}}};
    bool available=false;std::vector<m::OwnedIntent> admitted;
    const auto sender=[&](const m::OwnedIntent& intent){if(!available)return false;admitted.push_back(intent);return true;};
    assert(hub.configure_scoped({{"Watch","display","source","","",0,id<m::ActivityId>('5')}},selectors,sender,controller,1,{},1,{member})==ESP_OK);
    run_one_command();fixture_finish_discovery(hub);
    fixture_tick=0;fixture_async_autocomplete=false;fixture_sent.clear();
    assert(hub.select_activity(0));run_one_command();
    for(unsigned pass=0;pass<16&&fixture_pending.empty();++pass)run_idle(1);
    assert(hub.snapshot().current_activity==-1&&hub.activity_setup_in_progress()&&!fixture_pending.empty());
    available=true;std::vector<m::OwnedContextResult> completed;
    assert(hub.refresh_owned_context(member,1,[&](auto result){completed.push_back(result);}));run_one_command();
    assert(completed==std::vector<m::OwnedContextResult>{m::OwnedContextResult::published});
    assert(admitted.size()==1&&admitted[0].kind==m::OwnedIntentKind::activity&&admitted[0].selected&&admitted[0].source==selectors[1].target);
    assert(hub.snapshot().current_activity==-1&&hub.activity_setup_in_progress());
    fixture_complete_all();fixture_async_autocomplete=true;fixture_tick=100000;settle(hub);
    assert(hub.snapshot().current_activity==0);
}
static void never_admitted_selection_idle_progress(){
    const auto group=id<m::ConsortiumId>('1');const auto controller=id<m::OctopusId>('2'),member=id<m::OctopusId>('3');
    fixture_present=true;fixture_tick=100000;fixture_async_autocomplete=true;
    HubService hub;hub.set_system_identity(group,controller);hub.set_boot_identity(id<m::BootId>('4'));assert(hub.start(1,2)==ESP_OK);
    const std::vector<m::ScopedSelector> selectors{{"source",{{group,member,0,m::Side::back},"Source"}}};
    const Activity watch{"Watch","","source","","",0,id<m::ActivityId>('5')};
    bool available=false;std::vector<m::OwnedIntent> attempts,admitted;
    const auto sender=[&](const m::OwnedIntent& intent){attempts.push_back(intent);if(!available)return false;admitted.push_back(intent);return true;};
    assert(hub.configure_scoped({watch},selectors,sender,controller,1,{},1,{member})==ESP_OK);run_one_command();fixture_finish_discovery(hub);
    assert(hub.select_activity(0));run_one_command();settle(hub);
    const auto receipt=hub.activity_selection();
    assert(admitted.empty()&&hub.snapshot().current_activity==0);
    // A full runtime FIFO can drain while its context remains ready. No new
    // owner-ready callback is needed to admit this never-admitted selection.
    available=true;run_idle(1);
    assert(admitted.size()==1&&admitted[0].kind==m::OwnedIntentKind::activity&&admitted[0].selected);
    const auto once=attempts.size();run_idle(4);assert(attempts.size()==once&&hub.activity_selection()==receipt);
    // Submission of newer Standby suppresses old unsent work even while an
    // unrelated command and an already-queued ready capture precede it.
    available=false;assert(hub.select_activity(0));run_one_command();settle(hub);attempts.clear();admitted.clear();
    assert(hub.release_key(0xff));
    std::vector<m::OwnedContextResult> completed;
    assert(hub.refresh_owned_context(member,1,[&](auto result){completed.push_back(result);}));
    assert(hub.standby());available=true;
    run_one_command();assert(attempts.empty());
    run_one_command();assert(attempts.empty()&&completed==std::vector<m::OwnedContextResult>{m::OwnedContextResult::published});
    run_one_command();settle(hub);
    assert(admitted.size()==1&&admitted[0].kind==m::OwnedIntentKind::activity&&!admitted[0].selected);
    // Repair while already off is still one-shot, although its wire intent is
    // Standby rather than a selected-activity repair.
    available=false;assert(hub.repair_activity()==ESP_OK);run_one_command();settle(hub);
    const auto rejected_repair=attempts.size();available=true;run_idle(4);
    assert(attempts.size()==rejected_repair&&admitted.size()==1);
    // A successful one-shot repair does not establish receipt of the normal
    // desired Standby. Its independent normal delivery still has to succeed.
    available=false;assert(hub.standby());run_one_command();settle(hub);
    available=true;assert(hub.repair_activity()==ESP_OK);run_one_command();settle(hub);
    const auto repaired=attempts.size();assert(admitted.size()==3&&!admitted.back().selected);
    assert(admitted[1].repair&&!admitted[2].repair&&admitted[1].generation==admitted[2].generation);
    run_idle(4);assert(attempts.size()==repaired&&admitted.size()==3);
    // Removing an owner prunes its never-admitted selection even when there
    // are no selected refs left to carry its pending Standby.
    available=false;assert(hub.standby());run_one_command();settle(hub);
    assert(hub.configure_scoped({}, {},sender,controller,2,{},2,{})==ESP_OK);run_one_command();
    const auto removed=attempts.size();available=true;run_idle(4);
    assert(hub.refresh_owned_context(member,2));run_one_command();assert(attempts.size()==removed);
}
// Two real HubService workers separated by a transport admission boundary.
// Returning true while discarding one activity models a queued transport item
// lost before owner admission. Recovery must produce the original CEC recipe,
// not merely another successful sender call or a non-emitting context update.
struct RecoveryOwners {
    const m::ConsortiumId group=id<m::ConsortiumId>('1');
    const m::OctopusId controller=id<m::OctopusId>('2'),member=id<m::OctopusId>('3');
    const m::BootId controller_boot=id<m::BootId>('4'),member_boot=id<m::BootId>('5');
    m::BootId current_controller_boot=controller_boot;
    m::LinkId current_link=id<m::LinkId>('6');
    HubService main,owner;
    FakeTask* main_task=nullptr;FakeTask* owner_task=nullptr;
    bool drop_next_activity=false;
    std::vector<m::OwnedIntent> offers,dropped;
    m::AdmissionCode last_admission=m::AdmissionCode::invalid;
    uint64_t revision=1;

    RecoveryOwners(){
        assert(fixture_pending.empty());fixture_present=true;fixture_tick=100000;fixture_async_autocomplete=true;
        fixture_sent.clear();fixture_completed.clear();fixture_incoming.clear();
        fixture_peer_address=[](int gpio)->uint8_t{return gpio%2?0:4;};
        main.set_system_identity(group,controller);main.set_boot_identity(controller_boot);assert(main.start(1,2)==ESP_OK);main_task=latest_task;
        owner.set_system_identity(group,member);owner.set_boot_identity(member_boot);owner.set_coordinator_authority(false,1);
        assert(owner.start(3,4)==ESP_OK);owner_task=latest_task;
        assert(owner.bind_owned_controller(controller,revision)==ESP_OK);fixture_finish_discovery(owner);
        owner.set_owned_authorizer([&](const auto& auth){return auth.controller_id==controller&&auth.controller_boot==current_controller_boot&&
            auth.owner_boot==member_boot&&auth.link_id==current_link&&auth.config_revision==revision;});
        owner.set_owned_observer([&](const auto& report){assert(main.update_owned_snapshot(report));});
        // Populate the second source through the same incoming announcements
        // used by production discovery. No resolver/cache bypass is needed.
        fixture_incoming.push_back({4,{{0x8f,0x84,0x20,0x00,0x04},5}});
        fixture_incoming.push_back({4,{{0x80,0x47,'N','e','x','t'},6}});
        fixture_incoming.push_back({4,{{0x8f,0x87,0,0,1},5}});
        run_idle(8);
        latest_task=main_task;
        const auto named=[&](m::Side side,const char* name){return m::QualifiedDeviceName{{group,member,0,side},name};};
        const std::vector<m::ScopedSelector> selectors{
            {"display",{{group,controller,0,m::Side::front},"TV"}},
            {"source",named(m::Side::back,"Source")},{"next",named(m::Side::back,"Next")}};
        assert(main.configure_scoped({{"Old","display","source","source","",0,id<m::ActivityId>('7')},
                {"New","display","next","next","",0,id<m::ActivityId>('8')}},selectors,
            [&](const auto& intent){return send(intent);},controller,revision,{},revision,{member})==ESP_OK);
        run_one_command();fixture_finish_discovery(main);pump(8);
        assert(main.request_activity(0)==ESP_OK);finish(0);
        offers.clear();fixture_sent.clear();fixture_completed.clear();
    }
    ~RecoveryOwners(){latest_task=main_task;fixture_peer_address={};fixture_async_autocomplete=true;fixture_tick=100000;}
    bool send(const m::OwnedIntent& intent){
        assert(intent.target.port.octopus_id==member);offers.push_back(intent);
        if(drop_next_activity&&intent.kind==m::OwnedIntentKind::activity){drop_next_activity=false;dropped.push_back(intent);return true;}
        auto* previous=latest_task;latest_task=owner_task;
        m::OwnedAuthorization auth{controller,current_controller_boot,member_boot,current_link,revision,0};

        const auto result=(intent.kind==m::OwnedIntentKind::key_edges?owner.submit_key_edges(intent.edges,intent.edge_count,intent.generation,current_link,0):owner.submit_owned(intent,auth));last_admission=result.code;latest_task=previous;
        return result.code==m::AdmissionCode::accepted||result.code==m::AdmissionCode::pending||result.code==m::AdmissionCode::unresolved;
    }
    void pump(unsigned count=1){for(unsigned pass=0;pass<count;++pass){latest_task=owner_task;run_idle(1);latest_task=main_task;run_idle(1);}}
    void finish(int selected){
        for(unsigned pass=0;pass<200;++pass){pump();if(main.snapshot().current_activity==selected&&!main.activity_setup_in_progress()&&
                !owner.activity_setup_in_progress()&&fixture_pending.empty())return;}
        assert(false&&"Recovered owner activity did not finish");
    }
    void refresh(){
        std::vector<m::OwnedContextResult> completed;latest_task=main_task;
        assert(main.refresh_owned_context(member,revision,[&](auto result){completed.push_back(result);}));run_one_command();
        assert(completed==std::vector<m::OwnedContextResult>{m::OwnedContextResult::published});
    }
    void recover(){
        const auto previous=current_link;const auto input_epoch=owner.input_epoch();
        latest_task=owner_task;assert(owner.fence_owned(controller,previous));
        assert(owner.input_epoch()==input_epoch+1&&"Connection invalidation fences stale key work independently of activity progress");
        current_link=id<m::LinkId>(previous==id<m::LinkId>('6')?'9':'6');
        refresh();
    }
    size_t count(int gpio,uint8_t address,std::initializer_list<uint8_t> data)const{
        return std::count_if(fixture_sent.begin(),fixture_sent.end(),[&](const auto& tx){return tx.gpio==gpio&&tx.frame.destination()==address&&
            tx.frame.size==data.size()+1&&std::equal(data.begin(),data.end(),tx.frame.data+1);});
    }
    size_t completed(uint8_t address,std::initializer_list<uint8_t> data)const{
        return std::count_if(fixture_completed.begin(),fixture_completed.end(),[&](const auto& frame){return frame.destination()==address&&
            frame.size==data.size()+1&&std::equal(data.begin(),data.end(),frame.data+1);});
    }
    size_t pending(int gpio,uint8_t opcode)const{
        return std::count_if(fixture_pending.begin(),fixture_pending.end(),[&](const auto& tx){return tx.gpio==gpio&&tx.frame.size>=2&&tx.frame.data[1]==opcode;});
    }
    const m::OwnedIntent& latest_selection()const{
        const auto found=std::find_if(offers.rbegin(),offers.rend(),[](const auto& intent){return intent.kind==m::OwnedIntentKind::activity;});
        assert(found!=offers.rend());return *found;
    }
};
static void transport_admitted_selection_recovers_its_physical_recipe(){
    RecoveryOwners owners;const auto discovered=owners.owner.snapshot().discovery_generation;
    owners.drop_next_activity=true;assert(owners.main.request_activity(1)==ESP_OK);owners.finish(1);
    assert(owners.dropped.size()==1&&owners.dropped[0].selected&&!owners.dropped[0].repair);
    assert(owners.count(4,4,{0x36})==0&&owners.count(4,8,{0x44,0x6d})==0);
    owners.recover();owners.finish(1);
    assert(owners.count(4,4,{0x36})==1&&owners.count(4,8,{0x44,0x6d})==1&&owners.count(4,15,{0x86,0x20,0})==1);
    const auto old_off=std::find_if(fixture_sent.begin(),fixture_sent.end(),[](const auto& tx){return tx.gpio==4&&tx.frame.destination()==4&&tx.frame.size==2&&tx.frame.data[1]==0x36;});
    const auto new_on=std::find_if(fixture_sent.begin(),fixture_sent.end(),[](const auto& tx){return tx.gpio==4&&tx.frame.destination()==8&&tx.frame.size==3&&tx.frame.data[1]==0x44&&tx.frame.data[2]==0x6d;});
    assert(old_off!=fixture_sent.end()&&new_on!=fixture_sent.end()&&old_off<new_on);
    assert(owners.owner.snapshot().discovery_generation==discovered&&owners.owner.snapshot().current_activity>=0);
    const auto sent=fixture_sent.size();const auto receipt=owners.owner.activity_selection(),epoch=owners.owner.input_epoch();
    owners.refresh();owners.pump(8);owners.recover();owners.pump(8);
    assert(fixture_sent.size()==sent&&owners.owner.activity_selection()==receipt&&owners.owner.input_epoch()==epoch+1);
    // Rebinding input after recovery preserves the committed source context
    // used by a fresh announcement from the previously selected player.
    fixture_incoming.push_back({4,{{0x4f,0x82,0x10,0},4}});owners.pump(16);
    assert(owners.completed(4,{0x36})==2&&owners.completed(15,{0x86,0x20,0})==2);
}
static void owner_admitted_selection_survives_recovery(bool first_step_completed){
    RecoveryOwners owners;const auto discovered=owners.owner.snapshot().discovery_generation;
    fixture_tick=0;fixture_async_autocomplete=false;
    assert(owners.main.request_activity(1)==ESP_OK);owners.pump(4);
    assert(owners.count(4,4,{0x36})==1&&owners.count(4,8,{0x44,0x6d})==0);
    assert(owners.pending(4,0x36)==1);
    if(first_step_completed){
        assert(fixture_complete_next(cec::TxResult::Success,4));owners.pump(4);
        assert(owners.completed(4,{0x36})==1&&owners.count(4,8,{0x44,0x6d})==1);
        assert(owners.pending(4,0x44)==1);
    }
    const auto receipt=owners.owner.activity_selection(),epoch=owners.owner.input_epoch();
    const auto generation=owners.latest_selection().generation;
    // A duplicate received while the original job is waiting for its first
    // or second TX completion must acknowledge that job without restarting it.
    owners.refresh();owners.pump(3);
    assert(owners.owner.activity_selection()==receipt&&owners.owner.input_epoch()==epoch);
    assert(owners.latest_selection().generation==generation&&!owners.latest_selection().repair);
    owners.recover();owners.pump(3);
    assert(owners.owner.activity_selection()==receipt&&owners.owner.input_epoch()==epoch+1);
    assert(owners.owner.activity_setup_in_progress()&&owners.pending(4,first_step_completed?0x44:0x36)==1);
    // The admitted local TX retains its authority across a logical session
    // fence. Completing it after the fence must still complete the wire frame.
    assert(fixture_complete_next(cec::TxResult::Success,4));
    fixture_complete_all(cec::TxResult::Success);fixture_async_autocomplete=true;owners.finish(1);
    assert(owners.count(4,4,{0x36})==1&&owners.count(4,8,{0x44,0x6d})==1&&owners.count(4,15,{0x86,0x20,0})==1);
    assert(owners.completed(4,{0x36})==1&&owners.completed(8,{0x44,0x6d})==1&&owners.completed(15,{0x86,0x20,0})==1);
    assert(owners.owner.snapshot().discovery_generation==discovered);
    const auto sent=fixture_sent.size();owners.refresh();owners.recover();owners.pump(8);
    assert(fixture_sent.size()==sent&&owners.owner.activity_selection()==receipt&&owners.owner.input_epoch()==epoch+2);
}
static void same_target_selection_is_fresh_but_recovery_is_not(){
    RecoveryOwners owners;
    const auto receipt=owners.owner.activity_selection(),epoch=owners.owner.input_epoch();
    assert(owners.main.request_activity(0)==ESP_OK);owners.finish(0);
    const auto first_generation=owners.latest_selection().generation;
    assert(!owners.latest_selection().repair&&owners.owner.activity_selection()==receipt+1&&owners.owner.input_epoch()==epoch+1);
    assert(owners.count(4,4,{0x44,0x6d})==1&&owners.count(4,15,{0x86,0x10,0})==1);
    assert(owners.count(4,4,{0x36})==0);
    assert(owners.main.request_activity(0)==ESP_OK);owners.finish(0);
    assert(owners.latest_selection().generation>first_generation&&!owners.latest_selection().repair);
    assert(owners.owner.activity_selection()==receipt+2&&owners.owner.input_epoch()==epoch+2);
    assert(owners.count(4,4,{0x44,0x6d})==2&&owners.count(4,15,{0x86,0x10,0})==2);
    const auto sent=fixture_sent.size();owners.refresh();owners.recover();owners.pump(8);
    assert(fixture_sent.size()==sent&&owners.owner.activity_selection()==receipt+2&&owners.owner.input_epoch()==epoch+3);
}
static void reselected_target_still_transitions_an_owner_that_missed_it(){
    RecoveryOwners owners;owners.drop_next_activity=true;
    assert(owners.main.request_activity(1)==ESP_OK);owners.finish(1);
    assert(owners.count(4,4,{0x36})==0&&owners.count(4,8,{0x44,0x6d})==0);
    assert(owners.main.request_activity(1)==ESP_OK);owners.finish(1);
    assert(owners.latest_selection().reselect&&!owners.latest_selection().repair);
    assert(owners.count(4,4,{0x36})==1&&owners.count(4,8,{0x44,0x6d})==1&&owners.count(4,15,{0x86,0x20,0})==1);
    const auto old_off=std::find_if(fixture_sent.begin(),fixture_sent.end(),[](const auto& tx){return tx.gpio==4&&tx.frame.destination()==4&&tx.frame.size==2&&tx.frame.data[1]==0x36;});
    const auto new_on=std::find_if(fixture_sent.begin(),fixture_sent.end(),[](const auto& tx){return tx.gpio==4&&tx.frame.destination()==8&&tx.frame.size==3&&tx.frame.data[1]==0x44;});
    assert(old_off!=fixture_sent.end()&&new_on!=fixture_sent.end()&&old_off<new_on);
    const auto sent=fixture_sent.size();owners.recover();owners.pump(4);assert(fixture_sent.size()==sent);
}
static void recovery_of_newer_queued_selection_preserves_order(){
    RecoveryOwners owners;fixture_tick=0;fixture_async_autocomplete=false;
    assert(owners.main.request_activity(1)==ESP_OK);owners.pump(4);
    assert(owners.count(4,4,{0x36})==1&&owners.pending(4,0x36)==1);
    assert(owners.main.request_activity(0)==ESP_OK);owners.pump(4);
    const auto generation=owners.latest_selection().generation;
    const auto receipt=owners.owner.activity_selection(),epoch=owners.owner.input_epoch();
    assert(owners.latest_selection().source&&owners.latest_selection().source->osd_name=="Source");
    owners.recover();owners.pump(3);
    assert(owners.owner.activity_selection()==receipt&&owners.owner.input_epoch()==epoch+1&&owners.latest_selection().generation==generation);
    assert(owners.count(4,4,{0x36})==1&&owners.count(4,8,{0x44,0x6d})==0);
    assert(fixture_complete_next(cec::TxResult::Success,4));fixture_complete_all(cec::TxResult::Success);fixture_async_autocomplete=true;owners.finish(0);
    assert(owners.count(4,4,{0x36})==1&&owners.count(4,8,{0x44,0x6d})==1);
    assert(owners.count(4,8,{0x36})==1&&owners.count(4,4,{0x44,0x6d})==1);
    const auto new_on=std::find_if(fixture_sent.begin(),fixture_sent.end(),[](const auto& tx){return tx.gpio==4&&tx.frame.destination()==8&&tx.frame.size==3&&tx.frame.data[1]==0x44;});
    const auto new_off=std::find_if(fixture_sent.begin(),fixture_sent.end(),[](const auto& tx){return tx.gpio==4&&tx.frame.destination()==8&&tx.frame.size==2&&tx.frame.data[1]==0x36;});
    const auto old_on=std::find_if(fixture_sent.begin(),fixture_sent.end(),[](const auto& tx){return tx.gpio==4&&tx.frame.destination()==4&&tx.frame.size==3&&tx.frame.data[1]==0x44;});
    assert(new_on!=fixture_sent.end()&&new_off!=fixture_sent.end()&&old_on!=fixture_sent.end()&&new_on<new_off&&new_off<old_on);
    const auto sent=fixture_sent.size();owners.refresh();owners.pump(4);
    assert(fixture_sent.size()==sent&&owners.owner.activity_selection()==receipt&&owners.owner.input_epoch()==epoch+1);
}
static void conflicting_duplicate_and_stale_repair_are_rejected(){
    RecoveryOwners owners;assert(owners.main.request_activity(1)==ESP_OK);owners.finish(1);
    const auto accepted=owners.latest_selection();const auto sent=fixture_sent.size();
    const auto receipt=owners.owner.activity_selection(),epoch=owners.owner.input_epoch();
    auto conflict=accepted;conflict.reselect=!conflict.reselect;
    assert(!owners.send(conflict)&&owners.last_admission==m::AdmissionCode::stale);
    conflict=accepted;conflict.source->osd_name="Source";
    assert(!owners.send(conflict)&&owners.last_admission==m::AdmissionCode::stale);
    auto stale_repair=accepted;assert(stale_repair.generation>1);--stale_repair.generation;stale_repair.repair=true;
    assert(!owners.send(stale_repair)&&owners.last_admission==m::AdmissionCode::stale);
    owners.pump(4);
    assert(fixture_sent.size()==sent&&owners.owner.activity_selection()==receipt&&owners.owner.input_epoch()==epoch);
    owners.refresh();owners.pump(4);
    assert(fixture_sent.size()==sent&&owners.owner.activity_selection()==receipt&&owners.owner.input_epoch()==epoch);
}
static void standby_recovery_does_not_repeat_one_shot_repairs(){
    RecoveryOwners owners;
    assert(owners.main.standby());owners.finish(-1);
    const auto standby_generation=owners.latest_selection().generation;
    const auto receipt=owners.owner.activity_selection();
    fixture_sent.clear();fixture_completed.clear();owners.offers.clear();
    assert(owners.main.repair_activity()==ESP_OK);owners.pump(4);owners.finish(-1);
    assert(!owners.latest_selection().selected&&owners.latest_selection().repair);
    assert(owners.count(3,15,{0x36})==1&&owners.count(4,15,{0x36})==1);
    assert(owners.owner.activity_selection()==receipt);
    const auto sent=fixture_sent.size();owners.refresh();owners.recover();owners.pump(8);
    assert(!owners.latest_selection().selected&&!owners.latest_selection().repair&&owners.latest_selection().generation==standby_generation);
    assert(fixture_sent.size()==sent&&owners.owner.activity_selection()==receipt);
    // Another explicit Standby is a new user decision, even while already off.
    assert(owners.main.standby());owners.finish(-1);
    assert(!owners.latest_selection().repair&&owners.latest_selection().generation>standby_generation);
    assert(owners.owner.activity_selection()==receipt+1&&owners.count(4,15,{0x36})==2);
}
static void retired_owner_does_not_resume_admitted_activity(){
    RecoveryOwners owners;fixture_tick=0;fixture_async_autocomplete=false;
    assert(owners.main.request_activity(1)==ESP_OK);owners.pump(4);
    assert(owners.count(4,4,{0x36})==1&&owners.count(4,8,{0x44,0x6d})==0);
    latest_task=owners.owner_task;assert(owners.owner.retire_membership()==ESP_OK);
    fixture_complete_all(cec::TxResult::Success);fixture_async_autocomplete=true;owners.pump(8);
    assert(owners.count(4,8,{0x44,0x6d})==0&&owners.count(4,15,{0x86,0x20,0})==0);
    // The old controller's fresh network arrival is still unauthorized after
    // retirement, even though a temporary link fence preserves admitted work.
    auto intent=owners.latest_selection();assert(!owners.send(intent));owners.pump(4);
    assert(owners.count(4,8,{0x44,0x6d})==0&&!owners.owner.activity_setup_in_progress());
}
static void new_controller_boot_cancels_old_activity_and_restarts_generation(){
    RecoveryOwners owners;fixture_tick=0;fixture_async_autocomplete=false;
    assert(owners.main.request_activity(1)==ESP_OK);owners.pump(4);
    assert(owners.pending(4,0x36)==1&&owners.count(4,8,{0x44,0x6d})==0);
    const auto old=owners.latest_selection();assert(old.generation>1);
    owners.current_controller_boot=id<m::BootId>('9');
    auto fresh=old;fresh.generation=1;fresh.selected=false;fresh.repair=false;fresh.reselect=false;
    fresh.source.reset();fresh.audio.reset();fresh.display.reset();fresh.switch_device.reset();fresh.input=0;
    assert(owners.send(fresh));owners.pump(4);
    fixture_complete_all(cec::TxResult::Success);fixture_async_autocomplete=true;owners.pump(32);
    assert(!owners.owner.activity_setup_in_progress()&&owners.owner.snapshot().current_activity==-1);
    assert(owners.count(4,8,{0x44,0x6d})==0&&owners.count(4,15,{0x86,0x20,0})==0);
    const auto sent=fixture_sent.size();const auto receipt=owners.owner.activity_selection(),epoch=owners.owner.input_epoch();
    assert(owners.send(fresh));owners.pump(4);
    assert(fixture_sent.size()==sent&&owners.owner.activity_selection()==receipt&&owners.owner.input_epoch()==epoch);
    latest_task=owners.owner_task;
    m::OwnedAuthorization old_auth{owners.controller,owners.controller_boot,owners.member_boot,owners.current_link,owners.revision,0};
    assert(owners.owner.submit_owned(old,old_auth).code==m::AdmissionCode::unauthorized);
    owners.pump(4);assert(fixture_sent.size()==sent);
}
int main(){transport_admitted_selection_recovers_its_physical_recipe();owner_admitted_selection_survives_recovery(false);owner_admitted_selection_survives_recovery(true);same_target_selection_is_fresh_but_recovery_is_not();reselected_target_still_transitions_an_owner_that_missed_it();recovery_of_newer_queued_selection_preserves_order();conflicting_duplicate_and_stale_repair_are_rejected();standby_recovery_does_not_repeat_one_shot_repairs();new_controller_boot_cancels_old_activity_and_restarts_generation();never_admitted_selection_delivery();never_admitted_selection_is_not_current();never_admitted_selection_idle_progress();saved_edits_preserve_current();owner_revision_preserves_admitted_press_and_hold();scoped_save_preserves_admitted_frames_and_timed_release();retired_owner_does_not_resume_admitted_activity();std::puts("PASS scoped edits preserve current activity and holds; recovery delivers lost selections once and preserves admitted local progress; duplicate recovery is nonemitting; same-target and Standby decisions remain fresh; repairs stay one-shot");}
