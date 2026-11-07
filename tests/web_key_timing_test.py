#!/usr/bin/env python3
"""Execute HTTP-key timing against a controlled clock and blocked responses.

The actual production scope is extracted unchanged. Authentication, body reads,
key admission and response writes advance the fake clock independently so a
slow stage remains distinguishable from an intentional hold. No DUT is used.
"""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

from consortium_gatt_callback_test import function

ROOT = Path(__file__).resolve().parents[1]

PREFIX = r'''
#include <cassert>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>
using esp_err_t=int;
constexpr int ESP_OK=0,ESP_FAIL=-1,ESP_ERR_NO_MEM=5,ESP_LOG_WARN=2,ESP_LOG_INFO=3;
int64_t clock_us=0;
unsigned clock_reads=0;
int64_t esp_timer_get_time(){++clock_reads;return clock_us;}
struct KeyRequest {uint8_t key=0;uint16_t duration_ms=0;};
struct Log {int level;std::string tag,text;};
std::vector<Log> logs;
void log(int level,const char* tag,const char* format,...) {
    char text[512];va_list args;va_start(args,format);
    const auto size=std::vsnprintf(text,sizeof(text),format,args);va_end(args);
    assert(size>=0&&size<static_cast<int>(sizeof(text)));
    logs.push_back({level,tag,text});
}
#define ESP_LOG_LEVEL(...) log(__VA_ARGS__)
'''

SUITE = r'''
int response(unsigned delay_us,int result) {clock_us+=delay_us;return result;}
int run(uint16_t hold_ms,unsigned guards_us,unsigned body_us,unsigned prepare_us,
        unsigned submit_overhead_us,unsigned response_us,int result,int write_result=ESP_OK) {
    KeyRequestTiming timing(true);
    clock_us+=guards_us;timing.guards_complete();
    clock_us+=body_us;timing.body_complete();
    clock_us+=prepare_us;timing.submitting(KeyRequest{65,hold_ms});
    if(hold_ms)timing.holding(hold_ms);
    clock_us+=static_cast<int64_t>(hold_ms)*1000+submit_overhead_us;timing.submitted(result);
    timing.responding();return timing.finish_response(response(response_us,write_result));
}
void expect(const char* text,int level) {
    assert(logs.size()==1&&logs.back().tag=="web_admin");
    assert(logs.back().level==level&&logs.back().text==text);logs.clear();
}
struct Control {
    bool setup=false,admit=true;unsigned presses=0,releases=0;
    uint32_t remote_activity_generation(){return 7;}
    uint32_t activity_selection(){return 8;}
    uint32_t remote_input_epoch(){return 9;}
    bool activity_setup_in_progress(){clock_us+=150000;return setup;}
    bool press_remote_key(uint8_t){return true;}
    bool press_timed_remote_key(uint8_t,uint32_t,uint32_t,uint32_t){++presses;return admit;}
    bool release_timed_remote_key(uint8_t,uint32_t,uint32_t,uint32_t){++releases;return true;}
};
void skipped_hold(bool setup) {
    KeyRequestTiming timing(true);
    clock_us+=1000;timing.guards_complete();clock_us+=1000;timing.body_complete();
    const KeyRequest request{65,5000};clock_us+=1000;timing.submitting(request);
    Control control;control.setup=setup;control.admit=false;
    const bool result=submit_key_request(control,request,[&timing](uint16_t duration){
        timing.holding(duration);clock_us+=static_cast<int64_t>(duration)*1000;
    });
    assert(result==setup&&control.presses==(setup?0u:1u)&&control.releases==0);
    timing.submitted(result?ESP_OK:ESP_ERR_NO_MEM);timing.responding();
    timing.finish_response(response(1000,ESP_OK));
}
int main() {
    // Other endpoints must not sample the clock or produce key logs.
    {
        KeyRequestTiming unrelated(false);unrelated.guards_complete();unrelated.body_complete();
        unrelated.submitting(KeyRequest{65,0});unrelated.submitted(ESP_OK);unrelated.responding();
    }
    assert(!clock_reads&&logs.empty());
    // Rejected credentials / invalid JSON never supply a parsed key to log.
    {KeyRequestTiming rejected(true);clock_us+=1000;}
    assert(logs.empty());
    // A legitimate zero clock origin still reports a valid tap after response.
    clock_us=0;assert(run(0,2000,1300,200,1500,3000,ESP_OK)==ESP_OK);
    expect("key=65 hold_ms=0 hold_wait_ms=0 guards_us=2000 body_us=1300 prepare_us=200 submit_us=1500 response_us=3000 total_us=8000 submit_result=0 response_result=0",ESP_LOG_INFO);
    // A healthy long hold is not a latency warning. Its intentional duration
    // remains visible in submit time rather than being silently subtracted.
    assert(run(5000,1000,1000,1000,1000,1000,ESP_OK)==ESP_OK);
    expect("key=65 hold_ms=5000 hold_wait_ms=5000 guards_us=1000 body_us=1000 prepare_us=1000 submit_us=5001000 response_us=1000 total_us=5005000 submit_result=0 response_result=0",ESP_LOG_INFO);
    // The 100 ms overhead boundary is independent of requested hold length.
    run(750,1000,1000,1000,96000,1000,ESP_OK);
    expect("key=65 hold_ms=750 hold_wait_ms=750 guards_us=1000 body_us=1000 prepare_us=1000 submit_us=846000 response_us=1000 total_us=850000 submit_result=0 response_result=0",ESP_LOG_INFO);
    run(750,1000,1000,1000,96001,1000,ESP_OK);
    expect("key=65 hold_ms=750 hold_wait_ms=750 guards_us=1000 body_us=1000 prepare_us=1000 submit_us=846001 response_us=1000 total_us=850001 submit_result=0 response_result=0",ESP_LOG_WARN);
    // Expose each potential stall independently, including response writes
    // which block after admission and a failed key admission's unchanged code.
    run(0,1500000,1000,1000,1000,1000,ESP_OK);
    expect("key=65 hold_ms=0 hold_wait_ms=0 guards_us=1500000 body_us=1000 prepare_us=1000 submit_us=1000 response_us=1000 total_us=1504000 submit_result=0 response_result=0",ESP_LOG_WARN);
    run(0,1000,1500000,1000,1000,1000,ESP_OK);
    expect("key=65 hold_ms=0 hold_wait_ms=0 guards_us=1000 body_us=1500000 prepare_us=1000 submit_us=1000 response_us=1000 total_us=1504000 submit_result=0 response_result=0",ESP_LOG_WARN);
    run(0,1000,1000,1000,1500000,1000,ESP_OK);
    expect("key=65 hold_ms=0 hold_wait_ms=0 guards_us=1000 body_us=1000 prepare_us=1000 submit_us=1500000 response_us=1000 total_us=1504000 submit_result=0 response_result=0",ESP_LOG_WARN);
    assert(run(0,1000,1000,1000,1000,1500000,ESP_ERR_NO_MEM,ESP_FAIL)==ESP_FAIL);
    expect("key=65 hold_ms=0 hold_wait_ms=0 guards_us=1000 body_us=1000 prepare_us=1000 submit_us=1000 response_us=1500000 total_us=1504000 submit_result=5 response_result=-1",ESP_LOG_WARN);
    // Execute real submission's early returns: the request can ask for five
    // seconds yet do no hold wait. That cannot excuse an unrelated 150 ms stall.
    skipped_hold(true);
    expect("key=65 hold_ms=5000 hold_wait_ms=0 guards_us=1000 body_us=1000 prepare_us=1000 submit_us=150000 response_us=1000 total_us=154000 submit_result=0 response_result=0",ESP_LOG_WARN);
    skipped_hold(false);
    expect("key=65 hold_ms=5000 hold_wait_ms=0 guards_us=1000 body_us=1000 prepare_us=1000 submit_us=150000 response_us=1000 total_us=154000 submit_result=5 response_result=0",ESP_LOG_WARN);
    std::cout<<"PASS HTTP key stage timing and response scope\n";
}
'''


def main():
    source = (ROOT / 'components/web_admin/web_admin.cpp').read_text()
    actual = function(source, 'struct KeyRequestTiming') + ';\n'
    submission = (ROOT / 'components/web_admin/key_request.hpp').read_text()
    actual += function(submission, 'template<class Control,class Wait>bool submit_key_request(')
    with tempfile.TemporaryDirectory(prefix='amity-web-key-timing-') as directory:
        cpp = Path(directory) / 'test.cpp'
        binary = Path(directory) / 'test'
        cpp.write_text(PREFIX + actual + SUITE)
        subprocess.run(shlex.split(os.environ.get('CXX', 'c++')) + [
            '-std=c++17', '-Wall', '-Wextra', '-Werror', '-g',
            '-fsanitize=address,undefined', str(cpp), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
