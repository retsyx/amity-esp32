#!/usr/bin/env python3
"""Run HomeKit's production input refresh and edit functions against saved configurations."""
from pathlib import Path
import re
import subprocess
import tempfile

from slice_support import function

ROOT = Path(__file__).resolve().parents[1]
PREFIX = r'''
#include "system_configuration.hpp"
#include "amity/hub.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>
template<class T>T id(unsigned n){char text[33];std::snprintf(text,sizeof(text),"%032x",n);return T::parse(text).value();}
unsigned saved_reads=0,input_updates=0,playback_reads=0,battery_reads=0;
namespace config_store {
int read(const char*,std::string&){++saved_reads;return -1;}
int write(const char*,const std::string&){return ESP_OK;}
}
struct hap_char_t {std::string text;int number=0;};
struct hap_val_t {int i=0;bool b=false;char* s=nullptr;struct {uint8_t* buf;uint32_t buflen;} t{};};
struct Control {
    std::shared_ptr<config_store::Configuration> config=std::make_shared<config_store::Configuration>();
    amity::PlaybackState state;
    std::shared_ptr<const config_store::Configuration> active_configuration()const{return config;}
    auto playback_state()const{++playback_reads;return state;}
    std::optional<unsigned> external_activity_index(const amity::PlaybackState& value)const{return value.current_activity<0?std::nullopt:std::optional<unsigned>(0);}
    std::optional<config_store::ConfiguredActivity> external_activity(unsigned slot)const{auto* activity=config_store::activity_for_slot(*config,slot);return activity?std::optional<config_store::ConfiguredActivity>(*activity):std::nullopt;}
    amity::Snapshot snapshot()const{return {};}
} service;
Control* hub=&service;
// Saving through the configuration store applies the candidate as current.
namespace amity {
unsigned activations=0;esp_err_t activation_result=ESP_OK;config_store::RevisionPair expected_revisions;
esp_err_t activate_distributed_configuration(config_store::Configuration candidate,config_store::RevisionPair expected,system::MessageId request){
    ++activations;assert(request);expected_revisions=expected;
    if(activation_result!=ESP_OK)return activation_result;
    ++candidate.presentation_revision;*service.config=std::move(candidate);return ESP_OK;
}
}
void esp_fill_random(void* data,size_t size){static uint8_t counter=0;std::memset(data,++counter,size);}
std::atomic<bool> disabled{false};
struct amity_matter_callbacks_t {int(*power)(bool,void*);int(*activity)(uint8_t,void*);int(*key)(uint8_t,void*);int(*mute)(bool,void*);int(*volume_step)(int16_t,void*);void* context;} callbacks{};
void update(hap_char_t* value,int number){value->number=number;}
void update_bool(hap_char_t* value,bool flag){value->number=flag;}
void update_string(hap_char_t* value,const std::string& text){++input_updates;value->text=text;}
void refresh_remote_battery(){++battery_reads;}
hap_char_t active_value,speaker_value,selected_value,mute_value,volume_value,key_value,name_value;
hap_char_t *active=&active_value,*speaker_active=&speaker_value,*active_input=&selected_value,*mute_char=&mute_value;
hap_char_t *volume_selector=&volume_value,*remote_key=&key_value,*tv_name=&name_value;
bool audio_known=false;
unsigned order_updates=0;std::vector<unsigned> displayed;
hap_char_t order_value;hap_char_t* display_order=&order_value;
void hap_char_update_val(hap_char_t* value,hap_val_t* v){
    if(value!=display_order){value->text=v->s?v->s:"";value->number=v->i;return;}
    // Decode the TLV8 list: one-byte Identifier items separated by empty items.
    ++order_updates;displayed.clear();
    for(uint32_t n=0;n<v->t.buflen;n+=3){
        if(n){assert(v->t.buf[n]==0&&v->t.buf[n+1]==0);n+=2;}
        assert(n+3<=v->t.buflen&&v->t.buf[n]==1&&v->t.buf[n+1]==1);displayed.push_back(v->t.buf[n+2]);
    }
}
constexpr int HAP_SUCCESS=0,HAP_STATUS_SUCCESS=0,HAP_STATUS_COMM_ERR=-70402,HAP_STATUS_VAL_INVALID=-70410;
struct hap_write_data_t {hap_char_t* hc;hap_val_t val;int* status;};
std::vector<unsigned> order(std::vector<unsigned> first){
    for(unsigned n=1;n<=16;++n)if(std::find(first.begin(),first.end(),n)==first.end())first.push_back(n);
    return first;
}
'''
SUITE = r'''
std::array<std::array<hap_char_t,5>,16> chars;
int edit(hap_char_t* characteristic,const char* name,int number=0){
    int status=1;hap_write_data_t data{characteristic,{},&status};data.val.s=const_cast<char*>(name);data.val.i=number;
    assert(write(&data,1,nullptr,nullptr)==HAP_SUCCESS);return status;
}
int main(){
    for(unsigned i=0;i<inputs.size();++i){auto& in=inputs[i];in.name=&chars[i][0];in.label=&chars[i][1];in.configured=&chars[i][2];in.visible=&chars[i][3];in.target_visible=&chars[i][4];}
    auto& config=*service.config;
    config_store::ConfiguredActivity activity;activity.activity_id=id<amity::system::ActivityId>(3);activity.label="Watch television";
    config.activities.push_back(activity);config.external_slots.push_back({0,activity.activity_id,false,false,{}});
    refresh_values();assert(input_updates==32&&chars[0][0].text==activity.label&&chars[0][2].number==1&&chars[1][0].text=="Unused 2");
    // Repeated per-service accessory reads and idle worker refreshes must not
    // walk/render inputs. Live playback still changes.
    service.state.current_activity=0;service.state.audio_status_known=true;service.state.muted=true;
    for(unsigned n=0;n<1000;++n)refresh_values();
    assert(input_updates==32&&playback_reads==1001&&battery_reads==1001);
    assert(active_value.number==1&&selected_value.number==1&&audio_known&&mute_value.number==1);
    ++config.presentation_revision;config.activities[0].label="New name";refresh_values();
    assert(input_updates==64&&chars[0][0].text=="New name");
    // The saved slot presentation is the only source of names and visibility.
    ++config.presentation_revision;config.external_slots[0].alias="Saved alias";refresh_values();
    assert(chars[0][0].text=="Saved alias"&&chars[0][1].text=="Saved alias");
    ++config.presentation_revision;config.external_slots[0].hidden=true;refresh_values();assert(chars[0][3].number==1&&chars[0][4].number==1);
    ++config.config_revision;config.activities[0].activity_id=id<amity::system::ActivityId>(4);config.external_slots[0].activity_id=config.activities[0].activity_id;
    config.external_slots[0].alias="Replacement";config.external_slots[0].hidden=false;refresh_values();
    assert(chars[0][0].text=="Replacement"&&chars[0][3].number==0);
    ++config.config_revision;config.external_slots[0].tombstone=true;refresh_values();
    assert(chars[0][2].number==0&&chars[0][3].number==1&&chars[0][0].text=="Unused 1");
    const auto updates=input_updates;refresh_values();assert(input_updates==updates);
    // Display Order follows the saved activity order, not identifier allocation
    // order, and notifies only when that order changes.
    assert(order_updates==1&&displayed==order({}));
    ++config.config_revision;config.activities.clear();config.external_slots.clear();
    for(unsigned n=0;n<3;++n){
        config_store::ConfiguredActivity value;value.activity_id=id<amity::system::ActivityId>(10+n);
        value.label="Activity "+std::to_string(n);config.activities.push_back(value);
        config.external_slots.push_back({static_cast<uint16_t>(n),value.activity_id,false,false,{}});
    }
    const auto activity_id=[](unsigned n){return id<amity::system::ActivityId>(n);};
    config.activity_order={activity_id(12),activity_id(10),activity_id(11)};refresh_values();
    assert(order_updates==2&&displayed==order({3,1,2}));
    ++config.presentation_revision;config.activity_order={activity_id(11),activity_id(12),activity_id(10)};refresh_values();
    assert(order_updates==3&&displayed==order({2,3,1}));
    ++config.presentation_revision;refresh_values();assert(order_updates==3);
    ++config.config_revision;config.external_slots[2].tombstone=true;config.activities.pop_back();
    config.activity_order={activity_id(11),activity_id(10)};refresh_values();
    assert(order_updates==4&&displayed==order({2,1}));
    // A Home app rename or visibility change saves the configuration slot,
    // which every reader of input names shares; it is not stored separately.
    auto expected=config.revisions();
    assert(edit(inputs[1].name,"Couch")==HAP_STATUS_SUCCESS&&amity::activations==1&&amity::expected_revisions==expected);
    assert(config.external_slots[1].alias=="Couch"&&chars[1][0].text=="Couch"&&chars[1][1].text=="Couch");
    assert(edit(inputs[1].target_visible,nullptr,1)==HAP_STATUS_SUCCESS&&amity::activations==2);
    assert(config.external_slots[1].hidden&&chars[1][3].number==1&&chars[1][4].number==1&&config.external_slots[1].alias=="Couch");
    // The activity's own name clears the alias, so later activity renames show.
    assert(edit(inputs[1].name,"Activity 1")==HAP_STATUS_SUCCESS&&amity::activations==3&&config.external_slots[1].alias.empty());
    ++config.presentation_revision;config.activities[1].label="Renamed";refresh_values();assert(chars[1][0].text=="Renamed");
    // Unchanged edits save nothing; unused inputs and invalid values are rejected.
    assert(edit(inputs[1].target_visible,nullptr,1)==HAP_STATUS_SUCCESS&&amity::activations==3);
    assert(edit(inputs[2].name,"Unused")==HAP_STATUS_VAL_INVALID&&edit(inputs[5].target_visible,nullptr,0)==HAP_STATUS_VAL_INVALID);
    assert(edit(inputs[1].name,"")==HAP_STATUS_VAL_INVALID&&edit(inputs[1].target_visible,nullptr,2)==HAP_STATUS_VAL_INVALID&&amity::activations==3);
    // A rejected save leaves the saved and displayed presentation unchanged.
    amity::activation_result=ESP_ERR_INVALID_STATE;const auto before=config.external_slots[0];
    assert(edit(inputs[0].name,"Den")==HAP_STATUS_COMM_ERR&&amity::activations==4);
    assert(config.external_slots[0].alias==before.alias&&chars[0][0].text=="Activity 0");
    assert(saved_reads==0);
    std::puts("PASS HomeKit inputs: names and visibility come only from the saved configuration; Home app edits save it; reads stay cheap; display order follows activity order");
}
'''


def main():
    source = (ROOT / 'components/amity_homekit/homekit.cpp').read_text()
    configuration = (ROOT / 'components/config_store/system_configuration.cpp').read_text()
    declarations = re.search(r'struct Input \{.*?bool inputs_dirty=true;', source, re.S).group()
    code = PREFIX + '\nnamespace config_store {\n' + ''.join(function(configuration, declaration) for declaration in (
        'const ConfiguredActivity* activity_for_slot(', 'std::vector<model::ActivityId> ordered_activities(',
        'std::vector<ExternalInput> external_inputs(')) + '\n}\n'
    code += declarations + ''.join(function(source, declaration) for declaration in (
        'esp_err_t save_input(', 'void refresh_display_order(', 'void refresh_inputs()', 'void refresh_values()',
        'int write(hap_write_data_t data[]')) + SUITE
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
