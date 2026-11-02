// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#include "cec_hub_fixture.hpp"
using namespace amity;
namespace m=amity::system;
template<class T> T id(char digit){return *T::parse(std::string(32,digit));}
int main(){
    const auto group=id<m::ConsortiumId>('1');
    const auto controller=id<m::OctopusId>('2'),member=id<m::OctopusId>('3');
    const auto boot=id<m::BootId>('4');
    fixture_present=true;HubService hub;hub.set_system_identity(group,controller);hub.set_boot_identity(boot);assert(hub.start(1,2)==ESP_OK);
    const std::vector<m::ScopedSelector> selectors{{"display",{{group,controller,0,m::Side::front},"TV"}},{"source",{{group,member,0,m::Side::back},"Source"}}};
    struct Action{m::OwnedIntentKind kind;bool selected;uint64_t revision;};std::vector<Action> sent;
    bool publication_allowed=true;
    const auto sender=[&](const m::OwnedIntent& intent){
        assert(intent.config_revision==hub.executing_configuration_revision());
        sent.push_back({intent.kind,intent.selected,intent.config_revision});
        if(intent.kind==m::OwnedIntentKind::context&&!publication_allowed)return false;
        return true;
    };
    assert(hub.configure_scoped({{"Watch","display","source","","",0}},selectors,sender,controller,1)==ESP_OK);
    assert(hub.executing_configuration_revision()==0);run_one_command();fixture_finish_discovery(hub);
    assert(hub.select_activity(0));run_one_command();
    for(unsigned pass=0;pass<32&&hub.activity_setup_in_progress();++pass)run_idle(1);
    assert(hub.snapshot().current_activity==0&&!hub.activity_setup_in_progress());
    sent.clear();assert(hub.configure_scoped({{"Renamed","display","source","","",0}},selectors,sender,controller,2)==ESP_OK);
    assert(hub.executing_configuration_revision()==1);run_one_command();
    for(unsigned pass=0;pass<32&&hub.executing_configuration_revision()!=2;++pass)run_idle(1);
    assert(hub.executing_configuration_revision()==2&&hub.snapshot().current_activity==-1);
    // The transport's existing ready hook requests the new revision's
    // nonemitting context after configuration synchronization.
    assert(hub.refresh_owned_context(member,2));run_one_command();
    const auto cleanup=std::find_if(sent.begin(),sent.end(),[](const auto& action){return action.kind==m::OwnedIntentKind::activity&&!action.selected;});
    const auto context=std::find_if(sent.begin(),sent.end(),[](const auto& action){return action.kind==m::OwnedIntentKind::context&&action.revision==2;});
    assert(cleanup!=sent.end()&&cleanup->revision==1&&context!=sent.end()&&cleanup<context);
    sent.clear();const auto commands_before=command_waits.size();
    // The actual production ready hook calls this method from the TLS task.
    // Admission must not pump/wait for owner execution; that later command
    // captures current state and supplies the newly scoped context itself.
    assert(!hub.refresh_owned_context(member,0));assert(sent.empty()&&command_waits.size()==commands_before);
    assert(hub.refresh_owned_context(member,2));assert(sent.empty()&&command_waits.size()==commands_before);
    run_one_command();assert(sent.size()==1&&sent[0].kind==m::OwnedIntentKind::context&&sent[0].revision==2);
    sent.clear();publication_allowed=false;std::vector<m::OwnedContextResult> completed;
    assert(hub.refresh_owned_context(member,2,[&](auto result){completed.push_back(result);}));assert(completed.empty()&&sent.empty());
    run_one_command();assert(completed==std::vector<m::OwnedContextResult>{m::OwnedContextResult::failed}&&sent.size()==1);
    sent.clear();completed.clear();publication_allowed=true;
    // Local configuration can advance before the runtime publishes its new
    // session revision. An old queued recapture is stale, not a link failure.
    assert(hub.refresh_owned_context(member,1,[&](auto result){completed.push_back(result);}));assert(completed.empty());
    run_one_command();assert(completed==std::vector<m::OwnedContextResult>{m::OwnedContextResult::stale}&&sent.empty());
    assert(hub.configure_scoped({}, {},sender,controller,3)==ESP_OK);run_one_command();sent.clear();completed.clear();fixture_sent.clear();
    assert(hub.refresh_owned_context(member,3,[&](auto result){completed.push_back(result);}));assert(completed.empty()&&sent.empty());
    run_one_command();assert(completed==std::vector<m::OwnedContextResult>{m::OwnedContextResult::published}&&sent.empty()&&fixture_sent.empty());
    std::puts("PASS actual configuration boundary: old-recipe cleanup keeps old scope, new context uses new revision, and ready recapture admits owner IPC without waiting for execution");
}
