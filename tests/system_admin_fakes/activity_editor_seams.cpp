/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

// Seams for what components/web_admin/activity_editor.cpp reads outside the
// system administration harness: the station's Wi-Fi association.
#include "esp_wifi.h"
#include "seams.hpp"

esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *ap_info) { return AMITY_SEAM(esp_wifi_sta_get_ap_info, ap_info); }
