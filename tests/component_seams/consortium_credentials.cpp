/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

// Seams for the consortium platform primitives defined in
// components/consortium/credentials.cpp. Never link them with credentials.cpp.
// hex and unhex are pure and have no seams.
#include "platform.hpp"
#include "seams.hpp"

std::string amity::consortium::platform::sha256(std::string_view bytes) {
    return AMITY_SEAM(amity::consortium::platform::sha256, bytes);
}
std::string amity::consortium::platform::random_hex(size_t bytes) {
    return AMITY_SEAM(amity::consortium::platform::random_hex, bytes);
}
uint64_t amity::consortium::platform::now_ms() { return AMITY_SEAM(amity::consortium::platform::now_ms); }
esp_err_t amity::consortium::platform::read_record(const char *space, const char *key, std::string &out, size_t cap) {
    return AMITY_SEAM(amity::consortium::platform::read_record, space, key, out, cap);
}
esp_err_t amity::consortium::platform::write_record(const char *space, const char *key, std::string_view bytes) {
    return AMITY_SEAM(amity::consortium::platform::write_record, space, key, bytes);
}
esp_err_t amity::consortium::platform::load_identity(model::OctopusId id, Identity &out, bool create) {
    return AMITY_SEAM(amity::consortium::platform::load_identity, id, out, create);
}
bool amity::consortium::platform::certificate(std::string_view der, model::OctopusId id, std::string &pin) {
    return AMITY_SEAM(amity::consortium::platform::certificate, der, id, pin);
}
bool amity::consortium::platform::certificate_binding(const mbedtls_x509_crt &crt, model::OctopusId id, std::string &pin) {
    return AMITY_SEAM(amity::consortium::platform::certificate_binding, crt, id, pin);
}
esp_err_t amity::consortium::platform::load_group(Group &out) {
    return AMITY_SEAM(amity::consortium::platform::load_group, out);
}
esp_err_t amity::consortium::platform::save_group(const Group &group) {
    return AMITY_SEAM(amity::consortium::platform::save_group, group);
}
