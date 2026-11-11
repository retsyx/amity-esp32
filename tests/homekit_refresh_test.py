#!/usr/bin/env python3
"""Run HomeKit's production refresh functions across reads and config changes."""
from pathlib import Path
import re
import subprocess
import tempfile

from consortium_gatt_callback_test import function

ROOT = Path(__file__).resolve().parents[1]
PREFIX = r'''
#include "system_configuration.hpp"
#include "amity/hub.hpp"
#include <array>
#include <cassert>
#include <cstdio>
#include <memory>
template<class T>T id(unsigned n){char text[33];std::snprintf(text,sizeof(text),"%032x",n);return T::parse(text).value();}
unsigned saved_reads=0,input_updates=0,playback_reads=0,battery_reads=0;
namespace config_store {
int read(const char*,std::string&){++saved_reads;return -1;}
}
struct hap_char_t {std::string text;int number=0;};
struct Control {
    std::shared_ptr<config_store::Configuration> config=std::make_shared<config_store::Configuration>();
    amity::PlaybackState state;
    auto active_configuration()const{return config;}
    auto octopus_id()const{return id<amity::system::OctopusId>(1);}
    auto playback_state()const{++playback_reads;return state;}
    std::optional<unsigned> external_activity_index(const amity::PlaybackState& value)const{return value.current_activity<0?std::nullopt:std::optional<unsigned>(0);}
} service;
Control* hub=&service;
std::string input_key(unsigned index){return "hap_input_"+std::to_string(index);}
void update(hap_char_t* value,int number){value->number=number;}
void update_bool(hap_char_t* value,bool flag){value->number=flag;}
void update_string(hap_char_t* value,const std::string& text){++input_updates;value->text=text;}
int save_input(unsigned){return ESP_OK;}
void refresh_remote_battery(){++battery_reads;}
hap_char_t active_value,speaker_value,selected_value,mute_value;
hap_char_t *active=&active_value,*speaker_active=&speaker_value,*active_input=&selected_value,*mute_char=&mute_value;
bool audio_known=false;
'''
SUITE = r'''
int main(){
    std::array<std::array<hap_char_t,5>,16> chars;
    for(unsigned i=0;i<inputs.size();++i){auto& in=inputs[i];in.name=&chars[i][0];in.label=&chars[i][1];in.configured=&chars[i][2];in.visible=&chars[i][3];in.target_visible=&chars[i][4];}
    auto& config=*service.config;config.primary_zone_id=id<amity::system::ZoneId>(2);
    config_store::ConfiguredActivity activity;activity.activity_id=id<amity::system::ActivityId>(3);activity.zone_id=config.primary_zone_id;activity.label=activity.generated_label="Watch television";
    config.activities.push_back(activity);config.external_slots.push_back({0,activity.activity_id,false,false,{}});
    refresh_values();assert(input_updates==32&&saved_reads==16&&chars[0][0].text==activity.label&&chars[0][2].number==1);
    // Repeated per-service accessory reads and idle worker refreshes must not
    // walk/render inputs or reload settings. Live playback still changes.
    service.state.current_activity=0;service.state.audio_status_known=true;service.state.muted=true;
    for(unsigned n=0;n<1000;++n)refresh_values();
    assert(input_updates==32&&saved_reads==16&&playback_reads==1001&&battery_reads==1001);
    assert(active_value.number==1&&selected_value.number==1&&audio_known&&mute_value.number==1);
    ++config.presentation_revision;config.activities[0].label="New name";refresh_values();
    assert(input_updates==64&&saved_reads==16&&chars[0][0].text=="New name");
    inputs[0].alias="Home alias";inputs_dirty=true;refresh_values();
    assert(chars[0][0].text=="Home alias"&&chars[0][1].text=="Home alias");
    inputs[0].hidden=true;inputs_dirty=true;refresh_values();assert(chars[0][3].number==1&&chars[0][4].number==1);
    ++config.config_revision;config.activities[0].activity_id=id<amity::system::ActivityId>(4);config.external_slots[0].activity_id=config.activities[0].activity_id;
    config.external_slots[0].alias="Replacement";refresh_values();assert(saved_reads==17&&chars[0][0].text=="Replacement"&&!inputs[0].hidden);
    ++config.config_revision;config.external_slots[0].tombstone=true;refresh_values();
    assert(chars[0][2].number==0&&chars[0][3].number==1);
    const auto updates=input_updates;refresh_values();assert(input_updates==updates);
    std::puts("PASS HomeKit refresh: repeated reads allocate no input projections, live playback remains current, revisions/aliases/visibility/replacement/tombstones refresh correctly");
}
'''


def main():
    source = (ROOT / 'components/amity_homekit/homekit.cpp').read_text()
    configuration = (ROOT / 'components/config_store/system_configuration.cpp').read_text()
    declarations = re.search(r'struct Input \{.*?bool inputs_dirty=true;', source, re.S).group()
    code = PREFIX + '\nnamespace config_store {\n' + function(configuration, 'const ConfiguredActivity* activity_for_slot(') + '\n}\n'
    code += declarations + function(source, 'void refresh_inputs()') + function(source, 'void refresh_values()') + SUITE
    with tempfile.TemporaryDirectory(prefix='amity-homekit-refresh-') as directory:
        path, binary = Path(directory) / 'test.cpp', Path(directory) / 'test'
        path.write_text(code)
        subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-g', '-fsanitize=address,undefined',
                        '-I'+str(ROOT / 'tests/reset_fakes'), '-I'+str(ROOT / 'components/config_store/include'),
                        '-I'+str(ROOT / 'components/amity_system/include'), '-I'+str(ROOT / 'components/amity_hub/include'),
                        '-I'+str(ROOT / 'components/cec/include'), str(path), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
