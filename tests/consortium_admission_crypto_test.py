#!/usr/bin/env python3
"""Run actual admission code with real OpenSSL-backed PSA primitives.

The existing ESP Security2/protocomm and CHIP parsers are explicit host seams;
this checks the NEW strict transcript-binding state machine, not SRP/CHIP itself.
Firmware admission_crypto_self_test uses the actual pinned SDK implementations.
"""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PREFIX = r'''
#include "admission_crypto.hpp"
#include "factory_credentials.hpp"
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
int psa_crypto_init(){return 0;}
void psa_set_key_type(psa_key_attributes_t* a,int type){a->type=type;}
void psa_set_key_bits(psa_key_attributes_t*,int){}
void psa_set_key_usage_flags(psa_key_attributes_t*,int){}
void psa_set_key_algorithm(psa_key_attributes_t*,int){}
int psa_destroy_key(unsigned id){auto found=key_store.find(id);if(found!=key_store.end()){EVP_PKEY_free(found->second.ec);if(!found->second.aes.empty())OPENSSL_cleanse(found->second.aes.data(),found->second.aes.size());key_store.erase(found);}return 0;}
int psa_generate_key(const psa_key_attributes_t*,unsigned* id){EVP_PKEY_CTX* context=EVP_PKEY_CTX_new_from_name(nullptr,"EC",nullptr);EVP_PKEY* key=nullptr;bool good=context&&EVP_PKEY_keygen_init(context)>0&&EVP_PKEY_CTX_set_group_name(context,"prime256v1")>0&&EVP_PKEY_generate(context,&key)>0;EVP_PKEY_CTX_free(context);if(!good)return -1;*id=++next_key;key_store[*id].ec=key;return 0;}
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
 if(capacity<input_size+16||nonce_size!=12)return -1;auto* context=EVP_CIPHER_CTX_new();int used=0,tail=0;
 bool good=context&&EVP_EncryptInit_ex(context,EVP_aes_256_gcm(),nullptr,key_store.at(id).aes.data(),nonce)>0&&EVP_EncryptUpdate(context,nullptr,&used,aad,aad_size)>0&&EVP_EncryptUpdate(context,output,&used,input,input_size)>0&&EVP_EncryptFinal_ex(context,output+used,&tail)>0&&EVP_CIPHER_CTX_ctrl(context,EVP_CTRL_GCM_GET_TAG,16,output+used+tail)>0;EVP_CIPHER_CTX_free(context);*size=used+tail+16;return good?0:-1;
}
int psa_aead_decrypt(unsigned id,int,const uint8_t* nonce,size_t nonce_size,const uint8_t* aad,size_t aad_size,const uint8_t* input,size_t input_size,uint8_t* output,size_t capacity,size_t* size){
 if(input_size<16||capacity<input_size-16||nonce_size!=12)return -1;auto* context=EVP_CIPHER_CTX_new();int used=0,tail=0;
 bool good=context&&EVP_DecryptInit_ex(context,EVP_aes_256_gcm(),nullptr,key_store.at(id).aes.data(),nonce)>0&&EVP_DecryptUpdate(context,nullptr,&used,aad,aad_size)>0&&EVP_DecryptUpdate(context,output,&used,input,input_size-16)>0&&EVP_CIPHER_CTX_ctrl(context,EVP_CTRL_GCM_SET_TAG,16,const_cast<uint8_t*>(input+input_size-16))>0&&EVP_DecryptFinal_ex(context,output+used,&tail)>0;EVP_CIPHER_CTX_free(context);*size=used+tail;return good?0:-1;
}
void mbedtls_platform_zeroize(void* p,size_t n){OPENSSL_cleanse(p,n);}
constexpr int ESP_OK=0,ESP_ERR_INVALID_ARG=-1,ESP_ERR_NO_MEM=-2,CHIP_NO_ERROR=0;
using esp_err_t=int;
namespace chip{
struct SetupPayload{uint32_t setUpPINCode=0;};
struct ManualSetupPayloadParser{std::string input;explicit ManualSetupPayloadParser(std::string s):input(std::move(s)){}int populatePayload(SetupPayload& out){if(input!="3497-011-2332"&&input!="34970112332")return -1;out.setUpPINCode=20202021;return 0;}};
struct QRCodeSetupPayloadParser{std::string input;explicit QRCodeSetupPayloadParser(std::string s):input(std::move(s)){}int populatePayload(SetupPayload& out){if(input!="MT:Y.K9042C00KA0648G00")return -1;out.setUpPINCode=20202021;return 0;}};
}
// Explicit PAKE host seam: authenticates a mock exchange under the supplied
// PIN-derived secret. The production source uses actual ESP SRP/protocomm.
std::string mock_hash(std::string_view in){std::string out(32,0);size_t size=0;psa_hash_compute(1,reinterpret_cast<const uint8_t*>(in.data()),in.size(),reinterpret_cast<uint8_t*>(out.data()),32,&size);return out;}
std::string mock_mac(std::string_view key,std::string_view text){std::string out(32,0);size_t size=0;auto* mac=EVP_Q_mac(nullptr,"HMAC",nullptr,"SHA256",nullptr,key.data(),key.size(),reinterpret_cast<const unsigned char*>(text.data()),text.size(),reinterpret_cast<unsigned char*>(out.data()),out.size(),&size);assert(mac&&size==32);return out;}
bool mock_equal(std::string_view a,std::string_view b){return a.size()==b.size()&&CRYPTO_memcmp(a.data(),b.data(),a.size())==0;}
std::string mock_protect(std::string_view key,std::string_view text){return std::string(text)+mock_mac(key,text);}
bool mock_unprotect(std::string_view key,std::string_view input,std::string& out){if(input.size()<32||!mock_equal(input.substr(input.size()-32),mock_mac(key,input.substr(0,input.size()-32))))return false;out=input.substr(0,input.size()-32);return true;}
using Endpoint=esp_err_t(*)(uint32_t,const uint8_t*,ssize_t,uint8_t**,ssize_t*,void*);
struct protocomm_security2_params_t{const char* salt;uint16_t salt_len;const char* verifier;uint16_t verifier_len;};int protocomm_security2=0;
struct protocomm_t{std::string key,transcript,challenge;Endpoint endpoint=nullptr;void* context=nullptr;bool authenticated=false;};
protocomm_t* protocomm_new(){return new protocomm_t;}
void protocomm_delete(protocomm_t* pc){delete pc;}
int protocomm_close_session(protocomm_t*,int){return 0;}int protocomm_open_session(protocomm_t*,int){return 0;}
int protocomm_set_security(protocomm_t* pc,const char*,const void*,const protocomm_security2_params_t* p){pc->key.assign(p->verifier,32);return 0;}
int protocomm_add_endpoint(protocomm_t* pc,const char*,Endpoint endpoint,void* context){pc->endpoint=endpoint;pc->context=context;return 0;}
int esp_srp_gen_salt_verifier(const char*,int,const char* password,int count,char** salt,int salt_size,char** verifier,int* verifier_size){*salt=static_cast<char*>(malloc(salt_size));RAND_bytes(reinterpret_cast<unsigned char*>(*salt),salt_size);*verifier_size=384;*verifier=static_cast<char*>(calloc(1,384));auto key=mock_hash(std::string_view(password,count));std::memcpy(*verifier,key.data(),32);return 0;}
int protocomm_req_handle(protocomm_t* pc,const char* endpoint,int,const uint8_t* bytes,size_t count,uint8_t** out,ssize_t* size){
 std::string_view input(reinterpret_cast<const char*>(bytes),count);std::string response;
 if(std::string(endpoint)=="session"){
  if(input.substr(0,2)=="C0"){pc->transcript=input;pc->challenge.resize(16);RAND_bytes(reinterpret_cast<unsigned char*>(pc->challenge.data()),16);response="R0"+pc->challenge;}
  else if(input.substr(0,2)=="C1"&&mock_equal(input.substr(2),mock_mac(pc->key,pc->transcript+pc->challenge))){pc->authenticated=true;response="R1"+mock_mac(pc->key,pc->transcript+pc->challenge+"server");}
  else return -1;
 }else{
  std::string plain;if(!pc->authenticated||!mock_unprotect(pc->key,input,plain))return -1;uint8_t* answer=nullptr;ssize_t length=0;
  auto result=pc->endpoint(1,reinterpret_cast<const uint8_t*>(plain.data()),plain.size(),&answer,&length,pc->context);if(result!=0){free(answer);return result;}response=mock_protect(pc->key,std::string_view(reinterpret_cast<char*>(answer),length));free(answer);
 }
 *size=response.size();*out=static_cast<uint8_t*>(malloc(*size));std::memcpy(*out,response.data(),*size);return 0;
}
constexpr int SEC_SCHEME_VERSION__SecScheme2=2,SEC2_MSG_TYPE__S2Session_Response0=0,SEC2_MSG_TYPE__S2Session_Response1=1,STATUS__Success=0;
struct Status{int status=0;};struct Sec2{int msg=0;Status* sr0=nullptr;Status* sr1=nullptr;};struct SessionData{int sec_ver=2;Sec2* sec2=nullptr;};
SessionData* session_data__unpack(void*,size_t count,const uint8_t* bytes){if(count<2||bytes[0]!='R')return nullptr;auto* value=new SessionData;value->sec2=new Sec2;value->sec2->msg=bytes[1]=='0'?0:1;if(bytes[1]=='0')value->sec2->sr0=new Status;else value->sec2->sr1=new Status;return value;}
void session_data__free_unpacked(SessionData* value,void*){delete value->sec2->sr0;delete value->sec2->sr1;delete value->sec2;delete value;}
namespace amity::consortium::platform{
class Security2Client{
 std::string user,key,transcript,challenge_bytes;bool ready=false;
public:
 Security2Client(std::string name,std::string password):user(std::move(name)),key(mock_hash(password)){}
 bool begin(std::string& out){std::string nonce(16,0);RAND_bytes(reinterpret_cast<unsigned char*>(nonce.data()),16);transcript="C0"+user+nonce;out=transcript;return true;}
 bool challenge(std::string_view input,std::string& out){if(input.size()!=18||input.substr(0,2)!="R0")return false;challenge_bytes=input.substr(2);out="C1"+mock_mac(key,transcript+challenge_bytes);return true;}
 bool verify(std::string_view input){ready=input.substr(0,2)=="R1"&&mock_equal(input.substr(2),mock_mac(key,transcript+challenge_bytes+"server"));return ready;}
 bool encrypt(std::string_view input,std::string& out){if(!ready)return false;out=mock_protect(key,input);return true;}
 bool decrypt(std::string_view input,std::string& out){return ready&&mock_unprotect(key,input,out);}
};
}
'''

SUITE = r'''
using namespace amity::consortium::platform;
int main(){
 assert(admission_crypto_self_test());
 const std::string id="00000000000000000000000000000001";
 for(auto mode:{AdmissionMode::button,AdmissionMode::setup_code}){
  for(int attack=0;attack<6;++attack){
   AdmissionServer server(mode,id,20202021);AdmissionClient client(mode,20202021);
   assert(client.handshake([&](auto in,auto& out){return server.handshake(in,out);}));
   std::string packet,plain;assert(client.encrypt("command",packet));
   if(attack==0)packet[3]^=1; // reflected direction
   if(attack==1)packet[11]+=1; // skipped/out-of-order sequence
   if(attack==2)packet[12]^=1; // ciphertext
   if(attack==3)packet.back()^=1; // tag
   if(attack==4)packet.erase(0,1); // malformed header
   if(attack==5){AdmissionServer fresh(mode,id,20202021);AdmissionClient next(mode,20202021);assert(next.handshake([&](auto in,auto& out){return fresh.handshake(in,out);}));assert(!fresh.decrypt(packet,plain));continue;}
   assert(!server.decrypt(packet,plain)&&plain.empty()&&!server.authenticated());
  }
  AdmissionServer edited(mode,id,20202021);AdmissionClient denied(mode,20202021);
  assert(!denied.handshake([&](auto in,auto& out){if(!edited.handshake(in,out))return false;if(out.size()==139&&out.substr(0,8)=="AMADMIT3")out.back()^=1;return true;}));
 }
 assert(key_store.empty());
 std::cout<<"PASS actual admission handshake/AEAD both modes: transcript, direction, sequence, replay, cross-session, tamper; PIN-PAKE seam mocked, PSA uses OpenSSL"<<std::endl;
}
'''


def main():
    source = (ROOT / 'components/consortium/admission_crypto.cpp').read_text()
    # Replace SDK adapter headers, not any production algorithm or state logic.
    source = '\n'.join(line for line in source.splitlines()
                       if not line.startswith('#include "'))
    flags = shlex.split(subprocess.check_output(['pkg-config', '--cflags', '--libs', 'openssl'], text=True))
    with tempfile.TemporaryDirectory(prefix='amity-admission-crypto-') as temp:
        cpp = Path(temp) / 'actual.cpp'
        cpp.write_text(PREFIX + source + SUITE)
        for strict in (0, 1):
            binary = Path(temp) / ('strict' if strict else 'button')
            subprocess.run(shlex.split(os.environ.get('CXX', 'c++')) + [
                '-std=c++17', '-Wall', '-Wextra', '-Werror', '-g', '-fsanitize=address,undefined',
                '-DESP_PLATFORM=1', f'-DCONFIG_AMITY_ADMISSION_REQUIRE_SETUP_CODE={strict}',
                '-I' + str(ROOT / 'components/consortium'),
                '-I' + str(ROOT / 'components/device_settings/include'), str(cpp), *flags, '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
