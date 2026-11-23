/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

// Strict seams for what the system administration harness calls outside the
// firmware files it builds and outside tests/component_seams: the OTA, flash
// partition, PSA and random-number SDK calls, the NVS settings store of
// config_store.cpp and the response deadline of components/web/stall.cpp.
#include "config_store.hpp"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_random.h"
#include "psa/crypto.h"
#include "stall.hpp"
#include "system_configuration.hpp"
#include "seams.hpp"

const esp_partition_t *esp_ota_get_running_partition(void) { return AMITY_SEAM(esp_ota_get_running_partition); }
esp_err_t esp_ota_get_state_partition(const esp_partition_t *partition, esp_ota_img_states_t *ota_state) {
    return AMITY_SEAM(esp_ota_get_state_partition, partition, ota_state);
}

const esp_partition_t *esp_partition_find_first(int type, int subtype, const char *label) {
    return AMITY_SEAM(esp_partition_find_first, type, subtype, label);
}
esp_err_t esp_partition_read(const esp_partition_t *partition, size_t offset, void *buffer, size_t size) {
    return AMITY_SEAM(esp_partition_read, partition, offset, buffer, size);
}
psa_status_t psa_crypto_init() { return AMITY_SEAM(psa_crypto_init); }
psa_status_t psa_hash_compute(unsigned algorithm, const uint8_t *input, size_t input_length, uint8_t *hash,
                              size_t hash_size, size_t *hash_length) {
    return AMITY_SEAM(psa_hash_compute, algorithm, input, input_length, hash, hash_size, hash_length);
}

void esp_fill_random(void *buffer, size_t size) { AMITY_SEAM(esp_fill_random, buffer, size); }

esp_err_t config_store::read(const char *key, std::string &value) { return AMITY_SEAM(config_store::read, key, value); }
esp_err_t config_store::write(const char *key, const std::string &value) {
    return AMITY_SEAM(config_store::write, key, value);
}

// The harness builds system_configuration.cpp with serialize_configuration
// renamed, so every serialization from another file is recorded here.
namespace config_store {
std::string actual_serialize_configuration(const Configuration &, bool);
}
std::string config_store::serialize_configuration(const Configuration &value, bool executable_policy_only) {
    amity_seam::record("config_store::serialize_configuration");
    return actual_serialize_configuration(value, executable_policy_only);
}

// A response's deadline is armed when its constructor seam returns true. The
// constructor runs just before the response is sent, after its bytes are fixed.
web::stall::Response::Response(httpd_req_t *request, size_t size) {
    armed_ = amity_seam::call<&amity_seam::constructor<bool, web::stall::Response, httpd_req_t *, size_t>>(
        "web::stall::Response::Response", *this, request, size);
}
web::stall::Response::~Response() { amity_seam::record("web::stall::Response::~Response"); }
