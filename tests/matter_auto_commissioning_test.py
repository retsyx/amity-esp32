#!/usr/bin/env python3
"""Execute the real boot commissioning decision and require its app_main wiring.

Only Matter/radio/role service boundaries and delays are instrumented. This test
never opens a real commissioning window, changes a fabric or touches hardware.
"""
from pathlib import Path
import os
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'main/main.cpp'


def masked_cpp(source):
    pattern = r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\''
    return re.sub(pattern, lambda match: ''.join('\n' if char == '\n' else ' ' for char in match.group()), source)


def closing_brace(masked, opening):
    depth = 0
    for index in range(opening, len(masked)):
        if masked[index] == '{':
            depth += 1
        elif masked[index] == '}':
            depth -= 1
            if not depth:
                return index
    raise AssertionError('Unclosed production C++ block')


def production_parts(source):
    masked = masked_cpp(source)
    def function(signature):
        start = source.index(signature)
        opening = masked.index('{', start)
        return source[start:closing_brace(masked, opening) + 1]
    helpers = '\n'.join(function(signature) for signature in (
        'esp_err_t open_boot_matter_setup()', 'void start_boot_matter_setup()',
        'void setup_button('))
    main = function('extern "C" void app_main()')
    main_masked = masked_cpp(main)
    call = 'start_boot_matter_setup();'
    assert main_masked.count(call) == 1, 'app_main must open eligible Matter setup automatically once per boot'
    position = main_masked.index(call)
    assert main.index('system_services::confirm_firmware()') < position, 'Open commissioning only after healthy local startup'
    assert main.index('const auto migrated=') < position, 'Retain the migration/health boundary before opening setup'
    assert position < main.index('xTaskCreate(await_original_task,'), 'Open setup before waiting for a first enrollment'
    stack = []
    for index, char in enumerate(main_masked[:position]):
        if char == '{':
            stack.append(index)
        elif char == '}':
            stack.pop()
    assert len(stack) == 2, 'Automatic setup must be at app_main top level, independent of a button/GPIO/IP branch'
    preprocessor = []
    for line in main[:position].splitlines():
        if re.match(r'\s*#\s*(if|ifdef|ifndef)\b', line):
            preprocessor.append(line)
        elif re.match(r'\s*#\s*endif\b', line):
            preprocessor.pop()
    assert not preprocessor, 'Automatic setup must not depend on a build-time button or board conditional'
    opening = stack[-1]
    guard = re.search(r'\bif\s*\([^{};]*\)\s*$', main_masked[:opening])
    assert guard, 'Execute the actual guarded app_main boot invocation, not an unused helper'
    boot = main[guard.start():closing_brace(main_masked, opening) + 1]
    return helpers, boot


PREFIX = r'''
#include <cassert>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <vector>
using esp_err_t=int;
constexpr int ESP_OK=0,ESP_ERR_INVALID_STATE=1,ESP_FAIL=4;
#define pdMS_TO_TICKS(value) (value)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
bool firmware_healthy=false,eligible_unclaimed=true,station_has_ip=false;
unsigned matter_fabrics=0,status_calls=0,open_calls=0,windows_opened=0,delays=0,delayed_ms=0,enrollment_windows=0,radio_reservations=0;
unsigned status_failures=0,open_failures=0;
int status_result=ESP_OK,open_result=ESP_OK;bool window_open=false;
std::function<void()>on_delay;
namespace system_services{bool firmware_ready(){return firmware_healthy;}}
namespace amity::consortium {
struct RuntimeStatus {bool initialized=true,member=false,coordinator=false,authority_ready=false;std::string phase="awaiting_setup";}role;
RuntimeStatus runtime_status(){return role;}
bool runtime_original_setup_eligible(){return eligible_unclaimed;}
int runtime_open_enrollment(uint64_t){++enrollment_windows;return ESP_OK;}
}
namespace amity::radio {
int reserve_consortium(bool,uint64_t& generation){++radio_reservations;generation=1;return ESP_OK;}
void release(uint64_t){}
}
struct Configuration{std::vector<int>activities,components;};
struct Hub {
    std::shared_ptr<Configuration> configuration;std::vector<int>legacy_activities;
    auto active_configuration(){return configuration;}
    struct Snapshot{std::vector<int>activities;};Snapshot snapshot(){return {legacy_activities};}
}hub;
struct amity_matter_status_t {bool started=true,commissioning_window=false;unsigned fabric_count=0;};
int amity_matter_get_status(amity_matter_status_t* status){
    ++status_calls;if(status_failures){--status_failures;return ESP_FAIL;}
    if(status_result!=ESP_OK)return status_result;
    status->fabric_count=matter_fabrics;status->commissioning_window=window_open;return ESP_OK;
}
int amity_matter_open_commissioning(unsigned seconds){
    assert(seconds==300);++open_calls;if(open_failures){--open_failures;return ESP_FAIL;}
    if(open_result!=ESP_OK)return open_result;
    if(!window_open){window_open=true;++windows_opened;}return ESP_OK;
}
void vTaskDelay(unsigned ticks){++delays;assert(delays<=50&&ticks==100);delayed_ms+=ticks;if(on_delay)on_delay();}
'''

SUITE = r'''
void reset(){
    amity::consortium::role={};firmware_healthy=false;eligible_unclaimed=true;station_has_ip=false;
    matter_fabrics=status_calls=open_calls=windows_opened=delays=delayed_ms=enrollment_windows=radio_reservations=0;
    status_failures=open_failures=0;status_result=open_result=ESP_OK;window_open=false;on_delay={};
    hub.configuration.reset();hub.legacy_activities.clear();
}
void healthy(){reset();firmware_healthy=true;}
void no_setup(){assert(!open_calls&&!windows_opened&&!enrollment_windows&&!radio_reservations);}
int main(){using namespace amity::consortium;
    // This executes the actual app_main guard and call; a helper left unused or
    // moved behind a physical button cannot satisfy these startup assertions.
    healthy();actual_boot_tail();assert(!station_has_ip&&window_open&&windows_opened==1&&!delays&&!enrollment_windows&&!radio_reservations);
    healthy();hub.configuration=std::make_shared<Configuration>();hub.configuration->activities.push_back(1);hub.configuration->components.push_back(2);actual_boot_tail();assert(windows_opened==1);
    healthy();hub.legacy_activities.push_back(1);actual_boot_tail();assert(windows_opened==1);
    healthy();role.coordinator=role.authority_ready=true;role.phase="coordinator";eligible_unclaimed=false;hub.configuration=std::make_shared<Configuration>();hub.configuration->activities.push_back(1);actual_boot_tail();assert(windows_opened==1&&role.coordinator&&role.authority_ready);
    for(unsigned fabrics:{1u,2u,5u})for(bool original:{false,true}){
        healthy();matter_fabrics=fabrics;if(original){role.coordinator=role.authority_ready=true;eligible_unclaimed=false;}
        actual_boot_tail();no_setup();assert(matter_fabrics==fabrics&&!delays);
    }
    // Headless/pending/retired/recovery roles cannot acquire a Matter window.
    for(unsigned state=0;state<7;++state){
        healthy();eligible_unclaimed=false;
        if(state==0){role.member=true;role.phase="member";}
        if(state==1){role.member=true;role.phase="prepared";}
        if(state==2)role.phase="retired";
        if(state==3)role.phase="recovery_required";
        if(state==4)role.initialized=false;
        if(state==5){role.coordinator=true;role.authority_ready=false;}
        if(state==6)role.phase="enrollment_open";
        actual_boot_tail();no_setup();assert(!status_calls&&!delays);
    }
    reset();actual_boot_tail();no_setup();assert(!status_calls);
    healthy();actual_boot_tail(ESP_FAIL,ESP_OK);no_setup();assert(!status_calls);
    healthy();actual_boot_tail(ESP_OK,ESP_FAIL);no_setup();assert(!status_calls);
    healthy();status_result=ESP_ERR_INVALID_STATE;actual_boot_tail();no_setup();assert(status_calls==1&&!delays);
    // A busy shared host is retried only during this bounded startup call.
    healthy();open_failures=2;actual_boot_tail();assert(open_calls==3&&windows_opened==1&&delays==2&&delayed_ms==200);
    healthy();status_failures=2;actual_boot_tail();assert(status_calls==3&&open_calls==1&&windows_opened==1&&delays==2);
    healthy();open_result=ESP_FAIL;actual_boot_tail();assert(open_calls>1&&open_calls<=50&&!windows_opened&&delays==open_calls-1&&delayed_ms<=5000);
    healthy();status_result=ESP_FAIL;actual_boot_tail();no_setup();assert(status_calls>1&&status_calls<=50&&delays==status_calls-1&&delayed_ms<=5000);
    healthy();open_result=ESP_ERR_INVALID_STATE;actual_boot_tail();assert(open_calls==1&&!windows_opened&&!delays);
    // Recheck live fabrics and authority on every retry, never a saved latch.
    healthy();open_result=ESP_FAIL;on_delay=[](){matter_fabrics=1;};actual_boot_tail();assert(open_calls==1&&!windows_opened&&delays==1&&matter_fabrics==1);
    healthy();open_result=ESP_FAIL;on_delay=[](){role.member=true;role.phase="prepared";eligible_unclaimed=false;};actual_boot_tail();assert(open_calls==1&&!windows_opened&&delays==1);
    // After the automatic window expires, a physical short press still offers
    // the existing optional unclaimed-device setup path. It is not mandatory.
    healthy();actual_boot_tail();assert(windows_opened==1);window_open=false;setup_button(false,nullptr);assert(windows_opened==2&&!enrollment_windows&&!radio_reservations);
    matter_fabrics=1;window_open=false;setup_button(false,nullptr);assert(windows_opened==2&&!window_open);
    std::cout<<"PASS automatic Matter boot setup: actual app_main invocation, fresh/configured/proven-original no-fabric boots, no button/IP gate, fabric/headless/recovery safety, bounded live-state retries and optional physical reopening\n";
}
'''


def main():
    source = SOURCE.read_text()
    helpers, boot = production_parts(source)
    fixture = PREFIX + helpers + '\nvoid actual_boot_tail(int group_start=ESP_OK,int migrated=ESP_OK){\n' + boot + '\n}\n' + SUITE
    with tempfile.TemporaryDirectory(prefix='matter-boot-') as directory:
        base = Path(directory)
        (base / 'test.cpp').write_text(fixture)
        subprocess.run(shlex.split(os.environ.get('CXX', 'c++')) + ['-std=c++17', '-Wall', '-Wextra', '-Werror', '-g', '-fsanitize=address,undefined', str(base/'test.cpp'), '-o', str(base/'test')], check=True)
        subprocess.run([str(base/'test')], check=True)

if __name__ == '__main__':
    main()
