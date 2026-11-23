/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

// Strict seams for the network, event, Wi-Fi, mDNS and NVS calls of the SDK
// fakes in this directory, and the event bases they declare.
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "mdns.h"
#include "nvs_flash.h"
#include "seams.hpp"

extern "C" {

esp_event_base_t const IP_EVENT = "IP_EVENT";
esp_event_base_t const WIFI_EVENT = "WIFI_EVENT";

esp_err_t esp_event_handler_register(esp_event_base_t event_base, int32_t event_id, esp_event_handler_t event_handler,
                                     void *event_handler_arg) {
    return AMITY_SEAM(esp_event_handler_register, event_base, event_id, event_handler, event_handler_arg);
}

esp_netif_t *esp_netif_get_handle_from_ifkey(const char *if_key) {
    return AMITY_SEAM(esp_netif_get_handle_from_ifkey, if_key);
}
bool esp_netif_is_netif_up(esp_netif_t *esp_netif) { return AMITY_SEAM(esp_netif_is_netif_up, esp_netif); }
esp_err_t esp_netif_get_ip_info(esp_netif_t *esp_netif, esp_netif_ip_info_t *ip_info) {
    return AMITY_SEAM(esp_netif_get_ip_info, esp_netif, ip_info);
}
int esp_netif_get_all_preferred_ip6(esp_netif_t *esp_netif, esp_ip6_addr_t if_ip6[]) {
    return AMITY_SEAM(esp_netif_get_all_preferred_ip6, esp_netif, if_ip6);
}
esp_err_t esp_netif_set_hostname(esp_netif_t *esp_netif, const char *hostname) {
    return AMITY_SEAM(esp_netif_set_hostname, esp_netif, hostname);
}

esp_err_t esp_wifi_set_ps(wifi_ps_type_t type) { return AMITY_SEAM(esp_wifi_set_ps, type); }
esp_err_t esp_wifi_get_ps(wifi_ps_type_t *type) { return AMITY_SEAM(esp_wifi_get_ps, type); }

esp_err_t mdns_init(void) { return AMITY_SEAM(mdns_init); }
esp_err_t mdns_hostname_set(const char *hostname) { return AMITY_SEAM(mdns_hostname_set, hostname); }
esp_err_t mdns_hostname_get(char *hostname) { return AMITY_SEAM(mdns_hostname_get, hostname); }
esp_err_t mdns_delegate_hostname_add(const char *hostname, const mdns_ip_addr_t *address_list) {
    return AMITY_SEAM(mdns_delegate_hostname_add, hostname, address_list);
}
esp_err_t mdns_delegate_hostname_set_address(const char *hostname, const mdns_ip_addr_t *address_list) {
    return AMITY_SEAM(mdns_delegate_hostname_set_address, hostname, address_list);
}
esp_err_t mdns_delegate_hostname_remove(const char *hostname) {
    return AMITY_SEAM(mdns_delegate_hostname_remove, hostname);
}
esp_err_t mdns_service_add_for_host(const char *instance_name, const char *service_type, const char *proto,
                                    const char *hostname, uint16_t port, mdns_txt_item_t txt[], size_t num_items) {
    return AMITY_SEAM(mdns_service_add_for_host, instance_name, service_type, proto, hostname, port, txt, num_items);
}
esp_err_t mdns_service_remove_for_host(const char *instance, const char *service_type, const char *proto,
                                       const char *hostname) {
    return AMITY_SEAM(mdns_service_remove_for_host, instance, service_type, proto, hostname);
}

}  // extern "C"

esp_err_t nvs_flash_init() { return AMITY_SEAM(nvs_flash_init); }
