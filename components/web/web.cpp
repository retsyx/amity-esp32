/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#include "web.hpp"
#include "stall.hpp"
#include "tls_identity.hpp"
#include "esp_https_server.h"
#include "esp_log.h"
#include "esp_tls.h"
#include "lwip/sockets.h"
#include <algorithm>
#include <cerrno>
#include <cstring>
namespace web {
namespace {
std::string certificate,private_key;
void configure_tcp_socket(int socket) {
    // Configure the accepted socket before the SDK sends its first TLS record.
    const int no_delay=1;
    if(setsockopt(socket,IPPROTO_TCP,TCP_NODELAY,&no_delay,sizeof(no_delay))!=0)
        ESP_LOGW("web","Could not enable TCP_NODELAY on HTTPS socket: errno %d",errno);
}
}
esp_err_t start(httpd_handle_t &handle,const std::string &hostname,const std::string &initial_auth) {
    auto e=credentials_init(initial_auth);if(e!=ESP_OK)return e;
    e=tls_identity(hostname,certificate,private_key);if(e!=ESP_OK)return e;
    e=stall::prepare();if(e!=ESP_OK)return e;
    httpd_ssl_config_t cfg=HTTPD_SSL_CONFIG_DEFAULT();
    cfg.httpd.max_uri_handlers=32;
    cfg.httpd.stack_size=12288;
    // A browser can pool six HTTP/1.1 connections. Also accommodate an
    // authenticated API client and one connection being replaced, so normal
    // UI traffic does not evict another request from the shared server.
    cfg.httpd.max_open_sockets=8;
    cfg.httpd.lru_purge_enable=true;
    cfg.httpd.recv_wait_timeout=15;
    cfg.httpd.send_wait_timeout=15;
    cfg.httpd.uri_match_fn=httpd_uri_match_wildcard;
    cfg.servercert=reinterpret_cast<const uint8_t *>(certificate.c_str());
    cfg.servercert_len=certificate.size()+1;
    cfg.prvtkey_pem=reinterpret_cast<const uint8_t *>(private_key.c_str());
    cfg.prvtkey_len=private_key.size()+1;
    e=httpd_ssl_start(&handle,&cfg);
    if(e==ESP_OK)stall::serve(handle);
    return e;
}
esp_err_t stop(httpd_handle_t handle) {
    const auto e=httpd_ssl_stop(handle);
    if(e==ESP_OK)stall::release(handle);
    return e;
}
esp_err_t route(httpd_handle_t handle,const char *path,httpd_method_t method,Handler handler,void *context) {
    httpd_uri_t uri{};uri.uri=path;uri.method=method;uri.handler=handler;uri.user_ctx=context;
    return httpd_register_uri_handler(handle,&uri);
}
}

extern "C" int __real_esp_tls_server_session_create(esp_tls_cfg_server_t*, int, esp_tls_t*);
extern "C" int __wrap_esp_tls_server_session_create(esp_tls_cfg_server_t *cfg, int socket, esp_tls_t *tls) {
    web::configure_tcp_socket(socket);
    web::stall::arm(socket,web::stall::handshake_us);
    const int result=__real_esp_tls_server_session_create(cfg,socket,tls);
    web::stall::disarm();
    return result;
}
