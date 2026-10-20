// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#include "firmware_update.hpp"
#include "system_services.hpp"
#include "config_store.hpp"
#include "system_configuration.hpp"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "amity/consortium/json.hpp"
#include "amity/consortium/wire.hpp"
#include "psa/crypto.h"
#include "sdkconfig.h"
#include <mutex>
#include <algorithm>

#ifndef CONFIG_AMITY_PRODUCTION_UPDATES
#define CONFIG_AMITY_PRODUCTION_UPDATES 0
#endif
#ifndef CONFIG_AMITY_RELEASE_PUBLIC_KEY_DER_HEX
#define CONFIG_AMITY_RELEASE_PUBLIC_KEY_DER_HEX ""
#endif

#if CONFIG_AMITY_PRODUCTION_UPDATES && (!CONFIG_SECURE_BOOT_V2_ENABLED || !CONFIG_SECURE_FLASH_ENC_ENABLED || !CONFIG_SECURE_FLASH_ENCRYPTION_MODE_RELEASE || !CONFIG_NVS_ENCRYPTION || !CONFIG_NVS_SEC_KEY_PROTECT_USING_FLASH_ENC)
#error "Production updates require the reviewed native secure-boot and flash-encryption provisioning profile"
#endif
#if CONFIG_AMITY_PRODUCTION_UPDATES
static_assert(sizeof(CONFIG_AMITY_RELEASE_PUBLIC_KEY_DER_HEX)==183,"Production requires a provisioned P-256 publisher SPKI");
#endif
namespace system_services::updates {
namespace {
namespace c=amity::consortium;
std::mutex mutex;
std::function<esp_err_t()> begin_maintenance;
std::function<void()> end_maintenance;
std::optional<ReleaseManifest> staged;
uint64_t deadline=0,received=0;
std::string job,phase="idle",reason,old_boot;
bool installing=false;
std::string quote(std::string_view value){std::string out="\"";for(unsigned char b:value){if(b=='"'||b=='\\'){out+='\\';out+=char(b);}else if(b>=32&&b<127)out+=char(b);else out+='?';}return out+'"';}
std::string hex(const uint8_t* bytes,size_t count){constexpr char alphabet[]="0123456789abcdef";std::string out(2*count,'0');for(size_t n=0;n<count;++n){out[2*n]=alphabet[bytes[n]>>4];out[2*n+1]=alphabet[bytes[n]&15];}return out;}
bool decode(std::string_view in,std::string& out){if(in.size()%2)return false;out.clear();for(size_t n=0;n<in.size();n+=2){auto digit=[](char b){return b>='0'&&b<='9'?b-'0':b>='a'&&b<='f'?b-'a'+10:-1;};int a=digit(in[n]),b=digit(in[n+1]);if(a<0||b<0)return false;out+=char(a*16+b);}return true;}
uint64_t now(){return uint64_t(esp_timer_get_time()/1000);}
bool production(){return CONFIG_AMITY_PRODUCTION_UPDATES;}
std::string image_identity(){const auto* desc=esp_app_get_description();return hex(desc->app_elf_sha256,sizeof(desc->app_elf_sha256));}
std::string report(){
    return "{\"version\":1,\"job_id\":"+quote(job)+",\"target_octopus_id\":\"local\",\"release_id\":"+quote(staged?staged->release_id:"")+",\"signing_key_id\":"+quote(staged?staged->signing_key_id:"")+",\"phase\":"+quote(phase)+",\"error\":"+quote(reason)+",\"old_boot_id\":"+quote(old_boot)+",\"current_boot_id\":"+quote(boot_id())+",\"received_bytes\":\""+std::to_string(received)+"\",\"security_profile\":"+quote(production()?"production":"development")+",\"publisher_verified\":"+(staged&&!staged->signing_key_id.empty()?"true":"false")+",\"expected_image_sha256\":"+quote(staged?staged->image_sha256:"")+",\"expected_elf_sha256\":"+quote(staged?staged->image_elf_sha256:"")+",\"expected_version\":"+quote(staged?staged->firmware_version:"")+",\"running_elf_sha256\":"+quote(image_identity())+",\"running_version\":"+quote(esp_app_get_description()->version)+",\"local_boot_health\":"+(firmware_ready()?"true":"false")+",\"cohort_health\":\"requires_reconciliation\"}";
}
esp_err_t persist(){return config_store::write("ota_job",report());}
bool signature(const ReleaseManifest& manifest,std::string_view encoded){
    if(manifest.signing_key_id.empty())return encoded.empty()&&!production();
    // The trusted public root is provisioned at build/provisioning time. It is
    // separate from every operational peer, HTTPS and home-integration key.
    std::string public_der,raw;if(!decode(CONFIG_AMITY_RELEASE_PUBLIC_KEY_DER_HEX,public_der)||public_der.size()!=91||config_store::sha256_hex(public_der)!=manifest.signing_key_id||!decode(encoded,raw)||raw.size()!=64)return false;
    static constexpr uint8_t prefix[]={0x30,0x59,0x30,0x13,0x06,0x07,0x2a,0x86,0x48,0xce,0x3d,0x02,0x01,0x06,0x08,0x2a,0x86,0x48,0xce,0x3d,0x03,0x01,0x07,0x03,0x42,0x00};
    if(!std::equal(std::begin(prefix),std::end(prefix),reinterpret_cast<const uint8_t*>(public_der.data())))return false;
    psa_key_attributes_t attributes=PSA_KEY_ATTRIBUTES_INIT;psa_set_key_type(&attributes,PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_SECP_R1));psa_set_key_bits(&attributes,256);psa_set_key_usage_flags(&attributes,PSA_KEY_USAGE_VERIFY_HASH);psa_set_key_algorithm(&attributes,PSA_ALG_ECDSA(PSA_ALG_SHA_256));
    mbedtls_svc_key_id_t key=0;const auto imported=psa_import_key(&attributes,reinterpret_cast<const uint8_t*>(public_der.data()+sizeof(prefix)),65,&key);psa_reset_key_attributes(&attributes);if(imported!=PSA_SUCCESS)return false;
    uint8_t digest[32];size_t length=0;bool ok=psa_hash_compute(PSA_ALG_SHA_256,reinterpret_cast<const uint8_t*>(manifest.canonical.data()),manifest.canonical.size(),digest,sizeof(digest),&length)==PSA_SUCCESS&&length==32&&psa_verify_hash(key,PSA_ALG_ECDSA(PSA_ALG_SHA_256),digest,length,reinterpret_cast<const uint8_t*>(raw.data()),raw.size())==PSA_SUCCESS;psa_destroy_key(key);return ok;
}
void recover(){
    if(!job.empty()||phase!="idle")return;
    std::string saved;if(config_store::read("ota_job",saved)!=ESP_OK)return;c::Json root;c::JsonError detail;
    if(!c::parse_json(saved,root,detail)||!c::json_string(root.get("job_id"),job,32,32)||!c::json_string(root.get("phase"),phase,1,32)||!c::json_string(root.get("old_boot_id"),old_boot,32,32)){job.clear();phase="recovery_required";reason="Update report is corrupt; current firmware was retained";return;}
    ReleaseManifest manifest;c::json_string(root.get("release_id"),manifest.release_id,0,96);c::json_string(root.get("signing_key_id"),manifest.signing_key_id,0,64);c::json_string(root.get("expected_image_sha256"),manifest.image_sha256,64,64);c::json_string(root.get("expected_elf_sha256"),manifest.image_elf_sha256,64,64);c::json_string(root.get("expected_version"),manifest.firmware_version,1,31);staged=std::move(manifest);
    if(phase=="installing"||phase=="staged"||phase=="validating"){phase="interrupted";reason="Upload was interrupted; validate and send the complete image again";persist();}
    else if(phase=="boot_checking"&&old_boot!=boot_id())phase=staged->image_elf_sha256==image_identity()?"boot_checking":"rolled_back";
}
}
void maintenance_hooks(std::function<esp_err_t()> begin,std::function<void()> end){std::lock_guard<std::mutex> lock(mutex);begin_maintenance=std::move(begin);end_maintenance=std::move(end);}
esp_err_t stage_release(std::string_view bytes,std::string_view proof,std::string& error){
    ReleaseManifest manifest;if(!parse_release_manifest(bytes,manifest,error))return ESP_ERR_INVALID_ARG;
    const auto* partition=esp_ota_get_next_update_partition(nullptr);
    if(!partition||!compatible_release(manifest,partition->size,2,2,c::known_capabilities,production(),error)||!signature(manifest,proof)){if(error.empty())error="Publisher signature is missing, invalid or untrusted";return ESP_ERR_INVALID_ARG;}
    std::lock_guard<std::mutex> lock(mutex);if(installing){error="A firmware installation is already in progress";return ESP_ERR_INVALID_STATE;}
    uint8_t bytes_id[16];esp_fill_random(bytes_id,sizeof(bytes_id));job=hex(bytes_id,sizeof(bytes_id));staged=std::move(manifest);deadline=now()+300000;received=0;phase="staged";reason.clear();old_boot=boot_id();
    const auto stored=persist();if(stored!=ESP_OK){staged.reset();phase="failed";error="Update report could not be stored";}return stored;
}
std::string status_json(){std::lock_guard<std::mutex> lock(mutex);recover();return report();}
std::optional<ReleaseManifest> staged_release(){std::lock_guard<std::mutex> lock(mutex);return phase=="staged"&&now()<deadline?staged:std::nullopt;}
bool installation_active(){std::lock_guard<std::mutex> lock(mutex);return installing;}
esp_err_t begin_install(std::string& error){
    std::function<esp_err_t()> begin;{std::lock_guard<std::mutex> lock(mutex);if(installing||phase!="staged"||!staged||now()>=deadline){error="Validate the release manifest before uploading this exact image";return ESP_ERR_INVALID_STATE;}installing=true;begin=begin_maintenance;}
    const auto admitted=begin?begin():ESP_ERR_INVALID_STATE;
    if(admitted!=ESP_OK){install_failed("Control is still draining or unavailable; retry after it settles");error="Control could not enter its maintenance window";return admitted;}
    esp_err_t stored;std::function<void()> end;
    {std::lock_guard<std::mutex> lock(mutex);phase="installing";received=0;stored=persist();if(stored!=ESP_OK){installing=false;phase="failed";end=end_maintenance;error="Update report could not be stored";}}
    if(end)end();
    return stored;
}
void install_progress(uint64_t bytes){std::lock_guard<std::mutex> lock(mutex);received=bytes;}
esp_err_t install_checkpoint(uint64_t bytes){std::lock_guard<std::mutex> lock(mutex);received=bytes;return persist();}
esp_err_t install_selected(){std::lock_guard<std::mutex> lock(mutex);phase="boot_checking";return persist();}
void install_failed(const std::string& error){std::function<void()> end;{std::lock_guard<std::mutex> lock(mutex);phase="failed";reason=error;installing=false;persist();end=end_maintenance;}if(end)end();}
void local_boot_healthy(){std::lock_guard<std::mutex> lock(mutex);recover();if(phase=="boot_checking"&&staged&&old_boot!=boot_id()){phase=staged->image_elf_sha256==image_identity()&&staged->firmware_version==esp_app_get_description()->version?"healthy":"rolled_back";reason=phase=="healthy"?"":"The selected image did not reach local boot health";persist();}}
} // namespace system_services::updates
