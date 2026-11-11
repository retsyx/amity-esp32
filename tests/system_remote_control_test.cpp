#include "cec_hub_fixture.hpp"
#include "intent_configuration_fixture.hpp"
#include "amity/system_control.hpp"
#include "key_request.hpp"
#include "json_input.hpp"
#include <iostream>
#include <openssl/evp.h>
static bool fail_policy_hash=false;
extern "C" unsigned char* amity_test_sha256(const unsigned char* data,size_t size,unsigned char* digest){
    if(fail_policy_hash)return nullptr;
    unsigned length=0;
    return EVP_Digest(data,size,digest,&length,EVP_sha256(),nullptr)==1&&length==32?digest:nullptr;
}
using namespace amity::system;
static void drain(unsigned count=120){run_idle(static_cast<int>(count));}
static std::string outcome(const amity::SystemControl& control,MessageId id){for(const auto& result:control.execution_snapshot().activity_outcomes)if(result.request_id==id)return result.stage;assert(false);return {};}
static size_t presses(uint8_t key){return std::count_if(fixture_sent.begin(),fixture_sent.end(),[&](const auto& s){return s.frame.size==3&&s.frame.data[1]==0x44&&s.frame.data[2]==key;});}
static bool decode_key(const std::string& body,web_admin::KeyRequest& request){std::unique_ptr<cJSON,decltype(&cJSON_Delete)> root(web_admin::parse_json_input(body),cJSON_Delete);return web_admin::parse_key_request(root.get(),request);}
int main(){
    {amity::HubService unstarted;
        const auto rejected=[&]{assert(!unstarted.press_key(0x41,true)&&!unstarted.press_key_for_activity(0x41,1,true)&&!unstarted.press_timed_key(0x41,1,1,0));};
        rejected();assert(unstarted.start(1,1)==ESP_ERR_INVALID_ARG);rejected();
    }
    web_admin::KeyRequest key_request;
    for(const auto* invalid:{"{}","[]","null",R"({"key":-1})",R"({"key":256})",R"({"key":1.5})",R"({"key":true})",R"({"key":"65"})",R"({"key":65,"key":66})",R"({"key":65,"duration_ms":1,"duration_ms":2})",R"({"key":65,"duration_ms":-1})",R"({"key":65,"duration_ms":5001})",R"({"key":65,"duration_ms":1.5})",R"({"key":65,"duration_ms":1e100})",R"({"key":65,"duration_ms":null})",R"({"key":65,"duration_ms":true})",R"({"key":65,"duration_ms":"5000"})",R"({"key":65,"action":"press"})"})assert(!decode_key(invalid,key_request));
    assert(decode_key(R"({"key":0})",key_request)&&key_request.key==0&&key_request.duration_ms==0);
    assert(decode_key(R"({"key":255,"duration_ms":5000})",key_request)&&key_request.key==255&&key_request.duration_ms==5000);
    fixture_present=true;fixture_tick=100000;
    auto config=direct_configuration();const auto boot=id<BootId>(90);
    amity::SystemControl original;original.set_identity(config.consortium_id,*config.controller_octopus_id);original.set_boot_identity(boot);assert(original.start(1,2)==ESP_OK);
    unsigned configuration_events=0;
    original.on_configuration_applied([&]{
        ++configuration_events;
        const auto applied=original.applied_configuration();const auto saved=original.active_configuration();
        assert(saved&&applied.config_revision==saved->config_revision&&applied.presentation_revision==saved->presentation_revision);
        assert(applied.policy_hash==executable_policy_hash(*saved));
    });
    assert(original.configure(config)==ESP_OK);
    assert(configuration_events==0&&original.applied_configuration().config_revision==0);
    drain(1000);assert(configuration_events==1);
    const auto initial_configuration=original.active_configuration();
    assert(initial_configuration&&initial_configuration.get()==original.active_configuration().get());
    assert(original.accepts_local_input()&&original.active_configuration());
    // A valid candidate whose hash cannot be computed must not reach either
    // persistence or live publication, including equipment unused by activities.
    auto failed_hash_candidate=config;
    fixture_equipment(failed_hash_candidate,"Unused player","source",Side::back);
    ++failed_hash_candidate.config_revision;
    std::string hash_candidate_error;assert(validate_configuration(failed_hash_candidate,hash_candidate_error));
    unsigned failed_hash_commits=0;
    fail_policy_hash=true;
    assert(original.configure(failed_hash_candidate,[&]{++failed_hash_commits;return ESP_OK;})==ESP_FAIL);
    fail_policy_hash=false;
    assert(failed_hash_commits==0&&original.active_configuration().get()==initial_configuration.get());
    drain(4);assert(configuration_events==1&&original.execution_snapshot().applied_policy_hash==executable_policy_hash(config));
    const auto first=config.activities[0].activity_id;
    auto generation=original.remote_activity_generation();const auto selection=original.activity_selection();
    assert(original.request_remote_activity(0,generation+1)==ESP_ERR_INVALID_STATE&&original.activity_selection()==selection);
    assert(original.request_remote_activity(0,generation)==ESP_OK);drain();assert(original.snapshot().current_activity==0&&original.execution_snapshot().sessions[0].activity_id==first);
    const auto playback=original.playback_state();
    assert(configuration_events==1&&playback.current_activity==0&&playback.activity_id==first&&original.external_activity_index(playback)==0);
    generation=original.remote_activity_generation();const auto selected=original.activity_selection();
    assert(original.request_remote_activity(0,generation)==ESP_OK&&original.activity_selection()==selected+1);drain();
    const auto repair=original.activity_selection();assert(original.repair_remote_activity(original.remote_activity_generation())==ESP_OK);drain();assert(original.activity_selection()==repair);
    // An accepted same-target selection must execute before its UI operation
    // completes; later selections do not rewrite a latched terminal outcome.
    const auto same=id<MessageId>(750);assert(original.request_activity(config.primary_zone_id,first,same,original.execution_snapshot().state_revision)==ESP_OK);assert(outcome(original,same)=="admitted");drain();assert(outcome(original,same)=="applied");
    const auto superseded=id<MessageId>(751);assert(original.request_activity(config.primary_zone_id,first,superseded,original.execution_snapshot().state_revision)==ESP_OK);assert(original.stop_remote_zone()==ESP_OK);drain();assert(outcome(original,superseded)=="rejected"&&outcome(original,same)=="applied");assert(original.request_remote_activity(0)==ESP_OK);drain();
    // Normal button-up affects one key only and advances neither activity nor
    // the accepted-selection gate. Both initial presses can enter their CEC
    // FIFOs while the first key's completion remains outstanding.
    generation=original.remote_activity_generation();const auto before_keys=original.activity_selection();
    fixture_async_autocomplete=false;fixture_sent.clear();assert(original.press_remote_key_for_activity(0x41,generation,true));assert(original.press_remote_key_for_activity(3,generation,true));drain(5);
    assert(std::count_if(fixture_sent.begin(),fixture_sent.end(),[](const auto& s){return s.frame.size==3&&s.frame.data[1]==0x44;})==2);
    assert(original.release_remote_key(3));drain(4);assert(original.remote_activity_generation()==generation&&original.activity_selection()==before_keys);
    fixture_async_autocomplete=true;fixture_complete_all();drain(12);fixture_sent.clear();drain(12);assert(presses(0x41)>0&&presses(3)==0);assert(original.release_remote_key(0x41));drain(8);
    // A duration belongs to one complete HTTP request. The caller can vanish:
    // the firmware releases the exact key before returning any response.
    fixture_sent.clear();assert(decode_key(R"({"key":65,"duration_ms":5000})",key_request));
    assert(web_admin::submit_key_request(original,key_request,[&](uint16_t duration){
        assert(duration==5000);drain(8);assert(presses(0x41)>1);
        // Another independent physical key remains held after the timed key ends.
        assert(original.press_remote_key(3,true));drain(8);
    }));drain(8);fixture_sent.clear();drain(12);assert(presses(0x41)==0&&presses(3)>0);assert(original.release_remote_key(3));drain(8);
    fixture_sent.clear();assert(decode_key(R"({"key":0,"duration_ms":0})",key_request));
    assert(web_admin::submit_key_request(original,key_request,[](uint16_t){assert(false);}));drain(12);assert(presses(0)==1);
    fixture_sent.clear();assert(decode_key(R"({"key":0})",key_request));
    assert(web_admin::submit_key_request(original,key_request,[](uint16_t){assert(false);}));drain(12);assert(presses(0)==1);
    fixture_sent.clear();assert(decode_key(R"({"key":65,"duration_ms":1})",key_request));
    bool unwound=false;try{web_admin::submit_key_request(original,key_request,[&](uint16_t duration){assert(duration==1);drain(8);throw 7;});}catch(int){unwound=true;}assert(unwound);drain(8);fixture_sent.clear();drain(12);assert(presses(0x41)==0);
    // Explicit raw key 255 cleanup must not use the facade's legacy all-key sentinel.
    assert(original.press_remote_key(0x41,true));drain(8);assert(decode_key(R"({"key":255,"duration_ms":1})",key_request));
    assert(web_admin::submit_key_request(original,key_request,[&](uint16_t){drain(8);}));drain(8);fixture_sent.clear();drain(12);assert(presses(0x41)>0);assert(original.release_remote_key(0x41));drain(8);
    // A setup-discarded timed down has no later release to affect a fresh key.
    assert(original.request_remote_activity(0)==ESP_OK&&original.activity_setup_in_progress());
    assert(decode_key(R"({"key":65,"duration_ms":5000})",key_request));
    assert(web_admin::submit_key_request(original,key_request,[](uint16_t){assert(false);}));drain();
    assert(original.press_remote_key(0x41,true));drain(8);fixture_sent.clear();drain(12);assert(presses(0x41)>0);assert(original.release_remote_key(0x41));drain(8);
    // Same-target selection does not advance activity generation, but its
    // selection receipt still fences the old request's eventual cleanup.
    const auto same_generation=original.remote_activity_generation();
    assert(web_admin::submit_key_request(original,key_request,[&](uint16_t){
        drain(8);assert(original.request_remote_activity(0)==ESP_OK);drain();
        assert(!original.activity_setup_in_progress()&&original.remote_activity_generation()==same_generation);
        assert(original.press_remote_key(0x41,true));drain(8);
    }));drain(8);fixture_sent.clear();drain(12);assert(presses(0x41)>0);assert(original.release_remote_key(0x41));drain(8);
    // Repair keeps both selection identifiers, but begins a new input epoch.
    // Cleanup from the old HTTP wait must not end a newly admitted hold.
    const auto repair_generation=original.remote_activity_generation(),repair_selection=original.activity_selection();
    assert(web_admin::submit_key_request(original,key_request,[&](uint16_t){
        drain(8);assert(original.repair_remote_activity(repair_generation)==ESP_OK);drain();
        assert(original.remote_activity_generation()==repair_generation&&original.activity_selection()==repair_selection);
        assert(original.press_remote_key(0x41,true));drain(8);
    }));drain(8);fixture_sent.clear();drain(12);assert(presses(0x41)>0);assert(original.release_remote_key(0x41));drain(8);
    const auto captured_epoch=original.remote_input_epoch();
    assert(original.repair_remote_activity(repair_generation)==ESP_OK);drain();assert(original.remote_input_epoch()!=captured_epoch);
    assert(original.press_remote_key(0x41,true));drain(8);
    assert(original.press_timed_remote_key(0x41,repair_generation,repair_selection,captured_epoch));
    assert(original.release_timed_remote_key(0x41,repair_generation,repair_selection,captured_epoch));drain(8);fixture_sent.clear();drain(12);
    assert(presses(0x41)>0);assert(original.release_remote_key(0x41));drain(8);
    assert(web_admin::submit_key_request(original,key_request,[&](uint16_t){
        drain(8);assert(original.stop_remote_zone()==ESP_OK);drain();assert(original.request_remote_activity(0)==ESP_OK);drain();
        assert(original.remote_activity_generation()!=same_generation);
        assert(original.press_remote_key(0x41,true));drain(8);
    }));drain(8);fixture_sent.clear();drain(12);assert(presses(0x41)>0);assert(original.release_remote_key(0x41));drain(8);
    // A timed down already queued when a newer selection arrives is discarded
    // at owner execution, and its old cleanup cannot end the new selection's key.
    assert(web_admin::submit_key_request(original,key_request,[&](uint16_t){
        assert(original.request_remote_activity(0)==ESP_OK);drain();fixture_sent.clear();
        assert(presses(0x41)==0&&original.press_remote_key(0x41,true));drain(8);
    }));drain(8);fixture_sent.clear();drain(12);assert(presses(0x41)>0);assert(original.release_remote_key(0x41));drain(8);
    // Full ordinary IPC cannot lose the exact conditional cleanup or turn it
    // into an unfenced raw-key emergency release.
    assert(web_admin::submit_key_request(original,key_request,[&](uint16_t){drain(8);for(unsigned i=0;i<32;++i)assert(original.press_key(0xff));assert(!original.press_key(0xff));}));drain();fixture_sent.clear();drain(12);assert(presses(0x41)==0);
    // Rejected down admission creates neither a wait nor a later up which
    // could terminate an already registered physical key of the same value.
    assert(original.press_remote_key(0x41,true));drain(8);
    for(unsigned i=0;i<32;++i)assert(original.press_key(0xff));
    assert(!web_admin::submit_key_request(original,key_request,[](uint16_t){assert(false);}));drain();fixture_sent.clear();drain(12);assert(presses(0x41)>0);assert(original.release_remote_key(0x41));drain(8);
    // Positive Power must reserve the original Standby selection and arm its
    // input gate immediately, just like the zero-duration Power path.
    const auto power_selection=original.activity_selection();
    assert(decode_key(R"({"key":64,"duration_ms":100})",key_request));
    assert(web_admin::submit_key_request(original,key_request,[&](uint16_t duration){
        assert(duration==100&&original.activity_selection()==power_selection+1&&original.activity_setup_in_progress());drain();assert(original.snapshot().current_activity==-1);
    }));drain();assert(original.snapshot().current_activity==-1);assert(original.request_remote_activity(0)==ESP_OK);drain();
    assert(decode_key(R"({"key":64,"duration_ms":0})",key_request)&&web_admin::submit_key_request(original,key_request,[](uint16_t){assert(false);}));drain();assert(original.snapshot().current_activity==-1);assert(original.request_remote_activity(0)==ESP_OK);drain();
    assert(original.press_remote_key(0x41,true));drain(8);
    assert(original.begin_maintenance()==ESP_OK&&!original.accepts_local_input());
    assert(decode_key(R"({"key":65,"duration_ms":100})",key_request));
    assert(!web_admin::submit_key_request(original,key_request,[](uint16_t){assert(false);}));original.end_maintenance();drain();assert(original.accepts_local_input()&&!original.snapshot().activity_settling);
    // A stable ID request uses presentation order; integration slots stay fixed.
    auto reordered=config;auto second=config.activities[0];second.activity_id=id<ActivityId>(800);second.label="Other screen";reordered.activities.push_back(second);reordered.zones[0].activity_order.push_back(second.activity_id);reordered.activity_order={second.activity_id,first};reordered.external_slots.push_back({1,second.activity_id,false,false,{}});reordered.config_revision=2;reordered.presentation_revision=2;
    assert(original.configure(reordered)==ESP_OK);
    assert(original.execution_snapshot().sessions[0].activity_id==first&&original.external_activity_index()==0);
    assert(original.execution_snapshot().applied_config_revision==0);
    drain();const auto request=id<MessageId>(801);assert(original.request_activity(second.zone_id,first,request,original.execution_snapshot().state_revision)==ESP_OK);drain();assert(original.snapshot().current_activity==1&&original.execution_snapshot().sessions[0].activity_id==first);assert(original.external_activity(0)->activity_id==first&&original.external_activity_index()==0);assert(outcome(original,same)=="applied");
    // The actual facade rejects changes/removal before invoking persistence.
    unsigned protected_writes=0;
    auto active_edit=reordered;active_edit.config_revision=3;active_edit.activities[0].label="Changed current";
    assert(original.configure(active_edit,[&]{++protected_writes;return ESP_OK;})!=ESP_OK&&protected_writes==0);
    auto active_delete=reordered;active_delete.config_revision=3;
    active_delete.activities.erase(active_delete.activities.begin());
    active_delete.activity_order.erase(std::remove(active_delete.activity_order.begin(),active_delete.activity_order.end(),first),active_delete.activity_order.end());
    for(auto& zone:active_delete.zones)zone.activity_order.erase(std::remove(zone.activity_order.begin(),zone.activity_order.end(),first),zone.activity_order.end());
    active_delete.external_slots[0].tombstone=true;
    assert(original.configure(active_delete,[&]{++protected_writes;return ESP_OK;})!=ESP_OK&&protected_writes==0);
    const auto retained=original.snapshot();const auto retained_selection=original.activity_selection(),retained_input=original.remote_input_epoch();
    auto sparse=reordered;sparse.config_revision=3;sparse.presentation_revision=3;sparse.external_slots[0].slot=7;
    sparse.activities[1].label="Updated inactive";sparse.activities[1].source.reset();sparse.activity_order={first,second.activity_id};
    fixture_sent.clear();assert(original.configure(sparse)==ESP_OK);
    assert(original.snapshot().current_activity==1&&original.execution_snapshot().sessions[0].activity_id==first&&original.external_activity_index()==7);
    drain();
    assert(original.snapshot().current_activity==0&&original.execution_snapshot().sessions[0].activity_id==first);
    assert(original.snapshot().activity_generation==retained.activity_generation&&original.activity_selection()==retained_selection&&original.remote_input_epoch()==retained_input);
    assert(std::none_of(fixture_sent.begin(),fixture_sent.end(),[](const auto& frame){const auto op=frame.frame.data[1];return op==0x36||op==0x44||op==0x04||op==0x0d||op==0x86;}));
    assert(original.press_remote_key(3));drain();assert(presses(3)==1);
    assert(original.request_activity(7)==ESP_OK);drain();assert(original.snapshot().current_activity==0&&original.external_activity_index()==7);assert(original.request_activity(0)==ESP_ERR_INVALID_ARG);
    assert(original.request_activity(second.zone_id,first,id<MessageId>(802),original.execution_snapshot().state_revision+1)==ESP_ERR_INVALID_STATE);
    // The original algorithm decides what a missing AVR/source means. Saved
    // selections do not acquire a persistent unavailable state or lose fields.
    assert(original.stop_remote_zone()==ESP_OK);drain();
    auto offline=switch_configuration(sparse,"Offline console");offline.config_revision=4;
    const auto remote=id<OctopusId>(70);offline.octopuses.push_back({remote,"amity-offline","amity-offline",false,1});for(auto& c:offline.components)if(c.record.label=="AVR")for(auto& name:c.record.qualified_names)name.port.octopus_id=remote;
    const auto saved=serialize_configuration(offline);assert(original.configure(offline)==ESP_OK);drain();const auto console=std::find_if(offline.activities.begin(),offline.activities.end(),[](const auto& a){return a.label=="Offline console";});assert(console!=offline.activities.end());assert(original.request_activity(console->zone_id,console->activity_id,id<MessageId>(803),original.execution_snapshot().state_revision)==ESP_OK);drain(1000);assert(original.active_configuration()&&serialize_configuration(*original.active_configuration())==saved);assert(original.activity_target()==2);
    // Configuration persistence runs only after ordinary IPC capacity has been
    // reserved. A full queue and a failed commit leave the active intent intact.
    for(unsigned i=0;i<32;++i)assert(original.press_key(0xff));assert(!original.press_key(0xff));unsigned writes=0;auto changed=offline;changed.config_revision=5;changed.activities[0].switch_input=7;
    assert(original.configure(changed,[&]{++writes;return ESP_OK;})==ESP_ERR_NO_MEM&&writes==0);assert(serialize_configuration(*original.active_configuration())==saved);
    assert(original.begin_maintenance()!=ESP_OK&&original.accepts_local_input());drain();
    assert(original.configure(changed,[&]{++writes;return ESP_FAIL;})==ESP_FAIL&&writes==1);assert(serialize_configuration(*original.active_configuration())==saved);
    // Equal activity labels are insufficient to acknowledge a selector-only
    // update: the owner task must have installed its exact configuration.
    auto relocated=offline;relocated.config_revision=5;for(auto& component:relocated.components)if(component.record.label=="AVR")component.record.qualified_names[0].osd_name="AVR2";
    assert(original.configure(relocated,[&]{++protected_writes;return ESP_OK;})!=ESP_OK&&protected_writes==0);
    assert(original.stop_remote_zone()==ESP_OK);drain();
    assert(original.configure(relocated)==ESP_OK);assert(original.execution_snapshot().applied_config_revision!=relocated.config_revision);drain();assert(original.execution_snapshot().applied_config_revision==relocated.config_revision);
    auto labels=relocated;++labels.presentation_revision;labels.activities[0].label="Renamed";assert(original.configure(labels)==ESP_OK);assert(original.execution_snapshot().applied_presentation_revision==0);drain();assert(original.execution_snapshot().applied_presentation_revision==labels.presentation_revision);
    // Readers retain a coherent immutable version across later publications.
    assert(initial_configuration.get()!=original.active_configuration().get());
    assert(serialize_configuration(*initial_configuration)==serialize_configuration(config));
    assert(original.execution_snapshot().applied_policy_hash==executable_policy_hash(labels));
    // A foreign owner's lookup and loss never stall the main's local recipe.
    auto interrupted_config=direct_configuration();interrupted_config.octopuses.push_back({remote,"amity-offline","amity-offline",false,1});for(auto& component:interrupted_config.components)if(component.record.component_id==interrupted_config.activities[0].source)component.record.qualified_names[0].port.octopus_id=remote;
    amity::SystemControl interrupted;interrupted.set_identity(interrupted_config.consortium_id,*interrupted_config.controller_octopus_id);interrupted.set_boot_identity(id<BootId>(92));bool available=true;unsigned jobs=0;
    interrupted.set_owned_sender([&](const auto& intent){if(intent.kind==OwnedIntentKind::activity)++jobs;return available;});assert(interrupted.start(1,2)==ESP_OK&&interrupted.configure(interrupted_config)==ESP_OK);drain(1000);
    const auto broken=id<MessageId>(805);assert(interrupted.request_activity(interrupted_config.primary_zone_id,interrupted_config.activities[0].activity_id,broken,interrupted.execution_snapshot().state_revision)==ESP_OK);drain();
    assert(jobs==1&&interrupted.snapshot().current_activity==0&&!interrupted.snapshot().activity_settling&&outcome(interrupted,broken)=="applied");
    available=false;interrupted.fence_owned(remote,id<LinkId>(93));drain();assert(interrupted.snapshot().current_activity==0&&outcome(interrupted,broken)=="applied");
    // A headless member cannot originate activity, remote or integration work.
    amity::SystemControl member;member.set_identity(config.consortium_id,remote);member.set_boot_identity(id<BootId>(91));member.set_coordinator_authority(false,1);assert(member.start(1,2)==ESP_OK);assert(!member.accepts_local_input()&&member.remote_activity_generation()==0);assert(member.request_remote_activity(0)==ESP_ERR_INVALID_STATE&&!member.press_remote_key(1)&&!member.release_remote_key());assert(member.request_activity(config.primary_zone_id,first,id<MessageId>(804))==ESP_ERR_INVALID_STATE);assert(!web_admin::submit_key_request(member,key_request,[](uint16_t){assert(false);}));
    assert(member.configure_passive_metadata(config)==ESP_OK);const auto previous_metadata=member.active_configuration();
    fail_policy_hash=true;assert(member.configure_passive_metadata(failed_hash_candidate)==ESP_FAIL);fail_policy_hash=false;
    assert(member.active_configuration().get()==previous_metadata.get());
    assert(member.configure_passive_metadata(failed_hash_candidate)==ESP_OK);
    assert(serialize_configuration(*member.active_configuration())==serialize_configuration(failed_hash_candidate));
    std::cout<<"PASS actual activity facade: generation and gate/repair parity, independent key-up, stable-ID ordered selection, offline AVR/switch intent and IPC-before-persistence\n";
}
