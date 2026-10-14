/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
#include "cJSON.h"
#include <string>

namespace web_admin {
inline cJSON *parse_json_input(const std::string &data) {
    if(data.find('\0')!=std::string::npos)return nullptr;
    unsigned depth=0;bool quoted=false,escaped=false;
    for(size_t i=0;i<data.size();++i) {
        const char ch=data[i];
        if(quoted) {
            if(escaped) {
                // cJSON strings have no length, so a decoded NUL would silently
                // truncate a password, key or configuration value at its C-string use.
                // An escaped backslash leaves the following u0000 as ordinary text.
                if(ch=='u'&&data.compare(i+1,4,"0000")==0)return nullptr;
                escaped=false;
            } else if(ch=='\\')escaped=true;
            else if(ch=='\"')quoted=false;
        } else if(ch=='\"')quoted=true;
        else if(ch=='{'||ch=='[') {if(++depth>8)return nullptr;}
        else if(ch=='}'||ch==']') {if(depth)--depth;}
    }
    return cJSON_ParseWithLengthOpts(data.c_str(),data.size()+1,nullptr,true);
}
}
