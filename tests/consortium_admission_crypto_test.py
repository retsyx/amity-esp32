#!/usr/bin/env python3
"""Run actual admission code with real OpenSSL-backed PSA primitives."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PREFIX = r'''
#include "admission_crypto.hpp"
#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/params.h>
#include <openssl/rand.h>
#include <map>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <cassert>
#include <iostream>
using psa_key_id_t=unsigned;
constexpr int PSA_SUCCESS=0,PSA_ALG_SHA_256=1,PSA_ALG_ECDH=2,PSA_ALG_GCM=3;
constexpr int PSA_ECC_FAMILY_SECP_R1=1,PSA_KEY_USAGE_DERIVE=1,PSA_KEY_USAGE_ENCRYPT=2,PSA_KEY_USAGE_DECRYPT=4,PSA_KEY_TYPE_AES=2;
constexpr int PSA_KEY_DERIVATION_INPUT_SALT=1,PSA_KEY_DERIVATION_INPUT_SECRET=2,PSA_KEY_DERIVATION_INPUT_INFO=3;
#define PSA_KEY_TYPE_ECC_KEY_PAIR(x) (x)
#define PSA_ALG_HKDF(x) 4
struct psa_key_attributes_t{int type=0;};
#define PSA_KEY_ATTRIBUTES_INIT {}
struct psa_key_derivation_operation_t{std::string salt,secret,info;};
#define PSA_KEY_DERIVATION_OPERATION_INIT {}
struct Key{EVP_PKEY* ec=nullptr;std::vector<unsigned char> aes;};
std::map<unsigned,Key> key_store;unsigned next_key=0;
bool fail_key_generation=false;
int psa_crypto_init(){return 0;}
void psa_set_key_type(psa_key_attributes_t* a,int type){a->type=type;}
void psa_set_key_bits(psa_key_attributes_t*,int){}
void psa_set_key_usage_flags(psa_key_attributes_t*,int){}
void psa_set_key_algorithm(psa_key_attributes_t*,int){}
int psa_destroy_key(unsigned id){auto found=key_store.find(id);if(found!=key_store.end()){EVP_PKEY_free(found->second.ec);if(!found->second.aes.empty())OPENSSL_cleanse(found->second.aes.data(),found->second.aes.size());key_store.erase(found);}return 0;}
int psa_generate_key(const psa_key_attributes_t*,unsigned* id){if(fail_key_generation)return -1;EVP_PKEY_CTX* context=EVP_PKEY_CTX_new_from_name(nullptr,"EC",nullptr);EVP_PKEY* key=nullptr;bool good=context&&EVP_PKEY_keygen_init(context)>0&&EVP_PKEY_CTX_set_group_name(context,"prime256v1")>0&&EVP_PKEY_generate(context,&key)>0;EVP_PKEY_CTX_free(context);if(!good)return -1;*id=++next_key;key_store[*id].ec=key;return 0;}
int psa_export_public_key(unsigned id,uint8_t* out,size_t capacity,size_t* size){return EVP_PKEY_get_octet_string_param(key_store.at(id).ec,OSSL_PKEY_PARAM_PUB_KEY,out,capacity,size)>0?0:-1;}
int psa_generate_random(uint8_t* out,size_t size){return RAND_bytes(out,size)>0?0:-1;}
int psa_hash_compute(int,const uint8_t* in,size_t size,uint8_t* out,size_t capacity,size_t* length){unsigned count=0;if(capacity<32||EVP_Digest(in,size,out,&count,EVP_sha256(),nullptr)<=0)return -1;*length=count;return 0;}
int psa_raw_key_agreement(int,unsigned id,const uint8_t* input,size_t count,uint8_t* output,size_t capacity,size_t* length){
 char group[]="prime256v1";OSSL_PARAM parameters[]={OSSL_PARAM_construct_utf8_string(OSSL_PKEY_PARAM_GROUP_NAME,group,0),OSSL_PARAM_construct_octet_string(OSSL_PKEY_PARAM_PUB_KEY,const_cast<uint8_t*>(input),count),OSSL_PARAM_construct_end()};
 auto* parse=EVP_PKEY_CTX_new_from_name(nullptr,"EC",nullptr);EVP_PKEY* peer=nullptr;bool good=parse&&EVP_PKEY_fromdata_init(parse)>0&&EVP_PKEY_fromdata(parse,&peer,EVP_PKEY_PUBLIC_KEY,parameters)>0;EVP_PKEY_CTX_free(parse);
 auto* derive=good?EVP_PKEY_CTX_new(key_store.at(id).ec,nullptr):nullptr;*length=capacity;good=good&&derive&&EVP_PKEY_derive_init(derive)>0&&EVP_PKEY_derive_set_peer(derive,peer)>0&&EVP_PKEY_derive(derive,output,length)>0;EVP_PKEY_CTX_free(derive);EVP_PKEY_free(peer);return good?0:-1;
}
int psa_key_derivation_setup(psa_key_derivation_operation_t*,int){return 0;}
int psa_key_derivation_input_bytes(psa_key_derivation_operation_t* op,int step,const uint8_t* bytes,size_t size){auto* out=step==1?&op->salt:step==2?&op->secret:&op->info;out->assign(reinterpret_cast<const char*>(bytes),size);return 0;}
int psa_key_derivation_output_bytes(psa_key_derivation_operation_t* op,uint8_t* output,size_t size){
 auto* kdf=EVP_KDF_fetch(nullptr,"HKDF",nullptr);auto* context=kdf?EVP_KDF_CTX_new(kdf):nullptr;char digest[]="SHA256";
 OSSL_PARAM parameters[]={OSSL_PARAM_construct_utf8_string(OSSL_KDF_PARAM_DIGEST,digest,0),OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_KEY,op->secret.data(),op->secret.size()),OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SALT,op->salt.data(),op->salt.size()),OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_INFO,op->info.data(),op->info.size()),OSSL_PARAM_construct_end()};
 bool good=context&&EVP_KDF_derive(context,output,size,parameters)>0;EVP_KDF_CTX_free(context);EVP_KDF_free(kdf);return good?0:-1;
}
int psa_key_derivation_abort(psa_key_derivation_operation_t* op){if(!op->secret.empty())OPENSSL_cleanse(op->secret.data(),op->secret.size());return 0;}
int psa_import_key(const psa_key_attributes_t*,const uint8_t* bytes,size_t size,unsigned* id){*id=++next_key;key_store[*id].aes.assign(bytes,bytes+size);return 0;}
int psa_aead_encrypt(unsigned id,int,const uint8_t* nonce,size_t nonce_size,const uint8_t* aad,size_t aad_size,const uint8_t* input,size_t input_size,uint8_t* output,size_t capacity,size_t* size){
 if(capacity<input_size+16||nonce_size!=12)return -1;
 auto* context=EVP_CIPHER_CTX_new();int used=0,tail=0;
 bool good=context&&EVP_EncryptInit_ex(context,EVP_aes_256_gcm(),nullptr,key_store.at(id).aes.data(),nonce)>0&&EVP_EncryptUpdate(context,nullptr,&used,aad,aad_size)>0&&EVP_EncryptUpdate(context,output,&used,input,input_size)>0&&EVP_EncryptFinal_ex(context,output+used,&tail)>0&&EVP_CIPHER_CTX_ctrl(context,EVP_CTRL_GCM_GET_TAG,16,output+used+tail)>0;EVP_CIPHER_CTX_free(context);*size=used+tail+16;return good?0:-1;
}
int psa_aead_decrypt(unsigned id,int,const uint8_t* nonce,size_t nonce_size,const uint8_t* aad,size_t aad_size,const uint8_t* input,size_t input_size,uint8_t* output,size_t capacity,size_t* size){
 if(input_size<16||capacity<input_size-16||nonce_size!=12)return -1;
 auto* context=EVP_CIPHER_CTX_new();int used=0,tail=0;
 bool good=context&&EVP_DecryptInit_ex(context,EVP_aes_256_gcm(),nullptr,key_store.at(id).aes.data(),nonce)>0&&EVP_DecryptUpdate(context,nullptr,&used,aad,aad_size)>0&&EVP_DecryptUpdate(context,output,&used,input,input_size-16)>0&&EVP_CIPHER_CTX_ctrl(context,EVP_CTRL_GCM_SET_TAG,16,const_cast<uint8_t*>(input+input_size-16))>0&&EVP_DecryptFinal_ex(context,output+used,&tail)>0;EVP_CIPHER_CTX_free(context);*size=used+tail;return good?0:-1;
}
void mbedtls_platform_zeroize(void* p,size_t n){OPENSSL_cleanse(p,n);}
'''

SUITE = r'''
using namespace amity::consortium::platform;
int main(){
 assert(admission_crypto_self_test());
 fail_key_generation=true;
 assert(!admission_crypto_self_test());fail_key_generation=false;
 const std::string id="00000000000000000000000000000001";
 {
  for(int attack=0;attack<6;++attack){
   AdmissionServer server(id);AdmissionClient client;
   assert(client.handshake([&](auto in,auto& out){return server.handshake(in,out);}));
   std::string packet,plain;assert(client.encrypt("command",packet));
   if(attack==0)packet[3]^=1; // reflected direction
   if(attack==1)packet[11]+=1; // skipped/out-of-order sequence
   if(attack==2)packet[12]^=1; // ciphertext
   if(attack==3)packet.back()^=1; // tag
   if(attack==4)packet.erase(0,1); // malformed header
   if(attack==5){AdmissionServer fresh(id);AdmissionClient next;assert(next.handshake([&](auto in,auto& out){return fresh.handshake(in,out);}));assert(!fresh.decrypt(packet,plain));continue;}
   assert(!server.decrypt(packet,plain)&&plain.empty()&&!server.authenticated());
  }
  AdmissionServer edited(id);AdmissionClient denied;
  assert(!denied.handshake([&](auto in,auto& out){if(!edited.handshake(in,out))return false;if(out.size()==138&&out.substr(0,8)=="AMADMIT4")out.back()^=1;return true;}));
 }
 assert(key_store.empty());
 std::cout<<"PASS actual admission handshake/AEAD and self-test: transcript, direction, sequence, replay, cross-session, tamper, failure gates; PSA uses OpenSSL"<<std::endl;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT / 'components/consortium/admission_crypto.cpp')
    source = parser.parse_args().source.read_text()
    # Replace SDK adapter headers, not any production algorithm or state logic.
    source = '\n'.join(line for line in source.splitlines()
                       if not line.startswith('#include "'))
    flags = shlex.split(subprocess.check_output(['pkg-config', '--cflags', '--libs', 'openssl'], text=True))
    with tempfile.TemporaryDirectory(prefix='amity-admission-crypto-') as temp:
        cpp = Path(temp) / 'actual.cpp'
        cpp.write_text(PREFIX + source + SUITE)
        binary = Path(temp) / 'admission'
        subprocess.run(shlex.split(os.environ.get('CXX', 'c++')) + [
            '-std=c++17', '-Wall', '-Wextra', '-Werror', '-g', '-fsanitize=address,undefined',
            '-DESP_PLATFORM=1', '-I' + str(ROOT / 'components/consortium'), str(cpp), *flags, '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
