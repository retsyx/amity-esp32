#pragma once
#include <cstddef>
#include <cstdint>
using psa_status_t=int;using psa_algorithm_t=uint32_t;using mbedtls_svc_key_id_t=uint32_t;
constexpr psa_status_t PSA_SUCCESS=0;
constexpr psa_algorithm_t PSA_ALG_SHA_256=0x02000009;
#define PSA_ALG_ECDSA(hash) (0x06000600u|((hash)&0xffu))
#define PSA_ECC_FAMILY_SECP_R1 0x12
#define PSA_KEY_TYPE_ECC_PUBLIC_KEY(curve) (0x4100u|(curve))
constexpr uint32_t PSA_KEY_USAGE_VERIFY_HASH=0x2000;
struct psa_key_attributes_t {uint32_t type=0,bits=0,usage=0,alg=0;};
#define PSA_KEY_ATTRIBUTES_INIT psa_key_attributes_t{}
inline void psa_set_key_type(psa_key_attributes_t* a,uint32_t t){a->type=t;}
inline void psa_set_key_bits(psa_key_attributes_t* a,uint32_t b){a->bits=b;}
inline void psa_set_key_usage_flags(psa_key_attributes_t* a,uint32_t u){a->usage=u;}
inline void psa_set_key_algorithm(psa_key_attributes_t* a,uint32_t g){a->alg=g;}
inline void psa_reset_key_attributes(psa_key_attributes_t* a){*a={};}
inline psa_status_t psa_import_key(const psa_key_attributes_t*,const uint8_t*,size_t,mbedtls_svc_key_id_t*){return -1;}
inline psa_status_t psa_destroy_key(mbedtls_svc_key_id_t){return PSA_SUCCESS;}
inline psa_status_t psa_hash_compute(psa_algorithm_t,const uint8_t*,size_t,uint8_t*,size_t,size_t*){return -1;}
inline psa_status_t psa_verify_hash(mbedtls_svc_key_id_t,psa_algorithm_t,const uint8_t*,size_t,const uint8_t*,size_t){return -1;}
struct psa_hash_operation_t {int active=0;};
#define PSA_HASH_OPERATION_INIT psa_hash_operation_t{}
inline psa_status_t psa_hash_setup(psa_hash_operation_t* o,psa_algorithm_t){o->active=1;return PSA_SUCCESS;}
inline psa_status_t psa_hash_update(psa_hash_operation_t*,const uint8_t*,size_t){return PSA_SUCCESS;}
inline psa_status_t psa_hash_finish(psa_hash_operation_t* o,uint8_t*,size_t,size_t* n){o->active=0;*n=32;return PSA_SUCCESS;}
inline psa_status_t psa_hash_abort(psa_hash_operation_t* o){o->active=0;return PSA_SUCCESS;}
