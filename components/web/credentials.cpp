/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#include "web.hpp"
#include "config_store.hpp"
#include "psa/crypto.h"
#include "esp_random.h"
#include "esp_timer.h"
#include <array>
#include <cstring>
namespace web {
namespace {
std::string stored;
struct Session { std::string token; int64_t expiry=0; };
std::array<Session,4> sessions;
int64_t next_login=0;
unsigned failures=0;
std::string hex(const uint8_t *p,size_t n) {
    static const char digits[]="0123456789abcdef";
    std::string s(n*2,'0');
    for(size_t i=0;i<n;++i) {s[2*i]=digits[p[i]>>4];s[2*i+1]=digits[p[i]&15];}
    return s;
}
std::string random_hex(size_t n) { uint8_t b[32]; esp_fill_random(b,n); return hex(b,n); }
bool equal(const std::string &a,const std::string &b) {
    if(a.size()!=b.size()) return false;
    unsigned diff=0;
    for(size_t i=0;i<a.size();++i) diff|=a[i]^b[i];
    return diff==0;
}
std::string derive(const std::string &password,const std::string &salt) {
    psa_key_derivation_operation_t op=PSA_KEY_DERIVATION_OPERATION_INIT;
    uint8_t out[32];
    psa_status_t s=psa_key_derivation_setup(&op,PSA_ALG_PBKDF2_HMAC(PSA_ALG_SHA_256));
    if(s==PSA_SUCCESS) s=psa_key_derivation_input_integer(&op,PSA_KEY_DERIVATION_INPUT_COST,10000);
    if(s==PSA_SUCCESS) s=psa_key_derivation_input_bytes(&op,PSA_KEY_DERIVATION_INPUT_SALT,reinterpret_cast<const uint8_t *>(salt.data()),salt.size());
    if(s==PSA_SUCCESS) s=psa_key_derivation_input_bytes(&op,PSA_KEY_DERIVATION_INPUT_PASSWORD,reinterpret_cast<const uint8_t *>(password.data()),password.size());
    if(s==PSA_SUCCESS) s=psa_key_derivation_output_bytes(&op,out,sizeof(out));
    psa_key_derivation_abort(&op);
    return s==PSA_SUCCESS?hex(out,sizeof(out)):std::string();
}
std::string bearer(httpd_req_t *request) {
    char value[96];
    if(httpd_req_get_hdr_value_str(request,"Authorization",value,sizeof(value))!=ESP_OK || strncmp(value,"Bearer ",7)!=0) return {};
    return value+7;
}
}
esp_err_t change_password(const std::string &password) {
    if(password.empty() || password.size()>128) return ESP_ERR_INVALID_ARG;
    auto salt=random_hex(16), digest=derive(password,salt);
    if(digest.empty()) return ESP_FAIL;
    std::string record=salt+":"+digest;
    auto e=config_store::write("admin_auth",record);
    if(e==ESP_OK) {stored=record;for(auto &s:sessions)s={};failures=0;next_login=0;}
    return e;
}
esp_err_t credentials_init(const std::string &initial_auth) {
    if(psa_crypto_init()!=PSA_SUCCESS) return ESP_FAIL;
    stored.clear();for(auto &s:sessions)s={};failures=0;next_login=0;
    auto e=config_store::read("admin_auth",stored);
    if(e==ESP_ERR_NVS_NOT_FOUND) {
        if(initial_auth.empty()) {stored.clear();return ESP_OK;}
        if(initial_auth.size()!=97 || initial_auth[32]!=':')return ESP_ERR_INVALID_STATE;
        e=config_store::write("admin_auth",initial_auth);
        if(e==ESP_OK)stored=initial_auth;
        return e;
    }
    if(e==ESP_OK && (stored.size()!=97 || stored[32]!=':')) return ESP_ERR_INVALID_STATE;
    return e;
}
bool setup_required() {return stored.empty();}
esp_err_t setup_password(const std::string &password) {
    if(!setup_required())return ESP_ERR_INVALID_STATE;
    return change_password(password);
}
unsigned login_retry_after() {
    auto remaining=next_login-esp_timer_get_time();
    return remaining>0?static_cast<unsigned>((remaining+999999)/1000000):0;
}
bool login(const std::string &password,std::string &token) {
    auto now=esp_timer_get_time();
    if(now<next_login || password.size()>128 || stored.size()!=97) return false;
    auto digest=derive(password,stored.substr(0,32));
    if(digest.empty() || !equal(digest,stored.substr(33))) {
        failures=failures<5?failures+1:5;
        next_login=now+static_cast<int64_t>(failures)*1000000;
        return false;
    }
    failures=0;
    Session *slot=&sessions[0];
    for(auto &s:sessions) if(s.expiry<slot->expiry) slot=&s;
    slot->token=random_hex(32); slot->expiry=now+8LL*60*60*1000000;
    token=slot->token;
    return true;
}
bool authenticated(httpd_req_t *request) {
    if(setup_required())return false;
    auto token=bearer(request);
    if(token.empty()) return false;
    for(auto &s:sessions) if(s.expiry>esp_timer_get_time() && equal(s.token,token))return true;
    return false;
}
void logout(httpd_req_t *request) { auto token=bearer(request);for(auto &s:sessions)if(equal(s.token,token))s={}; }
}
