#!/usr/bin/env python3
"""Execute admission parsing, routing and operation tracking without radio work."""
from pathlib import Path
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
    settings = (ROOT / 'components/device_settings/device_settings.cpp').read_text()
    helpers = function(source, 'void wipe(') + function(source, 'std::string string(') + function(source, 'bool unique_keys(') + source[source.index('bool object_fields('):source.index('bool parse_port_value(')]
    selection = source[source.index('struct AdmissionSelection {'):source.index('esp_err_t admission_response(')]
    addition = block(source[source.index('    Tracked operation;'):], '    if(path=="/api/membership/add")').replace('root.get()', 'root')
    update = block(source[source.index('void update_operation('):], '    if(operation.path=="/api/membership/add")')
    get_status = function(source, 'esp_err_t admission_response(')
    prefix = r'''
#include "amity/system/model.hpp"
#include "factory_credentials.hpp"
#include "json_input.hpp"
#include "cJSON.h"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <iostream>
#include <memory>
#include <vector>
namespace m=amity::system;
using esp_err_t=int;using httpd_req_t=int;
constexpr int ESP_OK=0,ESP_FAIL=1;
using Json=std::unique_ptr<cJSON,decltype(&cJSON_Delete)>;
struct SensitiveString {std::string& value;~SensitiveString();};
namespace device_settings {
const char* hostname(){return "amity-main";}
''' + function(settings, 'bool normalize_hostname(') + r'''
}
namespace amity::consortium {
struct RuntimeAdmissionCandidate {std::string token,label;};
struct RuntimeAdmissionStatus {m::MessageId scan_id;std::string phase="idle",error;bool requires_setup_code=false;std::vector<RuntimeAdmissionCandidate> candidates;m::OctopusId candidate_id;};
struct Peer {m::OctopusId id;bool enrolled=false,ready=false,provisional=false;std::string hostname;};
RuntimeAdmissionStatus current;std::vector<Peer> peers;unsigned connects=0;std::string sent_token,sent_name,sent_code;
RuntimeAdmissionStatus runtime_admission_status(){return current;}
std::vector<Peer> runtime_peers(){return peers;}
int runtime_add_octopus(m::MessageId scan,const std::string& token,const std::string& name,const std::string& code){assert(scan==current.scan_id);++connects;sent_token=token;sent_name=name;sent_code=code;return ESP_OK;}
}
std::string rejection,serialized;
int error(httpd_req_t*,const char* code,const std::string&,const char* ="400 Bad Request",bool=false){rejection=code;return ESP_FAIL;}
int send(httpd_req_t*,cJSON* value){char* data=cJSON_PrintUnformatted(value);serialized=data;cJSON_free(data);return ESP_OK;}
struct Tracked {std::string path="/api/membership/add",status="admitted",error; m::MessageId scan_id; m::OctopusId candidate;std::string name;};
''' + helpers + r'''
SensitiveString::~SensitiveString(){wipe(value);}
''' + selection + get_status + r'''
bool add(cJSON* root,Tracked& operation){
    const std::string path="/api/membership/add";httpd_req_t* req=nullptr;int result=0;bool done=false;
    auto finish=[&]{done=true;};
''' + addition + r'''
    return done||result==ESP_FAIL;
}
void update_add(Tracked& operation){if(operation.status!="admitted")return;
''' + update + r'''
}
template<class T>T id(unsigned n){char value[33];std::snprintf(value,sizeof(value),"%032x",n);return *T::parse(value);}
std::string request(std::string name="office",std::string extra=""){return "{\"request_id\":\""+id<m::MessageId>(1).str()+"\",\"scan_id\":\""+id<m::MessageId>(2).str()+"\",\"candidate_token\":\"opaque-public-token\",\"name\":\""+name+"\""+extra+"}";}
bool read(const std::string& raw,bool strict,AdmissionSelection& selection){Json root(web_admin::parse_json_input(raw),cJSON_Delete);std::string message;return root&&unique_keys(root.get())&&admission_selection(root.get(),strict,selection,message);}
void reject(const std::string& raw,bool strict=false){AdmissionSelection selection;assert(!read(raw,strict,selection));}
int main(){
    AdmissionSelection selection;assert(read(request("  AMITY-OFFICE  "),false,selection)&&selection.name=="amity-office"&&selection.candidate_token=="opaque-public-token"&&selection.setup_code.empty());
    assert(read(request("a"),false,selection));assert(read(request(std::string(32,'n')),false,selection));
    for(const auto* name:{"","bad name","-bad","bad-","amity.room","caf\xc3\xa9"})reject(request(name));
    reject(request(std::string(33,'n')));reject(request("office",",\"extra\":true"));reject(request("office",",\"name\":\"other\""));reject(request()+" trailing");
    reject(request("office",",\"setup_code\":true"));reject(request("office",",\"setup_code\":[]"));reject(request("office",",\"setup_code\":\"amityc1:retired\""));
    assert(read(request("office",",\"setup_code\":\"\""),false,selection));reject(request(),true);
    const auto printed=" 1234-567-8901 ";assert(read(request("office",std::string(",\"setup_code\":\"")+printed+"\""),true,selection)&&selection.setup_code==printed);
    assert(read(request("office",",\"setup_code\":\"MT:EXAMPLE-QR-PAYLOAD\""),true,selection));
    assert(read(request("office",",\"setup_code\":\""+std::string(256,'q')+"\""),true,selection));reject(request("office",",\"setup_code\":\""+std::string(257,'q')+"\""),true);
    for(const auto* field:{"scan_id","candidate_token","name"}){auto raw=request();Json root(cJSON_Parse(raw.c_str()),cJSON_Delete);cJSON_ReplaceItemInObjectCaseSensitive(root.get(),field,cJSON_CreateNumber(1));char* text=cJSON_PrintUnformatted(root.get());reject(text);cJSON_free(text);}
    for(const auto* field:{"scan_id","candidate_token","name"}){Json root(cJSON_Parse(request().c_str()),cJSON_Delete);cJSON_DeleteItemFromObjectCaseSensitive(root.get(),field);char* text=cJSON_PrintUnformatted(root.get());reject(text);cJSON_free(text);}
    for(const auto& token:{std::string{},std::string(129,'x'),std::string("bad")+char(1)+"token"}){Json root(cJSON_Parse(request().c_str()),cJSON_Delete);cJSON_ReplaceItemInObjectCaseSensitive(root.get(),"candidate_token",cJSON_CreateString(token.c_str()));char* text=cJSON_PrintUnformatted(root.get());reject(text);cJSON_free(text);}
    auto invalid=request();invalid.replace(invalid.find(id<m::MessageId>(2).str()),32,std::string(32,'0'));reject(invalid);
    using namespace amity::consortium;
    current.scan_id=id<m::MessageId>(2);current.phase="found";Tracked operation;Json document(cJSON_Parse(request().c_str()),cJSON_Delete);
    assert(add(document.get(),operation)&&connects==1&&sent_name=="office"&&sent_code.empty()&&operation.scan_id==current.scan_id&&!operation.candidate);
    current.scan_id=id<m::MessageId>(3);rejection.clear();assert(add(document.get(),operation)&&rejection=="stale_scan"&&connects==1);current.scan_id=id<m::MessageId>(2);
    Json own_name(cJSON_Parse(request("AMITY-MAIN").c_str()),cJSON_Delete);assert(add(own_name.get(),operation)&&rejection=="name_in_use"&&connects==1);
    peers={{id<m::OctopusId>(8),true,false,false,"office"}};assert(add(document.get(),operation)&&rejection=="name_in_use"&&connects==1);peers.clear();
    current.phase="connecting";update_add(operation);assert(operation.status=="admitted"&&!operation.candidate);
    current.candidate_id=id<m::OctopusId>(9);peers={{current.candidate_id,true,true,false,"office"}};current.phase="finishing";update_add(operation);assert(operation.status=="admitted");
    current.phase="complete";update_add(operation);assert(operation.status=="complete"&&operation.candidate==current.candidate_id);
    operation.status="admitted";peers[0].hostname="wrong";update_add(operation);assert(operation.status=="failed");peers[0].hostname="office";
    operation.status="admitted";current.scan_id=id<m::MessageId>(3);update_add(operation);assert(operation.status=="complete");
    operation.status="admitted";operation.candidate={};update_add(operation);assert(operation.status=="unknown"&&operation.error.find("Check the Amity devices list")!=std::string::npos);current.scan_id=id<m::MessageId>(2);
    operation.status="admitted";current.phase="failed";current.error="Could not connect";update_add(operation);assert(operation.status=="failed"&&operation.error==current.error);
    operation.status="admitted";current.phase="cancelled";update_add(operation);assert(operation.status=="cancelled");
    current.requires_setup_code=true;current.candidates={{"token","Amity <nearby>"}};assert(admission_response(nullptr)==ESP_OK);Json status(cJSON_Parse(serialized.c_str()),cJSON_Delete);assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(status.get(),"requires_setup_code")));auto* candidates=cJSON_GetObjectItemCaseSensitive(status.get(),"candidates");assert(cJSON_GetArraySize(candidates)==1&&string(cJSON_GetArrayItem(candidates,0),"label")=="Amity <nearby>");
    std::cout<<"PASS admission API: closed typed scan/token/name/code contract, normal/strict profiles, no connect before validation, stale scans, exact final peer/name and safe status JSON\n";
}
'''
    json_dir = ROOT / 'managed_components/espressif__cjson/cJSON'
    if not (json_dir / 'cJSON.c').exists():
        json_dir = json_dir.parent
    with tempfile.TemporaryDirectory(prefix='membership-api-', dir=ROOT / 'test-results') as directory:
        temp = Path(directory)
        (temp / 'test.cpp').write_text(prefix)
        subprocess.run(['cc', '-g', '-Wno-deprecated-declarations', '-fsanitize=address,undefined', '-I'+str(json_dir), '-c', str(json_dir/'cJSON.c'), '-o', str(temp/'json.o')], check=True)
        subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-g', '-fsanitize=address,undefined', '-I'+str(json_dir), '-I'+str(ROOT/'components/amity_system/include'), '-I'+str(ROOT/'components/device_settings/include'), '-I'+str(ROOT/'components/web_admin/include'), str(temp/'test.cpp'), str(ROOT/'components/amity_system/model.cpp'), str(temp/'json.o'), '-o', str(temp/'test')], check=True)
        subprocess.run([str(temp/'test')], check=True)

if __name__ == '__main__':
    main()
