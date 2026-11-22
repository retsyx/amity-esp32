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
}
