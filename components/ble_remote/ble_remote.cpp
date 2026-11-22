/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#include "ble_remote.h"
#include "hid_keyboard.hpp"
#include "battery_status.hpp"
#include "gatt_layout.hpp"
#include "pairing_policy.hpp"
#include "siri_remote.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <new>
#include <vector>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
extern "C" {
#include "host/ble_gap.h"
#include "host/ble_att.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/ble_sm.h"
#include "host/ble_store.h"
#if MYNEWT_VAL(BLE_HS_PVCY)
// The pinned SDK's public ble_gap_unpair() masks controller and store errors.
// Use its checked privacy primitive so an unsuccessful Unpair remains retryable.
int ble_hs_pvcy_remove_entry(uint8_t addr_type, const uint8_t *addr);
#endif
}

#if !CONFIG_BT_NIMBLE_ENABLED || !CONFIG_BT_NIMBLE_ROLE_CENTRAL
#error "BLE remote requires the ESP-IDF NimBLE central options"
#endif

namespace {
constexpr char kTag[] = "ble_remote";
constexpr size_t kMaxPeers = 16;
enum class CommandType { AutoPair, Unpair, Wake };
struct Command {
    CommandType type;
    uint32_t seconds;
};
struct Candidate {
    ble_addr_t address;
    int8_t rssi;
    bool hid, connectable, attempted;
    ble::SiriGeneration siri = ble::SiriGeneration::None;
    bool supported() const { return siri != ble::SiriGeneration::Unsupported &&
        (hid || siri == ble::SiriGeneration::Gen2 || siri == ble::SiriGeneration::Gen3); }
};
SemaphoreHandle_t mutex;
QueueHandle_t commands;
ble_remote_status_t state{};
std::vector<Candidate> candidates;
ble::HidKeyboard *keyboard;
ble_remote_key_callback_t key_callback;
void *key_context;
ble_remote_disconnect_callback_t disconnect_callback;
void *disconnect_context;
ble_remote_siri_callback_t siri_callback;
void *siri_context;
bool siri_connected;
ble::SiriProfile siri_profile{};
struct InputReport { uint16_t handle; uint8_t map, id; };
std::vector<InputReport> input_reports;
struct BatteryCharacteristic { uint16_t handle, uuid; bool poll; };
std::vector<BatteryCharacteristic> battery_characteristics;
int64_t next_battery_read;
void update_battery(uint16_t uuid, const uint8_t *data, size_t size) {
    if (siri_connected && uuid == ble::battery_power_state_uuid) {
        const int charging = ble::siri_charging_state(siri_profile, data, size);
        if (charging >= 0) state.charging = charging;
        return;
    }
    ble::BatteryStatus value{state.battery_percent, state.charging};
    if (ble::parse_battery(uuid, data, size, value)) {
        state.battery_percent = value.percent;
        state.charging = value.charging;
    }
}
SemaphoreHandle_t procedure_done;
uintptr_t generation;
int procedure_status;
bool procedure_pending;
enum class Procedure { Gatt, Connect, Security };
Procedure procedure_kind;
std::vector<ble_gatt_svc> services;
std::vector<ble_gatt_chr> characteristics;
std::vector<ble_gatt_dsc> descriptors;
std::vector<uint8_t> read_value;
ble_addr_t selected{};
ble_addr_t saved{};
bool have_selected, have_saved, descriptors_ready, encrypted, forgetting;
bool unpair_cleanup_pending;
bool unpair_cleanup_started, unpair_delete_bond;
ble_addr_t unpair_target{};
// A failed pairing or Unpair could not remove the remote's bond or saved peer.
// The next Pair retries it first; `selected` names the identity when set.
bool bond_cleanup_failed;
// Filled by full discovery and saved once the remote is ready.
ble::GattLayout discovered_layout;
// Link timing and signal strength for the link up/down log lines and status.
int64_t connect_started_us, link_up_us, next_rssi_sample, rssi_sampled_us;
int8_t last_rssi;
bool have_rssi;

std::atomic<uint16_t> connection_handle{BLE_HS_CONN_HANDLE_NONE};
bool reconnect_pending;
bool reconnect_cancel_pending;
int64_t next_cancel_attempt;
int64_t next_reconnect;
// Failed remote connections since the last ready link; sets the retry backoff.
unsigned reconnect_failures;
int64_t pairing_deadline;
uint32_t pending_scan_seconds;
bool pending_scan_clear_error;
std::atomic_bool radio_paused{false};
std::atomic_bool setup_cancelled{false};
// A user stopped the search; the abandoned setup must not report a failure.
std::atomic_bool pairing_stopped{false};
ble_remote_radio_admit_t radio_admit=nullptr;
ble_remote_radio_done_t radio_done=nullptr;
void *radio_context=nullptr;
std::atomic_bool radio_owned{false};

class RadioAdmission {
public:
    explicit RadioAdmission(uint32_t seconds) : accepted(!radio_admit||radio_admit(seconds,radio_context)) {}
    ~RadioAdmission(){if(accepted&&!committed&&radio_done)radio_done(radio_context);}
    void commit(){committed=true;radio_owned.store(true);}
    bool accepted;
private: bool committed=false;
};


class Guard {
public:
    Guard() { xSemaphoreTakeRecursive(mutex, portMAX_DELAY); }
    ~Guard() { xSemaphoreGiveRecursive(mutex); }
};

void to_public(ble_remote_peer_t &peer, const ble_addr_t &address) {
    for (size_t i = 0; i < 6; ++i) peer.address[i] = address.val[5 - i];
    peer.address_type = address.type;
}
bool same_address(const ble_addr_t &a, const ble_addr_t &b) {
    // Identity and ordinary addresses share the same public/random low bit.
    return (a.type & 1) == (b.type & 1) && !memcmp(a.val, b.val, 6);
}

constexpr char kLayoutKey[] = "layout";

esp_err_t erase_key(nvs_handle_t handle, const char *key) {
    const esp_err_t err = nvs_erase_key(handle, key);
    return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
}

esp_err_t persist_peer(const ble_addr_t *address) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open("ble_remote", NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    if (address) err = nvs_set_blob(handle, "peer", address, sizeof(*address));
    else {
        err = erase_key(handle, "peer");
        // The saved layout belongs to the forgotten remote.
        if (err == ESP_OK) err = erase_key(handle, kLayoutKey);
    }
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

esp_err_t erase_layout() {
    nvs_handle_t handle;
    esp_err_t err = nvs_open("ble_remote", NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = erase_key(handle, kLayoutKey);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

// A layout without validators cannot be checked later, so that remote is
// always discovered in full.
esp_err_t store_layout(const ble::GattLayout &layout) {
    const auto data = ble::encode_gatt_layout(layout);
    if (data.empty()) return erase_layout();
    nvs_handle_t handle;
    esp_err_t err = nvs_open("ble_remote", NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(handle, kLayoutKey, data.data(), data.size());
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

bool load_layout(ble::GattLayout &layout) {
    nvs_handle_t handle;
    if (nvs_open("ble_remote", NVS_READONLY, &handle) != ESP_OK) return false;
    size_t size = 0;
    std::vector<uint8_t> data;
    bool loaded = nvs_get_blob(handle, kLayoutKey, nullptr, &size) == ESP_OK && size &&
                  size <= ble::max_gatt_layout_size;
    if (loaded) {
        data.resize(size);
        loaded = nvs_get_blob(handle, kLayoutKey, data.data(), &size) == ESP_OK &&
                 ble::decode_gatt_layout(data.data(), size, layout);
    }
    nvs_close(handle);
    return loaded;
}

int remove_peer(ble_addr_t address) {
    // A failed connection can still have its scan report's identity type.
    // Security records and controller commands use public/random (0/1).
    address.type &= 1;
#if MYNEWT_VAL(BLE_HS_PVCY)
    // Revocation must precede deleting the security record which owns the IRK.
    const int privacy = ble_hs_pvcy_remove_entry(address.type, address.val);
    if (privacy && privacy != BLE_HS_HCI_ERR(BLE_ERR_UNK_CONN_ID)) return privacy;
#endif
    const int stored = ble_store_util_delete_peer(&address);
    return stored == BLE_HS_ENOENT ? 0 : stored;
}

void on_key(uint16_t page, uint16_t usage, bool pressed, void *) {
    if (key_callback) key_callback(page, usage, pressed, key_context);
}

// Each procedure has a generation token. Delayed callbacks after timeout or
// disconnection cannot mutate the next operation's result.
void finish_procedure(int status) {
    procedure_status = status;
    procedure_pending = false;
    xSemaphoreGive(procedure_done);
}
void *begin_procedure(Procedure kind = Procedure::Gatt) {
    Guard lock;
    while (xSemaphoreTake(procedure_done, 0) == pdTRUE) {}
    ++generation;
    procedure_pending = true;
    procedure_kind = kind;
    procedure_status = 0;
    return reinterpret_cast<void *>(generation);
}
bool current_procedure(uint16_t handle, void *token) {
    return handle == connection_handle && reinterpret_cast<uintptr_t>(token) == generation && procedure_pending;
}
int wait_procedure(int start_status, uint32_t timeout_ms = 10000) {
    if (start_status) {
        Guard lock;
        procedure_pending = false;
        return start_status;
    }
    if (xSemaphoreTake(procedure_done, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        Guard lock;
        ++generation;
        procedure_pending = false;
        return BLE_HS_ETIMEOUT;
    }
    Guard lock;
    return procedure_status;
}
int on_service(uint16_t handle, const ble_gatt_error *error, const ble_gatt_svc *service, void *token) {
    Guard lock;
    if (!current_procedure(handle, token)) return BLE_HS_EDONE;
    if (!error->status && service) {
        if (services.size() == 24) { finish_procedure(BLE_HS_ENOMEM); return BLE_HS_ENOMEM; }
        services.push_back(*service);
    } else finish_procedure(error->status == BLE_HS_EDONE ? 0 : error->status);
    return 0;
}
int on_characteristic(uint16_t handle, const ble_gatt_error *error, const ble_gatt_chr *chr, void *token) {
    Guard lock;
    if (!current_procedure(handle, token)) return BLE_HS_EDONE;
    if (!error->status && chr) {
        if (characteristics.size() == 48) { finish_procedure(BLE_HS_ENOMEM); return BLE_HS_ENOMEM; }
        characteristics.push_back(*chr);
    } else finish_procedure(error->status == BLE_HS_EDONE ? 0 : error->status);
    return 0;
}
int on_descriptor(uint16_t handle, const ble_gatt_error *error, uint16_t, const ble_gatt_dsc *dsc, void *token) {
    Guard lock;
    if (!current_procedure(handle, token)) return BLE_HS_EDONE;
    if (!error->status && dsc) {
        if (descriptors.size() == 16) { finish_procedure(BLE_HS_ENOMEM); return BLE_HS_ENOMEM; }
        descriptors.push_back(*dsc);
    } else finish_procedure(error->status == BLE_HS_EDONE ? 0 : error->status);
    return 0;
}
int on_read(uint16_t handle, const ble_gatt_error *error, ble_gatt_attr *attr, void *token) {
    Guard lock;
    if (!current_procedure(handle, token)) return BLE_HS_EDONE;
    if (!error->status && attr) {
        const size_t length = OS_MBUF_PKTLEN(attr->om);
        if (read_value.size() + length > 4096) { finish_procedure(BLE_HS_ENOMEM); return BLE_HS_ENOMEM; }
        const size_t offset = read_value.size();
        read_value.resize(offset + length);
        if (os_mbuf_copydata(attr->om, 0, length, read_value.data() + offset) != 0) {
            finish_procedure(BLE_HS_EBADDATA); return BLE_HS_EBADDATA;
        }
    } else finish_procedure(error->status == BLE_HS_EDONE ? 0 : error->status);
    return 0;
}
int on_write(uint16_t handle, const ble_gatt_error *error, ble_gatt_attr *, void *token) {
    Guard lock;
    if (current_procedure(handle, token)) finish_procedure(error->status);
    return 0;
}
int on_mtu(uint16_t handle, const ble_gatt_error *error, uint16_t, void *token) {
    Guard lock;
    if (current_procedure(handle, token)) finish_procedure(error->status);
    return 0;
}
int read_attribute(uint16_t attribute) {
    void *token = begin_procedure();
    { Guard lock; read_value.clear(); }
    return wait_procedure(ble_gattc_read_long(connection_handle, attribute, 0, on_read, token));
}
int write_attribute(uint16_t attribute, const void *value, size_t length) {
    void *token = begin_procedure();
    return wait_procedure(ble_gattc_write_flat(connection_handle, attribute, value, length, on_write, token));
}
bool att_error(int err) { return err > BLE_HS_ERR_ATT_BASE && err < BLE_HS_ERR_ATT_BASE + 0x100; }

void sample_rssi() {
    const uint16_t handle = connection_handle.load();
    int8_t rssi;
    // 127 means the controller has no measurement.
    if (handle == BLE_HS_CONN_HANDLE_NONE || ble_gap_conn_rssi(handle, &rssi) || rssi == 127) return;
    Guard lock;
    if (connection_handle != handle) return;
    last_rssi = rssi;
    rssi_sampled_us = esp_timer_get_time();
    have_rssi = true;
}

void log_link_up() {
    sample_rssi();
    Guard lock;
    if (!link_up_us) return; // Already down and logged.
    char rssi[16] = "unknown";
    if (have_rssi) snprintf(rssi, sizeof(rssi), "%d dBm", last_rssi);
    ESP_LOGI(kTag, "Remote link up after %lld ms, RSSI %s",
             static_cast<long long>((link_up_us - connect_started_us) / 1000), rssi);
}

// Caller holds the remote mutex. The first retry after a failure starts at
// once; later ones wait 0.1 s, doubling to at most 5 s.
int64_t reconnect_backoff_us() {
    if (reconnect_failures <= 1) return 0;
    return std::min<int64_t>(100000LL << std::min(reconnect_failures - 2, 6u), 5000000);
}

// Caller holds the remote mutex. Everything but the backoff time that allows
// listening for the saved remote again.
bool reconnect_allowed() {
    return state.ready && have_saved && connection_handle == BLE_HS_CONN_HANDLE_NONE &&
           !reconnect_pending && !ble_gap_conn_active() && !state.connecting && !state.scanning &&
           !state.pairing && !forgetting;
}

// Caller holds the remote mutex.
void count_reconnect_failure() {
    if (reconnect_failures < 16) ++reconnect_failures;
}

// Caller holds the remote mutex.
void log_link_down(int reason) {
    const int64_t now = esp_timer_get_time();
    const bool hci = reason > BLE_HS_ERR_HCI_BASE && reason < BLE_HS_ERR_HCI_BASE + 0x100;
    const char *name = "host error";
    if (hci) switch (reason - BLE_HS_ERR_HCI_BASE) {
    case BLE_ERR_CONN_SPVN_TMO: name = "supervision timeout"; break;
    case BLE_ERR_REM_USER_CONN_TERM: name = "closed by remote"; break;
    case BLE_ERR_RD_CONN_TERM_RESRCS: name = "remote out of resources"; break;
    case BLE_ERR_RD_CONN_TERM_PWROFF: name = "remote powered off"; break;
    case BLE_ERR_CONN_TERM_LOCAL: name = "closed locally"; break;
    case BLE_ERR_LMP_LL_RSP_TMO: name = "link layer response timeout"; break;
    case BLE_ERR_CONN_TERM_MIC: name = "message integrity failure"; break;
    case BLE_ERR_CONN_ESTABLISHMENT: name = "failed to establish"; break;
    case BLE_ERR_AUTH_FAIL: name = "authentication failure"; break;
    default: name = "controller error"; break;
    }
    char rssi[40] = "unknown";
    if (have_rssi) snprintf(rssi, sizeof(rssi), "%d dBm %lld ms earlier", last_rssi,
                            static_cast<long long>((now - rssi_sampled_us) / 1000));
    const int code = hci ? reason - BLE_HS_ERR_HCI_BASE : reason;
    char up[32] = "never reported up";
    if (link_up_us) snprintf(up, sizeof(up), "up %lld ms", static_cast<long long>((now - link_up_us) / 1000));
    // Intentional closure is routine; anything else is a dropped link.
    if (reason == BLE_HS_HCI_ERR(BLE_ERR_REM_USER_CONN_TERM) || reason == BLE_HS_HCI_ERR(BLE_ERR_CONN_TERM_LOCAL))
        ESP_LOGI(kTag, "Remote link down: %s (%s 0x%02x), %s, RSSI %s", name, hci ? "HCI" : "status", code, up, rssi);
    else
        ESP_LOGW(kTag, "Remote link down: %s (%s 0x%02x), %s, RSSI %s", name, hci ? "HCI" : "status", code, up, rssi);
    link_up_us = 0;
    have_rssi = false;
}

int on_gap_event(ble_gap_event *event, void *token) {
    Guard lock;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (reinterpret_cast<uintptr_t>(token) != generation || !procedure_pending ||
            procedure_kind != Procedure::Connect) {
            if (!event->connect.status) ble_gap_terminate(event->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            break;
        }
        if (forgetting || (setup_cancelled.load()&&!have_saved)) {
            if (!event->connect.status) ble_gap_terminate(event->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            finish_procedure(BLE_HS_EAPP);
            break;
        }
        if (!event->connect.status) {
            connection_handle = event->connect.conn_handle;
            link_up_us = esp_timer_get_time();
            have_rssi = false;
            next_rssi_sample = 0;
        }
        if (reconnect_pending) {
            state.waiting = false;
            state.connecting = !event->connect.status;
        }
        finish_procedure(event->connect.status);
        if (reconnect_pending) {
            // Start security/discovery promptly when the remote wakes, without
            // making the host callback perform any blocking GATT operations.
            const Command wake{CommandType::Wake, 0};
            xQueueSend(commands, &wake, 0);
        }
        break;
    case BLE_GAP_EVENT_ENC_CHANGE: {
        if (event->enc_change.conn_handle != connection_handle) break;
        ble_gap_conn_desc desc{};
        const int err = ble_gap_conn_find(connection_handle, &desc);
        encrypted = !err && desc.sec_state.encrypted;
        if (encrypted && !have_saved) selected = desc.peer_id_addr;
        if (!encrypted && descriptors_ready)
            ble_gap_terminate(connection_handle, BLE_ERR_AUTH_FAIL);
        if (procedure_pending && procedure_kind == Procedure::Security)
            finish_procedure(event->enc_change.status ? event->enc_change.status
                                                                      : encrypted ? 0 : BLE_HS_EAUTHEN);
        break;
    }
    case BLE_GAP_EVENT_DISCONNECT:
        if (connection_handle == BLE_HS_CONN_HANDLE_NONE && procedure_pending &&
            procedure_kind == Procedure::Connect && reinterpret_cast<uintptr_t>(token) == generation) {
            // NimBLE reports a central link only after reading the remote's
            // features and version. A link lost before then ends the attempt.
            log_link_down(event->disconnect.reason);
            if (reconnect_pending) state.waiting = false;
            finish_procedure(event->disconnect.reason);
            if (reconnect_pending) {
                const Command wake{CommandType::Wake, 0};
                xQueueSend(commands, &wake, 0);
            }
            break;
        }
        if (event->disconnect.conn.conn_handle != connection_handle) break;
        log_link_down(event->disconnect.reason);
        if (!have_saved) selected = event->disconnect.conn.peer_id_addr;
        // Preserve a failed pairing's Security Manager error when its cleanup
        // disconnect arrives; it is more useful than "locally terminated".
        if (!forgetting && (state.connected || !state.last_error)) {
            const int reason = event->disconnect.reason;
            // Sleeping, going out of range, and intentional link closure are
            // normal for a bonded remote; resume listening without a failure.
            state.last_error = reason == BLE_HS_HCI_ERR(BLE_ERR_CONN_SPVN_TMO) ||
                               reason == BLE_HS_HCI_ERR(BLE_ERR_REM_USER_CONN_TERM) ||
                               reason == BLE_HS_HCI_ERR(BLE_ERR_CONN_TERM_LOCAL) ? 0 : reason;
        }
        if (procedure_pending) finish_procedure(BLE_HS_ENOTCONN);
        connection_handle = BLE_HS_CONN_HANDLE_NONE;
        encrypted = descriptors_ready = false;
        if (disconnect_callback) disconnect_callback(disconnect_context);
        keyboard->clear();
        siri_connected = false;
        input_reports.clear();
        battery_characteristics.clear();
        // A ready link that failed counts here; a failed setup was already
        // counted when the worker finished it.
        if (state.last_error && state.connected) count_reconnect_failure();
        state.connected = state.connecting = state.waiting = false;
        state.battery_percent = -1;
        state.charging = -1;
        if (!unpair_cleanup_pending && !ble_gap_conn_active()) state.unpairing = false;
        next_reconnect = esp_timer_get_time() + (state.last_error ? reconnect_backoff_us() : 0);
        if (have_saved) {
            // Listen for the remote again without waiting for the worker's next turn.
            const Command wake{CommandType::Wake, 0};
            xQueueSend(commands, &wake, 0);
        }
        break;
    case BLE_GAP_EVENT_NOTIFY_RX: {
        if (event->notify_rx.conn_handle != connection_handle || !encrypted || forgetting) break;
        const size_t length = OS_MBUF_PKTLEN(event->notify_rx.om);
        for (const auto &characteristic : battery_characteristics) {
            if (event->notify_rx.attr_handle != characteristic.handle || length > 7) continue;
            uint8_t value[7];
            if (!os_mbuf_copydata(event->notify_rx.om, 0, length, value))
                update_battery(characteristic.uuid, value, length);
        }
        if (!descriptors_ready || length > 512) break;
        if (siri_connected) {
            const uint16_t attribute = event->notify_rx.attr_handle;
            const bool touch = attribute == siri_profile.touch_handle;
            if (siri_callback && (touch || attribute == siri_profile.input_handle) && length <= 18) {
                uint8_t data[18];
                if (!os_mbuf_copydata(event->notify_rx.om, 0, length, data))
                    siri_callback(touch ? BLE_REMOTE_SIRI_TOUCH : BLE_REMOTE_SIRI_BUTTONS,
                                  data, length, siri_context);
            }
            break; // Never pass Apple's reports to the keyboard/Atomic adapter.
        }
        for (const auto &report : input_reports) if (report.handle == event->notify_rx.attr_handle) {
            uint8_t data[512];
            if (!os_mbuf_copydata(event->notify_rx.om, 0, length, data))
                keyboard->input(report.map, report.id, data, length);
            break;
        }
        break;
    }
    case BLE_GAP_EVENT_REPEAT_PAIRING:
        // Re-pairing requires explicitly forgetting the old peer in the UI.
        // Never silently discard a known remote's identity.
        return BLE_GAP_REPEAT_PAIRING_IGNORE;
    case BLE_GAP_EVENT_PASSKEY_ACTION:
        // Atomic and Siri use Just Works. Reject devices requiring a passkey rather
        // than injecting IDF's example fixed passkey or accepting comparison.
        ble_gap_terminate(event->passkey.conn_handle, BLE_ERR_AUTH_FAIL);
        if (procedure_pending) finish_procedure(BLE_HS_EAUTHEN);
        break;
    default: break;
    }
    return 0;
}

int discover_hid(const std::vector<ble_gatt_svc> &found_services, bool siri) {
    void *token;
    int err;
    uint8_t map_index = 0;
    for (const auto &service : found_services) {
        const uint16_t uuid = ble_uuid_u16(&service.uuid.u);
        if (uuid != 0x1812 && uuid != 0x180f && uuid != 0x1800) continue;
        if (siri && uuid == 0x1812) continue;
        { Guard lock; characteristics.clear(); }
        token = begin_procedure();
        err = wait_procedure(ble_gattc_disc_all_chrs(connection_handle, service.start_handle,
                                                    service.end_handle, on_characteristic, token));
        if (err) return err;
        const auto chars = characteristics;
        if (uuid == 0x1812) {
            if (map_index == 8) return BLE_HS_ENOMEM;
            auto map = std::find_if(chars.begin(), chars.end(), [](const ble_gatt_chr &c) {
                return ble_uuid_u16(&c.uuid.u) == 0x2a4b;
            });
            if (map == chars.end()) return BLE_HS_ENOENT;
            if ((err = read_attribute(map->val_handle))) return err;
            { Guard lock; if (!keyboard->add_map(map_index, read_value.data(), read_value.size())) return BLE_HS_ENOTSUP; }
            discovered_layout.maps.push_back(read_value);
        }
        for (size_t c = 0; c < chars.size(); ++c) {
            const auto &chr = chars[c];
            const uint16_t cuuid = ble_uuid_u16(&chr.uuid.u);
            if (uuid == 0x1800 && cuuid == 0x2a00) {
                if (!read_attribute(chr.val_handle)) {
                    Guard lock;
                    const size_t len = std::min(read_value.size(), sizeof(state.peer.name) - 1);
                    memcpy(state.peer.name, read_value.data(), len); state.peer.name[len] = 0;
                }
                continue;
            }
            if (uuid == 0x1812 && cuuid == 0x2a4e) {
                const uint8_t mode = 1; // Report protocol, needed for NKRO and consumer controls.
                const bool without_response = chr.properties & BLE_GATT_CHR_PROP_WRITE_NO_RSP;
                if (without_response)
                    err = ble_gattc_write_no_rsp_flat(connection_handle, chr.val_handle, &mode, 1);
                else if (chr.properties & BLE_GATT_CHR_PROP_WRITE) err = write_attribute(chr.val_handle, &mode, 1);
                else continue;
                if (err) return err;
                discovered_layout.protocol_mode = chr.val_handle;
                discovered_layout.protocol_mode_without_response = without_response;
                continue;
            }
            const bool battery = uuid == 0x180f && ble::battery_characteristic(cuuid);
            if (!battery && !(uuid == 0x1812 && cuuid == 0x2a4d && (chr.properties & BLE_GATT_CHR_PROP_NOTIFY))) continue;
            if (battery) {
                const bool readable = chr.properties & BLE_GATT_CHR_PROP_READ;
                { Guard lock; battery_characteristics.push_back({chr.val_handle, cuuid, readable}); }
                discovered_layout.batteries.push_back({chr.val_handle, cuuid, 0, readable, 0});
                ESP_LOGI(kTag, "Battery characteristic 0x%04x", cuuid);
                if ((chr.properties & BLE_GATT_CHR_PROP_READ) && !read_attribute(chr.val_handle)) {
                    Guard lock; update_battery(cuuid, read_value.data(), read_value.size());
                }
            }
            const uint16_t end = c + 1 < chars.size() ? chars[c + 1].def_handle - 1 : service.end_handle;
            if (end <= chr.val_handle) continue;
            { Guard lock; descriptors.clear(); }
            token = begin_procedure();
            err = wait_procedure(ble_gattc_disc_all_dscs(connection_handle, chr.val_handle, end, on_descriptor, token));
            if (err) {
                if (!battery) return err;
                ESP_LOGW(kTag, "Battery descriptor discovery failed: %d", err);
                continue;
            }
            uint16_t ccc = 0, reference = 0;
            for (const auto &desc : descriptors) {
                const uint16_t duuid = ble_uuid_u16(&desc.uuid.u);
                if (duuid == 0x2902) ccc = desc.handle;
                if (duuid == 0x2908) reference = desc.handle;
            }
            if (!ccc || !(chr.properties & (BLE_GATT_CHR_PROP_NOTIFY | BLE_GATT_CHR_PROP_INDICATE))) continue;
            const uint8_t notify[] = {static_cast<uint8_t>((chr.properties & BLE_GATT_CHR_PROP_NOTIFY) ? 1 : 2), 0};
            if (!battery) {
                if (!reference) return BLE_HS_ENOENT;
                if ((err = read_attribute(reference))) return err;
                if (read_value.size() != 2) return BLE_HS_EBADDATA;
                if (read_value[1] != 1) continue; // Only input reports produce key events.
                Guard lock;
                if (input_reports.size() == 16) return BLE_HS_ENOMEM;
                input_reports.push_back(InputReport{chr.val_handle, map_index, read_value[0]});
                discovered_layout.reports.push_back({chr.val_handle, ccc, map_index, read_value[0], notify[0]});
            } else {
                discovered_layout.batteries.back().ccc = ccc;
                discovered_layout.batteries.back().subscription = notify[0];
            }
            if ((err = write_attribute(ccc, notify, sizeof(notify)))) {
                if (!battery) return err; // Optional telemetry must not prevent HID use.
                ESP_LOGW(kTag, "Battery subscription failed: %d", err);
            } else if (battery) {
                Guard lock;
                for (auto &entry : battery_characteristics) if (entry.handle == chr.val_handle) entry.poll = false;
            }
        }
        if (uuid == 0x1812) ++map_index;
    }
    Guard lock;
    return siri || !input_reports.empty() ? 0 : BLE_HS_ENOENT;
}

// Python negotiates 527 bytes for these models. This uses the existing
// shared host; mbuf chains carry reports larger than an ACL fragment.
int exchange_siri_mtu() {
    int err = ble_att_set_preferred_mtu(527);
    if (err) return err;
    void *token = begin_procedure();
    err = wait_procedure(ble_gattc_exchange_mtu(connection_handle, on_mtu, token));
    return err == BLE_HS_EALREADY ? 0 : err;
}

// Apple's firmware-specific report setup, repeated on every connection.
int start_siri_reports() {
    int err;
    const uint8_t notify[] = {1, 0};
    for (size_t i = 0; i < siri_profile.notification_count; ++i)
        if ((err = write_attribute(siri_profile.notification_handles[i], notify, sizeof(notify)))) return err;
    for (size_t i = 0; i < siri_profile.init_count; ++i)
        if ((err = write_attribute(siri_profile.init_handle, siri_profile.init_values[i].data(), 2))) return err;
    return 0;
}

// The Database Hash identifies this GATT layout on later connections. It is
// optional; a remote without one is discovered in full every time.
void discover_database_hash(const std::vector<ble_gatt_svc> &found_services) {
    for (const auto &service : found_services) {
        if (ble_uuid_u16(&service.uuid.u) != 0x1801) continue;
        { Guard lock; characteristics.clear(); }
        void *token = begin_procedure();
        if (wait_procedure(ble_gattc_disc_all_chrs(connection_handle, service.start_handle,
                                                   service.end_handle, on_characteristic, token))) return;
        const auto chars = characteristics;
        for (const auto &chr : chars)
            if (ble_uuid_u16(&chr.uuid.u) == 0x2b2a && !read_attribute(chr.val_handle) && read_value.size() == 16)
                discovered_layout.validators.push_back({chr.val_handle, read_value});
        return;
    }
}

int discover_remote(ble::SiriGeneration advertised) {
    {
        Guard lock;
        services.clear(); input_reports.clear(); battery_characteristics.clear(); keyboard->clear();
        siri_connected = false;
        state.battery_percent = state.charging = -1;
    }
    discovered_layout = {};
    void *token = begin_procedure();
    int err = wait_procedure(ble_gattc_disc_all_svcs(connection_handle, on_service, token));
    if (err) return err;
    const auto found_services = services;
    uint16_t product = 0, firmware = 0;
    bool have_firmware = false, apple = false;
    ble::GattLayout::Validator firmware_validator;
    const bool advertised_siri = advertised == ble::SiriGeneration::Gen2 ||
                                 advertised == ble::SiriGeneration::Gen3;
    // Probe Device Information on every connection, including saved identities
    // and HID-only advertisements. Early Siri Remotes must not fall back to HID.
    for (const auto &service : found_services) {
        if (ble_uuid_u16(&service.uuid.u) != 0x180a) continue;
        { Guard lock; characteristics.clear(); }
        token = begin_procedure();
        err = wait_procedure(ble_gattc_disc_all_chrs(connection_handle, service.start_handle,
                                                    service.end_handle, on_characteristic, token));
        if (err) {
            if (advertised_siri) return err;
            continue;
        }
        const auto chars = characteristics;
        for (const auto &chr : chars) {
            const uint16_t uuid = ble_uuid_u16(&chr.uuid.u);
            if (uuid != 0x2a50 && uuid != 0x2a26) continue;
            err = read_attribute(chr.val_handle);
            if (err) {
                if (advertised_siri) return err;
                continue; // Optional information on ordinary HID keyboards.
            }
            if (uuid == 0x2a50) apple = ble::siri_parse_pnp(read_value.data(), read_value.size(), product);
            else {
                have_firmware = ble::siri_parse_firmware(read_value.data(), read_value.size(), firmware);
                firmware_validator = {chr.val_handle, read_value};
            }
        }
    }
    if (apple && (product == 0x0266 || product == 0x026d)) return BLE_HS_ENOTSUP;
    const bool supported_siri = apple && ble::siri_supported_product(product);
    if (advertised_siri && !supported_siri) return BLE_HS_ENOTSUP;
    if (supported_siri) {
        if (!have_firmware) return BLE_HS_EBADDATA;
        if ((err = exchange_siri_mtu())) return err;
        {
            Guard lock;
            siri_profile = ble::siri_profile(firmware);
            siri_connected = true;
            snprintf(state.peer.name, sizeof(state.peer.name), "Siri Remote, Gen %u", product == 0x0314 ? 2 : 3);
        }
        ESP_LOGI(kTag, "Siri Remote product 0x%04x firmware 0x%04x", product, firmware);
        // Apple's profile depends on the firmware, so an update invalidates the saved layout.
        discovered_layout.siri = true;
        discovered_layout.siri_firmware = firmware;
        discovered_layout.validators.push_back(firmware_validator);
    }
    discover_database_hash(found_services);
    // Siri uses dynamically discovered battery and name characteristics, while
    // its input service requires Apple's firmware-specific setup, below.
    if ((err = discover_hid(found_services, supported_siri))) return err;
    if (!supported_siri) return 0;
    if ((err = start_siri_reports())) return err;
    // Read telemetry after enabling Apple's reports, matching Python setup.
    std::vector<BatteryCharacteristic> battery_reads;
    { Guard lock; battery_reads = battery_characteristics; }
    for (const auto &entry : battery_reads) if (!read_attribute(entry.handle)) {
        Guard lock; update_battery(entry.uuid, read_value.data(), read_value.size());
    }
    return 0;
}

// Reuses the bonded remote's saved layout once its validators read back
// unchanged. Returns false to run full discovery on the same link.
bool restore_remote(ble::GattLayout &layout) {
    ble_addr_t peer{};
    {
        Guard lock;
        ble_gap_conn_desc desc{};
        if (!have_saved || ble_gap_conn_find(connection_handle, &desc) || !same_address(desc.peer_id_addr, saved))
            return false;
        peer = desc.peer_id_addr;
    }
    ble_addr_t cached{};
    if (!load_layout(layout)) return false;
    cached.type = layout.peer_type;
    memcpy(cached.val, layout.peer.data(), sizeof(cached.val));
    if (!same_address(cached, peer)) return false;
    for (const auto &validator : layout.validators) {
        const int err = read_attribute(validator.handle);
        bool unchanged;
        { Guard lock; unchanged = !err && read_value == validator.value; }
        if (unchanged) continue;
        // A different value or an attribute error means the database changed;
        // a lost link says nothing about the layout.
        if (!err || att_error(err)) {
            ESP_LOGI(kTag, "Remote GATT layout changed; discovering again");
            erase_layout();
        }
        return false;
    }
    {
        Guard lock;
        services.clear(); input_reports.clear(); battery_characteristics.clear(); keyboard->clear();
        state.battery_percent = state.charging = -1;
        for (size_t i = 0; i < layout.maps.size(); ++i)
            if (!keyboard->add_map(static_cast<uint8_t>(i), layout.maps[i].data(), layout.maps[i].size())) return false;
        for (const auto &report : layout.reports) input_reports.push_back({report.handle, report.map, report.id});
        for (const auto &battery : layout.batteries) battery_characteristics.push_back({battery.handle, battery.uuid, battery.readable});
        const size_t length = std::min(layout.name.size(), sizeof(state.peer.name) - 1);
        memcpy(state.peer.name, layout.name.data(), length);
        state.peer.name[length] = 0;
        siri_connected = layout.siri;
        if (layout.siri) siri_profile = ble::siri_profile(layout.siri_firmware);
    }
    if (!layout.siri) return true;
    int err = exchange_siri_mtu();
    if (!err) err = start_siri_reports();
    if (err && att_error(err)) erase_layout();
    return !err;
}

// Repeats the remaining per-connection setup after a restored remote is ready.
// Bonded remotes keep their subscriptions, so input can arrive before this ends.
int complete_restore(const ble::GattLayout &layout) {
    int err;
    if (layout.protocol_mode) {
        const uint8_t mode = 1;
        err = layout.protocol_mode_without_response
                  ? ble_gattc_write_no_rsp_flat(connection_handle, layout.protocol_mode, &mode, 1)
                  : write_attribute(layout.protocol_mode, &mode, 1);
        if (err) return err;
    }
    for (const auto &report : layout.reports) {
        const uint8_t value[] = {report.subscription, 0};
        if ((err = write_attribute(report.ccc, value, sizeof(value)))) return err;
    }
    for (const auto &battery : layout.batteries) {
        if (battery.ccc) {
            const uint8_t value[] = {battery.subscription, 0};
            if ((err = write_attribute(battery.ccc, value, sizeof(value)))) {
                ESP_LOGW(kTag, "Battery subscription failed: %d", err);
            } else {
                Guard lock;
                for (auto &entry : battery_characteristics) if (entry.handle == battery.handle) entry.poll = false;
            }
        }
        if (battery.readable && !read_attribute(battery.handle)) {
            Guard lock; update_battery(battery.uuid, read_value.data(), read_value.size());
        }
    }
    return 0;
}

int on_scan_event(ble_gap_event *event, void *) {
    Guard lock;
    if (event->type == BLE_GAP_EVENT_DISC_COMPLETE) {
        state.scanning = false;
        return 0;
    }
    if (event->type != BLE_GAP_EVENT_DISC) return 0;
    const auto &report = event->disc;
    ble_hs_adv_fields fields{};
    if (ble_hs_adv_parse_fields(&fields, report.data, report.length_data) != 0) return 0;
    bool hid = fields.appearance_is_present && (fields.appearance >> 6) == 15;
    for (size_t i = 0; i < fields.num_uuids16; ++i)
        hid |= ble_uuid_u16(&fields.uuids16[i].u) == 0x1812;
    const auto siri = ble::siri_classify_manufacturer(fields.mfg_data, fields.mfg_data_len);
    const bool supported = siri != ble::SiriGeneration::Unsupported &&
        (hid || siri == ble::SiriGeneration::Gen2 || siri == ble::SiriGeneration::Gen3);
    auto found = std::find_if(candidates.begin(), candidates.end(), [&](const Candidate &candidate) {
        return same_address(candidate.address, report.addr);
    });
    if (found == candidates.end()) {
        if (candidates.size() == kMaxPeers) {
            // Do not let unrelated advertisements hide keyboards later in scan.
            if (!supported) return 0;
            found = std::find_if(candidates.begin(), candidates.end(), [](const Candidate &c) { return !c.supported(); });
            if (found == candidates.end()) return 0;
            *found = Candidate{};
        } else {
            candidates.push_back(Candidate{});
            found = candidates.end() - 1;
        }
        found->address = report.addr;
    }
    found->hid |= hid;
    if (siri != ble::SiriGeneration::None) found->siri = siri;
    found->connectable |= report.event_type == BLE_HCI_ADV_RPT_EVTYPE_ADV_IND ||
                          report.event_type == BLE_HCI_ADV_RPT_EVTYPE_DIR_IND;
    found->rssi = report.rssi;
    return 0;
}

// Caller holds the remote mutex, on the worker with no search or connection
// running. Searching first could replace `selected` and orphan that bond, and a
// kept bond would make the same remote's new pairing a refused repeat pairing.
int retry_bond_cleanup() {
    int err = have_selected ? remove_peer(selected) : 0;
    if (!err) err = persist_peer(nullptr);
    if (err) ESP_LOGW(kTag, "Removing remote bond: %d", err);
    else bond_cleanup_failed = have_selected = false;
    return err;
}

void cancel_reconnect() {
    Guard lock;
    if (!reconnect_pending) return;
    // Cancellation completes asynchronously. Invalidate the callback first so
    // even a connection established during cancellation cannot revive the peer.
    reconnect_pending = false;
    state.waiting = state.connecting = false;
    ++generation;
    procedure_pending = false;
    const int err = ble_gap_conn_cancel();
    reconnect_cancel_pending = err && ble_gap_conn_active();
    next_cancel_attempt = esp_timer_get_time() + 5000000;
    if (reconnect_cancel_pending) ESP_LOGW(kTag, "Cancelling remote wait failed: %d", err);
}

void launch_scan(uint32_t seconds, bool clear_error) {
    ble_gap_disc_cancel();
    uint8_t public_address[6];
    int err = ble_hs_id_copy_addr(BLE_ADDR_PUBLIC, public_address, nullptr);
    if (!err) {
        ble_gap_disc_params params{};
        params.itvl = 0x80;
        // Wi-Fi can interrupt listening; keep the full interval available to BLE.
        params.window = params.itvl;
        params.filter_duplicates = 0; // Preserve separate advertisement + scan-response data.
        err = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, seconds * 1000, &params, on_scan_event, nullptr);
    }
    Guard lock;
    state.scanning = !err;
    if (err || clear_error) state.last_error = err;
    if (err) state.pairing = false;
}

void start_scan(uint32_t seconds, bool clear_error = true) {
    Guard lock;
    if (radio_paused.load() || setup_cancelled.load() || forgetting || state.unpairing) return;
    cancel_reconnect();
    if (connection_handle != BLE_HS_CONN_HANDLE_NONE && !descriptors_ready)
        ble_gap_terminate(connection_handle, BLE_ERR_REM_USER_CONN_TERM);
    // Wait for the controller's cancellation acknowledgement before scanning.
    // Otherwise a scan accepted while waiting could immediately fail with BUSY.
    pending_scan_seconds = seconds;
    pending_scan_clear_error = clear_error;
    state.scanning = true;
    if (clear_error) state.last_error = 0;
    if (!ble_gap_conn_active()) {
        pending_scan_seconds = 0;
        launch_scan(seconds, clear_error);
    }
}

void finish_peer(int err, ble::SiriGeneration advertised) {
    {
        Guard lock;
        state.waiting = false;
        if (setup_cancelled.load() || pairing_stopped.load() || forgetting) {
            state.connecting = false;
            return;
        }
        if (!err && connection_handle == BLE_HS_CONN_HANDLE_NONE) {
            // The link dropped after the connect callback, before security.
            // A saved peer can reconnect; an initial pairing needs an outcome.
            if (!have_saved && !state.last_error) state.last_error = BLE_HS_ENOTCONN;
            state.connecting = false;
            return;
        }
        state.connecting = !err;
    }
    if (!err) log_link_up();
    if (!err) {
        begin_procedure(Procedure::Security);
        err = wait_procedure(ble_gap_security_initiate(connection_handle), 30000);
    }
    ble::GattLayout restored;
    bool reused = false;
    if (!err && advertised == ble::SiriGeneration::Unsupported) err = BLE_HS_ENOTSUP;
    else if (!err) {
        reused = restore_remote(restored);
        if (!reused) err = discover_remote(advertised);
    }
    if (!err) {
        Guard lock;
        ble_gap_conn_desc desc{};
        err = ble_gap_conn_find(connection_handle, &desc);
        if (!err && (!desc.sec_state.bonded || !desc.sec_state.encrypted)) err = BLE_HS_EAUTHEN;
        if (!err && (setup_cancelled.load()||pairing_stopped.load()||forgetting)) err = BLE_HS_EAPP;
        if (!err) err = persist_peer(&desc.peer_id_addr);
        if (!err) {
            saved = desc.peer_id_addr;
            have_saved = state.bonded = true;
            descriptors_ready = state.connected = true;
            to_public(state.peer, saved);
            ESP_LOGI(kTag, "Encrypted remote ready: %s (%u HID report characteristics, %s layout)", state.peer.name,
                     static_cast<unsigned>(input_reports.size()), reused ? "saved" : "discovered");
            if (!reused) {
                discovered_layout.peer_type = saved.type;
                memcpy(discovered_layout.peer.data(), saved.val, discovered_layout.peer.size());
                discovered_layout.name = state.peer.name;
            }
        }
    }
    if (!err && !reused) {
        const esp_err_t stored = store_layout(discovered_layout);
        if (stored != ESP_OK) ESP_LOGW(kTag, "Saving remote layout failed: %s", esp_err_to_name(stored));
    }
    if (err) {
        ESP_LOGW(kTag, "Remote connection/discovery failed: %d", err);
        ble_gap_conn_cancel();
        uint16_t handle;
        {
            Guard lock;
            state.last_error = pairing_stopped.load() ? 0 : err;
            handle = connection_handle;
            // A device that bonded but failed remote setup must not consume a
            // bond slot forever or leave an identity inaccessible to Unpair.
            ble_gap_conn_desc desc{};
            if (!have_saved) {
                if (!ble_gap_conn_find(handle, &desc)) selected = desc.peer_id_addr;
                const int cleanup = remove_peer(selected);
                if (cleanup) {
                    bond_cleanup_failed = true;
                    ESP_LOGW(kTag, "Removing failed remote bond: %d", cleanup);
                }
            }
        }
        if (handle != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(handle, BLE_ERR_REM_USER_CONN_TERM);
    }
    {
        Guard lock;
        state.connecting = false;
        state.last_error = pairing_stopped.load() ? 0 : err;
        if (err) count_reconnect_failure();
        else reconnect_failures = 0;
        next_reconnect = esp_timer_get_time() + reconnect_backoff_us();
    }
    if (err || !reused) return;
    const int setup = complete_restore(restored);
    const uint16_t handle = connection_handle.load();
    if (!setup || handle == BLE_HS_CONN_HANDLE_NONE) return;
    // The link is up but the saved layout did not work; discover it next time.
    ESP_LOGW(kTag, "Saved remote layout setup failed: %d", setup);
    erase_layout();
    ble_gap_terminate(handle, BLE_ERR_REM_USER_CONN_TERM);
}

void open_peer(const ble_addr_t &address) {
    ble::SiriGeneration advertised = ble::SiriGeneration::None;
    {
        Guard lock;
        if (setup_cancelled.load()||radio_paused.load()||state.unpairing) return;
        ble_gap_disc_cancel();
        pending_scan_seconds = 0;
        selected = address;
        have_selected = true;
        state.scanning = state.waiting = false;
        state.connecting = true;
        forgetting = false;
        state.last_error = 0;
        state.peer = {};
        to_public(state.peer, address);
        connect_started_us = esp_timer_get_time();
        for (const auto &candidate : candidates)
            if (same_address(address, candidate.address))
                advertised = candidate.siri;
    }
    // Use the stable public identity for HID; Matter's commissioning address
    // changes on boot and would make a bonded remote see a different central.
    uint8_t public_address[6];
    int err = ble_hs_id_copy_addr(BLE_ADDR_PUBLIC, public_address, nullptr);
    if (!err) {
        void *token = begin_procedure(Procedure::Connect);
        err = wait_procedure(ble_gap_connect(BLE_OWN_ADDR_PUBLIC, &address, 10000, nullptr,
                                            on_gap_event, token), 12000);
    }
    finish_peer(err, advertised);
}

void start_reconnect() {
    Guard lock;
    if (radio_paused.load() || forgetting || state.unpairing || !have_saved || !state.ready || state.scanning ||
        state.connecting || state.pairing || reconnect_pending || reconnect_cancel_pending ||
        connection_handle != BLE_HS_CONN_HANDLE_NONE || ble_gap_conn_active()) return;
    selected = saved;
    have_selected = true;
    state.waiting = true;
    state.last_error = 0;
    // Wi-Fi coexistence can interrupt listening, so use a full scan window.
    // The controller initiates the connection when the saved peer advertises;
    // an absent or sleeping remote never expires this procedure.
    ble_gap_conn_params params{};
    params.scan_itvl = 0x80;
    params.scan_window = params.scan_itvl;
    params.itvl_min = BLE_GAP_INITIAL_CONN_ITVL_MIN;
    params.itvl_max = BLE_GAP_INITIAL_CONN_ITVL_MAX;
    params.latency = BLE_GAP_INITIAL_CONN_LATENCY;
    params.supervision_timeout = BLE_GAP_INITIAL_SUPERVISION_TIMEOUT;
    params.min_ce_len = BLE_GAP_INITIAL_CONN_MIN_CE_LEN;
    params.max_ce_len = BLE_GAP_INITIAL_CONN_MAX_CE_LEN;
    reconnect_pending = true;
    connect_started_us = esp_timer_get_time();
    void *token = begin_procedure(Procedure::Connect);
    uint8_t public_address[6];
    int err = ble_hs_id_copy_addr(BLE_ADDR_PUBLIC, public_address, nullptr);
    if (!err) err = ble_gap_connect(BLE_OWN_ADDR_PUBLIC, &saved, BLE_HS_FOREVER, &params,
                                   on_gap_event, token);
    if (err) finish_procedure(err);
}

void finish_forget_peer() {
    Guard lock;
    if (!unpair_cleanup_pending ||
        !unpair_cleanup_started || ble_gap_conn_active()) return;
    // Initiating cancellation is asynchronous. Resolver changes are illegal
    // until its completion, even though the UI has already accepted Unpair.
    const int bond_error = unpair_delete_bond ? remove_peer(unpair_target) : 0;
    if (bond_error) ESP_LOGW(kTag, "Removing remote bond: %d", bond_error);
    const esp_err_t err = persist_peer(nullptr);
    state.last_error = bond_error ? bond_error : err;
    bond_cleanup_failed = bond_error != 0 || err != ESP_OK;
    // Keep the exact failed target available for an explicit retry.
    if (bond_cleanup_failed && unpair_delete_bond) { selected = unpair_target; have_selected = true; }
    state.peer = {};
    unpair_cleanup_started = unpair_cleanup_pending = false;
    if (connection_handle == BLE_HS_CONN_HANDLE_NONE) state.unpairing = false;
}

void forget_peer() {
    uint16_t handle;
    ble_addr_t old{};
    bool cancelling_background;
    {
        Guard lock;
        forgetting = true;
        cancelling_background = reconnect_pending || reconnect_cancel_pending;
        cancel_reconnect();
        pending_scan_seconds = 0;
        handle = connection_handle;
        old = have_saved ? saved : selected;
        ble_gap_conn_desc desc{};
        if (handle != BLE_HS_CONN_HANDLE_NONE && !ble_gap_conn_find(handle, &desc)) old = desc.peer_id_addr;
        unpair_target = old;
        unpair_delete_bond = have_saved || have_selected;
        unpair_cleanup_started = true;
        have_saved = have_selected = false;
        reconnect_failures = 0;
        state.bonded = false;
        state.battery_percent = state.charging = -1;
        state.connecting = state.waiting = false;
        state.scanning = false;
        state.pairing = false;
        if (disconnect_callback) disconnect_callback(disconnect_context);
        keyboard->clear();
        descriptors_ready = false;
    }
    ble_gap_disc_cancel();
    if (!cancelling_background) ble_gap_conn_cancel();
    if (handle != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(handle, BLE_ERR_REM_USER_CONN_TERM);
    finish_forget_peer();
}

void worker(void *) {
    for (;;) {
        Command command{};
        // Wake when a backed-off reconnect is due, and at least every 500 ms.
        TickType_t wait = pdMS_TO_TICKS(500);
        {
            Guard lock;
            const int64_t until = next_reconnect - esp_timer_get_time();
            if (reconnect_allowed() && until < 500000)
                wait = std::max<TickType_t>(1, pdMS_TO_TICKS((std::max<int64_t>(until, 0) + 999) / 1000));
        }
        if (xQueueReceive(commands, &command, wait) == pdTRUE) {
            switch (command.type) {
            case CommandType::AutoPair: {
                Guard lock;
                if (radio_paused.load()||setup_cancelled.load()||forgetting) { state.pairing = false; break; }
                if (bond_cleanup_failed) {
                    const int cleanup = retry_bond_cleanup();
                    if (cleanup) { state.pairing = state.scanning = false; state.last_error = cleanup; break; }
                }
                pairing_deadline = esp_timer_get_time() + command.seconds * 1000000LL;
                start_scan(command.seconds);
                break;
            }
            case CommandType::Unpair:
                forget_peer();
                break;
            case CommandType::Wake: break;
            }
        }
        {
            Guard lock;
            state.ready = ble_hs_synced();
            if(radio_paused.load() && reconnect_pending)cancel_reconnect();
            // A host reset ends the controller procedure. Let Matter resync
            // its shared host, then start listening again with a fresh token.
            if (reconnect_pending && !state.ready) cancel_reconnect();
            if (reconnect_cancel_pending) {
                if (!ble_gap_conn_active()) reconnect_cancel_pending = false;
                else if (state.ready && esp_timer_get_time() >= next_cancel_attempt) {
                    const int err = ble_gap_conn_cancel();
                    reconnect_cancel_pending = err && ble_gap_conn_active();
                    next_cancel_attempt = esp_timer_get_time() + 5000000;
                    if (reconnect_cancel_pending) ESP_LOGW(kTag, "Cancelling remote wait failed: %d", err);
                }
            }
            if (state.unpairing && !unpair_cleanup_pending &&
                connection_handle == BLE_HS_CONN_HANDLE_NONE && !ble_gap_conn_active()) state.unpairing = false;
            if (pending_scan_seconds && state.ready && !forgetting && !radio_paused.load() && !ble_gap_conn_active()) {
                const uint32_t seconds = pending_scan_seconds;
                pending_scan_seconds = 0;
                launch_scan(seconds, pending_scan_clear_error);
            }
        }
        finish_forget_peer();
        // Run connection/security/discovery on this worker, never on the host's
        // scan callback. Selection is first eligible advertisement, not RSSI rank.
        ble_addr_t automatic_peer{};
        bool pair_now = false, resume_search = false, stop_search = false;
        uint32_t remaining_seconds = 0;
        {
            Guard lock;
            if (state.pairing && !state.connecting) {
                const int64_t remaining = pairing_deadline - esp_timer_get_time();
                // A bond left behind ends the search; the next Pair removes it.
                if (state.bonded || forgetting || bond_cleanup_failed || remaining <= 0) {
                    state.pairing = false;
                    pending_scan_seconds = 0;
                    stop_search = state.scanning;
                    state.scanning = false;
                    if (!state.bonded && !state.last_error) state.last_error = BLE_HS_ETIMEOUT;
                } else if (connection_handle == BLE_HS_CONN_HANDLE_NONE) {
                    auto found = std::find_if(candidates.begin(), candidates.end(), [](const Candidate &c) {
                        return ble::pairing_candidate(c.supported(), c.connectable, c.attempted,
                            c.rssi, CONFIG_AMITY_REMOTE_PAIRING_MIN_RSSI);
                    });
                    if (found != candidates.end()) {
                        found->attempted = true;
                        automatic_peer = found->address;
                        pair_now = true;
                    } else if (!state.scanning) {
                        resume_search = true;
                        remaining_seconds = static_cast<uint32_t>((remaining + 999999) / 1000000);
                    }
                }
            }
        }
        if (stop_search) ble_gap_disc_cancel();
        if (resume_search) start_scan(remaining_seconds, false);
        if (pair_now) {
            open_peer(automatic_peer);
            Guard lock;
            if (state.bonded) state.pairing = false;
        }
        int reconnect_result = 0;
        bool reconnect_complete = false;
        {
            Guard lock;
            if (reconnect_pending && !procedure_pending) {
                reconnect_pending = false;
                state.waiting = false;
                reconnect_result = procedure_status;
                reconnect_complete = true;
            }
        }
        if (reconnect_complete) finish_peer(reconnect_result, ble::SiriGeneration::None);
        bool reconnect_now = false;
        {
            Guard lock;
            reconnect_now = reconnect_allowed() && esp_timer_get_time() >= next_reconnect;
        }
        if (reconnect_now) start_reconnect();
        bool setup_done=false;
        {Guard lock;setup_done=!state.scanning&&!state.connecting&&!state.pairing&&!pending_scan_seconds;}
        if(setup_done&&radio_owned.exchange(false)&&radio_done)radio_done(radio_context);
        // Keep a recent signal strength for the link-down log and status.
        bool rssi_due = false;
        {
            Guard lock;
            const int64_t now = esp_timer_get_time();
            if (connection_handle != BLE_HS_CONN_HANDLE_NONE && now >= next_rssi_sample) {
                rssi_due = true;
                next_rssi_sample = now + 1000000;
            }
        }
        if (rssi_due) sample_rssi();
        // Read-only characteristics (or failed subscriptions) still get refreshed.
        std::vector<BatteryCharacteristic> battery_reads;
        {
            Guard lock;
            if (state.connected && !forgetting && esp_timer_get_time() >= next_battery_read) {
                battery_reads = battery_characteristics;
                next_battery_read = esp_timer_get_time() + 30000000;
            }
        }
        for (const auto &entry : battery_reads) if (entry.poll && !read_attribute(entry.handle)) {
            Guard lock;
            if (state.connected && !forgetting) update_battery(entry.uuid, read_value.data(), read_value.size());
        }
    }
}
} // namespace

extern "C" esp_err_t ble_remote_start(ble_remote_key_callback_t callback, void *context) {
    if (mutex) return ESP_ERR_INVALID_STATE;
    // Wait for Matter's asynchronous host sync before calling. Requiring it here prevents accidental
    // reinitialization of its controller or overwriting its host configuration.
    if (!ble_hs_synced()) return ESP_ERR_INVALID_STATE;
    mutex = xSemaphoreCreateRecursiveMutex();
    commands = xQueueCreate(4, sizeof(Command));
    procedure_done = xSemaphoreCreateBinary();
    if (!mutex || !commands || !procedure_done) return ESP_ERR_NO_MEM;
    key_callback = callback;
    key_context = context;
    keyboard = new (std::nothrow) ble::HidKeyboard(on_key, nullptr);
    if (!keyboard) return ESP_ERR_NO_MEM;
    state.ready = true;
    state.battery_percent = -1;
    state.charging = -1;
    nvs_handle_t handle;
    if (nvs_open("ble_remote", NVS_READONLY, &handle) == ESP_OK) {
        size_t size = sizeof(saved);
        have_saved = nvs_get_blob(handle, "peer", &saved, &size) == ESP_OK && size == sizeof(saved) && saved.type <= 3;
        nvs_close(handle);
        if (have_saved) { state.bonded = true; to_public(state.peer, saved); }
    }
    // Atomic has no pairing display or text input. Standard Just Works bonding
    // matches its firmware. Avoid IDF HID's keyboard-only default and its fixed
    // input passkey fallback.
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    return xTaskCreate(worker, "ble_remote", 6144, nullptr, 4, nullptr) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

extern "C" esp_err_t ble_remote_start_pairing(uint32_t seconds) {
    if (!commands) return ESP_ERR_INVALID_STATE;
    if (!seconds || seconds > 60) return ESP_ERR_INVALID_ARG;
    RadioAdmission admission(seconds);if(!admission.accepted)return ESP_ERR_INVALID_STATE;
    Guard lock;
    if (!state.ready || state.connected || state.connecting || state.scanning ||
        state.pairing || state.unpairing || have_saved || connection_handle != BLE_HS_CONN_HANDLE_NONE)
        return ESP_ERR_INVALID_STATE;
    Command command{CommandType::AutoPair, seconds};
    if (xQueueSend(commands, &command, 0) != pdTRUE) return ESP_ERR_TIMEOUT;
    candidates.clear();
    forgetting = false;
    state.pairing = state.scanning = true;
    state.last_error = 0;
    setup_cancelled.store(false);
    pairing_stopped.store(false);
    admission.commit();
    return ESP_OK;
}

extern "C" void ble_remote_set_disconnect_callback(ble_remote_disconnect_callback_t callback, void *context) {
    if (mutex) {
        Guard lock;
        disconnect_callback = callback;
        disconnect_context = context;
    } else {
        disconnect_callback = callback;
        disconnect_context = context;
    }
}

extern "C" void ble_remote_set_siri_callback(ble_remote_siri_callback_t callback, void *context) {
    if (mutex) {
        Guard lock;
        siri_callback = callback;
        siri_context = context;
    } else {
        siri_callback = callback;
        siri_context = context;
    }
}

extern "C" esp_err_t ble_remote_unpair(void) {
    if (!commands) return ESP_ERR_INVALID_STATE;
    Guard lock;
    if (state.unpairing) return ESP_ERR_INVALID_STATE;
    Command command{CommandType::Unpair, 0};
    if (xQueueSend(commands, &command, 0) != pdTRUE) return ESP_ERR_TIMEOUT;
    state.unpairing = unpair_cleanup_pending = true;
    unpair_cleanup_started = false;
    state.last_error = 0;
    forgetting = true;
    return ESP_OK;
}

extern "C" void ble_remote_get_status(ble_remote_status_t *out) {
    if (!out) return;
    if (!mutex) { *out = {}; out->battery_percent = 100; out->rssi = 127; return; }
    Guard lock;
    *out = state;
    // Derive the hint from current scan RSSI so moving a remote closer clears
    // it. Show it even before the remote allows connections; rejected peers
    // do not cause a proximity prompt.
    out->too_far = state.pairing && state.scanning && !state.connecting &&
        std::any_of(candidates.begin(), candidates.end(), [](const Candidate &c) {
            return c.supported() && !c.attempted &&
                   c.rssi < CONFIG_AMITY_REMOTE_PAIRING_MIN_RSSI;
        }) &&
        !std::any_of(candidates.begin(), candidates.end(), [](const Candidate &c) {
            return ble::pairing_candidate(c.supported(), c.connectable, c.attempted,
                c.rssi, CONFIG_AMITY_REMOTE_PAIRING_MIN_RSSI);
        });
    const auto battery = ble::reported_battery(out->bonded, out->connected, {out->battery_percent, out->charging});
    out->battery_percent = battery.percent;
    out->charging = battery.charging;
    out->rssi = state.connected && have_rssi ? last_rssi : 127;
}

extern "C" void ble_remote_set_radio_hooks(ble_remote_radio_admit_t admit,ble_remote_radio_done_t done,void *context) {
    radio_admit=admit;radio_done=done;radio_context=context;
}
extern "C" esp_err_t ble_remote_pause_background(bool paused) {
    if(!mutex){radio_paused.store(paused);return ESP_OK;}
    Guard lock;
    if(paused&&(state.scanning||state.pairing||(state.connecting&&!reconnect_pending)))return ESP_ERR_INVALID_STATE;
    radio_paused.store(paused);
    if(!paused){setup_cancelled.store(false);return ESP_OK;}
    cancel_reconnect();
    return ble_gap_conn_active()?ESP_ERR_TIMEOUT:ESP_OK;
}
// Caller holds the remote mutex. Abandons an uncommitted setup attempt, keeps
// a saved bond, and returns whether the radio must be released after unlocking.
static bool abandon_setup_locked(bool &waiting) {
    setup_cancelled.store(true);
    state.pairing=false;state.scanning=false;pending_scan_seconds=0;
    ble_gap_disc_cancel();cancel_reconnect();
    waiting=false;
    if(!have_saved){
        if(procedure_pending)finish_procedure(BLE_HS_EAPP);
        if(ble_gap_conn_active())ble_gap_conn_cancel();
        const auto handle=connection_handle.load();
        if(handle!=BLE_HS_CONN_HANDLE_NONE)ble_gap_terminate(handle,BLE_ERR_REM_USER_CONN_TERM);
        waiting=ble_gap_conn_active()||handle!=BLE_HS_CONN_HANDLE_NONE;
        if(!waiting)state.connecting=false;
    }
    return radio_owned.exchange(false);
}
extern "C" esp_err_t ble_remote_cancel_setup(void) {
    if(!mutex)return ESP_OK;
    bool owned=false,waiting=false;
    {
        Guard lock;
        radio_paused.store(true);
        owned=abandon_setup_locked(waiting);
    }
    if(owned&&radio_done)radio_done(radio_context);
    return waiting?ESP_ERR_TIMEOUT:ESP_OK;
}
extern "C" esp_err_t ble_remote_stop_pairing(void) {
    if(!mutex)return ESP_ERR_INVALID_STATE;
    bool owned=false,waiting=false;
    {
        Guard lock;
        if(have_saved||!(state.pairing||state.scanning||state.connecting))return ESP_OK;
        pairing_stopped.store(true);state.last_error=0;
        owned=abandon_setup_locked(waiting);
    }
    if(owned&&radio_done)radio_done(radio_context);
    return waiting?ESP_ERR_TIMEOUT:ESP_OK;
}
