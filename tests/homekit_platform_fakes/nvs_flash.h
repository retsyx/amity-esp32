#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#define ESP_OK 0
#define ESP_FAIL -1
#define NVS_READONLY 0
#define NVS_READWRITE 1
#define ESP_PARTITION_TYPE_DATA 1
#define ESP_PARTITION_SUBTYPE_DATA_NVS_KEYS 4
#define CONFIG_HAP_PLATFORM_DEF_NVS_RUNTIME_PARTITION "nvs"
#define CONFIG_HAP_PLATFORM_DEF_NVS_FACTORY_PARTITION "nvs"
typedef int esp_err_t;
typedef int nvs_handle;
typedef struct { uint8_t key[64]; } nvs_sec_cfg_t;
typedef struct { int id; } esp_partition_t;
typedef const esp_partition_t *esp_partition_iterator_t;
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t nvs_flash_init_partition(const char *partition);
esp_err_t nvs_flash_secure_init_partition(const char *partition, nvs_sec_cfg_t *config);
esp_err_t nvs_flash_read_security_cfg(const esp_partition_t *partition, nvs_sec_cfg_t *config);
esp_partition_iterator_t esp_partition_find(int type, int subtype, const char *label);
const esp_partition_t *esp_partition_get(esp_partition_iterator_t iterator);
esp_err_t nvs_open_from_partition(const char *partition, const char *space, int mode, nvs_handle *handle);
esp_err_t nvs_get_blob(nvs_handle handle, const char *key, void *value, size_t *size);
esp_err_t nvs_set_blob(nvs_handle handle, const char *key, const void *value, size_t size);
esp_err_t nvs_commit(nvs_handle handle);
esp_err_t nvs_erase_key(nvs_handle handle, const char *key);
esp_err_t nvs_erase_all(nvs_handle handle);
void nvs_close(nvs_handle handle);
#ifdef __cplusplus
}
#endif
