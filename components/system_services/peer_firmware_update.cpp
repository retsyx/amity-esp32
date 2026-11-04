// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#include "firmware_update.hpp"
#include "partition_image.hpp"
#include "config_store.hpp"
#include "system_configuration.hpp"
#include "system_services.hpp"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_image_format.h"
#include "esp_timer.h"
#include "psa/crypto.h"
#include <mutex>
#include <cstring>
#include <algorithm>
namespace system_services::updates {
namespace {
std::mutex gate;
std::optional<ReleaseManifest> release;
esp_ota_handle_t handle=0;
psa_hash_operation_t digest=PSA_HASH_OPERATION_INIT;
uint64_t offset=0,last_offset=0;
int64_t last_progress_us=0;
std::string last_hash;
std::string release_signature;
const esp_partition_t* receiving_partition=nullptr;
esp_err_t flash(const std::function<esp_err_t()>& call){esp_err_t result=ESP_FAIL;return config_store::with_internal_flash_stack([&]{result=call();})?result:ESP_FAIL;}
void abort_locked(const std::string& error){if(handle)flash([]{return esp_ota_abort(handle);});handle=0;psa_hash_abort(&digest);release.reset();release_signature.clear();receiving_partition=nullptr;last_hash.clear();offset=0;install_failed(error);}
bool header(std::string_view bytes,const ReleaseManifest& expected){
    constexpr size_t size=sizeof(esp_image_header_t)+sizeof(esp_image_segment_header_t)+sizeof(esp_app_desc_t);
    if(bytes.size()<size)return false;
    esp_image_header_t image;esp_app_desc_t app;
    memcpy(&image,bytes.data(),sizeof(image));memcpy(&app,bytes.data()+sizeof(image)+sizeof(esp_image_segment_header_t),sizeof(app));
    if(image.magic!=ESP_IMAGE_HEADER_MAGIC||image.chip_id!=CONFIG_IDF_FIRMWARE_CHIP_ID||app.magic_word!=ESP_APP_DESC_MAGIC_WORD||memcmp(app.project_name,esp_app_get_description()->project_name,sizeof(app.project_name))||!memchr(app.version,0,sizeof(app.version))||expected.firmware_version!=app.version)return false;
    char elf[65]{};for(size_t n=0;n<sizeof(app.app_elf_sha256);++n)snprintf(elf+2*n,3,"%02x",app.app_elf_sha256[n]);return expected.image_elf_sha256==elf;
}
}
esp_err_t peer_begin(std::string_view manifest,std::string_view signature_hex,std::string& error){
    std::lock_guard<std::mutex> lock(gate);
    if(release){ReleaseManifest candidate;if(parse_release_manifest(manifest,candidate,error)&&candidate.canonical==release->canonical)return ESP_OK;error="Another peer release is already being received";return ESP_ERR_INVALID_STATE;}
    auto result=stage_release(manifest,signature_hex,error);if(result!=ESP_OK)return result;
    release=staged_release();if(!release){error="Release admission expired";return ESP_ERR_INVALID_STATE;}
    result=begin_install(error);if(result!=ESP_OK){release.reset();return result;}
    if(psa_hash_setup(&digest,PSA_ALG_SHA_256)!=PSA_SUCCESS){error="Image digest initialization failed";abort_locked(error);return ESP_FAIL;}
    offset=0;last_hash.clear();release_signature=signature_hex;receiving_partition=nullptr;last_progress_us=esp_timer_get_time();return ESP_OK;
}
esp_err_t peer_chunk(std::string_view release_id,uint64_t requested,std::string_view bytes,std::string& error){
    std::lock_guard<std::mutex> lock(gate);
    if(!release||release->release_id!=release_id||bytes.empty()||bytes.size()>2048){error="No matching admitted release";return ESP_ERR_INVALID_ARG;}
    if(!last_hash.empty()&&requested==last_offset&&config_store::sha256_hex(bytes)==last_hash)return ESP_OK;
    if(requested!=offset||offset>=release->image_bytes||bytes.size()!=std::min<uint64_t>(2048,release->image_bytes-offset)){error="Chunk offset or size does not match the target checkpoint";return ESP_ERR_INVALID_ARG;}
    if(!offset){
        if(!header(bytes,*release)){error="Application header differs from the validated release";abort_locked(error);return ESP_ERR_INVALID_ARG;}
        receiving_partition=esp_ota_get_next_update_partition(nullptr);
        if(!receiving_partition||erase_partition_release(receiving_partition)!=ESP_OK||flash([&]{return esp_ota_begin(receiving_partition,release->image_bytes,&handle);})!=ESP_OK){error="Inactive OTA slot could not be opened";abort_locked(error);return ESP_FAIL;}
    }
    if(flash([&]{return esp_ota_write(handle,bytes.data(),bytes.size());})!=ESP_OK||psa_hash_update(&digest,reinterpret_cast<const uint8_t*>(bytes.data()),bytes.size())!=PSA_SUCCESS){error="Image chunk could not be stored";abort_locked(error);return ESP_FAIL;}
    const auto next=offset+bytes.size();
    // Interrupted streams restart at zero; only phase changes need persistence.
    install_progress(next);
    last_offset=offset;last_hash=config_store::sha256_hex(bytes);offset=next;last_progress_us=esp_timer_get_time();return ESP_OK;
}
esp_err_t peer_stage(std::string_view release_id,std::string& error){
    std::lock_guard<std::mutex> lock(gate);
    if(!release){ReleaseManifest existing;std::string proof;const auto result=read_partition_release(esp_ota_get_next_update_partition(nullptr),existing,proof,error);if(result==ESP_OK&&existing.release_id==release_id)return ESP_OK;if(result==ESP_OK)error="Another firmware image is staged";return ESP_ERR_INVALID_STATE;}
    if(release->release_id!=release_id||offset!=release->image_bytes||!handle){error="The exact release image has not been completely received";return ESP_ERR_INVALID_STATE;}
    uint8_t hash[32];size_t count=0;char text[65]{};
    if(psa_hash_finish(&digest,hash,sizeof(hash),&count)!=PSA_SUCCESS||count!=32){error="Image digest failed";abort_locked(error);return ESP_FAIL;}
    for(size_t n=0;n<sizeof(hash);++n)snprintf(text+2*n,3,"%02x",hash[n]);
    if(release->image_sha256!=text){error="Image digest does not match the admitted release";abort_locked(error);return ESP_ERR_INVALID_CRC;}
    auto result=flash([]{return esp_ota_end(handle);});handle=0;
    if(result==ESP_OK)result=write_partition_release(receiving_partition,*release,release_signature,error);
    if(result!=ESP_OK){if(error.empty())error="Firmware image validation failed";abort_locked(error);return result;}
    release.reset();release_signature.clear();receiving_partition=nullptr;last_hash.clear();offset=0;
    // The verified partition is self-contained. Losing RAM, TLS or power from
    // here cannot select it for boot, and no upload timeout applies to it.
    return install_staged();
}
esp_err_t peer_install(std::string_view release_id,std::string& error,uint32_t reboot_delay_ms){
    const auto activation_started=esp_timer_get_time();
    std::lock_guard<std::mutex> lock(gate);
    if(release||reboot_delay_ms>15000){error="Finish staging the firmware before activating it";return ESP_ERR_INVALID_STATE;}
    const auto* partition=esp_ota_get_next_update_partition(nullptr);ReleaseManifest staged;std::string proof;
    auto result=read_partition_release(partition,staged,proof,error);if(result!=ESP_OK)return result;
    if(staged.release_id!=release_id){error="The requested firmware image is not staged";return ESP_ERR_INVALID_STATE;}
    result=stage_release(staged.canonical,proof,error);if(result!=ESP_OK)return result;
    result=begin_install(error);if(result!=ESP_OK)return result;
    result=install_selected();
    if(result==ESP_OK)result=flash([&]{return esp_ota_set_boot_partition(partition);});
    if(result!=ESP_OK){error="Firmware boot selection failed";install_failed(error);return result;}
    const auto elapsed=uint64_t(std::max<int64_t>(0,esp_timer_get_time()-activation_started)/1000);
    reboot_later(elapsed<reboot_delay_ms?uint32_t(reboot_delay_ms-elapsed):0);return ESP_OK;
}
esp_err_t partition_release(bool running,ReleaseManifest& value,std::string& signature_hex,std::string& error){
    std::lock_guard<std::mutex> lock(gate);
    return read_partition_release(running?esp_ota_get_running_partition():esp_ota_get_next_update_partition(nullptr),value,signature_hex,error);
}
esp_err_t read_running_image(uint64_t requested,size_t count,std::string& bytes,std::string& error){std::lock_guard<std::mutex> lock(gate);return read_partition_image(esp_ota_get_running_partition(),requested,count,bytes,error);}
esp_err_t read_staged_image(uint64_t requested,size_t count,std::string& bytes,std::string& error){std::lock_guard<std::mutex> lock(gate);if(release){error="Firmware upload is still in progress";return ESP_ERR_INVALID_STATE;}return read_partition_image(esp_ota_get_next_update_partition(nullptr),requested,count,bytes,error);}
void peer_abort(const std::string& error){std::lock_guard<std::mutex> lock(gate);if(release)abort_locked(error);}
void poll_peer_timeout(){std::lock_guard<std::mutex> lock(gate);if(release&&esp_timer_get_time()-last_progress_us>120000000)abort_locked("Peer upload stopped making progress; current image retained");}
} // namespace system_services::updates
