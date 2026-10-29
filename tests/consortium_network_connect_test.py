#!/usr/bin/env python3
"""Exercise actual admission Wi-Fi staging with association before delayed DHCP.

Only the CHIP driver, clock, network interface and FreeRTOS seams are faked.
The production connection callback and stage_connect body are compiled unchanged.
No hardware, credentials, or networking are used.
"""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

from consortium_gatt_callback_test import function


PREFIX = r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
using esp_err_t=int;using TickType_t=unsigned;
constexpr int ESP_OK=0,ESP_FAIL=-1,ESP_ERR_INVALID_ARG=1,ESP_ERR_INVALID_STATE=2,ESP_ERR_TIMEOUT=3,CHIP_NO_ERROR=0,pdTRUE=1;
constexpr uint64_t never=std::numeric_limits<uint64_t>::max();
constexpr unsigned tick_ms=10;
struct Semaphore{bool available;};using SemaphoreHandle_t=Semaphore*;
Semaphore transaction_value{true},completed_value{false};
SemaphoreHandle_t transaction=&transaction_value,completed=&completed_value;
std::atomic_bool pending{false};bool staged=false,chip_thread=false;
uint64_t clock_ms=0,ip_at=never,association_delay=5000,association_at=never;
unsigned connects=0,reverts=0,delays=0,driver_calls=0,admission_ms=0;
uint64_t reverted_at=never;
bool association_ok=true,remove_ok=true,add_ok=true;
namespace chip {
struct CharSpan{};
struct MutableCharSpan{template<size_t N>explicit MutableCharSpan(char(&)[N]){}};
struct ByteSpan{ByteSpan(const uint8_t*,size_t){}};
}
namespace nc {
enum class Status{kSuccess,kFailure};
namespace Internal {struct WirelessDriver {struct ConnectCallback {
 virtual ~ConnectCallback()=default;
 virtual void OnResult(Status,chip::CharSpan,int32_t)=0;
};};}
struct ESPWiFiDriver {
 static ESPWiFiDriver&GetInstance(){static ESPWiFiDriver instance;return instance;}
 Status AddOrUpdateNetwork(chip::ByteSpan,chip::ByteSpan,chip::MutableCharSpan&,uint8_t& index){assert(chip_thread);index=0;return add_ok?Status::kSuccess:Status::kFailure;}
 void ConnectNetwork(chip::ByteSpan,Internal::WirelessDriver::ConnectCallback*);
 int RevertConfiguration(){assert(chip_thread);++reverts;reverted_at=clock_ms;return CHIP_NO_ERROR;}
};
}
nc::Internal::WirelessDriver::ConnectCallback*association_callback=nullptr;
int64_t esp_timer_get_time(){return int64_t(clock_ms*1000);}
unsigned pdMS_TO_TICKS(uint64_t ms){return unsigned(ms/tick_ms);}
void xSemaphoreGive(SemaphoreHandle_t semaphore){semaphore->available=true;}
int xSemaphoreTake(SemaphoreHandle_t semaphore,unsigned ticks){
 if(!semaphore->available&&ticks){
  assert(!chip_thread&&semaphore==completed);
  const auto end=clock_ms+uint64_t(ticks)*tick_ms;
  if(association_callback&&association_at<=end){
   clock_ms=association_at;auto* callback=association_callback;association_callback=nullptr;
   chip_thread=true;callback->OnResult(association_ok?nc::Status::kSuccess:nc::Status::kFailure,{},0);chip_thread=false;
  }else clock_ms=end;
 }
 if(!semaphore->available)return 0;
 semaphore->available=false;return pdTRUE;
}
void vTaskDelay(unsigned ticks){assert(!chip_thread&&ticks>0&&ticks<=100/tick_ms);++delays;clock_ms+=uint64_t(ticks)*tick_ms;}
bool usable_ip(){return clock_ms>=ip_at;}
bool remove_other(nc::ESPWiFiDriver&,const std::string&){assert(chip_thread);return remove_ok;}
esp_err_t driver_call(const std::function<esp_err_t()>& call){
 assert(!chip_thread);if(++driver_calls==1)clock_ms+=admission_ms;
 chip_thread=true;const auto result=call();chip_thread=false;return result;
}
void nc::ESPWiFiDriver::ConnectNetwork(chip::ByteSpan,Internal::WirelessDriver::ConnectCallback* callback){
 assert(chip_thread);++connects;association_callback=callback;
 association_at=association_delay==never?never:clock_ms+association_delay;
}
'''

SUITE = r'''
void check(bool condition,const char*message){if(!condition)throw std::runtime_error(message);}
void reset(){
 transaction_value.available=true;completed_value.available=false;pending=false;staged=chip_thread=false;
 clock_ms=0;ip_at=association_at=never;association_delay=5000;
 connects=reverts=delays=driver_calls=admission_ms=0;reverted_at=never;
 association_ok=remove_ok=add_ok=true;association_callback=nullptr;callback.success=false;
}
void delayed_address(){
 for(const auto ready:{uint64_t(6000),uint64_t(34900)}){
  reset();ip_at=ready;
  check(stage_connect("unit-network","unit-password")==ESP_OK,"association success was rolled back before DHCP completed");
  check(staged&&!pending&&!reverts&&connects==1,"successful DHCP staging retried or rolled back");
  check(clock_ms>=ready&&clock_ms<35000&&delays>0,"DHCP readiness did not remain inside the original budget");
  check(transaction_value.available,"successful staging kept transaction locked");
 }
}
void immediate_address(){
 ip_at=5000;
 check(stage_connect("unit-network","")==ESP_OK&&staged&&!delays&&!reverts,"already available address did not complete immediately");
}
void failed_association(){
 association_ok=false;
 check(stage_connect("unit-network","unit-password")==ESP_ERR_TIMEOUT,"failed callback was treated as usable network");
 check(reverts==1&&reverted_at==5000&&!delays&&!staged&&transaction_value.available,"failed callback waited for DHCP or skipped rollback");
}
void absent_callback(){
 association_delay=never;
 check(stage_connect("unit-network","unit-password")==ESP_ERR_TIMEOUT,"missing callback did not time out");
 check(reverts==1&&reverted_at==35000&&!delays&&!staged,"callback timeout changed the 35-second budget");
}
void address_deadline(){
 for(const auto ready:{never,uint64_t(35000),uint64_t(35100)}){
  reset();ip_at=ready;
  check(stage_connect("unit-network","unit-password")==ESP_ERR_TIMEOUT,"address after deadline was accepted");
  check(reverts==1&&reverted_at==35000&&connects==1&&!staged&&transaction_value.available,"DHCP wait extended the callback deadline or retried");
 }
}
void single_budget(){
 admission_ms=3000;ip_at=36000;
 check(stage_connect("unit-network","unit-password")==ESP_ERR_TIMEOUT,"DHCP was given an additional timeout after association");
 check(association_at==8000&&reverted_at==35000&&connects==1,"admission/callback/DHCP did not share one total budget");
}
void admission_failure(){
 for(const bool remove:{false,true}){
  reset();remove_ok=remove;add_ok=false;
  check(stage_connect("unit-network","unit-password")==ESP_ERR_INVALID_STATE,"driver admission failure was accepted");
  check(!pending&&!staged&&!connects&&!reverts&&!delays&&transaction_value.available,"admission failure leaked state or began connection work");
 }
}
int main(){
 struct Test{const char*name;void(*run)();};unsigned failed=0;
 for(const auto test:{Test{"association then delayed IPv4 stages without early rollback",delayed_address},Test{"association with usable IPv4 completes immediately",immediate_address},Test{"association failure rolls back immediately",failed_association},Test{"missing association callback respects deadline",absent_callback},Test{"missing or late DHCP rolls back at original deadline",address_deadline},Test{"admission, association and DHCP share one budget",single_budget},Test{"driver admission failure releases transaction without connection",admission_failure}}){
  reset();try{test.run();std::cout<<"PASS "<<test.name<<'\n';}catch(const std::exception&error){++failed;std::cerr<<"FAIL "<<test.name<<": "<<error.what()<<'\n';}
 }
 return failed?1:0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[1] /
                        'components/amity_matter/consortium_network.cpp')
    source = parser.parse_args().source.read_text()
    callback = function(source, 'class Callback final:') + ' callback;\n'
    actual = function(source, 'esp_err_t stage_connect(')
    with tempfile.TemporaryDirectory(prefix='amity-network-connect-') as directory:
        base = Path(directory)
        cpp, binary = base / 'test.cpp', base / 'test'
        cpp.write_text(PREFIX + callback + actual + SUITE)
        subprocess.run(shlex.split(os.environ.get('CXX', 'c++')) + [
            '-std=c++17', '-Wall', '-Wextra', '-Werror', '-g', '-fsanitize=address,undefined',
            str(cpp), '-o', str(binary)], check=True)
        return subprocess.run([str(binary)], check=False).returncode


if __name__ == '__main__':
    raise SystemExit(main())
