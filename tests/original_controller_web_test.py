#!/usr/bin/env python3
"""Run actual administration start/stop and existing-handler admission guards."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile
from consortium_gatt_callback_test import function

ROOT=Path(__file__).resolve().parents[1]
PREFIX=r'''
#include <atomic>
#include <cassert>
#include <iostream>
#include <mutex>
#include <string>
using esp_err_t=int;using httpd_handle_t=void*;
constexpr int ESP_OK=0,ESP_ERR_INVALID_STATE=1,HTTP_GET=0,HTTP_POST=1,HTTPD_403_FORBIDDEN=403;
struct httpd_req_t{};
namespace amity{struct SystemControl{};namespace consortium{
struct RuntimeStatus{bool member=false,coordinator=true,authority_ready=true;std::string phase;};RuntimeStatus role;
RuntimeStatus runtime_status(){return role;}
}}
unsigned starts=0,stops=0,routes=0,effects=0;int stop_error=0;
std::atomic_bool ui_enabled{false},ui_revoked{false};std::mutex server_mutex;
amity::SystemControl*hub=nullptr;httpd_handle_t server=nullptr;
namespace web{
int start(httpd_handle_t&out,const std::string&,const char*){++starts;out=reinterpret_cast<void*>(1);return ESP_OK;}
int route(httpd_handle_t,const char*,int,int(*)(httpd_req_t*)){++routes;return ESP_OK;}
int error(httpd_req_t*,const char*,const char*){return 403;}
}
int httpd_resp_send_err(httpd_req_t*,int status,const char*){return status;}
int httpd_ssl_stop(httpd_handle_t){++stops;assert(ui_revoked&&!ui_enabled);return stop_error;}
'''
SUITE=r'''
int main(int argc,char**argv){assert(argc==2);const std::string mode=argv[1];amity::SystemControl control;httpd_req_t request;
 if(mode=="member"||mode=="prepared")amity::consortium::role.member=true;
 else if(mode=="retired"||mode=="recovery_required")amity::consortium::role.phase=mode;
 else if(mode=="awaiting_setup"){amity::consortium::role.coordinator=amity::consortium::role.authority_ready=false;amity::consortium::role.phase=mode;}
 else if(mode=="unclaimed"){amity::consortium::role.coordinator=false;amity::consortium::role.authority_ready=false;}
 if(mode=="member"||mode=="prepared"||mode=="retired"||mode=="recovery_required"||mode=="unclaimed"||mode=="awaiting_setup"){
  assert(start(control,"test","private")==ESP_ERR_INVALID_STATE&&starts==0&&!server&&!ui_enabled);
  assert(api(&request)==403&&asset(&request)==403&&effects==0);
 }else if(mode=="stop-before-start"){
  assert(stop()==ESP_OK&&starts==0);assert(start(control,"test","private")==ESP_ERR_INVALID_STATE&&starts==0);
 }else{
  assert(start(control,"test","private")==ESP_OK&&starts==1&&routes==6&&server&&ui_enabled);
  assert(api(&request)==ESP_OK&&asset(&request)==ESP_OK&&effects==2);
  if(mode=="stop-error")stop_error=77;
  assert(stop()==stop_error&&!ui_enabled&&ui_revoked);
  assert(api(&request)==403&&asset(&request)==403&&effects==2); // Already accepted handlers must reject too.
  assert(start(control,"test","private")==ESP_ERR_INVALID_STATE&&starts==1);
  stop_error=0;assert(stop()==ESP_OK&&!server);assert(stop()==ESP_OK);
 }
 std::cout<<"PASS actual administration lifecycle "<<mode<<'\n';
}
'''

def main():
    source=(ROOT/'components/web_admin/web_admin.cpp').read_text()
    # Execute each actual first-statement guard unchanged, replacing only the
    # large unrelated handler body with an observable fake downstream effect.
    guards=[]
    for name in ('api','asset'):
        body=function(source,'esp_err_t '+name+'(httpd_req_t *req)')
        guard=next(line.strip() for line in body.splitlines() if 'if (!ui_enabled.load())' in line)
        guards.append('int '+name+'(httpd_req_t *req){'+guard+'++effects;return ESP_OK;}\n')
    actual=function(source,'esp_err_t start(amity::SystemControl &service,')+function(source,'esp_err_t stop()')
    with tempfile.TemporaryDirectory(prefix='amity-controller-web-') as directory:
        cpp=Path(directory)/'test.cpp';binary=Path(directory)/'test';cpp.write_text(PREFIX+''.join(guards)+actual+SUITE)
        subprocess.run(shlex.split(os.environ.get('CXX','c++'))+['-std=c++17','-Wall','-Wextra','-Werror','-g','-fsanitize=address,undefined',str(cpp),'-o',str(binary)],check=True)
        for mode in ('member','prepared','retired','recovery_required','unclaimed','awaiting_setup','stop-before-start','live-stop','stop-error'):
            subprocess.run([str(binary),mode],check=True)

if __name__=='__main__':main()
