// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#include "release_manifest.hpp"
#include <cassert>
#include <fstream>
#include <iostream>
#include <iterator>
int main(int argc,char** argv){
    assert(argc==2);std::ifstream file(argv[1]);std::string text((std::istreambuf_iterator<char>(file)),{}),error;
    system_services::updates::ReleaseManifest release;
    assert(system_services::updates::parse_release_manifest(text,release,error));
    assert(system_services::updates::compatible_release(release,0x3d0000,2,2,511,false,error));
    for(const std::string& candidate:{text+"\n",std::string(" ")+text,text.substr(0,text.size()-1)+",\"format\":1}",text.substr(0,text.size()-1)+",\"arbitrary\":true}"})
        assert(!system_services::updates::parse_release_manifest(candidate,release,error));
    assert(system_services::updates::parse_release_manifest(text,release,error));
    auto forward=text;const auto capabilities=forward.find("\"optional_capabilities\":\"511\"");assert(capabilities!=std::string::npos);forward.replace(capabilities,std::string("\"optional_capabilities\":\"511\"").size(),"\"optional_capabilities\":\"2147484159\"");
    assert(system_services::updates::parse_release_manifest(forward,release,error));
    assert(system_services::updates::compatible_release(release,0x3d0000,2,2,511,false,error));
    auto required=forward;const auto floor=required.find("\"required_capabilities\":\"23\"");assert(floor!=std::string::npos);required.replace(floor,std::string("\"required_capabilities\":\"23\"").size(),"\"required_capabilities\":\"2147483671\"");assert(!system_services::updates::parse_release_manifest(required,release,error));
    assert(system_services::updates::parse_release_manifest(text,release,error));
    auto incompatible=release;incompatible.image_bytes=0x3d0001;assert(!system_services::updates::compatible_release(incompatible,0x3d0000,2,2,511,false,error));
    incompatible=release;incompatible.storage_write=3;assert(!system_services::updates::compatible_release(incompatible,0x3d0000,2,2,511,false,error));
    incompatible=release;incompatible.config_read_min=3;assert(!system_services::updates::compatible_release(incompatible,0x3d0000,2,2,511,false,error));
    assert(!system_services::updates::compatible_release(release,0x3d0000,2,2,0,false,error));
    assert(!system_services::updates::compatible_release(release,0x3d0000,2,2,511,true,error));
    std::cout<<"PASS canonical release manifest rejects duplicate/unknown fields, wrong image capacity, unreadable rollback and profile changes\n";
}
