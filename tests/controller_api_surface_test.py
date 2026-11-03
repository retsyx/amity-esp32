#!/usr/bin/env python3
"""Execute real administration routing and advertised membership capacity."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    source = (ROOT / 'components/web_admin/system_admin.cpp').read_text()
    web_source = (ROOT / 'components/web_admin/web_admin.cpp').read_text()
    assert 'path=="/api/front-physical-address"' not in web_source
    start = source.index('std::optional<std::string> device_settings_target(')
    router = source[start:source.index('void publish_inputs()', start)]
    capacity = next(line for line in source.splitlines()
                    if 'cJSON_AddNumberToObject(capabilities,"supported_octopuses",' in line)
    # Unknown routes are rejected before reading a request body or admitting work.
    rejection = source.index('const auto fields=request_fields(path);if(!fields)')
    assert rejection < source.index('std::string data;if(!web::body(req,data))', rejection)
    get_start = source.index('if(req->method==HTTP_GET)', source.index('bool admin_request('))
    post_start = source.index('if(req->method!=HTTP_POST)', get_start)
    assert 'if(path=="/api/updates/cohort"){result=web::json(req,system_services::updates::cohort_status_json());return true;}' in source[get_start:post_start]
    firmware = web_source.index('if(path=="/api/firmware")return system_services::upload_firmware(req);')
    assert web_source.index('if(!web::authenticated(req))') < firmware
    assert web_source.index('if(req->method==HTTP_POST)', web_source.index('if(!web::authenticated(req))')) < firmware
    program = r'''
#include <algorithm>
#include <cassert>
#include <iostream>
#include <optional>
#include <string>
#include <vector>
#include "amity/consortium/limits.hpp"
#include "cJSON.h"
''' + router + r'''
int main(){
    for(const auto* path:{"membership/create","membership/move-coordinator",
        "membership/input-role","remotes/manage","integrations/primary-zone",
        "setup-draft","setup/connection","setup/propose-activity",
        "input-context",
        "scans","zones/0123456789abcdef0123456789abcdef/activity",
        "zones/0123456789abcdef0123456789abcdef/key",
        "zones/0123456789abcdef0123456789abcdef/standby","settings/remotes",
        "updates/validate","updates/cohort","updates/chunk","updates/install","updates/cancel"}){
        assert(!request_fields(std::string("/api/")+path));
    }
    for(const auto* path:{"config","config/validate","replacements/preview",
        "replacements/apply","membership/scan","membership/cancel","membership/add","membership/remove"}){
        const auto fields=request_fields(std::string("/api/")+path);
        assert(fields&&std::count(fields->begin(),fields->end(),"request_id")==1);
        for(const auto& field:*fields)assert(field!="remote_id"&&field!="allow"&&field!="primary_remote_id");
    }
    const auto replacement=request_fields("/api/replacements/apply");
    assert(std::find(replacement->begin(),replacement->end(),"remote_pairing_acknowledged")==replacement->end());
    const auto addition=request_fields("/api/membership/add");
    assert(std::find(addition->begin(),addition->end(),"name")!=addition->end());
    assert(addition->size()==5&&std::find(addition->begin(),addition->end(),"scan_id")!=addition->end()&&std::find(addition->begin(),addition->end(),"candidate_token")!=addition->end());
    assert(request_fields("/api/membership/scan")->size()==1&&request_fields("/api/membership/cancel")->size()==2);
    assert(std::find(addition->begin(),addition->end(),"location")==addition->end());
    const auto settings=request_fields("/api/devices/0123456789abcdef0123456789abcdef/settings");
    assert(settings&&settings->size()==6&&std::find(settings->begin(),settings->end(),"hostname")!=settings->end()&&std::find(settings->begin(),settings->end(),"expected_hostname_revision")!=settings->end());
    assert(std::find(settings->begin(),settings->end(),"confirm_restart")==settings->end());
    for(const auto* path:{"/api/devices/00000000000000000000000000000000/settings",
        "/api/devices/0123456789ABCDEF0123456789abcdef/settings",
        "/api/devices/0123456789abcdef0123456789abcdef/settings/extra",
        "/api/devices/0123456789abcdef/settings"})assert(!request_fields(path));
    assert(!request_fields("/api/unknown"));
    for(const auto* path:{"/api/key","/api/status","/api/setup","/api/login","/api/logout",
        "/api/password","/api/hostname","/api/reboot","/api/factory-reset","/api/firmware",
        "/api/remotes","/api/remotes/pair","/api/matter","/api/matter/open","/api/homekit"})
        assert(!administrative_path(path));
    for(const auto* path:{"/api/system","/api/config","/api/backup","/api/config/validate",
        "/api/membership/admission","/api/operations/0123456789abcdef0123456789abcdef",
        "/api/devices/0123456789abcdef0123456789abcdef/settings","/api/updates","/api/updates/cohort"})
        assert(administrative_path(path));
    auto* body=cJSON_CreateObject();auto* capabilities=cJSON_AddObjectToObject(body,"capabilities");
''' + capacity + r'''
    const auto* advertised=cJSON_GetObjectItemCaseSensitive(capabilities,"supported_octopuses");
    assert(cJSON_IsNumber(advertised)&&advertised->valuedouble==4
        &&advertised->valuedouble==amity::consortium::maximum_roster_peers+1);
    cJSON_Delete(body);
    std::cout<<"PASS actual administration router: shared authenticated firmware upload, read-only update status, retired mutations rejected before admission and four total Amity devices\n";
}
'''
    json_dir = ROOT / 'managed_components/espressif__cjson/cJSON'
    if not (json_dir / 'cJSON.c').exists():
        json_dir = json_dir.parent
    with tempfile.TemporaryDirectory(dir=ROOT / 'test-results') as directory:
        path = Path(directory)
        (path / 'test.cpp').write_text(program)
        subprocess.run(['cc', '-g', '-Wno-deprecated-declarations', '-fsanitize=address,undefined',
                        '-I'+str(json_dir), '-c', str(json_dir/'cJSON.c'), '-o', str(path/'json.o')], check=True)
        subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-g',
                        '-fsanitize=address,undefined', '-I'+str(json_dir),
                        '-I'+str(ROOT/'components/consortium/include'), str(path / 'test.cpp'), str(path/'json.o'),
                        '-o', str(path / 'test')], check=True)
        subprocess.run([str(path / 'test')], check=True)


if __name__ == '__main__':
    main()
