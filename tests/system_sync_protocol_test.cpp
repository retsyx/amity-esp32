#include "intent_configuration_fixture.hpp"
#include "amity/consortium/owned_intent.hpp"
#include "../components/system_sync/system_sync.cpp"
#include <iostream>
#include <memory>
#include <thread>
#include <condition_variable>

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
std::function<void()> on_report_send;
bool require_retry_lock=false,require_report_lock=false;
void require_state_lock(){
    bool held=false;std::thread contender([&]{held=!amity::state.mutex.try_lock();if(!held)amity::state.mutex.unlock();});contender.join();assert(held);
}
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
        amity::state.reports.clear();amity::state.pending_reports.clear();amity::state.retry_reports={};amity::state.maintenance.clear();amity::state.published_revision=amity::state.owner_revision=0;}
    role={};role.coordinator=true;role.authority_ready=true;role.initialized=true;role.epoch=1;
    peers.clear();sent.clear();packets.clear();result_packets.clear();result_fences.clear();deadline_queries.clear();current_result_link={};configuration_changes.clear();hooks={};
    send_result=ESP_OK;report_result=ESP_OK;task_ok=true;auth_allowed=true;now=100;
    on_report_send={};
    require_retry_lock=require_report_lock=false;
    system_sync_test_log::messages.clear();
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
    value.discovery_generation=3;value.devices.push_back({s::Side::back,"Source",4,4,0x1000,0x123456,true});return value;
}
c::RuntimePeerStatus remote(unsigned owner=9,unsigned boot=19,uint8_t capacity=4,uint8_t mask=0b1101){
    c::RuntimePeerStatus value;value.id=id<s::OctopusId>(owner);value.boot_id=id<s::BootId>(boot);
    value.supported_tentacles=capacity;value.configured_tentacles_mask=mask;value.ready=value.enrolled=true;return value;
}
s::OwnedIntent down(const amity::SystemControl& owner){
    s::OwnedIntent value;value.kind=s::OwnedIntentKind::key_edges;
    value.target={{owner.consortium_id(),owner.octopus_id(),0,s::Side::back},"Source"};
    value.generation=7;value.edge_count=1;value.edges[0]={0x41,true};return value;
}
c::Envelope envelope(const amity::SystemControl& owner,s::OctopusId sender,const s::OwnedIntent& intent){
    c::Envelope value;value.kind=c::Kind::scoped_intent;value.consortium_id=owner.consortium_id();
    value.origin_octopus_id=sender;value.destination_octopus_id=owner.octopus_id();
    value.origin_boot_id=id<s::BootId>(12);value.destination_boot_id=owner.boot_identity();value.config_revision=23;
    value.payload=json(c::owned_intent_json(intent));return value;
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
std::vector<RuntimePeerStatus> runtime_peers(){if(fixture::require_report_lock)fixture::require_state_lock();return fixture::peers;}
void runtime_control_hooks(RuntimeControlHooks hooks){fixture::hooks=std::move(hooks);}
esp_err_t runtime_send_owned(const model::OwnedIntent& intent){fixture::sent.push_back(intent);return fixture::send_result;}
bool runtime_owned_authorized(const model::OwnedAuthorization&){++fixture::authorization_checks;return fixture::auth_allowed;}
esp_err_t runtime_send_control(model::OctopusId owner,Kind kind,std::string payload,uint32_t ttl){fixture::packets.push_back({owner,kind,std::move(payload),ttl});if(fixture::on_report_send)fixture::on_report_send();return fixture::report_result;}
esp_err_t runtime_send_owned_result(model::OctopusId owner,model::LinkId link,const model::OwnedAdapterSnapshot& snapshot,uint32_t ttl,uint64_t revision){
    const auto payload="{\"snapshot\":"+owned_snapshot_json(snapshot)+"}";
    if(!link)return runtime_send_control(owner,Kind::scoped_result,payload,ttl);
    fixture::result_packets.push_back({owner,link,payload,ttl,revision});if(fixture::on_report_send)fixture::on_report_send();return fixture::current_result_link&&fixture::current_result_link!=link?ESP_ERR_INVALID_STATE:fixture::report_result;
}
std::optional<model::LinkId> runtime_link_id(model::OctopusId owner,model::BootId boot){
    if(fixture::require_retry_lock)fixture::require_state_lock();
    for(const auto& p:fixture::peers)if(p.ready&&p.id==owner&&p.boot_id==boot&&fixture::current_result_link)return fixture::current_result_link;return {};
}
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
    assert(hooks.snapshot&&hooks.synchronize&&hooks.control&&hooks.disconnected&&hooks.ready&&hooks.restart_session&&main.sender&&main.observer&&main.authorizer&&main.deadline);
    assert(amity::start_configuration_synchronization(main)==ESP_OK&&task_creations==1);
    auto other=control();assert(amity::start_configuration_synchronization(other)==ESP_ERR_INVALID_STATE);
    const auto document=json(hooks.snapshot(id<s::OctopusId>(9)));
    assert(c::closed(document,{"controller","config_revision"}));
    assert(document.get("controller")->string==main.octopus_id().str()&&document.get("config_revision")->string=="1");
    assert(!document.get("activities")&&!document.get("components")&&!document.get("configuration"));
    assert(memory.writes==0&&main.configure_calls==0&&fixture::sent.empty());
    const auto ready_owner=id<s::OctopusId>(9);assert(hooks.ready(ready_owner,1,{})&&main.refreshed_context==std::vector<s::OctopusId>{ready_owner});
    main.refresh_result=false;assert(!hooks.ready(ready_owner,1,{})&&main.refreshed_context.size()==2&&memory.writes==0&&fixture::sent.empty());
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
    assert(!hooks.snapshot&&!hooks.synchronize&&!hooks.control&&!hooks.disconnected&&!hooks.ready&&!hooks.restart_session);
    assert(!unavailable.sender&&!unavailable.observer&&!unavailable.authorizer&&!unavailable.deadline);
    // Failed initialization leaves no callback into the unstarted task, and a
    // later legitimate start can install a fresh set of callbacks.
    task_ok=true;assert(amity::start_configuration_synchronization(unavailable)==ESP_OK&&task_creations==2);
}
void intent_delivery_preserves_authorization(){
    reset();auto member=control(true);assert(amity::start_configuration_synchronization(member)==ESP_OK);
    const auto original=*member.configuration.controller_octopus_id;const auto intent=down(member);auto received=envelope(member,original,intent);
    const auto link=id<s::LinkId>(44);const uint64_t deadline=123456;
    assert(hooks.control(original,received,deadline,link)==c::Error::none&&member.submitted.size()==1&&authorization_checks==1);
    const auto& actual=member.submitted.back();assert(actual.target.port.octopus_id==member.octopus_id()&&actual.target.port.consortium_id==member.consortium_id()&&actual.target.osd_name.empty()&&actual.generation==7&&actual.edge_count==1&&actual.edges[0].key==0x41&&actual.edges[0].down);
    const auto& auth=member.authorizations.back();assert(auth.controller_id==original&&auth.controller_boot==received.origin_boot_id&&auth.owner_boot==member.boot&&auth.link_id==link&&auth.config_revision==23&&auth.deadline_ms==deadline);
    assert(auth.authority==s::OwnedAuthority::input);
    assert(member.deadline(auth)==refreshed_deadline&&deadline_queries.size()==1&&deadline_queries.back().link_id==link&&deadline_queries.back().deadline_ms==deadline);
    auto up=intent;up.edges[0].down=false;auto released=envelope(member,original,up);
    assert(hooks.control(original,released,deadline,link)==c::Error::none&&member.authorizations.back().authority==s::OwnedAuthority::cleanup);
    auto power=intent;power.kind=s::OwnedIntentKind::activity;power.selected=true;power.source=intent.target;auto powered=envelope(member,original,power);
    assert(hooks.control(original,powered,deadline,link)==c::Error::none&&member.authorizations.back().authority==s::OwnedAuthority::control);
    auto tap=intent;tap.edge_count=2;tap.edges[1]={tap.edges[0].key,false};const auto pair_calls=member.submitted.size();
    assert(hooks.control(original,envelope(member,original,tap),deadline,link)==c::Error::none&&member.submitted.size()==pair_calls+1&&member.submitted.back().edge_count==2&&member.authorizations.back().authority==s::OwnedAuthority::input);
    auth_allowed=false;assert(hooks.control(original,received,deadline,link)==c::Error::unauthorized_role);auth_allowed=true;
    for(const auto& test:std::vector<std::pair<s::AdmissionCode,c::Error>>{{s::AdmissionCode::overloaded,c::Error::queue_full},{s::AdmissionCode::expired,c::Error::expired},{s::AdmissionCode::stale,c::Error::stale_boot},{s::AdmissionCode::invalid,c::Error::invalid_message}}){member.admission=test.first;assert(hooks.control(original,received,deadline,link)==test.second);}member.admission=s::AdmissionCode::accepted;
    const auto calls=member.submitted.size();auto target=envelope(member,original,intent);target.destination_octopus_id=id<s::OctopusId>(99);
    assert(hooks.control(original,target,deadline,link)==c::Error::target_not_local&&member.submitted.size()==calls);
    target=envelope(member,original,intent);target.consortium_id=id<s::ConsortiumId>(99);
    assert(hooks.control(original,target,deadline,link)==c::Error::target_not_local&&member.submitted.size()==calls);
    for(unsigned retired:{2u,4u,6u,12u,18u,20u,26u,35u,36u}){received.kind=static_cast<c::Kind>(retired);assert(hooks.control(original,received,deadline,link)==c::Error::unauthorized_role&&member.submitted.size()==calls);}
    received.kind=c::Kind::scoped_result;assert(hooks.control(original,received,deadline,link)==c::Error::unauthorized_role);
    role.member=false;role.coordinator=true;received.kind=c::Kind::scoped_intent;
    assert(hooks.control(original,received,deadline,link)==c::Error::unauthorized_role&&member.submitted.size()==calls);
}
void key_rejection_diagnostics_preserve_results(){
    reset();auto main=control();assert(amity::start_configuration_synchronization(main)==ESP_OK);
    const auto intent=down(main);
    assert(main.sender(intent)&&system_sync_test_log::messages.empty());
    send_result=ESP_ERR_INVALID_STATE;
    assert(!main.sender(intent)&&sent.size()==2&&system_sync_test_log::messages.size()==1);
    const auto transport=system_sync_test_log::messages.back();
    assert(transport.find("phase=transport key=65 generation=7 error=")!=std::string::npos);
    assert(transport.find("Source")==std::string::npos);
    auto power=intent;power.kind=s::OwnedIntentKind::activity;
    assert(!main.sender(power)&&system_sync_test_log::messages.size()==1);

    reset();auto member=control(true);assert(amity::start_configuration_synchronization(member)==ESP_OK);
    const auto original=*member.configuration.controller_octopus_id;
    const auto key=down(member);const auto packet=envelope(member,original,key);const auto link=id<s::LinkId>(44);
    assert(hooks.control(original,packet,123456,link)==c::Error::none&&system_sync_test_log::messages.empty());
    member.admission=s::AdmissionCode::pending;
    assert(hooks.control(original,packet,123456,link)==c::Error::none&&system_sync_test_log::messages.empty());
    for(const auto& failure:std::vector<std::pair<s::AdmissionCode,c::Error>>{
        {s::AdmissionCode::overloaded,c::Error::queue_full},{s::AdmissionCode::expired,c::Error::expired},
        {s::AdmissionCode::stale,c::Error::stale_boot},{s::AdmissionCode::unauthorized,c::Error::unauthorized_role}}){
        member.admission=failure.first;const auto count=system_sync_test_log::messages.size();
        assert(hooks.control(original,packet,123456,link)==failure.second&&system_sync_test_log::messages.size()==count+1);
        const auto& message=system_sync_test_log::messages.back();
        assert(message=="Key rejected phase=owner key=65 generation=7 admission="+std::to_string(static_cast<int>(failure.first)));
        assert(message.find("Source")==std::string::npos);
    }
}
void activity_selection_delivery_is_independent(){
    reset();auto member=control(true);assert(amity::start_configuration_synchronization(member)==ESP_OK);
    const auto original=*member.configuration.controller_octopus_id;auto activity=down(member);activity.kind=s::OwnedIntentKind::activity;
    activity.target.osd_name.clear();activity.selected=true;activity.source=down(member).target;activity.repair=true;
    const auto received=envelope(member,original,activity);const auto link=id<s::LinkId>(44);const auto began=now;
    for(auto code:{s::AdmissionCode::accepted,s::AdmissionCode::pending,s::AdmissionCode::unresolved}){
        member.admission=code;assert(hooks.control(original,received,123456,link)==c::Error::none);
        const auto& delivered=member.submitted.back();
        assert(delivered.kind==s::OwnedIntentKind::activity&&delivered.selected&&delivered.repair&&delivered.source==activity.source);
        assert(!delivered.activity_step&&member.authorizations.back().authority==s::OwnedAuthority::control);
        assert(result_packets.empty()&&result_fences.empty()&&packets.empty()&&now==began);
    }
    activity.selected=false;activity.repair=false;member.admission=s::AdmissionCode::accepted;
    assert(hooks.control(original,envelope(member,original,activity),123456,link)==c::Error::none&&!member.submitted.back().selected);
    reset();auto main=control();assert(amity::start_configuration_synchronization(main)==ESP_OK);
    c::Envelope retired;retired.kind=c::Kind::scoped_result;retired.payload=json("{\"action_result\":{}}");
    assert(!c::valid_owned_result(retired.payload)&&hooks.control(id<s::OctopusId>(9),retired,123456,link)==c::Error::invalid_message);
}
void reports_and_exact_disconnect(){
    reset();auto main=control();assert(amity::start_configuration_synchronization(main)==ESP_OK);
    peers={remote(9,19,2,0b11)};const auto owner=peers[0].id;const auto oldboot=peers[0].boot_id;auto first=report(owner,oldboot);
    assert(hooks.synchronize(owner,string_document("{\"adapters\":["+c::owned_snapshot_json(first)+"]}"))==c::Error::none&&main.updates.size()==1);
    assert(main.updates[0].discovery_generation==3);
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
void removal_serializes_report_and_disconnect_publication(){
    for(bool losing_connection:{false,true}){
        reset();auto main=control();assert(amity::start_configuration_synchronization(main)==ESP_OK);
        peers={remote(9,19,1,1),remote(10,20,1,1)};
        const auto removed=report(peers[0].id,peers[0].boot_id),kept=report(peers[1].id,peers[1].boot_id);
        assert(amity::accept_report(removed.owner,removed)&&amity::accept_report(kept.owner,kept));
        std::mutex gate;std::condition_variable changed;bool publishing=false,resume=false,removing=false;
        main.before_snapshot_update=[&](const s::OwnedAdapterSnapshot& value){
            if(value.owner!=removed.owner)return;
            assert(value.ready!=losing_connection);
            // A removal cannot overtake a validated report or a disconnect's
            // blank publication between its two diagnostic cache updates.
            require_state_lock();
            std::unique_lock<std::mutex> lock(gate);publishing=true;changed.notify_all();changed.wait(lock,[&]{return resume;});
        };
        require_report_lock=true;
        std::thread publication([&]{
            if(losing_connection)assert(hooks.disconnected(removed.owner,removed.boot,id<s::LinkId>(81)));
            else assert(amity::accept_report(removed.owner,removed));
        });
        {std::unique_lock<std::mutex> lock(gate);changed.wait(lock,[&]{return publishing;});}
        // Runtime membership is revoked before the lifecycle purge. Exact key
        // cleanup may be busy, but it must not prevent observation deletion.
        peers.erase(peers.begin());main.fence_result=false;
        std::thread removal([&]{
            {std::lock_guard<std::mutex> lock(gate);removing=true;changed.notify_all();}
            amity::forget_owner(removed.owner);
        });
        {std::unique_lock<std::mutex> lock(gate);changed.wait(lock,[&]{return removing;});resume=true;changed.notify_all();}
        publication.join();removal.join();main.before_snapshot_update={};
        assert(main.forgotten_snapshot_owners==std::vector<s::OctopusId>{removed.owner});
        assert(main.snapshots.size()==1&&c::owned_snapshot_json(main.snapshots.front())==c::owned_snapshot_json(kept));
        assert(amity::state.reports.size()==1&&c::owned_snapshot_json(amity::state.reports.front())==c::owned_snapshot_json(kept));
        const auto updates=main.updates.size();
        assert(!amity::accept_report(removed.owner,removed)&&main.updates.size()==updates);
        require_report_lock=false;
        const auto replacement=remote(11,21,1,1);peers.push_back(replacement);const auto added=report(replacement.id,replacement.boot_id);
        assert(amity::accept_report(added.owner,added));
        const auto observations=amity::discovered_equipment();
        assert(observations.size()==2&&std::none_of(observations.begin(),observations.end(),[&](const auto& value){return value.endpoint.port.octopus_id==removed.owner;}));
        assert(main.snapshots.size()==2&&std::none_of(main.snapshots.begin(),main.snapshots.end(),[&](const auto& value){return value.owner==removed.owner;}));
    }
}
void full_roster_adapter_reports_keep_exact_owner_fences(){
    reset();auto main=control();assert(amity::start_configuration_synchronization(main)==ESP_OK);
    static_assert(c::maximum_roster_peers==3&&amity::max_tentacles==4);
    std::vector<s::OwnedAdapterSnapshot> expected;
    for(unsigned index=0;index<c::maximum_roster_peers;++index){
        peers.push_back(remote(9+index,19+index,amity::max_tentacles,0b1111));
        const auto& owner=peers.back();std::string adapters="{\"adapters\":[";
        for(uint8_t adapter=0;adapter<amity::max_tentacles;++adapter){
            auto value=report(owner.id,owner.boot_id,adapter);value.discovery_generation=100+index*amity::max_tentacles+adapter;
            if(adapter)adapters+=',';adapters+=c::owned_snapshot_json(value);expected.push_back(value);
        }
        assert(hooks.synchronize(owner.id,string_document(adapters+"]}"))==c::Error::none);
    }
    const auto count=c::maximum_roster_peers*amity::max_tentacles;
    const auto retained=[&]{
        assert(amity::state.reports.size()==expected.size());
        for(const auto& value:expected){
            const auto found=std::find_if(amity::state.reports.begin(),amity::state.reports.end(),[&](const auto& saved){return saved.owner==value.owner&&saved.adapter==value.adapter;});
            assert(found!=amity::state.reports.end()&&c::owned_snapshot_json(*found)==c::owned_snapshot_json(value));
        }
        const auto available=amity::discovered_equipment();assert(available.size()==expected.size());
        for(const auto& value:expected)assert(std::count_if(available.begin(),available.end(),[&](const auto& item){return item.endpoint.port.octopus_id==value.owner&&item.endpoint.port.tentacle_id==value.adapter&&item.owner_boot_id==value.boot&&item.endpoint.logical_address==value.devices[0].logical_address;})==1);
    };
    assert(count==12&&main.updates.size()==count);retained();

    // A full roster still updates exactly one existing owner/adapter slot.
    auto changed=expected[3];changed.discovery_generation=900;changed.devices[0].logical_address=8;
    const auto link=id<s::LinkId>(82);
    assert(hooks.control(changed.owner,report_envelope(changed),100,link)==c::Error::none);
    expected[3]=changed;assert(main.updates.size()==count+1);retained();
    const auto updates=main.updates.size();
    auto rejected=changed;rejected.boot=id<s::BootId>(99);
    assert(hooks.control(changed.owner,report_envelope(rejected),100,link)==c::Error::invalid_message);
    rejected=changed;rejected.owner=peers[1].id;
    assert(hooks.control(changed.owner,report_envelope(rejected),100,link)==c::Error::invalid_message);
    auto invalid_adapter=report_envelope(changed);invalid_adapter.payload.get("snapshot")->get("adapter")->integer=amity::max_tentacles;
    assert(hooks.control(changed.owner,invalid_adapter,100,link)==c::Error::invalid_message);
    peers[0].supported_tentacles=3;
    assert(hooks.control(changed.owner,report_envelope(changed),100,link)==c::Error::invalid_message);
    peers[0].supported_tentacles=amity::max_tentacles;
    rejected=changed;rejected.owner=id<s::OctopusId>(99);
    assert(hooks.control(rejected.owner,report_envelope(rejected),100,link)==c::Error::invalid_message);
    assert(main.updates.size()==updates);retained();

    // Replacement boot observations survive cleanup of that owner's old boot;
    // all adapters belonging to the other two owners remain untouched.
    const auto owner=changed.owner;const auto oldboot=changed.boot;
    peers[0].boot_id=changed.boot=id<s::BootId>(29);changed.discovery_generation=901;
    assert(hooks.control(owner,report_envelope(changed),100,link)==c::Error::none);
    expected[3]=changed;retained();
    main.fence_result=false;
    assert(!hooks.disconnected(owner,oldboot,id<s::LinkId>(81)));retained();
    main.fence_result=true;
    const auto before_cleanup=main.updates.size();
    assert(hooks.disconnected(owner,oldboot,id<s::LinkId>(81)));
    expected.erase(std::remove_if(expected.begin(),expected.end(),[&](const auto& value){return value.owner==owner&&value.boot==oldboot;}),expected.end());
    assert(expected.size()==9&&main.updates.size()==before_cleanup+3);retained();
    for(size_t index=before_cleanup;index<main.updates.size();++index){const auto& missing=main.updates[index];assert(missing.owner==owner&&missing.boot==oldboot&&!missing.ready&&!missing.discovering&&missing.devices.empty());}
    assert(hooks.disconnected(owner,changed.boot,link));
    expected.erase(std::remove_if(expected.begin(),expected.end(),[&](const auto& value){return value.owner==owner;}),expected.end());
    assert(expected.size()==8);retained();
    assert(sent.empty()&&memory.writes==0); // Reports remain diagnostic observations.
}
void logical_session_restart_preserves_reports(){
    reset();auto main=control();assert(amity::start_configuration_synchronization(main)==ESP_OK);
    peers={remote(9,19,2,0b11)};const auto owner=peers[0].id,other=id<s::OctopusId>(10);const auto boot=peers[0].boot_id;
    auto first=report(owner,boot);auto second=first;second.adapter=1;
    assert(hooks.synchronize(owner,string_document("{\"adapters\":["+c::owned_snapshot_json(first)+","+c::owned_snapshot_json(second)+"]}"))==c::Error::none);
    const auto oldlink=id<s::LinkId>(81);const auto updates=main.updates.size(),reports=amity::state.reports.size();
    const auto saved_first=c::owned_snapshot_json(amity::state.reports.front());
    const auto observations=amity::discovered_equipment();assert(observations.size()==2);
    amity::set_owner_maintenance(other,true);
    main.fence_result=false;assert(!hooks.restart_session(owner,boot,oldlink));
    assert(main.fences.back()==std::make_pair(owner,oldlink)&&main.updates.size()==updates&&amity::state.reports.size()==reports);
    main.fence_result=true;assert(hooks.restart_session(owner,boot,oldlink));
    assert(main.fences.back()==std::make_pair(owner,oldlink)&&main.updates.size()==updates&&amity::state.reports.size()==reports);
    assert(c::owned_snapshot_json(amity::state.reports.front())==saved_first&&amity::state.maintenance.count(other));
    const auto retained=amity::discovered_equipment();assert(retained.size()==observations.size());
    for(size_t i=0;i<retained.size();++i)assert(retained[i].endpoint==observations[i].endpoint
        &&retained[i].osd_name==observations[i].osd_name&&retained[i].owner_boot_id==observations[i].owner_boot_id
        &&retained[i].physical_address==observations[i].physical_address&&retained[i].stale==observations[i].stale);
    const auto fences=main.fences.size();
    assert(!hooks.restart_session({},boot,oldlink)&&!hooks.restart_session(owner,{},oldlink)&&!hooks.restart_session(owner,boot,{})&&main.fences.size()==fences);
    assert(sent.empty()&&memory.writes==0); // A restart emits/replays no ordinary CEC work.
    // Actual transport loss still publishes missing observations after exact cleanup.
    assert(hooks.disconnected(owner,boot,oldlink)&&amity::discovered_equipment().empty()&&main.updates.size()>updates);
}
void report_coalescing_and_no_intent_backlog(){
    reset();auto member=control(true);assert(amity::start_configuration_synchronization(member)==ESP_OK);
    auto local=report(member.identity,member.boot);member.snapshots={local};const auto snapshot=json(hooks.snapshot({}));assert(c::closed(snapshot,{"adapters"})&&snapshot.get("adapters")->array.size()==1);
    const auto original=*member.configuration.controller_octopus_id;peers={remote(2,12,1,1)};assert(peers[0].id==original);
    member.observer(local);local.discovery_generation=4;member.snapshots={local};member.observer(local);assert(amity::state.pending_reports.size()==1);
    report_result=ESP_FAIL;amity::flush_reports();assert(packets.size()==1&&amity::state.pending_reports.empty()&&amity::state.retry_reports[0]&&sent.empty());
    report_result=ESP_OK;amity::flush_reports();assert(packets.size()==2&&amity::state.pending_reports.empty());
    assert(packets.back().kind==c::Kind::scoped_result&&packets.back().owner==original&&packets.back().ttl==1000);
    auto body=json(packets.back().payload);s::OwnedAdapterSnapshot actual;assert(c::parse_owned_snapshot(*body.get("snapshot"),actual)&&actual.discovery_generation==4);
    reset();auto main=control();assert(amity::start_configuration_synchronization(main)==ESP_OK);auto intent=down(main);intent.target.port.octopus_id=id<s::OctopusId>(9);
    send_result=ESP_FAIL;assert(!main.sender(intent)&&sent.size()==1);amity::flush_reports();assert(sent.size()==1);
    amity::set_owner_maintenance(intent.target.port.octopus_id,true);assert(!main.sender(intent)&&sent.size()==1);
    amity::set_owner_maintenance(intent.target.port.octopus_id,false);send_result=ESP_OK;assert(main.sender(intent)&&sent.size()==2);
    assert(sent.back().generation==intent.generation);
}
void dropped_snapshot_recaptures_latest_without_regression(){
    reset();auto member=control(true);assert(amity::start_configuration_synchronization(member)==ESP_OK);
    const auto controller=*member.configuration.controller_octopus_id;peers={remote(2,12,1,1)};current_result_link=id<s::LinkId>(71);
    assert(hooks.synchronize(controller,string_document("{\"controller\":\""+controller.str()+"\",\"config_revision\":\"23\"}"))==c::Error::none);
    c::RuntimeSnapshotRetry retry{controller,member.boot,peers[0].boot_id,current_result_link,23,0};
    auto latest=report(member.identity,member.boot);
    member.snapshots={latest};member.observer(latest);amity::flush_reports();assert(packets.size()==1&&amity::state.pending_reports.empty());
    // The admitted latest discovery snapshot expired on TLS. Only its adapter
    // and immutable scope return; the worker recaptures the current cache.
    hooks.snapshot_retry(retry);assert(result_packets.empty());amity::flush_reports();assert(result_packets.size()==1);
    auto decoded=[&](const std::string& payload){s::OwnedAdapterSnapshot value;const auto body=json(payload);assert(c::parse_owned_snapshot(*body.get("snapshot"),value));return value;};
    assert(decoded(result_packets.back().payload).discovery_generation==3&&result_packets.back().link==retry.link&&result_packets.back().revision==23&&result_packets.back().ttl==1000);
    // A cache update can precede its observer. Recovery reads that latest state,
    // rather than replaying the payload which originally expired.
    latest.discovery_generation=4;member.snapshots={latest};hooks.snapshot_retry(retry);amity::flush_reports();
    assert(decoded(result_packets.back().payload).discovery_generation==4);
    // A newer observation arrives while the old publication fails. It must
    // remain pending; the failed payload must never overwrite it.
    report_result=ESP_FAIL;on_report_send=[&]{latest.discovery_generation=5;member.snapshots={latest};member.observer(latest);};
    hooks.snapshot_retry(retry);amity::flush_reports();assert(amity::state.pending_reports.size()==1&&!amity::state.retry_reports[0]);
    assert(amity::state.pending_reports[0].discovery_generation==5);
    report_result=ESP_OK;on_report_send={};amity::flush_reports();assert(decoded(packets.back().payload).discovery_generation==5);
    // The same race on the ordinary observer path also preserves the newer
    // pending observation, with no hidden typed intent or old-payload replay.
    member.observer(latest);report_result=ESP_FAIL;on_report_send=[&]{latest.discovery_generation=6;member.snapshots={latest};member.observer(latest);};
    amity::flush_reports();assert(amity::state.pending_reports[0].discovery_generation==6&&!amity::state.retry_reports[0]);
    report_result=ESP_OK;on_report_send={};amity::flush_reports();assert(decoded(packets.back().payload).discovery_generation==6&&sent.empty());
    // Failed recapture retains only a flag. A later pass reads newer completion.
    hooks.snapshot_retry(retry);report_result=ESP_FAIL;amity::flush_reports();assert(amity::state.retry_reports[0]&&amity::state.pending_reports.empty());
    latest.discovery_generation=7;member.snapshots={latest};report_result=ESP_OK;amity::flush_reports();assert(decoded(result_packets.back().payload).discovery_generation==7&&!amity::state.retry_reports[0]);
    const auto count=result_packets.size();
    // A delayed obsolete notification must not replace the current final-state
    // retry. Validation and flag selection share one lock, so an event cannot
    // prevalidate, pause, then overwrite a newer scope's flag.
    hooks.snapshot_retry(retry);auto delayed=retry;delayed.link=id<s::LinkId>(100);hooks.snapshot_retry(delayed);amity::flush_reports();assert(result_packets.size()==count+1);
    const auto after_delayed=result_packets.size();
    auto stale=retry;stale.owner_boot=id<s::BootId>(100);hooks.snapshot_retry(stale);amity::flush_reports();
    stale=retry;stale.controller_boot=id<s::BootId>(100);hooks.snapshot_retry(stale);amity::flush_reports();
    stale=retry;stale.link=id<s::LinkId>(100);hooks.snapshot_retry(stale);amity::flush_reports();
    stale=retry;stale.config_revision=22;hooks.snapshot_retry(stale);amity::flush_reports();
    assert(result_packets.size()==after_delayed&&sent.empty());
    require_retry_lock=true;hooks.snapshot_retry(retry);require_retry_lock=false;amity::flush_reports();assert(result_packets.size()==after_delayed+1);
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
void added_owner_label_remains_unique_after_middle_removal(){
    for(bool renamed:{false,true}){
        reset();auto main=control();const auto original=main.identity,removed=id<s::OctopusId>(9),kept=id<s::OctopusId>(10),added=id<s::OctopusId>(1);
        main.configuration.octopuses.front().label=main.configuration.octopuses.front().generated_label="Amity 1";
        main.configuration.octopuses.push_back({removed,"Amity 2","Amity 2",false,1});
        main.configuration.octopuses.push_back({kept,renamed?"Den":"Amity 3","Amity 3",renamed,1});
        Configuration active;commit(*journal,main.configuration,{},active);main.configuration=active;
        assert(amity::start_configuration_synchronization(main)==ESP_OK);
        Configuration candidate;std::vector<s::ActivityId> affected;std::string error;
        assert(propose_octopus_removal(active,removed,candidate,affected,error)&&affected.empty());
        assert(amity::activate_distributed_configuration(candidate,active.revisions(),id<s::MessageId>(140))==ESP_OK);
        peers={remote(10,20,1,1),remote(1,21,1,1)};amity::adopt_owners();
        const auto owner=[&](s::OctopusId id)->const ConfiguredOctopus&{const auto found=std::find_if(main.configuration.octopuses.begin(),main.configuration.octopuses.end(),[&](const auto& value){return value.octopus_id==id;});assert(found!=main.configuration.octopuses.end());return *found;};
        assert(owner(original).label=="Amity 1"&&owner(kept).label==(renamed?"Den":"Amity 3")&&owner(kept).generated_label=="Amity 3"&&owner(kept).label_override==renamed);
        assert(owner(added).label=="Amity 2"&&owner(added).generated_label=="Amity 2"&&!owner(added).label_override);
        const auto encoded=serialize_configuration(main.configuration);Configuration decoded;
        assert(encoded.find(removed.str())==std::string::npos&&parse_configuration(encoded,decoded,error));
        assert(decoded.octopuses.front().octopus_id==added&&decoded.controller_octopus_id==original&&decoded.activities.front().source==active.activities.front().source);
    }
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
int main(){startup_and_minimal_handshake();intent_delivery_preserves_authorization();key_rejection_diagnostics_preserve_results();activity_selection_delivery_is_independent();reports_and_exact_disconnect();removal_serializes_report_and_disconnect_publication();full_roster_adapter_reports_keep_exact_owner_fences();logical_session_restart_preserves_reports();report_coalescing_and_no_intent_backlog();dropped_snapshot_recaptures_latest_without_regression();scans_and_saved_intent_without_peer_ack();saved_owner_capacity_does_not_shrink();added_owner_label_remains_unique_after_middle_removal();removal_commit_boundary();std::cout<<"PASS actual owner synchronization: minimal handshake, derived authority/exact auth/deadline, bounded key rejection diagnostics, independent activity-selection delivery, latest snapshot recapture without failed-send regression, twelve adapter reports with exact owner/boot updates and disconnect fences, serialized removal/report cleanup, unique labels after middle removal, sparse scans/capacity retention and real journal save/applied revisions independent of offline peers, removal only after stage and owner reservation\n";}
