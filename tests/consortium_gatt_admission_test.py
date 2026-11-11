#!/usr/bin/env python3
"""Exercise the actual candidate worker and record assembler without BLE hardware."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
PREFIX = r'''
#include "amity/consortium/bootstrap.hpp"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
using namespace amity::consortium;
using QueueHandle_t=void*;
#define DRAM_ATTR
#define EXT_RAM_BSS_ATTR
struct StaticQueue_t{uint8_t* storage=nullptr;size_t capacity=0,item_size=0,head=0,count=0;};
QueueHandle_t xQueueCreateStatic(size_t,size_t,uint8_t*,StaticQueue_t*);
using esp_err_t=int;
constexpr int pdTRUE=1,ESP_OK=0,BLE_ERR_REM_USER_CONN_TERM=19;
constexpr uint16_t BLE_HS_CONN_HANDLE_NONE=0xffff;
unsigned pdMS_TO_TICKS(unsigned value){return value;}
int xQueueSend(void*,const void*,unsigned);
int xQueueReceive(void*,void*,unsigned);
int ble_gap_terminate(uint16_t,int);
int ble_gap_adv_stop();
uint64_t clock_ms=100;
uint64_t now_ms(){return clock_ms;}
enum class BootstrapFailure : uint8_t { none=0, crypto_self_test_failed=1 };
enum class AdmissionMode { button,setup_code };
AdmissionMode mode=AdmissionMode::button;
AdmissionMode configured_admission_mode(){return mode;}
using BootstrapHandler=std::function<esp_err_t(std::string_view,std::string&)>;
unsigned checks=0,constructors=0,handshakes=0,decryptions=0,handlers=0,terminations=0,advert_stops=0;
bool crypto_ok=true;
std::function<void()>during_check;
extern std::mutex state_mutex;
bool admission_crypto_self_test(AdmissionMode chosen){
    if(chosen!=mode)throw std::runtime_error("wrong admission profile");
    bool unlocked=false;
    std::thread probe([&]{if(state_mutex.try_lock()){state_mutex.unlock();unlocked=true;}});probe.join();
    if(!unlocked)throw std::runtime_error("self-test ran under transport lock");
    ++checks;clock_ms+=1950;if(during_check){auto callback=std::move(during_check);callback();}
    return crypto_ok;
}
class AdmissionServer {
    bool ready_=false;
public:
    AdmissionServer(AdmissionMode chosen,std::string,uint32_t){
        if(chosen!=mode||!checks||!crypto_ok)throw std::runtime_error("crypto used before successful test");
        ++constructors;
    }
    bool authenticated()const{return ready_;}
    bool handshake(std::string_view request,std::string& response){++handshakes;ready_=request=="public";response="proof";return ready_;}
    bool decrypt(std::string_view request,std::string& plain){++decryptions;plain=request;return ready_;}
    bool encrypt(std::string_view plain,std::string& response){response=plain;return ready_;}
};
'''
SUITE = r'''
struct StopWorker {};
unsigned queue_creations=0;bool fail_queue_creation=false;
QueueHandle_t xQueueCreateStatic(size_t capacity,size_t item_size,uint8_t* storage,StaticQueue_t* control){
    ++queue_creations;
    if(capacity!=16||item_size!=sizeof(Work)||storage!=work_queue_storage||control!=&work_queue_control)
        throw std::runtime_error("admission queue did not use persistent external payload/internal control seams");
    if(fail_queue_creation)return nullptr;
    *control={storage,capacity,item_size,0,0};return control;
}
int xQueueSend(void* handle,const void* bytes,unsigned wait){
    auto& queue=*static_cast<StaticQueue_t*>(handle);
    if(wait)throw std::runtime_error("GATT callback waited for admission queue capacity");
    if(queue.count==queue.capacity)return 0;
    memcpy(queue.storage+((queue.head+queue.count)%queue.capacity)*queue.item_size,bytes,queue.item_size);
    ++queue.count;return pdTRUE;
}
int xQueueReceive(void* handle,void* bytes,unsigned){
    auto& queue=*static_cast<StaticQueue_t*>(handle);if(!queue.count)throw StopWorker{};
    memcpy(bytes,queue.storage+queue.head*queue.item_size,queue.item_size);
    queue.head=(queue.head+1)%queue.capacity;--queue.count;return pdTRUE;
}
int ble_gap_terminate(uint16_t,int){++terminations;return 0;}
int ble_gap_adv_stop(){++advert_stops;return 0;}
void check(bool good,const char* message){if(!good)throw std::runtime_error(message);}
void run(){try{worker(nullptr);}catch(const StopWorker&){}check(!processing_record&&!outstanding_work,"worker retained work");}
void submit(uint32_t rid,uint8_t endpoint,std::string_view bytes,size_t total=0,size_t offset=0){
    Work work;work.generation=generation;work.session=server_session;work.connection=server_connection;
    work.record=rid;work.endpoint=endpoint;work.total=total?total:bytes.size();work.offset=offset;work.length=bytes.size();
    memcpy(work.bytes,bytes.data(),bytes.size());check(post_work(work),"could not post request");
}
void reset(){
    close_channel();fail_queue_creation=false;work_queue=create_work_queue();tested_generation=0;self_test_passed=false;
    generation=7;server_session=9;server_connection=31;server_open=true;server_sealed=false;deadline=180100;
    server_id="00000000000000000000000000000001";server_pin=20202021;
    clock_ms=100;failed_proofs=0;outstanding_work=0;processing_record=false;
    response_record=response_offset=response_endpoint=response_status=0;response_failure=BootstrapFailure::none;srv_response.clear();
    checks=constructors=handshakes=decryptions=handlers=terminations=advert_stops=0;crypto_ok=true;during_check={};
    server_handler=[](std::string_view request,std::string& response){++handlers;response=request;return ESP_OK;};
}
void queue_storage_and_capacity(){
    reset();check(work_queue==&work_queue_control&&sizeof(work_queue_storage)==16*sizeof(Work),"queue capacity/storage size changed");
    // Every callback record is copied before its stack and BLE input buffers
    // are reused. The seventeenth record must fail without disturbing FIFO.
    for(unsigned n=0;n<16;++n){
        Work work;work.record=n;work.length=sizeof(work.bytes);memset(work.bytes,int(n+1),sizeof(work.bytes));
        check(post_work(work),"admission queue lost an available slot");memset(work.bytes,0,sizeof(work.bytes));
    }
    check(outstanding_work==16&&!post_work(Work{})&&outstanding_work==16,"full queue did not roll back work count");
    for(unsigned n=0;n<16;++n){Work work;check(xQueueReceive(work_queue,&work,0)==pdTRUE,"queued callback vanished");
        check(work.record==n&&std::all_of(std::begin(work.bytes),std::end(work.bytes),[n](auto byte){return byte==n+1;}),"callback bytes/lifetime/FIFO changed");
        --outstanding_work;
    }
    // Wrap the ring and verify reuse after consuming a prefix, not only a full
    // drain. No backing allocation or queue control object is replaced.
    for(unsigned n=0;n<12;++n){Work work;work.record=n;check(post_work(work),"ring refill failed");}
    for(unsigned n=0;n<8;++n){Work work;check(xQueueReceive(work_queue,&work,0)&&work.record==n,"ring prefix order changed");--outstanding_work;}
    for(unsigned n=12;n<24;++n){Work work;work.record=n;check(post_work(work),"wrapped ring refill failed");}
    for(unsigned n=8;n<24;++n){Work work;check(xQueueReceive(work_queue,&work,0)&&work.record==n,"wrapped ring order changed");--outstanding_work;}
    check(!outstanding_work,"queue drain retained admission work");
    fail_queue_creation=true;check(create_work_queue()==nullptr,"queue creation failure was concealed");
    fail_queue_creation=false;work_queue=create_work_queue();
}
void lazy_success(){
    for(auto profile:{AdmissionMode::button,AdmissionMode::setup_code}){
        reset();mode=profile;run();check(!checks&&!constructors,"idle boot executed admission crypto");
        submit(1,0,"pub",6);run();check(!checks&&!constructors&&channel_live,"incomplete request ran crypto or lost assembly");
        submit(1,0,"lic",6,3);run();
        check(checks==1&&constructors==1&&handshakes==1&&response_status==1&&response_failure==BootstrapFailure::none,"complete handshake skipped selected test");
        submit(1,0,"public");run();check(checks==1&&handshakes==1,"duplicate repeated test or handshake");
        submit(2,1,"secret");run();check(handlers==1&&decryptions==1&&srv_response=="secret","successful check changed encrypted setup");
        ++server_session;++server_connection;submit(3,0,"public");run();
        check(checks==1&&constructors==2&&handshakes==2,"same physical window repeated successful test");
        ++generation;submit(4,0,"public");run();check(checks==2&&handshakes==3,"new window reused prior successful result");
    }
}
void diagnostic_failure(){
    for(auto profile:{AdmissionMode::button,AdmissionMode::setup_code}){
    reset();mode=profile;crypto_ok=false;submit(1,0,"public");run();
    check(checks==1&&!constructors&&!handshakes&&!decryptions&&!handlers,"failed check reached admission crypto/credentials");
    check(server_open&&!failed_proofs&&!advert_stops&&!terminations&&response_status==2&&response_failure==BootstrapFailure::crypto_self_test_failed&&response_record==1&&response_endpoint==0&&srv_response.empty(),"failed check did not leave bounded diagnostic available");
    ++server_session;++server_connection;submit(2,0,"public");run();
    check(checks==1&&response_record==2&&response_failure==BootstrapFailure::crypto_self_test_failed,"reconnect lost cached failure diagnostic");
    submit(3,1,"secret");run();
    check(checks==1&&!constructors&&!decryptions&&!handlers&&response_status==2&&response_failure==BootstrapFailure::none,"encrypted request obtained self-test diagnostic or credentials");
    ++generation;crypto_ok=true;submit(4,0,"public");run();
    check(checks==2&&constructors==1&&handshakes==1&&response_status==1&&response_failure==BootstrapFailure::none,"new physical window did not retry failed check");
    }
}
void stale_check(){
    for(unsigned mutation=0;mutation<5;++mutation)for(bool passes:{false,true}){
        reset();crypto_ok=passes;during_check=[=](){switch(mutation){case 0:++generation;break;case 1:++server_session;break;case 2:++server_connection;break;case 3:server_open=false;break;case 4:clock_ms=deadline;break;}};
        submit(1,0,"public");run();
        check(checks==1&&!constructors&&!handshakes&&!handlers&&!response_record&&response_failure==BootstrapFailure::none&&!channel_live,"stale completed check published diagnostic or admitted handshake");
    }
    reset();submit(1,1,"secret");run();check(!checks&&!constructors&&!handlers&&response_failure==BootstrapFailure::none,"encrypted first request started self-test");
    reset();submit(1,0,"pub",6);run();clock_ms+=30000;submit(1,0,"lic",6,3);run();
    check(!checks&&!constructors&&terminations==1,"expired assembly started crypto");
}
int main(){try{queue_storage_and_capacity();lazy_success();diagnostic_failure();stale_check();std::cout<<"PASS actual candidate worker: persistent copied queue records,16-slot FIFO/overflow/ring reuse; lazy self-test outside locks, success/failure per window, duplicate/reconnect reuse and exact stale-work fences\n";return 0;}catch(const std::exception& error){std::cerr<<"FAIL "<<error.what()<<'\n';return 1;}}
'''


def main():
    source = (ROOT / 'components/consortium/gatt_transport.cpp').read_text()
    # Native addresses cannot prove target PSRAM placement. Verify the declared
    # placement seams here; the firmware ELF checks their actual sections.
    assert 'DRAM_ATTR StaticQueue_t work_queue_control;' in source
    assert 'EXT_RAM_BSS_ATTR uint8_t work_queue_storage[' in source
    assert 'EXT_RAM_BSS_ATTR BootstrapRecords records;' in source
    assert 'work_queue=create_work_queue();' in source
    assert 'xQueueCreate(16,sizeof(Work))' not in source
    actual = source[source.index('struct Work {'):source.index('int access(uint16_t connection,')]
    with tempfile.TemporaryDirectory(prefix='amity-gatt-admission-') as directory:
        cpp = Path(directory) / 'test.cpp'
        binary = Path(directory) / 'test'
        cpp.write_text(PREFIX + actual + SUITE)
        subprocess.run(shlex.split(os.environ.get('CXX', 'c++')) +
                       ['-std=c++17', '-Wall', '-Wextra', '-Werror', '-g', '-fsanitize=address,undefined',
                        '-I' + str(ROOT / 'components/consortium/include'),
                        '-I' + str(ROOT / 'components/amity_system/include'),
                        str(cpp), str(ROOT / 'components/consortium/bootstrap.cpp'),
                        str(ROOT / 'components/amity_system/model.cpp'), '-o', str(binary)], check=True)
        return subprocess.run([str(binary)], check=False).returncode


if __name__ == '__main__':
    raise SystemExit(main())
