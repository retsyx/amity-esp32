#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*ble_remote_key_callback_t)(uint16_t usage_page, uint16_t usage,
                                        bool pressed, void *context);
typedef void (*ble_remote_disconnect_callback_t)(void *context);
typedef struct {
    // Address bytes use the conventional printed order, most significant first.
    uint8_t address[6];
    uint8_t address_type;
    int8_t rssi;
    char name[48];
} ble_remote_peer_t;

typedef struct {
    bool ready;
    bool scanning;
    bool pairing;       // Automatic search/bonding operation is in progress.
    bool unpairing;     // Bond removal or link termination is still in progress.
    bool too_far;       // Weak HID advertisements seen during pairing search.
    bool connecting;
    bool connected;
    bool bonded;
    int charging;        // -1 unknown, 0 not charging, 1 charging.
    int battery_percent; // -1 means unknown.
    int last_error;      // ESP-IDF or NimBLE error, zero after a successful connection.
    ble_remote_peer_t peer;
} ble_remote_status_t;

// Start after the application has initialized NimBLE (Matter does this). This
// component never starts or stops the shared controller/host. Callbacks run on
// the NimBLE host task and must be short, enqueueing work rather than blocking.
esp_err_t ble_remote_start(ble_remote_key_callback_t callback, void *context);
// Register during application startup. Invoked BEFORE synthetic key releases
// on link loss/unpair, so consumers can cancel pending power/activity gestures.
void ble_remote_set_disconnect_callback(ble_remote_disconnect_callback_t callback, void *context);
esp_err_t ble_remote_scan(uint32_t seconds); // 1..60 seconds, HID devices only.
// Search for the first nearby HID device accepting a bond; skip rejected peers.
esp_err_t ble_remote_start_pairing(uint32_t seconds);
size_t ble_remote_scan_results(ble_remote_peer_t *peers, size_t capacity);
esp_err_t ble_remote_pair(const uint8_t address[6], uint8_t address_type);
// Unpair only the HID remote; never clears Matter fabrics or other BLE bonds.
esp_err_t ble_remote_unpair(void);
void ble_remote_get_status(ble_remote_status_t *status);

#ifdef __cplusplus
}
#endif
