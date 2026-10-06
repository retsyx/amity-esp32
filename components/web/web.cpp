#include "web.hpp"
#include "config_store.hpp"
#include "esp_https_server.h"
#include "esp_random.h"
#include "mbedtls/pk.h"
#include "mbedtls/x509_crt.h"
#include "psa/crypto.h"
#include <algorithm>
#include <cstring>
namespace web {
namespace {
std::string certificate,private_key;
esp_err_t tls_identity(const std::string &hostname) {
    auto a=config_store::read("tls_cert",certificate),b=config_store::read("tls_key",private_key);
    if(a==ESP_OK && b==ESP_OK) return ESP_OK;
    if((a!=ESP_OK && a!=ESP_ERR_NVS_NOT_FOUND)||(b!=ESP_OK && b!=ESP_ERR_NVS_NOT_FOUND)) return ESP_FAIL;
    psa_key_attributes_t attributes=PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attributes,PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attributes,256);
    psa_set_key_algorithm(&attributes,PSA_ALG_ECDSA(PSA_ALG_SHA_256));
    psa_set_key_usage_flags(&attributes,PSA_KEY_USAGE_SIGN_HASH|PSA_KEY_USAGE_EXPORT|PSA_KEY_USAGE_COPY);
    mbedtls_svc_key_id_t id=0;
    if(psa_generate_key(&attributes,&id)!=PSA_SUCCESS) return ESP_FAIL;
    mbedtls_pk_context key;
    mbedtls_pk_init(&key);
    int r=mbedtls_pk_copy_from_psa(id,&key);
    psa_destroy_key(id);
    mbedtls_x509write_cert cert;
    mbedtls_x509write_crt_init(&cert);
    uint8_t serial[16];esp_fill_random(serial,sizeof(serial));serial[0]&=0x7f;
    mbedtls_x509write_crt_set_version(&cert,MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&cert,MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&cert,&key);
    mbedtls_x509write_crt_set_issuer_key(&cert,&key);
    if(!r) r=mbedtls_x509write_crt_set_serial_raw(&cert,serial,sizeof(serial));
    const std::string subject="CN="+hostname+",O=Amity";
    if(!r) r=mbedtls_x509write_crt_set_subject_name(&cert,subject.c_str());
    if(!r) r=mbedtls_x509write_crt_set_issuer_name(&cert,subject.c_str());
    if(!r) r=mbedtls_x509write_crt_set_validity(&cert,"20240101000000","20500101000000");
    if(!r) r=mbedtls_x509write_crt_set_basic_constraints(&cert,0,-1);
    if(!r) r=mbedtls_x509write_crt_set_key_usage(&cert,MBEDTLS_X509_KU_DIGITAL_SIGNATURE);
    unsigned char pem[1600]{};
    if(!r) r=mbedtls_x509write_crt_pem(&cert,pem,sizeof(pem));
    if(!r) certificate=reinterpret_cast<char *>(pem);
    if(!r) r=mbedtls_pk_write_key_pem(&key,pem,sizeof(pem));
    if(!r) private_key=reinterpret_cast<char *>(pem);
    mbedtls_x509write_crt_free(&cert);mbedtls_pk_free(&key);
    if(r) return ESP_FAIL;
    auto e=config_store::write("tls_key",private_key);
    return e==ESP_OK?config_store::write("tls_cert",certificate):e;
}
}
esp_err_t start(httpd_handle_t &handle,const std::string &hostname,const std::string &initial_auth) {
    auto e=credentials_init(initial_auth);if(e!=ESP_OK)return e;
    e=tls_identity(hostname);if(e!=ESP_OK)return e;
    httpd_ssl_config_t cfg=HTTPD_SSL_CONFIG_DEFAULT();
    cfg.httpd.max_uri_handlers=32;
    cfg.httpd.stack_size=12288;
    cfg.httpd.max_open_sockets=4;
    cfg.httpd.lru_purge_enable=true;
    cfg.httpd.recv_wait_timeout=15;
    cfg.httpd.send_wait_timeout=15;
    cfg.httpd.uri_match_fn=httpd_uri_match_wildcard;
    cfg.servercert=reinterpret_cast<const uint8_t *>(certificate.c_str());
    cfg.servercert_len=certificate.size()+1;
    cfg.prvtkey_pem=reinterpret_cast<const uint8_t *>(private_key.c_str());
    cfg.prvtkey_len=private_key.size()+1;
    return httpd_ssl_start(&handle,&cfg);
}
esp_err_t route(httpd_handle_t handle,const char *path,httpd_method_t method,Handler handler,void *context) {
    httpd_uri_t uri{};uri.uri=path;uri.method=method;uri.handler=handler;uri.user_ctx=context;
    return httpd_register_uri_handler(handle,&uri);
}
bool body(httpd_req_t *request,std::string &out,size_t limit) {
    if(request->content_len>limit) {error(request,"Request is too large","413 Content Too Large");return false;}
    out.resize(request->content_len);size_t offset=0;
    while(offset<out.size()) {
        int n=httpd_req_recv(request,out.data()+offset,out.size()-offset);
        if(n<=0) {error(request,"Incomplete request");return false;}
        offset+=n;
    }
    return true;
}
esp_err_t json(httpd_req_t *req,const std::string &data,const char *status) {
    httpd_resp_set_status(req,status);httpd_resp_set_type(req,"application/json");
    httpd_resp_set_hdr(req,"Cache-Control","no-store");
    httpd_resp_set_hdr(req,"X-Content-Type-Options","nosniff");
    return httpd_resp_send(req,data.data(),data.size());
}
esp_err_t error(httpd_req_t *req,const char *message,const char *status) {
    // Callers supply static messages or ESP-IDF error names, never unescaped user data.
    return json(req,std::string("{\"error\":\"")+message+"\"}",status);
}
}
