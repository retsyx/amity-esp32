/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#include "web.hpp"
#include "freertos/FreeRTOS.h"
#include <algorithm>
#include <sys/socket.h>

namespace web {
BodyReader::BodyReader(httpd_req_t *request,uint64_t timeout_us):request_(request) {
    if(!request->content_len) {valid_=true;return;}
    socket_=httpd_req_to_sockfd(request);
    if(socket_<0)return;
    deadline_=esp_timer_get_time()+timeout_us;
    esp_timer_create_args_t args{};
    args.callback=expire;args.arg=this;args.name="http_body";
    if(esp_timer_create(&args,&timer_)!=ESP_OK)return;
    valid_=esp_timer_start_once(timer_,timeout_us)==ESP_OK;
}
BodyReader::~BodyReader() {
    if(!timer_)return;
    // The callback must finish before this object dies or HTTPD can reuse its socket fd.
    ESP_ERROR_CHECK(esp_timer_stop_blocking(timer_,portMAX_DELAY));
    ESP_ERROR_CHECK(esp_timer_delete(timer_));
}
void BodyReader::expire(void *context) {
    auto *reader=static_cast<BodyReader *>(context);
    reader->expired_.store(true);
    // A receive timeout alone can be renewed by each fragment of a TLS record.
    // shutdown interrupts that blocked TLS receive without releasing/reusing the fd.
    shutdown(reader->socket_,SHUT_RDWR);
}
int BodyReader::receive(char *buffer,size_t length) {
    if(!valid_||expired_.load()||esp_timer_get_time()>=deadline_)return HTTPD_SOCK_ERR_TIMEOUT;
    const int n=httpd_req_recv(request_,buffer,length);
    if(expired_.load()||esp_timer_get_time()>=deadline_)return HTTPD_SOCK_ERR_TIMEOUT;
    return n;
}
bool body(httpd_req_t *request,std::string &out,size_t limit) {
    if(request->content_len>limit) {error(request,"Request is too large","413 Content Too Large");return false;}
    BodyReader reader(request);
    if(!reader.valid()) {error(request,"Cannot receive request","503 Service Unavailable");return false;}
    out.resize(request->content_len);size_t offset=0;
    while(offset<out.size()) {
        const int n=reader.receive(out.data()+offset,out.size()-offset);
        if(n<=0) {error(request,"Incomplete request","408 Request Timeout");return false;}
        offset+=n;
    }
    return true;
}
esp_err_t response(httpd_req_t *req,const char *data,size_t size) {
    // ESP32-S3 AES copies PSRAM records through scarce internal DMA buffers.
    // Keep application writes small while retaining the SDK's full TLS
    // handshake buffers and incoming-record capacity. Large bodies use HTTP's
    // standard chunk framing; never terminate a response after a failed write.
    constexpr size_t chunk_size=512;
    if(size<=chunk_size)return httpd_resp_send(req,data,size);
    for(size_t offset=0;offset<size;) {
        const auto count=std::min(chunk_size,size-offset);
        const auto result=httpd_resp_send_chunk(req,data+offset,count);
        if(result!=ESP_OK)return result;
        offset+=count;
    }
    return httpd_resp_send_chunk(req,nullptr,0);
}
esp_err_t json(httpd_req_t *req,const std::string &data,const char *status) {
    httpd_resp_set_status(req,status);httpd_resp_set_type(req,"application/json");
    httpd_resp_set_hdr(req,"Cache-Control","no-store");
    httpd_resp_set_hdr(req,"X-Content-Type-Options","nosniff");
    return response(req,data.data(),data.size());
}
esp_err_t error(httpd_req_t *req,const char *message,const char *status) {
    // Callers supply static messages or ESP-IDF error names, never unescaped user data.
    httpd_resp_set_hdr(req,"Connection","close");
    json(req,std::string("{\"error\":\"")+message+"\"}",status);
    // Returning success would make HTTPD drain the entire advertised unread body.
    return ESP_FAIL;
}
}
