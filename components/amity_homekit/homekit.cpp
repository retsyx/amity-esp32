/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#include "homekit.hpp"
#include "device_settings.hpp"
#include "config_store.hpp"
#include "ble_remote.h"
#include "esp_app_desc.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "psa/crypto.h"
extern "C" {
#include "hap.h"
#include "amity_hap.h"
#include "hap_apple_chars.h"
esp_err_t amity_homekit_generate_setup(const char *code,hap_setup_info_t *setup);
}
#include <array>
#include <algorithm>
#include <cstring>
#include <cstdio>
namespace amity_homekit {
namespace {
constexpr auto tag="amity_homekit";
constexpr uint16_t read_notify=HAP_CHAR_PERM_PR|HAP_CHAR_PERM_EV;
constexpr uint16_t read_write_notify=read_notify|HAP_CHAR_PERM_PW;
constexpr auto television_category=static_cast<hap_cid_t>(31);
constexpr int64_t pairing_duration=300LL*1000000;
enum class Command { Open, Close, Remove };
amity::HubService *hub;
amity_matter_callbacks_t callbacks{};
QueueHandle_t commands;
SemaphoreHandle_t status_lock;
SemaphoreHandle_t startup_ready;
esp_err_t startup_result=ESP_OK;
Status current_status;
bool initialized=false, running=false;
int64_t deadline=0;
int64_t published_deadline=0;
hap_char_t *active, *active_input, *tv_name, *remote_key, *speaker_active, *mute_char, *volume_selector;
hap_char_t *battery_level, *battery_low, *battery_charging;
struct Input {
    hap_char_t *name=nullptr,*label=nullptr,*configured=nullptr,*visible=nullptr,*target_visible=nullptr;
    std::string activity,alias;
    bool hidden=false;
};
std::array<Input,16> inputs;
bool battery_known=false, charging_known=false, audio_known=false;
struct HapLock { HapLock(){hap_amity_lock();} ~HapLock(){hap_amity_unlock();} };
struct StatusLock { StatusLock(){xSemaphoreTake(status_lock,portMAX_DELAY);} ~StatusLock(){xSemaphoreGive(status_lock);} };
char *text(const char *value){return const_cast<char *>(value);}
void update(hap_char_t *hc,int value){hap_val_t v{};v.i=value;hap_char_update_val(hc,&v);}
void update_bool(hap_char_t *hc,bool value){hap_val_t v{};v.b=value;hap_char_update_val(hc,&v);}
void update_string(hap_char_t *hc,const std::string &value){
    if(value==hap_char_get_val(hc)->s)return;
    hap_val_t v{};v.s=text(value.c_str());hap_char_update_val(hc,&v);
}
std::string input_key(unsigned index){return "hap_input_"+std::to_string(index);}
esp_err_t save_input(unsigned index){
    const auto &in=inputs[index];
    std::string record=in.activity;record+='\0';record+=in.hidden?'1':'0';record+=in.alias;
    return config_store::write(input_key(index).c_str(),record);
}
void refresh_values(){
    const auto s=hub->snapshot();
    update(active,s.current_activity>=0);update(speaker_active,s.current_activity>=0);
    update(active_input,s.current_activity>=0?s.current_activity+1:0);
    audio_known=s.audio_status_known;
    if(audio_known)update_bool(mute_char,s.muted);
    for(unsigned i=0;i<inputs.size();++i){
        auto &in=inputs[i];const bool exists=i<s.activities.size();
        const std::string name=exists?s.activities[i].name:"Unused "+std::to_string(i+1);
        if(in.activity!=name){
            in.activity=name;in.alias.clear();in.hidden=false;
            std::string record;
            if(config_store::read(input_key(i).c_str(),record)==ESP_OK){
                const auto split=record.find('\0');
                if(split!=std::string::npos&&record.substr(0,split)==name&&record.size()>split+1){
                    in.hidden=record[split+1]=='1';in.alias=record.substr(split+2);
                }
            }
        }
        update_string(in.name,in.alias.empty()?name:in.alias);
        update_string(in.label,in.alias.empty()?name:in.alias);
        update(in.configured,exists?1:0);update(in.visible,(!exists||in.hidden)?1:0);
        update(in.target_visible,(!exists||in.hidden)?1:0);
    }
    ble_remote_status_t remote{};ble_remote_get_status(&remote);
    battery_known=!remote.bonded||(remote.connected&&remote.battery_percent>=0);
    charging_known=!remote.bonded||(remote.connected&&remote.charging>=0);
    if(battery_known){const int percent=remote.bonded?std::clamp(remote.battery_percent,0,100):100;update(battery_level,percent);update(battery_low,percent<=20);}
    if(charging_known)update(battery_charging,remote.bonded?remote.charging:0);
}
int read(hap_read_data_t data[],int count,void *,void *){
    refresh_values();
    for(int i=0;i<count;++i){
        const auto hc=data[i].hc;
        *data[i].status=((hc==mute_char&&!audio_known)||
            ((hc==battery_level||hc==battery_low)&&!battery_known)||
            (hc==battery_charging&&!charging_known))?HAP_STATUS_RES_ABSENT:HAP_STATUS_SUCCESS;
    }
    return HAP_SUCCESS;
}
int write(hap_write_data_t data[],int count,void *,void *){
    static const std::pair<int,uint8_t> keys[]={{0,0x48},{1,0x49},{2,0x4b},{3,0x4c},{4,0x01},{5,0x02},
        {6,0x03},{7,0x04},{8,0x00},{9,0x0d},{10,0x0d},{15,0x35}};
    for(int i=0;i<count;++i){
        auto &d=data[i];esp_err_t result=ESP_ERR_INVALID_ARG;
        if(d.hc==active&&(d.val.i==0||d.val.i==1))result=callbacks.power(d.val.i!=0,callbacks.context);
        else if(d.hc==active_input&&d.val.i>=1&&d.val.i<=static_cast<int>(hub->snapshot().activities.size()))
            result=callbacks.activity(d.val.i-1,callbacks.context);
        else if(d.hc==mute_char)result=callbacks.mute(d.val.b,callbacks.context);
        else if(d.hc==volume_selector&&(d.val.i==0||d.val.i==1))result=callbacks.volume_step(d.val.i==0?1:-1,callbacks.context);
        else if(d.hc==remote_key){
            if(d.val.i==11)result=callbacks.key(0x61,callbacks.context);
            else for(const auto &key:keys)if(key.first==d.val.i){result=callbacks.key(key.second,callbacks.context);break;}
        }
        else if(d.hc==tv_name&&d.val.s&&strlen(d.val.s)>0&&strlen(d.val.s)<=63)
            result=config_store::write("hap_name",d.val.s);
        else for(unsigned n=0;n<inputs.size();++n){
            auto &in=inputs[n];
            if(d.hc==in.name&&d.val.s&&strlen(d.val.s)>0&&strlen(d.val.s)<=63){
                const auto old=in.alias;in.alias=d.val.s;result=save_input(n);if(result!=ESP_OK)in.alias=old;break;
            }
            if(d.hc==in.target_visible&&(d.val.i==0||d.val.i==1)){
                const bool old=in.hidden;in.hidden=d.val.i==1;result=save_input(n);if(result!=ESP_OK)in.hidden=old;break;
            }
        }
        *d.status=result==ESP_OK?HAP_STATUS_SUCCESS:result==ESP_ERR_INVALID_ARG?HAP_STATUS_VAL_INVALID:HAP_STATUS_COMM_ERR;
        // Power/input/mute report observed hub state, not optimistic acknowledgements.
        if(result==ESP_OK&&d.hc!=active&&d.hc!=active_input&&d.hc!=mute_char)hap_char_update_val(d.hc,&d.val);
    }
    refresh_values();return HAP_SUCCESS;
}
hap_char_t *integer(hap_serv_t *service,const char *uuid,uint16_t permissions,int value,int max){
    auto *hc=hap_char_uint8_create(text(uuid),permissions,value);
    if(!hc||hap_serv_add_char(service,hc)!=HAP_SUCCESS)return nullptr;
    hap_char_int_set_constraints(hc,0,max,1);return hc;
}
hap_char_t *string(hap_serv_t *service,const char *uuid,uint16_t permissions,const char *value){
    auto *hc=hap_char_string_create(text(uuid),permissions,text(value));
    if(!hc||hap_serv_add_char(service,hc)!=HAP_SUCCESS)return nullptr;
    hap_char_string_set_maxlen(hc,63);
    return hc;
}
int identify(hap_acc_t *){return HAP_SUCCESS;}
esp_err_t create_accessory(){
    hap_acc_cfg_t cfg{};cfg.name=text("Amity");cfg.model=text("Amity Board / Fin");cfg.manufacturer=text("Amity");
    uint8_t mac[6];esp_read_mac(mac,ESP_MAC_WIFI_STA);char serial[13];snprintf(serial,sizeof(serial),"%02X%02X%02X%02X%02X%02X",mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    cfg.serial_num=serial;cfg.fw_rev=text(esp_app_get_description()->version);cfg.hw_rev=text("1");cfg.pv=text("1.1");cfg.cid=television_category;cfg.identify_routine=identify;
    auto *accessory=hap_acc_create(&cfg);auto *tv=hap_serv_create(text("D8"));auto *speaker=hap_serv_create(text("113"));auto *battery=hap_serv_create(text("96"));
    if(!accessory||!tv||!speaker||!battery)return ESP_ERR_NO_MEM;
    active=integer(tv,"B0",read_write_notify,0,1);
    active_input=hap_char_uint32_create(text("E7"),read_write_notify,0);
    if(!active_input||hap_serv_add_char(tv,active_input)!=HAP_SUCCESS)return ESP_ERR_NO_MEM;
    std::string name="Amity",saved;if(config_store::read("hap_name",saved)==ESP_OK&&!saved.empty())name=saved;
    tv_name=string(tv,"E3",read_write_notify,name.c_str());
    remote_key=integer(tv,"E1",HAP_CHAR_PERM_PW,0,16);
    if(!integer(tv,"E8",read_notify,1,1))return ESP_ERR_NO_MEM;
    speaker_active=integer(speaker,"B0",read_notify,0,1);
    mute_char=hap_char_bool_create(text("11A"),read_write_notify,false);
    if(!mute_char||hap_serv_add_char(speaker,mute_char)!=HAP_SUCCESS)return ESP_ERR_NO_MEM;
    if(!integer(speaker,"E9",read_notify,1,3))return ESP_ERR_NO_MEM;
    volume_selector=integer(speaker,"EA",HAP_CHAR_PERM_PW,0,1);
    battery_level=integer(battery,"68",read_notify,100,100);battery_low=integer(battery,"79",read_notify,0,1);battery_charging=integer(battery,"8F",read_notify,0,2);
    if(!active||!active_input||!tv_name||!remote_key||!speaker_active||!mute_char||!volume_selector||!battery_level||!battery_low||!battery_charging)return ESP_ERR_NO_MEM;
    if(!string(battery,"23",HAP_CHAR_PERM_PR,"Remote battery"))return ESP_ERR_NO_MEM;
    for(auto *service:{tv,speaker,battery}){
        hap_serv_set_bulk_read_cb(service,read);hap_serv_set_write_cb(service,write);hap_acc_add_serv(accessory,service);
    }
    hap_serv_mark_primary(tv);hap_serv_link_serv(tv,speaker);
    for(unsigned i=0;i<inputs.size();++i){
        auto *service=hap_serv_create(text("D9"));if(!service)return ESP_ERR_NO_MEM;
        auto *id=hap_char_uint32_create(text("E6"),HAP_CHAR_PERM_PR,i+1);
        if(!id||hap_serv_add_char(service,id)!=HAP_SUCCESS)return ESP_ERR_NO_MEM;
        auto &in=inputs[i];in.name=string(service,"E3",read_write_notify,"Unused");
        in.label=string(service,"23",HAP_CHAR_PERM_PR,"Unused");
        in.configured=integer(service,"D6",read_notify,0,1);in.visible=integer(service,"135",read_notify,1,1);
        in.target_visible=integer(service,"134",read_write_notify,1,1);
        if(!integer(service,"DB",HAP_CHAR_PERM_PR,3,10)||!in.name||!in.label||!in.configured||!in.visible||!in.target_visible)return ESP_ERR_NO_MEM;
        hap_serv_set_bulk_read_cb(service,read);hap_serv_set_write_cb(service,write);
        hap_acc_add_serv(accessory,service);hap_serv_link_serv(tv,service);
    }
    hap_add_accessory(accessory);return ESP_OK;
}
esp_err_t prepare_setup(char code[11],std::string &record){
    // Fresh PINs are generated only on a web pairing request; persist only the verifier.
    uint32_t pin;do{pin=esp_random()%100000000;}while(pin==12345678||pin==87654321||pin%11111111==0);
    snprintf(code,11,"%03lu-%02lu-%03lu",static_cast<unsigned long>(pin/100000),static_cast<unsigned long>((pin/1000)%100),static_cast<unsigned long>(pin%1000));
    hap_setup_info_t setup{};
    const auto error=amity_homekit_generate_setup(code,&setup);if(error!=ESP_OK)return error;
    std::string old;char id[5]{};
    if(config_store::read("hap_setup",old)==ESP_OK&&old.size()==404)memcpy(id,old.data(),4);
    else {const char alphabet[]="0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";for(unsigned i=0;i<4;++i)id[i]=alphabet[esp_random()%36];}
    record.assign(id,4);record.append(reinterpret_cast<const char *>(&setup),sizeof(setup));
    return config_store::write("hap_setup",record);
}
esp_err_t initialize(const std::string &record){
    if(record.size()!=404)return ESP_ERR_INVALID_SIZE;
    if(!initialized){
        hap_cfg_t config;hap_get_config(&config);config.max_event_notif_chars=128;
        if(hap_set_config(&config)!=HAP_SUCCESS||hap_init(HAP_TRANSPORT_WIFI)!=HAP_SUCCESS)return ESP_FAIL;
        const auto error=create_accessory();if(error!=ESP_OK)return error;
        initialized=true;
    }
    char id[5]{};memcpy(id,record.data(),4);hap_setup_info_t setup;memcpy(&setup,record.data()+4,sizeof(setup));
    if(hap_set_setup_id(id)!=HAP_SUCCESS||hap_set_setup_info(&setup)!=HAP_SUCCESS)return ESP_ERR_NO_MEM;
    refresh_values();
    if(!running){if(hap_start()!=HAP_SUCCESS)return ESP_FAIL;running=true;}
    return ESP_OK;
}
bool network_available(){
    wifi_ap_record_t ap{};
    // Matter provisions this station; its fabric may later be removed independently.
    return esp_wifi_sta_get_ap_info(&ap)==ESP_OK;
}
void publish_status(bool available,esp_err_t error=ESP_OK){
    StatusLock lock;current_status.started=running;current_status.available=available;
    current_status.controllers=initialized?hap_get_paired_controller_count():0;
    current_status.paired=current_status.controllers>0;
    current_status.pairing=running&&deadline>esp_timer_get_time()&&!current_status.paired;
    published_deadline=deadline;
    if(error!=ESP_OK)current_status.error=error;
    if(!current_status.pairing){memset(current_status.manual_code,0,sizeof(current_status.manual_code));memset(current_status.qr_payload,0,sizeof(current_status.qr_payload));}
}
void worker(void *){
    std::string record;
    const auto saved=config_store::read("hap_setup",record);
    if(saved==ESP_OK){HapLock lock;startup_result=initialize(record);publish_status(network_available(),startup_result);}
    else if(saved!=ESP_ERR_NVS_NOT_FOUND)startup_result=saved;
    xSemaphoreGive(startup_ready);
    for(;;){
        Command command;const bool requested=xQueueReceive(commands,&command,pdMS_TO_TICKS(1000))==pdTRUE;
        const bool available=network_available();esp_err_t error=ESP_OK;
        {
            HapLock lock;
            if(requested){
                if(command==Command::Open){
                    if(!available)error=ESP_ERR_INVALID_STATE;
                    else if(initialized&&hap_get_paired_controller_count()>0)error=ESP_ERR_INVALID_STATE;
                    else {
                        char code[11]{};std::string setup;
                        error=prepare_setup(code,setup);
                        if(error==ESP_OK){deadline=esp_timer_get_time()+pairing_duration;hap_amity_set_pairing_deadline(deadline);error=initialize(setup);}
                        if(error==ESP_OK){
                            char id[5]{};memcpy(id,setup.data(),4);char *qr=esp_hap_get_setup_payload(code,id,false,television_category);
                            if(!qr)error=ESP_ERR_NO_MEM;
                            else{StatusLock status;snprintf(current_status.manual_code,sizeof(current_status.manual_code),"%s",code);snprintf(current_status.qr_payload,sizeof(current_status.qr_payload),"%s",qr);free(qr);}
                        }
                        if(error==ESP_OK&&hap_amity_refresh_mdns()!=HAP_SUCCESS)error=ESP_FAIL;
                        memset(code,0,sizeof(code));
                    }
                } else {
                    deadline=0;hap_amity_set_pairing_deadline(0);
                    if(running){
                        if(command==Command::Remove)error=hap_amity_remove_pairings()==HAP_SUCCESS?ESP_OK:ESP_FAIL;
                        else error=hap_amity_refresh_mdns()==HAP_SUCCESS?ESP_OK:ESP_FAIL;
                    }
                }
            }
            if(deadline&&(deadline<=esp_timer_get_time()||(initialized&&hap_get_paired_controller_count()>0)||error!=ESP_OK)){
                    deadline=0;hap_amity_set_pairing_deadline(0);hap_amity_refresh_mdns();
            }
            if(running){
                refresh_values();
            }
            publish_status(available,error);
            if(requested){StatusLock status;current_status.busy=false;}
        }
        if(error!=ESP_OK)ESP_LOGW(tag,"HomeKit operation: %s",esp_err_to_name(error));
    }
}
esp_err_t submit(Command command){
    StatusLock lock;if(current_status.busy)return ESP_ERR_INVALID_STATE;
    if(command==Command::Open&&(!current_status.available||current_status.paired))return ESP_ERR_INVALID_STATE;
    current_status.busy=true;current_status.error=ESP_OK;
    if(xQueueSend(commands,&command,0)!=pdTRUE){current_status.busy=false;return ESP_ERR_NO_MEM;}
    return ESP_OK;
}
}
esp_err_t start(amity::HubService &service,const amity_matter_callbacks_t &handlers){
    hub=&service;callbacks=handlers;
    status_lock=xSemaphoreCreateMutex();commands=xQueueCreate(1,sizeof(Command));startup_ready=xSemaphoreCreateBinary();
    if(!status_lock||!commands||!startup_ready||hap_amity_init(device_settings::hostname())!=HAP_SUCCESS)return ESP_ERR_NO_MEM;
    if(xTaskCreate(worker,"homekit",8192,nullptr,3,nullptr)!=pdPASS)return ESP_ERR_NO_MEM;
    if(xSemaphoreTake(startup_ready,pdMS_TO_TICKS(30000))!=pdTRUE)return ESP_ERR_TIMEOUT;
    return startup_result;
}
Status status(){
    StatusLock lock;auto result=current_status;
    if(result.pairing&&published_deadline<=esp_timer_get_time()){
        result.pairing=false;memset(result.manual_code,0,sizeof(result.manual_code));memset(result.qr_payload,0,sizeof(result.qr_payload));
    }
    return result;
}
esp_err_t open_pairing(){return submit(Command::Open);}
esp_err_t close_pairing(){return submit(Command::Close);}
esp_err_t remove_pairings(){return submit(Command::Remove);}
}
