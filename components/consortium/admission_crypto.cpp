// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#ifdef ESP_PLATFORM
#include "admission_crypto.hpp"
#include "security2_client.hpp"
#include "factory_credentials.hpp"
#include "sdkconfig.h"
#include "psa/crypto.h"
#include "mbedtls/platform_util.h"
#include "protocomm.h"
#include "protocomm_security2.h"
#include "esp_srp.h"
#include "session.pb-c.h"
#include "setup_payload/ManualSetupPayloadParser.h"
#include "setup_payload/QRCodeSetupPayloadParser.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace amity::consortium::platform {
namespace {
constexpr std::string_view magic="AMADMIT3";
constexpr size_t public_size=65,nonce_size=32,max_plain=4000;
enum Message:uint8_t {hello=1,welcome=2,finish=3,confirmed=4,srp=5,srp_response=6,bind=7,bound=8};
void wipe(std::string& value){if(!value.empty())mbedtls_platform_zeroize(value.data(),value.size());value.clear();}
bool equal(std::string_view a,std::string_view b){if(a.size()!=b.size())return false;uint8_t difference=0;for(size_t i=0;i<a.size();++i)difference|=uint8_t(a[i])^uint8_t(b[i]);return difference==0;}
std::string hash(std::string_view input){std::string output(32,0);size_t size=0;if(psa_hash_compute(PSA_ALG_SHA_256,reinterpret_cast<const uint8_t*>(input.data()),input.size(),reinterpret_cast<uint8_t*>(output.data()),output.size(),&size)!=PSA_SUCCESS||size!=32)return {};return output;}
std::string hex(std::string_view input){const char* digits="0123456789abcdef";std::string result;result.reserve(input.size()*2);for(uint8_t value:input){result+=digits[value>>4];result+=digits[value&15];}return result;}
bool valid_id(std::string_view id){return id.size()==32&&id!=std::string(32,'0')&&std::all_of(id.begin(),id.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');});}
std::string packet(Message kind,AdmissionMode mode,std::string_view payload){std::string value(magic);value+=char(kind);value+=char(mode);value.append(payload);return value;}
bool unpack(std::string_view value,Message kind,AdmissionMode mode,std::string_view& payload){if(value.size()<10||value.substr(0,8)!=magic||uint8_t(value[8])!=kind||uint8_t(value[9])!=uint8_t(mode))return false;payload=value.substr(10);return true;}
std::string pin_password(std::string_view id,uint32_t pin){if(!amity::valid_setup_pin(pin)||!valid_id(id))return {};char digits[9];snprintf(digits,sizeof(digits),"%08u",unsigned(pin));std::string material="Amity admission Matter PIN v1:";material.append(id);material+=':';material+=digits;auto result=hex(hash(material));wipe(material);mbedtls_platform_zeroize(digits,sizeof(digits));return result;}
struct Channel {
    psa_key_id_t ephemeral=0,tx=0,rx=0;uint64_t tx_sequence=0,rx_sequence=0;
    uint8_t tx_direction=0,rx_direction=1;bool failed=false;
    std::string public_key,nonce,transcript_hash;
    ~Channel(){if(ephemeral)psa_destroy_key(ephemeral);if(tx)psa_destroy_key(tx);if(rx)psa_destroy_key(rx);}
    bool prepare(){
        if(psa_crypto_init()!=PSA_SUCCESS)return false;
        psa_key_attributes_t attributes=PSA_KEY_ATTRIBUTES_INIT;
        psa_set_key_type(&attributes,PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));psa_set_key_bits(&attributes,256);
        psa_set_key_usage_flags(&attributes,PSA_KEY_USAGE_DERIVE);psa_set_key_algorithm(&attributes,PSA_ALG_ECDH);
        if(psa_generate_key(&attributes,&ephemeral)!=PSA_SUCCESS)return false;
        public_key.resize(public_size);size_t size=0;
        if(psa_export_public_key(ephemeral,reinterpret_cast<uint8_t*>(public_key.data()),public_key.size(),&size)!=PSA_SUCCESS||size!=public_size)return false;
        nonce.resize(nonce_size);return psa_generate_random(reinterpret_cast<uint8_t*>(nonce.data()),nonce.size())==PSA_SUCCESS;
    }
    bool derive(std::string_view peer,std::string_view transcript,bool server){
        if(peer.size()!=public_size||uint8_t(peer.front())!=4)return false;
        transcript_hash=hash(transcript);if(transcript_hash.size()!=32)return false;
        std::array<uint8_t,32> shared{};size_t size=0;
        auto result=psa_raw_key_agreement(PSA_ALG_ECDH,ephemeral,reinterpret_cast<const uint8_t*>(peer.data()),peer.size(),shared.data(),shared.size(),&size);
        psa_destroy_key(ephemeral);ephemeral=0;
        if(result!=PSA_SUCCESS||size!=shared.size()){mbedtls_platform_zeroize(shared.data(),shared.size());return false;}
        psa_key_derivation_operation_t operation=PSA_KEY_DERIVATION_OPERATION_INIT;
        constexpr std::string_view info="Amity admission directional AES-256-GCM v1";
        std::array<uint8_t,64> keys{};
        bool good=psa_key_derivation_setup(&operation,PSA_ALG_HKDF(PSA_ALG_SHA_256))==PSA_SUCCESS&&
            psa_key_derivation_input_bytes(&operation,PSA_KEY_DERIVATION_INPUT_SALT,reinterpret_cast<const uint8_t*>(transcript_hash.data()),transcript_hash.size())==PSA_SUCCESS&&
            psa_key_derivation_input_bytes(&operation,PSA_KEY_DERIVATION_INPUT_SECRET,shared.data(),shared.size())==PSA_SUCCESS&&
            psa_key_derivation_input_bytes(&operation,PSA_KEY_DERIVATION_INPUT_INFO,reinterpret_cast<const uint8_t*>(info.data()),info.size())==PSA_SUCCESS&&
            psa_key_derivation_output_bytes(&operation,keys.data(),keys.size())==PSA_SUCCESS;
        psa_key_derivation_abort(&operation);mbedtls_platform_zeroize(shared.data(),shared.size());
        psa_key_attributes_t attributes=PSA_KEY_ATTRIBUTES_INIT;psa_set_key_type(&attributes,PSA_KEY_TYPE_AES);psa_set_key_bits(&attributes,256);
        psa_set_key_algorithm(&attributes,PSA_ALG_GCM);psa_set_key_usage_flags(&attributes,PSA_KEY_USAGE_ENCRYPT|PSA_KEY_USAGE_DECRYPT);
        if(good)good=psa_import_key(&attributes,keys.data()+(server?32:0),32,&tx)==PSA_SUCCESS&&psa_import_key(&attributes,keys.data()+(server?0:32),32,&rx)==PSA_SUCCESS;
        mbedtls_platform_zeroize(keys.data(),keys.size());tx_direction=server?1:0;rx_direction=server?0:1;return good;
    }
    bool seal(std::string_view plain,std::string& output){
        output.clear();if(failed||!tx||plain.empty()||plain.size()>max_plain||tx_sequence==UINT64_MAX)return false;
        const uint64_t next=tx_sequence+1;std::string header="AE3";header+=char(tx_direction);for(int i=7;i>=0;--i)header+=char(next>>(8*i));
        const auto aad=transcript_hash+header;output=header;output.resize(12+plain.size()+16);size_t size=0;
        if(psa_aead_encrypt(tx,PSA_ALG_GCM,reinterpret_cast<const uint8_t*>(header.data()),header.size(),reinterpret_cast<const uint8_t*>(aad.data()),aad.size(),reinterpret_cast<const uint8_t*>(plain.data()),plain.size(),reinterpret_cast<uint8_t*>(output.data()+12),output.size()-12,&size)!=PSA_SUCCESS||size!=plain.size()+16){output.clear();failed=true;return false;}
        tx_sequence=next;return true;
    }
    bool open(std::string_view input,std::string& plain){
        plain.clear();if(failed||!rx)return false;
        uint64_t sequence=0;if(input.size()>=12)for(size_t i=4;i<12;++i)sequence=(sequence<<8)|uint8_t(input[i]);
        if(input.size()<29||input.size()>max_plain+28||input.substr(0,3)!="AE3"||uint8_t(input[3])!=rx_direction||rx_sequence==UINT64_MAX||sequence!=rx_sequence+1){failed=true;return false;}
        auto header=input.substr(0,12);auto aad=transcript_hash+std::string(header);plain.resize(input.size()-28);size_t size=0;
        if(psa_aead_decrypt(rx,PSA_ALG_GCM,reinterpret_cast<const uint8_t*>(header.data()),header.size(),reinterpret_cast<const uint8_t*>(aad.data()),aad.size(),reinterpret_cast<const uint8_t*>(input.data()+12),input.size()-12,reinterpret_cast<uint8_t*>(plain.data()),plain.size(),&size)!=PSA_SUCCESS||size!=plain.size()){wipe(plain);failed=true;return false;}
        rx_sequence=sequence;return true;
    }
};
std::string binding(std::string_view role,const Channel& channel){return "Amity admission "+std::string(role)+" binding v1:"+channel.transcript_hash;}
}
AdmissionMode configured_admission_mode(){
#if CONFIG_AMITY_ADMISSION_REQUIRE_SETUP_CODE
    return AdmissionMode::setup_code;
#else
    return AdmissionMode::button;
#endif
}
bool parse_admission_setup_code(std::string_view text,uint32_t& pin){
    pin=0;if(text.empty()||text.size()>512)return false;
    std::string clean;for(unsigned char c:text)if(!std::isspace(c))clean+=char(c);
    chip::SetupPayload payload;bool qr=clean.rfind("MT:",0)==0;
    const auto result=qr?chip::QRCodeSetupPayloadParser(clean).populatePayload(payload):chip::ManualSetupPayloadParser(clean).populatePayload(payload);
    wipe(clean);if(result!=CHIP_NO_ERROR||!amity::valid_setup_pin(payload.setUpPINCode))return false;
    pin=payload.setUpPINCode;return true;
}
struct AdmissionClient::State {AdmissionMode mode;uint32_t pin;Channel channel;std::string peer;bool started=false,ready=false;State(AdmissionMode m,uint32_t p):mode(m),pin(p){}~State(){mbedtls_platform_zeroize(&pin,sizeof(pin));}};
AdmissionClient::AdmissionClient(AdmissionMode mode,uint32_t pin):state_(new State(mode,pin)){}
AdmissionClient::~AdmissionClient()=default;
const std::string& AdmissionClient::peer_id()const{return state_->peer;}
bool AdmissionClient::handshake(const std::function<bool(std::string_view,std::string&)>& exchange){
    auto& s=*state_;if(s.started||!exchange||(s.mode==AdmissionMode::setup_code&&!amity::valid_setup_pin(s.pin)))return false;s.started=true;
    if(!s.channel.prepare())return false;
    const auto request=packet(hello,s.mode,s.channel.public_key+s.channel.nonce);std::string response;std::string_view body;
    if(!exchange(request,response)||!unpack(response,welcome,s.mode,body)||body.size()!=32+public_size+nonce_size||!valid_id(body.substr(0,32)))return false;
    s.peer=body.substr(0,32);const auto transcript=request+response;
    if(!s.channel.derive(body.substr(32,public_size),transcript,false))return false;
    if(s.mode==AdmissionMode::button){
        std::string encrypted,plain;if(!s.channel.seal(binding("client",s.channel),encrypted)||!exchange(packet(finish,s.mode,encrypted),response)||!unpack(response,confirmed,s.mode,body)||!s.channel.open(body,plain)||!equal(plain,binding("server",s.channel)))return false;
    }else{
        auto password=pin_password(s.peer,s.pin);s.pin=0;Security2Client srp_client(s.peer,password);wipe(password);std::string message;
        if(!srp_client.begin(message)||!exchange(packet(srp,s.mode,message),response)||!unpack(response,srp_response,s.mode,body)||!srp_client.challenge(body,message)||!exchange(packet(srp,s.mode,message),response)||!unpack(response,srp_response,s.mode,body)||!srp_client.verify(body))return false;
        std::string encrypted,plain;if(!srp_client.encrypt(binding("client",s.channel),encrypted)||!exchange(packet(bind,s.mode,encrypted),response)||!unpack(response,bound,s.mode,body)||!srp_client.decrypt(body,plain)||!equal(plain,binding("server",s.channel)))return false;
    }
    s.ready=true;return true;
}
bool AdmissionClient::encrypt(std::string_view value,std::string& out){return state_->ready&&state_->channel.seal(value,out);}
bool AdmissionClient::decrypt(std::string_view value,std::string& out){return state_->ready&&state_->channel.open(value,out);}
struct AdmissionServer::State {
    AdmissionMode mode;std::string id;uint32_t pin;Channel channel;unsigned phase=0,srp_round=0;bool ready=false,binding_verified=false;
    protocomm_t* pc=nullptr;std::string salt,verifier;protocomm_security2_params_t parameters{};
    State(AdmissionMode m,std::string identity,uint32_t p):mode(m),id(std::move(identity)),pin(p){}
    ~State(){if(pc){protocomm_close_session(pc,1);protocomm_delete(pc);}wipe(salt);wipe(verifier);pin=0;}
    static esp_err_t bind_endpoint(uint32_t,const uint8_t* bytes,ssize_t size,uint8_t** out,ssize_t* length,void* context){
        auto& self=*static_cast<State*>(context);if(size<=0||!equal(std::string_view(reinterpret_cast<const char*>(bytes),size),binding("client",self.channel)))return ESP_ERR_INVALID_ARG;
        auto response=binding("server",self.channel);*out=static_cast<uint8_t*>(malloc(response.size()));if(!*out)return ESP_ERR_NO_MEM;
        memcpy(*out,response.data(),response.size());*length=response.size();self.binding_verified=true;return ESP_OK;
    }
    bool prepare_srp(){
        auto password=pin_password(id,pin);pin=0;if(password.empty())return false;
        char *salt_bytes=nullptr,*verifier_bytes=nullptr;int verifier_size=0;
        auto result=esp_srp_gen_salt_verifier(id.data(),id.size(),password.data(),password.size(),&salt_bytes,32,&verifier_bytes,&verifier_size);wipe(password);
        if(result!=ESP_OK||!salt_bytes||!verifier_bytes||verifier_size<=0||verifier_size>384){free(salt_bytes);free(verifier_bytes);return false;}
        salt.assign(salt_bytes,32);verifier.assign(384-verifier_size,0);verifier.append(verifier_bytes,verifier_size);mbedtls_platform_zeroize(salt_bytes,32);mbedtls_platform_zeroize(verifier_bytes,verifier_size);free(salt_bytes);free(verifier_bytes);
        parameters={salt.data(),uint16_t(salt.size()),verifier.data(),uint16_t(verifier.size())};pc=protocomm_new();
        return pc&&protocomm_set_security(pc,"session",&protocomm_security2,&parameters)==ESP_OK&&protocomm_add_endpoint(pc,"bind",bind_endpoint,this)==ESP_OK&&protocomm_open_session(pc,1)==ESP_OK;
    }
};
AdmissionServer::AdmissionServer(AdmissionMode mode,std::string own_id,uint32_t pin):state_(new State(mode,std::move(own_id),pin)){}
AdmissionServer::~AdmissionServer()=default;
bool AdmissionServer::authenticated()const{return state_->ready&&!state_->channel.failed;}
bool AdmissionServer::handshake(std::string_view input,std::string& output){
    auto& s=*state_;output.clear();std::string_view body;bool good=false;
    if(s.phase==0){
        if(!valid_id(s.id)||!unpack(input,hello,s.mode,body)||body.size()!=public_size+nonce_size||!s.channel.prepare())goto failed;
        output=packet(welcome,s.mode,s.id+s.channel.public_key+s.channel.nonce);
        if(!s.channel.derive(body.substr(0,public_size),std::string(input)+output,true))goto failed;
        if(s.mode==AdmissionMode::setup_code&&!s.prepare_srp())goto failed;
        s.phase=1;return true;
    }
    if(s.mode==AdmissionMode::button&&s.phase==1){
        std::string plain,encrypted;if(!unpack(input,finish,s.mode,body)||!s.channel.open(body,plain)||!equal(plain,binding("client",s.channel))||!s.channel.seal(binding("server",s.channel),encrypted))goto failed;
        output=packet(confirmed,s.mode,encrypted);s.phase=3;s.ready=true;return true;
    }
    if(s.mode==AdmissionMode::setup_code&&s.phase==1&&s.srp_round<2){
        if(!unpack(input,srp,s.mode,body)||body.empty()||body.size()>4096)goto failed;
        uint8_t* response=nullptr;ssize_t size=0;auto result=protocomm_req_handle(s.pc,"session",1,reinterpret_cast<const uint8_t*>(body.data()),body.size(),&response,&size);
        good=result==ESP_OK&&response&&size>0&&size<=4096;
        if(good){auto* parsed=session_data__unpack(nullptr,size,response);good=parsed&&parsed->sec_ver==SEC_SCHEME_VERSION__SecScheme2&&parsed->sec2;
            if(good)good=s.srp_round==0?(parsed->sec2->msg==SEC2_MSG_TYPE__S2Session_Response0&&parsed->sec2->sr0&&parsed->sec2->sr0->status==STATUS__Success):(parsed->sec2->msg==SEC2_MSG_TYPE__S2Session_Response1&&parsed->sec2->sr1&&parsed->sec2->sr1->status==STATUS__Success);
            if(parsed)session_data__free_unpacked(parsed,nullptr);
        }
        if(good){output=packet(srp_response,s.mode,std::string_view(reinterpret_cast<char*>(response),size));++s.srp_round;if(s.srp_round==2)s.phase=2;}free(response);if(!good)goto failed;return true;
    }
    if(s.mode==AdmissionMode::setup_code&&s.phase==2){
        if(!unpack(input,bind,s.mode,body)||body.empty()||body.size()>4096)goto failed;
        uint8_t* response=nullptr;ssize_t size=0;auto result=protocomm_req_handle(s.pc,"bind",1,reinterpret_cast<const uint8_t*>(body.data()),body.size(),&response,&size);
        good=result==ESP_OK&&response&&size>0&&size<=4096&&s.binding_verified;
        if(good){output=packet(bound,s.mode,std::string_view(reinterpret_cast<char*>(response),size));s.phase=3;s.ready=true;}free(response);if(!good)goto failed;return true;
    }
failed:
    s.phase=4;s.channel.failed=true;s.ready=false;output.clear();return false;
}
bool AdmissionServer::encrypt(std::string_view value,std::string& out){return authenticated()&&state_->channel.seal(value,out);}
bool AdmissionServer::decrypt(std::string_view value,std::string& out){return authenticated()&&state_->channel.open(value,out);}
bool admission_crypto_self_test(AdmissionMode mode){
    if(mode!=AdmissionMode::button&&mode!=AdmissionMode::setup_code)return false;
    uint32_t parsed=0;if(!parse_admission_setup_code("3497-011-2332",parsed)||parsed!=20202021||!parse_admission_setup_code("MT:Y.K9042C00KA0648G00",parsed)||parsed!=20202021||parse_admission_setup_code("3497-011-2333",parsed)||parse_admission_setup_code("00000000000",parsed))return false;
    const std::string id="00000000000000000000000000000001";
    {
        AdmissionServer server(mode,id,20202021);AdmissionClient client(mode,20202021);
        if(!client.handshake([&](auto input,auto& output){return server.handshake(input,output);})||client.peer_id()!=id||!server.authenticated())return false;
        std::string request,response,plain;if(!client.encrypt("client message",request)||!server.decrypt(request,plain)||plain!="client message"||!server.encrypt("server message",response)||!client.decrypt(response,plain)||plain!="server message")return false;
        if(server.decrypt(request,plain)||server.authenticated())return false;
        AdmissionServer other(mode,id,20202021);AdmissionClient wrong(mode,20202022);
        if(mode==AdmissionMode::setup_code&&wrong.handshake([&](auto input,auto& output){return other.handshake(input,output);}))return false;
        AdmissionServer mismatched(mode==AdmissionMode::button?AdmissionMode::setup_code:AdmissionMode::button,id,20202021);AdmissionClient mismatch(mode,20202021);
        if(mismatch.handshake([&](auto input,auto& output){return mismatched.handshake(input,output);}))return false;
        AdmissionServer tampered(mode,id,20202021);AdmissionClient fresh(mode,20202021);
        if(!fresh.handshake([&](auto input,auto& output){return tampered.handshake(input,output);})||!fresh.encrypt("tamper",request))return false;
        request.back()^=1;if(tampered.decrypt(request,plain))return false;
    }
    return true;
}
bool admission_crypto_self_test(){return admission_crypto_self_test(AdmissionMode::button)&&admission_crypto_self_test(AdmissionMode::setup_code);}
} // namespace amity::consortium::platform
#endif
