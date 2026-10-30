#!/usr/bin/env python3
"""Execute Remove review, persistence and operation tracking against real journals."""

import os
from pathlib import Path
import shlex
import subprocess
import tempfile

from consortium_gatt_callback_test import function

ROOT = Path(__file__).resolve().parents[1]


def block(source, marker):
    start = source.index(marker)
    opening = source.index('{', start)
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == '{':
            depth += 1
        elif source[index] == '}':
            depth -= 1
            if depth == 0:
                return source[start:index + 1]
    raise AssertionError(marker)


def main():
    source = (ROOT / 'components/web_admin/system_admin.cpp').read_text()
    fixture = (ROOT / 'tests/system_sync_protocol_test.cpp').read_text().split('\nnamespace {\n', 1)[0]
    helpers = ''.join(function(source, marker) for marker in (
        'std::string string(', 'std::string json(', 'bool uint64(', 'bool expected_pair(',
        'bool reviewed_activities(', 'bool reviewed_member('))
    helpers += source[source.index('bool object_fields('):source.index('bool parse_port_value(')]
    helpers += function(source, 'bool parse_port_value(')
    tracked = block(source, 'struct Tracked {') + ';\n'
    update = block(source[source.index('void update_operation('):], '    if(removing)')
    remove = block(source[source.index('    Tracked operation;'):], '    if(path=="/api/membership/remove")').replace('root.get()', 'root')
    replacement = block(source[source.index('    Tracked operation;'):], '    if(path=="/api/replacements/preview"').replace('root.get()', 'root')
    test = '#include "configuration_replacement.hpp"\n' + fixture + r'''
namespace removal_fixture {
namespace m=amity::system;
constexpr esp_err_t ESP_ERR_NOT_SUPPORTED=0x106;
using Json=std::unique_ptr<cJSON,decltype(&cJSON_Delete)>;
using httpd_req_t=int;
amity::SystemControl* control=nullptr;
std::unique_ptr<amity::SystemControl> owned_control;
std::optional<config_store::Configuration> active;
std::string rejection,rejection_message;
std::string serialized;
bool finished=false;
esp_err_t error(httpd_req_t*,const char* code,const std::string& message,const char* ="400 Bad Request",bool=false){rejection=code;rejection_message=message;return ESP_FAIL;}
esp_err_t send(httpd_req_t*,cJSON* root){char* text=cJSON_PrintUnformatted(root);serialized=text;cJSON_free(text);return ESP_OK;}
}
namespace amity::consortium {
unsigned revoked=0,forgotten=0;
bool forget_fails=false;
std::string removal_stage="admitted",reset_result="pending";
std::vector<std::pair<model::OctopusId,std::string>> names;
std::vector<std::pair<model::OctopusId,std::string>> runtime_known_hostnames(){return names;}
esp_err_t runtime_remove_octopus(model::OctopusId owner,model::MessageId,uint64_t expected,std::optional<model::BootId> boot){
    if(expected!=fixture::role.membership_revision)return ESP_ERR_INVALID_STATE;
    auto peer=std::find_if(fixture::peers.begin(),fixture::peers.end(),[&](const auto& peer){return peer.id==owner;});
    if(peer==fixture::peers.end()||(boot&&*boot!=peer->boot_id))return ESP_ERR_INVALID_STATE;
    ++revoked;++fixture::role.membership_revision;fixture::peers.erase(peer);return ESP_OK;
}
std::string runtime_removal_operation(model::OctopusId,model::MessageId){return "{\"stage\":\""+removal_stage+"\",\"reset\":\""+reset_result+"\"}";}
esp_err_t runtime_forget_removed_octopus(model::OctopusId owner){
    ++forgotten;if(forget_fails)return ESP_FAIL;
    assert(std::none_of(fixture::peers.begin(),fixture::peers.end(),[&](const auto& peer){return peer.id==owner;}));
    names.erase(std::remove_if(names.begin(),names.end(),[&](const auto& item){return item.first==owner;}),names.end());return ESP_OK;
}
}
namespace removal_fixture {
''' + tracked + helpers + r'''
void update_remove(Tracked& operation){const bool removing=true;
''' + update + r'''
}
Tracked captured;
bool perform(cJSON* root){
    const std::string path="/api/membership/remove",id=string(root,"request_id");httpd_req_t* req=nullptr;[[maybe_unused]] esp_err_t result=ESP_OK;
    Tracked operation;operation.id=id;operation.path=path;operation.status="complete";
    auto finish=[&]{finished=true;update_remove(operation);captured=operation;};
''' + remove + r'''
    assert(false);return false;
}
bool perform_replace(cJSON* root,bool apply){
    const std::string path=apply?"/api/replacements/apply":"/api/replacements/preview",id=string(root,"request_id");httpd_req_t* req=nullptr;[[maybe_unused]] esp_err_t result=ESP_OK;
    Tracked operation;operation.id=id;operation.path=path;operation.status="complete";
    auto finish=[&]{finished=true;update_remove(operation);captured=operation;};
''' + replacement + r'''
    assert(false);return false;
}
void prepare(bool enrolled=true){
    fixture::reset();rejection.clear();rejection_message.clear();finished=false;captured={};
    amity::consortium::revoked=amity::consortium::forgotten=0;amity::consortium::forget_fails=false;
    amity::consortium::removal_stage="admitted";amity::consortium::reset_result="pending";
    owned_control=std::make_unique<amity::SystemControl>(fixture::control());control=owned_control.get();
    control->configuration=switch_configuration(control->configuration);
    const auto owner=id<m::OctopusId>(9);control->configuration.octopuses.push_back({owner,"Member","Member",false,1});
    for(auto& component:control->configuration.components)if(component.record.component_id==control->configuration.activities.front().source)component.record.qualified_names[0].port.octopus_id=owner;
    Configuration committed;commit(*fixture::journal,control->configuration,{},committed);control->configuration=committed;active=committed;
    control->applied.applied_config_revision=committed.config_revision;control->applied.applied_presentation_revision=committed.presentation_revision;
    fixture::role.membership_revision=7;
    if(enrolled)fixture::peers={fixture::remote(9,19,1,1)};
    amity::consortium::names={{owner,"member"}};
    assert(amity::start_configuration_synchronization(*control)==ESP_OK);
}
Json request(bool enrolled=true){
    Json root(cJSON_CreateObject(),cJSON_Delete);
    cJSON_AddStringToObject(root.get(),"request_id",id<m::MessageId>(200).str().c_str());
    cJSON_AddStringToObject(root.get(),"octopus_id",id<m::OctopusId>(9).str().c_str());
    cJSON_AddBoolToObject(root.get(),"confirm",true);
    cJSON_AddStringToObject(root.get(),"expected_membership_revision","7");
    cJSON_AddStringToObject(root.get(),"expected_config_revision",std::to_string(active->config_revision).c_str());
    cJSON_AddStringToObject(root.get(),"expected_presentation_revision",std::to_string(active->presentation_revision).c_str());
    if(enrolled)cJSON_AddStringToObject(root.get(),"expected_target_boot_id",id<m::BootId>(19).str().c_str());else cJSON_AddNullToObject(root.get(),"expected_target_boot_id");
    auto* affected=cJSON_AddArrayToObject(root.get(),"affected_activity_ids");
    cJSON_AddItemToArray(affected,cJSON_CreateString(active->activities.front().activity_id.str().c_str()));return root;
}
cJSON* port_value(const m::CecPortRef& port){auto* value=cJSON_Parse(m::name_json({port,"OSD"}).c_str());cJSON_DeleteItemFromObjectCaseSensitive(value,"osd_name");return value;}
Json replacement_request(){
    auto root=request();cJSON_DeleteItemFromObjectCaseSensitive(root.get(),"octopus_id");cJSON_DeleteItemFromObjectCaseSensitive(root.get(),"confirm");cJSON_DeleteItemFromObjectCaseSensitive(root.get(),"affected_activity_ids");
    cJSON_AddStringToObject(root.get(),"previous_octopus_id",id<m::OctopusId>(9).str().c_str());cJSON_AddStringToObject(root.get(),"replacement_octopus_id",id<m::OctopusId>(10).str().c_str());cJSON_AddBoolToObject(root.get(),"old_hardware_isolated",true);
    auto* ports=cJSON_AddArrayToObject(root.get(),"ports");auto* equipment=cJSON_AddArrayToObject(root.get(),"equipment");
    for(const auto& component:active->components)for(const auto& previous:component.record.qualified_names)if(previous.port.octopus_id==id<m::OctopusId>(9)){
        auto next=previous;next.port.octopus_id=id<m::OctopusId>(10);
        auto* mapping=cJSON_CreateObject();cJSON_AddItemToObject(mapping,"previous",port_value(previous.port));cJSON_AddItemToObject(mapping,"replacement",port_value(next.port));cJSON_AddItemToArray(ports,mapping);
        auto* selection=cJSON_CreateObject();cJSON_AddStringToObject(selection,"component_id",component.record.component_id.str().c_str());cJSON_AddItemToObject(selection,"previous",cJSON_Parse(m::name_json(previous).c_str()));cJSON_AddItemToObject(selection,"replacement",cJSON_Parse(m::name_json(next).c_str()));cJSON_AddBoolToObject(selection,"same_equipment_confirmed",true);cJSON_AddItemToArray(equipment,selection);
    }
    return root;
}
int run(){
    using namespace amity::consortium;
    for(const auto* field:{"expected_config_revision","expected_presentation_revision","expected_membership_revision"}){
        prepare();auto root=request();cJSON_ReplaceItemInObjectCaseSensitive(root.get(),field,cJSON_CreateString("99"));perform(root.get());assert(rejection=="stale_removal"&&!finished&&revoked==0&&forgotten==0);
    }
    prepare();auto root=request();cJSON_ReplaceItemInObjectCaseSensitive(root.get(),"affected_activity_ids",cJSON_CreateArray());perform(root.get());assert(rejection=="stale_removal"&&revoked==0);
    prepare();root=request();cJSON_AddItemToArray(cJSON_GetObjectItemCaseSensitive(root.get(),"affected_activity_ids"),cJSON_CreateString(active->activities.front().activity_id.str().c_str()));perform(root.get());assert(rejection=="stale_removal"&&revoked==0);
    prepare();root=request();cJSON_ReplaceItemInObjectCaseSensitive(root.get(),"expected_target_boot_id",cJSON_CreateString(id<m::BootId>(99).str().c_str()));perform(root.get());assert(rejection=="stale_removal"&&revoked==0);
    prepare();root=request();cJSON_ReplaceItemInObjectCaseSensitive(root.get(),"expected_target_boot_id",cJSON_CreateNull());perform(root.get());assert(rejection=="stale_removal"&&revoked==0);
    prepare();root=request();cJSON_ReplaceItemInObjectCaseSensitive(root.get(),"confirm",cJSON_CreateBool(false));perform(root.get());assert(rejection=="invalid_removal"&&revoked==0);
    prepare();root=request();control->configure_result=ESP_ERR_NO_MEM;perform(root.get());assert(!finished&&revoked==0&&forgotten==0);
    prepare();root=request();fixture::memory.fail_marker=true;perform(root.get());assert(finished&&captured.status=="failed"&&revoked==1&&forgotten==0&&captured.reset=="pending");
    removal_stage="complete";reset_result="scheduled";update_remove(captured);assert(captured.status=="failed"&&captured.reset=="scheduled");
    prepare();root=request();const auto keep=active->activities.back().activity_id;perform(root.get());assert(finished&&captured.status=="configuring"&&revoked==1&&forgotten==1&&names.empty());
    assert(control->configuration.activities.size()==1&&control->configuration.activities.front().activity_id==keep&&serialize_configuration(control->configuration).find(id<m::OctopusId>(9).str())==std::string::npos);
    removal_stage="complete";reset_result="scheduled";update_remove(captured);assert(captured.status=="complete"&&captured.reset=="scheduled");
    prepare();root=request();forget_fails=true;perform(root.get());assert(finished&&captured.status=="failed"&&revoked==1&&forgotten==1&&captured.error.find("incomplete")!=std::string::npos);
    prepare(false);root=request(false);perform(root.get());assert(finished&&captured.status=="complete"&&captured.reset=="not_needed"&&revoked==0&&forgotten==1);
    // A committed config deletion followed by a metadata write failure must
    // remain explicitly purgeable using the retained name alone after reboot.
    prepare(false);Configuration clean;std::vector<m::ActivityId> affected;std::string error;assert(propose_octopus_removal(*active,id<m::OctopusId>(9),clean,affected,error));
    assert(amity::activate_distributed_configuration(clean,active->revisions(),id<m::MessageId>(201))==ESP_OK);active=control->configuration;
    root=request(false);cJSON_ReplaceItemInObjectCaseSensitive(root.get(),"affected_activity_ids",cJSON_CreateArray());perform(root.get());assert(finished&&captured.status=="complete"&&names.empty());
    // Replace remains a separate action: preview is read-only while apply
    // revokes the isolated old device and preserves every mapped activity ID.
    prepare();auto expanded=*active;expanded.octopuses.push_back({id<m::OctopusId>(10),"Replacement","Replacement",false,1});
    assert(amity::activate_distributed_configuration(expanded,active->revisions(),id<m::MessageId>(202))==ESP_OK);active=control->configuration;
    fixture::peers.push_back(fixture::remote(10,20,1,1));const auto original_order=active->activity_order;
    root=replacement_request();perform_replace(root.get(),false);assert(!finished&&revoked==0&&forgotten==0&&!serialized.empty()&&control->configuration.activity_order==original_order);
    perform_replace(root.get(),true);assert(finished&&captured.status=="configuring"&&revoked==1&&forgotten==1&&control->configuration.activity_order==original_order);
    assert(control->configuration.activities.size()==2&&serialize_configuration(control->configuration).find(id<m::OctopusId>(9).str())==std::string::npos);
    removal_stage="complete";reset_result="offline";update_remove(captured);assert(captured.status=="applied"&&captured.reset=="offline");
    std::cout<<"PASS actual Remove API: exact reviewed activity set and revision/boot fences, no revoke before stage/reservation, marker failure never complete, reset outcome independent, all owner traces purged, saved-only/metadata-only retries\n";
    return 0;
}
}
int main(){return removal_fixture::run();}
'''
    json_dir = ROOT / 'managed_components/espressif__cjson/cJSON'
    if not (json_dir / 'cJSON.c').exists():
        json_dir = json_dir.parent
    with tempfile.TemporaryDirectory(prefix='removal-api-', dir=ROOT / 'test-results') as directory:
        temp = Path(directory)
        (temp / 'test.cpp').write_text(test)
        subprocess.run(['cc', '-g', '-Wno-deprecated-declarations', '-fsanitize=address,undefined', '-I'+str(json_dir), '-c', str(json_dir/'cJSON.c'), '-o', str(temp/'json.o')], check=True)
        includes = ['tests', 'tests/system_sync_fakes', 'tests/cec_hub_fakes', 'tests/reset_fakes',
                    'components/config_store/include', 'components/amity_system/include',
                    'components/system_sync/include', 'components/consortium/include']
        sources = ['components/amity_system/model.cpp', 'components/config_store/system_configuration.cpp',
                   'components/config_store/configuration_journal.cpp', 'components/config_store/configuration_replacement.cpp',
                   'components/consortium/json.cpp', 'components/consortium/wire.cpp',
                   'components/consortium/owned_intent.cpp', 'components/consortium/settings.cpp',
                   'components/consortium/update.cpp', 'components/consortium/transfer.cpp']
        openssl = ['-I/opt/homebrew/opt/openssl@3/include', '-L/opt/homebrew/opt/openssl@3/lib'] if Path('/opt/homebrew/opt/openssl@3').exists() else []
        subprocess.run(shlex.split(os.environ.get('CXX', 'c++')) + ['-std=c++17', '-Wall', '-Wextra', '-Werror', '-g', '-fsanitize=address,undefined', '-I'+str(json_dir)] + openssl + ['-I'+str(ROOT/path) for path in includes] + [str(temp/'test.cpp')] + [str(ROOT/path) for path in sources] + [str(temp/'json.o'), '-lcrypto', '-o', str(temp/'test')], check=True)
        subprocess.run([str(temp/'test')], check=True)


if __name__ == '__main__':
    main()
