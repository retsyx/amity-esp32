/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

// Seams for components/web/request.cpp. A BodyReader is valid when its
// constructor seam returns true.
#include "web.hpp"
#include "seams.hpp"

web::BodyReader::BodyReader(httpd_req_t *request, uint64_t timeout_us) : request_(request) {
    valid_ = amity_seam::call<&amity_seam::constructor<bool, web::BodyReader, httpd_req_t *, uint64_t>>(
        "web::BodyReader::BodyReader", *this, request, timeout_us);
}
web::BodyReader::~BodyReader() { amity_seam::record("web::BodyReader::~BodyReader"); }
int web::BodyReader::receive(char *buffer, size_t length) {
    return AMITY_SEAM(web::BodyReader::receive, *this, buffer, length);
}
bool web::body(httpd_req_t *request, std::string &out, size_t limit) { return AMITY_SEAM(web::body, request, out, limit); }
esp_err_t web::response(httpd_req_t *request, const char *data, size_t size) {
    return AMITY_SEAM(web::response, request, data, size);
}
esp_err_t web::json(httpd_req_t *request, const std::string &body, const char *status) {
    return AMITY_SEAM(web::json, request, body, status);
}
esp_err_t web::error(httpd_req_t *request, const char *message, const char *status) {
    return AMITY_SEAM(web::error, request, message, status);
}
