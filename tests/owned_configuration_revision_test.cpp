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
    struct Action{m::OwnedIntentKind kind;bool on;uint64_t revision;};std::vector<Action> sent;
    const auto sender=[&](const m::OwnedIntent& intent){
        assert(intent.config_revision==hub.executing_configuration_revision());
        sent.push_back({intent.kind,intent.on,intent.config_revision});
        if(intent.kind==m::OwnedIntentKind::source_input)assert(hub.owned_input_result(member,intent.generation,intent.sequence,true));
        return true;
    };
    assert(hub.configure_scoped({{"Watch","display","source","","",0}},selectors,sender,controller,1)==ESP_OK);
    assert(hub.executing_configuration_revision()==0);run_one_command();fixture_finish_discovery(hub);
    assert(hub.select_activity(0));run_one_command();run_idle(2);assert(hub.snapshot().current_activity==0);
    sent.clear();assert(hub.configure_scoped({{"Renamed","display","source","","",0}},selectors,sender,controller,2)==ESP_OK);
    assert(hub.executing_configuration_revision()==1);run_one_command();
    assert(hub.executing_configuration_revision()==2&&hub.snapshot().current_activity==-1);
    const auto cleanup=std::find_if(sent.begin(),sent.end(),[](const auto& action){return action.kind==m::OwnedIntentKind::power&&!action.on;});
    const auto context=std::find_if(sent.begin(),sent.end(),[](const auto& action){return action.kind==m::OwnedIntentKind::context&&action.revision==2;});
    assert(cleanup!=sent.end()&&cleanup->revision==1&&context!=sent.end()&&cleanup<context);
    std::puts("PASS actual configuration boundary: asynchronous admission retains executing revision, old-recipe cleanup keeps old scope, then new context is stamped with new revision");
}
