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
