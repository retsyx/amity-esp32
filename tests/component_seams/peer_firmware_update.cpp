/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

// Seams for components/system_services/peer_firmware_update.cpp.
#include "firmware_update.hpp"
#include "seams.hpp"

esp_err_t system_services::updates::peer_begin(const ReleaseManifest &validated, std::string_view signature_hex,
                                               std::string &error) {
    return AMITY_SEAM(system_services::updates::peer_begin, validated, signature_hex, error);
}
esp_err_t system_services::updates::peer_chunk(std::string_view release_id, uint64_t offset, std::string_view bytes,
                                               std::string &error) {
    return AMITY_SEAM(system_services::updates::peer_chunk, release_id, offset, bytes, error);
}
esp_err_t system_services::updates::peer_stage(std::string_view release_id, std::string &error) {
    return AMITY_SEAM(system_services::updates::peer_stage, release_id, error);
}
esp_err_t system_services::updates::peer_install(std::string_view release_id, std::string &error,
                                                 uint32_t reboot_delay_ms) {
    return AMITY_SEAM(system_services::updates::peer_install, release_id, error, reboot_delay_ms);
}
esp_err_t system_services::updates::partition_release(bool running, ReleaseManifest &release, std::string &signature_hex,
                                                      std::string &error) {
    return AMITY_SEAM(system_services::updates::partition_release, running, release, signature_hex, error);
}
esp_err_t system_services::updates::read_running_image(uint64_t offset, size_t count, std::string &bytes,
                                                       std::string &error) {
    return AMITY_SEAM(system_services::updates::read_running_image, offset, count, bytes, error);
}
esp_err_t system_services::updates::read_staged_image(uint64_t offset, size_t count, std::string &bytes,
                                                      std::string &error) {
    return AMITY_SEAM(system_services::updates::read_staged_image, offset, count, bytes, error);
}
void system_services::updates::peer_abort(const std::string &error) {
    AMITY_SEAM(system_services::updates::peer_abort, error);
}
void system_services::updates::poll_peer_timeout() { AMITY_SEAM(system_services::updates::poll_peer_timeout); }
