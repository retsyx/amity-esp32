#include "hap.h"
#include "mu_srp.h"
#include "esp_err.h"
#include "esp_random.h"
#include "psa/crypto.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
esp_err_t amity_homekit_generate_setup(const char *code,hap_setup_info_t *setup) {
    if(psa_crypto_init()!=PSA_SUCCESS)return ESP_FAIL;
    memset(setup,0,sizeof(*setup));esp_fill_random(setup->salt,sizeof(setup->salt));
    char password[22];snprintf(password,sizeof(password),"Pair-Setup:%s",code);
    uint8_t inner[64],combined[80],digest[64];size_t len;
    if(psa_hash_compute(PSA_ALG_SHA_512,(uint8_t *)password,strlen(password),inner,sizeof(inner),&len)!=PSA_SUCCESS)return ESP_FAIL;
    memcpy(combined,setup->salt,16);memcpy(combined+16,inner,64);
    if(psa_hash_compute(PSA_ALG_SHA_512,combined,sizeof(combined),digest,sizeof(digest),&len)!=PSA_SUCCESS)return ESP_FAIL;
    mu_srp_handle_t srp={0};if(mu_srp_init(&srp,MU_NG_3072)<0)return ESP_ERR_NO_MEM;
    mu_bn_t *x=mu_bn_new_from_bin((char *)digest,64),*v=mu_bn_new();
    int size=0;char *bytes=NULL;
    if(x&&v&&mu_bn_a_exp_b_mod_c(v,srp.g,x,srp.n,srp.ctx)==0)bytes=mu_bn_to_bin(v,&size);
    const bool valid=bytes&&size>0&&size<=384;
    if(valid)memcpy(setup->verifier+384-size,bytes,size);
    free(bytes);if(x)mu_bn_free(x);if(v)mu_bn_free(v);mu_srp_free(&srp);
    memset(password,0,sizeof(password));memset(inner,0,sizeof(inner));memset(digest,0,sizeof(digest));memset(combined,0,sizeof(combined));
    return valid?ESP_OK:ESP_FAIL;
}
