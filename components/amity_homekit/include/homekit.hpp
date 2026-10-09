/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
#include "amity/hub_service.hpp"
#include "amity_matter.h"
namespace amity_homekit {
struct Status {
    bool started=false, available=false, pairing=false, busy=false, paired=false;
    unsigned controllers=0;
    esp_err_t error=ESP_OK;
    char manual_code[11]{}, qr_payload[32]{};
};
// Matter owns provisioning and Wi-Fi. Start the worker and restore saved HomeKit state.
esp_err_t start(amity::HubService &hub, const amity_matter_callbacks_t &callbacks);
Status status();
esp_err_t open_pairing();
esp_err_t close_pairing();
esp_err_t remove_pairings();
}
