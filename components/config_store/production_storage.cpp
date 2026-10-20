// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#include "production_storage.hpp"
#include "psa/crypto.h"
#include <cstring>
namespace config_store {
bool derive_production_storage_key(std::string_view partition,const uint8_t root[64],uint8_t output[64]){
    if(!root||!output||(partition!="amity_cfg"&&partition!="fctry"))return false;
    static constexpr char salt[]="Amity production NVS v1";
    const auto info=partition=="amity_cfg"?std::string_view("amity/nvs/partition/amity_cfg"):std::string_view("amity/nvs/partition/fctry");
    if(psa_crypto_init()!=PSA_SUCCESS)return false;
    psa_key_derivation_operation_t operation=PSA_KEY_DERIVATION_OPERATION_INIT;
    const bool valid=psa_key_derivation_setup(&operation,PSA_ALG_HKDF(PSA_ALG_SHA_256))==PSA_SUCCESS&&
        psa_key_derivation_input_bytes(&operation,PSA_KEY_DERIVATION_INPUT_SALT,reinterpret_cast<const uint8_t*>(salt),sizeof(salt)-1)==PSA_SUCCESS&&
        psa_key_derivation_input_bytes(&operation,PSA_KEY_DERIVATION_INPUT_SECRET,root,64)==PSA_SUCCESS&&
        psa_key_derivation_input_bytes(&operation,PSA_KEY_DERIVATION_INPUT_INFO,reinterpret_cast<const uint8_t*>(info.data()),info.size())==PSA_SUCCESS&&
        psa_key_derivation_output_bytes(&operation,output,64)==PSA_SUCCESS;
    psa_key_derivation_abort(&operation);
    if(!valid){volatile uint8_t* bytes=output;for(size_t n=0;n<64;++n)bytes[n]=0;}
    return valid;
}
bool production_storage_self_test(){
    // Public synthetic vectors independently computed with RFC5869 HKDF-SHA256.
    uint8_t root[64],output[64];for(size_t n=0;n<64;++n)root[n]=static_cast<uint8_t>(n);
    const char* vectors[]={"d18780a0fe11189ccfcb13b418ac3801db33224df7aa339024bd03b8c07199d089606d9e434526e9c2ad8e5820e7bf2fbc3d5d2733b509c38314620687ddc76b","e1b5a4d51e255acdb0be4899a6edbe9bf958094a47aa4262eb42a272590f5eecb19b1aef0a36fd157ab223d7c3e44f5ee9d663653c8b705096c8749d1fa6152e"};
    for(size_t n=0;n<2;++n){if(!derive_production_storage_key(n?"fctry":"amity_cfg",root,output))return false;for(size_t i=0;i<64;++i){const auto digit=[](char c){return c<='9'?c-'0':c-'a'+10;};if(output[i]!=digit(vectors[n][2*i])*16+digit(vectors[n][2*i+1]))return false;}}
    return !derive_production_storage_key("nvs",root,output)&&!derive_production_storage_key("other",root,output);
}
}
