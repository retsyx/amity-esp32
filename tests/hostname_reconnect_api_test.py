#!/usr/bin/env python3
"""Exercise actual reconnect handlers with real JSON and fake HTTP/settings IO."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

from consortium_gatt_callback_test import function

ROOT = Path(__file__).resolve().parents[1]

PREFIX = r'''
#include "cJSON.h"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <string>
using esp_err_t=int;
constexpr int ESP_OK=0,ESP_FAIL=-1,HTTP_GET=0,HTTP_POST=1,HTTP_OPTIONS=2;
using Json=std::unique_ptr<cJSON,decltype(&cJSON_Delete)>;
struct httpd_req_t {int method=HTTP_GET;size_t content_len=0;const char* uri="/api/ready";std::string response,status,type;std::map<std::string,std::string> headers;bool authenticated=false;};
std::atomic_bool ui_enabled{true};
unsigned auth_checks=0,downstream=0,saves=0,reboots=0;
bool firmware_ready=false,save_failed=false;
std::string saved_hostname;
struct Id {std::string str()const{return "0123456789abcdef0123456789abcdef";}};
struct Control {Id octopus_id()const{return {};}} control;
Control* hub=&control;
namespace system_services {
const char* boot_id(){return "11111111111111111111111111111111";}
bool firmware_ready(){return ::firmware_ready;}
void reboot_later(){++reboots;}
}
namespace device_settings {
const char* hostname(){return "current-name";}
bool normalize_hostname(std::string& value){return value=="new-name";}
int save_hostname(const std::string& value){++saves;if(save_failed)return ESP_FAIL;saved_hostname=value;return ESP_OK;}
}
int httpd_resp_set_status(httpd_req_t* req,const char* value){req->status=value;return ESP_OK;}
int httpd_resp_set_type(httpd_req_t* req,const char* value){req->type=value;return ESP_OK;}
int httpd_resp_set_hdr(httpd_req_t* req,const char* name,const char* value){req->headers[name]=value;return ESP_OK;}
int httpd_resp_send(httpd_req_t* req,const char* data,size_t size){req->response.assign(data,size);return ESP_OK;}
int httpd_resp_send_chunk(httpd_req_t* req,const char* data,size_t size){if(size)req->response.append(data,size);return ESP_OK;}
namespace web {
int response(httpd_req_t*,const char*,size_t);
int json(httpd_req_t*,const std::string&,const char* status="200 OK");
int error(httpd_req_t* req,const char*,const char* status="400 Bad Request"){req->status=status;return ESP_FAIL;}
bool authenticated(httpd_req_t* req){++auth_checks;return req->authenticated;}
}
const uint8_t asset_bytes[]={'x'};
const uint8_t *html_start=asset_bytes,*html_end=asset_bytes+1,*js_start=asset_bytes,*js_end=asset_bytes+1,*css_start=asset_bytes,*css_end=asset_bytes+1,*system_js_start=asset_bytes,*system_js_end=asset_bytes+1;
'''

SUITE = r'''
std::string text(cJSON* json,const char* key){auto* field=cJSON_GetObjectItemCaseSensitive(json,key);assert(cJSON_IsString(field));return field->valuestring;}
void no_cors(const httpd_req_t& req){assert(!req.headers.count("Access-Control-Allow-Origin")&&!req.headers.count("Access-Control-Allow-Credentials")&&!req.headers.count("Access-Control-Allow-Methods"));}
int main(){
 for(bool ready:{false,true}){
  firmware_ready=ready;httpd_req_t req;const auto previous_auth=auth_checks;
  assert(api(&req)==ESP_OK&&auth_checks==previous_auth&&req.status=="200 OK"&&req.type=="application/json");
  assert(req.headers.at("Access-Control-Allow-Origin")=="*"&&req.headers.at("Cache-Control")=="no-store"&&!req.headers.count("Access-Control-Allow-Credentials"));
  Json body(cJSON_Parse(req.response.c_str()),cJSON_Delete);assert(body&&cJSON_GetArraySize(body.get())==4);
  assert(text(body.get(),"device_id")==control.octopus_id().str()&&text(body.get(),"boot_id")==system_services::boot_id()&&text(body.get(),"hostname")==device_settings::hostname());
  assert(bool(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(body.get(),"ready")))==ready);
 }
 for(int method:{HTTP_POST,HTTP_OPTIONS}){httpd_req_t req;req.method=method;assert(api(&req)==ESP_FAIL&&req.status=="401 Unauthorized");no_cors(req);}
 for(const char* path:{"/api/config","/api/status","/api/ready?cache=1"}){httpd_req_t req;req.uri=path;assert(api(&req)==ESP_FAIL&&req.status=="401 Unauthorized");no_cors(req);}
 {httpd_req_t req;req.content_len=1;assert(api(&req)==ESP_FAIL&&req.status=="400 Bad Request");no_cors(req);}
 {httpd_req_t req;ui_enabled=false;assert(api(&req)==ESP_FAIL&&req.status=="403 Forbidden");no_cors(req);ui_enabled=true;}
 assert(downstream==0);
 std::cout<<"PASS actual readiness router exposes only GET health with no credentials or mutation CORS\n";
 {httpd_req_t req;assert(hostname_save(&req,cJSON_Parse("{\"hostname\":\"new-name\"}"))==ESP_OK);no_cors(req);
  Json body(cJSON_Parse(req.response.c_str()),cJSON_Delete);assert(body&&cJSON_GetArraySize(body.get())==4);
  assert(text(body.get(),"hostname")=="new-name"&&text(body.get(),"device_id")==control.octopus_id().str()&&text(body.get(),"previous_boot_id")==system_services::boot_id());
  assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(body.get(),"rebooting"))&&saves==1&&reboots==1&&saved_hostname=="new-name");
 }
 {httpd_req_t req;save_failed=true;assert(hostname_save(&req,cJSON_Parse("{\"hostname\":\"new-name\"}"))==ESP_FAIL&&req.status=="500 Internal Server Error"&&reboots==1);no_cors(req);save_failed=false;}
 {httpd_req_t req;assert(hostname_save(&req,cJSON_Parse("{\"hostname\":\"https://other.example\"}"))==ESP_FAIL&&saves==2&&reboots==1);no_cors(req);}
 std::cout<<"PASS actual hostname save returns exact previous boot/device only after successful persistence\n";
 {httpd_req_t req;req.uri="/";assert(asset(&req)==ESP_OK);const auto& csp=req.headers.at("Content-Security-Policy");
  assert(csp.find("connect-src 'self' https://*.local;")!=std::string::npos&&csp.find("script-src 'self';")!=std::string::npos&&csp.find("frame-ancestors 'none'")!=std::string::npos);no_cors(req);
 }
 std::cout<<"PASS actual asset CSP allows HTTPS local probes while retaining script/frame restrictions\n";
}
'''


def main():
    source = (ROOT / 'components/web_admin/web_admin.cpp').read_text()
    api = function(source, 'esp_err_t api(httpd_req_t *req)')
    # Execute the unchanged public prefix and original authentication guard;
    # unrelated setup/login/authenticated handlers become a downstream sentinel.
    public = api[:api.index('    if(path=="/api/setup") {')]
    guard = next(line for line in api.splitlines() if 'if(!web::authenticated(req))' in line)
    public += guard + '\n++downstream;return ESP_OK;\n}\n'
    branch = function(source, 'else if(path=="/api/hostname")')
    save = 'esp_err_t hostname_save(httpd_req_t* req,cJSON* parsed){Json root(parsed,cJSON_Delete);esp_err_t e=ESP_OK;\n' + branch[branch.index('{') + 1:]
    request = (ROOT / 'components/web/request.cpp').read_text()
    actual = ('namespace web {\n' + function(request, 'esp_err_t response(httpd_req_t *req,') + function(request, 'esp_err_t json(httpd_req_t *req,') + '\n}\n'
              + function(source, 'esp_err_t send(httpd_req_t *req,cJSON *root)')
              + public + save + function(source, 'esp_err_t asset(httpd_req_t *req)'))
    cjson = ROOT / 'managed_components/espressif__cjson/cJSON'
    if not (cjson / 'cJSON.c').exists():
        cjson = cjson.parent
    with tempfile.TemporaryDirectory(prefix='amity-hostname-api-') as temporary:
        base = Path(temporary)
        (base / 'test.cpp').write_text(PREFIX + actual + SUITE)
        subprocess.run(['cc', '-g', '-Wno-deprecated-declarations', '-fsanitize=address,undefined', '-I' + str(cjson), '-c', str(cjson / 'cJSON.c'), '-o', str(base / 'cjson.o')], check=True)
        subprocess.run(shlex.split(os.environ.get('CXX', 'c++')) + [
            '-std=c++17', '-Wall', '-Wextra', '-Werror', '-g', '-fsanitize=address,undefined',
            '-I' + str(cjson), str(base / 'test.cpp'), str(base / 'cjson.o'), '-o', str(base / 'test')], check=True)
        subprocess.run([str(base / 'test')], check=True)


if __name__ == '__main__':
    main()
