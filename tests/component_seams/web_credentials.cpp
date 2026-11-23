/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

// Seams for components/web/credentials.cpp.
#include "web.hpp"
#include "seams.hpp"

esp_err_t web::credentials_init(const std::string &initial_auth) { return AMITY_SEAM(web::credentials_init, initial_auth); }
bool web::setup_required() { return AMITY_SEAM(web::setup_required); }
esp_err_t web::setup_password(const std::string &password) { return AMITY_SEAM(web::setup_password, password); }
bool web::authenticated(httpd_req_t *request) { return AMITY_SEAM(web::authenticated, request); }
bool web::login(const std::string &password, std::string &token) { return AMITY_SEAM(web::login, password, token); }
unsigned web::login_retry_after() { return AMITY_SEAM(web::login_retry_after); }
esp_err_t web::change_password(const std::string &password) { return AMITY_SEAM(web::change_password, password); }
void web::logout(httpd_req_t *request) { AMITY_SEAM(web::logout, request); }
