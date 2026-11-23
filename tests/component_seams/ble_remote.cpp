/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

// Seams for the ble_remote_* functions of components/ble_remote/ble_remote.cpp.
#include "ble_remote.h"
#include "seams.hpp"

esp_err_t ble_remote_start(ble_remote_key_callback_t callback, void *context) {
    return AMITY_SEAM(ble_remote_start, callback, context);
}
void ble_remote_set_disconnect_callback(ble_remote_disconnect_callback_t callback, void *context) {
    AMITY_SEAM(ble_remote_set_disconnect_callback, callback, context);
}
void ble_remote_set_siri_callback(ble_remote_siri_callback_t callback, void *context) {
    AMITY_SEAM(ble_remote_set_siri_callback, callback, context);
}
esp_err_t ble_remote_start_pairing(uint32_t seconds) { return AMITY_SEAM(ble_remote_start_pairing, seconds); }
esp_err_t ble_remote_unpair(void) { return AMITY_SEAM(ble_remote_unpair); }
void ble_remote_get_status(ble_remote_status_t *status) { AMITY_SEAM(ble_remote_get_status, status); }
void ble_remote_set_radio_hooks(ble_remote_radio_admit_t admit, ble_remote_radio_done_t done, void *context) {
    AMITY_SEAM(ble_remote_set_radio_hooks, admit, done, context);
}
esp_err_t ble_remote_pause_background(bool paused) { return AMITY_SEAM(ble_remote_pause_background, paused); }
esp_err_t ble_remote_cancel_setup(void) { return AMITY_SEAM(ble_remote_cancel_setup); }
esp_err_t ble_remote_stop_pairing(void) { return AMITY_SEAM(ble_remote_stop_pairing); }
