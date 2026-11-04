/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

// Execute the real worker and GAP callbacks with a deterministic BLE controller.
// Including the implementation exposes its private worker without a test API.
#include "../components/ble_remote/ble_remote.cpp"

#include <cassert>
#include <cstdarg>
#include <deque>
#include <iostream>
#include <memory>
#include <string>

struct TestSemaphore { bool recursive, available; };
struct TestQueue { unsigned capacity, item_size; std::deque<std::vector<uint8_t>> items; };

namespace fake {
struct Stopped {};
struct Connect {
    int32_t duration;
    bool custom_params;
    ble_gap_conn_params params;
    ble_gap_event_fn *callback;
    void *token;
};
int64_t now;
unsigned steps, blocking_waits, cancellations, scans, security_calls, discovery_calls,
    persisted, removed_bonds;
bool synced = true, storage_peer = true, connecting, scanning, immediate_connect,
    disconnect_during_discovery;
int connect_error, cancel_error, discovery_error, bond_delete_error, peer_erase_error;
int privacy_remove_error;
bool host_bond, controller_irk;
std::vector<ble_addr_t> privacy_removals, bond_removals;
unsigned recursive_depth, positive_keys, key_releases;
uint16_t live_handle = BLE_HS_CONN_HANDLE_NONE;
ble_gap_conn_desc descriptor{};
ble_gap_event_fn *scan_callback;
void *scan_token;
std::vector<Connect> connects;
std::vector<uint16_t> terminations;
std::vector<std::string> warnings;
std::vector<std::unique_ptr<TestSemaphore>> semaphores;
std::unique_ptr<TestQueue> queue;
ble_addr_t peer{BLE_ADDR_PUBLIC, {1, 2, 3, 4, 5, 6}};
constexpr uint8_t report_map[] = {
    0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x05, 0x07,
    0x19, 0x04, 0x29, 0x0b, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0xc0
};

void complete_connect(int status, const Connect &attempt, uint16_t handle = 42) {
    connecting = false;
    if (!status) {
        live_handle = handle;
        descriptor = {};
        descriptor.conn_handle = handle;
        descriptor.peer_id_addr = peer;
        descriptor.sec_state.bonded = true;
    }
    ble_gap_event event{};
    event.type = BLE_GAP_EVENT_CONNECT;
    event.connect.status = status;
    event.connect.conn_handle = handle;
    attempt.callback(&event, attempt.token);
}

void complete_connect(int status = 0) {
    assert(!connects.empty());
    const auto attempt = connects.back(); // Callbacks can append future attempts.
    complete_connect(status, attempt);
}

void disconnect(int reason = BLE_HS_HCI_ERR(BLE_ERR_REM_USER_CONN_TERM)) {
    assert(live_handle != BLE_HS_CONN_HANDLE_NONE);
    ble_gap_event event{};
    event.type = BLE_GAP_EVENT_DISCONNECT;
    event.disconnect.reason = reason;
    event.disconnect.conn = descriptor;
    live_handle = BLE_HS_CONN_HANDLE_NONE;
    connects.back().callback(&event, connects.back().token);
}

void finish_scan() {
    assert(scanning && scan_callback);
    scanning = false;
    ble_gap_event event{};
    event.type = BLE_GAP_EVENT_DISC_COMPLETE;
    scan_callback(&event, scan_token);
}

void run_steps(unsigned count) {
    steps = count;
    try { worker(nullptr); assert(false); }
    catch (const Stopped &) {}
}

ble_remote_status_t status() {
    ble_remote_status_t result{};
    ble_remote_get_status(&result);
    return result;
}

ble_uuid_any_t uuid(uint16_t value) {
    ble_uuid_any_t result{};
    result.u16.u.type = 16;
    result.u16.value = value;
    return result;
}
} // namespace fake

void test_ble_log(bool warning, const char *format, ...) {
    char text[256];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    if (warning) fake::warnings.emplace_back(text);
}
int64_t esp_timer_get_time() { return fake::now; }

SemaphoreHandle_t xSemaphoreCreateRecursiveMutex() {
    fake::semaphores.push_back(std::make_unique<TestSemaphore>(TestSemaphore{true, true}));
    return fake::semaphores.back().get();
}
SemaphoreHandle_t xSemaphoreCreateBinary() {
    fake::semaphores.push_back(std::make_unique<TestSemaphore>(TestSemaphore{false, false}));
    return fake::semaphores.back().get();
}
BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t sem, TickType_t) {
    assert(sem && sem->recursive); ++fake::recursive_depth; return pdTRUE;
}
BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t sem) {
    assert(sem && sem->recursive && fake::recursive_depth); --fake::recursive_depth; return pdTRUE;
}
BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t ticks) {
    assert(sem && !sem->recursive);
    if (ticks) ++fake::blocking_waits;
    if (!sem->available) { fake::now += int64_t(ticks) * 1000; return pdFALSE; }
    sem->available = false;
    return pdTRUE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t sem) {
    assert(sem && !sem->recursive); sem->available = true; return pdTRUE;
}
QueueHandle_t xQueueCreate(unsigned capacity, unsigned item_size) {
    fake::queue = std::make_unique<TestQueue>(TestQueue{capacity, item_size, {}});
    return fake::queue.get();
}
BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t ticks) {
    assert(queue && !ticks);
    if (queue->items.size() == queue->capacity) return pdFALSE;
    const auto data = static_cast<const uint8_t *>(item);
    queue->items.emplace_back(data, data + queue->item_size);
    return pdTRUE;
}
BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t ticks) {
    assert(queue && (ticks == 500 || ticks == 0));
    if(ticks){if (!fake::steps) throw fake::Stopped{};--fake::steps;}
    else if(queue->items.empty())return pdFALSE;
    if (queue->items.empty()) { fake::now += int64_t(ticks) * 1000; return pdFALSE; }
    memcpy(item, queue->items.front().data(), queue->item_size);
    queue->items.pop_front();
    return pdTRUE;
}
BaseType_t xTaskCreate(TaskFunction_t function, const char *, uint32_t, void *, unsigned, TaskHandle_t *) {
    assert(function == worker); return pdPASS;
}

esp_err_t nvs_open(const char *, nvs_open_mode_t, nvs_handle_t *handle) { *handle = 1; return ESP_OK; }
void nvs_close(nvs_handle_t) {}
esp_err_t nvs_get_blob(nvs_handle_t, const char *, void *data, size_t *size) {
    if (!fake::storage_peer) return ESP_ERR_NVS_NOT_FOUND;
    assert(*size == sizeof(fake::peer));
    memcpy(data, &fake::peer, *size);
    return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t, const char *, const void *data, size_t size) {
    assert(size == sizeof(fake::peer) && !memcmp(data, &fake::peer, size));
    fake::storage_peer = true; ++fake::persisted; return ESP_OK;
}
esp_err_t nvs_erase_key(nvs_handle_t, const char *) {
    if (fake::peer_erase_error) return fake::peer_erase_error;
    fake::storage_peer = false; return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t) { return ESP_OK; }

extern "C" {
struct ble_hs_cfg ble_hs_cfg{};
int ble_hs_synced() { return fake::synced; }
int ble_hs_id_copy_addr(uint8_t type, uint8_t *address, int *) {
    assert(type == BLE_ADDR_PUBLIC); memset(address, 0x17, 6); return 0;
}
uint16_t ble_uuid_u16(const ble_uuid_t *uuid) {
    return uuid->type == 16 ? reinterpret_cast<const ble_uuid16_t *>(uuid)->value : 0;
}
int os_mbuf_copydata(const os_mbuf *mbuf, int offset, int length, void *out) {
    assert(offset >= 0 && length >= 0 && offset + length <= mbuf->length);
    memcpy(out, mbuf->data + offset, length); return 0;
}
int ble_hs_adv_parse_fields(ble_hs_adv_fields *, const uint8_t *, uint8_t) { return BLE_HS_EBADDATA; }
int ble_gap_connect(uint8_t own, const ble_addr_t *peer, int32_t duration,
                    const ble_gap_conn_params *params, ble_gap_event_fn *callback, void *token) {
    assert(own == BLE_OWN_ADDR_PUBLIC && (peer->type & 1)==fake::peer.type &&
           !memcmp(peer->val, fake::peer.val, sizeof(peer->val)));
    assert(!fake::connecting && !fake::scanning);
    fake::connects.push_back({duration, params != nullptr, params ? *params : ble_gap_conn_params{}, callback, token});
    if (fake::connect_error) return fake::connect_error;
    fake::connecting = true;
    if (fake::immediate_connect) fake::complete_connect();
    return 0;
}
int ble_gap_conn_active() { return fake::connecting; }
int ble_gap_conn_cancel() {
    if (!fake::connecting) return BLE_HS_EALREADY;
    ++fake::cancellations;
    if (fake::cancel_error) return fake::cancel_error;
    // The controller remains busy until its connection-completion event arrives.
    return 0;
}
int ble_gap_conn_find(uint16_t handle, ble_gap_conn_desc *out) {
    if (handle == BLE_HS_CONN_HANDLE_NONE || handle != fake::live_handle) return BLE_HS_ENOTCONN;
    *out = fake::descriptor; return 0;
}
int ble_gap_terminate(uint16_t handle, uint8_t) {
    assert(handle != BLE_HS_CONN_HANDLE_NONE);
    fake::terminations.push_back(handle); return 0;
}
int ble_gap_disc(uint8_t own, int32_t duration, const ble_gap_disc_params *params, ble_gap_event_fn *callback, void *token) {
    assert(own == BLE_OWN_ADDR_PUBLIC && duration > 0);
    assert(params && params->itvl==0x80 && params->window==params->itvl && !params->filter_duplicates);
    assert(!fake::connecting); // Cancellation must finish before a diagnostic scan.
    ++fake::scans; fake::scanning = true; fake::scan_callback = callback; fake::scan_token = token; return 0;
}
int ble_gap_disc_cancel() { fake::scanning = false; return 0; }
int ble_gap_security_initiate(uint16_t handle) {
    assert(handle != BLE_HS_CONN_HANDLE_NONE && handle == fake::live_handle);
    ++fake::security_calls;
    fake::host_bond = true;
    fake::controller_irk = MYNEWT_VAL(BLE_HS_PVCY);
    fake::descriptor.sec_state.encrypted = true;
    ble_gap_event event{};
    event.type = BLE_GAP_EVENT_ENC_CHANGE;
    event.enc_change.conn_handle = handle;
    fake::connects.back().callback(&event, fake::connects.back().token);
    return 0;
}
int ble_hs_pvcy_remove_entry(uint8_t type, const uint8_t *address) {
    // The pinned controller API cannot change its resolver while initiating
    // or discovering. Its state is independent of persisted security records.
    assert(!fake::connecting && !fake::scanning);
    ble_addr_t peer{}; peer.type = type & 1; memcpy(peer.val, address, sizeof(peer.val));
    fake::privacy_removals.push_back(peer);
    assert(!memcmp(&peer, &fake::peer, sizeof(peer)));
    if (fake::privacy_remove_error) return fake::privacy_remove_error;
    if (!fake::controller_irk) return BLE_HS_HCI_ERR(BLE_ERR_UNK_CONN_ID);
    fake::controller_irk = false;
    return 0;
}
int ble_store_util_delete_peer(const ble_addr_t *peer) {
    ++fake::removed_bonds; fake::bond_removals.push_back(*peer);
    assert(!memcmp(peer, &fake::peer, sizeof(*peer)));
    if (!fake::bond_delete_error) fake::host_bond = false;
    return fake::bond_delete_error;
}
int ble_att_set_preferred_mtu(uint16_t) { return 0; }
int ble_gattc_disc_all_svcs(uint16_t handle, ble_gatt_disc_svc_fn *callback, void *token) {
    ++fake::discovery_calls;
    if (fake::disconnect_during_discovery) { fake::disconnect(); return 0; }
    if (fake::discovery_error) return fake::discovery_error;
    const ble_gatt_error success{}, done{BLE_HS_EDONE, 0};
    const ble_gatt_svc service{1, 10, fake::uuid(0x1812)};
    callback(handle, &success, &service, token);
    callback(handle, &done, nullptr, token); return 0;
}
int ble_gattc_disc_all_chrs(uint16_t handle, uint16_t start, uint16_t end, ble_gatt_chr_fn *callback, void *token) {
    assert(start == 1 && end == 10);
    const ble_gatt_error success{}, done{BLE_HS_EDONE, 0};
    const ble_gatt_chr map{2, 3, BLE_GATT_CHR_PROP_READ, fake::uuid(0x2a4b)};
    const ble_gatt_chr report{4, 5, BLE_GATT_CHR_PROP_NOTIFY, fake::uuid(0x2a4d)};
    callback(handle, &success, &map, token);
    callback(handle, &success, &report, token);
    callback(handle, &done, nullptr, token); return 0;
}
int ble_gattc_disc_all_dscs(uint16_t handle, uint16_t start, uint16_t end, ble_gatt_dsc_fn *callback, void *token) {
    assert(start == 5 && end == 10);
    const ble_gatt_error success{}, done{BLE_HS_EDONE, 0};
    const ble_gatt_dsc ccc{6, fake::uuid(0x2902)}, reference{7, fake::uuid(0x2908)};
    callback(handle, &success, start, &ccc, token);
    callback(handle, &success, start, &reference, token);
    callback(handle, &done, start, nullptr, token); return 0;
}
int ble_gattc_read_long(uint16_t handle, uint16_t attr_handle, uint16_t, ble_gatt_attr_fn *callback, void *token) {
    constexpr uint8_t reference[] = {0, 1};
    assert(attr_handle == 3 || attr_handle == 7);
    os_mbuf mbuf{attr_handle == 3 ? fake::report_map : reference,
                 uint16_t(attr_handle == 3 ? sizeof(fake::report_map) : sizeof(reference))};
    ble_gatt_attr attr{attr_handle, 0, &mbuf};
    const ble_gatt_error success{}, done{BLE_HS_EDONE, 0};
    callback(handle, &success, &attr, token);
    callback(handle, &done, nullptr, token); return 0;
}
int ble_gattc_write_flat(uint16_t handle, uint16_t attr, const void *value, uint16_t length,
                        ble_gatt_attr_fn *callback, void *token) {
    assert(attr == 6 && length == 2 && static_cast<const uint8_t *>(value)[0] == 1);
    const ble_gatt_error success{};
    callback(handle, &success, nullptr, token); return 0;
}
int ble_gattc_write_no_rsp_flat(uint16_t, uint16_t, const void *, uint16_t) { return 0; }
int ble_gattc_exchange_mtu(uint16_t handle, ble_gatt_mtu_fn *callback, void *token) {
    const ble_gatt_error success{}; callback(handle, &success, 527, token); return 0;
}
} // extern "C"

int main(int argc, char **argv) {
    assert(argc == 2);
    const std::string scenario = argv[1];
    if (scenario == "pairing" || scenario == "pairing-cleanup-privacy" ||
        scenario == "pairing-cleanup-privacy-error" || scenario == "pairing-disconnected-privacy" ||
        scenario == "pairing-resolved-identity-error" ||
        scenario == "pairing-resolved-random-error") fake::storage_peer = false;
    if (scenario == "pairing-resolved-random-error") fake::peer.type=1;
    fake::host_bond = fake::storage_peer;
    fake::controller_irk = fake::storage_peer && MYNEWT_VAL(BLE_HS_PVCY);
    if(scenario=="actuator-startup")assert(ble_remote_set_controller_enabled(false)==ESP_OK);
    assert(ble_remote_start(nullptr, nullptr) == ESP_OK);

    if(scenario=="pairing-disconnected-privacy") {
        fake::immediate_connect=true;fake::disconnect_during_discovery=true;
        open_peer(fake::peer);
        assert(fake::security_calls==1 && fake::discovery_calls==1);
        assert(fake::status().last_error==BLE_HS_ENOTCONN && !fake::status().connected);
        assert(fake::privacy_removals.size()==1 && fake::removed_bonds==1);
        assert(!fake::controller_irk && !fake::host_bond && !fake::storage_peer);
        assert(fake::terminations.empty()); // The peer has already disconnected.
    }else if(scenario=="pairing-resolved-identity-error" || scenario=="pairing-resolved-random-error") {
        // Scan reports can use identity types before a connection descriptor exists.
        auto resolved=fake::peer;resolved.type+=2;
        fake::connect_error=BLE_HS_EBUSY;
        open_peer(resolved);
        assert(fake::status().last_error==BLE_HS_EBUSY);
        assert(fake::privacy_removals.size()==1 && fake::privacy_removals[0].type==fake::peer.type);
        assert(fake::removed_bonds==1 && fake::bond_removals[0].type==fake::peer.type);
    }else if(scenario=="unpair-privacy") {
        assert(ble_remote_unpair()==ESP_OK);fake::run_steps(1);
        assert(!fake::controller_irk && !fake::host_bond && !fake::storage_peer);
        assert(fake::privacy_removals.size()==1 && fake::removed_bonds==1);
        assert(!fake::status().unpairing && !fake::status().last_error);
        // Repeating a completed UI Unpair has no owned peer left to revoke.
        assert(ble_remote_unpair()==ESP_OK);fake::run_steps(1);
        assert(fake::privacy_removals.size()==1 && fake::removed_bonds==1);
        assert(!fake::status().last_error);
    }else if(scenario=="unpair-privacy-cancel") {
        fake::run_steps(1);assert(fake::connecting);
        assert(ble_remote_unpair()==ESP_OK);fake::run_steps(1);
        assert(fake::privacy_removals.empty() && !fake::removed_bonds && fake::storage_peer);
        assert(fake::status().unpairing);
        fake::complete_connect(BLE_HS_EPREEMPTED);fake::run_steps(1);
        assert(fake::privacy_removals.size()==1 && fake::removed_bonds==1);
        assert(!fake::controller_irk && !fake::host_bond && !fake::storage_peer);
        assert(!fake::status().unpairing && !fake::status().last_error);
    }else if(scenario=="unpair-privacy-error") {
        fake::privacy_remove_error=77;
        assert(ble_remote_unpair()==ESP_OK);fake::run_steps(1);
        assert(fake::controller_irk && fake::host_bond && !fake::removed_bonds);
        assert(fake::status().last_error==77 && !fake::status().unpairing);
        assert(ble_remote_start_pairing(20)==ESP_ERR_INVALID_STATE);
        fake::privacy_remove_error=0;
        assert(ble_remote_unpair()==ESP_OK);fake::run_steps(1);
        assert(fake::privacy_removals.size()==2 && fake::removed_bonds==1);
        assert(!fake::controller_irk && !fake::host_bond && !fake::status().last_error);
    }else if(scenario=="unpair-privacy-connected") {
        fake::run_steps(1);fake::complete_connect();fake::run_steps(1);
        assert(fake::status().connected);
        assert(ble_remote_unpair()==ESP_OK);fake::run_steps(1);
        assert(fake::terminations.size()==1 && fake::privacy_removals.size()==1);
        assert(!fake::controller_irk && !fake::host_bond && fake::status().unpairing);
        fake::disconnect();fake::run_steps(1);
        assert(!fake::status().unpairing && !fake::status().connected);
    }else if(scenario=="actuator-pending-unpair") {
        fake::run_steps(1);assert(ble_remote_unpair()==ESP_OK);fake::run_steps(1);
        assert(fake::status().unpairing && fake::privacy_removals.empty());
        const auto result=ble_remote_set_controller_enabled(false);
        assert(result==ESP_OK||result==ESP_ERR_TIMEOUT);
        fake::complete_connect(BLE_HS_EPREEMPTED);fake::run_steps(4);
        assert(fake::privacy_removals.empty() && !fake::removed_bonds && fake::storage_peer);
    }else if(scenario=="unpair-no-privacy") {
        assert(!MYNEWT_VAL(BLE_HS_PVCY));
        assert(ble_remote_unpair()==ESP_OK);fake::run_steps(1);
        assert(fake::privacy_removals.empty() && fake::removed_bonds==1 && !fake::host_bond);
        assert(!fake::status().last_error && !fake::status().unpairing);
    }else if(scenario=="pairing-cleanup-privacy" || scenario=="pairing-cleanup-privacy-error") {
        fake::immediate_connect=true;fake::discovery_error=BLE_HS_ENOTSUP;
        if(scenario=="pairing-cleanup-privacy-error")fake::privacy_remove_error=77;
        open_peer(fake::peer);
        assert(fake::status().last_error==BLE_HS_ENOTSUP);
        assert(fake::privacy_removals.size()==1 && fake::terminations.size()==1);
        if(fake::privacy_remove_error) {
            assert(fake::controller_irk && fake::host_bond && !fake::removed_bonds);
            fake::disconnect();fake::run_steps(1);
            assert(ble_remote_start_pairing(20)==ESP_ERR_INVALID_STATE);
            fake::privacy_remove_error=0;
            assert(ble_remote_unpair()==ESP_OK);fake::run_steps(1);
            assert(!fake::controller_irk && !fake::host_bond && !fake::status().last_error);
        } else assert(!fake::controller_irk && !fake::host_bond && fake::removed_bonds==1);
    }else if(scenario=="actuator-startup"){
        fake::run_steps(20);assert(fake::connects.empty()&&!fake::scans&&fake::storage_peer&&fake::status().bonded);
        assert(ble_remote_scan(5)==ESP_ERR_INVALID_STATE&&ble_remote_start_pairing(5)==ESP_ERR_INVALID_STATE&&ble_remote_unpair()==ESP_ERR_INVALID_STATE);
        const uint8_t address[6]={6,5,4,3,2,1};assert(ble_remote_pair(address,0)==ESP_ERR_INVALID_STATE);
        assert(ble_remote_pause_background(false)==ESP_OK);fake::run_steps(10);assert(fake::connects.empty()&&!fake::removed_bonds);
        assert(ble_remote_set_controller_enabled(true)==ESP_ERR_INVALID_STATE);
    }else if(scenario=="actuator-input"){
        key_callback=[](uint16_t,uint16_t,bool pressed,void*){if(pressed)++fake::positive_keys;else++fake::key_releases;};
        on_key(7,4,true,nullptr);assert(fake::positive_keys==1);
        assert(ble_remote_set_controller_enabled(false)==ESP_OK);on_key(7,4,true,nullptr);on_key(7,4,false,nullptr);
        assert(fake::positive_keys==1&&fake::key_releases==1);
        // Even an already-popped Pair command cannot cancel another GAP owner.
        const auto cancellations=fake::cancellations;open_peer(fake::peer);assert(fake::cancellations==cancellations&&fake::connects.empty());
    }else if(scenario=="actuator-reconnect"){
        fake::run_steps(1);const auto attempt=fake::connects.back();assert(fake::status().waiting);
        const auto result=ble_remote_set_controller_enabled(false);assert(result==ESP_OK||result==ESP_ERR_TIMEOUT);
        fake::complete_connect(0,attempt);assert(fake::terminations.size()==1);fake::run_steps(5);
        assert(!fake::security_calls&&!fake::discovery_calls&&!fake::removed_bonds&&fake::storage_peer&&fake::status().bonded);
        ble_remote_pause_background(false);fake::run_steps(20);assert(fake::connects.size()==1);
    }else if(scenario=="actuator-connected"){
        fake::run_steps(1);fake::complete_connect();fake::run_steps(1);assert(fake::status().connected);
        assert(ble_remote_set_controller_enabled(false)==ESP_ERR_TIMEOUT);assert(fake::terminations.size()==1);
        fake::disconnect();assert(ble_remote_set_controller_enabled(false)==ESP_OK);ble_remote_pause_background(false);fake::run_steps(20);
        assert(fake::connects.size()==1&&!fake::removed_bonds&&fake::storage_peer&&fake::status().bonded&&!fake::status().connected);
    }else if(scenario=="actuator-queued-unpair"){
        assert(ble_remote_unpair()==ESP_OK);assert(ble_remote_set_controller_enabled(false)==ESP_OK);fake::run_steps(10);
        assert(!fake::removed_bonds&&fake::storage_peer&&fake::status().bonded&&fake::connects.empty());
    }else if (scenario == "absence") {
        fake::run_steps(1);
        const auto status = fake::status();
        assert(status.waiting && status.bonded && !status.connecting && !status.connected && !status.last_error);
        assert(fake::connects.size() == 1 && fake::connects[0].duration == BLE_HS_FOREVER);
        const auto &attempt = fake::connects[0];
        assert(attempt.custom_params && attempt.params.scan_itvl == 0x80);
        assert(attempt.params.scan_window == attempt.params.scan_itvl);
        assert(attempt.params.itvl_min && attempt.params.itvl_max >= attempt.params.itvl_min);
        assert(attempt.params.supervision_timeout);
        fake::run_steps(1200); // Ten minutes of absence must remain one initiator.
        assert(fake::connects.size() == 1 && !fake::blocking_waits && fake::warnings.empty());
        assert(fake::status().waiting && !fake::status().last_error);
    } else if (scenario == "success") {
        fake::run_steps(1);
        fake::complete_connect();
        assert(!fake::status().waiting && fake::status().connecting);
        assert(!fake::security_calls && !fake::discovery_calls); // Setup belongs to the worker.
        const auto completion_time = fake::now;
        fake::run_steps(1);
        assert(fake::now == completion_time); // Completion wakes the worker promptly.
        const auto status = fake::status();
        assert(status.connected && status.bonded && !status.waiting && !status.connecting && !status.last_error);
        assert(fake::security_calls == 1 && fake::discovery_calls == 1 && fake::persisted == 1);
        fake::run_steps(60);
        assert(fake::connects.size() == 1 && fake::warnings.empty());
        fake::disconnect();
        fake::run_steps(1);
        assert(fake::connects.size() == 2 && fake::status().waiting && !fake::status().last_error);
    } else if (scenario == "unpair-delete-error") {
        fake::run_steps(1);fake::bond_delete_error=77;
        assert(ble_remote_unpair()==ESP_OK);fake::run_steps(1);
        assert(!fake::status().last_error && !fake::removed_bonds && fake::controller_irk);
        fake::complete_connect(BLE_HS_EPREEMPTED);fake::run_steps(1);
        assert(!fake::status().unpairing&&!fake::status().bonded&&!fake::storage_peer&&fake::status().last_error==77);
        assert(!fake::controller_irk && fake::host_bond);
        assert(fake::removed_bonds==1&&ble_remote_start_pairing(20)==ESP_ERR_INVALID_STATE);
        fake::bond_delete_error=0;assert(ble_remote_unpair()==ESP_OK);fake::run_steps(1);
        assert(fake::removed_bonds==2&&!fake::status().last_error&&!fake::status().unpairing&&!fake::status().bonded);
        assert(!fake::host_bond && fake::privacy_removals.size()==2);
    } else if (scenario == "unpair-storage-error") {
        fake::peer_erase_error=ESP_ERR_TIMEOUT;
        assert(ble_remote_unpair()==ESP_OK);fake::run_steps(1);
        assert(!fake::status().unpairing&&fake::status().last_error==ESP_ERR_TIMEOUT&&fake::storage_peer);
        assert(fake::removed_bonds==1&&ble_remote_start_pairing(20)==ESP_ERR_INVALID_STATE);
        fake::peer_erase_error=0;fake::bond_delete_error=BLE_HS_ENOENT;
        assert(ble_remote_unpair()==ESP_OK);fake::run_steps(1);
        assert(fake::removed_bonds==2&&!fake::storage_peer&&!fake::status().last_error);
    } else if (scenario == "unpair") {
        fake::run_steps(1);
        const auto stale = fake::connects.back();
        assert(ble_remote_unpair() == ESP_OK);
        fake::run_steps(1);
        assert(fake::cancellations >= 1 && !fake::status().bonded && !fake::status().waiting);
        assert(fake::storage_peer && !fake::removed_bonds && fake::privacy_removals.empty());
        fake::complete_connect(0, stale, 77);
        assert(fake::terminations.size() == 1 && fake::terminations.back() == 77);
        fake::run_steps(60);
        assert(fake::connects.size() == 1 && !fake::security_calls && !fake::persisted);
        assert(!fake::storage_peer && fake::removed_bonds == 1 && !fake::controller_irk);
    } else if (scenario == "scan-unpair") {
        fake::run_steps(1);
        assert(ble_remote_scan(5) == ESP_OK);
        assert(ble_remote_unpair() == ESP_OK);
        fake::complete_connect(BLE_HS_EPREEMPTED); // Cancellation completes before queued work.
        fake::run_steps(2);
        assert(!fake::scans && !fake::status().scanning && !fake::status().bonded);
        assert(!fake::status().waiting && !fake::status().unpairing);
    } else if (scenario == "unpair-scan") {
        fake::run_steps(1);
        const auto stale = fake::connects.back();
        assert(ble_remote_unpair() == ESP_OK);
        assert(ble_remote_scan(5) == ESP_ERR_INVALID_STATE);
        fake::run_steps(1);
        fake::complete_connect(BLE_HS_EPREEMPTED);
        fake::run_steps(1);
        assert(!fake::status().unpairing && !fake::status().bonded);
        assert(ble_remote_scan(5) == ESP_OK);
        fake::run_steps(1);
        assert(fake::scans == 1 && fake::status().scanning);
        // Allowing a new scan must not revive the cancelled saved-peer attempt.
        fake::complete_connect(0, stale, 77);
        assert(fake::terminations.size() == 1 && fake::terminations.back() == 77);
        fake::disconnect();
        fake::run_steps(10);
        assert(!fake::status().bonded && !fake::status().connected && !fake::status().waiting);
        assert(!fake::security_calls && !fake::discovery_calls && !fake::persisted);
        fake::finish_scan();
        fake::run_steps(1);
        assert(!fake::status().scanning && fake::connects.size() == 1);
        assert(ble_remote_start_pairing(20) == ESP_OK);
        fake::run_steps(1);
        assert(fake::scans == 2 && fake::status().pairing && fake::status().scanning);
    } else if (scenario == "cancel-error") {
        fake::run_steps(1);
        fake::cancel_error = BLE_HS_EBUSY;
        assert(ble_remote_scan(5) == ESP_OK);
        fake::run_steps(1);
        assert(fake::cancellations == 1 && !fake::scans && fake::warnings.size() == 1);
        fake::cancel_error = 0;
        fake::run_steps(9);
        assert(fake::cancellations == 1 && !fake::scans);
        fake::run_steps(1);
        assert(fake::cancellations == 2 && !fake::scans);
        fake::complete_connect(BLE_HS_EPREEMPTED);
        fake::run_steps(1);
        assert(fake::scans == 1 && fake::status().scanning);
    } else if (scenario == "unpair-cancel-error") {
        fake::run_steps(1);
        fake::cancel_error = BLE_HS_EBUSY;
        assert(ble_remote_unpair() == ESP_OK);
        fake::run_steps(1);
        assert(fake::status().unpairing && !fake::status().bonded && fake::storage_peer);
        assert(fake::cancellations == 1);
        fake::cancel_error = 0;
        fake::run_steps(10);
        assert(fake::cancellations == 2 && fake::status().unpairing);
        fake::complete_connect(BLE_HS_EPREEMPTED);
        fake::run_steps(1);
        assert(!fake::status().unpairing && !fake::status().waiting && fake::connects.size() == 1);
        assert(!fake::storage_peer && !fake::controller_irk && fake::removed_bonds==1);
    } else if (scenario == "unpair-before-completion") {
        fake::run_steps(1);
        assert(ble_remote_unpair() == ESP_OK);
        fake::complete_connect(); // Event races with accepted Unpair, before worker cleanup.
        assert(fake::terminations.size() == 1);
        fake::run_steps(60);
        assert(fake::connects.size() == 1 && !fake::security_calls && !fake::persisted);
        assert(!fake::status().bonded && !fake::status().waiting);
    } else if (scenario == "scan") {
        fake::run_steps(1);
        assert(ble_remote_scan(5) == ESP_OK);
        fake::run_steps(1);
        assert(fake::cancellations == 1 && !fake::scans);
        fake::complete_connect(BLE_HS_EPREEMPTED);
        fake::run_steps(1);
        assert(fake::scans == 1 && fake::status().scanning && !fake::status().waiting);
        fake::run_steps(4);
        assert(fake::connects.size() == 1);
        fake::finish_scan();
        fake::run_steps(1);
        assert(fake::connects.size() == 2 && fake::status().waiting && !fake::status().last_error);
        assert(fake::warnings.empty());
    } else if (scenario == "connect-error") {
        fake::connect_error = BLE_HS_EBUSY;
        fake::run_steps(2); // Worker consumes the start error on its next turn.
        assert(fake::connects.size() == 1 && fake::status().last_error == BLE_HS_EBUSY);
        assert(!fake::status().waiting && !fake::status().connecting);
        fake::connect_error = 0;
        fake::run_steps(9);
        assert(fake::connects.size() == 1);
        fake::run_steps(1);
        assert(fake::connects.size() == 2 && fake::status().waiting);
    } else if (scenario == "async-error") {
        fake::run_steps(1);
        fake::complete_connect(BLE_HS_EAUTHEN);
        fake::run_steps(1);
        assert(fake::status().last_error == BLE_HS_EAUTHEN && !fake::status().waiting);
        assert(!fake::security_calls && !fake::discovery_calls);
        fake::run_steps(9);
        assert(fake::connects.size() == 1);
        fake::run_steps(1);
        assert(fake::connects.size() == 2 && fake::status().waiting);
    } else if (scenario == "discovery-error") {
        fake::discovery_error = BLE_HS_ENOTSUP;
        fake::run_steps(1);
        fake::complete_connect();
        fake::run_steps(1);
        assert(fake::security_calls == 1 && fake::discovery_calls == 1);
        assert(fake::status().last_error == BLE_HS_ENOTSUP && !fake::status().connected);
        assert(fake::terminations.size() == 1 && fake::warnings.size() == 1);
        fake::disconnect();
        assert(fake::status().last_error == BLE_HS_ENOTSUP); // Cleanup must preserve setup error.
        fake::discovery_error = 0;
        fake::run_steps(9);
        assert(fake::connects.size() == 1);
        fake::run_steps(1);
        assert(fake::status().waiting && fake::connects.size() == 2);
    } else if (scenario == "disconnect-before-setup") {
        fake::run_steps(1);
        fake::complete_connect();
        fake::disconnect();
        fake::run_steps(1);
        assert(!fake::security_calls && !fake::discovery_calls);
        assert(fake::status().waiting && fake::connects.size() == 2);
    } else if (scenario == "host-reset") {
        fake::run_steps(1);
        fake::synced = false;
        fake::connecting = false; // Host reset discards controller procedures.
        fake::run_steps(1);
        assert(!fake::status().ready && !fake::status().waiting);
        fake::synced = true;
        fake::run_steps(1);
        assert(fake::status().ready && fake::status().waiting && fake::connects.size() == 2);
    } else if (scenario == "pairing") {
        fake::immediate_connect = true;
        open_peer(fake::peer);
        assert(fake::connects.size() == 1 && fake::connects[0].duration == 10000);
        assert(fake::status().connected && fake::status().bonded);
        assert(fake::security_calls == 1 && fake::discovery_calls == 1);
    } else assert(false);

    delete keyboard;
    keyboard = nullptr;
    std::cout << "BLE transport " << scenario << " passed\n";
}
