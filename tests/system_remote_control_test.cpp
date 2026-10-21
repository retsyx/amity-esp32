#include "cec_hub_fixture.hpp"
#include "amity/system_control.hpp"
#include "../main/swipe_sequence.hpp"
#include "../main/volume_sequence.hpp"
using namespace amity::system;
template<class T> T input_id(unsigned n) { char value[33]; std::snprintf(value, sizeof(value), "%032x", n); return *T::parse(value); }
namespace config_store {
esp_err_t persist_owner_configuration(const ConfigurationV2&, OctopusId, OctopusId, uint64_t, std::string&) { return ESP_OK; }
}
int main() {
    amity::SystemControl control;
    const auto owner = input_id<OctopusId>(2), authority = input_id<OctopusId>(3);
    control.set_identity(input_id<ConsortiumId>(1), owner); control.set_coordinator_authority(false, 1);
    assert(control.start(1, 2) == ESP_OK);
    fixture_sent.clear(); run_idle(2); assert(fixture_sent.empty()); // No transient legacy startup identity.
    config_store::ConfigurationV2 config; config.consortium_id = input_id<ConsortiumId>(1); config.primary_zone_id = input_id<ZoneId>(10);
    config.octopuses = {{owner, "Member", "", false, 1}, {authority, "Coordinator", "", false, 1}};
    config.zones = {{config.primary_zone_id, "Primary", "", false, {}}, {input_id<ZoneId>(11), "Secondary", "", false, {}}};
    config.controller_octopus_id=authority;
    assert(control.configure_member_slice(config, authority, 1) == ESP_OK);
    assert(!control.accepts_local_input());
    assert(control.remote_activity_generation()==0 && control.remote_activity_target()==-1);
    assert(control.request_activity(0)==ESP_ERR_INVALID_STATE);
    assert(control.request_activity(-1)==ESP_ERR_INVALID_STATE);
    assert(!control.press_key(1) && !control.press_key_for_activity(1,control.activity_generation()));
    assert(!control.release_key());
    assert(control.request_remote_activity(0)==ESP_ERR_INVALID_STATE);
    assert(control.request_remote_activity(0,1)==ESP_ERR_INVALID_STATE);
    assert(!control.press_remote_key(1) && !control.press_remote_key_for_activity(1,1,true));
    assert(!control.release_remote_key() && control.stop_remote_zone()==ESP_ERR_INVALID_STATE);
    assert(control.repair_remote_activity(1)==ESP_ERR_INVALID_STATE);
    assert(control.request_activity_v2(config.primary_zone_id,input_id<ActivityId>(40),input_id<MessageId>(41))==ESP_ERR_INVALID_STATE);
    assert(control.stop_zone_v2(config.primary_zone_id)==ESP_ERR_INVALID_STATE);
    InputIntent forged;forged.zone_id=config.primary_zone_id;forged.action=InputAction::stop_zone;
    assert(!control.submit_input_intent(forged));
    assert(control.transition_coordinator_authority(true,1)==ESP_ERR_INVALID_STATE);
    control.set_coordinator_authority(true,1);
    assert(!control.accepts_local_input()); // Configuration binds the original identity independently of caller role.
    config.config_revision=2;
    assert(control.configure_member_slice(config,authority,1)==ESP_OK);
    assert(control.configure_v2(config) == ESP_ERR_INVALID_STATE); // Passive member cannot become local coordinator.
    assert(control.retire_membership() == ESP_OK);
    assert(!control.snapshot().ready && control.execution_snapshot().applied_config_revision == 2); // Historical installed graph survives authority fence.
    for(const uint8_t mask:{uint8_t{0},uint8_t{16}}){
        amity::SystemControl optional;optional.set_identity(config.consortium_id,owner);optional.set_boot_identity(input_id<BootId>(90));optional.set_coordinator_authority(false,1);
        cec::BoardProfile profile{cec::BoardProfileId::Development5Tentacle,1,5,{},-1,-1};
        for(uint8_t id=0;id<5;++id){profile.ports[2*id]={id,cec::Side::Front,(mask&(1u<<id))?7:-1};profile.ports[2*id+1]={id,cec::Side::Back,(mask&(1u<<id))?8:-1};}
        assert(optional.start(profile)==ESP_OK&&optional.supported_tentacles()==5&&optional.configured_tentacles_mask()==mask);
        auto expanded=config;for(auto& board:expanded.octopuses)if(board.octopus_id==owner)board.supported_tentacles=5;
        fixture_sent.clear();assert(optional.configure_member_slice(expanded,authority,1)==ESP_OK);
        assert(optional.execution_snapshot().applied_config_revision==expanded.config_revision&&optional.snapshot().ready);
        const auto policies=optional.local_policy_snapshot();assert(policies.size()==(mask?2:0));
        for(const auto& policy:policies)assert(policy.port.tentacle_id==4&&policy.role==PortRole::monitor&&!policy.owned_logical_address);
        assert(std::none_of(fixture_sent.begin(),fixture_sent.end(),[](const auto& sent){return sent.frame.size>1&&(sent.frame.data[1]==0x44||sent.frame.data[1]==0x36||sent.frame.data[1]==0x82||sent.frame.data[1]==0x86||sent.frame.data[1]==0x04||sent.frame.data[1]==0x0d);}));
    }
    {
        fixture_present=true;fixture_tick=10000;
        amity::SystemControl original;original.set_identity(config.consortium_id,authority);original.set_coordinator_authority(true,1);assert(original.start(1,2)==ESP_OK);
        config_store::ConfigurationV2 initial;initial.consortium_id=config.consortium_id;initial.controller_octopus_id=authority;initial.primary_zone_id=input_id<ZoneId>(100);
        initial.octopuses={{authority,"Original controller","",false,1}};initial.zones={{initial.primary_zone_id,"TV","",false,{}}};
        CecPortRef back{initial.consortium_id,authority,0,Side::back};auto front=back;front.side=Side::front;
        std::vector<config_store::LegacyEvidence> evidence{{{back,"Source"},true,true,0x1000,true,true,false,1},{{front,"TV"},true,true,0,true,true,false,1}};
        auto random=[n=uint64_t{5000}](uint8_t* data,size_t size)mutable{std::fill(data,data+size,0);++n;for(size_t b=0;b<std::min<size_t>(size,8);++b)data[size-1-b]=uint8_t(n>>(8*b));return true;};
        config_store::ConfigurationV2 routed;std::vector<std::string> tasks;std::string error;
        assert(config_store::propose_local_activity(initial,{"First","TV","Source","TV","",0},evidence,random,routed,tasks,error)&&tasks.empty());
        const auto first=routed.activities[0].activity_id;auto second=routed.activities[0];second.activity_id=input_id<ActivityId>(200);second.zone_id=input_id<ZoneId>(201);second.label="Second screen";
        routed.activities.push_back(second);routed.zones.push_back({second.zone_id,"Second screen","",false,{second.activity_id}});routed.activity_order={first,second.activity_id};routed.external_slots.push_back({1,second.activity_id,false,false,{}});
        assert(original.configure_v2(routed)==ESP_OK);run_idle(300);
        fixture_send=[](int gpio,const cec::Frame& frame){if(frame.size>1&&frame.data[1]==0x8f){cec::Frame reply;reply.size=3;reply.data[0]=(frame.destination()<<4)|(frame.data[0]>>4);reply.data[1]=0x90;reply.data[2]=0;fixture_incoming.push_back({gpio,reply});}return true;};
        assert(original.request_remote_activity(0)==ESP_OK);run_idle(80);auto running=original.execution_snapshot();assert(running.sessions.size()==1&&running.sessions[0].activity_id==first);
        const auto audio_generation=original.remote_activity_generation();
        fixture_tick=1000; // Fast fake completions must not age unrelated discovery evidence between assertions.
        // Explicit mute/restore use the activity's audio component through
        // both ingress paths, even when its source is on another physical wire.
        for(const uint8_t audio_key:{uint8_t{0x65},uint8_t{0x66}})for(const bool remote:{false,true}) {
            fixture_sent.clear();
            assert(remote ? original.press_remote_key_for_activity(audio_key,original.remote_activity_generation()) :
                original.press_key_for_activity(audio_key,original.activity_generation()));
            run_idle(4);
            assert(std::count_if(fixture_sent.begin(),fixture_sent.end(),[&](const auto& sent){return sent.frame.size==3&&sent.frame.data[1]==0x44&&sent.frame.data[2]==audio_key&&sent.gpio==1&&sent.frame.destination()==0;})==1);
            assert(std::none_of(fixture_sent.begin(),fixture_sent.end(),[](const auto& sent){return sent.frame.size>1&&sent.frame.data[1]==0x44&&sent.gpio==2;}));
        }
        { // Actual owner admission and the sole driver FIFO: all messages,
          // including final release, drop at eight admitted messages.
            const auto saved_tick=fixture_tick;fixture_tick=0;
            const auto policies=original.local_policy_snapshot();
            const auto policy=std::find_if(policies.begin(),policies.end(),[](const auto& p){return std::any_of(p.bindings.begin(),p.bindings.end(),[](const auto& b){return b.binding.capability==Capability::volume||b.binding.capability==Capability::audio;});});assert(policy!=policies.end());
            const auto permission=std::find_if(policy->bindings.begin(),policy->bindings.end(),[](const auto& b){return b.binding.capability==Capability::volume||b.binding.capability==Capability::audio;});
            const auto& binding=permission->binding;
            const auto grant=std::find_if(policy->route_grants.begin(),policy->route_grants.end(),[&](const auto& g){return g.binding_id==binding.binding_id;});assert(grant!=policy->route_grants.end());
            AuthorizedCecOperation down{input_id<CommandId>(70000),OperationKind::press_key,binding.component_id,binding.binding_id,binding.endpoint,
                {policy->owner_boot_id,policy->coordinator_epoch,policy->config_revision,binding.domain_revision,binding.binding_revision,grant->session_id,grant->route_generation,policy->policy_generation},uint64_t(fixture_time)/1000+500,{}};
            down.arguments.key=0x41;down.arguments.repeat=true;down.arguments.hold_token=input_id<HoldToken>(70001);
            assert(!original.remote_key_active(down));
            assert(original.execute_authorized(down)==ESP_OK);assert(!original.remote_key_active(down)); // Queued is not yet processed.
            run_one_command();assert(original.remote_key_active(down)==std::optional<bool>{true});
            fixture_driver_paused=true;fixture_sent.clear();fixture_submitted.clear();fixture_fifo_dropped.clear();
            unsigned queued_queries=0;
            for(unsigned i=0;i<8;++i){auto query=down;query.command_id=input_id<CommandId>(70100+i);query.kind=OperationKind::query_state;query.arguments={};query.arguments.query_opcode=0x71;if(!original.admit_authorized(query))break;++queued_queries;}
            assert(queued_queries>=7); // An already-admitted held repeat may occupy the other normal slot.
            auto full=down;full.command_id=input_id<CommandId>(70200);full.kind=OperationKind::query_state;full.arguments={};full.arguments.query_opcode=0x71;assert(!original.admit_authorized(full));
            auto up=down;up.command_id=input_id<CommandId>(70201);up.kind=OperationKind::release_key;assert(original.admit_authorized(up));
            assert(fixture_fifo_dropped.back().frame.data[1]==0x45); // Release reaches the full driver's rejection/logging boundary.
            const auto dropped=fixture_fifo_dropped.size();
            auto tap=down;tap.command_id=input_id<CommandId>(70204);tap.arguments.repeat=false;
            assert(!original.admit_authorized(tap));
            assert(fixture_fifo_dropped.size()==dropped+1&&fixture_fifo_dropped[dropped].frame.data[1]==0x44); // A dropped press never submits its release.
            assert(original.remote_key_active(down,true)==std::optional<bool>{false});assert(!original.remote_key_active(down));
            auto never_processed=down;never_processed.command_id=input_id<CommandId>(70202);assert(!original.remote_key_active(never_processed));
            auto wrong_boot=down;wrong_boot.fence.owner_boot_id=input_id<BootId>(70203);assert(!original.remote_key_active(wrong_boot,true));
            fixture_driver_paused=false;run_idle(20);
            assert(std::count_if(fixture_submitted.begin(),fixture_submitted.end(),[](const auto& s){return s.frame.size>1&&s.frame.data[1]==0x71;})==static_cast<ptrdiff_t>(queued_queries));
            assert(std::count_if(fixture_submitted.begin(),fixture_submitted.end(),[](const auto& s){return s.frame.size>1&&s.frame.data[1]==0x45&&s.attempts==3;})==0);
            fixture_sent.clear();run_idle(4);assert(std::none_of(fixture_sent.begin(),fixture_sent.end(),[](const auto& s){return s.frame.size>1&&(s.frame.data[1]==0x45||s.frame.data[1]==0x44);}));
            auto newer=down;newer.command_id=input_id<CommandId>(70300);newer.arguments.hold_token=input_id<HoldToken>(70301);assert(original.admit_authorized(newer));
            for(unsigned i=0;i<32;++i)assert(original.press_key(0xff));assert(!original.press_key(0xff));
            // A late up for ended A is classified without queue admission;
            // it cannot cause overload cleanup to tear down currently-down B.
            assert(original.remote_key_active(down,true)==std::optional<bool>{false});assert(original.remote_key_active(newer,true)==std::optional<bool>{true});assert(!original.press_key(0xff));
            for(unsigned i=0;i<32;++i)run_one_command();assert(original.remote_key_active(newer,true)==std::optional<bool>{true});
            auto newer_up=newer;newer_up.command_id=input_id<CommandId>(70302);newer_up.kind=OperationKind::release_key;assert(original.admit_authorized(newer_up));run_idle(4);
            fixture_tick=saved_tick;
        }
        { // The actual owner preserves main's press-first admission for finite
          // audio taps and discrete power-on, including a dropped final release.
            fixture_time+=600000;run_idle(8); // Drain earlier audio-query deadlines.
            const auto saved_tick=fixture_tick;fixture_tick=0;
            const auto policies=original.local_policy_snapshot();
            std::vector<AuthorizedCecOperation> recipes;
            for(const auto kind:{OperationKind::press_key,OperationKind::ensure_power}) {
                bool found=false;
                for(const auto& policy:policies)for(const auto& permission:policy.bindings) {
                    if(found)continue;
                    const auto& binding=permission.binding;
                    const bool audio=kind==OperationKind::press_key&&(binding.capability==Capability::volume||binding.capability==Capability::audio);
                    const bool power=kind==OperationKind::ensure_power&&binding.capability==Capability::power&&permission.discrete_power_on&&
                        !(permission.image_view_on&&policy.role==PortRole::playback_proxy);
                    if(!audio&&!power)continue;
                    const auto grant=std::find_if(policy.route_grants.begin(),policy.route_grants.end(),[&](const auto& g){return g.binding_id==binding.binding_id;});
                    if(grant==policy.route_grants.end())continue;
                    AuthorizedCecOperation operation{input_id<CommandId>(70400+recipes.size()),kind,binding.component_id,binding.binding_id,binding.endpoint,
                        {policy.owner_boot_id,policy.coordinator_epoch,policy.config_revision,binding.domain_revision,binding.binding_revision,grant->session_id,grant->route_generation,policy.policy_generation},0,{}};
                    if(audio){operation.arguments.key=0x41;operation.arguments.hold_token=input_id<HoldToken>(70410);}
                    recipes.push_back(operation);found=true;
                }
                assert(found);
            }
            unsigned identity=70500;
            for(auto recipe:recipes)for(const unsigned occupied:{7u,8u}) {
                recipe.command_id=input_id<CommandId>(identity++);recipe.execution_expires_at_ms=uint64_t(fixture_time)/1000+500;
                fixture_driver_paused=true;fixture_sent.clear();fixture_submitted.clear();fixture_fifo_dropped.clear();
                for(unsigned i=0;i<occupied;++i) {
                    auto query=recipe;query.command_id=input_id<CommandId>(identity++);query.kind=OperationKind::query_state;query.arguments={};
                    assert(original.admit_authorized(query));
                }
                assert(bool(original.admit_authorized(recipe))==(occupied==7));
                assert(fixture_fifo_dropped.size()==1);
                assert(fixture_fifo_dropped[0].frame.data[1]==(occupied==7?0x45:0x44));
                assert(fixture_fifo_dropped[0].attempts==(occupied==7?3u:1u));
                fixture_driver_paused=false;run_idle(12);
                assert(std::count_if(fixture_submitted.begin(),fixture_submitted.end(),[](const auto& s){return s.frame.size>1&&s.frame.data[1]==0x8f;})==occupied);
                const auto key=recipe.kind==OperationKind::ensure_power?0x6d:0x41;
                assert(std::count_if(fixture_submitted.begin(),fixture_submitted.end(),[&](const auto& s){return s.frame.size==3&&s.frame.data[1]==0x44&&s.frame.data[2]==key&&s.attempts==1;})==(occupied==7?1:0));
                assert(std::none_of(fixture_submitted.begin(),fixture_submitted.end(),[](const auto& s){return s.frame.size>1&&s.frame.data[1]==0x45;}));
                fixture_sent.clear();fixture_time+=600000;run_idle(8);
                assert(fixture_fifo_dropped.size()==1);
                assert(std::none_of(fixture_sent.begin(),fixture_sent.end(),[](const auto& s){return s.frame.size>1&&(s.frame.data[1]==0x44||s.frame.data[1]==0x45||s.frame.data[1]==0x71);}));
            }
            fixture_tick=saved_tick;
        }
        // Four finite taps fill the only driver FIFO. A fifth intent
        // returns busy now; the executor retains no frames to retry later.
        fixture_driver_paused=true;fixture_sent.clear();
        for(unsigned tap=0;tap<4;++tap)assert(original.press_remote_key_for_activity(2,audio_generation));
        assert(!original.press_remote_key_for_activity(2,audio_generation)&&fixture_sent.empty());
        fixture_driver_paused=false;run_idle(8);
        assert(std::count_if(fixture_sent.begin(),fixture_sent.end(),[](const auto& sent){return sent.frame.size==3&&sent.frame.data[1]==0x44&&sent.frame.data[2]==2;})==4);
        assert(std::count_if(fixture_sent.begin(),fixture_sent.end(),[](const auto& sent){return sent.frame.size==2&&sent.frame.data[1]==0x45;})==4);
        fixture_sent.clear();run_idle(4);assert(std::none_of(fixture_sent.begin(),fixture_sent.end(),[](const auto& sent){return sent.frame.size>1&&sent.frame.data[1]==0x44;}));
        // Keep the unchanged original finite sequences on their tap path:
        // neither Siri navigation steps nor relative-volume steps create a hold.
        amity::SwipeSequence swipe;swipe.add(2,3,audio_generation);fixture_sent.clear();fixture_submitted.clear();
        for(unsigned step=0;step<3;++step){fixture_time+=250000;swipe.tick(fixture_time,audio_generation,[&](uint8_t key,uint32_t generation){return original.press_remote_key_for_activity(key,generation);});run_idle(4);}
        assert(swipe.idle());
        assert(std::count_if(fixture_sent.begin(),fixture_sent.end(),[](const auto& sent){return sent.frame.size==3&&sent.frame.data[1]==0x44&&sent.frame.data[2]==2;})==3);
        assert(std::count_if(fixture_sent.begin(),fixture_sent.end(),[](const auto& sent){return sent.frame.size==2&&sent.frame.data[1]==0x45;})==3);
        for(const auto& sent:fixture_submitted)if(sent.frame.size>1&&(sent.frame.data[1]==0x44||sent.frame.data[1]==0x45))assert(sent.attempts==(sent.frame.data[1]==0x44?1u:3u));
        fixture_sent.clear();run_idle(8);assert(std::none_of(fixture_sent.begin(),fixture_sent.end(),[](const auto& sent){return sent.frame.size>1&&sent.frame.data[1]==0x44;}));
        // Incoming keys reach their CEC queues while another key is awaiting
        // completion. Right's up ends only Right; Volume Up remains held.
        fixture_driver_paused=true;fixture_sent.clear();fixture_submitted.clear();
        unsigned admitted_keys=0;
        fixture_submit_admission=[&](int,const cec::Frame& frame){if(frame.size==3&&frame.data[1]==0x44)++admitted_keys;return true;};
        assert(original.press_remote_key_for_activity(0x41,audio_generation,true));
        assert(original.press_remote_key_for_activity(3,audio_generation,true));
        const auto overlap_selection=original.activity_selection();
        assert(original.request_remote_activity(99,audio_generation)==ESP_ERR_INVALID_ARG);
        assert(original.activity_selection()==overlap_selection);
        assert(original.release_remote_key(3));
        assert(fixture_sent.empty());
        assert(admitted_keys==2);fixture_submit_admission={};
        fixture_driver_paused=false;run_idle(8);fixture_sent.clear();run_idle(8);
        assert(std::any_of(fixture_sent.begin(),fixture_sent.end(),[](const auto& sent){return sent.frame.size==3&&sent.frame.data[1]==0x44&&sent.frame.data[2]==0x41;}));
        assert(std::none_of(fixture_sent.begin(),fixture_sent.end(),[](const auto& sent){return sent.frame.size==3&&sent.frame.data[1]==0x44&&sent.frame.data[2]==3;}));
        assert(std::none_of(fixture_sent.begin(),fixture_sent.end(),[](const auto& sent){return sent.gpio==1&&sent.frame.size==2&&sent.frame.data[1]==0x45;}));
        assert(original.press_remote_key_for_activity(3,audio_generation,true));run_idle(2);
        for(unsigned i=0;i<32;++i)assert(original.press_key(0xff));
        assert(!original.press_key(0xff));
        assert(original.release_remote_key(3)); // Reserved cleanup admission survives a full ordinary IPC queue.
        run_idle(4);fixture_sent.clear();run_idle(4);
        assert(std::any_of(fixture_sent.begin(),fixture_sent.end(),[](const auto& sent){return sent.frame.size==3&&sent.frame.data[1]==0x44&&sent.frame.data[2]==0x41;}));
        assert(std::none_of(fixture_sent.begin(),fixture_sent.end(),[](const auto& sent){return sent.frame.size==3&&sent.frame.data[1]==0x44&&sent.frame.data[2]==3;}));
        assert(original.release_remote_key(0x41));run_idle(8);
        amity::VolumeSequence volume;volume.start({3,original.activity_generation()});fixture_sent.clear();fixture_submitted.clear();
        for(unsigned step=0;step<3;++step){fixture_time+=250000;volume.tick(fixture_time,original.activity_generation(),[&](uint8_t key,uint32_t generation){return original.press_key_for_activity(key,generation);});run_idle(4);}
        assert(volume.idle());
        assert(std::count_if(fixture_sent.begin(),fixture_sent.end(),[](const auto& sent){return sent.frame.size==3&&sent.frame.data[1]==0x44&&sent.frame.data[2]==0x41;})==3);
        assert(std::count_if(fixture_sent.begin(),fixture_sent.end(),[](const auto& sent){return sent.frame.size==2&&sent.frame.data[1]==0x45;})==3);
        for(const auto& sent:fixture_submitted)if(sent.frame.size>1&&(sent.frame.data[1]==0x44||sent.frame.data[1]==0x45))assert(sent.attempts==(sent.frame.data[1]==0x44?1u:3u));
        fixture_sent.clear();run_idle(8);assert(std::none_of(fixture_sent.begin(),fixture_sent.end(),[](const auto& sent){return sent.frame.size>1&&sent.frame.data[1]==0x44;}));
        // Physical up appends final release immediately after already admitted
        // presses, without waiting for the preceding attempt's completion.
        fixture_driver_paused=true;fixture_sent.clear();assert(original.press_remote_key_for_activity(0x41,audio_generation,true));assert(original.release_remote_key());assert(fixture_sent.empty());
        fixture_driver_paused=false;run_idle(4);
        std::vector<uint8_t> short_hold;for(const auto& sent:fixture_sent)if(sent.frame.size>1&&(sent.frame.data[1]==0x44||sent.frame.data[1]==0x45))short_hold.push_back(sent.frame.data[1]);
        assert((short_hold==std::vector<uint8_t>{0x44,0x45}));
        assert(original.press_remote_key_for_activity(0x41,audio_generation,true));run_idle(3);fixture_driver_paused=true;run_idle(2);
        fixture_sent.clear();assert(original.release_remote_key());fixture_driver_paused=false;run_idle(4);
        assert(std::count_if(fixture_sent.begin(),fixture_sent.end(),[](const auto& sent){return sent.frame.size>1&&sent.frame.data[1]==0x44;})<=1);
        assert(std::count_if(fixture_sent.begin(),fixture_sent.end(),[](const auto& sent){return sent.frame.size==2&&sent.frame.data[1]==0x45;})==1);
        fixture_time+=600000;run_idle(4); // Drain the prior short stream's already-admitted audio query.
        fixture_sent.clear();fixture_submitted.clear();assert(original.press_remote_key_for_activity(0x41,audio_generation,true));run_idle(8);
        fixture_time+=5000000;run_idle(8);
        assert(std::count_if(fixture_sent.begin(),fixture_sent.end(),[](const auto& sent){return sent.frame.size==3&&sent.frame.data[1]==0x44&&sent.frame.data[2]==0x41;})>5);
        for(const auto& sent:fixture_sent)if(sent.frame.size==3&&sent.frame.data[1]==0x44)assert(sent.gpio==1&&sent.frame.destination()==0);
        for(const auto& sent:fixture_submitted)if(sent.frame.size>1&&sent.frame.data[1]==0x44)assert(sent.attempts==1);
        // Unrelated queued commands do not cancel an existing held key or
        // prevent its successful completion from admitting another repeat.
        assert(original.press_key(0xff));assert(original.press_key(0xff));fixture_sent.clear();command_waits.clear();
        run_one_command();run_one_command();run_idle(2);
        assert(std::any_of(fixture_sent.begin(),fixture_sent.end(),[](const auto& sent){return sent.frame.size==3&&sent.frame.data[1]==0x44&&sent.frame.data[2]==0x41;}));
        fixture_completion_ready=[](int){return false;};run_idle(3);fixture_completion_ready={};
        fixture_sent.clear();assert(original.press_remote_key_for_activity(0x41,audio_generation,true));run_idle(2);
        assert(std::none_of(fixture_sent.begin(),fixture_sent.end(),[](const auto& sent){return sent.frame.size==2&&sent.frame.data[1]==0x45;}));
        const auto advancing_tick=fixture_tick;fixture_tick=0;
        bool admit_release=false;uint64_t release_admitted_ms=0;unsigned release_attempts=0;
        fixture_submit_admission=[&](int,const cec::Frame& frame){if(frame.size>1&&frame.data[1]==0x45){++release_attempts;if(!admit_release)return false;release_admitted_ms=uint64_t(fixture_time)/1000;}return true;};
        fixture_sent.clear();fixture_submitted.clear();assert(original.release_remote_key());run_idle(3);
        fixture_time+=600000;run_idle(3);
        assert(release_attempts==1&&std::none_of(fixture_sent.begin(),fixture_sent.end(),[](const auto& sent){return sent.frame.size>1&&(sent.frame.data[1]==0x45||sent.frame.data[1]==0x71);})); // A full eight-slot FIFO drops final release once, with no delayed query or retry.
        admit_release=true;run_idle(2);assert(release_attempts==1);
        assert(original.press_remote_key_for_activity(0x41,audio_generation,true));run_idle(2);
        assert(original.release_remote_key());run_idle(2);assert(release_attempts==2&&release_admitted_ms==uint64_t(fixture_time)/1000);
        fixture_submit_admission={};fixture_completion_ready=[](int gpio){return gpio!=1;};
        fixture_time+=499000;run_idle(1);fixture_completion_ready={};run_idle(1);
        assert(std::none_of(fixture_sent.begin(),fixture_sent.end(),[](const auto& sent){return sent.frame.size>1&&sent.frame.data[1]==0x71;}));
        fixture_time+=1000;run_idle(2);
        assert(std::count_if(fixture_sent.begin(),fixture_sent.end(),[](const auto& sent){return sent.frame.size==2&&sent.frame.data[1]==0x71;})==1); // Actual Bus queue admission +500ms, even though wire completion was withheld until +499ms.
        fixture_tick=advancing_tick;
        assert(std::count_if(fixture_sent.begin(),fixture_sent.end(),[](const auto& sent){return sent.frame.size==2&&sent.frame.data[1]==0x45;})==1);
        for(const auto& sent:fixture_submitted)if(sent.frame.size>1&&sent.frame.data[1]==0x45)assert(sent.attempts==3);
        const auto successful_send=fixture_send;
        assert(original.press_remote_key_for_activity(0x41,audio_generation,true));run_idle(4); // Establish the hold before failing its next attempt.
        fixture_send=[successful_send](int gpio,const cec::Frame& frame){return frame.size>1&&frame.data[1]==0x44?false:successful_send(gpio,frame);};
        fixture_sent.clear();assert(original.press_remote_key_for_activity(0x41,audio_generation,true));run_idle(8);
        assert(std::count_if(fixture_sent.begin(),fixture_sent.end(),[](const auto& sent){return sent.frame.size==3&&sent.frame.data[1]==0x44;})==1);
        assert(std::count_if(fixture_sent.begin(),fixture_sent.end(),[](const auto& sent){return sent.frame.size==2&&sent.frame.data[1]==0x45;})==1); // Failure ends the last pressed key and its aggregate stream.
        fixture_sent.clear();assert(original.press_remote_key_for_activity(0x41,audio_generation,true));run_idle(8);
        assert(std::none_of(fixture_sent.begin(),fixture_sent.end(),[](const auto& sent){return sent.frame.size>1&&sent.frame.data[1]==0x44;})); // Unchanged physical down cannot restart after failure.
        fixture_sent.clear();original.release_remote_key();run_idle(4);
        assert(std::none_of(fixture_sent.begin(),fixture_sent.end(),[](const auto& sent){return sent.frame.size==2&&sent.frame.data[1]==0x45;}));
        fixture_sent.clear();assert(original.press_remote_key_for_activity(0x41,audio_generation,true));run_idle(8);
        assert(std::count_if(fixture_sent.begin(),fixture_sent.end(),[](const auto& sent){return sent.frame.size==3&&sent.frame.data[1]==0x44;})==1);
        assert(std::count_if(fixture_sent.begin(),fixture_sent.end(),[](const auto& sent){return sent.frame.size==2&&sent.frame.data[1]==0x45;})==1); // An admitted failed initial UCP also ends its stream.
        fixture_sent.clear();assert(original.press_remote_key_for_activity(0x41,audio_generation,true));run_idle(8);
        assert(std::none_of(fixture_sent.begin(),fixture_sent.end(),[](const auto& sent){return sent.frame.size>1&&sent.frame.data[1]==0x44;}));
        fixture_sent.clear();original.release_remote_key();run_idle(4);
        assert(std::none_of(fixture_sent.begin(),fixture_sent.end(),[](const auto& sent){return sent.frame.size==2&&sent.frame.data[1]==0x45;}));
        fixture_send=successful_send;
        assert(original.press_remote_key_for_activity(0x41,audio_generation,true));run_idle(4);
        fixture_submit_admission=[](int,const cec::Frame& frame){return frame.size<2||frame.data[1]!=0x44;};
        fixture_sent.clear();run_idle(8);
        assert(std::count_if(fixture_sent.begin(),fixture_sent.end(),[](const auto& sent){return sent.frame.size==2&&sent.frame.data[1]==0x45;})==1);
        fixture_submit_admission={};fixture_sent.clear();assert(original.press_remote_key_for_activity(0x41,audio_generation,true));run_idle(8);
        assert(std::none_of(fixture_sent.begin(),fixture_sent.end(),[](const auto& sent){return sent.frame.size>1&&sent.frame.data[1]==0x44;})); // Admission failure stops continuation; unchanged down cannot restart it.
        assert(!idle_waits.empty()&&std::all_of(idle_waits.begin(),idle_waits.end(),[](unsigned wait){return wait==pdMS_TO_TICKS(10);})); // Stale facade-held metadata cannot busy-spin the owner.
        fixture_sent.clear();original.release_remote_key();run_idle(4);
        assert(std::none_of(fixture_sent.begin(),fixture_sent.end(),[](const auto& sent){return sent.frame.size==2&&sent.frame.data[1]==0x45;}));
        const auto held_generation=original.remote_activity_generation();const bool held_admitted=held_generation&&original.press_remote_key_for_activity(1,held_generation,true);if(!held_admitted)std::fprintf(stderr,"original key rejected: generation=%u state=%llu context=%llu error=%s time=%lld\n",held_generation,(unsigned long long)original.execution_snapshot().state_revision,(unsigned long long)original.input_context().state_revision,original.execution_snapshot().error.c_str(),(long long)fixture_time);assert(held_admitted);run_idle(2);
        fixture_sent.clear();assert(original.request_remote_activity(1,held_generation)==ESP_OK);run_idle(80);running=original.execution_snapshot();
        assert(running.sessions.size()==1&&running.sessions[0].activity_id==second.activity_id&&running.sessions[0].zone_id==second.zone_id);
        assert(std::any_of(fixture_sent.begin(),fixture_sent.end(),[](const auto& sent){return sent.frame.size==2&&sent.frame.data[1]==0x45;}));
        fixture_sent.clear();assert(!original.press_remote_key_for_activity(1,held_generation,true));assert(original.request_remote_activity(0,held_generation)==ESP_ERR_INVALID_STATE);run_idle(2);assert(std::none_of(fixture_sent.begin(),fixture_sent.end(),[](const auto& sent){return sent.frame.size>1&&sent.frame.data[1]==0x44;}));
        const auto secondary_generation=original.remote_activity_generation();assert(secondary_generation!=held_generation&&original.remote_activity_target()==1);
        fixture_sent.clear();assert(original.press_remote_key_for_activity(1,secondary_generation,true));run_idle(2);assert(std::any_of(fixture_sent.begin(),fixture_sent.end(),[](const auto& sent){return sent.frame.size==3&&sent.frame.data[1]==0x44&&sent.frame.data[2]==1;}));
        auto reordered=routed;reordered.presentation_revision=2;reordered.activity_order={second.activity_id,first};assert(original.configure_v2(reordered)==ESP_OK);run_idle(2);
        assert(original.remote_activity_target()==0&&original.remote_activity_generation()!=secondary_generation);
        assert(original.release_remote_key());run_idle(2); // The original input controller releases on its next generation synchronization.
        fixture_sent.clear();assert(!original.press_remote_key_for_activity(1,secondary_generation,true));assert(original.request_remote_activity(0,secondary_generation)==ESP_ERR_INVALID_STATE);run_idle(2);assert(std::none_of(fixture_sent.begin(),fixture_sent.end(),[](const auto& sent){return sent.frame.size>1&&sent.frame.data[1]==0x44;}));
        assert(original.release_remote_key());
        const auto back_to_first=original.request_remote_activity(1);
        if(back_to_first!=ESP_OK)std::fprintf(stderr,"screen selection rejected: result=%d error=%s state=%llu generation=%u\n",back_to_first,original.execution_snapshot().error.c_str(),(unsigned long long)original.execution_snapshot().state_revision,original.remote_activity_generation());
        assert(back_to_first==ESP_OK);run_idle(80);running=original.execution_snapshot();assert(running.sessions.size()==1&&running.sessions[0].activity_id==first);
        assert(routed.external_slots[0].activity_id==reordered.external_slots[0].activity_id&&routed.external_slots[1].activity_id==reordered.external_slots[1].activity_id);
        const auto before_selection=original.activity_selection();
        unsigned selector_waits=0;fixture_delay=[&]{++selector_waits;};
        fixture_driver_paused=true;
        assert(original.request_remote_activity(0)==ESP_OK);
        fixture_delay={};
        assert(original.activity_selection()==before_selection+1);
        assert(selector_waits==0); // No input-task CEC drain or response wait.
        assert(original.release_remote_key(3)); // Input remains available with the transition's frames pending.
        fixture_driver_paused=false;run_idle(80);
        assert(original.remote_activity_target()==0);
        const auto before_repair=original.activity_selection();
        assert(original.repair_remote_activity(original.remote_activity_generation())==ESP_OK);run_idle(80);
        assert(original.activity_selection()==before_repair); // Long Power repairs without a new release gate.
        fixture_delay={};fixture_send={};fixture_present=false;
    }
    std::puts("PASS actuator-only member rejection and original-controller cross-screen activity/order/hold fencing");
}
