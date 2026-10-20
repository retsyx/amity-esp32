// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
namespace system_services::updates {
inline constexpr std::string_view update_bundle_magic="AMITY-UPDATE-V1\n";
inline constexpr uint32_t update_bundle_header_bytes=32;
struct UpdateBundleHeader {
    uint32_t manifest_bytes=0,signature_bytes=0;
    uint64_t image_bytes=0;
    uint64_t image_offset()const{return update_bundle_header_bytes+uint64_t(manifest_bytes)+signature_bytes;}
};
// No allocation from an advertised length. Validate exact total size before
// reading metadata; its native image remains byte-for-byte unchanged.
inline bool parse_update_bundle_header(std::string_view bytes,uint64_t file_bytes,UpdateBundleHeader& out,std::string& error){
    if(bytes.size()!=update_bundle_header_bytes||bytes.substr(0,update_bundle_magic.size())!=update_bundle_magic){error="Invalid update bundle header";return false;}
    auto integer=[&](size_t offset,size_t count){uint64_t value=0;for(size_t n=0;n<count;++n)value=(value<<8)|static_cast<unsigned char>(bytes[offset+n]);return value;};
    UpdateBundleHeader candidate;candidate.manifest_bytes=static_cast<uint32_t>(integer(16,4));candidate.signature_bytes=static_cast<uint32_t>(integer(20,4));candidate.image_bytes=integer(24,8);
    if(!candidate.manifest_bytes||candidate.manifest_bytes>8192||(candidate.signature_bytes!=0&&candidate.signature_bytes!=64)||!candidate.image_bytes||candidate.image_bytes>0x3d0000){error="Update bundle lengths exceed the supported profile";return false;}
    if(file_bytes!=candidate.image_offset()+candidate.image_bytes){error="Update bundle is truncated or has trailing data";return false;}
    out=candidate;return true;
}
}
