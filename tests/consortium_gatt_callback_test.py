#!/usr/bin/env python3
"""Compile actual GATT callbacks against a cancel-without-event controller.

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
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>
constexpr int BLE_GAP_EVENT_DISC=1,BLE_GAP_EVENT_DISC_COMPLETE=2,BLE_GAP_EVENT_CONNECT=3,BLE_GAP_EVENT_DISCONNECT=4;
constexpr int BLE_HS_ENOTCONN=12;
constexpr uint16_t BLE_HS_CONN_HANDLE_NONE=0xffff;
constexpr int BLE_ERR_REM_USER_CONN_TERM=19;
struct ble_addr_t{uint8_t type=0,val[6]{};};
struct ble_uuid_t{uint8_t bytes[16]{};};struct ble_uuid128_t{ble_uuid_t u;};
const ble_uuid128_t service_uuid{{{1,2,3}}};ble_uuid128_t parsed_uuid;
int ble_uuid_cmp(const ble_uuid_t* a,const ble_uuid_t* b){return std::memcmp(a,b,16);}
struct ble_gap_event{
 int type=0;struct{const uint8_t* data=nullptr;uint8_t length_data=0;ble_addr_t addr{};}disc;
 struct{int status=0;uint16_t conn_handle=0;}connect;struct{struct{uint16_t conn_handle=0;}conn;}disconnect;
};
struct ble_hs_adv_fields{const uint8_t* name=nullptr;uint8_t name_len=0,num_uuids128=0;ble_uuid128_t* uuids128=nullptr;};
int ble_hs_adv_parse_fields(ble_hs_adv_fields* fields,const uint8_t* data,uint8_t length){if(length<16)return -1;std::memcpy(parsed_uuid.u.bytes,data,16);fields->uuids128=&parsed_uuid;fields->num_uuids128=1;fields->name=data+16;fields->name_len=length-16;return 0;}
struct RuntimeAdmissionCandidate{std::string token,label;};struct Discovered{RuntimeAdmissionCandidate public_value;ble_addr_t address;};
std::vector<Discovered> discovered;std::mutex client_mutex;
uint64_t client_generation=7;uint32_t client_callback_cookie=7;
uint16_t client_connection=BLE_HS_CONN_HANDLE_NONE,cancelled_client=BLE_HS_CONN_HANDLE_NONE;
ble_addr_t candidate_address{};bool client_workflow=true,client_discovery_waiting=true;std::atomic_bool client_stopping{false};std::atomic_int client_error{0};void* client_done=nullptr;
int wakes=0,cancellations=0,connection_cancellations=0,terminations=0;uint16_t terminated_handle=BLE_HS_CONN_HANDLE_NONE;
std::string random_hex(size_t length){static unsigned n=0;auto text=std::to_string(++n);return std::string(length*2-text.size(),'0')+text;}
void xSemaphoreGive(void*){++wakes;}int ble_gap_disc_cancel(){++cancellations;return 0;}int ble_gap_conn_cancel(){++connection_cancellations;return 0;}
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
'''

SUITE = r'''
void check(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
void reset(){client_generation=7;client_callback_cookie=7;client_connection=cancelled_client=BLE_HS_CONN_HANDLE_NONE;candidate_address={};discovered.clear();client_workflow=client_discovery_waiting=true;client_stopping=false;client_error=0;wakes=cancellations=connection_cancellations=terminations=0;terminated_handle=BLE_HS_CONN_HANDLE_NONE;server_open=true;server_sealed=false;server_connection=BLE_HS_CONN_HANDLE_NONE;server_session=0;advertising=true;advert_starts=advert_stops=closed_work=0;failed_proofs=0;}
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
void seal_prepared(){
 ble_gap_event event;event.type=BLE_GAP_EVENT_CONNECT;event.connect.conn_handle=41;server_gap(&event,token(9));
 check(server_connection==41&&server_session==1,"candidate connection not admitted");
 gatt_server_seal();check(server_open&&server_sealed&&server_connection==41&&terminations==0&&advert_stops==1,"sealing dropped active prepare/WiFi channel");
 event.type=BLE_GAP_EVENT_DISCONNECT;event.disconnect.conn.conn_handle=41;server_gap(&event,token(9));
 check(server_connection==BLE_HS_CONN_HANDLE_NONE&&closed_work==1&&advert_starts==0,"prepared candidate re-advertised after disconnect");
 event.type=BLE_GAP_EVENT_CONNECT;event.connect.conn_handle=42;server_gap(&event,token(9));check(terminations==1&&terminated_handle==42&&server_connection==BLE_HS_CONN_HANDLE_NONE,"sealed candidate accepted another connection");
}
int main(){struct Test{const char* name;void(*run)();};int failed=0;for(auto test:{Test{"collect multiple real-service candidates",collection},Test{"service filtering, address dedup, bounded list",service_and_bounds},Test{"exact callback cookie independent of radio generation",stale},Test{"cancel wakes failure and fences callbacks",cancellation},Test{"only exact connection disconnect completes",exact_disconnect},Test{"prepared seal keeps active channel, blocks reconnect",seal_prepared}}){reset();try{test.run();std::cout<<"PASS "<<test.name<<'\n';}catch(const std::exception& error){++failed;std::cerr<<"FAIL "<<test.name<<": "<<error.what()<<'\n';}}return failed?1:0;}
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
        'bool admission_service(const ble_hs_adv_fields& fields)', 'int client_gap(ble_gap_event *event,void *token)', 'void gatt_client_disconnect()',
        'int advertise_remaining()', 'int server_gap(ble_gap_event *event,void *token)', 'void gatt_server_seal()'))
    with tempfile.TemporaryDirectory(prefix='amity-gatt-callback-') as directory:
        cpp = Path(directory) / 'actual_callback_test.cpp'
        binary = Path(directory) / 'actual_callback_test'
        cpp.write_text(PREFIX + actual + SUITE)
        compiler = shlex.split(os.environ.get('CXX', 'c++'))
        subprocess.run(compiler + ['-std=c++17', '-Wall', '-Wextra', '-Werror', '-g',
                                  '-fsanitize=address,undefined', str(cpp), '-o', str(binary)], check=True)
        return subprocess.run([str(binary)], check=False).returncode


if __name__ == '__main__':
    raise SystemExit(main())
