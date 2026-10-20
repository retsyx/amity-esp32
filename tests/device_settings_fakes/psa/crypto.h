#pragma once
#include <cstddef>
#include <cstdint>
using psa_status_t=int;
constexpr psa_status_t PSA_SUCCESS=0;
constexpr unsigned PSA_ALG_SHA_256=0x02000009;
psa_status_t psa_crypto_init();
psa_status_t psa_hash_compute(unsigned,const uint8_t*,size_t,uint8_t*,size_t,size_t*);
