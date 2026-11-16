/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#include "stall.hpp"
#include "esp_httpd_priv.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include <atomic>
#include <sys/socket.h>
namespace web::stall {
namespace {
std::atomic<httpd_data *> secure{nullptr};
esp_timer_handle_t timer=nullptr;
std::atomic<int> watched{-1};
void expire(void *) {shutdown(watched.load(),SHUT_RDWR);}
}
esp_err_t prepare() {
    if(timer)return ESP_OK;
    esp_timer_create_args_t args{};
    args.callback=expire;args.name="https_stall";
    return esp_timer_create(&args,&timer);
}
void serve(httpd_handle_t server) {secure.store(static_cast<httpd_data *>(server));}
void release(httpd_handle_t server) {
    auto *expected=static_cast<httpd_data *>(server);
    secure.compare_exchange_strong(expected,nullptr);
}
void arm(int socket,uint64_t timeout_us) {
    if(!timer)return;
    watched.store(socket);
    esp_timer_start_once(timer,timeout_us);
}
void disarm() {
    if(!timer)return;
    // Waits for a running expiry, so it never reaches a socket HTTPD reuses.
    esp_timer_stop_blocking(timer,portMAX_DELAY);
    watched.store(-1);
}
}

// The SDK calls each of these across source files, so the linker routes
// those calls here. Other HTTPD servers pass through unchanged.
extern "C" {
esp_err_t __real_httpd_req_new(httpd_data *server,sock_db *session);
esp_err_t __real_httpd_uri(httpd_data *server);
esp_err_t __real_httpd_req_delete(httpd_data *server);
// From the session's first received byte until its head is routed.
esp_err_t __wrap_httpd_req_new(httpd_data *server,sock_db *session) {
    if(server!=web::stall::secure.load())return __real_httpd_req_new(server,session);
    web::stall::arm(session->fd,web::stall::head_us);
    const auto result=__real_httpd_req_new(server,session);
    web::stall::disarm();
    return result;
}
// Routing ends the head. Handlers bound their own body reads.
esp_err_t __wrap_httpd_uri(httpd_data *server) {
    if(server==web::stall::secure.load())web::stall::disarm();
    return __real_httpd_uri(server);
}
// A body the handler left unread is drained before the next request.
esp_err_t __wrap_httpd_req_delete(httpd_data *server) {
    if(server!=web::stall::secure.load()||!server->hd_req_aux.remaining_len)return __real_httpd_req_delete(server);
    web::stall::arm(httpd_req_to_sockfd(&server->hd_req),web::stall::purge_us);
    const auto result=__real_httpd_req_delete(server);
    web::stall::disarm();
    return result;
}
}
