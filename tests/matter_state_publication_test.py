#!/usr/bin/env python3
"""Run actual Matter publication against a blocked CHIP task and bounded queue."""

import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

from consortium_gatt_callback_test import function


ROOT = Path(__file__).resolve().parents[1]
PREFIX = r'''
#include "amity_matter.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <iostream>
#include <mutex>
#include <thread>
#include <tuple>
#include <vector>
using CHIP_ERROR=int;
constexpr CHIP_ERROR CHIP_NO_ERROR=0,CHIP_ERROR_BUSY=19;
[[maybe_unused]] constexpr unsigned portMAX_DELAY=UINT32_MAX;
constexpr uint16_t player_endpoint=1,speaker_endpoint=2;
std::atomic<bool> started{false},controller_enabled{true};
amity_matter_state_t state{};
std::recursive_mutex chip_mutex;
thread_local bool on_chip_task=false;
std::atomic<unsigned> chip_lock_entries{0};
namespace esp_matter::lock {
class ScopedChipStackLock {
    std::unique_lock<std::recursive_mutex> guard;
public:
    explicit ScopedChipStackLock(unsigned):guard(chip_mutex){++chip_lock_entries;}
};
}
namespace OnOff {constexpr uint32_t Id=6;namespace Attributes {namespace OnOff {constexpr uint32_t Id=0;}}}
namespace MediaInput {constexpr uint32_t Id=0x506;namespace Attributes {namespace CurrentInput {constexpr uint32_t Id=1;}}}
namespace LevelControl {constexpr uint32_t Id=8;namespace Attributes {namespace CurrentLevel {constexpr uint32_t Id=0;}}}
std::vector<std::tuple<uint16_t,uint32_t,uint32_t>> reports;
std::function<void()> during_report;
void report(uint16_t endpoint,uint32_t cluster,uint32_t attribute){
    assert((on_chip_task||chip_lock_entries.load())&&started.load()&&controller_enabled.load());
    reports.emplace_back(endpoint,cluster,attribute);
    if(during_report){auto callback=std::move(during_report);during_report={};callback();}
}
using Work=std::pair<void(*)(intptr_t),intptr_t>;
std::mutex queue_mutex;
std::deque<Work> work;
unsigned schedules=0,schedule_failures=0;
bool immediate_callback=false;
std::function<void()> during_schedule;
void run_on_chip(Work item){
    std::lock_guard<std::recursive_mutex> guard(chip_mutex);
    assert(!on_chip_task);on_chip_task=true;item.first(item.second);on_chip_task=false;
}
namespace chip::DeviceLayer {
struct Platform {
    CHIP_ERROR ScheduleWork(void(*callback)(intptr_t),intptr_t argument){
        ++schedules;
        if(during_schedule){auto update=std::move(during_schedule);during_schedule={};update();}
        if(schedule_failures){--schedule_failures;return CHIP_ERROR_BUSY;}
        if(immediate_callback){std::thread worker([=]{run_on_chip({callback,argument});});worker.join();}
        else {std::lock_guard<std::mutex> guard(queue_mutex);work.emplace_back(callback,argument);}
        return CHIP_NO_ERROR;
    }
};
Platform& PlatformMgr(){static Platform value;return value;}
}
size_t queued(){std::lock_guard<std::mutex> guard(queue_mutex);return work.size();}
void run_one(){
    Work item;
    {std::lock_guard<std::mutex> guard(queue_mutex);assert(!work.empty());item=work.front();work.pop_front();}
    run_on_chip(item);
}
'''

SUITE = r'''
int main(){
    amity_matter_state_t initial{false,1,false,false,false,0};
    assert(amity_matter_update_state(&initial)==ESP_OK&&state.current_input==1&&queued()==0&&reports.empty());
    auto invalid=initial;invalid.volume_known=true;invalid.volume=255;
    assert(amity_matter_update_state(nullptr)==ESP_ERR_INVALID_ARG);
    assert(amity_matter_update_state(&invalid)==ESP_ERR_INVALID_ARG&&queued()==0);
    started.store(true);
    amity_matter_state_t next{true,2,true,true,true,10};

    // The real update must return while unrelated CHIP work owns its lock.
    // Release before asserting so the original blocking implementation exits
    // cleanly and fails this behavioral check instead of hanging the suite.
    std::unique_lock<std::recursive_mutex> blocked(chip_mutex);
    auto update=std::async(std::launch::async,[&]{return amity_matter_update_state(&next);});
    const bool admitted=update.wait_for(std::chrono::milliseconds(100))==std::future_status::ready;
    blocked.unlock();const auto admitted_result=update.get();
    assert(admitted&&admitted_result==ESP_OK&&chip_lock_entries.load()==0);
    assert(state.current_input==1&&queued()==1&&reports.empty());

    // Overwrites use one queued callback and publish the final observation.
    next.current_input=3;assert(amity_matter_update_state(&next)==ESP_OK);
    next.current_input=4;next.volume=120;assert(amity_matter_update_state(&next)==ESP_OK);
    assert(queued()==1&&schedules==1&&state.current_input==1);
    run_one();assert(state.current_input==4&&state.volume==120&&reports.size()==4&&queued()==0);
    reports.clear();assert(amity_matter_update_state(&next)==ESP_OK);run_one();assert(reports.empty());

    // A failed admission retains even the final state. Background polling
    // retries it without requiring another activity or input observation.
    next.current_input=5;schedule_failures=1;
    assert(amity_matter_update_state(&next)==ESP_FAIL&&queued()==0&&state.current_input==4);
    assert(amity_matter_flush_state()==ESP_OK&&queued()==1);run_one();
    assert(state.current_input==5&&reports.size()==1);reports.clear();

    // Updates racing a full queue overwrite the retained value, not its dirty
    // marker. The next background poll still publishes the latest observation.
    next.current_input=6;schedule_failures=1;
    during_schedule=[&]{auto newer=next;newer.current_input=7;assert(amity_matter_update_state(&newer)==ESP_OK);};
    assert(amity_matter_update_state(&next)==ESP_FAIL&&queued()==0);
    assert(amity_matter_flush_state()==ESP_OK);run_one();
    assert(state.current_input==7&&reports.size()==1);reports.clear();

    // CHIP may execute work before ScheduleWork returns. The producer cannot
    // hold its mailbox lock across scheduling, reporting or callback entry.
    immediate_callback=true;next.current_input=8;
    assert(amity_matter_update_state(&next)==ESP_OK&&state.current_input==8&&queued()==0);
    immediate_callback=false;reports.clear();
    next.current_input=9;
    during_report=[&]{auto newer=next;newer.current_input=10;assert(amity_matter_update_state(&newer)==ESP_OK);};
    assert(amity_matter_update_state(&next)==ESP_OK);run_one();
    assert(state.current_input==9&&queued()==1);run_one();
    assert(state.current_input==10&&reports.size()==2);reports.clear();

    // Revocation is latched for this boot. Work already queued must not touch
    // stopped endpoint state or publish attributes after controller shutdown.
    next.current_input=11;assert(amity_matter_update_state(&next)==ESP_OK&&queued()==1);
    controller_enabled.store(false);run_one();assert(state.current_input==10&&reports.empty());
    started.store(false);next.current_input=12;
    assert(amity_matter_update_state(&next)==ESP_OK&&amity_matter_flush_state()==ESP_OK);
    assert(state.current_input==10&&queued()==0&&reports.empty()&&chip_lock_entries.load()==0);
    std::cout<<"PASS actual Matter state publication: blocked CHIP never blocks input owner; latest state coalesces, final state survives queue failure/background retry, inline callbacks and concurrent updates remain safe, revocation fences queued publication\n";
}
'''


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--source', type=Path, default=ROOT / 'components/amity_matter/amity_matter.cpp')
    args = parser.parse_args()
    source = args.source.read_text()
    actual = ''
    if 'std::mutex state_publication_mutex;' in source:
        actual += source[source.index('std::mutex state_publication_mutex;'):source.index('amity_matter_input_t inputs[')]
        actual += function(source, 'void publish_state(intptr_t)')
        actual += function(source, 'extern "C" esp_err_t amity_matter_flush_state(')
    else:
        actual += 'extern "C" esp_err_t amity_matter_flush_state(void){return ESP_OK;}\n'
    actual += function(source, 'extern "C" esp_err_t amity_matter_update_state(')
    if args.source == ROOT / 'components/amity_matter/amity_matter.cpp':
        polling = function((ROOT / 'main/main.cpp').read_text(), 'void remote_battery_task(')
        assert polling.index('amity_matter_flush_state();') < polling.index('vTaskDelay(')
    with tempfile.TemporaryDirectory(prefix='amity-matter-publication-') as directory:
        cpp, binary = Path(directory) / 'test.cpp', Path(directory) / 'test'
        cpp.write_text(PREFIX + actual + SUITE)
        subprocess.run(shlex.split(os.environ.get('CXX', 'c++')) + [
            '-std=c++17', '-Wall', '-Wextra', '-Werror', '-g', '-fsanitize=address,undefined', '-pthread',
            '-I' + str(ROOT / 'tests/cec_hub_fakes'),
            '-I' + str(ROOT / 'components/amity_matter/include'),
            str(cpp), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == '__main__':
    main()
