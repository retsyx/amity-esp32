#include "activity_editor.hpp"
#include "activity_editor_model.hpp"
#include "configuration_replacement.hpp"
#include "amity/system_sync.hpp"
#include "amity/consortium/runtime.hpp"
#include "web.hpp"
#include "system_services.hpp"
#include "json_input.hpp"
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "device_settings.hpp"
#include <algorithm>
#include <memory>
#include <set>
#include <cstring>

namespace web_admin {
namespace {
namespace cs=config_store;
namespace m=amity::system;
namespace ae=activity_editor;
using Json=std::unique_ptr<cJSON,decltype(&cJSON_Delete)>;
m::BootId current_boot(){return m::BootId::parse(system_services::boot_id()).value_or(m::BootId{});}
std::string printed(cJSON* value){char* text=cJSON_PrintUnformatted(value);std::string out=text?text:"";cJSON_free(text);return out;}
std::string text(const cJSON* value,const char* key){const auto* field=cJSON_GetObjectItemCaseSensitive(value,key);return cJSON_IsString(field)&&field->valuestring?field->valuestring:"";}
bool keys(const cJSON* value,std::initializer_list<const char*> allowed){
    if(!cJSON_IsObject(value))return false;
    std::set<std::string> seen;
    for(auto* child=value->child;child;child=child->next){if(!child->string||!seen.insert(child->string).second)return false;bool known=false;for(const auto* key:allowed)known|=!strcmp(key,child->string);if(!known)return false;}
    return true;
}

bool revision(const cJSON* value,const char* key,uint64_t& out){const auto parsed=m::parse_uint64(text(value,key));if(!parsed)return false;out=*parsed;return true;}
void decimal(cJSON* value,const char* key,uint64_t number){cJSON_AddStringToObject(value,key,m::format_uint64(number).c_str());}
esp_err_t respond(httpd_req_t* req,cJSON* value){return web::json(req,printed(value));}
esp_err_t reject(httpd_req_t* req,const std::string& reason,const char* code="409 Conflict"){return web::error(req,reason.c_str(),code);}
bool pending(const std::string& phase){return phase=="admitted"||phase=="configuring"||phase=="activating";}
struct Operation {
    m::MessageId id;std::string digest,kind,stage="admitted",error;
    cs::RevisionPair revisions;m::BootId boot;
    std::vector<std::string> unresolved;
};
std::vector<Operation> operations;
uint32_t scan_generation=0;
std::vector<m::CecObservation> observations(amity::SystemControl& control){auto out=amity::discovered_equipment();if(out.empty())out=control.native_observations();return out;}
void update(amity::SystemControl& control,Operation& op){
    if(!pending(op.stage))return;
    if(op.kind=="config"){
        const auto status=amity::configuration_activation_status();if(status.request_id==op.id){op.stage=status.phase;op.error=status.error;}return;
    }
    if(op.kind!="activity")return;
    const auto config=control.active_configuration();
    if(!config||config->revisions()!=op.revisions||current_boot()!=op.boot){op.stage="failed";op.error="The saved configuration or controller changed before the request completed";return;}
    for(const auto& result:control.execution_snapshot().activity_outcomes)if(result.request_id==op.id){
        op.stage=result.stage=="applied"?"complete":result.stage;op.error=result.error;return;
    }
}
esp_err_t operation_json(httpd_req_t* req,const Operation& op){
    Json root(cJSON_CreateObject(),cJSON_Delete);cJSON_AddStringToObject(root.get(),"operation_id",op.id.str().c_str());cJSON_AddStringToObject(root.get(),"stage",op.stage.c_str());cJSON_AddStringToObject(root.get(),"status",op.stage.c_str());cJSON_AddStringToObject(root.get(),"message",op.error.c_str());if(op.stage=="complete"&&!op.error.empty()){cJSON_AddStringToObject(root.get(),"warning",op.error.c_str());cJSON_AddBoolToObject(root.get(),"partial",true);}cJSON_AddBoolToObject(root.get(),"saved",op.stage=="applied");auto* reasons=cJSON_AddArrayToObject(root.get(),"unresolved");for(const auto& reason:op.unresolved)cJSON_AddItemToArray(reasons,cJSON_CreateString(reason.c_str()));return respond(req,root.get());
}
cJSON* encode_choices(const std::vector<ae::Choice>& choices){
    auto* array=cJSON_CreateArray();for(const auto& c:choices){auto* value=cJSON_CreateObject();cJSON_AddItemToArray(array,value);cJSON_AddStringToObject(value,"value",c.value.c_str());cJSON_AddStringToObject(value,"title",c.title.c_str());cJSON_AddStringToObject(value,"location",c.location.c_str());auto* roles=cJSON_AddArrayToObject(value,"roles");for(const auto& role:c.roles)cJSON_AddItemToArray(roles,cJSON_CreateString(role.c_str()));}return array;
}

}
bool activity_editor_request(amity::SystemControl& control,httpd_req_t* req,const std::string& path,esp_err_t& result){
    constexpr const char* prefix="/api/v2/activity-editor";
    for(auto& op:operations)update(control,op);
    const std::string operation_prefix="/api/v2/operations/";
    if(req->method==HTTP_GET&&path.rfind(operation_prefix,0)==0){const auto requested=m::MessageId::parse(path.substr(operation_prefix.size()));for(const auto& op:operations)if(requested&&op.id==*requested){result=operation_json(req,op);return true;}return false;}
    if(path.rfind(prefix,0)!=0)return false;
    const auto config=control.active_configuration();
    if(!config||!config->controller_octopus_id||*config->controller_octopus_id!=control.octopus_id()){result=reject(req,"Activity administration belongs to the original Amity controller","403 Forbidden");return true;}
    if(req->method!=HTTP_GET&&!control.accepts_local_input()){result=reject(req,"Activity control is unavailable while this Amity is passive or updating","503 Service Unavailable");return true;}
    ae::OwnerHostnames hostnames;for(const auto& name:amity::consortium::runtime_known_hostnames())hostnames[name.first]=name.second;
    hostnames[control.octopus_id()]=device_settings::hostname();
    const auto peers=amity::consortium::runtime_peers();
    ae::OwnerCapacities capacities{{control.octopus_id(),control.supported_tentacles()}};
    for(const auto& peer:peers)if(peer.boot_id)capacities[peer.id]=peer.supported_tentacles;
    const bool has_added_amities=std::any_of(peers.begin(),peers.end(),[&](const auto& peer){return peer.id!=control.octopus_id()&&peer.enrolled;});
    const auto state=control.execution_snapshot();const auto native=observations(control);const auto choices=ae::choices(*config,native,hostnames,has_added_amities,capacities);
    if(req->method==HTTP_GET&&path==prefix){
        Json root(cJSON_CreateObject(),cJSON_Delete);cJSON_AddNumberToObject(root.get(),"version",1);decimal(root.get(),"expected_config_revision",config->config_revision);decimal(root.get(),"expected_presentation_revision",config->presentation_revision);cJSON_AddItemToObject(root.get(),"device_choices",encode_choices(choices));auto* rows=cJSON_AddArrayToObject(root.get(),"activities");
        for(const auto& id:ae::projected_activities(*config))for(const auto& a:config->activities)if(a.activity_id==id){auto* row=cJSON_CreateObject();cJSON_AddItemToArray(rows,row);cJSON_AddStringToObject(row,"activity_id",a.activity_id.str().c_str());cJSON_AddStringToObject(row,"name",a.label.c_str());for(const auto& ref:std::vector<std::pair<const char*,std::optional<m::ComponentId>>>{{"display",a.display},{"source",a.source},{"audio",a.audio}})cJSON_AddStringToObject(row,ref.first,ae::component_value(ref.second).c_str());if(a.switch_device){auto* sw=cJSON_AddObjectToObject(row,"switch");cJSON_AddStringToObject(sw,"device",ae::component_value(a.switch_device).c_str());cJSON_AddNumberToObject(sw,"input",a.switch_input);}}
        result=respond(req,root.get());return true;
    }
    if(req->method==HTTP_GET&&path==std::string(prefix)+"/status"){
        const auto legacy=control.snapshot();Json root(cJSON_CreateObject(),cJSON_Delete);cJSON_AddStringToObject(root.get(),"version",esp_app_get_description()->version);cJSON_AddBoolToObject(root.get(),"ready",legacy.ready);cJSON_AddBoolToObject(root.get(),"activity_settling",state.activity_settling);cJSON_AddStringToObject(root.get(),"activity_error",state.error.c_str());decimal(root.get(),"expected_config_revision",config->config_revision);decimal(root.get(),"expected_presentation_revision",config->presentation_revision);decimal(root.get(),"expected_state_revision",state.state_revision);cJSON_AddStringToObject(root.get(),"expected_boot_id",current_boot().str().c_str());
        cJSON_AddStringToObject(root.get(),"hostname",device_settings::hostname());cJSON_AddStringToObject(root.get(),"front_physical_address",device_settings::format_physical_address(device_settings::front_physical_address()).c_str());
        if(legacy.front_physical_address==0xffff)cJSON_AddNullToObject(root.get(),"determined_front_physical_address");else cJSON_AddStringToObject(root.get(),"determined_front_physical_address",device_settings::format_physical_address(legacy.front_physical_address).c_str());
        cJSON_AddNumberToObject(root.get(),"free_heap",esp_get_free_heap_size());cJSON_AddNumberToObject(root.get(),"free_internal_heap",heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));cJSON_AddNumberToObject(root.get(),"largest_internal_block",heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
        wifi_ap_record_t ap{};const bool connected=esp_wifi_sta_get_ap_info(&ap)==ESP_OK;cJSON_AddBoolToObject(root.get(),"wifi_connected",connected);if(connected){cJSON_AddNumberToObject(root.get(),"wifi_rssi",ap.rssi);cJSON_AddNumberToObject(root.get(),"wifi_channel",ap.primary);}cJSON_AddStringToObject(root.get(),"hardware","Amity Board / Fin");
        const auto projection=ae::projected_activities(*config);int current=-1;for(const auto& session:state.sessions)for(size_t n=0;n<projection.size();++n)if(projection[n]==session.activity_id)current=static_cast<int>(n);cJSON_AddNumberToObject(root.get(),"current_activity",current);
        bool scanning=legacy.discovering;for(const auto& owner:control.owned_snapshots())scanning=scanning||owner.discovering;cJSON_AddBoolToObject(root.get(),"discovering",scanning);cJSON_AddNumberToObject(root.get(),"discovery_generation",scan_generation+legacy.discovery_generation);cJSON_AddNumberToObject(root.get(),"activity_generation",legacy.activity_generation);cJSON_AddNumberToObject(root.get(),"front_address",legacy.front_address);cJSON_AddNumberToObject(root.get(),"back_address",legacy.back_address);cJSON_AddNumberToObject(root.get(),"tx_errors",legacy.tx_errors);cJSON_AddNumberToObject(root.get(),"rx_dropped",legacy.rx_dropped);
        auto* devices=cJSON_AddArrayToObject(root.get(),"devices");for(const auto& c:choices)if(c.observation){const auto& o=*c.observation;auto* item=cJSON_CreateObject();cJSON_AddItemToArray(devices,item);cJSON_AddStringToObject(item,"name",c.value.c_str());cJSON_AddStringToObject(item,"title",c.title.c_str());cJSON_AddStringToObject(item,"location",c.location.c_str());cJSON_AddStringToObject(item,"bus",o.endpoint.port.side==m::Side::front?"front":"back");cJSON_AddNumberToObject(item,"address",o.endpoint.logical_address);cJSON_AddNumberToObject(item,"physical_address",o.physical_address);cJSON_AddNumberToObject(item,"type",o.device_type);cJSON_AddNumberToObject(item,"vendor_id",o.vendor_id);}cJSON_AddItemToObject(root.get(),"device_choices",encode_choices(choices));result=respond(req,root.get());return true;
    }
    if(req->method!=HTTP_POST||(path!=prefix&&path!=std::string(prefix)+"/discover"&&path!=std::string(prefix)+"/activity"&&path!=std::string(prefix)+"/restore")){result=reject(req,"Unknown activity editor operation","404 Not Found");return true;}
    std::string body;if(!web::body(req,body)){result=ESP_FAIL;return true;}Json root(parse_json_input(body),cJSON_Delete);
    if(!root||body.size()>cs::configuration_compatibility_limit||body.find('\0')!=std::string::npos){result=reject(req,"Invalid activity editor JSON","400 Bad Request");return true;}
    const auto request=m::MessageId::parse(text(root.get(),"request_id"));const auto digest=cs::sha256_hex(path+body);if(!request){result=reject(req,"An activity request identity is required","400 Bad Request");return true;}
    for(const auto& op:operations)if(op.id==*request){result=op.digest==digest?operation_json(req,op):reject(req,"The request identity already describes a different action");return true;}
    if(operations.size()>=32){const auto done=std::find_if(operations.begin(),operations.end(),[](const auto& op){return !pending(op.stage);});if(done==operations.end()){result=reject(req,"Activity administration is busy","503 Service Unavailable");return true;}operations.erase(done);}
    Operation op;op.id=*request;op.digest=digest;op.revisions=config->revisions();op.boot=current_boot();
    if(path==std::string(prefix)+"/restore"){
        cs::ConfigurationV2 backup,candidate;std::string error;
        if(!state.sessions.empty()||state.activity_settling){result=reject(req,"Stop playback before restoring a saved configuration");return true;}
        if(!ae::parse_restore_request(root.get(),*config,current_boot(),backup,error)||!cs::restore_main_configuration(backup,*config,true,candidate,op.unresolved,error)){result=reject(req,error,"400 Bad Request");return true;}
        if(amity::activate_distributed_configuration(std::move(candidate),config->revisions(),op.id)!=ESP_OK){result=reject(req,"The backup could not be saved");return true;}op.kind="config";op.stage="configuring";
    }else if(path==std::string(prefix)+"/discover"){
        if(!keys(root.get(),{"request_id"})){result=reject(req,"Unknown scan field","400 Bad Request");return true;}
        if(amity::request_equipment_scan()!=ESP_OK){result=reject(req,"The equipment scan could not be submitted","503 Service Unavailable");return true;}
        if(scan_generation<UINT32_MAX)++scan_generation;
        op.kind="scan";op.stage="complete";
    }else if(path==prefix){
        ae::Submission submission;std::string parse_error;if(!ae::parse_submission(body,*config,submission,parse_error)){result=reject(req,parse_error);return true;}
        const auto& edits=submission.rows;
        cs::ConfigurationV2 candidate;std::string error;if(!ae::edit(*config,edits,choices,[](uint8_t* data,size_t size){esp_fill_random(data,size);return true;},candidate,op.unresolved,error)){result=reject(req,error);return true;}
        if(amity::activate_distributed_configuration(std::move(candidate),config->revisions(),op.id)!=ESP_OK){result=reject(req,"The activity configuration could not be saved");return true;}op.kind="config";op.stage="configuring";
    }else{
        uint64_t revision_value,semantic;if(!keys(root.get(),{"request_id","activity_id","expected_config_revision","expected_state_revision","expected_boot_id"})||!revision(root.get(),"expected_config_revision",semantic)||semantic!=config->config_revision||!revision(root.get(),"expected_state_revision",revision_value)||revision_value!=state.state_revision||text(root.get(),"expected_boot_id")!=current_boot().str()){result=reject(req,"Playback changed. Review the current activity before continuing");return true;}
        const auto* target=cJSON_GetObjectItemCaseSensitive(root.get(),"activity_id");if(!target){result=reject(req,"Choose an activity or standby","400 Bad Request");return true;}
        esp_err_t admitted=ESP_ERR_INVALID_ARG;
        if(cJSON_IsNull(target)){
            admitted=control.request_activity(-1);op.kind="standby";op.stage="complete";
        }else{
            const auto id=m::ActivityId::parse(text(root.get(),"activity_id"));const auto activity=std::find_if(config->activities.begin(),config->activities.end(),[&](const auto& a){return id&&a.activity_id==*id;});
            if(activity==config->activities.end()){result=reject(req,"The selected activity no longer exists");return true;}
            admitted=control.request_activity_v2(activity->zone_id,activity->activity_id,op.id,revision_value);op.kind="activity";
        }
        if(admitted!=ESP_OK){result=reject(req,admitted==ESP_ERR_TIMEOUT?"System is settling. Try again shortly.":"The activity request could not be admitted");return true;}
        update(control,op);
    }
    operations.push_back(std::move(op));result=operation_json(req,operations.back());return true;
}
}
