#include "intent_configuration_fixture.hpp"
#include "amity/consortium/owned_intent.hpp"
#include "../components/system_sync/system_sync.cpp"
#include <iostream>
#include <memory>

namespace fixture {
namespace s=amity::system;namespace c=amity::consortium;
uint64_t now=100;
bool task_ok=true,auth_allowed=true;
unsigned task_creations=0,authorization_checks=0;
c::RuntimeStatus role;
c::RuntimeControlHooks hooks;
std::vector<c::RuntimePeerStatus> peers;
std::vector<s::OwnedIntent> sent;
struct Packet {s::OctopusId owner;c::Kind kind;std::string payload;uint32_t ttl;};
std::vector<Packet> packets;
struct ResultPacket {s::OctopusId owner;s::LinkId link;std::string payload;uint32_t ttl;uint64_t revision;};
std::vector<ResultPacket> result_packets;
std::vector<std::pair<s::OctopusId,s::LinkId>> result_fences;
std::vector<s::OwnedAuthorization> deadline_queries;
s::LinkId current_result_link;
uint64_t refreshed_deadline=654321;
esp_err_t send_result=ESP_OK,report_result=ESP_OK;
std::vector<std::pair<uint64_t,std::string>> configuration_changes;
struct JournalMemory:Memory {
    bool fail_marker=false;
    bool write(const char* key,const std::string& value) override {
        if(fail_marker&&(std::string(key)=="cm0"||std::string(key)=="cm1")){++writes;return false;}
        return Memory::write(key,value);
    }
} memory;
std::unique_ptr<ConfigurationStore> journal;
void reset(){
    {std::lock_guard<std::mutex> guard(amity::state.mutex);
        amity::state.control=nullptr;amity::state.activating=false;amity::state.activation={};
        amity::state.reports.clear();amity::state.pending_reports.clear();amity::state.maintenance.clear();amity::state.published_revision=amity::state.owner_revision=0;}
    role={};role.coordinator=true;role.authority_ready=true;role.initialized=true;role.epoch=1;
    peers.clear();sent.clear();packets.clear();result_packets.clear();result_fences.clear();deadline_queries.clear();current_result_link={};configuration_changes.clear();hooks={};
    send_result=ESP_OK;report_result=ESP_OK;task_ok=true;auth_allowed=true;now=100;
    task_creations=authorization_checks=0;memory=JournalMemory{};journal=std::make_unique<ConfigurationStore>(memory);
}
amity::SystemControl control(bool member=false){
    amity::SystemControl result;result.configuration=direct_configuration();
    result.identity=member?id<s::OctopusId>(9):*result.configuration.controller_octopus_id;
    result.boot=id<s::BootId>(member?19:12);
    if(member){role.coordinator=false;role.member=true;role.authority_ready=false;}
    return result;
}
c::Json string_document(std::string text){c::Json value;value.type=c::Json::Type::string;value.string=std::move(text);return value;}
c::Json json(std::string_view text){c::Json value;c::JsonError error;if(!c::parse_json(text,value,error)){std::cerr<<"Invalid generated fixture JSON ("<<text.size()<<" bytes): "<<text<<"\n";assert(false);}return value;}
s::OwnedAdapterSnapshot report(s::OctopusId owner,s::BootId boot,uint8_t adapter=0){
    s::OwnedAdapterSnapshot value;value.owner=owner;value.boot=boot;value.adapter=adapter;
    value.ready=true;value.front_address=4;value.back_address=0;value.front_physical_address=0x1000;
    value.discovery_generation=3;value.setup_generation=3;value.setup_last_sequence=11;value.setup_pending=2;value.setup_replies_pending=true;value.devices.push_back({s::Side::back,"Source",4,4,0x1000,0x123456,true});return value;
}
c::RuntimePeerStatus remote(unsigned owner=9,unsigned boot=19,uint8_t capacity=4,uint8_t mask=0b1101){
    c::RuntimePeerStatus value;value.id=id<s::OctopusId>(owner);value.boot_id=id<s::BootId>(boot);
    value.supported_tentacles=capacity;value.configured_tentacles_mask=mask;value.ready=value.enrolled=true;return value;
}
s::OwnedIntent down(const amity::SystemControl& owner){
    s::OwnedIntent value;value.kind=s::OwnedIntentKind::key_down;
    value.target={{owner.consortium_id(),owner.octopus_id(),0,s::Side::back},"Source"};
    value.generation=7;value.sequence=11;value.key=0x41;value.repeat=true;value.token=id<s::HoldToken>(77);return value;
}
c::Envelope envelope(const amity::SystemControl& owner,s::OctopusId sender,const s::OwnedIntent& intent){
    c::Envelope value;value.kind=c::Kind::scoped_intent;value.consortium_id=owner.consortium_id();
    value.origin_octopus_id=sender;value.destination_octopus_id=owner.octopus_id();
    value.origin_boot_id=id<s::BootId>(12);value.destination_boot_id=owner.boot_identity();value.config_revision=23;
    value.payload=json(c::owned_intent_json(intent,id<s::ExecutionLeaseId>(80)));return value;
}
c::Envelope report_envelope(const s::OwnedAdapterSnapshot& value){
    c::Envelope result;result.kind=c::Kind::scoped_result;result.payload=json("{\"snapshot\":"+c::owned_snapshot_json(value)+"}");return result;
}
}

int64_t esp_timer_get_time(){return static_cast<int64_t>(fixture::now*1000);}
void esp_fill_random(void* target,size_t size){static unsigned next=100;auto* bytes=static_cast<uint8_t*>(target);std::fill(bytes,bytes+size,0);bytes[size-1]=static_cast<uint8_t>(++next);}
int xTaskCreate(TaskFunction_t,const char*,unsigned,void*,unsigned,TaskHandle_t*){++fixture::task_creations;return fixture::task_ok?pdPASS:pdFALSE;}
void vTaskDelete(TaskHandle_t){}
void vTaskDelay(unsigned ticks){fixture::now+=ticks;}
namespace amity::consortium {
RuntimeStatus runtime_status(){return fixture::role;}
std::vector<RuntimePeerStatus> runtime_peers(){return fixture::peers;}
void runtime_control_hooks(RuntimeControlHooks hooks){fixture::hooks=std::move(hooks);}
esp_err_t runtime_send_owned(const model::OwnedIntent& intent){fixture::sent.push_back(intent);return fixture::send_result;}
bool runtime_owned_authorized(const model::OwnedAuthorization&){++fixture::authorization_checks;return fixture::auth_allowed;}
esp_err_t runtime_send_control(model::OctopusId owner,Kind kind,std::string payload,uint32_t ttl){fixture::packets.push_back({owner,kind,std::move(payload),ttl});return fixture::report_result;}
esp_err_t runtime_send_owned_result(model::OctopusId owner,model::LinkId link,std::string payload,uint32_t ttl,uint64_t revision){fixture::result_packets.push_back({owner,link,std::move(payload),ttl,revision});return fixture::current_result_link&&fixture::current_result_link!=link?ESP_ERR_INVALID_STATE:fixture::report_result;}
void runtime_fence_owned_link(model::OctopusId owner,model::LinkId link){fixture::result_fences.push_back({owner,link});}
uint64_t runtime_owned_deadline(const model::OwnedAuthorization& auth){fixture::deadline_queries.push_back(auth);return fixture::refreshed_deadline;}
void runtime_configuration_changed(uint64_t revision,const std::string& hash){fixture::configuration_changes.push_back({revision,hash});}
}
namespace config_store {
esp_err_t stage_system_configuration(Configuration candidate,RevisionPair expected,std::string& error){return fixture::journal->stage(std::move(candidate),expected,error)==ConfigResult::ok?ESP_OK:ESP_FAIL;}
esp_err_t staged_system_configuration(Configuration& out){if(!fixture::journal->staged())return ESP_ERR_INVALID_STATE;out=*fixture::journal->staged();return ESP_OK;}
esp_err_t activate_system_configuration(Configuration& out,std::string& error){return fixture::journal->commit(out,error)==ConfigResult::ok?ESP_OK:ESP_FAIL;}
esp_err_t cancel_staged_configuration(){fixture::journal->cancel_stage();return ESP_OK;}
}

namespace {
using namespace fixture;
void startup_and_minimal_handshake(){
    reset();auto main=control();assert(amity::start_configuration_synchronization(main)==ESP_OK);
    assert(hooks.snapshot&&hooks.synchronize&&hooks.control&&hooks.disconnected&&hooks.ready&&main.sender&&main.observer&&main.authorizer&&main.deadline&&main.input_observer);
    assert(amity::start_configuration_synchronization(main)==ESP_OK&&task_creations==1);
    auto other=control();assert(amity::start_configuration_synchronization(other)==ESP_ERR_INVALID_STATE);
    const auto document=json(hooks.snapshot(id<s::OctopusId>(9)));
    assert(c::closed(document,{"controller","config_revision"}));
    assert(document.get("controller")->string==main.octopus_id().str()&&document.get("config_revision")->string=="1");
    assert(!document.get("activities")&&!document.get("components")&&!document.get("configuration"));
    assert(memory.writes==0&&main.configure_calls==0&&fixture::sent.empty());
    const auto ready_owner=id<s::OctopusId>(9);assert(hooks.ready(ready_owner)&&main.refreshed_context==std::vector<s::OctopusId>{ready_owner});
    main.refresh_result=false;assert(!hooks.ready(ready_owner)&&main.refreshed_context.size()==2&&memory.writes==0&&fixture::sent.empty());
    reset();auto member=control(true);const auto original=*member.configuration.controller_octopus_id;
    assert(amity::start_configuration_synchronization(member)==ESP_OK);
    const auto saved=serialize_configuration(member.configuration);
    auto accepted=string_document("{\"controller\":\""+original.str()+"\",\"config_revision\":\"37\"}");
    assert(hooks.synchronize(original,accepted)==c::Error::none&&member.bound_controller==original&&member.bound_revision==37);
    assert(memory.writes==0&&member.configure_calls==0&&serialize_configuration(member.configuration)==saved);
    assert(hooks.synchronize(id<s::OctopusId>(99),accepted)==c::Error::unauthorized_role);
    auto zero=string_document("{\"controller\":\""+original.str()+"\",\"config_revision\":\"0\"}");
    assert(hooks.synchronize(original,zero)==c::Error::unauthorized_role);
    auto forged=string_document("{\"controller\":\""+id<s::OctopusId>(99).str()+"\",\"config_revision\":\"1\"}");
    assert(hooks.synchronize(id<s::OctopusId>(99),forged)!=c::Error::none&&member.bound_controller==original);
    auto replica=accepted;replica.string.pop_back();replica.string+=",\"activities\":[]}";
    assert(hooks.synchronize(original,replica)!=c::Error::none);
    assert(hooks.synchronize(original,json("{}"))==c::Error::invalid_message);
    reset();task_ok=false;auto unavailable=control();assert(amity::start_configuration_synchronization(unavailable)==ESP_ERR_NO_MEM&&amity::state.control==nullptr);
    assert(!hooks.snapshot&&!hooks.synchronize&&!hooks.control&&!hooks.disconnected&&!hooks.ready);
    assert(!unavailable.sender&&!unavailable.observer&&!unavailable.authorizer&&!unavailable.deadline&&!unavailable.input_observer);
    // Failed initialization leaves no callback into the unstarted task, and a
    // later legitimate start can install a fresh set of callbacks.
    task_ok=true;assert(amity::start_configuration_synchronization(unavailable)==ESP_OK&&task_creations==2);
}
void intent_delivery_preserves_authorization(){
    reset();auto member=control(true);assert(amity::start_configuration_synchronization(member)==ESP_OK);
    const auto original=*member.configuration.controller_octopus_id;const auto intent=down(member);auto received=envelope(member,original,intent);
    const auto link=id<s::LinkId>(44);const uint64_t deadline=123456;
    assert(hooks.control(original,received,deadline,link)==c::Error::none&&member.submitted.size()==1&&authorization_checks==1);
    const auto& actual=member.submitted.back();assert(actual.target==intent.target&&actual.generation==7&&actual.sequence==11&&actual.token==intent.token&&actual.key==0x41&&actual.repeat);
    const auto& auth=member.authorizations.back();assert(auth.controller_id==original&&auth.controller_boot==received.origin_boot_id&&auth.owner_boot==member.boot&&auth.link_id==link&&auth.config_revision==23&&auth.deadline_ms==deadline);
    assert(auth.authority==s::OwnedAuthority::realtime);
    assert(member.deadline(auth)==refreshed_deadline&&deadline_queries.size()==1&&deadline_queries.back().link_id==link&&deadline_queries.back().deadline_ms==deadline);
    auto up=intent;up.kind=s::OwnedIntentKind::key_up;auto released=envelope(member,original,up);
    assert(hooks.control(original,released,deadline,link)==c::Error::none&&member.authorizations.back().authority==s::OwnedAuthority::cleanup);
    auto power=intent;power.kind=s::OwnedIntentKind::power;power.on=true;auto powered=envelope(member,original,power);
    assert(hooks.control(original,powered,deadline,link)==c::Error::none&&member.authorizations.back().authority==s::OwnedAuthority::control);
    auth_allowed=false;assert(hooks.control(original,received,deadline,link)==c::Error::unauthorized_role);auth_allowed=true;
    for(const auto& test:std::vector<std::pair<s::AdmissionCode,c::Error>>{{s::AdmissionCode::overloaded,c::Error::queue_full},{s::AdmissionCode::expired,c::Error::lease_expired},{s::AdmissionCode::stale,c::Error::stale_boot},{s::AdmissionCode::invalid,c::Error::invalid_message}}){member.admission=test.first;assert(hooks.control(original,received,deadline,link)==test.second);}member.admission=s::AdmissionCode::accepted;
    const auto calls=member.submitted.size();auto wrong=intent;wrong.target.port.octopus_id=id<s::OctopusId>(99);auto target=envelope(member,original,wrong);
    assert(hooks.control(original,target,deadline,link)==c::Error::target_not_local&&member.submitted.size()==calls);
    wrong=intent;wrong.target.port.consortium_id=id<s::ConsortiumId>(99);target=envelope(member,original,wrong);
    assert(hooks.control(original,target,deadline,link)==c::Error::target_not_local&&member.submitted.size()==calls);
    for(unsigned retired:{2u,4u,6u,12u,18u,20u,26u,35u,36u}){received.kind=static_cast<c::Kind>(retired);assert(hooks.control(original,received,deadline,link)==c::Error::unauthorized_role&&member.submitted.size()==calls);}
    received.kind=c::Kind::scoped_result;assert(hooks.control(original,received,deadline,link)==c::Error::unauthorized_role);
    role.member=false;role.coordinator=true;received.kind=c::Kind::scoped_intent;
    assert(hooks.control(original,received,deadline,link)==c::Error::unauthorized_role&&member.submitted.size()==calls);
}
void source_lookup_results_preserve_original_fallback(){
    reset();auto member=control(true);assert(amity::start_configuration_synchronization(member)==ESP_OK);
    const auto original=*member.configuration.controller_octopus_id;auto input=down(member);input.kind=s::OwnedIntentKind::source_input;input.setup_generation=3;
    const auto received=envelope(member,original,input);const auto link=id<s::LinkId>(44);const auto began=now;
    // Device presence and CEC admission are distinct in the original Hub:
    // a full TX FIFO must not incorrectly trigger receiver-input fallback.
    for(const auto& entry:std::vector<std::tuple<s::AdmissionCode,c::Error,bool>>{
        {s::AdmissionCode::accepted,c::Error::none,true},
        {s::AdmissionCode::unresolved,c::Error::none,false},
        {s::AdmissionCode::overloaded,c::Error::none,true}}){
        member.admission=std::get<0>(entry);member.source_present=std::get<2>(entry);const auto count=result_packets.size();
        assert(hooks.control(original,received,123456,link)==std::get<1>(entry)&&result_packets.size()==count+1&&now==began);
        assert(member.submitted.back().setup_generation==3);
        const auto& packet=result_packets.back();assert(packet.owner==original&&packet.link==link&&packet.ttl==1000);
        const auto payload=json(packet.payload);assert(c::valid_owned_result(payload));const auto* result=payload.get("input_result");
        assert(result&&result->get("generation")->string=="7"&&result->get("sequence")->string=="11"&&result->get("source_present")->boolean==std::get<2>(entry));
    }
    const auto reports=result_packets.size();for(auto code:{s::AdmissionCode::unauthorized,s::AdmissionCode::stale,s::AdmissionCode::expired,s::AdmissionCode::invalid}){
        member.admission=code;assert(hooks.control(original,received,123456,link)!=c::Error::none&&result_packets.size()==reports);
    }
    member.admission=s::AdmissionCode::overloaded;member.source_present.reset();const auto before_unknown=result_packets.size();
    assert(hooks.control(original,received,123456,link)==c::Error::queue_full&&result_packets.size()==before_unknown);
    assert(result_fences.back()==std::make_pair(original,link)); // IPC admission is not an owner lookup.
    member.admission=s::AdmissionCode::pending;member.source_present.reset();const auto deferred=result_packets.size();
    assert(hooks.control(original,received,123456,link)==c::Error::none&&result_packets.size()==deferred);
    const auto pending_auth=member.authorizations.back();assert(pending_auth.authority==s::OwnedAuthority::control);
    s::AdmissionResult located{s::AdmissionCode::accepted,{}};located.source_present=false;
    member.input_observer(input,pending_auth,located);
    assert(result_packets.back().revision==pending_auth.config_revision);
    assert(result_packets.size()==deferred+1&&result_packets.back().link==link);
    assert(!json(result_packets.back().payload).get("input_result")->get("source_present")->boolean);
    // Once the authenticated link is replaced, an old asynchronous lookup
    // result must fail on that exact link instead of reaching the replacement.
    current_result_link=id<s::LinkId>(45);const auto before_replace=result_fences.size();
    member.input_observer(input,pending_auth,located);
    assert(result_packets.back().link==link&&result_fences.size()==before_replace+1&&result_fences.back()==std::make_pair(original,link));
    current_result_link={};s::AdmissionResult stale{s::AdmissionCode::stale,{}};
    const auto before_stale=result_packets.size();member.input_observer(input,pending_auth,stale);
    assert(result_packets.size()==before_stale&&result_fences.back()==std::make_pair(original,link));
    member.admission=s::AdmissionCode::accepted;member.source_present=true;report_result=ESP_FAIL;
    const auto failed=result_fences.size();assert(hooks.control(original,received,123456,link)==c::Error::queue_full);
    assert(result_fences.size()==failed+1&&result_fences.back()==std::make_pair(original,link));
    const auto obsolete_packets=result_packets.size(),obsolete_fences=result_fences.size();
    assert(hooks.synchronize(original,string_document("{\"controller\":\""+original.str()+"\",\"config_revision\":\"24\"}"))==c::Error::none);
    member.input_observer(input,pending_auth,located);
    assert(result_packets.size()==obsolete_packets&&result_fences.size()==obsolete_fences); // Old-revision continuations never disconnect the live replacement scope.
    reset();auto main=control();assert(amity::start_configuration_synchronization(main)==ESP_OK);peers={remote()};const auto owner=peers[0].id;
    c::Envelope result;result.kind=c::Kind::scoped_result;result.origin_boot_id=peers[0].boot_id;
    result.payload=json("{\"input_result\":{\"generation\":\"7\",\"sequence\":\"11\",\"source_present\":false}}");
    assert(c::valid_owned_result(result.payload));assert(hooks.control(owner,result,123456,link)==c::Error::none&&main.input_results.size()==1);
    assert(main.input_results.back()==std::make_tuple(owner,uint64_t{7},uint64_t{11},false));
    const auto calls=main.input_results.size();assert(hooks.control(id<s::OctopusId>(99),result,123456,link)==c::Error::invalid_message&&main.input_results.size()==calls);
    result.origin_boot_id=id<s::BootId>(99);assert(hooks.control(owner,result,123456,link)==c::Error::invalid_message&&main.input_results.size()==calls);result.origin_boot_id=peers[0].boot_id;
    main.lookup_result=false;assert(hooks.control(owner,result,123456,link)==c::Error::queue_full&&main.input_results.size()==calls+1);
    amity::flush_reports();assert(main.input_results.size()==calls+1&&sent.empty()); // No hidden continuation replay.
}
void reports_and_exact_disconnect(){
    reset();auto main=control();assert(amity::start_configuration_synchronization(main)==ESP_OK);
    peers={remote(9,19,2,0b11)};const auto owner=peers[0].id;const auto oldboot=peers[0].boot_id;auto first=report(owner,oldboot);
    assert(hooks.synchronize(owner,string_document("{\"adapters\":["+c::owned_snapshot_json(first)+"]}"))==c::Error::none&&main.updates.size()==1);
    assert(main.updates[0].setup_generation==3&&main.updates[0].setup_last_sequence==11&&main.updates[0].setup_pending==2&&main.updates[0].setup_replies_pending);
    auto available=amity::discovered_equipment();assert(available.size()==1&&available[0].endpoint.port.octopus_id==owner&&available[0].endpoint.logical_address==4&&available[0].owner_boot_id==oldboot);
    const auto count=main.updates.size();auto bad=first;bad.owner=id<s::OctopusId>(98);auto value=report_envelope(bad);
    assert(hooks.control(owner,value,100,id<s::LinkId>(1))==c::Error::invalid_message&&main.updates.size()==count);
    bad=first;bad.boot=id<s::BootId>(98);value=report_envelope(bad);assert(hooks.control(owner,value,100,id<s::LinkId>(1))==c::Error::invalid_message);
    bad=first;bad.adapter=peers[0].supported_tentacles;value=report_envelope(bad);assert(hooks.control(owner,value,100,id<s::LinkId>(1))==c::Error::invalid_message);
    value=report_envelope(first);assert(hooks.control(id<s::OctopusId>(98),value,100,id<s::LinkId>(1))==c::Error::invalid_message);
    main.update_result=false;const auto before_overload=main.updates.size();
    assert(hooks.control(owner,value,100,id<s::LinkId>(1))==c::Error::invalid_message&&main.updates.size()==before_overload+1);
    main.update_result=true;
    amity::set_owner_maintenance(owner,true);assert(hooks.control(owner,value,100,id<s::LinkId>(1))==c::Error::invalid_message);assert(amity::discovered_equipment().front().stale);amity::set_owner_maintenance(owner,false);
    auto second=first;second.adapter=1;value=report_envelope(second);assert(hooks.control(owner,value,100,id<s::LinkId>(1))==c::Error::none);
    const auto newboot=id<s::BootId>(29);peers[0].boot_id=newboot;auto fresh=first;fresh.boot=newboot;fresh.devices[0].logical_address=8;value=report_envelope(fresh);assert(hooks.control(owner,value,100,id<s::LinkId>(2))==c::Error::none);
    const auto oldlink=id<s::LinkId>(70);const auto sent_before=sent.size();const auto updates_before=main.updates.size();
    main.fence_result=false;assert(!hooks.disconnected(owner,oldboot,oldlink));
    assert(main.updates.size()==updates_before&&amity::discovered_equipment().size()==2&&main.fences.back()==std::make_pair(owner,oldlink));
    main.fence_result=true;assert(hooks.disconnected(owner,oldboot,oldlink));
    assert(main.fences.back()==std::make_pair(owner,oldlink)&&sent.size()==sent_before);
    available=amity::discovered_equipment();assert(available.size()==1&&available[0].owner_boot_id==newboot&&available[0].endpoint.logical_address==8);
    const auto& missing=main.updates.back();assert(missing.boot==oldboot&&missing.adapter==1&&!missing.ready&&!missing.discovering&&missing.devices.empty());
    assert(hooks.disconnected(owner,newboot,id<s::LinkId>(71))&&amity::discovered_equipment().empty());
    main.update_result=true;value=report_envelope(fresh);assert(hooks.control(owner,value,100,id<s::LinkId>(2))==c::Error::none);
    main.fence_result=false;amity::set_owner_maintenance(owner,true);amity::forget_owner(owner);
    assert(amity::discovered_equipment().empty()&&amity::state.reports.empty()&&!amity::state.maintenance.count(owner));
    assert(serialize_configuration(main.configuration).find("\"enabled\"")==std::string::npos&&memory.writes==0);
}
void report_coalescing_and_no_intent_backlog(){
    reset();auto member=control(true);assert(amity::start_configuration_synchronization(member)==ESP_OK);
    auto local=report(member.identity,member.boot);member.snapshots={local};const auto snapshot=json(hooks.snapshot({}));assert(c::closed(snapshot,{"adapters"})&&snapshot.get("adapters")->array.size()==1);
    const auto original=*member.configuration.controller_octopus_id;peers={remote(2,12,1,1)};assert(peers[0].id==original);
    member.observer(local);local.discovery_generation=4;member.observer(local);assert(amity::state.pending_reports.size()==1);
    report_result=ESP_FAIL;amity::flush_reports();assert(packets.size()==1&&amity::state.pending_reports.size()==1&&sent.empty());
    report_result=ESP_OK;amity::flush_reports();assert(packets.size()==2&&amity::state.pending_reports.empty());
    assert(packets.back().kind==c::Kind::scoped_result&&packets.back().owner==original&&packets.back().ttl==1000);
    auto body=json(packets.back().payload);s::OwnedAdapterSnapshot actual;assert(c::parse_owned_snapshot(*body.get("snapshot"),actual)&&actual.discovery_generation==4&&actual.setup_generation==3&&actual.setup_last_sequence==11&&actual.setup_pending==2&&actual.setup_replies_pending);
    reset();auto main=control();assert(amity::start_configuration_synchronization(main)==ESP_OK);auto intent=down(main);intent.target.port.octopus_id=id<s::OctopusId>(9);
    send_result=ESP_FAIL;assert(!main.sender(intent)&&sent.size()==1);amity::flush_reports();assert(sent.size()==1);
    amity::set_owner_maintenance(intent.target.port.octopus_id,true);assert(!main.sender(intent)&&sent.size()==1);
    amity::set_owner_maintenance(intent.target.port.octopus_id,false);send_result=ESP_OK;assert(main.sender(intent)&&sent.size()==2);
    assert(sent.back().generation==intent.generation&&sent.back().sequence==intent.sequence&&sent.back().token==intent.token);
}
void scans_and_saved_intent_without_peer_ack(){
    reset();auto main=control();assert(amity::start_configuration_synchronization(main)==ESP_OK);
    peers={remote(),remote(10,20,3,0b010),remote(11,21,1,1),remote(12,22,1,1)};peers[2].ready=false;peers[3].enrolled=false;
    assert(amity::request_equipment_scan()==ESP_OK&&main.local_scan_calls==1);
    const std::vector<std::pair<s::OctopusId,uint8_t>> expected={{id<s::OctopusId>(9),0},{id<s::OctopusId>(9),2},{id<s::OctopusId>(9),3},{id<s::OctopusId>(10),1}};
    assert(main.remote_scans==expected);main.remote_scan_result=false;assert(amity::request_equipment_scan()==ESP_ERR_INVALID_STATE);
    main.local_input=false;const auto scans=main.local_scan_calls;assert(amity::request_equipment_scan()==ESP_ERR_INVALID_STATE&&main.local_scan_calls==scans);main.local_input=true;
    peers.clear();main.configuration.octopuses.push_back({id<s::OctopusId>(9),"offline","offline",false,4});
    const auto source=main.configuration.activities[0].source;for(auto& item:main.configuration.components)if(item.record.component_id==source)item.record.qualified_names[0].port.octopus_id=id<s::OctopusId>(9);
    Configuration active;commit(*journal,main.configuration,{},active);main.configuration=active;
    auto candidate=active;candidate.activities[0].audio=candidate.activities[0].source;candidate.activities[0].label="Renamed";candidate.activities[0].label_override=true;main.auto_apply=false;const auto began=now;const auto request=id<s::MessageId>(100);
    assert(amity::activate_distributed_configuration(candidate,active.revisions(),request)==ESP_OK&&now==began);
    assert(configuration_changes.empty()); // Admission cannot invalidate old-recipe cleanup.
    const auto status=amity::configuration_activation_status();assert(status.request_id==request&&status.phase=="configuring"&&status.config_revision==2&&status.presentation_revision==2);
    main.applied.applied_config_revision=status.config_revision;
    assert(amity::configuration_activation_status().phase=="configuring");
    assert(configuration_changes.empty());
    main.applied.applied_presentation_revision=status.presentation_revision;
    assert(amity::configuration_activation_status().phase=="applied");
    assert(configuration_changes.size()==1&&configuration_changes[0].first==status.config_revision);
    assert(main.configuration.activities[0].audio==candidate.activities[0].source&&packets.empty()&&sent.empty()&&configuration_changes.size()==1);
    assert(serialize_configuration(main.configuration).find("\"enabled\"")==std::string::npos);
    const auto saved=serialize_configuration(main.configuration);const auto committed=memory.values;candidate=main.configuration;candidate.activities[0].audio=candidate.activities[0].display;
    memory.fail_marker=true;assert(amity::activate_distributed_configuration(candidate,main.configuration.revisions(),id<s::MessageId>(101))!=ESP_OK);memory.fail_marker=false;
    assert(serialize_configuration(main.configuration)==saved&&configuration_changes.size()==1&&amity::configuration_activation_status().phase=="failed");
    for(const auto& item:committed)if(item.first=="cm0"||item.first=="cm1")assert(memory.values[item.first]==item.second);
    ConfigurationStore recovered(memory);Configuration after;std::string error;assert(recovered.recover(after,error)==ConfigResult::ok&&serialize_configuration(after)==saved);
}
void saved_owner_capacity_does_not_shrink(){
    reset();auto main=control();main.configuration.octopuses.push_back({id<s::OctopusId>(9),"saved owner","saved owner",false,4});
    const auto source=main.configuration.activities[0].source;
    for(auto& component:main.configuration.components)if(component.record.component_id==source){component.record.qualified_names[0].port.octopus_id=id<s::OctopusId>(9);component.record.qualified_names[0].port.tentacle_id=3;}
    Configuration active;commit(*journal,main.configuration,{},active);main.configuration=active;
    assert(amity::start_configuration_synchronization(main)==ESP_OK);peers={remote(9,19,1,1)};
    const auto saved=serialize_configuration(main.configuration);const auto writes=memory.writes;
    amity::adopt_owners();
    assert(main.configure_calls==0&&memory.writes==writes&&configuration_changes.empty()&&serialize_configuration(main.configuration)==saved);
}
void removal_commit_boundary(){
    reset();auto main=control();const auto removed=id<s::OctopusId>(9);
    main.configuration.octopuses.push_back({removed,"Member","Member",false,1});
    for(auto& component:main.configuration.components)if(component.record.component_id==main.configuration.activities.front().source)component.record.qualified_names[0].port.octopus_id=removed;
    Configuration active;commit(*journal,main.configuration,{},active);main.configuration=active;
    assert(amity::start_configuration_synchronization(main)==ESP_OK);
    Configuration candidate;std::vector<s::ActivityId> affected;std::string error;assert(propose_octopus_removal(active,removed,candidate,affected,error));
    unsigned revoked=0;const auto revoke=[&]{assert(journal->staged());++revoked;return ESP_OK;};
    assert(amity::activate_distributed_configuration(candidate,{99,99},id<s::MessageId>(130),revoke)!=ESP_OK&&revoked==0);
    main.configure_result=ESP_ERR_NO_MEM;
    assert(amity::activate_distributed_configuration(candidate,active.revisions(),id<s::MessageId>(131),revoke)!=ESP_OK&&revoked==0);
    main.configure_result=ESP_OK;memory.fail_marker=true;
    assert(amity::activate_distributed_configuration(candidate,active.revisions(),id<s::MessageId>(132),revoke)!=ESP_OK&&revoked==1);
    assert(amity::configuration_activation_status().phase=="failed"&&serialize_configuration(main.configuration)==serialize_configuration(active));
    memory.fail_marker=false;
    assert(amity::activate_distributed_configuration(candidate,active.revisions(),id<s::MessageId>(133),revoke)==ESP_OK&&revoked==2);
    assert(amity::configuration_activation_status().phase=="applied"&&serialize_configuration(main.configuration).find(removed.str())==std::string::npos&&main.configuration.activities.empty());
}
}
int main(){startup_and_minimal_handshake();intent_delivery_preserves_authorization();source_lookup_results_preserve_original_fallback();reports_and_exact_disconnect();report_coalescing_and_no_intent_backlog();scans_and_saved_intent_without_peer_ack();saved_owner_capacity_does_not_shrink();removal_commit_boundary();std::cout<<"PASS actual owner synchronization: minimal handshake, derived authority/exact auth/deadline, deferred owner lookup/exact-link fencing, scoped snapshots, exact disconnect/no replay, callback cleanup, sparse scans/capacity retention and real journal save/applied revisions independent of offline peers, removal only after stage and owner reservation\n";}
