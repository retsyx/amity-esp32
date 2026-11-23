/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
// The mDNS responder calls Amity makes, with the managed mdns component's
// types. The functions are strict seams.
#include "esp_err.h"
#include "esp_netif.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *key;
    const char *value;
} mdns_txt_item_t;

typedef struct mdns_ip_addr_s {
    esp_ip_addr_t addr;
    struct mdns_ip_addr_s *next;
} mdns_ip_addr_t;

esp_err_t mdns_init(void);
esp_err_t mdns_hostname_set(const char *hostname);
esp_err_t mdns_hostname_get(char *hostname);
esp_err_t mdns_delegate_hostname_add(const char *hostname, const mdns_ip_addr_t *address_list);
esp_err_t mdns_delegate_hostname_set_address(const char *hostname, const mdns_ip_addr_t *address_list);
esp_err_t mdns_delegate_hostname_remove(const char *hostname);
esp_err_t mdns_service_add_for_host(const char *instance_name, const char *service_type, const char *proto,
                                    const char *hostname, uint16_t port, mdns_txt_item_t txt[], size_t num_items);
esp_err_t mdns_service_remove_for_host(const char *instance, const char *service_type, const char *proto,
                                       const char *hostname);

#ifdef __cplusplus
}
#endif
