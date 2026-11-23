/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

// The SDK calls of the system services files beyond the core fakes: OTA
// partitions and states, and the entropy sources. Each is a strict seam.
#include "bootloader_random.h"
#include "esp_ota_ops.h"
#include "esp_random.h"
#include "seams.hpp"

extern "C" {

const esp_partition_t *esp_ota_get_running_partition(void) { return AMITY_SEAM(esp_ota_get_running_partition); }
const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *start_from) {
    return AMITY_SEAM(esp_ota_get_next_update_partition, start_from);
}
esp_err_t esp_ota_get_state_partition(const esp_partition_t *partition, esp_ota_img_states_t *ota_state) {
    return AMITY_SEAM(esp_ota_get_state_partition, partition, ota_state);
}
esp_err_t esp_ota_mark_app_valid_cancel_rollback(void) { return AMITY_SEAM(esp_ota_mark_app_valid_cancel_rollback); }

void bootloader_random_enable(void) { AMITY_SEAM(bootloader_random_enable); }
void bootloader_random_disable(void) { AMITY_SEAM(bootloader_random_disable); }

uint32_t esp_random(void) { return AMITY_SEAM(esp_random); }
void esp_fill_random(void *buf, size_t len) { AMITY_SEAM(esp_fill_random, buf, len); }

}  // extern "C"
