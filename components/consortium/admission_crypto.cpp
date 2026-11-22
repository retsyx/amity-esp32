// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#ifdef ESP_PLATFORM
#include "admission_crypto.hpp"
#include "psa/crypto.h"
#include "mbedtls/platform_util.h"
#include <algorithm>
#include <array>

namespace amity::consortium::platform {
namespace {
constexpr std::string_view magic="AMADMIT4";
constexpr size_t public_size=65,nonce_size=32,max_plain=4000;
enum Message:uint8_t {hello=1,welcome=2,finish=3,confirmed=4};
void wipe(std::string& value){if(!value.empty())mbedtls_platform_zeroize(value.data(),value.size());value.clear();}
bool equal(std::string_view a,std::string_view b){if(a.size()!=b.size())return false;uint8_t difference=0;for(size_t i=0;i<a.size();++i)difference|=uint8_t(a[i])^uint8_t(b[i]);return difference==0;}
std::string hash(std::string_view input){std::string output(32,0);size_t size=0;if(psa_hash_compute(PSA_ALG_SHA_256,reinterpret_cast<const uint8_t*>(input.data()),input.size(),reinterpret_cast<uint8_t*>(output.data()),output.size(),&size)!=PSA_SUCCESS||size!=32)return {};return output;}
bool valid_id(std::string_view id){return id.size()==32&&id!=std::string(32,'0')&&std::all_of(id.begin(),id.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');});}
std::string packet(Message kind,std::string_view payload){std::string value(magic);value+=char(kind);value.append(payload);return value;}
bool unpack(std::string_view value,Message kind,std::string_view& payload){if(value.size()<9||value.substr(0,8)!=magic||uint8_t(value[8])!=kind)return false;payload=value.substr(9);return true;}
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
struct AdmissionClient::State {Channel channel;std::string peer;bool started=false,ready=false;};
AdmissionClient::AdmissionClient():state_(new State){}
AdmissionClient::~AdmissionClient()=default;
const std::string& AdmissionClient::peer_id()const{return state_->peer;}
bool AdmissionClient::handshake(const std::function<bool(std::string_view,std::string&)>& exchange){
    auto& s=*state_;if(s.started||!exchange)return false;s.started=true;
    if(!s.channel.prepare())return false;
    const auto request=packet(hello,s.channel.public_key+s.channel.nonce);std::string response;std::string_view body;
    if(!exchange(request,response)||!unpack(response,welcome,body)||body.size()!=32+public_size+nonce_size||!valid_id(body.substr(0,32)))return false;
    s.peer=body.substr(0,32);const auto transcript=request+response;
    if(!s.channel.derive(body.substr(32,public_size),transcript,false))return false;
    std::string encrypted,plain;if(!s.channel.seal(binding("client",s.channel),encrypted)||!exchange(packet(finish,encrypted),response)||!unpack(response,confirmed,body)||!s.channel.open(body,plain)||!equal(plain,binding("server",s.channel)))return false;
    s.ready=true;return true;
}
bool AdmissionClient::encrypt(std::string_view value,std::string& out){return state_->ready&&state_->channel.seal(value,out);}
bool AdmissionClient::decrypt(std::string_view value,std::string& out){return state_->ready&&state_->channel.open(value,out);}
struct AdmissionServer::State {
    std::string id;Channel channel;unsigned phase=0;bool ready=false;
    explicit State(std::string identity):id(std::move(identity)){}
};
AdmissionServer::AdmissionServer(std::string own_id):state_(new State(std::move(own_id))){}
AdmissionServer::~AdmissionServer()=default;
bool AdmissionServer::authenticated()const{return state_->ready&&!state_->channel.failed;}
bool AdmissionServer::handshake(std::string_view input,std::string& output){
    auto& s=*state_;output.clear();std::string_view body;
    if(s.phase==0){
        if(!valid_id(s.id)||!unpack(input,hello,body)||body.size()!=public_size+nonce_size||!s.channel.prepare())goto failed;
        output=packet(welcome,s.id+s.channel.public_key+s.channel.nonce);
        if(!s.channel.derive(body.substr(0,public_size),std::string(input)+output,true))goto failed;
        s.phase=1;return true;
    }
    if(s.phase==1){
        std::string plain,encrypted;if(!unpack(input,finish,body)||!s.channel.open(body,plain)||!equal(plain,binding("client",s.channel))||!s.channel.seal(binding("server",s.channel),encrypted))goto failed;
        output=packet(confirmed,encrypted);s.phase=2;s.ready=true;return true;
    }
failed:
    s.phase=3;s.channel.failed=true;s.ready=false;output.clear();return false;
}
bool AdmissionServer::encrypt(std::string_view value,std::string& out){return authenticated()&&state_->channel.seal(value,out);}
bool AdmissionServer::decrypt(std::string_view value,std::string& out){return authenticated()&&state_->channel.open(value,out);}
} // namespace amity::consortium::platform
#endif
