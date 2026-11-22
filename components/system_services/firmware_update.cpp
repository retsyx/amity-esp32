// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#include "firmware_update.hpp"
#include "partition_image.hpp"
#include "config_store.hpp"
#include "system_configuration.hpp"
#include "esp_ota_ops.h"
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
std::mutex mutex;
std::function<esp_err_t()> begin_hook;
std::function<void()> end_hook;
bool decode(std::string_view in,std::string& out){if(in.size()%2)return false;out.clear();for(size_t n=0;n<in.size();n+=2){auto digit=[](char b){return b>='0'&&b<='9'?b-'0':b>='a'&&b<='f'?b-'a'+10:-1;};int a=digit(in[n]),b=digit(in[n+1]);if(a<0||b<0)return false;out+=char(a*16+b);}return true;}
// Production firmware accepts only releases signed by its provisioned publisher.
bool signature(const ReleaseManifest& manifest,std::string_view encoded){
    if(manifest.signing_key_id.empty())return encoded.empty()&&!CONFIG_AMITY_PRODUCTION_UPDATES;
    // The trusted public root is provisioned at build/provisioning time. It is
    // separate from every operational peer, HTTPS and home-integration key.
    std::string public_der,raw;if(!decode(CONFIG_AMITY_RELEASE_PUBLIC_KEY_DER_HEX,public_der)||public_der.size()!=91||config_store::sha256_hex(public_der)!=manifest.signing_key_id||!decode(encoded,raw)||raw.size()!=64)return false;
    static constexpr uint8_t prefix[]={0x30,0x59,0x30,0x13,0x06,0x07,0x2a,0x86,0x48,0xce,0x3d,0x02,0x01,0x06,0x08,0x2a,0x86,0x48,0xce,0x3d,0x03,0x01,0x07,0x03,0x42,0x00};
    if(!std::equal(std::begin(prefix),std::end(prefix),reinterpret_cast<const uint8_t*>(public_der.data())))return false;
    psa_key_attributes_t attributes=PSA_KEY_ATTRIBUTES_INIT;psa_set_key_type(&attributes,PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_SECP_R1));psa_set_key_bits(&attributes,256);psa_set_key_usage_flags(&attributes,PSA_KEY_USAGE_VERIFY_HASH);psa_set_key_algorithm(&attributes,PSA_ALG_ECDSA(PSA_ALG_SHA_256));
    mbedtls_svc_key_id_t key=0;const auto imported=psa_import_key(&attributes,reinterpret_cast<const uint8_t*>(public_der.data()+sizeof(prefix)),65,&key);psa_reset_key_attributes(&attributes);if(imported!=PSA_SUCCESS)return false;
    uint8_t digest[32];size_t length=0;bool ok=psa_hash_compute(PSA_ALG_SHA_256,reinterpret_cast<const uint8_t*>(manifest.canonical.data()),manifest.canonical.size(),digest,sizeof(digest),&length)==PSA_SUCCESS&&length==32&&psa_verify_hash(key,PSA_ALG_ECDSA(PSA_ALG_SHA_256),digest,length,reinterpret_cast<const uint8_t*>(raw.data()),raw.size())==PSA_SUCCESS;psa_destroy_key(key);return ok;
}
}
void maintenance_hooks(std::function<esp_err_t()> begin,std::function<void()> end){std::lock_guard<std::mutex> lock(mutex);begin_hook=std::move(begin);end_hook=std::move(end);}
esp_err_t validate_release(std::string_view bytes,std::string_view proof,ReleaseManifest& manifest,std::string& error){
    if(bytes.size()>4096){error="This is not a valid Amity update file.";return ESP_ERR_INVALID_ARG;}
    if(!parse_release_manifest(bytes,manifest,error))return ESP_ERR_INVALID_ARG;
    const auto* partition=esp_ota_get_next_update_partition(nullptr);
    if(!partition||partition->size<=partition_release_bytes||manifest.image_bytes>partition->size-partition_release_bytes){error="This firmware is too large for Amity.";return ESP_ERR_INVALID_ARG;}
    if(!signature(manifest,proof)){error="This update is not signed by a trusted publisher.";return ESP_ERR_INVALID_ARG;}
    return ESP_OK;
}
esp_err_t enter_maintenance(std::string& error){
    std::function<esp_err_t()> begin;{std::lock_guard<std::mutex> lock(mutex);begin=begin_hook;}
    const auto entered=begin?begin():ESP_ERR_INVALID_STATE;
    if(entered!=ESP_OK)error="Amity is busy. Try the update again shortly.";
    return entered;
}
void leave_maintenance(){std::function<void()> end;{std::lock_guard<std::mutex> lock(mutex);end=end_hook;}if(end)end();}
} // namespace system_services::updates
