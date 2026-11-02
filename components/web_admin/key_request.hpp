// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "cJSON.h"
#include <cstdint>
#include <cstring>

namespace web_admin {
struct KeyRequest { uint8_t key=0;uint16_t duration_ms=0; };

inline bool parse_key_request(const cJSON* root,KeyRequest& out){
    if(!cJSON_IsObject(root))return false;
    bool key_seen=false,duration_seen=false;
    for(const auto* field=root->child;field;field=field->next){
        if(!field->string)return false;
        if(!std::strcmp(field->string,"key")){if(key_seen)return false;key_seen=true;}
        else if(!std::strcmp(field->string,"duration_ms")){if(duration_seen)return false;duration_seen=true;}
        else return false;
    }
    const auto* key=cJSON_GetObjectItemCaseSensitive(root,"key");
    if(!key_seen||!cJSON_IsNumber(key)||key->valuedouble!=key->valueint||key->valueint<0||key->valueint>255)return false;
    KeyRequest request;request.key=static_cast<uint8_t>(key->valueint);
    if(duration_seen){
        const auto* duration=cJSON_GetObjectItemCaseSensitive(root,"duration_ms");
        if(!cJSON_IsNumber(duration)||duration->valuedouble!=duration->valueint||duration->valueint<0||duration->valueint>5000)return false;
        request.duration_ms=static_cast<uint16_t>(duration->valueint);
    }
    out=request;return true;
}

// The administration task owns this bounded call. No response/socket I/O occurs
// before exact-key release, so a disconnected caller cannot strand the hold.
// The owner task continues ordinary RX, completion-paced repetition and setup.
// This scope also releases if the injected host-test wait unwinds.
template<class Control,class Wait>bool submit_key_request(Control& control,const KeyRequest& request,Wait wait){
    if(!request.duration_ms)return control.press_remote_key(request.key);
    const auto generation=control.remote_activity_generation(),selection=control.activity_selection(),input_epoch=control.remote_input_epoch();
    if(control.activity_setup_in_progress())return true;
    if(!control.press_timed_remote_key(request.key,generation,selection,input_epoch))return false;
    struct Release {
        Control& control;uint8_t key;uint32_t generation,selection,input_epoch;bool armed=true;
        ~Release(){if(armed)control.release_timed_remote_key(key,generation,selection,input_epoch);}
        bool finish(){armed=false;return control.release_timed_remote_key(key,generation,selection,input_epoch);}
    } release{control,request.key,generation,selection,input_epoch};
    wait(request.duration_ms);
    return release.finish();
}
}
