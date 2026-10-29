// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#include "v2_admin.hpp"
#include "configuration.hpp"
#include "system_configuration.hpp"
#include "configuration_replacement.hpp"
#include "config_store.hpp"
#include "system_services.hpp"
#include "firmware_update.hpp"
#include "group_firmware_update.hpp"
#include "device_settings.hpp"
#include "adapter_settings.hpp"
#include "amity_matter.h"
#include "amity/consortium/runtime.hpp"
#include "amity/system_sync.hpp"
#include "web.hpp"
#include "json_input.hpp"
#include "esp_random.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "cec/bus.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <algorithm>
#include <memory>
#include <cstring>
#include <set>

namespace web_admin {
namespace {
namespace m=amity::system;
using Json=std::unique_ptr<cJSON,decltype(&cJSON_Delete)>;
void wipe(std::string& value){volatile char* bytes=value.empty()?nullptr:&value[0];for(size_t n=0;n<value.size();++n)bytes[n]=0;}
void wipe_json(cJSON* value){if(!value)return;for(auto* child=value->child;child;child=child->next)wipe_json(child);if(value->valuestring){volatile char* p=value->valuestring;for(size_t n=strlen(value->valuestring);n;--n)*p++=0;}}
struct SensitiveRequest {std::string& body;cJSON* json;bool enabled;~SensitiveRequest(){if(enabled){wipe(body);wipe_json(json);}}};
struct SensitiveString {std::string& value;~SensitiveString(){wipe(value);}};
amity::SystemControl* control=nullptr;
std::optional<config_store::ConfigurationV2> active;
bool configuration_passive=false;
std::optional<config_store::ConfigurationV2> deferred_migration;
uint64_t deferred_epoch=1;
std::vector<std::string> migration_tasks;
struct Tracked {std::string id,path,digest,status,error; m::OctopusId candidate{};m::MessageId scan_id{};std::string name;bool rebooting=false;std::string reset;};
bool operation_pending(const Tracked& operation){return operation.status=="admitted"||operation.status=="configuring"||operation.status=="preparing"||operation.status=="activating";}
std::vector<Tracked> operations;
std::string string(cJSON* root,const char* key){auto* v=cJSON_GetObjectItemCaseSensitive(root,key);return cJSON_IsString(v)?v->valuestring:"";}
bool uint64(cJSON* root,const char* key,uint64_t& value){const auto v=m::parse_uint64(string(root,key));if(!v)return false;value=*v;return true;}
std::string json(cJSON* root){char* text=cJSON_PrintUnformatted(root);if(!text)return {};std::string out(text);cJSON_free(text);return out;}
esp_err_t send(httpd_req_t* req,cJSON* root){return web::json(req,json(root));}
esp_err_t error(httpd_req_t* req,const char* code,const std::string& text,const char* status="400 Bad Request",bool retry=false){
    Json out(cJSON_CreateObject(),cJSON_Delete);
    cJSON_AddStringToObject(out.get(),"code",code);cJSON_AddStringToObject(out.get(),"message",text.c_str());
    cJSON_AddStringToObject(out.get(),"error",text.c_str());cJSON_AddBoolToObject(out.get(),"retryable",retry);
    cJSON_AddArrayToObject(out.get(),"affected_ids");
    httpd_resp_set_hdr(req,"Connection","close");
    web::json(req,json(out.get()),status);
    // Router admission failures may precede body reads. Preserve the structured
    // response while preventing HTTPD from draining an untrusted Content-Length.
    return ESP_FAIL;
}
bool unique_keys(const cJSON* value,unsigned depth=0){
    if(depth>16)return false;
    if(cJSON_IsObject(value))for(auto* a=value->child;a;a=a->next){
        if(!a->string)return false;
        for(auto* b=a->next;b;b=b->next)if(b->string&&!strcmp(a->string,b->string))return false;
    }
    if(cJSON_IsObject(value)||cJSON_IsArray(value))for(auto* c=value->child;c;c=c->next)if(!unique_keys(c,depth+1))return false;
    return true;
}
bool expected_pair(cJSON* root,config_store::RevisionPair& pair){return uint64(root,"expected_config_revision",pair.config_revision)&&uint64(root,"expected_presentation_revision",pair.presentation_revision);}
std::vector<m::CecObservation> equipment_observations(){
    auto observations=amity::discovered_equipment();
    if(observations.empty())for(const auto& device:control->snapshot().devices){
        if(!device.resolved)continue;
        m::CecObservation item;item.endpoint={control->port(device.bus),device.address};item.osd_name=device.name;item.device_type=device.type;item.physical_address=device.physical_address;item.vendor_id=device.vendor_id;observations.push_back(std::move(item));
    }
    return observations;
}
uint64_t equipment_revision(){
    std::vector<std::string> evidence;
    for(const auto& item:equipment_observations())if(item.osd_name)evidence.push_back(m::name_json({item.endpoint.port,*item.osd_name})+":"+item.scan_id.str()+":"+std::to_string(item.endpoint.logical_address)+":"+(item.stale?"stale":"fresh"));
    std::sort(evidence.begin(),evidence.end());std::string bytes=std::to_string(control->snapshot().discovery_generation);for(const auto& item:evidence)bytes+='\n'+item;
    const auto hash=config_store::sha256_hex(bytes);uint64_t revision=0;for(size_t n=0;n<16&&n<hash.size();++n)revision=revision*16+(hash[n]<='9'?hash[n]-'0':hash[n]-'a'+10);return revision;
}
bool object_fields(cJSON* value,std::initializer_list<const char*> required,std::initializer_list<const char*> optional={}){
    if(!cJSON_IsObject(value))return false;
    for(auto key:required)if(!cJSON_GetObjectItemCaseSensitive(value,key))return false;
    for(auto* field=value->child;field;field=field->next){bool known=false;for(auto key:required)if(!strcmp(field->string,key))known=true;for(auto key:optional)if(!strcmp(field->string,key))known=true;if(!known)return false;}
    return true;
}
bool parse_port_value(cJSON* value,m::CecPortRef& port){
    if(!object_fields(value,{"consortium_id","octopus_id","tentacle_id","side"}))return false;
    const auto group=m::ConsortiumId::parse(string(value,"consortium_id"));const auto owner=m::OctopusId::parse(string(value,"octopus_id"));auto* tentacle=cJSON_GetObjectItemCaseSensitive(value,"tentacle_id");const auto side=string(value,"side");
    if(!group||!owner||!cJSON_IsNumber(tentacle)||tentacle->valuedouble!=tentacle->valueint||tentacle->valueint<0||static_cast<unsigned>(tentacle->valueint)>amity::last_tentacle_id||(side!="front"&&side!="back"))return false;
    port={*group,*owner,static_cast<uint8_t>(tentacle->valueint),side=="front"?m::Side::front:m::Side::back};return port.valid();
}
struct AdmissionSelection {
    m::MessageId scan_id;
    std::string candidate_token,name,setup_code;
};
bool admission_selection(cJSON* root,bool requires_code,AdmissionSelection& out,std::string& message){
    const auto fail=[&](const char* reason){message=reason;return false;};
    if(!object_fields(root,{"request_id","scan_id","candidate_token","name"},{"setup_code"}))return fail("Choose an Amity from the current search and give it a name.");
    AdmissionSelection value;SensitiveString clear_code{value.setup_code};
    for(const auto* key:{"scan_id","candidate_token","name"})if(!cJSON_IsString(cJSON_GetObjectItemCaseSensitive(root,key)))return fail("Choose an Amity from the current search and give it a name.");
    auto* code=cJSON_GetObjectItemCaseSensitive(root,"setup_code");
    if(code&&!cJSON_IsString(code))return fail("The Matter setup code must be text.");
    const auto scan=m::MessageId::parse(string(root,"scan_id"));
    value.candidate_token=string(root,"candidate_token");value.name=string(root,"name");value.setup_code=code?string(root,"setup_code"):std::string{};
    if(!scan||value.candidate_token.empty()||value.candidate_token.size()>128||std::any_of(value.candidate_token.begin(),value.candidate_token.end(),[](unsigned char byte){return byte<32||byte==127;}))return fail("Choose an Amity from the current search.");
    if(value.setup_code.size()>256)return fail("The Matter setup code is too long.");
    if(!requires_code&&!value.setup_code.empty())return fail("A setup code is not used when adding this Amity.");
    if(requires_code&&value.setup_code.empty())return fail("Enter the Matter setup code printed on the added Amity.");
    const auto first=value.name.find_first_not_of(" \t\r\n");
    value.name=first==std::string::npos?std::string{}:value.name.substr(first,value.name.find_last_not_of(" \t\r\n")-first+1);
    if(!device_settings::normalize_hostname(value.name))return fail("Use a name of 1–32 letters, digits or hyphens, beginning and ending with a letter or digit.");
    value.scan_id=*scan;out=value;message.clear();return true;
}
esp_err_t admission_response(httpd_req_t* req){
    const auto state=amity::consortium::runtime_admission_status();
    Json result(cJSON_CreateObject(),cJSON_Delete);
    if(state.scan_id)cJSON_AddStringToObject(result.get(),"scan_id",state.scan_id.str().c_str());else cJSON_AddNullToObject(result.get(),"scan_id");
    cJSON_AddStringToObject(result.get(),"phase",state.phase.c_str());cJSON_AddStringToObject(result.get(),"error",state.error.c_str());
    cJSON_AddBoolToObject(result.get(),"requires_setup_code",state.requires_setup_code);
    if(state.candidate_id)cJSON_AddStringToObject(result.get(),"candidate_id",state.candidate_id.str().c_str());else cJSON_AddNullToObject(result.get(),"candidate_id");
    auto* candidates=cJSON_AddArrayToObject(result.get(),"candidates");
    for(const auto& candidate:state.candidates){auto* item=cJSON_CreateObject();cJSON_AddItemToArray(candidates,item);cJSON_AddStringToObject(item,"token",candidate.token.c_str());cJSON_AddStringToObject(item,"label",candidate.label.c_str());}
    return send(req,result.get());
}
std::optional<std::string> device_settings_target(const std::string& path){
    const std::string prefix="/api/v2/devices/",suffix="/settings";
    if(path.size()!=prefix.size()+32+suffix.size()||path.compare(0,prefix.size(),prefix)||path.compare(path.size()-suffix.size(),suffix.size(),suffix))return {};
    const auto id=path.substr(prefix.size(),32);
    if(!std::all_of(id.begin(),id.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');})||id==std::string(32,'0'))return {};
    return id;
}
std::optional<std::vector<std::string>> request_fields(const std::string& path){
    std::vector<std::string> allowed{"request_id"};
    if(path=="/api/v2/config"||path=="/api/v2/config/validate")allowed.insert(allowed.end(),{"expected_config_revision","expected_presentation_revision","configuration"});
    else if(path=="/api/v2/replacements/preview"||path=="/api/v2/replacements/apply")allowed.insert(allowed.end(),{"expected_config_revision","expected_presentation_revision","previous_octopus_id","replacement_octopus_id","old_hardware_isolated","ports","equipment"});
    else if(path=="/api/v2/membership/scan"){}
    else if(path=="/api/v2/membership/cancel")allowed.push_back("scan_id");
    else if(path=="/api/v2/membership/add")allowed.insert(allowed.end(),{"scan_id","candidate_token","setup_code","name"});
    else if(path=="/api/v2/membership/remove")allowed.insert(allowed.end(),{"octopus_id","confirm","expected_membership_revision","expected_target_boot_id"});
    else if(path=="/api/v2/updates/validate")allowed.insert(allowed.end(),{"manifest","signature"});
    else if(path=="/api/v2/updates/cohort")allowed.insert(allowed.end(),{"manifest","signature","targets"});
    else if(path=="/api/v2/updates/chunk")allowed.insert(allowed.end(),{"cohort_job_id","target_octopus_id","offset","data_hex"});
    else if(path=="/api/v2/updates/install")allowed.insert(allowed.end(),{"cohort_job_id","target_octopus_id"});
    else if(path=="/api/v2/updates/cancel")allowed.push_back("cohort_job_id");
    else if(device_settings_target(path))allowed.insert(allowed.end(),{"expected_boot_id","expected_settings_revision","confirm_restart","changes"});
    else return {};
    return allowed;
}
void publish_inputs(){
    if(!active)return;
    amity_matter_input_t inputs[16]{};size_t count=0;
    for(const auto& slot:active->external_slots){
        if(slot.slot>=16||slot.tombstone||slot.hidden)continue;
        const auto* activity=config_store::activity_for_slot(*active,slot.slot);if(!activity||activity->zone_id!=active->primary_zone_id)continue;
        auto& in=inputs[count++];in.index=slot.slot;
        snprintf(in.name,sizeof(in.name),"%s",(slot.alias.empty()?activity->label:slot.alias).c_str());
    }
    amity_matter_set_inputs(inputs,count);
}
void update_operation(Tracked& operation){
    if(!operation_pending(operation))return;
    if(operation.path=="/api/v2/membership/remove"){
        Json status(cJSON_Parse(amity::consortium::runtime_removal_operation(operation.candidate,*m::MessageId::parse(operation.id)).c_str()),cJSON_Delete);
        if(!status)return;
        const auto stage=string(status.get(),"stage");
        if(stage=="admitted"||stage=="complete")operation.status=stage;
        operation.error=string(status.get(),"message");operation.reset=string(status.get(),"reset");
        return;
    }
    if(device_settings_target(operation.path)){
        Json status(cJSON_Parse(amity::consortium::runtime_device_settings_operation(operation.candidate,*m::MessageId::parse(operation.id)).c_str()),cJSON_Delete);
        if(!status)return;
        const auto stage=string(status.get(),"stage");
        if(stage=="admitted"||stage=="applied"||stage=="failed"||stage=="unknown")operation.status=stage;
        operation.error=string(status.get(),"message");operation.rebooting=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(status.get(),"rebooting"));
        return;
    }
    if(operation.path=="/api/v2/config"||operation.path=="/api/v2/replacements/apply"){
        const auto status=amity::configuration_activation_status();
        if(status.request_id.str()==operation.id){operation.status=status.phase;operation.error=status.error;}
        return;
    }
    if(operation.path=="/api/v2/membership/add"){
        const auto state=amity::consortium::runtime_admission_status();
        if(state.scan_id!=operation.scan_id){
            const auto peers=amity::consortium::runtime_peers();
            const auto peer=std::find_if(peers.begin(),peers.end(),[&](const auto& peer){return operation.candidate&&peer.id==operation.candidate&&peer.enrolled&&peer.ready&&!peer.provisional&&peer.hostname==operation.name;});
            operation.status=peer!=peers.end()?"complete":"unknown";
            operation.error=operation.status=="unknown"?"The search changed before the result was confirmed. Check the Amity devices list before trying again.":std::string{};return;
        }
        if(state.candidate_id)operation.candidate=state.candidate_id;
        if(state.phase=="failed"||state.phase=="cancelled"){operation.status=state.phase;operation.error=state.error;return;}
        if(state.phase!="complete")return;
        const auto peers=amity::consortium::runtime_peers();
        const auto peer=std::find_if(peers.begin(),peers.end(),[&](const auto& peer){return operation.candidate&&peer.id==operation.candidate&&peer.enrolled&&peer.ready&&!peer.provisional;});
        if(peer!=peers.end()){
            if(peer->hostname!=operation.name){operation.status="failed";operation.error="The added Amity did not confirm its requested name.";return;}
            operation.status="complete";return;
        }
    }
}
esp_err_t operation_response(httpd_req_t* req,Tracked& operation){
    update_operation(operation);
    Json result(cJSON_CreateObject(),cJSON_Delete);
    cJSON_AddStringToObject(result.get(),"operation_id",operation.id.c_str());
    cJSON_AddStringToObject(result.get(),"stage",operation.status.c_str());
    cJSON_AddStringToObject(result.get(),"message",operation.error.c_str());
    cJSON_AddBoolToObject(result.get(),"retryable",false);
    cJSON_AddStringToObject(result.get(),"boot_id",system_services::boot_id());
    cJSON_AddBoolToObject(result.get(),"rebooting",operation.rebooting);
    if(operation.path=="/api/v2/membership/remove")cJSON_AddStringToObject(result.get(),"reset",operation.reset.c_str());
    const auto sent=send(req,result.get());
    if(device_settings_target(operation.path))amity::consortium::runtime_device_settings_response_attempted(operation.candidate,*m::MessageId::parse(operation.id));
    return sent;
}
esp_err_t snapshot(httpd_req_t* req){
    Json result(cJSON_CreateObject(),cJSON_Delete);
    const auto state=control->snapshot();const auto execution=control->execution_snapshot();
    const auto group=amity::consortium::runtime_status();
    cJSON_AddNumberToObject(result.get(),"api_version",2);
    cJSON_AddStringToObject(result.get(),"mode",configuration_is_passive()?"passive":active?"coordinator":"migration_pending");
    cJSON_AddStringToObject(result.get(),"consortium_id",control->consortium_id().str().c_str());
    cJSON_AddStringToObject(result.get(),"octopus_id",control->octopus_id().str().c_str());
    cJSON_AddStringToObject(result.get(),"boot_id",system_services::boot_id());
    cJSON_AddStringToObject(result.get(),"hostname",device_settings::hostname());
    const auto known_names=amity::consortium::runtime_known_hostnames();
    auto* names=cJSON_AddArrayToObject(result.get(),"named_devices");
    auto add_name=[&](m::OctopusId id,const std::string& hostname){auto* entry=cJSON_CreateObject();cJSON_AddItemToArray(names,entry);cJSON_AddStringToObject(entry,"octopus_id",id.str().c_str());cJSON_AddStringToObject(entry,"hostname",hostname.c_str());};
    add_name(control->octopus_id(),device_settings::hostname());
    for(const auto& entry:known_names)if(entry.first!=control->octopus_id())add_name(entry.first,entry.second);
    cJSON_AddStringToObject(result.get(),"state_revision",m::format_uint64(execution.state_revision).c_str());
    cJSON_AddStringToObject(result.get(),"applied_config_revision",m::format_uint64(execution.applied_config_revision).c_str());
    cJSON_AddStringToObject(result.get(),"applied_presentation_revision",m::format_uint64(execution.applied_presentation_revision).c_str());
    cJSON_AddBoolToObject(result.get(),"configuration_pending",active&&(execution.applied_config_revision!=active->config_revision||execution.applied_presentation_revision!=active->presentation_revision));
    cJSON_AddBoolToObject(result.get(),"discovering",state.discovering);
    cJSON_AddBoolToObject(result.get(),"ready",state.ready);
    cJSON_AddStringToObject(result.get(),"discovery_revision",m::format_uint64(equipment_revision()).c_str());
    cJSON_AddStringToObject(result.get(),"activity_error",state.activity_error.c_str());
    cJSON_AddStringToObject(result.get(),"hardware_error",control->hardware_error().c_str());
    auto* capabilities=cJSON_AddObjectToObject(result.get(),"capabilities");
    cJSON_AddNumberToObject(capabilities,"configuration_schema",active?3:1);
    cJSON_AddNumberToObject(capabilities,"supported_tentacles",control->supported_tentacles());
    cJSON_AddNumberToObject(capabilities,"configured_tentacles_mask",control->configured_tentacles_mask());
    auto* connectors=cJSON_AddArrayToObject(capabilities,"connectors");
    for(uint8_t id=0;id<control->supported_tentacles();++id){auto* connector=cJSON_CreateObject();cJSON_AddNumberToObject(connector,"tentacle_id",id);cJSON_AddBoolToObject(connector,"configured",control->connector_enabled(id));const auto presence=control->connector_presence(id);cJSON_AddStringToObject(connector,"presence",presence?(*presence?"present":"absent"):"unknown");cJSON_AddItemToArray(connectors,connector);}
    cJSON_AddNumberToObject(capabilities,"supported_octopuses",3);
    cJSON_AddNumberToObject(capabilities,"configuration_payload_limit",config_store::configuration_payload_limit);
    config_store::StorageBudget budget;const auto storage_result=config_store::storage_budget(budget);auto* storage=cJSON_AddObjectToObject(result.get(),"storage");
    cJSON_AddBoolToObject(storage,"available",storage_result==ESP_OK);
    cJSON_AddNumberToObject(storage,"configuration_payload_limit",config_store::configuration_payload_limit);
    if(storage_result==ESP_OK){cJSON_AddNumberToObject(storage,"used_entries",budget.used_entries);cJSON_AddNumberToObject(storage,"available_entries",budget.available_entries);cJSON_AddNumberToObject(storage,"total_entries",budget.total_entries);}
    cJSON_AddBoolToObject(capabilities,"managed_updates",true);
    cJSON_AddBoolToObject(capabilities,"consortium_enrollment",group.initialized&&group.crypto_ready);
    const auto timing=cec::Bus::runtime_statistics();auto* runtime=cJSON_AddObjectToObject(result.get(),"cec_runtime");
    cJSON_AddNumberToObject(runtime,"active_ports",timing.active_ports);cJSON_AddNumberToObject(runtime,"timer_instances",timing.timer_instances);
    cJSON_AddNumberToObject(runtime,"ticks",timing.ticks);cJSON_AddNumberToObject(runtime,"late_ticks",timing.late_ticks);
    cJSON_AddNumberToObject(runtime,"max_tick_gap_us",timing.max_tick_gap_us);cJSON_AddNumberToObject(runtime,"max_isr_us",timing.max_isr_us);
    auto* membership=cJSON_AddObjectToObject(result.get(),"membership");
    cJSON_AddStringToObject(membership,"membership_revision",m::format_uint64(group.membership_revision).c_str());
    cJSON_AddBoolToObject(membership,"crypto_ready",group.crypto_ready);
    cJSON_AddBoolToObject(membership,"enrollment_open",group.enrollment_open);
    cJSON_AddBoolToObject(membership,"coordinator",group.coordinator);
    cJSON_AddBoolToObject(membership,"authority_ready",group.authority_ready);
    cJSON_AddBoolToObject(membership,"member",group.member);
    cJSON_AddBoolToObject(membership,"connected",group.connected);
    cJSON_AddStringToObject(membership,"phase",group.phase.c_str());
    cJSON_AddStringToObject(membership,"last_error",group.last_error.c_str());
    auto* peers=cJSON_AddArrayToObject(membership,"peers");
    for(const auto& peer:amity::consortium::runtime_peers()){
        auto* record=cJSON_CreateObject();cJSON_AddItemToArray(peers,record);
        cJSON_AddStringToObject(record,"octopus_id",peer.id.str().c_str());cJSON_AddBoolToObject(record,"ready",peer.ready);cJSON_AddBoolToObject(record,"management_ready",peer.management_ready);
        if(peer.boot_id)cJSON_AddStringToObject(record,"boot_id",peer.boot_id.str().c_str());else cJSON_AddNullToObject(record,"boot_id");
        cJSON_AddBoolToObject(record,"enrolled",peer.enrolled);
        cJSON_AddNumberToObject(record,"capabilities",peer.negotiated_capabilities);
        cJSON_AddNumberToObject(record,"supported_tentacles",peer.supported_tentacles);cJSON_AddNumberToObject(record,"configured_tentacles_mask",peer.configured_tentacles_mask);
        cJSON_AddStringToObject(record,"firmware_identity",peer.firmware_identity.c_str());
        cJSON_AddStringToObject(record,"hostname",peer.hostname.c_str());
    }
    auto* tasks=cJSON_AddArrayToObject(result.get(),"setup_tasks");
    for(const auto& task:migration_tasks)cJSON_AddItemToArray(tasks,cJSON_CreateString(task.c_str()));
    auto* sessions=cJSON_AddArrayToObject(result.get(),"sessions");
    for(const auto& session:execution.sessions){
        auto* item=cJSON_CreateObject();cJSON_AddItemToArray(sessions,item);
        cJSON_AddStringToObject(item,"session_id",session.session_id.str().c_str());
        cJSON_AddStringToObject(item,"zone_id",session.zone_id.str().c_str());
        cJSON_AddStringToObject(item,"activity_id",session.activity_id.str().c_str());
        cJSON_AddStringToObject(item,"generation",m::format_uint64(session.generation).c_str());
    }
    auto* observations=cJSON_AddArrayToObject(result.get(),"observations");
    for(const auto& device:equipment_observations()){
        if(!device.osd_name)continue;
        auto* item=cJSON_CreateObject();cJSON_AddItemToArray(observations,item);
        cJSON_AddStringToObject(item,"title",device.osd_name->c_str());
        cJSON_AddBoolToObject(item,"resolved",!device.stale);
        cJSON_AddBoolToObject(item,"stale",device.stale);
        cJSON_AddNumberToObject(item,"kind",device.device_type);
        std::string location="Name unavailable";
        if(device.endpoint.port.octopus_id==control->octopus_id())location=device_settings::hostname();
        else for(const auto& entry:known_names)if(entry.first==device.endpoint.port.octopus_id)location=entry.second;
        cJSON_AddStringToObject(item,"context",location.c_str());
        const m::QualifiedDeviceName name{device.endpoint.port,*device.osd_name};
        if(name.valid())cJSON_AddItemToObject(item,"qualified_name",cJSON_Parse(m::name_json(name).c_str()));
        // Diagnostic runtime addresses remain separate from persisted selectors.
        const auto address=device.endpoint;
        if(address.valid())cJSON_AddItemToObject(item,"runtime_address",cJSON_Parse(m::address_json(address).c_str()));
    }
    return send(req,result.get());
}
}

std::string active_v2_json(){return active?config_store::serialize_configuration_v2(*active):"";}
bool configuration_is_passive(){const auto membership=amity::consortium::runtime_status();return configuration_passive||!membership.coordinator||!membership.authority_ready;}
esp_err_t load_v2(amity::SystemControl& service,bool defer_configuration){
    control=&service;std::string message;
    configuration_passive=defer_configuration;
    if(configuration_passive)return ESP_OK;
    config_store::ConfigurationV2 configuration;
    auto result=config_store::load_system_configuration(configuration,message);
    const bool migration=result==ESP_ERR_NVS_NOT_FOUND;
    const bool schema_upgrade=result==ESP_OK&&configuration.legacy_encoding;
    bool staged_configuration=migration;
    if(migration){
        std::string original;result=config_store::read("config",original);
        if(result==ESP_ERR_NVS_NOT_FOUND)original="{\"version\":1,\"activities\":[]}";
        else if(result!=ESP_OK)return result;
        Configuration legacy;if(!parse_configuration(original,legacy,message))return ESP_ERR_INVALID_ARG;
        config_store::LegacyConfiguration old;
        for(const auto& activity:legacy.activities)old.activities.push_back({activity.name,activity.display,activity.source,activity.audio,activity.switch_device,activity.switch_input});
        // The saved local name already identifies its original owner/adapter.
        // Available observations refine that address; missing equipment never
        // postpones migration or adds an availability veto.
        std::vector<config_store::LegacyEvidence> evidence;
        const auto observed=service.snapshot();
        for(const auto& device:observed.devices)if(device.resolved){
            const auto count=std::count_if(observed.devices.begin(),observed.devices.end(),[&](const auto& other){return other.resolved&&other.bus==device.bus&&other.name==device.name;});
            evidence.push_back({{service.port(device.bus),device.name},true,count==1});
        }
        result=config_store::migrate_system_configuration(original,old,service.consortium_id(),service.octopus_id(),evidence,configuration,migration_tasks,message);
    }
    if(result!=ESP_OK)return result;
    amity::consortium::RuntimeStartup original;
    if(amity::consortium::runtime_read_startup(service.octopus_id(),original)!=ESP_OK || original.passive() || original.epoch>1)return ESP_ERR_INVALID_STATE;
    const auto controller=original.authority_id?original.authority_id:service.octopus_id();
    if(controller!=service.octopus_id())return ESP_ERR_INVALID_STATE;
    bool controller_bound=false;
    if(!config_store::bind_original_controller(configuration,controller,controller_bound,message))return ESP_ERR_INVALID_STATE;
    const auto expected=migration?config_store::RevisionPair{}:configuration.revisions();bool hardware_changed=false;
    if(!config_store::reconcile_hardware_profile(configuration,service.octopus_id(),service.supported_tentacles(),hardware_changed,message))return ESP_ERR_INVALID_STATE;
    if(hardware_changed||controller_bound||schema_upgrade){
        if(staged_configuration)config_store::cancel_staged_configuration();
        result=config_store::stage_system_configuration(configuration,expected,message);if(result!=ESP_OK)return result;
        result=config_store::staged_system_configuration(configuration);if(result!=ESP_OK)return result;staged_configuration=true;
    }
    esp_ota_img_states_t boot_state{};const auto* running=esp_ota_get_running_partition();
    if(staged_configuration&&running&&esp_ota_get_state_partition(running,&boot_state)==ESP_OK&&boot_state==ESP_OTA_IMG_PENDING_VERIFY){
        // Preserve the prior active record until this image has confirmed
        // protected local health; only then activate the intent-only format.
        amity::consortium::RuntimeStartup startup;if(amity::consortium::runtime_read_startup(service.octopus_id(),startup)!=ESP_OK)return ESP_ERR_INVALID_STATE;deferred_epoch=startup.epoch?startup.epoch:1;
        result=service.transition_coordinator_authority(false,deferred_epoch);if(result!=ESP_OK)return result;
        result=service.configure_passive_metadata(configuration);if(result!=ESP_OK)return result;
        deferred_migration=configuration;active=configuration;configuration_passive=true;return ESP_OK;
    }
    result=service.configure_v2(configuration,staged_configuration?std::function<esp_err_t()>([&]{return config_store::activate_system_configuration(configuration,message);}):std::function<esp_err_t()>{});
    if(result!=ESP_OK)return result;
    active=std::move(configuration);publish_inputs();return ESP_OK;
}

esp_err_t finish_migration_bridge(){
    if(!deferred_migration)return ESP_OK;
    if(!system_services::firmware_ready())return ESP_ERR_INVALID_STATE;
    auto candidate=*deferred_migration;const auto enabled=control->transition_coordinator_authority(true,deferred_epoch);if(enabled!=ESP_OK)return enabled;
    const auto activated=control->configure_v2(candidate,[candidate]() mutable {std::string message;return config_store::activate_system_configuration(candidate,message);});
    if(activated!=ESP_OK){control->transition_coordinator_authority(false,deferred_epoch);return activated;}
    deferred_migration.reset();configuration_passive=false;active=std::move(candidate);publish_inputs();return ESP_OK;
}

bool v2_request(httpd_req_t* req,const std::string& path,esp_err_t& result){
    if(path.rfind("/api/v2/",0)!=0)return false;
    const uint64_t admitted_until_ms=static_cast<uint64_t>(esp_timer_get_time()/1000)+5000;
    if(!control){result=error(req,"not_ready","System configuration is not ready.","503 Service Unavailable",true);return true;}
    if(!configuration_passive){const auto configured=control->active_configuration();if(configured&&(!active||configured->revisions()!=active->revisions())){active=*configured;publish_inputs();}}
    if(req->method==HTTP_GET){
        if(const auto target=device_settings_target(path)){
            const auto owner=*m::OctopusId::parse(*target);
            if(owner==control->octopus_id())result=web::json(req,"{\"status\":\"ready\",\"settings\":"+system_services::adapter_settings::snapshot()+",\"error\":\"\"}");
            else result=web::json(req,amity::consortium::runtime_device_settings(owner));
            return true;
        }
        if(path=="/api/v2/system"){result=snapshot(req);return true;}
        if(path=="/api/v2/membership/admission"){result=admission_response(req);return true;}
        if(path=="/api/v2/config"){result=active?web::json(req,active_v2_json()):error(req,"migration_pending","Equipment configuration needs recovery.","409 Conflict");return true;}
        if(path=="/api/v2/updates"){result=web::json(req,system_services::updates::status_json());return true;}
        if(path=="/api/v2/updates/cohort"){result=web::json(req,system_services::updates::cohort_status_json());return true;}
        if(path=="/api/v2/backups/pre-migration"){
            std::string backup;const auto loaded=config_store::read("config",backup);
            result=loaded==ESP_OK?web::json(req,backup):error(req,"backup_unavailable","No pre-migration configuration is retained.","404 Not Found");return true;
        }
        if(path.rfind("/api/v2/operations/",0)==0){
            const auto id=path.substr(19);auto found=std::find_if(operations.begin(),operations.end(),[&](const auto& o){return o.id==id;});
            result=found==operations.end()?error(req,"unknown_operation","This operation is no longer retained. Check current state before retrying.","404 Not Found"):operation_response(req,*found);return true;
        }
        result=error(req,"not_found","Unknown operation.","404 Not Found");return true;
    }
    if(req->method!=HTTP_POST){result=error(req,"method_not_allowed","Use GET or POST.","405 Method Not Allowed");return true;}
    if(configuration_is_passive()&&path!="/api/v2/updates/validate"){
        result=error(req,"coordinator_required","This Amity is passive. Open the system coordinator for equipment and activity changes.","403 Forbidden");return true;
    }
    const auto fields=request_fields(path);if(!fields){result=error(req,"not_found","Unknown operation.","404 Not Found");return true;}
    std::string data;if(!web::body(req,data)){result=ESP_FAIL;return true;}
    Json root(parse_json_input(data),cJSON_Delete);
    SensitiveRequest sensitive{data,root.get(),path=="/api/v2/membership/add"};
    if(!root||!cJSON_IsObject(root.get())||!unique_keys(root.get())){result=error(req,"invalid_json","Expected an object with unique, supported fields.");return true;}
    for(auto* field=root->child;field;field=field->next)if(std::find(fields->begin(),fields->end(),field->string)==fields->end()){result=error(req,"invalid_json","Expected supported fields only.");return true;}
    const auto id=string(root.get(),"request_id");
    if(!m::MutationId::parse(id)){result=error(req,"invalid_request_id","A canonical request ID is required.");return true;}
    const auto digest=config_store::sha256_hex(data);
    for(auto& old:operations)update_operation(old);
    auto prior=std::find_if(operations.begin(),operations.end(),[&](const auto& o){return o.id==id;});
    if(prior!=operations.end()){
        result=prior->path==path&&prior->digest==digest?operation_response(req,*prior):error(req,"request_conflict","This request ID was already used for different input.","409 Conflict");return true;
    }
    if(operations.size()==32){for(auto& old:operations)update_operation(old);auto terminal=std::find_if(operations.begin(),operations.end(),[](const auto& o){return !operation_pending(o);});if(terminal!=operations.end())operations.erase(terminal);}
    if(operations.size()>=32){result=error(req,"capacity","Too many operations are in progress.","503 Service Unavailable",true);return true;}
    Tracked operation;operation.id=id;operation.path=path;operation.digest=digest;operation.status="complete";
    auto finish=[&]{
        if(operations.size()==32){auto terminal=std::find_if(operations.begin(),operations.end(),[](const auto& o){return !operation_pending(o);});if(terminal!=operations.end())operations.erase(terminal);}
        if(operations.size()>=32){result=error(req,"capacity","Too many operations are in progress.","503 Service Unavailable",true);return;}
        operations.push_back(operation);result=operation_response(req,operations.back());
    };
    if(const auto target=device_settings_target(path)){
        const auto owner=*m::OctopusId::parse(*target);operation.candidate=owner;
        Json core(cJSON_Duplicate(root.get(),true),cJSON_Delete);cJSON_DeleteItemFromObjectCaseSensitive(core.get(),"request_id");
        amity::consortium::Json value;amity::consortium::JsonError parse_error;
        if(!amity::consortium::parse_json(json(core.get()),value,parse_error)){result=error(req,"invalid_settings","Choose valid adapter settings.");return true;}
        if(amity::consortium::runtime_write_device_settings(owner,*m::MessageId::parse(id),value,admitted_until_ms)!=ESP_OK){result=error(req,"settings_unavailable","Settings were not sent. Stop activities, check the adapters, and reconnect or update this Amity, then refresh the dialog.","409 Conflict");return true;}
        operation.status="admitted";
        finish();return true;
    }
    if(path=="/api/v2/updates/validate"){
        std::string message;const auto e=system_services::updates::stage_release(string(root.get(),"manifest"),string(root.get(),"signature"),message);
        if(e!=ESP_OK){result=error(req,"incompatible_release",message,"409 Conflict");return true;}
        finish();return true;
    }
    if(path=="/api/v2/updates/cohort"){
        auto* values=cJSON_GetObjectItemCaseSensitive(root.get(),"targets");std::vector<m::OctopusId> targets;
        if(!cJSON_IsArray(values)||cJSON_GetArraySize(values)<1||cJSON_GetArraySize(values)>3){result=error(req,"invalid_targets","Choose one to three Amity boards.");return true;}
        for(auto* value=values->child;value;value=value->next){const auto owner=cJSON_IsString(value)?m::OctopusId::parse(value->valuestring):std::nullopt;if(!owner){result=error(req,"invalid_targets","Choose a valid Amity board.");return true;}targets.push_back(*owner);}
        std::string message;const auto e=system_services::updates::stage_cohort(string(root.get(),"manifest"),string(root.get(),"signature"),std::move(targets),message);
        if(e!=ESP_OK){result=error(req,"update_unavailable",message,"409 Conflict",true);return true;}finish();return true;
    }
    if(path=="/api/v2/updates/chunk"){
        uint64_t offset=0;const auto encoded=string(root.get(),"data_hex");std::string bytes,message;
        const auto job=m::MessageId::parse(string(root.get(),"cohort_job_id"));const auto target=m::OctopusId::parse(string(root.get(),"target_octopus_id"));
        const auto digit=[](char b){return b>='0'&&b<='9'?b-'0':b>='a'&&b<='f'?b-'a'+10:-1;};
        if(!job||!target||!uint64(root.get(),"offset",offset)||encoded.empty()||encoded.size()>4096||encoded.size()%2){result=error(req,"invalid_chunk","Choose an exact update job, target and ordered image block of up to 2 KiB.");return true;}
        for(size_t n=0;n<encoded.size();n+=2){const int a=digit(encoded[n]),b=digit(encoded[n+1]);if(a<0||b<0){result=error(req,"invalid_chunk","Image blocks use lowercase hexadecimal.");return true;}bytes+=char(a*16+b);}
        const auto e=system_services::updates::cohort_chunk(*job,*target,offset,bytes,message);if(e!=ESP_OK){result=error(req,"update_checkpoint",message,"409 Conflict",true);return true;}finish();return true;
    }
    if(path=="/api/v2/updates/install"||path=="/api/v2/updates/cancel"){
        const auto job=m::MessageId::parse(string(root.get(),"cohort_job_id"));const auto target=m::OctopusId::parse(string(root.get(),"target_octopus_id"));
        if(!job||(path=="/api/v2/updates/install"&&!target)){result=error(req,"invalid_job","Choose the current update job and target.");return true;}
        std::string message;const auto e=path=="/api/v2/updates/install"?system_services::updates::cohort_install(*job,*target,message):system_services::updates::cancel_cohort(*job,message);
        if(e!=ESP_OK){result=error(req,"update_uncertain",message,"409 Conflict",true);return true;}finish();return true;
    }
    if(path=="/api/v2/replacements/preview"||path=="/api/v2/replacements/apply"){
        config_store::RevisionPair expected;std::string message;
        if(!active||!expected_pair(root.get(),expected)||expected!=active->revisions()){result=error(req,"stale_revision","Equipment or configuration changed. Review the replacement mapping.","409 Conflict");return true;}
        config_store::ReplacementApproval approval;
        const auto previous=m::OctopusId::parse(string(root.get(),"previous_octopus_id")),replacement=m::OctopusId::parse(string(root.get(),"replacement_octopus_id"));
        if(!previous||!replacement||*previous==control->octopus_id()){result=error(req,"coordinator_replacement","The original Amity cannot be replaced by an added device. This operation replaces added devices only.","409 Conflict");return true;}
        approval.previous=*previous;approval.replacement=*replacement;
        approval.previous_revoked=true;
        for(const auto& peer:amity::consortium::runtime_peers()){
            if(peer.id==*previous&&(peer.enrolled||peer.provisional))approval.previous_revoked=false;
            if(peer.id==*replacement&&peer.enrolled&&!peer.provisional)approval.replacement_enrolled=true;
        }
        approval.previous_isolated=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root.get(),"old_hardware_isolated"));
        auto* ports=cJSON_GetObjectItemCaseSensitive(root.get(),"ports");auto* equipment=cJSON_GetObjectItemCaseSensitive(root.get(),"equipment");
        if(!cJSON_IsArray(ports)||cJSON_GetArraySize(ports)<1||cJSON_GetArraySize(ports)>10||!cJSON_IsArray(equipment)||cJSON_GetArraySize(equipment)>64){result=error(req,"invalid_mapping","Map each old connector and its identified equipment.");return true;}
        for(auto* item=ports->child;item;item=item->next){
            config_store::ReplacementPort mapping;
            if(!object_fields(item,{"previous","replacement"})||!parse_port_value(cJSON_GetObjectItemCaseSensitive(item,"previous"),mapping.previous)||!parse_port_value(cJSON_GetObjectItemCaseSensitive(item,"replacement"),mapping.replacement)){result=error(req,"invalid_mapping","Choose explicit previous and replacement connectors.");return true;}
            approval.ports.push_back(std::move(mapping));
        }
        for(auto* item=equipment->child;item;item=item->next){
            if(!object_fields(item,{"component_id","previous","replacement","same_equipment_confirmed"})){result=error(req,"invalid_mapping","Confirm each logical piece of equipment.");return true;}
            const auto component=m::ComponentId::parse(string(item,"component_id"));const auto from=m::parse_name_json(json(cJSON_GetObjectItemCaseSensitive(item,"previous"))),to=m::parse_name_json(json(cJSON_GetObjectItemCaseSensitive(item,"replacement")));
            if(!component||!from||!to){result=error(req,"invalid_mapping","Choose the intended equipment location on the mapped connector.");return true;}
            approval.equipment.push_back({*component,*from,*to,bool(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(item,"same_equipment_confirmed")))});
        }
        config_store::ConfigurationV2 candidate;std::vector<m::ActivityId> affected;
        if(!config_store::propose_octopus_replacement(*active,approval,candidate,affected,message)){result=error(req,"replacement_not_ready",message,"409 Conflict");return true;}
        if(path=="/api/v2/replacements/apply"){
            const auto queued=amity::activate_distributed_configuration(std::move(candidate),expected,*m::MessageId::parse(id));
            if(queued!=ESP_OK){result=error(req,"activation_pending","The replacement configuration could not be saved. Reload before retrying.","409 Conflict",true);return true;}
            operation.status="configuring";finish();return true;
        }
        Json response(cJSON_CreateObject(),cJSON_Delete);cJSON_AddItemToObject(response.get(),"configuration",cJSON_Parse(config_store::serialize_configuration_v2(candidate).c_str()));cJSON_AddStringToObject(response.get(),"canonical_configuration",config_store::serialize_configuration_v2(candidate).c_str());
        auto* ids=cJSON_AddArrayToObject(response.get(),"affected_activity_ids");for(auto activity:affected)cJSON_AddItemToArray(ids,cJSON_CreateString(activity.str().c_str()));
        cJSON_AddStringToObject(response.get(),"pairing_notice","Reconnect and review the equipment on the replacement Amity before starting playback.");result=send(req,response.get());return true;
    }
    if(path=="/api/v2/config"||path=="/api/v2/config/validate"){
        config_store::RevisionPair expected;config_store::ConfigurationV2 candidate;std::string message;
        auto* document=cJSON_GetObjectItemCaseSensitive(root.get(),"configuration");
        if(!active||!expected_pair(root.get(),expected)||expected!=active->revisions()){result=error(req,"stale_revision","Configuration changed. Reload the current choices.","409 Conflict");return true;}
        if(!document||!config_store::parse_configuration_v2(json(document),candidate,message)||!config_store::reconcile_external_slots(*active,candidate,message)){
            result=error(req,"invalid_configuration",message.empty()?"A valid configuration is required.":message);return true;
        }
        if(candidate.consortium_id!=active->consortium_id||candidate.controller_octopus_id!=active->controller_octopus_id||candidate.primary_zone_id!=active->primary_zone_id){result=error(req,"identity_change","System identity and the home-app screen cannot change through an ordinary configuration edit.","409 Conflict");return true;}
        if(path=="/api/v2/config/validate"){finish();return true;}
        const auto e=amity::activate_distributed_configuration(std::move(candidate),expected,*m::MessageId::parse(id));
        if(e!=ESP_OK){result=error(req,"activation_pending","A configuration activation is already in progress or its revision changed.","409 Conflict",true);return true;}
        operation.status="configuring";
        finish();return true;
    }
    if(path=="/api/v2/membership/scan"){
        const auto e=amity::consortium::runtime_scan_admission(*m::MessageId::parse(id));
        if(e!=ESP_OK){result=error(req,"scan_unavailable","The search could not start. Close this window and try again.","409 Conflict",true);return true;}
        finish();return true;
    }
    if(path=="/api/v2/membership/cancel"){
        const auto scan=m::MessageId::parse(string(root.get(),"scan_id"));
        if(!scan){result=error(req,"invalid_scan","Choose the current search.");return true;}
        const auto e=amity::consortium::runtime_cancel_admission(*scan);
        if(e!=ESP_OK){
            const auto state=amity::consortium::runtime_admission_status();
            const bool finishing=state.scan_id==*scan&&(state.phase=="finishing"||state.phase=="complete");
            result=error(req,finishing?"addition_finishing":"stale_scan",finishing?"This Amity is finishing setup. Check the Amity devices list shortly.":"This search is no longer active.","409 Conflict");return true;
        }
        finish();return true;
    }
    if(path=="/api/v2/membership/add"){
        AdmissionSelection selection;std::string message;
        const auto state=amity::consortium::runtime_admission_status();
        if(!admission_selection(root.get(),state.requires_setup_code,selection,message)){result=error(req,"invalid_addition",message);return true;}
        SensitiveString clear_code{selection.setup_code};
        if(state.scan_id!=selection.scan_id){result=error(req,"stale_scan","The search changed. Close this window and try again.","409 Conflict");return true;}
        if(selection.name==device_settings::hostname()){result=error(req,"name_in_use","That name belongs to this Amity. Choose another name.","409 Conflict");return true;}
        for(const auto& peer:amity::consortium::runtime_peers())if((peer.enrolled||peer.provisional)&&peer.hostname==selection.name){result=error(req,"name_in_use","That name is already used by an added Amity. Choose another name.","409 Conflict");return true;}
        const auto e=amity::consortium::runtime_add_octopus(selection.scan_id,selection.candidate_token,selection.name,selection.setup_code);
        if(e!=ESP_OK){result=error(req,"enrollment_unavailable","Adding this Amity could not start. Check the selected device and its setup window, then try again.","409 Conflict",true);return true;}
        operation.scan_id=selection.scan_id;operation.name=selection.name;operation.status="admitted";finish();return true;
    }
    if(path=="/api/v2/membership/remove"){
        const auto owner=m::OctopusId::parse(string(root.get(),"octopus_id"));
        uint64_t membership_revision=0;std::optional<m::BootId> target_boot;
        auto* reviewed_boot=cJSON_GetObjectItemCaseSensitive(root.get(),"expected_target_boot_id");
        if(cJSON_IsString(reviewed_boot))target_boot=m::BootId::parse(reviewed_boot->valuestring);
        if(!owner||!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root.get(),"confirm"))||!uint64(root.get(),"expected_membership_revision",membership_revision)||(!cJSON_IsNull(reviewed_boot)&&!target_boot)){result=error(req,"invalid_removal","Refresh the device list, choose an Amity and confirm its removal.");return true;}
        if(*owner==control->octopus_id()){result=error(req,"invalid_removal","This Amity cannot remove itself.","409 Conflict");return true;}
        const auto e=amity::consortium::runtime_remove_octopus(*owner,*m::MessageId::parse(id),membership_revision,target_boot);
        if(e!=ESP_OK){result=error(req,e==ESP_ERR_NOT_SUPPORTED?"update_required":"removal_unavailable",e==ESP_ERR_NOT_SUPPORTED?"Update this Amity's firmware before removing it so it can be factory reset.":"Only added Amity devices can be removed. Refresh the device list and try again.","409 Conflict");return true;}
        operation.candidate=*owner;operation.status="admitted";
        finish();return true;
    }
    result=error(req,"not_found","Unknown operation.","404 Not Found");return true;
}
}
