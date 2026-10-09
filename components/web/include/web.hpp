/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
#include "esp_http_server.h"
#include <functional>
#include <string>
namespace web {
using Handler = esp_err_t (*)(httpd_req_t *);
esp_err_t start(httpd_handle_t &handle,const std::string &hostname,const std::string &initial_auth);
esp_err_t route(httpd_handle_t handle,const char *path,httpd_method_t method,Handler handler,void *context=nullptr);
bool body(httpd_req_t *request,std::string &out,size_t limit=32768);
esp_err_t json(httpd_req_t *request,const std::string &body,const char *status="200 OK");
esp_err_t error(httpd_req_t *request,const char *message,const char *status="400 Bad Request");
esp_err_t credentials_init(const std::string &initial_auth);
bool setup_required();
esp_err_t setup_password(const std::string &password);
bool authenticated(httpd_req_t *request);
bool login(const std::string &password,std::string &token);
unsigned login_retry_after();
esp_err_t change_password(const std::string &password);
void logout(httpd_req_t *request);
}
