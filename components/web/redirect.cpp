/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#include "web.hpp"
#include "esp_heap_caps.h"
#include <iterator>
#include <strings.h>
namespace web {
namespace {
constexpr httpd_method_t redirect_methods[]={HTTP_GET,HTTP_HEAD,HTTP_POST,HTTP_PUT,HTTP_DELETE,HTTP_PATCH,HTTP_OPTIONS};
esp_err_t redirect(httpd_req_t *request) {
    std::string host;
    if(const size_t length=httpd_req_get_hdr_value_len(request,"Host")) {
        host.resize(length+1);
        if(httpd_req_get_hdr_value_str(request,"Host",host.data(),host.size())==ESP_OK)host.resize(length);
        else host.clear();
    }
    const auto location=https_location(host,request->uri);
    if(location.empty())httpd_resp_set_status(request,"400 Bad Request");
    else {
        httpd_resp_set_status(request,"307 Temporary Redirect");
        httpd_resp_set_hdr(request,"Location",location.c_str());
    }
    httpd_resp_set_hdr(request,"Connection","close");
    const auto result=response(request,nullptr,0);
    // Queued behind HTTPD's purge of any unread request body.
    httpd_sess_trigger_close(request->handle,httpd_req_to_sockfd(request));
    return result;
}
}
std::string https_location(const std::string &host,const std::string &target) {
    // An absolute-form target is the requested URL itself.
    if(!strncasecmp(target.c_str(),"http://",7))return "https://"+target.substr(7);
    if(host.empty()||target.empty()||target.front()!='/')return {};
    return "https://"+host+target;
}
esp_err_t start_redirect(httpd_handle_t &handle) {
    httpd_config_t cfg=HTTPD_DEFAULT_CONFIG();
    // Port 80. Every response closes its connection. A second session admits
    // a browser's parallel connection without evicting the pending request.
    cfg.max_open_sockets=2;
    cfg.max_uri_handlers=std::size(redirect_methods);
    cfg.lru_purge_enable=true;
    cfg.uri_match_fn=httpd_uri_match_wildcard;
    cfg.task_caps=MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT;
    auto e=httpd_start(&handle,&cfg);if(e!=ESP_OK)return e;
    for(const auto method:redirect_methods)if((e=route(handle,"/*",method,redirect))!=ESP_OK)break;
    if(e!=ESP_OK){httpd_stop(handle);handle=nullptr;}
    return e;
}
}
