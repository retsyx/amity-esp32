#!/usr/bin/env python3
"""Run the actual Add search admission and its HTTP refusal on a full system."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile
import admission_workflow_test as workflow
from consortium_gatt_callback_test import function
from membership_admission_api_test import block
from setup_timing_support import timing_support

ROOT = Path(__file__).resolve().parents[1]
SUITE = r'''
}
#include "cJSON.h"
struct httpd_req_t {};
namespace web {std::string body,status;esp_err_t json(httpd_req_t*,const std::string& value,const char* code="200 OK"){body=value;status=code;return ESP_OK;}}
void httpd_resp_set_hdr(httpd_req_t*,const char*,const char*){}
namespace m=amity::system;
using Json=std::unique_ptr<cJSON,decltype(&cJSON_Delete)>;
'''
CHECKS = r'''
struct Response {bool started=false;std::string status,code,message;bool retryable=false;};
Response scan(unsigned request){
    const auto id=amity::consortium::ident<m::MessageId>(request).str();
    const std::string path="/api/membership/scan";httpd_req_t request_state;httpd_req_t* req=&request_state;
    Response out;esp_err_t result=ESP_OK;auto finish=[&]{out.started=true;};web::body.clear();web::status.clear();
    [&]{
'''
TAIL = r'''
    }();
    (void)result;
    if(out.started)return out;
    Json root(cJSON_Parse(web::body.c_str()),cJSON_Delete);assert(root);
    out.status=web::status;out.code=string(root.get(),"code");out.message=string(root.get(),"error");
    out.retryable=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root.get(),"retryable"));
    return out;
}
int main(){
    using namespace amity::consortium;
    auto added=[](unsigned count,bool active){for(unsigned n=0;n<count;++n)group.peers.push_back({ident<model::OctopusId>(10+n),"existing-leaf",std::string(64,char('e'+n)),active});};
    reset();added(3,true); // The main and three added Amities: the most supported.
    for(unsigned attempt=1;attempt<=3;++attempt){
        const auto refused=scan(attempt);
        assert(!refused.started&&!invitation_running&&reservations==0&&refused.status=="409 Conflict");
        assert(refused.code=="capacity_reached"&&!refused.retryable);
        assert(refused.message=="Up to "+std::to_string(maximum_roster_peers+1)+" Amity devices are supported. Remove one before adding another.");
    }
    reset();invitation_running=true; // Another search is running: still a temporary refusal.
    {const auto busy=scan(10);assert(!busy.started&&busy.code=="scan_unavailable"&&busy.retryable);}
    reset();added(2,true);added(1,false);group.join_id=ident<model::JoinId>(5);group.joining_id=ident<model::OctopusId>(10); // Setup is still finishing.
    {const auto finishing=scan(11);assert(!finishing.started&&finishing.code=="scan_unavailable"&&finishing.retryable);}
    reset();added(2,true);
    {const auto started=scan(12);assert(started.started&&invitation_running);}
    std::cout<<"PASS actual Add search on a full system explains the four-device limit without asking for a retry; temporary refusals still ask for one\n";
    return 0;
}
'''


def main():
    runtime = (ROOT / 'components/consortium/runtime.cpp').read_text()
    admin = (ROOT / 'components/web_admin/system_admin.cpp').read_text()
    names = ['std::string proposal_json(', 'void admission_failed(', 'void invite_worker_work(', 'RuntimeAdmissionStatus runtime_admission_status(',
             'esp_err_t runtime_scan_admission(', 'esp_err_t runtime_cancel_admission(', 'esp_err_t runtime_add_octopus(']
    actual = function((ROOT / 'components/consortium/settings_runtime.inc').read_text(), 'bool settings_hostname_pending(') + ''.join(function(runtime, name) for name in names)
    reset = workflow.SUITE[workflow.SUITE.index('void reset(){'):workflow.SUITE.index('void run(){')]
    helpers = function(admin, 'std::string string(') + function(admin, 'std::string json(cJSON* root)') + function(admin, 'esp_err_t error(httpd_req_t* req,')
    handler = block(admin[admin.index('    Tracked operation;'):], '    if(path=="/api/membership/scan")')
    json_dir = ROOT / 'managed_components/espressif__cjson/cJSON'
    if not (json_dir / 'cJSON.c').exists():
        json_dir = json_dir.parent
    with tempfile.TemporaryDirectory(prefix='amity-membership-capacity-') as directory:
        temp = Path(directory)
        (temp / 'test.cpp').write_text(timing_support() + workflow.PREFIX + actual + reset + SUITE + helpers + CHECKS + handler.replace('return true;', 'return;') + TAIL)
        include = ['tests/reset_fakes', 'components/consortium', 'components/consortium/include',
                   'components/amity_system/include', 'components/device_settings/include']
        sources = sorted((ROOT / 'components/consortium').glob('*.cpp')) + [ROOT / 'components/amity_system/model.cpp']
        subprocess.run(['cc', '-g', '-Wno-deprecated-declarations', '-fsanitize=address,undefined', '-I' + str(json_dir),
                        '-c', str(json_dir / 'cJSON.c'), '-o', str(temp / 'json.o')], check=True)
        subprocess.run(shlex.split(os.environ.get('CXX', 'c++')) +
                       ['-std=c++17', '-pthread', '-Wall', '-Wextra', '-Werror', '-g', '-fsanitize=address,undefined', '-I' + str(json_dir)] +
                       ['-I' + str(ROOT / path) for path in include] +
                       [str(temp / 'test.cpp')] + [str(path) for path in sources] + [str(temp / 'json.o'), '-o', str(temp / 'test')], check=True)
        return subprocess.run([str(temp / 'test')], check=False).returncode


if __name__ == '__main__':
    raise SystemExit(main())
