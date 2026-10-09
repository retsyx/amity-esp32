/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
#include <array>
#include <optional>
#include <string_view>
#include <vector>

namespace system_services {
namespace version_detail {
inline bool numeric(std::string_view s) {
    if(s.empty())return false;
    for(char c:s)if(c<'0'||c>'9')return false;
    return true;
}
inline bool number(std::string_view s) { return numeric(s)&&(s.size()==1||s.front()!='0'); }
inline std::vector<std::string_view> split(std::string_view s) {
    std::vector<std::string_view> parts;
    for(;;) {
        auto at=s.find('.');parts.push_back(s.substr(0,at));
        if(at==s.npos)return parts;
        s.remove_prefix(at+1);
    }
}
inline bool identifiers(std::string_view s,bool prerelease) {
    for(auto part:split(s)) {
        if(part.empty()||(prerelease&&numeric(part)&&!number(part)))return false;
        for(char c:part)if(!((c>='0'&&c<='9')||(c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='-'))return false;
    }
    return true;
}
struct Version { std::vector<std::string_view> core,pre; };
inline std::optional<Version> parse(std::string_view s) {
    auto plus=s.find('+');
    if(plus!=s.npos) {
        if(!identifiers(s.substr(plus+1),false))return {};
        s=s.substr(0,plus);
    }
    Version v;auto dash=s.find('-');
    if(dash!=s.npos) {
        auto pre=s.substr(dash+1);
        if(!identifiers(pre,true))return {};
        v.pre=split(pre);s=s.substr(0,dash);
    }
    v.core=split(s);
    if(v.core.size()!=3)return {};
    for(auto part:v.core)if(!number(part))return {};
    return v;
}
inline int compare_part(std::string_view a,std::string_view b,bool digits) {
    if(digits&&a.size()!=b.size())return a.size()<b.size()?-1:1;
    return a==b?0:a<b?-1:1;
}
}
// Semantic-version precedence; unknown versions require explicit confirmation.
// Compare digit strings without integer conversion so large components cannot overflow.
inline std::optional<int> compare_firmware_versions(std::string_view candidate,std::string_view running) {
    using namespace version_detail;
    auto a=parse(candidate),b=parse(running);
    if(!a||!b)return {};
    for(size_t i=0;i<3;++i)if(int c=compare_part(a->core[i],b->core[i],true))return c;
    if(a->pre.empty()||b->pre.empty())return a->pre.empty()==b->pre.empty()?0:a->pre.empty()?1:-1;
    for(size_t i=0;i<a->pre.size()&&i<b->pre.size();++i) {
        bool an=numeric(a->pre[i]),bn=numeric(b->pre[i]);
        if(an!=bn)return an?-1:1;
        if(int c=compare_part(a->pre[i],b->pre[i],an))return c;
    }
    return a->pre.size()==b->pre.size()?0:a->pre.size()<b->pre.size()?-1:1;
}
}
