#!/usr/bin/env python3
"""Exercise the structured v2 rejection response before any body is consumed."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

from consortium_gatt_callback_test import function

ROOT = Path(__file__).resolve().parents[1]


def main():
    source = (ROOT / 'components/web_admin/v2_admin.cpp').read_text()
    actual = function(source, 'std::string json(cJSON* root)') + function(
        source, 'esp_err_t error(httpd_req_t* req,')
    fixture = r'''
#include "cJSON.h"
#include <cassert>
#include <cstring>
#include <memory>
#include <string>
using esp_err_t=int;
constexpr int ESP_OK=0,ESP_FAIL=-1;
using Json=std::unique_ptr<cJSON,decltype(&cJSON_Delete)>;
struct httpd_req_t {size_t content_len=16000000;unsigned reads=0;bool close=false;std::string response,status;};
int httpd_resp_set_hdr(httpd_req_t* req,const char* key,const char* value){
 assert(!strcmp(key,"Connection")&&!strcmp(value,"close"));req->close=true;return ESP_OK;
}
namespace web {
int json(httpd_req_t* req,const std::string& body,const char* status){req->response=body;req->status=status;return ESP_OK;}
}
''' + actual + r'''
int main(){
 httpd_req_t req;
 const std::string message="A quoted \"device\"\nneeds review";
 assert(error(&req,"coordinator_required",message,"403 Forbidden",true)==ESP_FAIL);
 assert(req.close&&req.status=="403 Forbidden"&&req.reads==0&&req.content_len==16000000);
 Json body(cJSON_Parse(req.response.c_str()),cJSON_Delete);assert(body);
 assert(!strcmp(cJSON_GetObjectItemCaseSensitive(body.get(),"code")->valuestring,"coordinator_required"));
 for(const char* field:{"message","error"})assert(message==cJSON_GetObjectItemCaseSensitive(body.get(),field)->valuestring);
 assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(body.get(),"retryable")));
 const auto* affected=cJSON_GetObjectItemCaseSensitive(body.get(),"affected_ids");
 assert(cJSON_IsArray(affected)&&cJSON_GetArraySize(affected)==0);
 req={};assert(error(&req,"not_found","Unknown operation.","404 Not Found")==ESP_FAIL);
 assert(req.close&&req.status=="404 Not Found"&&req.reads==0);
}
'''
    cjson = ROOT / 'managed_components/espressif__cjson/cJSON'
    if not (cjson / 'cJSON.c').exists():
        cjson = cjson.parent
    with tempfile.TemporaryDirectory(prefix='amity-admin-rejection-') as directory:
        output = Path(directory)
        (output / 'test.cpp').write_text(fixture)
        subprocess.run(['cc', '-g', '-Wno-deprecated-declarations', '-fsanitize=address,undefined', '-I' + str(cjson),
                        '-c', str(cjson / 'cJSON.c'), '-o', str(output / 'cjson.o')], check=True)
        subprocess.run(shlex.split(os.environ.get('CXX', 'c++')) + [
            '-std=c++17', '-Wall', '-Wextra', '-Werror', '-g', '-fsanitize=address,undefined',
            '-I' + str(cjson), str(output / 'test.cpp'), str(output / 'cjson.o'),
            '-o', str(output / 'test')], check=True)
        subprocess.run([str(output / 'test')], check=True)
    print('PASS structured v2 rejection closes unread requests and preserves escaped error fields')


if __name__ == '__main__':
    main()
