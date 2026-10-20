// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#ifdef ESP_PLATFORM
#include "platform.hpp"
#include "amity/consortium/hostname.hpp"
#include "nvs.h"
#include "config_store.hpp"
#include "esp_random.h"
#include "esp_timer.h"
#include "mbedtls/oid.h"
#include "psa/crypto.h"
#include <algorithm>
#include <cstring>
namespace amity::consortium::platform {
std::string hex(std::string_view bytes){static const char digits[]="0123456789abcdef";std::string out(bytes.size()*2,'0');for(size_t i=0;i<bytes.size();++i){auto b=static_cast<uint8_t>(bytes[i]);out[2*i]=digits[b>>4];out[2*i+1]=digits[b&15];}return out;}
bool unhex(std::string_view value,std::string &out,size_t cap){if(value.size()%2||value.size()/2>cap)return false;out.clear();out.reserve(value.size()/2);auto nibble=[](char c){return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:-1;};for(size_t i=0;i<value.size();i+=2){int a=nibble(value[i]),b=nibble(value[i+1]);if(a<0||b<0){out.clear();return false;}out.push_back(char((a<<4)|b));}return true;}
std::string sha256(std::string_view bytes){uint8_t hash[32];size_t size=0;if(psa_hash_compute(PSA_ALG_SHA_256,reinterpret_cast<const uint8_t*>(bytes.data()),bytes.size(),hash,sizeof(hash),&size)!=PSA_SUCCESS||size!=32)return {};return hex(std::string_view(reinterpret_cast<char*>(hash),32));}
std::string random_hex(size_t count){if(!count||count>32)return {};uint8_t bytes[32];esp_fill_random(bytes,count);return hex(std::string_view(reinterpret_cast<char*>(bytes),count));}
uint64_t now_ms(){return static_cast<uint64_t>(esp_timer_get_time()/1000);}
static esp_err_t read_record_internal(const char *space,const char *key,std::string &out,size_t cap){nvs_handle_t h;auto e=nvs_open_from_partition("amity_cfg",space,NVS_READONLY,&h);if(e!=ESP_OK)return e;size_t size=0;e=nvs_get_blob(h,key,nullptr,&size);if(e==ESP_OK){if(!size||size>cap)e=ESP_ERR_INVALID_SIZE;else {out.resize(size);e=nvs_get_blob(h,key,out.data(),&size);if(e==ESP_OK&&size!=out.size())e=ESP_ERR_INVALID_SIZE;}}nvs_close(h);return e;}
static esp_err_t write_record_internal(const char *space,const char *key,std::string_view bytes){if(bytes.empty()||bytes.size()>32768)return ESP_ERR_INVALID_SIZE;nvs_handle_t h;auto e=nvs_open_from_partition("amity_cfg",space,NVS_READWRITE,&h);if(e!=ESP_OK)return e;e=nvs_set_blob(h,key,bytes.data(),bytes.size());if(e==ESP_OK)e=nvs_commit(h);nvs_close(h);return e;}
esp_err_t read_record(const char *space,const char *key,std::string &out,size_t cap){esp_err_t result=ESP_FAIL;return config_store::with_internal_flash_stack([&]{result=read_record_internal(space,key,out,cap);})?result:ESP_FAIL;}
esp_err_t write_record(const char *space,const char *key,std::string_view bytes){esp_err_t result=ESP_FAIL;return config_store::with_internal_flash_stack([&]{result=write_record_internal(space,key,bytes);})?result:ESP_FAIL;}
esp_err_t erase_record(const char *space,const char *key){
    esp_err_t result=ESP_FAIL;const bool completed=config_store::with_internal_flash_stack([&]{nvs_handle_t handle;result=nvs_open_from_partition("amity_cfg",space,NVS_READWRITE,&handle);if(result!=ESP_OK)return;result=key?nvs_erase_key(handle,key):nvs_erase_all(handle);if(result==ESP_ERR_NVS_NOT_FOUND)result=ESP_OK;if(result==ESP_OK)result=nvs_commit(handle);nvs_close(handle);});return completed?result:ESP_FAIL;
}
esp_err_t erase_namespace(const char *space){return erase_record(space,nullptr);}
namespace {
bool parse_certificate(std::string_view der,model::OctopusId id,mbedtls_x509_crt &crt,std::string &pin){
    if(der.empty()||der.size()>1024||!id||mbedtls_x509_crt_parse_der(&crt,reinterpret_cast<const uint8_t*>(der.data()),der.size()))return false;
    const int required_extensions=MBEDTLS_X509_EXT_BASIC_CONSTRAINTS|MBEDTLS_X509_EXT_KEY_USAGE|MBEDTLS_X509_EXT_EXTENDED_KEY_USAGE|MBEDTLS_X509_EXT_SUBJECT_ALT_NAME;
    if((crt.MBEDTLS_PRIVATE(ext_types)&required_extensions)!=required_extensions||crt.issuer_raw.len!=crt.subject_raw.len||memcmp(crt.issuer_raw.p,crt.subject_raw.p,crt.subject_raw.len)||mbedtls_pk_get_key_type(&crt.pk)!=PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_SECP_R1))return false;
    if(crt.next||crt.MBEDTLS_PRIVATE(ca_istrue)||crt.version!=3||crt.MBEDTLS_PRIVATE(key_usage)!=MBEDTLS_X509_KU_DIGITAL_SIGNATURE||mbedtls_pk_get_bitlen(&crt.pk)!=256||!mbedtls_pk_can_do_psa(&crt.pk,PSA_ALG_ECDSA(PSA_ALG_SHA_256),PSA_KEY_USAGE_VERIFY_HASH))return false;
    const std::string expected="o-"+id.str()+".amity.invalid";
    bool san=false;
    for(auto entry=&crt.subject_alt_names;entry&&entry->buf.p;entry=entry->next){if(san)return false;mbedtls_x509_subject_alternative_name name{};if(mbedtls_x509_parse_subject_alt_name(&entry->buf,&name)||name.type!=MBEDTLS_X509_SAN_DNS_NAME)return false;auto &value=name.san.unstructured_name;if(value.len!=expected.size()||std::memcmp(value.p,expected.data(),value.len))return false;san=true;}
    if(!san||mbedtls_x509_crt_check_extended_key_usage(&crt,MBEDTLS_OID_SERVER_AUTH,MBEDTLS_OID_SIZE(MBEDTLS_OID_SERVER_AUTH))||mbedtls_x509_crt_check_extended_key_usage(&crt,MBEDTLS_OID_CLIENT_AUTH,MBEDTLS_OID_SIZE(MBEDTLS_OID_CLIENT_AUTH)))return false;
    uint32_t flags=0;int result=mbedtls_x509_crt_verify(&crt,&crt,nullptr,expected.c_str(),&flags,nullptr,nullptr);
    if(result&&((flags&~uint32_t(MBEDTLS_X509_BADCERT_EXPIRED|MBEDTLS_X509_BADCERT_FUTURE))||!flags))return false;
    uint8_t digest[32];size_t hash_size=0;
    if(psa_hash_compute(PSA_ALG_SHA_256,crt.tbs.p,crt.tbs.len,digest,sizeof(digest),&hash_size)!=PSA_SUCCESS||hash_size!=32||crt.MBEDTLS_PRIVATE(sig_md)!=MBEDTLS_MD_SHA256||mbedtls_pk_verify(&crt.pk,MBEDTLS_MD_SHA256,digest,hash_size,crt.MBEDTLS_PRIVATE(sig).p,crt.MBEDTLS_PRIVATE(sig).len))return false;
    uint8_t spki[128];int length=mbedtls_pk_write_pubkey_der(&crt.pk,spki,sizeof(spki));if(length<=0)return false;
    pin=sha256(std::string_view(reinterpret_cast<char*>(spki+sizeof(spki)-length),length));return pin.size()==64;
}
bool decode_identity(std::string_view record,model::OctopusId id,Identity &out){Json json;JsonError err;if(!parse_json(record,json,err)||!closed(json,{"version","octopus_id","leaf","key"}))return false;uint64_t version=0;model::OctopusId stored;if(!json_uint(json.get("version"),version,1,1)||!consortium::id(json.get("octopus_id"),stored)||stored!=id)return false;std::string leaf,key;if(!json.get("leaf")||json.get("leaf")->type!=Json::Type::string||!json.get("key")||json.get("key")->type!=Json::Type::string||!unhex(json.get("leaf")->string,leaf,1024)||!unhex(json.get("key")->string,key,512))return false;std::string pin;if(!certificate(leaf,id,pin))return false;mbedtls_pk_context parsed;mbedtls_pk_init(&parsed);mbedtls_x509_crt crt;mbedtls_x509_crt_init(&crt);bool valid=mbedtls_pk_parse_key(&parsed,reinterpret_cast<const uint8_t*>(key.data()),key.size(),nullptr,0)==0&&mbedtls_x509_crt_parse_der(&crt,reinterpret_cast<const uint8_t*>(leaf.data()),leaf.size())==0&&mbedtls_pk_check_pair(&crt.pk,&parsed)==0;mbedtls_x509_crt_free(&crt);mbedtls_pk_free(&parsed);if(!valid)return false;out={id,std::move(leaf),std::move(key),std::move(pin)};return true;}
}
bool certificate(std::string_view der,model::OctopusId id,std::string &pin){mbedtls_x509_crt crt;mbedtls_x509_crt_init(&crt);bool valid=parse_certificate(der,id,crt,pin);mbedtls_x509_crt_free(&crt);return valid;}
esp_err_t load_identity(model::OctopusId id,Identity &out){
    std::string committed,candidate;auto marker=read_record("amity_group","identity",committed,2048);auto pending=read_record("amity_group","identity_tmp",candidate,2048);
    if(marker==ESP_OK){if(pending!=ESP_OK||candidate!=committed||!decode_identity(committed,id,out))return ESP_ERR_INVALID_STATE;return ESP_OK;}
    if(marker!=ESP_ERR_NVS_NOT_FOUND)return marker;
    if(pending==ESP_ERR_NVS_NOT_FOUND){
        psa_key_attributes_t attr=PSA_KEY_ATTRIBUTES_INIT;psa_set_key_type(&attr,PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));psa_set_key_bits(&attr,256);psa_set_key_algorithm(&attr,PSA_ALG_ECDSA(PSA_ALG_SHA_256));psa_set_key_usage_flags(&attr,PSA_KEY_USAGE_SIGN_HASH|PSA_KEY_USAGE_EXPORT|PSA_KEY_USAGE_COPY);mbedtls_svc_key_id_t key_id=0;if(psa_generate_key(&attr,&key_id)!=PSA_SUCCESS)return ESP_FAIL;
        mbedtls_pk_context key;mbedtls_pk_init(&key);int r=mbedtls_pk_copy_from_psa(key_id,&key);psa_destroy_key(key_id);mbedtls_x509write_cert cert;mbedtls_x509write_crt_init(&cert);
        uint8_t serial[16];esp_fill_random(serial,sizeof(serial));serial[0]=(serial[0]&0x7f)|1;
        mbedtls_x509write_crt_set_version(&cert,MBEDTLS_X509_CRT_VERSION_3);mbedtls_x509write_crt_set_md_alg(&cert,MBEDTLS_MD_SHA256);mbedtls_x509write_crt_set_subject_key(&cert,&key);mbedtls_x509write_crt_set_issuer_key(&cert,&key);
        const std::string dns="o-"+id.str()+".amity.invalid",subject="CN="+dns+",O=Amity Consortium";
        if(!r)r=mbedtls_x509write_crt_set_serial_raw(&cert,serial,sizeof(serial));
        if(!r)r=mbedtls_x509write_crt_set_subject_name(&cert,subject.c_str());
        if(!r)r=mbedtls_x509write_crt_set_issuer_name(&cert,subject.c_str());
        if(!r)r=mbedtls_x509write_crt_set_validity(&cert,"20240101000000","20500101000000");
        if(!r)r=mbedtls_x509write_crt_set_basic_constraints(&cert,0,-1);
        if(!r)r=mbedtls_x509write_crt_set_key_usage(&cert,MBEDTLS_X509_KU_DIGITAL_SIGNATURE);
        mbedtls_x509_san_list san{};san.node.type=MBEDTLS_X509_SAN_DNS_NAME;san.node.san.unstructured_name.p=reinterpret_cast<uint8_t*>(const_cast<char*>(dns.data()));san.node.san.unstructured_name.len=dns.size();
        if(!r)r=mbedtls_x509write_crt_set_subject_alternative_name(&cert,&san);
        mbedtls_asn1_sequence client{},server{};client.buf.tag=MBEDTLS_ASN1_OID;client.buf.p=reinterpret_cast<uint8_t*>(const_cast<char*>(MBEDTLS_OID_CLIENT_AUTH));client.buf.len=MBEDTLS_OID_SIZE(MBEDTLS_OID_CLIENT_AUTH);server.buf.tag=MBEDTLS_ASN1_OID;server.buf.p=reinterpret_cast<uint8_t*>(const_cast<char*>(MBEDTLS_OID_SERVER_AUTH));server.buf.len=MBEDTLS_OID_SIZE(MBEDTLS_OID_SERVER_AUTH);client.next=&server;
        if(!r)r=mbedtls_x509write_crt_set_ext_key_usage(&cert,&client);
        uint8_t der[1024],priv[512];int length=r?r:mbedtls_x509write_crt_der(&cert,der,sizeof(der)),key_length=length>0?mbedtls_pk_write_key_der(&key,priv,sizeof(priv)):-1;
        if(length>0&&key_length>0)candidate="{\"version\":1,\"octopus_id\":\""+id.str()+"\",\"leaf\":\""+hex(std::string_view(reinterpret_cast<char*>(der+sizeof(der)-length),length))+"\",\"key\":\""+hex(std::string_view(reinterpret_cast<char*>(priv+sizeof(priv)-key_length),key_length))+"\"}";
        std::fill(std::begin(priv),std::end(priv),0);mbedtls_x509write_crt_free(&cert);mbedtls_pk_free(&key);if(candidate.empty()||candidate.size()>2048||!decode_identity(candidate,id,out))return ESP_FAIL;auto e=write_record("amity_group","identity_tmp",candidate);if(e!=ESP_OK)return e;
    }else if(pending!=ESP_OK)return pending;
    if(!decode_identity(candidate,id,out))return ESP_ERR_INVALID_STATE;
    auto e=write_record("amity_group","identity",candidate);if(e!=ESP_OK)return e;std::string verify;if(read_record("amity_group","identity",verify,8192)!=ESP_OK||verify!=candidate)return ESP_ERR_INVALID_STATE;return ESP_OK;
}
esp_err_t load_group(Group &out){
    std::string text;auto e=read_record("amity_group","state",text,32768);if(e==ESP_ERR_NVS_NOT_FOUND){out={};return ESP_OK;}if(e!=ESP_OK)return e;Json j;JsonError error;if(!parse_json(text,j,error,32768)||!closed(j,{"version","role","consortium","authority","epoch","revision","host","join_id","joining_id","join_digest","join_revision","peers"},{"mutation_id","mutation_digest","handover_id","handover_digest"}))return ESP_ERR_INVALID_STATE;uint64_t version=0,role=0;Group g;if(!json_uint(j.get("version"),version,1,1)||!json_uint(j.get("role"),role,1,4)||!id(j.get("consortium"),g.consortium)||!id(j.get("authority"),g.authority)||!decimal(j.get("epoch"),g.epoch)||!g.epoch||!decimal(j.get("revision"),g.revision)||!g.revision||!json_string(j.get("host"),g.authority_host,0,64))return ESP_ERR_INVALID_STATE;if(!std::all_of(g.authority_host.begin(),g.authority_host.end(),[](unsigned char c){return (c>='0'&&c<='9')||(c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='.'||c=='-';}))return ESP_ERR_INVALID_STATE;
    g.role=static_cast<Group::Role>(role);
    if(j.get("mutation_id")||j.get("mutation_digest")){if(!id(j.get("mutation_id"),g.mutation_id)||!json_string(j.get("mutation_digest"),g.mutation_digest,64,64)||!std::all_of(g.mutation_digest.begin(),g.mutation_digest.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');}))return ESP_ERR_INVALID_STATE;}
    if(g.role==Group::retired&&!g.mutation_id)return ESP_ERR_INVALID_STATE;
    if(j.get("handover_id")||j.get("handover_digest")||g.epoch!=1)return ESP_ERR_INVALID_STATE;
    std::string jid,joining;
    if(!json_string(j.get("join_id"),jid,0,32)||!json_string(j.get("joining_id"),joining,0,32)||!json_string(j.get("join_digest"),g.join_digest,0,64)||!decimal(j.get("join_revision"),g.join_revision))return ESP_ERR_INVALID_STATE;
    if(!jid.empty()){auto parsed=model::JoinId::parse(jid);auto member=model::OctopusId::parse(joining);if(!parsed||!member||g.join_digest.size()!=64||!std::all_of(g.join_digest.begin(),g.join_digest.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');})||!g.join_revision)return ESP_ERR_INVALID_STATE;g.join_id=*parsed;g.joining_id=*member;}else if(!joining.empty()||!g.join_digest.empty()||!g.join_hostname.empty()||g.join_revision)return ESP_ERR_INVALID_STATE;
    auto peers=j.get("peers");if(peers->type!=Json::Type::array||peers->array.size()>7)return ESP_ERR_INVALID_STATE;
    for(const auto &p:peers->array){Peer peer;if(!closed(p,{"id","leaf","pin","active"},{"input_gateway"})||!id(p.get("id"),peer.id)||!json_bool(p.get("active"),peer.active)||(p.get("input_gateway")&&p.get("input_gateway")->type!=Json::Type::boolean)||p.get("leaf")->type!=Json::Type::string||!unhex(p.get("leaf")->string,peer.der,1024)||!json_string(p.get("pin"),peer.pin,64,64))return ESP_ERR_INVALID_STATE;std::string derived;if(!certificate(peer.der,peer.id,derived)||derived!=peer.pin)return ESP_ERR_INVALID_STATE;for(const auto &old:g.peers)if(old.id==peer.id||old.pin==peer.pin)return ESP_ERR_INVALID_STATE;g.peers.push_back(std::move(peer));}
    if(g.role==Group::coordinator){
        if(!g.authority_host.empty())return ESP_ERR_INVALID_STATE;
        if(g.join_id){auto joining=std::find_if(g.peers.begin(),g.peers.end(),[&](const auto &p){return p.id==g.joining_id;});if(joining==g.peers.end()||g.join_revision<g.revision||(g.join_revision-g.revision)>1)return ESP_ERR_INVALID_STATE;}
    }else if(g.role==Group::member||g.role==Group::prepared){
        if(g.peers.size()!=1||g.peers[0].id!=g.authority||g.authority_host.empty()||!g.join_id||g.join_revision>g.revision||(g.role==Group::member&&!g.peers[0].active)||(g.role==Group::prepared&&g.peers[0].active))return ESP_ERR_INVALID_STATE;
    }
    // Optional public transaction metadata is separate so old firmware can
    // still read the unchanged membership state after rollback. Stale/corrupt
    // names never alter role or trust; only an exact join/digest can use one.
    if(g.join_id){std::string named;Json metadata;JsonError detail;model::JoinId join;std::string digest,name;
        if(read_record("amity_group","join_name",named,512)==ESP_OK&&parse_json(named,metadata,detail,512)&&closed(metadata,{"join_id","join_digest","hostname"})&&id(metadata.get("join_id"),join)&&json_string(metadata.get("join_digest"),digest,64,64)&&json_string(metadata.get("hostname"),name,1,32)&&valid_hostname(name)&&join==g.join_id&&digest==g.join_digest)g.join_hostname=std::move(name);
    }
    out=std::move(g);return ESP_OK;
}
esp_err_t save_group(const Group &g){if(g.role==Group::unclaimed||g.peers.size()>2||!g.consortium||!g.authority||g.epoch!=1||(!g.join_hostname.empty()&&(!g.join_id||!valid_hostname(g.join_hostname))))return ESP_ERR_INVALID_ARG;std::string json="{\"version\":1,\"role\":"+std::to_string(g.role)+",\"consortium\":\""+g.consortium.str()+"\",\"authority\":\""+g.authority.str()+"\",\"epoch\":\""+std::to_string(g.epoch)+"\",\"revision\":\""+std::to_string(g.revision)+"\",\"host\":\""+g.authority_host+"\",\"join_id\":\""+(g.join_id?g.join_id.str():"")+"\",\"joining_id\":\""+(g.joining_id?g.joining_id.str():"")+"\",\"join_digest\":\""+g.join_digest+"\",\"join_revision\":\""+std::to_string(g.join_revision)+"\",\"peers\":[";for(size_t i=0;i<g.peers.size();++i){const auto &p=g.peers[i];if(i)json+=",";json+="{\"id\":\""+p.id.str()+"\",\"leaf\":\""+hex(p.der)+"\",\"pin\":\""+p.pin+"\",\"active\":"+(p.active?"true":"false")+"}";}json+="]";if(g.mutation_id)json+=",\"mutation_id\":\""+g.mutation_id.str()+"\",\"mutation_digest\":\""+g.mutation_digest+"\"";json+="}";if(json.size()>6144)return ESP_ERR_INVALID_SIZE;
    if(!g.join_hostname.empty()){const auto named="{\"join_id\":\""+g.join_id.str()+"\",\"join_digest\":\""+g.join_digest+"\",\"hostname\":\""+g.join_hostname+"\"}";const auto saved=write_record("amity_group","join_name",named);if(saved!=ESP_OK)return saved;}
    return write_record("amity_group","state",json);}
} // namespace
#endif
