#include "ble_remote.h"
#include "hid_keyboard.hpp"
#include "battery_status.hpp"
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
}

#if !CONFIG_BT_NIMBLE_ENABLED || !CONFIG_BT_NIMBLE_ROLE_CENTRAL
#error "BLE remote requires the ESP-IDF NimBLE central options"
#endif

namespace {
constexpr char kTag[] = "ble_remote";
constexpr size_t kMaxPeers = 16;
enum class CommandType { Scan, Pair, AutoPair, Unpair };
struct Command { CommandType type; uint32_t seconds; ble_addr_t address; };
struct Candidate {
    ble_remote_peer_t peer;
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

std::atomic<uint16_t> connection_handle{BLE_HS_CONN_HANDLE_NONE};
int64_t next_reconnect;
int64_t pairing_deadline;


class Guard {
public:
    Guard() { xSemaphoreTakeRecursive(mutex, portMAX_DELAY); }
    ~Guard() { xSemaphoreGiveRecursive(mutex); }
};

void to_public(ble_remote_peer_t &peer, const ble_addr_t &address) {
    for (size_t i = 0; i < 6; ++i) peer.address[i] = address.val[5 - i];
    peer.address_type = address.type;
}
ble_addr_t to_nimble(const uint8_t address[6], uint8_t type) {
    ble_addr_t result{};
    result.type = type;
    for (size_t i = 0; i < 6; ++i) result.val[i] = address[5 - i];
    return result;
}
bool same_address(const ble_addr_t &a, const ble_addr_t &b) {
    // Identity and ordinary addresses share the same public/random low bit.
    return (a.type & 1) == (b.type & 1) && !memcmp(a.val, b.val, 6);
}

esp_err_t persist_peer(const ble_addr_t *address) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open("ble_remote", NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    if (address) err = nvs_set_blob(handle, "peer", address, sizeof(*address));
    else {
        err = nvs_erase_key(handle, "peer");
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    }
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
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
int on_gap_event(ble_gap_event *event, void *token) {
    Guard lock;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (reinterpret_cast<uintptr_t>(token) != generation || !procedure_pending ||
            procedure_kind != Procedure::Connect) {
            if (!event->connect.status) ble_gap_terminate(event->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            break;
        }
        if (!event->connect.status) connection_handle = event->connect.conn_handle;
        finish_procedure(event->connect.status);
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
        if (event->disconnect.conn.conn_handle != connection_handle) break;
        if (!have_saved) selected = event->disconnect.conn.peer_id_addr;
        // Preserve a failed pairing's Security Manager error when its cleanup
        // disconnect arrives; it is more useful than "locally terminated".
        if (!forgetting && (state.connected || !state.last_error))
            state.last_error = event->disconnect.reason;
        if (procedure_pending) finish_procedure(BLE_HS_ENOTCONN);
        connection_handle = BLE_HS_CONN_HANDLE_NONE;
        encrypted = descriptors_ready = false;
        if (disconnect_callback) disconnect_callback(disconnect_context);
        keyboard->clear();
        siri_connected = false;
        input_reports.clear();
        battery_characteristics.clear();
        state.connected = state.connecting = false;
        state.battery_percent = -1;
        state.charging = -1;
        if (!unpair_cleanup_pending) state.unpairing = false;
        next_reconnect = esp_timer_get_time() + 3000000;
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
                if (chr.properties & BLE_GATT_CHR_PROP_WRITE_NO_RSP)
                    err = ble_gattc_write_no_rsp_flat(connection_handle, chr.val_handle, &mode, 1);
                else if (chr.properties & BLE_GATT_CHR_PROP_WRITE) err = write_attribute(chr.val_handle, &mode, 1);
                else err = 0;
                if (err) return err;
                continue;
            }
            const bool battery = uuid == 0x180f && ble::battery_characteristic(cuuid);
            if (!battery && !(uuid == 0x1812 && cuuid == 0x2a4d && (chr.properties & BLE_GATT_CHR_PROP_NOTIFY))) continue;
            if (battery) {
                { Guard lock; battery_characteristics.push_back({chr.val_handle, cuuid, bool(chr.properties & BLE_GATT_CHR_PROP_READ)}); }
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
            if (!battery) {
                if (!reference) return BLE_HS_ENOENT;
                if ((err = read_attribute(reference))) return err;
                if (read_value.size() != 2) return BLE_HS_EBADDATA;
                if (read_value[1] != 1) continue; // Only input reports produce key events.
                Guard lock;
                if (input_reports.size() == 16) return BLE_HS_ENOMEM;
                input_reports.push_back(InputReport{chr.val_handle, map_index, read_value[0]});
            }
            const uint8_t notify[] = {static_cast<uint8_t>((chr.properties & BLE_GATT_CHR_PROP_NOTIFY) ? 1 : 2), 0};
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

int discover_remote(ble::SiriGeneration advertised) {
    {
        Guard lock;
        services.clear(); input_reports.clear(); battery_characteristics.clear(); keyboard->clear();
        siri_connected = false;
        state.battery_percent = state.charging = -1;
    }
    void *token = begin_procedure();
    int err = wait_procedure(ble_gattc_disc_all_svcs(connection_handle, on_service, token));
    if (err) return err;
    const auto found_services = services;
    uint16_t product = 0, firmware = 0;
    bool have_firmware = false, apple = false;
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
            else have_firmware = ble::siri_parse_firmware(read_value.data(), read_value.size(), firmware);
        }
    }
    if (apple && (product == 0x0266 || product == 0x026d)) return BLE_HS_ENOTSUP;
    const bool supported_siri = apple && ble::siri_supported_product(product);
    if (advertised_siri && !supported_siri) return BLE_HS_ENOTSUP;
    if (supported_siri) {
        if (!have_firmware) return BLE_HS_EBADDATA;
        // Python negotiates 527 bytes for these models. This uses the existing
        // shared host; mbuf chains carry reports larger than an ACL fragment.
        if ((err = ble_att_set_preferred_mtu(527))) return err;
        token = begin_procedure();
        err = wait_procedure(ble_gattc_exchange_mtu(connection_handle, on_mtu, token));
        if (err && err != BLE_HS_EALREADY) return err;
        {
            Guard lock;
            siri_profile = ble::siri_profile(firmware);
            siri_connected = true;
            snprintf(state.peer.name, sizeof(state.peer.name), "Siri Remote, Gen %u", product == 0x0314 ? 2 : 3);
        }
        ESP_LOGI(kTag, "Siri Remote product 0x%04x firmware 0x%04x", product, firmware);
    }
    // Siri uses dynamically discovered battery and name characteristics, while
    // its input service requires Apple's firmware-specific setup, below.
    if ((err = discover_hid(found_services, supported_siri))) return err;
    if (!supported_siri) return 0;
    const uint8_t notify[] = {1, 0};
    for (size_t i = 0; i < siri_profile.notification_count; ++i)
        if ((err = write_attribute(siri_profile.notification_handles[i], notify, sizeof(notify)))) return err;
    for (size_t i = 0; i < siri_profile.init_count; ++i)
        if ((err = write_attribute(siri_profile.init_handle, siri_profile.init_values[i].data(), 2))) return err;
    // Read telemetry after enabling Apple's reports, matching Python setup.
    std::vector<BatteryCharacteristic> battery_reads;
    { Guard lock; battery_reads = battery_characteristics; }
    for (const auto &entry : battery_reads) if (!read_attribute(entry.handle)) {
        Guard lock; update_battery(entry.uuid, read_value.data(), read_value.size());
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
        return same_address(to_nimble(candidate.peer.address, candidate.peer.address_type), report.addr);
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
        to_public(found->peer, report.addr);
    }
    found->hid |= hid;
    if (siri != ble::SiriGeneration::None) found->siri = siri;
    found->connectable |= report.event_type == BLE_HCI_ADV_RPT_EVTYPE_ADV_IND ||
                          report.event_type == BLE_HCI_ADV_RPT_EVTYPE_DIR_IND;
    found->peer.rssi = report.rssi;
    if (fields.name && fields.name_len) {
        const size_t length = std::min<size_t>(fields.name_len, sizeof(found->peer.name) - 1);
        memcpy(found->peer.name, fields.name, length);
        found->peer.name[length] = 0;
    }
    if (!found->peer.name[0] && (siri == ble::SiriGeneration::Gen2 || siri == ble::SiriGeneration::Gen3))
        snprintf(found->peer.name, sizeof(found->peer.name), "Siri Remote, Gen %u",
                 siri == ble::SiriGeneration::Gen2 ? 2 : 3);
    return 0;
}

void start_scan(uint32_t seconds, bool clear_error = true) {
    ble_gap_disc_cancel();
    uint8_t public_address[6];
    int err = ble_hs_id_copy_addr(BLE_ADDR_PUBLIC, public_address, nullptr);
    if (!err) {
        ble_gap_disc_params params{};
        params.itvl = 0x80;
        params.window = 0x30;
        params.filter_duplicates = 0; // Preserve separate advertisement + scan-response data.
        err = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, seconds * 1000, &params, on_scan_event, nullptr);
    }
    Guard lock;
    state.scanning = !err;
    if (err || clear_error) state.last_error = err;
    if (err) state.pairing = false;
}

void open_peer(const ble_addr_t &address) {
    ble_gap_disc_cancel();
    ble::SiriGeneration advertised = ble::SiriGeneration::None;
    {
        Guard lock;
        if (state.unpairing) return;
        selected = address;
        have_selected = true;
        state.scanning = false;
        state.connecting = true;
        forgetting = false;
        state.last_error = 0;
        state.peer = {};
        to_public(state.peer, address);
        for (const auto &candidate : candidates)
            if (same_address(address, to_nimble(candidate.peer.address, candidate.peer.address_type)))
                advertised = candidate.siri;
    }
    // Matter regenerates its static random advertising address on startup.
    // Auto inference prefers that address, which would make a bonded keyboard
    // see a new central after every reboot. Use the controller's stable public
    // identity for HID without changing Matter's commissioning advertisement.
    uint8_t public_address[6];
    int err = ble_hs_id_copy_addr(BLE_ADDR_PUBLIC, public_address, nullptr);
    if (!err) {
        void *token = begin_procedure(Procedure::Connect);
        err = wait_procedure(ble_gap_connect(BLE_OWN_ADDR_PUBLIC, &address, 10000, nullptr,
                                            on_gap_event, token), 12000);
    }
    if (!err) {
        begin_procedure(Procedure::Security);
        err = wait_procedure(ble_gap_security_initiate(connection_handle), 30000);
    }
    if (!err) err = advertised == ble::SiriGeneration::Unsupported ? BLE_HS_ENOTSUP
                                                                 : discover_remote(advertised);
    if (!err) {
        Guard lock;
        ble_gap_conn_desc desc{};
        err = ble_gap_conn_find(connection_handle, &desc);
        if (!err && (!desc.sec_state.bonded || !desc.sec_state.encrypted)) err = BLE_HS_EAUTHEN;
        if (!err && forgetting) err = BLE_HS_EAPP;
        if (!err) err = persist_peer(&desc.peer_id_addr);
        if (!err) {
            saved = desc.peer_id_addr;
            have_saved = state.bonded = true;
            descriptors_ready = state.connected = true;
            to_public(state.peer, saved);
            ESP_LOGI(kTag, "Encrypted remote ready: %s (%u HID report characteristics)", state.peer.name,
                     static_cast<unsigned>(input_reports.size()));
        }
    }
    if (err) {
        ESP_LOGW(kTag, "Remote connection/discovery failed: %d", err);
        ble_gap_conn_cancel();
        uint16_t handle;
        {
            Guard lock;
            state.last_error = err;
            handle = connection_handle;
            // A device that bonded but failed remote setup must not consume a
            // bond slot forever or leave an identity inaccessible to Unpair.
            ble_gap_conn_desc desc{};
            if (!have_saved) {
                if (!ble_gap_conn_find(handle, &desc)) selected = desc.peer_id_addr;
                ble_store_util_delete_peer(&selected);
            }
        }
        if (handle != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(handle, BLE_ERR_REM_USER_CONN_TERM);
    }
    Guard lock;
    state.connecting = false;
    state.last_error = err;
    next_reconnect = esp_timer_get_time() + 5000000;
}

void forget_peer() {
    uint16_t handle;
    ble_addr_t old{};
    bool delete_bond;
    {
        Guard lock;
        forgetting = true;
        handle = connection_handle;
        old = have_saved ? saved : selected;
        ble_gap_conn_desc desc{};
        if (handle != BLE_HS_CONN_HANDLE_NONE && !ble_gap_conn_find(handle, &desc)) old = desc.peer_id_addr;
        delete_bond = have_saved || have_selected;
        have_saved = have_selected = false;
        state.bonded = false;
        state.battery_percent = state.charging = -1;
        state.connecting = false;
        state.scanning = false;
        state.pairing = false;
        if (disconnect_callback) disconnect_callback(disconnect_context);
        keyboard->clear();
        descriptors_ready = false;
    }
    ble_gap_disc_cancel();
    ble_gap_conn_cancel();
    if (handle != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(handle, BLE_ERR_REM_USER_CONN_TERM);
    if (delete_bond) {
        const int err = ble_store_util_delete_peer(&old);
        if (err && err != BLE_HS_ENOENT) ESP_LOGW(kTag, "Removing remote bond: %d", err);
    }
    const esp_err_t err = persist_peer(nullptr);
    Guard lock;
    state.last_error = err;
    state.peer = {};
    // The disconnect callback can arrive before or after storage cleanup.
    // Keep the operation busy until both have completed.
    unpair_cleanup_pending = false;
    if (connection_handle == BLE_HS_CONN_HANDLE_NONE) state.unpairing = false;
}

void worker(void *) {
    for (;;) {
        Command command{};
        if (xQueueReceive(commands, &command, pdMS_TO_TICKS(500)) == pdTRUE) {
            switch (command.type) {
            case CommandType::Scan: start_scan(command.seconds); break;
            case CommandType::Pair: open_peer(command.address); break;
            case CommandType::AutoPair: {
                Guard lock;
                if (forgetting) { state.pairing = false; break; }
                pairing_deadline = esp_timer_get_time() + command.seconds * 1000000LL;
                start_scan(command.seconds);
                break;
            }
            case CommandType::Unpair: forget_peer(); break;
            }
        }
        // Run connection/security/discovery on this worker, never on the host's
        // scan callback. Selection is first eligible advertisement, not RSSI rank.
        ble_addr_t automatic_peer{};
        bool pair_now = false, resume_search = false, stop_search = false;
        uint32_t remaining_seconds = 0;
        {
            Guard lock;
            if (state.pairing && !state.connecting) {
                const int64_t remaining = pairing_deadline - esp_timer_get_time();
                if (state.bonded || forgetting || remaining <= 0) {
                    state.pairing = false;
                    stop_search = state.scanning;
                    state.scanning = false;
                    if (!state.bonded && !state.last_error) state.last_error = BLE_HS_ETIMEOUT;
                } else if (connection_handle == BLE_HS_CONN_HANDLE_NONE) {
                    auto found = std::find_if(candidates.begin(), candidates.end(), [](const Candidate &c) {
                        return ble::pairing_candidate(c.supported(), c.connectable, c.attempted,
                            c.peer.rssi, CONFIG_AMITY_REMOTE_PAIRING_MIN_RSSI);
                    });
                    if (found != candidates.end()) {
                        found->attempted = true;
                        automatic_peer = to_nimble(found->peer.address, found->peer.address_type);
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
        ble_addr_t reconnect{};
        bool reconnect_now = false;
        {
            Guard lock;
            state.ready = ble_hs_synced();
            if (state.ready && have_saved && connection_handle == BLE_HS_CONN_HANDLE_NONE &&
                !state.connecting && !state.scanning && !state.pairing && !forgetting && esp_timer_get_time() >= next_reconnect) {
                reconnect = saved;
                reconnect_now = true;
            }
        }
        if (reconnect_now) open_peer(reconnect);
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
    // matches its firmware; the explicit web pairing action selects the peer.
    // Avoid IDF HID's keyboard-only default and its fixed input passkey fallback.
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    return xTaskCreate(worker, "ble_remote", 6144, nullptr, 4, nullptr) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

extern "C" esp_err_t ble_remote_scan(uint32_t seconds) {
    if (!commands) return ESP_ERR_INVALID_STATE;
    if (!seconds || seconds > 60) return ESP_ERR_INVALID_ARG;
    Guard lock;
    if (!state.ready || state.connecting || state.pairing || state.unpairing) return ESP_ERR_INVALID_STATE;
    candidates.clear();
    Command command{CommandType::Scan, seconds, {}};
    if (xQueueSend(commands, &command, 0) != pdTRUE) return ESP_ERR_TIMEOUT;
    state.scanning = true;
    return ESP_OK;
}

extern "C" esp_err_t ble_remote_start_pairing(uint32_t seconds) {
    if (!commands) return ESP_ERR_INVALID_STATE;
    if (!seconds || seconds > 60) return ESP_ERR_INVALID_ARG;
    Guard lock;
    if (!state.ready || state.connected || state.connecting || state.scanning ||
        state.pairing || state.unpairing || have_saved || connection_handle != BLE_HS_CONN_HANDLE_NONE)
        return ESP_ERR_INVALID_STATE;
    Command command{CommandType::AutoPair, seconds, {}};
    if (xQueueSend(commands, &command, 0) != pdTRUE) return ESP_ERR_TIMEOUT;
    candidates.clear();
    forgetting = false;
    state.pairing = state.scanning = true;
    state.last_error = 0;
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

extern "C" size_t ble_remote_scan_results(ble_remote_peer_t *peers, size_t capacity) {
    if (!mutex || (!peers && capacity)) return 0;
    Guard lock;
    size_t count = 0;
    for (const auto &candidate : candidates) {
        if (!candidate.supported()) continue;
        if (count < capacity) peers[count] = candidate.peer;
        ++count;
    }
    return std::min(count, capacity);
}

extern "C" esp_err_t ble_remote_pair(const uint8_t address[6], uint8_t address_type) {
    if (!commands) return ESP_ERR_INVALID_STATE;
    if (!address || address_type > 3) return ESP_ERR_INVALID_ARG;
    Guard lock;
    if (!state.ready || state.connected || state.connecting || state.pairing || state.unpairing || have_saved) return ESP_ERR_INVALID_STATE;
    const ble_addr_t peer = to_nimble(address, address_type);
    const bool found = std::any_of(candidates.begin(), candidates.end(), [&](const Candidate &candidate) {
        return candidate.supported() && same_address(peer, to_nimble(candidate.peer.address, candidate.peer.address_type));
    });
    if (!found) return ESP_ERR_NOT_FOUND;
    Command command{CommandType::Pair, 0, peer};
    if (xQueueSend(commands, &command, 0) != pdTRUE) return ESP_ERR_TIMEOUT;
    state.connecting = true;
    return ESP_OK;
}

extern "C" esp_err_t ble_remote_unpair(void) {
    if (!commands) return ESP_ERR_INVALID_STATE;
    Guard lock;
    if (state.unpairing) return ESP_ERR_INVALID_STATE;
    Command command{CommandType::Unpair, 0, {}};
    if (xQueueSend(commands, &command, 0) != pdTRUE) return ESP_ERR_TIMEOUT;
    state.unpairing = unpair_cleanup_pending = true;
    forgetting = true; // Stop input immediately while the worker closes the link.
    return ESP_OK;
}

extern "C" void ble_remote_get_status(ble_remote_status_t *out) {
    if (!out) return;
    if (!mutex) { *out = {}; out->battery_percent = 100; return; }
    Guard lock;
    *out = state;
    // Derive the hint from current scan RSSI so moving a remote closer clears
    // it. Show it even before the remote allows connections; rejected peers
    // do not cause a proximity prompt.
    out->too_far = state.pairing && state.scanning && !state.connecting &&
        std::any_of(candidates.begin(), candidates.end(), [](const Candidate &c) {
            return c.supported() && !c.attempted &&
                   c.peer.rssi < CONFIG_AMITY_REMOTE_PAIRING_MIN_RSSI;
        }) &&
        !std::any_of(candidates.begin(), candidates.end(), [](const Candidate &c) {
            return ble::pairing_candidate(c.supported(), c.connectable, c.attempted,
                c.peer.rssi, CONFIG_AMITY_REMOTE_PAIRING_MIN_RSSI);
        });
    const auto battery = ble::reported_battery(out->bonded, out->connected, {out->battery_percent, out->charging});
    out->battery_percent = battery.percent;
    out->charging = battery.charging;
}
