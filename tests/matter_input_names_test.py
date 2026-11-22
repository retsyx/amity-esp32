#!/usr/bin/env python3
"""Publish Matter input names from each applied save, without any web request."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

from consortium_gatt_callback_test import function

ROOT = Path(__file__).resolve().parents[1]
PREFIX = r'''
#include "amity_matter.h"
#include "amity/hub.hpp"
#include "system_configuration.hpp"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <tuple>
#include <vector>
template<class T>T id(unsigned n){char text[33];std::snprintf(text,sizeof(text),"%032x",n);return T::parse(text).value();}
using CHIP_ERROR=int;
constexpr CHIP_ERROR CHIP_NO_ERROR=0;
namespace config_store {
'''
MATTER = r'''
}
constexpr uint16_t player_endpoint=1,speaker_endpoint=2;
std::atomic<bool> started{true},controller_enabled{true};
namespace OnOff {constexpr uint32_t Id=6;namespace Attributes {namespace OnOff {constexpr uint32_t Id=0;}}}
namespace MediaInput {constexpr uint32_t Id=0x507;namespace Attributes {namespace InputList {constexpr uint32_t Id=0;}namespace CurrentInput {constexpr uint32_t Id=1;}}}
namespace LevelControl {constexpr uint32_t Id=8;namespace Attributes {namespace CurrentLevel {constexpr uint32_t Id=0;}}}
std::vector<std::tuple<uint16_t,uint32_t,uint32_t>> reports;
void report(uint16_t endpoint,uint32_t cluster,uint32_t attribute){reports.emplace_back(endpoint,cluster,attribute);}
bool input_list_reported(){return std::count(reports.begin(),reports.end(),std::make_tuple(player_endpoint,MediaInput::Id,MediaInput::Attributes::InputList::Id))==1;}
std::deque<void(*)(intptr_t)> work;
namespace chip::DeviceLayer {
struct Platform {CHIP_ERROR ScheduleWork(void(*callback)(intptr_t),intptr_t){work.push_back(callback);return CHIP_NO_ERROR;}};
Platform& PlatformMgr(){static Platform value;return value;}
}
void run_chip_work(){while(!work.empty()){auto callback=work.front();work.pop_front();callback(0);}}
amity_matter_callbacks_t callbacks{};
amity_matter_state_t state{};
'''
MAIN = r'''
struct Hub {
    std::shared_ptr<config_store::Configuration> config=std::make_shared<config_store::Configuration>();
    std::shared_ptr<const config_store::Configuration> active_configuration()const{return config;}
    std::optional<unsigned> external_activity_index(const amity::Snapshot& snapshot)const{return snapshot.current_activity<0?std::nullopt:std::optional<unsigned>(0);}
} hub;
std::atomic_bool original_interfaces_enabled{true};
'''
SUITE = r'''
std::vector<std::string> names(){
    std::vector<std::string> result;const size_t count=current_inputs();
    for(size_t n=0;n<count;++n)result.emplace_back(listed_inputs[n].name);
    return result;
}
// Apply the saved configuration as the hub does after a successful save.
void apply(amity::Snapshot& snapshot){
    snapshot.applied_config_revision=hub.config->config_revision;snapshot.applied_presentation_revision=hub.config->presentation_revision;
    reports.clear();publish_matter_state(snapshot);run_chip_work();
}
int main(){
    callbacks.inputs=matter_inputs;
    auto& config=*hub.config;
    for(unsigned n=0;n<2;++n){
        config_store::ConfiguredActivity activity;activity.activity_id=id<amity::system::ActivityId>(10+n);
        activity.label="Activity "+std::to_string(n);
        config.activities.push_back(activity);config.external_slots.push_back({static_cast<uint16_t>(2-n),activity.activity_id,false,false,{}});
    }
    amity::Snapshot snapshot;apply(snapshot);
    assert(input_list_reported()&&(names()==std::vector<std::string>{"Activity 1","Activity 0"}));
    assert(listed_inputs[0].index==1&&listed_inputs[1].index==2);
    // An activity editor save renames an activity. Applying it reports the
    // list and every read shows the saved name; no administration request runs.
    config.activities[0].label="Movies";++config.presentation_revision;apply(snapshot);
    assert(input_list_reported()&&(names()==std::vector<std::string>{"Activity 1","Movies"}));
    // Home app names and visibility are saved slot presentation.
    config.external_slots[0].alias="Film";++config.presentation_revision;apply(snapshot);
    assert(input_list_reported()&&(names()==std::vector<std::string>{"Activity 1","Film"}));
    config.external_slots[1].hidden=true;++config.presentation_revision;apply(snapshot);
    assert(input_list_reported()&&(names()==std::vector<std::string>{"Film"}));
    // A deleted activity leaves a tombstone, which is not an input.
    config.external_slots[0].tombstone=true;config.activities.erase(config.activities.begin());++config.config_revision;apply(snapshot);
    assert(input_list_reported()&&names().empty());
    // Playback changes publish state without republishing the input list.
    snapshot.current_activity=0;apply(snapshot);
    assert(!reports.empty()&&!input_list_reported());
    std::cout<<"PASS Matter input names: each applied save reports the input list, reads use saved names, aliases, visibility and tombstones, playback leaves the list alone\n";
}
'''


def main():
    configuration = (ROOT / 'components/config_store/system_configuration.cpp').read_text()
    matter = (ROOT / 'components/amity_matter/amity_matter.cpp').read_text()
    firmware = (ROOT / 'main/main.cpp').read_text()
    code = PREFIX + function(configuration, 'std::vector<ExternalInput> external_inputs(')
    code += MATTER + matter[matter.index('std::mutex state_publication_mutex;'):matter.index('std::atomic<bool> started')]
    code += ''.join(function(matter, declaration) for declaration in (
        'size_t current_inputs()', 'void publish_state(intptr_t)',
        'extern "C" esp_err_t amity_matter_flush_state(', 'extern "C" esp_err_t amity_matter_update_state('))
    code += MAIN + ''.join(function(firmware, declaration) for declaration in (
        'size_t matter_inputs(', 'void publish_matter_state(')) + SUITE
    with tempfile.TemporaryDirectory(prefix='amity-matter-inputs-') as directory:
        cpp, binary = Path(directory) / 'test.cpp', Path(directory) / 'test'
        cpp.write_text(code)
        subprocess.run(shlex.split(os.environ.get('CXX', 'c++')) + [
            '-std=c++17', '-Wall', '-Wextra', '-Werror', '-g', '-fsanitize=address,undefined',
            '-I' + str(ROOT / 'tests/cec_hub_fakes'), '-I' + str(ROOT / 'tests/reset_fakes'),
            '-I' + str(ROOT / 'components/amity_matter/include'), '-I' + str(ROOT / 'components/config_store/include'),
            '-I' + str(ROOT / 'components/amity_system/include'), '-I' + str(ROOT / 'components/amity_hub/include'),
            '-I' + str(ROOT / 'components/cec/include'), str(cpp), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
