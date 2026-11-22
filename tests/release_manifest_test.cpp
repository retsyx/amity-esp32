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
    assert(release.release_id=="test-release"&&release.image_bytes==4096&&release.signing_key_id.empty()&&release.canonical==text);
    const auto field=[&](const std::string& extra){return text.substr(0,text.size()-1)+","+extra+"}";};
    // Closed and canonical: duplicate, unknown or retired fields and other bytes are rejected.
    for(const std::string& candidate:{text+"\n",std::string(" ")+text,field("\"format\":1"),field("\"arbitrary\":true"),
            field("\"security_profile\":\"development\""),field("\"source_commit\":\""+std::string(40,'c')+"\"")})
        assert(!system_services::updates::parse_release_manifest(candidate,release,error));
    for(const auto* required:{"\"image_sha256\"","\"hardware_profiles\"","\"signing_key_id\""}){
        auto missing=text;const auto at=missing.find(required);assert(at!=std::string::npos);missing.replace(at,1,"\"x");
        assert(!system_services::updates::parse_release_manifest(missing,release,error));
    }
    assert(system_services::updates::parse_release_manifest(text,release,error));
    std::cout<<"PASS canonical release manifest: closed fields, no retired compatibility or provenance fields, exact bytes\n";
}
