// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#include "release_manifest.hpp"
#include "release_hardware.hpp"
#include "amity/consortium/wire.hpp"
#include <algorithm>
#include <set>
#if CONFIG_AMITY_BOARD_DEVELOPMENT_5T
namespace config_store { std::string sha256_hex(std::string_view); }
#endif
namespace system_services::updates {
namespace {
namespace c=amity::consortium;
// A real runtime reference retains the public descriptor with Xtensa section
// garbage collection. This toolchain does not support the retain attribute.
const char* volatile hardware_descriptor_anchor=release_hardware_descriptor;
bool hex(std::string_view text,size_t length){return text.size()==length&&std::all_of(text.begin(),text.end(),[](char b){return (b>='0'&&b<='9')||(b>='a'&&b<='f');});}
bool token(const c::Json* value,std::string& result,size_t min,size_t max){return c::json_string(value,result,min,max)&&std::all_of(result.begin(),result.end(),[](unsigned char b){return b>=0x20&&b<=0x7e&&b!='"'&&b!='\\';});}
std::string canonical(c::Json value){
    switch(value.type){
    case c::Json::Type::null_value:return "null";
    case c::Json::Type::boolean:return value.boolean?"true":"false";
    case c::Json::Type::integer:return std::to_string(value.integer);
    case c::Json::Type::string:return "\""+value.string+"\"";
    case c::Json::Type::array:{std::string out="[";for(const auto& item:value.array){if(out.size()>1)out+=',';out+=canonical(item);}return out+"]";}
    case c::Json::Type::object:{std::sort(value.object.begin(),value.object.end(),[](const auto& a,const auto& b){return a.first<b.first;});std::string out="{";for(const auto& item:value.object){if(out.size()>1)out+=',';out+='"'+item.first+"\":"+canonical(item.second);}return out+"}";}
    }
    return {};
}
bool bounds(const c::Json* value,uint64_t& low,uint64_t& high,uint64_t& write){return value&&c::closed(*value,{"read_min","read_max","write"})&&c::decimal(value->get("read_min"),low)&&low&&c::decimal(value->get("read_max"),high)&&high>=low&&c::decimal(value->get("write"),write)&&write>=low&&write<=high;}
}
std::string compiled_hardware_profile(){
    const std::string_view descriptor(hardware_descriptor_anchor);const auto first=descriptor.find("profile=");
    if(first==std::string_view::npos)return {};
    const auto end=descriptor.find('\n',first);if(end==std::string_view::npos)return {};
    return std::string(descriptor.substr(first+8,end-first-8));
}
std::string compiled_hardware_mapping_sha256(){
#if CONFIG_AMITY_BOARD_DEVELOPMENT_5T
    return config_store::sha256_hex(hardware_descriptor_anchor);
#else
    return {};
#endif
}
bool parse_release_manifest(std::string_view text,ReleaseManifest& out,std::string& error){
    const auto fail=[&](const char* message){error=message;return false;};c::Json value;c::JsonError detail;ReleaseManifest m;uint64_t format=0;
    if(!c::parse_json(text,value,detail,8192)||!c::closed(value,{"format","release_id","firmware_version","image_bytes","image_sha256","image_elf_sha256","chip","hardware_profiles","partition_layout","slot_bytes","protocol","configuration","storage","origin","factory","bootloader","security_profile","rollback_readable_max","source_commit","source_tree_sha256","source_dirty","dependencies","signing_key_id"},{"hardware_mapping_sha256"}))return fail("Invalid or unsupported release manifest fields");
    std::string chip,layout,bootloader;
    if(!c::json_uint(value.get("format"),format,1,1)||!token(value.get("release_id"),m.release_id,1,96)||!token(value.get("firmware_version"),m.firmware_version,1,31)||
       !token(value.get("image_sha256"),m.image_sha256,64,64)||!hex(m.image_sha256,64)||!token(value.get("image_elf_sha256"),m.image_elf_sha256,64,64)||!hex(m.image_elf_sha256,64)||
       !c::decimal(value.get("image_bytes"),m.image_bytes)||!m.image_bytes||!c::decimal(value.get("slot_bytes"),m.slot_bytes)||!m.slot_bytes||
       !token(value.get("chip"),chip,1,16)||chip!="esp32s3"||!token(value.get("partition_layout"),layout,1,64)||layout!="amity-8m-ota3d0000-v1"||
       !token(value.get("bootloader"),bootloader,1,64)||bootloader!="esp-idf6.1-rollback-v1"||!token(value.get("security_profile"),m.security_profile,1,16)||
       (m.security_profile!="development"&&m.security_profile!="production")||!token(value.get("signing_key_id"),m.signing_key_id,0,64)||(!m.signing_key_id.empty()&&!hex(m.signing_key_id,64))||
       !token(value.get("source_commit"),m.source_commit,40,40)||!hex(m.source_commit,40)||!token(value.get("source_tree_sha256"),m.source_tree_sha256,64,64)||!hex(m.source_tree_sha256,64)||
       !value.get("source_dirty")||value.get("source_dirty")->type!=c::Json::Type::boolean||!c::decimal(value.get("rollback_readable_max"),m.rollback_readable_max)||!m.rollback_readable_max)return fail("Invalid release image, target or build identity");
    if(!std::all_of(m.release_id.begin(),m.release_id.end(),[](unsigned char b){return (b>='a'&&b<='z')||(b>='A'&&b<='Z')||(b>='0'&&b<='9')||b=='.'||b=='_'||b=='-';}))return fail("Invalid release identifier");
    const auto* profiles=value.get("hardware_profiles");if(!profiles||profiles->type!=c::Json::Type::array||profiles->array.size()!=1||!token(&profiles->array.front(),m.hardware_profile,1,64)||m.hardware_profile!=compiled_hardware_profile())return fail("Firmware does not support this board profile");
    const auto* mapping=value.get("hardware_mapping_sha256");
    if(m.hardware_profile=="esp32s3-development-5t-v1"){
        if(!token(mapping,m.hardware_mapping_sha256,64,64)||!hex(m.hardware_mapping_sha256,64)||m.hardware_mapping_sha256!=compiled_hardware_mapping_sha256())return fail("Firmware GPIO mapping differs from this board profile");
    }else if(mapping)return fail("Unexpected GPIO mapping identity for the legacy board profile");
    uint64_t origin_min,origin_max,origin_write,factory_min,factory_max,factory_write;
    if(!bounds(value.get("configuration"),m.config_read_min,m.config_read_max,m.config_write)||!bounds(value.get("storage"),m.storage_read_min,m.storage_read_max,m.storage_write)||
       !bounds(value.get("origin"),origin_min,origin_max,origin_write)||origin_min>1||origin_max<1||origin_write!=1||!bounds(value.get("factory"),factory_min,factory_max,factory_write)||factory_min>1||factory_max<4||factory_write!=4)return fail("Unsupported configuration, origin or factory format");
    const auto* protocol=value.get("protocol");uint64_t low,high,required,optional;
    if(!protocol||!c::closed(*protocol,{"min","max","required_capabilities","optional_capabilities"})||!c::decimal(protocol->get("min"),low)||!c::decimal(protocol->get("max"),high)||low>1||high<1||
       !c::decimal(protocol->get("required_capabilities"),required)||required>UINT32_MAX||!c::decimal(protocol->get("optional_capabilities"),optional)||optional>UINT32_MAX||(required&~c::known_capabilities))return fail("Unsupported consortium protocol or capabilities");
    m.required_capabilities=static_cast<uint32_t>(required);
    const auto* dependencies=value.get("dependencies");if(!dependencies||!c::closed(*dependencies,{"esp_idf","esp_matter","connectedhomeip","homekit"}))return fail("Missing dependency identities");
    for(const auto& dependency:dependencies->object){std::string commit;if(!token(&dependency.second,commit,40,40)||!hex(commit,40))return fail("Invalid dependency commit");}
    m.canonical=canonical(value);
    if(text!=m.canonical)return fail("Manifest must use canonical RFC8785 JSON bytes");
    out=std::move(m);return true;
}
bool compatible_release(const ReleaseManifest& m,uint64_t slot,uint64_t installed,uint64_t rollback,uint32_t capabilities,bool production,std::string& error){
    if(m.image_bytes>slot||m.slot_bytes>slot||m.image_bytes>m.slot_bytes){error="Image exceeds the installed OTA partition";return false;}
    if(m.config_read_min>2||m.config_read_max<2||m.config_write!=2||installed<m.storage_read_min||installed>m.storage_read_max||m.storage_write>rollback||m.storage_write>m.rollback_readable_max){error="Release is incompatible with installed configuration or rollback storage";return false;}
    if(m.required_capabilities&~capabilities){error="Release requires an unsupported control capability";return false;}
    if(production!=(m.security_profile=="production")){error="A remote update cannot change the provisioned security profile";return false;}
    if(production&&m.signing_key_id.empty()){error="Production release requires a trusted publisher signature";return false;}
    return true;
}
} // namespace system_services::updates
