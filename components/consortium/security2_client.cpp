// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#ifdef ESP_PLATFORM
#include "security2_client.hpp"
#include "security2_vectors.hpp"
#include "mbedtls/bignum.h"
#include "session.pb-c.h"
#include "esp_random.h"
#include "esp_srp.h"
#include "protocomm.h"
#include "protocomm_security2.h"
#include <algorithm>
#include <cstring>
namespace amity::consortium::platform {
namespace {
constexpr char modulus[]="FFFFFFFFFFFFFFFFC90FDAA22168C234C4C6628B80DC1CD129024E088A67CC74020BBEA63B139B22514A08798E3404DDEF9519B3CD3A431B302B0A6DF25F14374FE1356D6D51C245E485B576625E7EC6F44C42E9A637ED6B0BFF5CB6F406B7EDEE386BFB5A899FA5AE9F24117C4B1FE649286651ECE45B3DC2007CB8A163BF0598DA48361C55D39A69163FA8FD24CF5F83655D23DCA3AD961C62F356208552BB9ED529077096966D670C354E4ABC9804F1746C08CA18217C32905E462E36CE3BE39E772C180E86039B2783A2EC07A28FB5C55DF06F4C52C9DE2BCBF6955817183995497CEA956AE515D2261898FA051015728E5A8AAAC42DAD33170D04507A33A85521ABDF1CBA64ECFB850458DBEF0A8AEA71575D060C7DB3970F85A6E1E4C7ABF5AE8CDB0933D71E8C94E04A25619DCEE3D2261AD2EE6BF12FFA06D98A0864D87602733EC86A64521F2B18177B200CBBE117577A615D6C770988C0BAD946E208E24FA074E5AB3143DB5BFCE0FD108E4B82D120A93AD2CAFFFFFFFFFFFFFFFF";
struct Mpi {mbedtls_mpi n;Mpi(){mbedtls_mpi_init(&n);}~Mpi(){mbedtls_mpi_free(&n);}Mpi(const Mpi&)=delete;Mpi&operator=(const Mpi&)=delete;};
std::string hash(std::initializer_list<std::string_view> values){psa_hash_operation_t op=PSA_HASH_OPERATION_INIT;if(psa_hash_setup(&op,PSA_ALG_SHA_512)!=PSA_SUCCESS)return {};for(auto v:values)if(psa_hash_update(&op,reinterpret_cast<const uint8_t*>(v.data()),v.size())!=PSA_SUCCESS){psa_hash_abort(&op);return {};}uint8_t bytes[64];size_t length=0;auto r=psa_hash_finish(&op,bytes,sizeof(bytes),&length);psa_hash_abort(&op);return r==PSA_SUCCESS&&length==64?std::string(reinterpret_cast<char*>(bytes),64):std::string();}
std::string binary(const Mpi &v,size_t width=0){if(!width)width=mbedtls_mpi_size(&v.n);std::string out(width,0);if(mbedtls_mpi_write_binary(&v.n,reinterpret_cast<uint8_t*>(out.data()),out.size()))return {};return out;}
bool from(Mpi &v,std::string_view bytes){return !bytes.empty()&&mbedtls_mpi_read_binary(&v.n,reinterpret_cast<const uint8_t*>(bytes.data()),bytes.size())==0;}
bool equal(std::string_view a,std::string_view b){if(a.size()!=b.size())return false;uint8_t diff=0;for(size_t i=0;i<a.size();++i)diff|=a[i]^b[i];return !diff;}
std::string pack(SessionData &session){const size_t size=session_data__get_packed_size(&session);if(!size||size>4096)return {};std::string out(size,0);session_data__pack(&session,reinterpret_cast<uint8_t*>(out.data()));return out;}
}
struct Security2Client::State {
    std::string username,password,A,key,proof,expected;
    Mpi N,g,a;
    psa_key_id_t aes=0;uint8_t nonce[12]{};unsigned phase=0;
    ~State(){if(aes)psa_destroy_key(aes);std::fill(password.begin(),password.end(),0);std::fill(key.begin(),key.end(),0);std::fill(proof.begin(),proof.end(),0);}
};
Security2Client::Security2Client(std::string user,std::string password):state_(std::make_unique<State>()){state_->username=std::move(user);state_->password=std::move(password);}
Security2Client::~Security2Client()=default;
bool Security2Client::begin(std::string &command){
    auto &s=*state_;if(s.phase||s.username.empty()||s.username.size()>32||s.password.size()!=64)return false;
    std::string secret(32,0);esp_fill_random(secret.data(),secret.size());secret[0]|=0x80;Mpi A;
    if(mbedtls_mpi_read_string(&s.N.n,16,modulus)||mbedtls_mpi_lset(&s.g.n,5)||!from(s.a,secret)||mbedtls_mpi_exp_mod(&A.n,&s.g.n,&s.a.n,&s.N.n,nullptr))return false;
    std::fill(secret.begin(),secret.end(),0);s.A=binary(A,384);if(s.A.size()!=384)return false;
    S2SessionCmd0 cmd=S2_SESSION_CMD0__INIT;cmd.client_username={s.username.size(),reinterpret_cast<uint8_t*>(s.username.data())};cmd.client_pubkey={s.A.size(),reinterpret_cast<uint8_t*>(s.A.data())};Sec2Payload payload=SEC2_PAYLOAD__INIT;payload.msg=SEC2_MSG_TYPE__S2Session_Command0;payload.payload_case=SEC2_PAYLOAD__PAYLOAD_SC0;payload.sc0=&cmd;SessionData session=SESSION_DATA__INIT;session.sec_ver=SEC_SCHEME_VERSION__SecScheme2;session.proto_case=SESSION_DATA__PROTO_SEC2;session.sec2=&payload;command=pack(session);s.phase=1;return !command.empty();
}
bool Security2Client::challenge(std::string_view response,std::string &command){
    auto &s=*state_;if(s.phase!=1||response.empty()||response.size()>4096)return false;SessionData *session=session_data__unpack(nullptr,response.size(),reinterpret_cast<const uint8_t*>(response.data()));if(!session)return false;
    bool shape=session->sec_ver==SEC_SCHEME_VERSION__SecScheme2&&session->proto_case==SESSION_DATA__PROTO_SEC2&&session->sec2&&session->sec2->msg==SEC2_MSG_TYPE__S2Session_Response0&&session->sec2->payload_case==SEC2_PAYLOAD__PAYLOAD_SR0&&session->sec2->sr0&&session->sec2->sr0->status==STATUS__Success;
    std::string Bbytes,salt;if(shape){auto &r=*session->sec2->sr0;shape=r.device_pubkey.len>0&&r.device_pubkey.len<=384&&r.device_salt.len==32;if(shape){Bbytes.assign(reinterpret_cast<char*>(r.device_pubkey.data),r.device_pubkey.len);salt.assign(reinterpret_cast<char*>(r.device_salt.data),r.device_salt.len);}}session_data__free_unpacked(session,nullptr);if(!shape)return false;
    Mpi B,u,x,k,v,kv,base,ux,exp,S,A;
    if(!from(B,Bbytes)||mbedtls_mpi_cmp_int(&B.n,0)<=0||mbedtls_mpi_cmp_mpi(&B.n,&s.N.n)>=0||!from(A,s.A))return false;
    const auto paddedN=binary(s.N,384),paddedG=binary(s.g,384),paddedB=binary(B,384);
    auto uh=hash({s.A,paddedB}),kh=hash({paddedN,paddedG}),inner=hash({s.username,":",s.password});if(uh.size()!=64||kh.size()!=64||inner.size()!=64)return false;
    auto xh=hash({salt,inner});std::fill(inner.begin(),inner.end(),0);std::fill(s.password.begin(),s.password.end(),0);s.password.clear();
    if(!from(u,uh)||mbedtls_mpi_cmp_int(&u.n,0)==0||!from(x,xh)||!from(k,kh)||mbedtls_mpi_exp_mod(&v.n,&s.g.n,&x.n,&s.N.n,nullptr)||mbedtls_mpi_mul_mpi(&kv.n,&k.n,&v.n)||mbedtls_mpi_sub_mpi(&base.n,&B.n,&kv.n)||mbedtls_mpi_mod_mpi(&base.n,&base.n,&s.N.n)||mbedtls_mpi_cmp_int(&base.n,0)==0||mbedtls_mpi_mul_mpi(&ux.n,&u.n,&x.n)||mbedtls_mpi_add_mpi(&exp.n,&s.a.n,&ux.n)||mbedtls_mpi_exp_mod(&S.n,&base.n,&exp.n,&s.N.n,nullptr))return false;
    s.key=hash({binary(S)});auto hn=hash({paddedN}),hg=hash({paddedG}),hi=hash({s.username});if(s.key.size()!=64||hn.size()!=64||hg.size()!=64||hi.size()!=64)return false;for(size_t i=0;i<64;++i)hn[i]^=hg[i];
    s.proof=hash({hn,hi,salt,s.A,Bbytes,s.key});s.expected=hash({s.A,s.proof,s.key});if(s.proof.size()!=64||s.expected.size()!=64)return false;
    S2SessionCmd1 cmd=S2_SESSION_CMD1__INIT;cmd.client_proof={s.proof.size(),reinterpret_cast<uint8_t*>(s.proof.data())};Sec2Payload payload=SEC2_PAYLOAD__INIT;payload.msg=SEC2_MSG_TYPE__S2Session_Command1;payload.payload_case=SEC2_PAYLOAD__PAYLOAD_SC1;payload.sc1=&cmd;SessionData packet=SESSION_DATA__INIT;packet.sec_ver=SEC_SCHEME_VERSION__SecScheme2;packet.proto_case=SESSION_DATA__PROTO_SEC2;packet.sec2=&payload;command=pack(packet);s.phase=2;return !command.empty();
}
bool Security2Client::verify(std::string_view response){
    auto &s=*state_;if(s.phase!=2||response.empty()||response.size()>4096)return false;SessionData *session=session_data__unpack(nullptr,response.size(),reinterpret_cast<const uint8_t*>(response.data()));if(!session)return false;bool good=session->sec_ver==SEC_SCHEME_VERSION__SecScheme2&&session->proto_case==SESSION_DATA__PROTO_SEC2&&session->sec2&&session->sec2->msg==SEC2_MSG_TYPE__S2Session_Response1&&session->sec2->payload_case==SEC2_PAYLOAD__PAYLOAD_SR1&&session->sec2->sr1&&session->sec2->sr1->status==STATUS__Success;
    if(good){auto &r=*session->sec2->sr1;good=r.device_proof.len==64&&r.device_nonce.len==12&&equal(s.expected,std::string_view(reinterpret_cast<char*>(r.device_proof.data),r.device_proof.len));if(good)std::memcpy(s.nonce,r.device_nonce.data,12);}session_data__free_unpacked(session,nullptr);if(!good)return false;
    if(s.nonce[8]||s.nonce[9]||s.nonce[10]||s.nonce[11]!=1)return false;
    psa_key_attributes_t attr=PSA_KEY_ATTRIBUTES_INIT;psa_set_key_type(&attr,PSA_KEY_TYPE_AES);psa_set_key_bits(&attr,256);psa_set_key_algorithm(&attr,PSA_ALG_GCM);psa_set_key_usage_flags(&attr,PSA_KEY_USAGE_ENCRYPT|PSA_KEY_USAGE_DECRYPT);if(psa_import_key(&attr,reinterpret_cast<const uint8_t*>(s.key.data()),32,&s.aes)!=PSA_SUCCESS)return false;std::fill(s.key.begin(),s.key.end(),0);s.key.clear();s.phase=3;return true;
}
bool Security2Client::authenticated()const{return state_->phase==3;}
namespace {
bool crypt(bool encrypt,psa_key_id_t key,uint8_t (&nonce)[12],std::string_view input,std::string &out){uint32_t counter=(uint32_t(nonce[8])<<24)|(uint32_t(nonce[9])<<16)|(uint32_t(nonce[10])<<8)|nonce[11];if(!counter||counter==UINT32_MAX||input.size()>4080||(!encrypt&&input.size()<16))return false;out.resize(encrypt?input.size()+16:input.size()-16);size_t written=0;auto status=encrypt?psa_aead_encrypt(key,PSA_ALG_GCM,nonce,12,nullptr,0,reinterpret_cast<const uint8_t*>(input.data()),input.size(),reinterpret_cast<uint8_t*>(out.data()),out.size(),&written):psa_aead_decrypt(key,PSA_ALG_GCM,nonce,12,nullptr,0,reinterpret_cast<const uint8_t*>(input.data()),input.size(),reinterpret_cast<uint8_t*>(out.data()),out.size(),&written);if(status!=PSA_SUCCESS||written!=out.size()){out.clear();return false;}++counter;for(int i=11;i>=8;--i){nonce[i]=counter&255;counter>>=8;}return true;}
}
bool Security2Client::encrypt(std::string_view input,std::string &out){return authenticated()&&crypt(true,state_->aes,state_->nonce,input,out);}
bool Security2Client::decrypt(std::string_view input,std::string &out){return authenticated()&&crypt(false,state_->aes,state_->nonce,input,out);}
bool security2_self_test(){
    // Exercise the actual pinned Security2 server, not another copy of client
    // equations. This test has no transport listener or operational authority.
    const std::string username="00000000000000000000000000000001",password(64,'a');
    std::string salt,verifier;
    if(!unhex(selftest_salt_hex,salt,32)||!unhex(selftest_verifier_hex,verifier,384))return false;
    protocomm_t *pc=protocomm_new();if(!pc)return false;protocomm_security2_params_t params{salt.data(),32,verifier.data(),384};bool good=protocomm_set_security(pc,"session",&protocomm_security2,&params)==ESP_OK&&protocomm_open_session(pc,1)==ESP_OK;
    Security2Client client(username,password);std::string request,response;auto exchange=[&](){uint8_t *output=nullptr;ssize_t size=0;auto result=protocomm_req_handle(pc,"session",1,reinterpret_cast<const uint8_t*>(request.data()),request.size(),&output,&size);if(result!=ESP_OK||!output||size<=0||size>4096){free(output);return false;}response.assign(reinterpret_cast<char*>(output),size);free(output);return true;};
    good=good&&client.begin(request)&&exchange()&&client.challenge(response,request)&&exchange();
    if(good){auto altered=response;SessionData *proof=session_data__unpack(nullptr,altered.size(),reinterpret_cast<const uint8_t*>(altered.data()));if(!proof||!proof->sec2||!proof->sec2->sr1||proof->sec2->sr1->device_proof.len!=64)good=false;else{proof->sec2->sr1->device_proof.data[0]^=1;altered=pack(*proof);good=!client.verify(altered)&&client.verify(response);}if(proof)session_data__free_unpacked(proof,nullptr);}
    auto echo=[](uint32_t,const uint8_t *in,ssize_t size,uint8_t **out,ssize_t *outsize,void*)->esp_err_t{if(size<=0||size>512)return ESP_ERR_INVALID_SIZE;*out=static_cast<uint8_t*>(malloc(size));if(!*out)return ESP_ERR_NO_MEM;memcpy(*out,in,size);*outsize=size;return ESP_OK;};
    good=good&&protocomm_add_endpoint(pc,"echo",echo,nullptr)==ESP_OK;
    for(unsigned i=0;i<3&&good;++i){const std::string plain="Security2 GCM counter "+std::to_string(i);good=client.encrypt(plain,request);uint8_t *out=nullptr;ssize_t size=0;good=good&&protocomm_req_handle(pc,"echo",1,reinterpret_cast<const uint8_t*>(request.data()),request.size(),&out,&size)==ESP_OK&&out&&size>0;if(good){response.assign(reinterpret_cast<char*>(out),size);std::string decrypted;good=client.decrypt(response,decrypted)&&decrypted==plain;}free(out);}
    // An altered ciphertext must fail authentication; never pass plaintext to
    // the endpoint or guess a replacement shared counter after this failure.
    if(good){good=client.encrypt("tamper must be rejected",request);if(good){request.back()^=1;uint8_t *out=nullptr;ssize_t size=0;good=protocomm_req_handle(pc,"echo",1,reinterpret_cast<const uint8_t*>(request.data()),request.size(),&out,&size)!=ESP_OK;free(out);}}
    // Invalid public values are rejected before deriving a key.
    Security2Client invalid_public(username,password);std::string ignored;
    good=good&&invalid_public.begin(ignored);
    S2SessionResp0 bad=S2_SESSION_RESP0__INIT;std::string zero(384,0);bad.device_pubkey={zero.size(),reinterpret_cast<uint8_t*>(zero.data())};bad.device_salt={salt.size(),reinterpret_cast<uint8_t*>(salt.data())};Sec2Payload payload=SEC2_PAYLOAD__INIT;payload.msg=SEC2_MSG_TYPE__S2Session_Response0;payload.payload_case=SEC2_PAYLOAD__PAYLOAD_SR0;payload.sr0=&bad;SessionData packet=SESSION_DATA__INIT;packet.sec_ver=SEC_SCHEME_VERSION__SecScheme2;packet.proto_case=SESSION_DATA__PROTO_SEC2;packet.sec2=&payload;good=good&&!invalid_public.challenge(pack(packet),ignored);
    Security2Client invalid_modulus(username,password);good=good&&invalid_modulus.begin(ignored);
    Mpi N;std::string modulus_bytes;if(mbedtls_mpi_read_string(&N.n,16,modulus)==0)modulus_bytes=binary(N,384);
    bad.device_pubkey={modulus_bytes.size(),reinterpret_cast<uint8_t*>(modulus_bytes.data())};good=good&&!modulus_bytes.empty()&&!invalid_modulus.challenge(pack(packet),ignored);
    protocomm_close_session(pc,1);protocomm_delete(pc);
    pc=protocomm_new();if(!pc)return false;
    good=good&&protocomm_set_security(pc,"session",&protocomm_security2,&params)==ESP_OK&&protocomm_open_session(pc,1)==ESP_OK;
    Security2Client wrong_password(username,std::string(64,'b'));
    good=good&&wrong_password.begin(request)&&exchange()&&wrong_password.challenge(response,request);
    if(good){const bool returned=exchange();good=!returned||!wrong_password.verify(response);}
    protocomm_close_session(pc,1);protocomm_delete(pc);return good;
}
} // namespace
#endif
