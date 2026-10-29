#!/usr/bin/env python3
"""Compile actual GATT callbacks and candidate connection against a fake controller.

Only the controller, advertisement parser, and FreeRTOS notification seams are
faked. The completion, generation checks, GAP callback, and disconnect function
are extracted unchanged from the firmware source. No Bluetooth hardware is used.
--source permits running the same assertions against an earlier source snapshot.
"""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


PREFIX = r'''
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
constexpr int BLE_GAP_EVENT_DISC=1,BLE_GAP_EVENT_DISC_COMPLETE=2,BLE_GAP_EVENT_CONNECT=3,BLE_GAP_EVENT_DISCONNECT=4,BLE_GAP_EVENT_MTU=15;
constexpr int BLE_HS_ENOTCONN=12,BLE_HS_EDONE=14,BLE_HS_EINVAL=3,BLE_HS_EALREADY=2;
constexpr uint16_t BLE_L2CAP_CID_ATT=4,BLE_ATT_MTU_DFLT=23;
using esp_err_t=int;constexpr int ESP_OK=0,ESP_FAIL=-1,ESP_ERR_INVALID_STATE=1,ESP_ERR_INVALID_ARG=2;
constexpr uint16_t BLE_HS_CONN_HANDLE_NONE=0xffff;
constexpr int BLE_ERR_REM_USER_CONN_TERM=19;
struct ble_addr_t{uint8_t type=0,val[6]{};};
struct ble_uuid_t{uint8_t bytes[16]{};};struct ble_uuid128_t{ble_uuid_t u;};
const ble_uuid128_t service_uuid{{{1,2,3}}},record_uuid{{{1,2,4}}};ble_uuid128_t parsed_uuid;
int ble_uuid_cmp(const ble_uuid_t* a,const ble_uuid_t* b){return std::memcmp(a,b,16);}
struct ble_gap_event{
 int type=0;struct{const uint8_t* data=nullptr;uint8_t length_data=0;ble_addr_t addr{};}disc;
 struct{int status=0;uint16_t conn_handle=0;}connect;struct{struct{uint16_t conn_handle=0;}conn;}disconnect;
 struct{uint16_t conn_handle=0,channel_id=0,value=0;}mtu;
};
struct ble_hs_adv_fields{const uint8_t* name=nullptr;uint8_t name_len=0,num_uuids128=0;ble_uuid128_t* uuids128=nullptr;};
int ble_hs_adv_parse_fields(ble_hs_adv_fields* fields,const uint8_t* data,uint8_t length){if(length<16)return -1;std::memcpy(parsed_uuid.u.bytes,data,16);fields->uuids128=&parsed_uuid;fields->num_uuids128=1;fields->name=data+16;fields->name_len=length-16;return 0;}
struct RuntimeAdmissionCandidate{std::string token,label;};struct Discovered{RuntimeAdmissionCandidate public_value;ble_addr_t address;};
std::vector<Discovered> discovered;std::mutex client_mutex;
uint64_t client_generation=7;uint32_t client_callback_cookie=7;
uint16_t client_connection=BLE_HS_CONN_HANDLE_NONE,cancelled_client=BLE_HS_CONN_HANDLE_NONE,client_service_start=0,client_service_end=0,client_attribute=0;
uint32_t client_record=0;
ble_addr_t candidate_address{};bool client_workflow=true,client_discovery_waiting=true,client_mtu_ready=false,client_mtu_waiting=false;std::atomic_bool client_stopping{false};std::atomic_int client_error{0};void* client_done=nullptr;
int wakes=0,cancellations=0,connection_cancellations=0,terminations=0;uint16_t terminated_handle=BLE_HS_CONN_HANDLE_NONE;
unsigned pending_notifications=0;std::function<void()>during_wait;std::vector<unsigned>bounded_waits;
constexpr int pdTRUE=1;unsigned pdMS_TO_TICKS(unsigned ms){return ms;}
std::string random_hex(size_t length){static unsigned n=0;auto text=std::to_string(++n);return std::string(length*2-text.size(),'0')+text;}
void xSemaphoreGive(void*){++wakes;pending_notifications=1;}
int xSemaphoreTake(void*,unsigned ticks){if(ticks){bounded_waits.push_back(ticks);if(!pending_notifications&&during_wait){auto callback=std::move(during_wait);during_wait={};callback();}}if(pending_notifications){pending_notifications=0;return pdTRUE;}return 0;}
int ble_gap_disc_cancel(){++cancellations;return 0;}int ble_gap_conn_cancel(){++connection_cancellations;return 0;}
int ble_gap_terminate(uint16_t handle,int){++terminations;terminated_handle=handle;return 0;}
std::mutex state_mutex;uint64_t generation=9,deadline=1000,server_session=0;
uint32_t server_callback_cookie=9,response_record=0;uint8_t response_status=0;
uint16_t server_connection=BLE_HS_CONN_HANDLE_NONE;
bool server_open=true,server_sealed=false,advertising=true;
std::atomic_uint failed_proofs{0};std::string srv_response;
int advert_starts=0,advert_stops=0,closed_work=0;
constexpr int BLE_HS_EAPP=22,BLE_GAP_CONN_MODE_UND=1,BLE_GAP_DISC_MODE_GEN=2,BLE_OWN_ADDR_PUBLIC=0;
struct ble_gap_adv_params{int conn_mode=0,disc_mode=0;};struct Work{bool close=false;};
bool post_work(const Work& work){if(work.close)++closed_work;return true;}
uint64_t now_ms(){return 100;}
int ble_gap_adv_start(int,void*,int32_t,const ble_gap_adv_params*,int(*)(ble_gap_event*,void*),void*){++advert_starts;advertising=true;return 0;}
int ble_gap_adv_stop(){++advert_stops;advertising=false;return 0;}
bool ble_gap_adv_active(){return advertising;}
int server_gap(ble_gap_event*,void*);
struct ble_gatt_error{int status=0;};
struct ble_gatt_svc{uint16_t start_handle=0,end_handle=0;};
struct ble_gatt_chr{uint16_t val_handle=0;};
struct ble_gap_conn_params{unsigned scan_itvl=0,scan_window=0,itvl_min=0,itvl_max=0,supervision_timeout=0;};
using GapCallback=int(*)(ble_gap_event*,void*);
using MtuCallback=int(*)(uint16_t,const ble_gatt_error*,uint16_t,void*);
using ServiceCallback=int(*)(uint16_t,const ble_gatt_error*,const ble_gatt_svc*,void*);
using CharacteristicCallback=int(*)(uint16_t,const ble_gatt_error*,const ble_gatt_chr*,void*);
int ble_gap_connect(int,const ble_addr_t*,unsigned,const ble_gap_conn_params*,GapCallback,void*);
int ble_gattc_exchange_mtu(uint16_t,MtuCallback,void*);
int ble_gattc_disc_svc_by_uuid(uint16_t,const ble_uuid_t*,ServiceCallback,void*);
int ble_gattc_disc_chrs_by_uuid(uint16_t,uint16_t,uint16_t,const ble_uuid_t*,CharacteristicCallback,void*);
'''

CONNECTION_SEAMS = r'''
enum class MtuMode{normal,already_complete,already_pending,start_error,callback_error,invalid_mtu,no_reply,stale_cookie,wrong_connection,cancelled};
MtuMode mtu_mode=MtuMode::normal;uint16_t negotiated_mtu=527;
std::vector<std::string>connection_steps;
void mtu_event(uint16_t connection,void* token,uint16_t channel=BLE_L2CAP_CID_ATT,uint16_t value=0){
 ble_gap_event event;event.type=BLE_GAP_EVENT_MTU;event.mtu={connection,channel,value?value:negotiated_mtu};client_gap(&event,token);
}
int ble_gap_connect(int,const ble_addr_t*,unsigned timeout,const ble_gap_conn_params*,GapCallback callback,void* token){
 if(timeout!=10000)throw std::runtime_error("connection timeout changed");
 connection_steps.push_back("connect");ble_gap_event event;event.type=BLE_GAP_EVENT_CONNECT;event.connect.conn_handle=31;callback(&event,token);
 if(mtu_mode==MtuMode::already_complete)mtu_event(31,token);
 return 0;
}
int ble_gattc_exchange_mtu(uint16_t connection,MtuCallback callback,void* token){
 connection_steps.push_back("mtu");
 if(mtu_mode==MtuMode::start_error)return BLE_HS_EINVAL;
 if(mtu_mode==MtuMode::already_complete)return BLE_HS_EALREADY;
 if(mtu_mode==MtuMode::no_reply)return 0;
 during_wait=[=](){
  if(mtu_mode==MtuMode::cancelled){gatt_client_disconnect();return;}
  if(mtu_mode==MtuMode::already_pending){mtu_event(connection,token);return;}
  const auto cookie=mtu_mode==MtuMode::stale_cookie?reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(token)-1):token;
  const auto handle=mtu_mode==MtuMode::wrong_connection?uint16_t(connection+1):connection;
  ble_gatt_error error{mtu_mode==MtuMode::callback_error?BLE_HS_ENOTCONN:0};
  if(mtu_mode==MtuMode::normal)mtu_event(connection,token);
  callback(handle,&error,mtu_mode==MtuMode::invalid_mtu?22:negotiated_mtu,cookie);
 };
 return mtu_mode==MtuMode::already_pending?BLE_HS_EALREADY:0;
}
int ble_gattc_disc_svc_by_uuid(uint16_t connection,const ble_uuid_t*,ServiceCallback callback,void* token){
 connection_steps.push_back("services");ble_gatt_error ok{},done{BLE_HS_EDONE};ble_gatt_svc service{10,20};callback(connection,&ok,&service,token);callback(connection,&done,nullptr,token);return 0;
}
int ble_gattc_disc_chrs_by_uuid(uint16_t connection,uint16_t,uint16_t,const ble_uuid_t*,CharacteristicCallback callback,void* token){
 connection_steps.push_back("characteristics");ble_gatt_error ok{},done{BLE_HS_EDONE};ble_gatt_chr characteristic{17};callback(connection,&ok,&characteristic,token);callback(connection,&done,nullptr,token);return 0;
}
'''

SUITE = r'''
void check(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
void reset(){client_generation=7;client_callback_cookie=7;client_connection=cancelled_client=BLE_HS_CONN_HANDLE_NONE;candidate_address={};discovered.clear();client_workflow=client_discovery_waiting=true;client_stopping=false;client_error=0;wakes=cancellations=connection_cancellations=terminations=0;terminated_handle=BLE_HS_CONN_HANDLE_NONE;server_open=true;server_sealed=false;server_connection=BLE_HS_CONN_HANDLE_NONE;server_session=0;advertising=true;advert_starts=advert_stops=closed_work=0;failed_proofs=0;client_service_start=client_service_end=client_attribute=0;client_record=0;client_mtu_ready=client_mtu_waiting=false;pending_notifications=0;during_wait={};bounded_waits.clear();connection_steps.clear();mtu_mode=MtuMode::normal;negotiated_mtu=527;}
void* token(uintptr_t value=7){return reinterpret_cast<void*>(value);}
void advertise(const std::string& name,unsigned address=42,bool real_service=true,uintptr_t cookie=7){
 std::string data(reinterpret_cast<const char*>(service_uuid.u.bytes),16);if(!real_service)data[0]^=1;data+=name;
 ble_gap_event event;event.type=BLE_GAP_EVENT_DISC;event.disc.data=reinterpret_cast<const uint8_t*>(data.data());event.disc.length_data=data.size();event.disc.addr.val[0]=address;client_gap(&event,token(cookie));
}
void collection(){
 advertise("A-abcdef");advertise("A-123456",43);
 check(discovered.size()==2&&discovered[0].public_value.label=="Amity abcdef"&&discovered[1].public_value.label=="Amity 123456","advertised candidates not collected");
 check(discovered[0].address.val[0]==42&&discovered[1].address.val[0]==43&&discovered[0].public_value.token!=discovered[1].public_value.token,"opaque tokens/exact addresses not distinct");
 check(wakes==0&&cancellations==0&&client_discovery_waiting,"discovery stopped at first candidate instead of collecting window");
 ble_gap_event done;done.type=BLE_GAP_EVENT_DISC_COMPLETE;client_gap(&done,token());check(wakes==1&&client_error==0&&!client_discovery_waiting,"natural scan completion did not wake once");
 client_gap(&done,token());check(wakes==1,"duplicate completion woke twice");
}
void service_and_bounds(){
 advertise("A-abcdef",42,false);check(discovered.empty(),"suffix-only advertiser admitted without service UUID");
 advertise("A-abcdef",42);advertise("A-abcdef",42);check(discovered.size()==1,"same BLE address duplicated");
 advertise("<script>",43);check(discovered.back().public_value.label=="Amity","untrusted label copied into public UI");
 for(unsigned n=44;n<60;++n)advertise("A-abcdef",n);check(discovered.size()==8,"candidate list exceeded bound");
}
void stale(){
 client_generation=UINT64_MAX;advertise("A-abcdef",42,true,6);check(discovered.empty()&&wakes==0,"stale callback affected scan");
 advertise("A-abcdef");check(discovered.size()==1,"64-bit radio generation was truncated into 32-bit callback cookie");
}
void cancellation(){
 advertise("A-abcdef");client_connection=31;gatt_client_disconnect();
 check(!client_workflow&&!client_discovery_waiting&&client_stopping&&discovered.empty(),"disconnect retained selectable candidates");
 check(client_callback_cookie==8&&client_generation==7,"disconnect did not fence callback independently of radio generation");
 check(wakes==1&&client_error==BLE_HS_ENOTCONN,"cancellation did not promptly wake waiter as FAILURE");
 check(terminations==1&&terminated_handle==31&&cancelled_client==31&&cancellations==1&&connection_cancellations==1,"disconnect did not cancel exact procedures");
 advertise("A-abcdef");ble_gap_event done;done.type=BLE_GAP_EVENT_DISC_COMPLETE;client_gap(&done,token());check(wakes==1&&discovered.empty(),"old callback revived canceled scan");
 gatt_client_disconnect();check(wakes==1&&terminations==1,"repeated disconnect affected new work");
}
void exact_disconnect(){
 client_connection=31;ble_gap_event event;event.type=BLE_GAP_EVENT_DISCONNECT;event.disconnect.conn.conn_handle=32;client_gap(&event,token());check(wakes==0&&client_connection==31,"other connection disconnected workflow");
 event.disconnect.conn.conn_handle=31;client_gap(&event,token());check(wakes==1&&client_stopping&&client_error==BLE_HS_ENOTCONN,"own disconnect was not surfaced");
}
int connect_candidate(){
 client_discovery_waiting=false;discovered.push_back({{"chosen","Amity abcdef"},{}});
 return gatt_client_connect_candidate("chosen",client_generation);
}
void mtu_negotiation(){
 for(const uint16_t mtu:{uint16_t(23),uint16_t(527)}){
  reset();negotiated_mtu=mtu;
  check(connect_candidate()==ESP_OK,"successful MTU negotiation prevented candidate connection");
  check((connection_steps==std::vector<std::string>{"connect","mtu","services","characteristics"}),"discovery began before MTU negotiation");
  check(client_mtu_ready&&!client_mtu_waiting&&client_attribute==17,"negotiation/discovery did not retain the exact live connection");
  check((bounded_waits==std::vector<unsigned>{11000,5000,5000,5000}),"MTU exchange did not use a bounded wait");
 }
}
void mtu_already_sent(){
 for(auto mode:{MtuMode::already_complete,MtuMode::already_pending})for(const uint16_t mtu:{uint16_t(23),uint16_t(527)}){
  reset();mtu_mode=mode;negotiated_mtu=mtu;
  check(connect_candidate()==ESP_OK,"already-sent MTU request did not await/use its exact ATT event");
  check(client_mtu_ready&&!client_mtu_waiting&&connection_steps.size()==4,"already-sent exchange bypassed its completion fence");
 }
}
void mtu_failures(){
 for(auto mode:{MtuMode::start_error,MtuMode::callback_error,MtuMode::invalid_mtu,MtuMode::no_reply,MtuMode::stale_cookie,MtuMode::wrong_connection,MtuMode::cancelled}){
  reset();mtu_mode=mode;
  check(connect_candidate()==ESP_FAIL,"failed/stale MTU negotiation admitted service discovery");
  check((connection_steps==std::vector<std::string>{"connect","mtu"})&&client_attribute==0,"discovery advanced after failed MTU negotiation");
  if(mode!=MtuMode::start_error)check(bounded_waits.back()==5000,"missing/stale MTU callback was not bounded");
 }
}
void mtu_callback_fences(){
 client_connection=31;ble_gatt_error ok{};
 check(on_mtu(32,&ok,527,token())==BLE_HS_EDONE,"MTU callback accepted another connection");
 check(on_mtu(31,&ok,527,token(6))==BLE_HS_EDONE,"MTU callback accepted stale session");
 check(!wakes&&!client_mtu_ready,"stale MTU callback completed current work");
 client_stopping=true;on_mtu(31,&ok,527,token());check(!wakes,"cancelled MTU callback completed current work");
 client_stopping=false;on_mtu(31,nullptr,527,token());check(wakes==1&&client_error==BLE_HS_EINVAL&&!client_mtu_ready,"missing MTU status was accepted");
}
void mtu_event_fences(){
 client_connection=31;client_mtu_waiting=true;
 mtu_event(32,token());mtu_event(31,token(6));mtu_event(31,token(),5);mtu_event(31,token(),BLE_L2CAP_CID_ATT,22);
 check(!wakes&&!client_mtu_ready&&client_mtu_waiting,"unrelated or invalid ATT event completed MTU exchange");
 mtu_event(31,token(),BLE_L2CAP_CID_ATT,23);
 check(wakes==1&&client_mtu_ready&&!client_mtu_waiting,"exact default-MTU event did not complete pending automatic exchange");
 mtu_event(31,token());check(wakes==1,"duplicate MTU event completed another procedure");
}
void seal_prepared(){
 ble_gap_event event;event.type=BLE_GAP_EVENT_CONNECT;event.connect.conn_handle=41;server_gap(&event,token(9));
 check(server_connection==41&&server_session==1,"candidate connection not admitted");
 gatt_server_seal();check(server_open&&server_sealed&&server_connection==41&&terminations==0&&advert_stops==1,"sealing dropped active prepare/WiFi channel");
 event.type=BLE_GAP_EVENT_DISCONNECT;event.disconnect.conn.conn_handle=41;server_gap(&event,token(9));
 check(server_connection==BLE_HS_CONN_HANDLE_NONE&&closed_work==1&&advert_starts==0,"prepared candidate re-advertised after disconnect");
 event.type=BLE_GAP_EVENT_CONNECT;event.connect.conn_handle=42;server_gap(&event,token(9));check(terminations==1&&terminated_handle==42&&server_connection==BLE_HS_CONN_HANDLE_NONE,"sealed candidate accepted another connection");
}
int main(){struct Test{const char* name;void(*run)();};int failed=0;for(auto test:{Test{"collect multiple real-service candidates",collection},Test{"service filtering, address dedup, bounded list",service_and_bounds},Test{"exact callback cookie independent of radio generation",stale},Test{"cancel wakes failure and fences callbacks",cancellation},Test{"only exact connection disconnect completes",exact_disconnect},Test{"negotiate full/default MTU before discovery",mtu_negotiation},Test{"already-sent MTU awaits exact ATT completion",mtu_already_sent},Test{"MTU initiation/error/timeout/stale/cancel failure fences",mtu_failures},Test{"MTU callback session/connection/status fences",mtu_callback_fences},Test{"automatic MTU event session/connection/channel fences",mtu_event_fences},Test{"prepared seal keeps active channel, blocks reconnect",seal_prepared}}){reset();try{test.run();std::cout<<"PASS "<<test.name<<'\n';}catch(const std::exception& error){++failed;std::cerr<<"FAIL "<<test.name<<": "<<error.what()<<'\n';}}return failed?1:0;}
'''

def function(source: str, declaration: str) -> str:
    start = source.index(declaration)
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[1] /
                        'components/consortium/gatt_transport.cpp')
    args = parser.parse_args()
    source = args.source.read_text()
    actual = ''.join(function(source, declaration) for declaration in (
        'void complete(int error)', 'bool current(void *token)',
        'bool admission_service(const ble_hs_adv_fields& fields)', 'int client_gap(ble_gap_event *event,void *token)', 'int on_mtu(',
        'int on_service(', 'int on_characteristic(', 'bool wait(', 'void clear_done()', 'void gatt_client_disconnect()',
        'int advertise_remaining()', 'int server_gap(ble_gap_event *event,void *token)', 'void gatt_server_seal()'))
    connection = function(source, 'esp_err_t gatt_client_connect_candidate(')
    with tempfile.TemporaryDirectory(prefix='amity-gatt-callback-') as directory:
        cpp = Path(directory) / 'actual_callback_test.cpp'
        binary = Path(directory) / 'actual_callback_test'
        cpp.write_text(PREFIX + actual + CONNECTION_SEAMS + connection + SUITE)
        compiler = shlex.split(os.environ.get('CXX', 'c++'))
        subprocess.run(compiler + ['-std=c++17', '-Wall', '-Wextra', '-Werror', '-g',
                                  '-fsanitize=address,undefined', str(cpp), '-o', str(binary)], check=True)
        return subprocess.run([str(binary)], check=False).returncode


if __name__ == '__main__':
    raise SystemExit(main())
