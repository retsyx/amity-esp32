#!/usr/bin/env python3
"""Compile the complete production observer with fake SDK/clock boundaries.

No radio, network or DUT is used. Poisoned SDK-owned pointers, allocation/log
guards, actual lifecycle callbacks and wraparound exercise the shipping body.
Repeat with the target's trait-false four-byte representation, routing its
operations through the unchanged pinned SDK dispatch macros. Only hardware
instructions/critical-section seams are modeled; target ELF placement and
disassembly remain the proof of which branch ships.
"""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
IDF = Path(os.environ.get('IDF_PATH', Path.home() / '.espressif/v6.1/esp-idf'))

SDK = r'''
#pragma once
#include <cstddef>
#include <cstdint>
using esp_err_t=int;
constexpr int ESP_OK=0,ESP_FAIL=-1,ESP_ERR_NO_MEM=0x101,ESP_ERR_INVALID_STATE=0x103,ESP_ERR_WIFI_NOT_STARTED=0x3002;
enum wifi_interface_t {WIFI_IF_STA,WIFI_IF_AP};
enum wifi_ps_type_t {WIFI_PS_NONE,WIFI_PS_MIN_MODEM,WIFI_PS_MAX_MODEM};
struct esp_netif_t {};
using esp_event_base_t=const char*;
using esp_event_handler_t=void(*)(void*,esp_event_base_t,int32_t,void*);
using wifi_tx_done_cb_t=void(*)(uint8_t,uint8_t*,uint16_t*,bool);
inline const char* WIFI_EVENT="wifi";
constexpr int ESP_EVENT_ANY_ID=-1,WIFI_EVENT_STA_START=1,WIFI_EVENT_STA_STOP=2,WIFI_EVENT_STA_CONNECTED=3;
#define DRAM_ATTR
esp_err_t esp_event_handler_register(esp_event_base_t,int32_t,esp_event_handler_t,void*);
esp_err_t esp_wifi_set_tx_done_cb(wifi_tx_done_cb_t);
esp_err_t esp_wifi_get_ps(wifi_ps_type_t*);
int64_t esp_timer_get_time();
void diagnostic_log(const char*,const char*,const char*,...);
#define ESP_LOGI(tag,...) diagnostic_log("I",tag,__VA_ARGS__)
#define ESP_LOGW(tag,...) diagnostic_log("W",tag,__VA_ARGS__)
'''

PREFIX = r'''
#include <atomic>
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <new>
#include <string>
#include <vector>
#include "sdk.hpp"
bool callback=false;
void* operator new(std::size_t size){assert(!callback&&"callback allocated");if(auto* p=std::malloc(size?size:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t size){return ::operator new(size);}
void operator delete(void* p) noexcept{std::free(p);}
void operator delete[](void* p) noexcept{std::free(p);}
void operator delete(void* p,std::size_t) noexcept{std::free(p);}
void operator delete[](void* p,std::size_t) noexcept{std::free(p);}
void* guarded_malloc(size_t size){assert(!callback&&"callback malloc");return std::malloc(size);}
void* guarded_calloc(size_t count,size_t size){assert(!callback&&"callback calloc");return std::calloc(count,size);}
void* guarded_realloc(void* pointer,size_t size){assert(!callback&&"callback realloc");return std::realloc(pointer,size);}
void guarded_free(void* pointer){assert(!callback&&"callback free");std::free(pointer);}
uint64_t clock_ms=100;
unsigned tx_calls=0,rx_calls=0,event_calls=0,register_calls=0,ps_calls=0,clock_reads=0;
int tx_result=0,rx_result=0,event_result=0,register_result=0,ps_result=0;
int registration_event=0;
wifi_ps_type_t actual_ps=WIFI_PS_NONE;
wifi_tx_done_cb_t sdk_done=nullptr;
esp_event_handler_t sdk_event=nullptr;
void* expected_buffer=reinterpret_cast<void*>(uintptr_t{1});
void* expected_driver=reinterpret_cast<void*>(uintptr_t{3});
esp_netif_t* expected_netif=reinterpret_cast<esp_netif_t*>(uintptr_t{5});
std::vector<std::string> logs;
struct Callback {Callback(){assert(!callback);callback=true;}~Callback(){callback=false;}};
int64_t esp_timer_get_time(){++clock_reads;return static_cast<int64_t>(clock_ms*1000);}
void diagnostic_log(const char* level,const char* tag,const char* format,...){
    assert(!callback&&"callback logged");assert(std::string(tag)=="network_diag");
    char line[384];va_list values;va_start(values,format);const int length=std::vsnprintf(line,sizeof(line),format,values);va_end(values);
    assert(length>0&&length<static_cast<int>(sizeof(line)));logs.push_back(std::string(level)+":"+line);
}
esp_err_t esp_event_handler_register(esp_event_base_t base,int32_t event,esp_event_handler_t fn,void* context){
    assert(base==WIFI_EVENT&&event==ESP_EVENT_ANY_ID&&context==nullptr&&register_calls==0);++event_calls;
    if(event_result==ESP_OK)sdk_event=fn;return event_result;
}
esp_err_t esp_wifi_set_tx_done_cb(wifi_tx_done_cb_t fn){
    assert(event_calls==1&&fn);++register_calls;
    if(registration_event)sdk_event(nullptr,WIFI_EVENT,registration_event,reinterpret_cast<void*>(uintptr_t{7}));
    if(register_result==ESP_OK)sdk_done=fn;return register_result;
}
esp_err_t esp_wifi_get_ps(wifi_ps_type_t* mode){++ps_calls;*mode=actual_ps;return ps_result;}
extern "C" esp_err_t __real_esp_wifi_internal_tx(wifi_interface_t interface,void* buffer,uint16_t length){
    assert(callback&&interface==WIFI_IF_AP&&buffer==expected_buffer&&length==UINT16_MAX);++tx_calls;clock_ms+=7;return tx_result;
}
extern "C" esp_err_t __real_esp_netif_receive(esp_netif_t* interface,void* buffer,size_t length,void* driver){
    assert(callback&&interface==expected_netif&&buffer==expected_buffer&&length==size_t{123456}&&driver==expected_driver);++rx_calls;clock_ms+=11;return rx_result;
}
'''

TARGET_ATOMICS = r'''
uintptr_t model_external_low=0,model_external_high=0;
unsigned model_critical_calls=0,model_hardware_calls=0;
void model_enter_critical(){assert(!callback&&"DRAM callback used SDK global critical section");++model_critical_calls;}
void model_exit_critical(){}
// Hardware seam: these correspond to the pinned SDK helpers compiled with
// -mno-disable-hardware-atomics. The dispatch macros below remain unchanged.
unsigned int __atomic_s32c1i_load_4(const volatile void* p,int order){++model_hardware_calls;return __atomic_load_n(static_cast<const volatile unsigned int*>(p),order);}
void __atomic_s32c1i_store_4(volatile void* p,unsigned int value,int order){++model_hardware_calls;__atomic_store_n(static_cast<volatile unsigned int*>(p),value,order);}
unsigned int __atomic_s32c1i_fetch_add_4(volatile void* p,unsigned int value,int order){++model_hardware_calls;return __atomic_fetch_add(static_cast<volatile unsigned int*>(p),value,order);}
unsigned int __atomic_s32c1i_fetch_and_4(volatile void* p,unsigned int value,int order){++model_hardware_calls;return __atomic_fetch_and(static_cast<volatile unsigned int*>(p),value,order);}
unsigned int __atomic_s32c1i_fetch_or_4(volatile void* p,unsigned int value,int order){++model_hardware_calls;return __atomic_fetch_or(static_cast<volatile unsigned int*>(p),value,order);}
unsigned int __atomic_s32c1i_exchange_4(volatile void* p,unsigned int value,int order){++model_hardware_calls;return __atomic_exchange_n(static_cast<volatile unsigned int*>(p),value,order);}
bool __atomic_s32c1i_compare_exchange_4(volatile void* p,void* expected,unsigned int desired,bool weak,int success,int failure){++model_hardware_calls;return __atomic_compare_exchange_n(static_cast<volatile unsigned int*>(p),static_cast<unsigned int*>(expected),desired,weak,success,failure);}
#define __atomic_load_4 sdk_load_4
#define __atomic_store_4 sdk_store_4
#define __atomic_fetch_add_4 sdk_fetch_add_4
#define __atomic_fetch_and_4 sdk_fetch_and_4
#define __atomic_fetch_or_4 sdk_fetch_or_4
#define __atomic_exchange_4 sdk_exchange_4
#define __atomic_compare_exchange_4 sdk_compare_exchange_4
// Include the actual esp_stdatomic.h immediately after this prefix.
'''

TARGET_ATOMICS_AFTER_HEADER = r'''
// The SDK file is C; rename its local identifier "new" for this C++ fixture.
#define new sdk_new_local
ATOMIC_LOAD(4,unsigned int)
ATOMIC_STORE(4,unsigned int)
_ATOMIC_OP_FUNCTION(4,unsigned int,fetch,add,old,+,)
_ATOMIC_OP_FUNCTION(4,unsigned int,fetch,and,old,&,)
_ATOMIC_OP_FUNCTION(4,unsigned int,fetch,or,old,|,)
ATOMIC_EXCHANGE(4,unsigned int)
ATOMIC_CMP_EXCHANGE(4,unsigned int)
#undef new
#undef __atomic_load_4
#undef __atomic_store_4
#undef __atomic_fetch_add_4
#undef __atomic_fetch_and_4
#undef __atomic_fetch_or_4
#undef __atomic_exchange_4
#undef __atomic_compare_exchange_4
namespace target_std {
using std::memory_order;
constexpr auto memory_order_relaxed=std::memory_order_relaxed,memory_order_release=std::memory_order_release,memory_order_acquire=std::memory_order_acquire;
template<class T> class alignas(4) atomic {
    unsigned int value;
public:
    // GCC's generic target trait is false because arbitrary PSRAM is allowed.
    static constexpr bool is_always_lock_free=false;
    constexpr atomic(T initial):value(static_cast<unsigned int>(initial)){}
    T load(memory_order order=std::memory_order_seq_cst)const{return static_cast<T>(sdk_load_4(&value,order));}
    void store(T next,memory_order order=std::memory_order_seq_cst){sdk_store_4(&value,static_cast<unsigned int>(next),order);}
    T fetch_add(T next,memory_order order=std::memory_order_seq_cst){return static_cast<T>(sdk_fetch_add_4(&value,static_cast<unsigned int>(next),order));}
    T fetch_and(T next,memory_order order=std::memory_order_seq_cst){return static_cast<T>(sdk_fetch_and_4(&value,static_cast<unsigned int>(next),order));}
    T fetch_or(T next,memory_order order=std::memory_order_seq_cst){return static_cast<T>(sdk_fetch_or_4(&value,static_cast<unsigned int>(next),order));}
    T exchange(T next,memory_order order=std::memory_order_seq_cst){return static_cast<T>(sdk_exchange_4(&value,static_cast<unsigned int>(next),order));}
    bool compare_exchange_strong(T& expected,T desired,memory_order order){
        unsigned int bits=static_cast<unsigned int>(expected);
        const bool result=sdk_compare_exchange_4(&value,&bits,static_cast<unsigned int>(desired),false,order,order);
        expected=static_cast<T>(bits);return result;
    }
};
}
void model_check_external_branch(){
    unsigned int external=0;model_external_low=reinterpret_cast<uintptr_t>(&external);model_external_high=model_external_low+sizeof(external);
    assert(sdk_fetch_add_4(&external,1,std::memory_order_relaxed)==0&&external==1&&model_critical_calls==1);
    model_external_low=model_external_high=0;
}
'''

SUITE = r'''
using namespace amity::network_diagnostics;
void event(int id){assert(sdk_event);Callback guard;sdk_event(reinterpret_cast<void*>(uintptr_t{9}),WIFI_EVENT,id,reinterpret_cast<void*>(uintptr_t{11}));}
void done(bool success){assert(sdk_done);Callback guard;sdk_done(255,reinterpret_cast<uint8_t*>(uintptr_t{1}),reinterpret_cast<uint16_t*>(uintptr_t{3}),success);}
void tx(){Callback guard;assert(__wrap_esp_wifi_internal_tx(WIFI_IF_AP,expected_buffer,UINT16_MAX)==tx_result);}
void rx(){Callback guard;assert(__wrap_esp_netif_receive(expected_netif,expected_buffer,size_t{123456},expected_driver)==rx_result);}
void begin(){Callback guard;start();}
void paths(){
    assert(snapshot().tx.attempts==0&&snapshot().rx.attempts==0);
    for(int result:{ESP_OK,ESP_ERR_NO_MEM,ESP_FAIL}){tx_result=rx_result=result;tx();rx();}
    assert(tx_calls==3&&rx_calls==3&&logs.empty()&&register_calls==0);
    const auto s=snapshot();assert(s.tx.attempts==3&&s.tx.ok==1&&s.tx.no_mem==1&&s.tx.other==1&&s.tx.last_duration_ms==7);
    assert(s.rx.attempts==3&&s.rx.ok==1&&s.rx.no_mem==1&&s.rx.other==1&&s.rx.last_duration_ms==11);
    assert(s.tx.last_error_code==ESP_FAIL&&s.rx.last_error_code==ESP_FAIL);
    assert(s.tx.last_call_ms==136&&s.tx.last_error_ms==143&&s.rx.last_error_ms==154&&s.now_ms==154);
    poll();const auto count=logs.size();assert(count==2);poll();assert(logs.size()==count);
    assert(logs.back().find("tx_nomem=1 tx_other=1 rx_nomem=1 rx_other=1")!=std::string::npos);
    // Reuse/mutate SDK storage after return. The observer keeps no buffer references.
    unsigned char bytes[4]={1,2,3,4};expected_buffer=bytes;tx_result=ESP_OK;tx();std::memset(bytes,0xff,sizeof(bytes));
    assert(snapshot().tx.ok==2&&snapshot().rx.ok==1);
    assert(snapshot().tx.last_error_code==ESP_FAIL); // Last error survives a later success.
    tx_result=0x7654;rx_result=-123;tx();rx();const auto errors=snapshot();
    assert(errors.tx.last_error_code==0x7654&&errors.rx.last_error_code==-123);
    poll();assert(logs.back().find("tx_rc=30292 rx_rc=-123")!=std::string::npos);
}
void lifecycle_test(){
    register_result=ESP_ERR_WIFI_NOT_STARTED;begin();assert(event_calls==1&&register_calls==1&&ps_calls==0&&logs.empty());
    auto s=snapshot();assert(s.event_registered==1&&s.tx_done_registered==0&&s.registration_error==ESP_ERR_WIFI_NOT_STARTED);
    event(WIFI_EVENT_STA_START);assert(register_calls==1&&snapshot().registration_pending!=0);
    register_result=ESP_OK;poll();s=snapshot();assert(register_calls==2&&ps_calls==1&&s.tx_done_registered&&s.power_save_checked&&s.power_save_mode==WIFI_PS_NONE);
    event(WIFI_EVENT_STA_CONNECTED);poll();assert(register_calls==2&&ps_calls==1);
    done(true);clock_ms+=5;done(false);assert(logs.size()==1);
    s=snapshot();assert(s.tx_done_ok==1&&s.tx_done_fail==1&&s.tx_done_last_fail_ms==105);
    poll();assert(logs.back().find("tx_done_fail=1")!=std::string::npos);const auto count=logs.size();poll();assert(logs.size()==count);
    event(WIFI_EVENT_STA_STOP);s=snapshot();assert(!s.tx_done_registered&&!s.power_save_checked&&s.power_save_mode==-1);
    poll();assert(register_calls==2&&ps_calls==1);
    event(WIFI_EVENT_STA_START);actual_ps=WIFI_PS_MIN_MODEM;poll();s=snapshot();assert(register_calls==3&&ps_calls==2&&s.power_save_checked&&s.power_save_mode==WIFI_PS_MIN_MODEM);
    begin();assert(event_calls==1&&register_calls==3);
}
void eager_success_start(){
    actual_ps=WIFI_PS_NONE;begin();assert(register_calls==1&&ps_calls==1&&snapshot().tx_done_registered);
    // Pinned setter accepts init-only state. Actual start must invalidate its
    // earlier readback, even if eager registration reported success.
    event(WIFI_EVENT_STA_START);assert(!snapshot().tx_done_registered&&!snapshot().power_save_checked);
    actual_ps=WIFI_PS_MAX_MODEM;poll();const auto s=snapshot();
    assert(register_calls==2&&ps_calls==2&&s.tx_done_registered&&s.power_save_checked&&s.power_save_mode==WIFI_PS_MAX_MODEM);
}
void startup_race(int first_result){
    registration_event=WIFI_EVENT_STA_START;register_result=first_result;begin();
    assert(register_calls==1&&snapshot().registration_pending&&ps_calls==0);
    registration_event=0;register_result=ESP_OK;poll();assert(register_calls==2&&ps_calls==1&&snapshot().tx_done_registered);
    poll();assert(register_calls==2&&ps_calls==1);
}
void stopped_registration(){
    registration_event=WIFI_EVENT_STA_STOP;begin();assert(register_calls==1&&!snapshot().tx_done_registered&&ps_calls==0);
    registration_event=0;event(WIFI_EVENT_STA_START);poll();assert(register_calls==2&&ps_calls==1&&snapshot().tx_done_registered);
}
void event_error_test(){
    event_result=ESP_FAIL;begin();const auto s=snapshot();assert(s.event_error==ESP_FAIL&&!s.event_registered&&!s.tx_done_registered&&register_calls==0);
    poll();assert(logs.size()==1);begin();poll();assert(event_calls==1&&register_calls==0&&logs.size()==1);
}
void registration_error_test(){
    register_result=ESP_FAIL;begin();poll();assert(register_calls==1&&!snapshot().tx_done_registered&&snapshot().registration_error==ESP_FAIL);
    event(WIFI_EVENT_STA_CONNECTED);poll();assert(register_calls==1&&ps_calls==0);
    event(WIFI_EVENT_STA_START);register_result=ESP_OK;ps_result=ESP_FAIL;poll();const auto s=snapshot();
    assert(register_calls==2&&ps_calls==1&&s.tx_done_registered&&s.power_save_checked&&s.power_save_mode==-1&&s.power_save_error==ESP_FAIL);
}
void wrap_test(){
    // 71.6-minute microsecond wrap must not affect millisecond timestamps.
    clock_ms=4'400'000;tx();assert(snapshot().tx.last_call_ms==4'400'000);
    tx_stats.attempts.store(UINT32_MAX);tx_stats.ok.store(UINT32_MAX);
    clock_ms=uint64_t{UINT32_MAX}-2;tx();const auto s=snapshot();
    assert(s.tx.attempts==0&&s.tx.ok==0&&s.tx.last_call_ms==UINT32_MAX-2&&s.tx.last_ok_ms==4&&s.tx.last_duration_ms==7);
    assert(uint32_t(s.now_ms-s.tx.last_call_ms)==7&&uint32_t(s.tx.attempts-UINT32_MAX)==1);
    begin();done_fail.store(UINT32_MAX);done(false);assert(snapshot().tx_done_fail==0);
}
void console_test(){
    poll();const auto lines=logs.size();clock_ms=1000;
    const auto reads=clock_reads;
    {
        Callback guard;
        record_console_call(100,0);record_console_call(200,99);
        // Overlapping calls contribute their full durations, not wall time.
        record_console_call(300,100);record_console_call(350,550);record_console_call(990,1);
    }
    assert(clock_reads==reads&&logs.size()==lines);
    auto s=snapshot();assert(s.console_calls==5&&s.console_elapsed_ms==750&&s.console_slow_calls==2);
    assert(s.console_last_slow_began_ms==350&&s.console_last_slow_duration_ms==550);
    poll();assert(logs.size()==lines); // Console observation cannot log itself.
    console_calls.store(UINT32_MAX);console_elapsed.store(UINT32_MAX-5);console_slow.store(UINT32_MAX);
    {Callback guard;record_console_call(UINT32_MAX-3,100);}
    clock_ms=uint64_t{UINT32_MAX}+102;s=snapshot();
    assert(s.console_calls==0&&s.console_elapsed_ms==94&&s.console_slow_calls==0);
    assert(s.console_last_slow_began_ms==UINT32_MAX-3&&s.console_last_slow_duration_ms==100);
    assert(uint32_t(s.now_ms-s.console_last_slow_began_ms)==105);
    poll();assert(logs.size()==lines);
}
int main(int argc,char** argv){
#ifdef NETWORK_TARGET_MODEL
    model_check_external_branch();
#endif
    assert(argc==2);const std::string name=argv[1];
    if(name=="paths")paths();else if(name=="lifecycle")lifecycle_test();else if(name=="eager")eager_success_start();
    else if(name=="race")startup_race(ESP_ERR_WIFI_NOT_STARTED);else if(name=="race-ok")startup_race(ESP_OK);
    else if(name=="stop")stopped_registration();else if(name=="event-error")event_error_test();
    else if(name=="registration-error")registration_error_test();else if(name=="wrap")wrap_test();
    else if(name=="console")console_test();else assert(false);
#ifdef NETWORK_TARGET_MODEL
    assert(model_critical_calls==1&&model_hardware_calls>0);
#endif
    std::cout<<"PASS actual network diagnostics "<<name<<"\n";
}
'''


def main():
    # Tie fake boundary declarations to the actual pinned SDK signatures.
    wifi = re.sub(r'\s+', ' ', (IDF / 'components/esp_wifi/include/esp_private/wifi.h').read_text())
    netif = re.sub(r'\s+', ' ', (IDF / 'components/esp_netif/include/esp_netif.h').read_text())
    for declaration in ('int esp_wifi_internal_tx(wifi_interface_t wifi_if, void *buffer, uint16_t len);',
                        'typedef void (* wifi_tx_done_cb_t)(uint8_t ifidx, uint8_t *data, uint16_t *data_len, bool txStatus);',
                        'esp_err_t esp_wifi_set_tx_done_cb(wifi_tx_done_cb_t cb);'):
        assert declaration in wifi, declaration
    assert 'esp_err_t esp_netif_receive(esp_netif_t *esp_netif, void *buffer, size_t len, void *eb);' in netif
    libc_cmake = (IDF / 'components/esp_libc/CMakeLists.txt').read_text()
    assert re.search(r'set_source_files_properties\("src/port/xtensa/stdatomic_s32c1i.c"\s+'
                     r'PROPERTIES COMPILE_FLAGS "-mno-disable-hardware-atomics"\)', libc_cmake)
    component = ROOT / 'components/network_diagnostics'
    with tempfile.TemporaryDirectory(prefix='amity-network-diagnostics-') as temporary:
        directory = Path(temporary)
        (directory / 'sdk.hpp').write_text(SDK)
        for name in ('esp_attr.h', 'esp_event.h', 'esp_log.h', 'esp_netif.h', 'esp_timer.h', 'esp_wifi.h', 'esp_private/wifi.h'):
            path = directory / name;path.parent.mkdir(parents=True, exist_ok=True);path.write_text('#include "sdk.hpp"\n')
        for target_model in (False, True):
            (directory / 'sdkconfig.h').write_text('#pragma once\n#define CONFIG_STDATOMIC_S32C1I_SPIRAM_WORKAROUND ' + str(int(target_model)) +
                                                 ('\n#define __XTENSA__ 1\n' if target_model else '\n'))
            model = ''
            if target_model:
                for name, content in {
                    'freertos/FreeRTOS.h': '#pragma once\n#define portENTER_CRITICAL_SAFE(value) model_enter_critical()\n#define portEXIT_CRITICAL_SAFE(value) model_exit_critical()\n',
                    'soc/soc_caps.h': '#pragma once\n#define SOC_CPU_CORES_NUM 2\n#define SOC_EXTRAM_DATA_LOW model_external_low\n#define SOC_EXTRAM_DATA_HIGH model_external_high\n',
                    'xtensa/config/core-isa.h': '#pragma once\n#define XCHAL_HAVE_S32C1I 1\n',
                }.items():
                    path = directory / name;path.parent.mkdir(parents=True, exist_ok=True);path.write_text(content)
                model = TARGET_ATOMICS + '\n#include "' + str(IDF / 'components/esp_libc/priv_include/esp_stdatomic.h') + '"\n' + TARGET_ATOMICS_AFTER_HEADER
                model += '\n#define NETWORK_TARGET_MODEL 1\n#define std target_std\n'
            source = directory / 'test.cpp'
            source.write_text(PREFIX + model + '\n#define malloc guarded_malloc\n#define calloc guarded_calloc\n'
                              '#define realloc guarded_realloc\n#define free guarded_free\n#include "' +
                              str(component / 'network_diagnostics.cpp') +
                              '"\n#undef malloc\n#undef calloc\n#undef realloc\n#undef free\n' + ('#undef std\n' if target_model else '') + SUITE)
            binary = directory / 'test'
            subprocess.run(shlex.split(os.environ.get('CXX', 'c++')) + [
                '-std=c++17', '-Wall', '-Wextra', '-Werror', '-g', '-fsanitize=address,undefined',
                '-I' + str(directory), '-I' + str(component / 'include'), str(source), '-o', str(binary)], check=True)
            for case in ('paths', 'lifecycle', 'eager', 'race', 'race-ok', 'stop', 'event-error', 'registration-error', 'wrap', 'console'):
                subprocess.run([str(binary), case], check=True)


if __name__ == '__main__':
    main()
