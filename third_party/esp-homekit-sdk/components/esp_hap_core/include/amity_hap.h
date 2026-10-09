/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
int hap_amity_init(const char *hostname);
void hap_amity_lock(void);
void hap_amity_unlock(void);
const char *hap_amity_hostname(void);
void hap_amity_set_pairing_deadline(int64_t deadline);
bool hap_amity_pairing_allowed(void);
uint32_t hap_amity_pairing_window(void);
int hap_amity_refresh_mdns(void);
int hap_amity_remove_pairings(void);
#ifdef __cplusplus
}
#endif
