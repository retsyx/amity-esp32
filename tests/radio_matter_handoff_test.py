#!/usr/bin/env python3
"""Actual radio-owner handoff function plus real arbiter; SDK/GAP are seams."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
from consortium_gatt_callback_test import function

ROOT = Path(__file__).resolve().parents[1]
PREFIX = r'''
#include "amity/consortium/radio.hpp"
#include <cassert>
#include <cstdint>
#include <functional>
#include <iostream>
#include <stdexcept>
using namespace amity::consortium;
using esp_err_t=int;
constexpr int ESP_OK=0,ESP_ERR_INVALID_STATE=1,ESP_ERR_TIMEOUT=2;
constexpr unsigned portMAX_DELAY=0xffffffff;
RadioArbiter arbiter;
void* mutex=reinterpret_cast<void*>(1);
bool matter_active=false,fail_safe=false,window_open=false,advertising=false;
unsigned ble_connections=0,chip_depth=0,radio_depth=0,closes=0,pauses=0,delays=0;
uint64_t clock_ms=0,advert_stop_at=0;
std::function<void()> on_close,on_delay;
class Lock {public:Lock(){++radio_depth;}~Lock(){--radio_depth;}};
uint64_t now(){return clock_ms;}
namespace esp_matter::lock {
class ScopedChipStackLock {public:explicit ScopedChipStackLock(unsigned){assert(radio_depth==0);++chip_depth;}~ScopedChipStackLock(){--chip_depth;}};
}
namespace chip {
struct FailSafe {bool IsFailSafeArmed(){assert(chip_depth);return fail_safe;}};
struct Window {
 bool IsCommissioningWindowOpen(){assert(chip_depth);return window_open;}
 void CloseCommissioningWindow(){assert(chip_depth&&radio_depth==0);++closes;window_open=false;if(!advert_stop_at)advertising=false;if(on_close)on_close();}
};
struct Server {
 static Server& GetInstance(){static Server value;return value;}
 FailSafe& GetFailSafeContext(){static FailSafe value;return value;}
 Window& GetCommissioningWindowManager(){static Window value;return value;}
};
namespace DeviceLayer {struct Connectivity {uint16_t NumBLEConnections(){assert(chip_depth);return ble_connections;}};Connectivity& ConnectivityMgr(){static Connectivity value;return value;}}
}
bool ble_gap_adv_active(){assert(chip_depth==0);return advertising;}
esp_err_t ble_remote_pause_background(bool paused){assert(chip_depth==0&&paused);++pauses;return ESP_OK;}
unsigned pdMS_TO_TICKS(unsigned value){return value;}
void vTaskDelay(unsigned ms){assert(chip_depth==0&&radio_depth==0);clock_ms+=ms;++delays;if(advert_stop_at&&clock_ms>=advert_stop_at)advertising=false;if(on_delay)on_delay();}
void release(uint64_t);
'''

SUITE = r'''
void check(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
void reset(){arbiter=RadioArbiter{};matter_active=fail_safe=window_open=advertising=false;ble_connections=chip_depth=radio_depth=closes=pauses=delays=0;clock_ms=advert_stop_at=0;on_close={};on_delay={};}
void idle_matter(){check(arbiter.external_matter_will_open(now(),300000,false)==RadioResult::granted,"could not open idle Matter");window_open=advertising=true;}
void idle_handoff(){
 for(bool inviter:{false,true}){
  reset();idle_matter();const auto previous=arbiter.reservation();advert_stop_at=300;uint64_t admitted=99;
  check(reserve_consortium(inviter,admitted)==ESP_OK,"idle Matter did not hand off");
  auto current=arbiter.reservation();check(closes==1&&!window_open&&!advertising,"SDK idle window not closed and drained");
  check(current.owner==(inviter?RadioMode::consortium_inviter:RadioMode::consortium_candidate)&&current.generation==admitted&&admitted>previous.generation,"wrong requested radio owner");
  check(clock_ms==300&&delays==3,"handoff did not wait for asynchronous advertisement stop");
  matter_closed();check(arbiter.reservation().generation==admitted&&arbiter.reservation().owner==current.owner,"queued old Matter close released new owner");
 }
}
void active_denied(){
 for(int active=0;active<3;++active){
  reset();idle_matter();if(active==0)matter_connection(true);if(active==1)ble_connections=1;if(active==2)fail_safe=true;
  const auto previous=arbiter.reservation();uint64_t admitted=99;
  check(reserve_consortium(false,admitted)==ESP_ERR_INVALID_STATE,"active Matter was interrupted");
  check(admitted==0&&closes==0&&window_open&&advertising&&arbiter.reservation().generation==previous.generation,"denied handoff changed radio/window");
 }
}
void other_busy(){
 check(arbiter.request(RadioMode::remote_pairing,now(),30000,false)==RadioResult::granted,"remote did not reserve");
 auto previous=arbiter.reservation();uint64_t admitted=99;
 check(reserve_consortium(true,admitted)==ESP_ERR_INVALID_STATE&&admitted==0&&closes==0,"busy unrelated owner was preempted");
 check(arbiter.reservation().generation==previous.generation&&arbiter.reservation().owner==RadioMode::remote_pairing,"busy owner state changed");
}
void replace_owner(){auto current=arbiter.reservation();arbiter.release(current.generation,now());current=arbiter.reservation();arbiter.cleanup_complete(current.generation,now());check(arbiter.request(RadioMode::remote_pairing,now(),30000,false)==RadioResult::granted,"replacement owner failed");}
void stale_close(){
 idle_matter();on_close=replace_owner;uint64_t admitted=99;
 check(reserve_consortium(false,admitted)==ESP_ERR_INVALID_STATE&&admitted==0,"stale close reservation admitted");
 check(arbiter.reservation().owner==RadioMode::remote_pairing,"handoff released replacement generation");
}
void stale_cleanup(){
 idle_matter();advert_stop_at=300;on_delay=[](){on_delay={};replace_owner();};uint64_t admitted=99;
 check(reserve_consortium(false,admitted)==ESP_ERR_INVALID_STATE&&admitted==0,"stale cleanup reservation admitted");
 check(arbiter.reservation().owner==RadioMode::remote_pairing,"cleanup released replacement generation");
}
void bounded_timeout(){
 idle_matter();advert_stop_at=6000;uint64_t admitted=99;
 check(reserve_consortium(false,admitted)==ESP_ERR_TIMEOUT&&admitted==0,"stuck advertiser did not fail bounded handoff");
 check(delays==50&&clock_ms==5000&&arbiter.reservation().owner!=RadioMode::consortium_candidate,"cancellation budget changed or request bypassed cleanup");
}
void ordinary(){uint64_t admitted=99;check(reserve_consortium(false,admitted)==ESP_OK&&admitted&&closes==0&&delays==0&&arbiter.reservation().owner==RadioMode::consortium_candidate,"ordinary existing admission changed");}
int main(){struct Test{const char* name;void(*run)();};unsigned failed=0;
 for(auto test:{Test{"idle Matter hands off to candidate and inviter",idle_handoff},Test{"active BLE/PASE/fail-safe denied",active_denied},Test{"other busy owner unchanged",other_busy},Test{"stale close generation fenced",stale_close},Test{"stale cleanup generation fenced",stale_cleanup},Test{"shared five-second cleanup bound",bounded_timeout},Test{"ordinary admission preserved",ordinary}}){reset();try{test.run();std::cout<<"PASS "<<test.name<<'\n';}catch(const std::exception& error){++failed;std::cerr<<"FAIL "<<test.name<<": "<<error.what()<<'\n';}}
 return failed?1:0;
}
'''


def main():
    source = (ROOT / 'components/radio_control/radio_control.cpp').read_text()
    actual = ''.join(function(source, declaration) for declaration in (
        'esp_err_t finish_cancellation(uint64_t generation)',
        'void matter_closed()', 'void matter_connection(bool active)',
        'esp_err_t reserve_consortium(bool inviter,uint64_t& generation)',
        'void release(uint64_t generation)'))
    with tempfile.TemporaryDirectory(prefix='amity-radio-matter-handoff-') as temporary:
        cpp = Path(temporary) / 'actual.cpp'
        binary = Path(temporary) / 'actual'
        cpp.write_text(PREFIX + actual + SUITE)
        subprocess.run(shlex.split(os.environ.get('CXX', 'c++')) + [
            '-std=c++17', '-Wall', '-Wextra', '-Werror', '-g', '-fsanitize=address,undefined',
            '-I' + str(ROOT / 'components/consortium/include'),
            str(ROOT / 'components/consortium/radio.cpp'), str(cpp), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
