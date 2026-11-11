// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#include "partition_image.hpp"
#include "config_store.hpp"
#include "system_configuration.hpp"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include <array>
#include <cstring>

namespace system_services::updates {
namespace {
esp_err_t partition_flash(const std::function<esp_err_t()>& call){esp_err_t result=ESP_FAIL;return config_store::with_internal_flash_stack([&]{result=call();})?result:ESP_FAIL;}
std::string hex(std::string_view input){constexpr char digits[]="0123456789abcdef";std::string out;out.reserve(input.size()*2);for(unsigned char b:input){out+=digits[b>>4];out+=digits[b&15];}return out;}
bool unhex(std::string_view input,std::string& out){if(input.size()%2)return false;out.clear();for(size_t i=0;i<input.size();i+=2){const auto digit=[](char c){return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:-1;};const int a=digit(input[i]),b=digit(input[i+1]);if(a<0||b<0)return false;out+=char((a<<4)|b);}return true;}
uint64_t number(const char* data,size_t count){uint64_t out=0;for(size_t i=0;i<count;++i)out=(out<<8)|static_cast<unsigned char>(data[i]);return out;}
void number(char* data,uint64_t value,size_t count){for(size_t i=count;i>0;--i){data[i-1]=char(value);value>>=8;}}
struct Header {std::array<char,partition_release_header_bytes> bytes{};uint32_t manifest=0,signature=0;uint64_t image=0;};
esp_err_t header(const esp_partition_t* partition,Header& out,std::string& error){
    if(!partition||partition->size<=partition_release_bytes){error="Firmware partition is unavailable";return ESP_ERR_INVALID_STATE;}
    const auto result=partition_flash([&]{return esp_partition_read(partition,partition->size-partition_release_bytes,out.bytes.data(),out.bytes.size());});
    if(result!=ESP_OK){error="Firmware release information could not be read";return result;}
    out.manifest=number(out.bytes.data()+16,4);out.signature=number(out.bytes.data()+20,4);out.image=number(out.bytes.data()+24,8);
    if(memcmp(out.bytes.data(),partition_release_magic,16)||!out.manifest||out.manifest>8192||(out.signature!=0&&out.signature!=64)||
       !out.image||out.image>partition->size-partition_release_bytes||partition_release_header_bytes+out.manifest+out.signature>partition_release_bytes){
        error="Firmware partition has no complete release information";return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}
}
esp_err_t erase_partition_release(const esp_partition_t* partition){
    if(!partition||partition->size<=partition_release_bytes)return ESP_ERR_INVALID_ARG;
    return partition_flash([&]{return esp_partition_erase_range(partition,partition->size-partition_release_bytes,partition_release_bytes);});
}
esp_err_t write_partition_release(const esp_partition_t* partition,const ReleaseManifest& release,std::string_view signature_hex,std::string& error){
    std::string signature;
    if(!partition||partition->size<=partition_release_bytes||release.image_bytes>partition->size-partition_release_bytes||release.canonical.empty()||release.canonical.size()>8192||
       !unhex(signature_hex,signature)||(signature.size()!=0&&signature.size()!=64)){error="Release information does not fit the firmware partition";return ESP_ERR_INVALID_ARG;}
    std::string payload=release.canonical+signature,digest;
    if(!unhex(config_store::sha256_hex(payload),digest)||digest.size()!=32)return ESP_FAIL;
    std::string storage(partition_release_bytes,'\xff');memcpy(storage.data(),partition_release_magic,16);
    number(storage.data()+16,release.canonical.size(),4);number(storage.data()+20,signature.size(),4);number(storage.data()+24,release.image_bytes,8);
    memcpy(storage.data()+32,digest.data(),digest.size());memcpy(storage.data()+partition_release_header_bytes,payload.data(),payload.size());
    auto result=erase_partition_release(partition);
    // Commit the header last. A partial trailer never advertises a staged image.
    if(result==ESP_OK)result=partition_flash([&]{return esp_partition_write(partition,partition->size-partition_release_bytes+partition_release_header_bytes,storage.data()+partition_release_header_bytes,storage.size()-partition_release_header_bytes);});
    if(result==ESP_OK)result=partition_flash([&]{return esp_partition_write(partition,partition->size-partition_release_bytes,storage.data(),partition_release_header_bytes);});
    if(result!=ESP_OK)error="Firmware release information could not be stored";
    return result;
}
namespace {
esp_err_t read_partition_release_direct(const esp_partition_t* partition,ReleaseManifest& release,std::string& signature_hex,std::string& error){
    Header info;auto result=header(partition,info,error);if(result!=ESP_OK)return result;
    std::string payload(info.manifest+info.signature,'\0');
    result=partition_flash([&]{return esp_partition_read(partition,partition->size-partition_release_bytes+partition_release_header_bytes,payload.data(),payload.size());});
    if(result!=ESP_OK){error="Firmware release information could not be read";return result;}
    if(config_store::sha256_hex(payload)!=hex(std::string_view(info.bytes.data()+32,32))){error="Firmware release information is incomplete";return ESP_ERR_INVALID_CRC;}
    signature_hex=hex(std::string_view(payload).substr(info.manifest));
    result=validate_release(std::string_view(payload).substr(0,info.manifest),signature_hex,release,error);if(result!=ESP_OK)return result;
    if(release.image_bytes!=info.image){error="Firmware image length differs from its release";return ESP_ERR_INVALID_CRC;}
    esp_app_desc_t app{};result=partition_flash([&]{return esp_ota_get_partition_description(partition,&app);});
    if(result!=ESP_OK||!memchr(app.version,0,sizeof(app.version))||release.firmware_version!=app.version||
       memcmp(app.project_name,esp_app_get_description()->project_name,sizeof(app.project_name))||
       release.image_elf_sha256!=hex(std::string_view(reinterpret_cast<const char*>(app.app_elf_sha256),32))){error="Firmware image identity differs from its release";return ESP_ERR_INVALID_CRC;}
    // The trailer is committed only after the receiving stream and native OTA
    // image pass validation. Reading its release must not hash the image again.
    return ESP_OK;
}
esp_err_t read_partition_image_direct(const esp_partition_t* partition,uint64_t offset,size_t count,std::string& bytes,std::string& error){
    Header info;const auto result=header(partition,info,error);if(result!=ESP_OK)return result;
    if(!count||count>2048||offset>info.image||count>info.image-offset){error="Firmware read is outside the validated image";return ESP_ERR_INVALID_ARG;}
    bytes.resize(count);const auto read=partition_flash([&]{return esp_partition_read(partition,offset,bytes.data(),count);});if(read!=ESP_OK){bytes.clear();error="Firmware image could not be read";}return read;
}
}
esp_err_t read_partition_release(const esp_partition_t* partition,ReleaseManifest& release,std::string& signature_hex,std::string& error){
    // Read the release and application descriptor in one internal I/O dispatch.
    return partition_flash([&]{return read_partition_release_direct(partition,release,signature_hex,error);});
}
esp_err_t read_partition_image(const esp_partition_t* partition,uint64_t offset,size_t count,std::string& bytes,std::string& error){
    return partition_flash([&]{return read_partition_image_direct(partition,offset,count,bytes,error);});
}
}
