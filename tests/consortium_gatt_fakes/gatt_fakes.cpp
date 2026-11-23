/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

// The fake NimBLE host and CHIP BLE manager of one board. See gatt_fakes.hpp.
#include "gatt_fakes.hpp"
#include "esp_timer.h"
#include "scenario_main.hpp"
#include <algorithm>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <optional>

namespace {

using namespace amity_fake::ble;

[[noreturn]] void fail(const std::string &message) { amity_test::fail(__FILE__, __LINE__, message.c_str()); }

struct Scheduled {
    esp_timer_handle_t timer = nullptr;
    std::function<void()> action;
};

struct ScanState {
    Scan scan;
    bool running;
};

// Advertising data, decoded from each ble_gap_adv_set_fields.
struct AdvertisingData {
    uint8_t flags;
    std::vector<std::array<uint8_t, 16>> uuids128;
    bool uuids128_complete;
    std::string name;
    bool name_complete;
};

struct Host {
    std::deque<std::unique_ptr<Scheduled>> scheduled;
    std::vector<std::string> calls;
    std::map<std::string, int, std::less<>> failures;
    std::vector<std::string> withheld, emptied;
    // GATT database
    std::vector<ble_gatt_svc_def> definitions;
    std::vector<Service> services;
    bool configured = false;
    // Advertising
    std::vector<AdvertisingData> data;
    std::vector<Advertisement> advertisements;
    bool advertising = false;
    uint64_t advertising_generation = 0;
    // Scanning
    std::vector<ScanState> scans;
    bool drop_completion = false;
    std::function<void()> cancel_action;
    // Connecting
    std::vector<Connect> connects;
    Outcome outcome;
    bool connecting = false;
    uint64_t connect_generation = 0;
    // Links
    std::map<uint16_t, Link> links;
    std::vector<Termination> terminations;
    uint16_t preferred_mtu = 527;
    // GATT client
    ExchangeMtu exchange;
    int64_t att_delay = 0;
    std::deque<int64_t> next_delays;
    std::vector<std::size_t> written, read;
    std::function<void(std::string &)> edit_read;
    std::function<void(const Access &)> before, after;
};

Host &host() {
    static Host instance;
    return instance;
}

void record(const char *name) { host().calls.emplace_back(name); }

// Records a call; returns a scripted failure for it, if any.
std::optional<int> enter(const char *name) {
    record(name);
    auto &failures = host().failures;
    const auto found = failures.find(std::string_view(name));
    if (found == failures.end()) return std::nullopt;
    const int rc = found->second;
    failures.erase(found);
    return rc;
}

bool take(std::vector<std::string> &list, const char *name) {
    const auto found = std::find(list.begin(), list.end(), name);
    if (found == list.end()) return false;
    list.erase(found);
    return true;
}
bool withheld(const char *name) { return take(host().withheld, name); }
bool emptied(const char *name) { return take(host().emptied, name); }

Link *find_link(uint16_t handle) {
    const auto found = host().links.find(handle);
    return found == host().links.end() ? nullptr : &found->second;
}

ble_gap_conn_desc descriptor(const Link &link) {
    ble_gap_conn_desc description{};
    description.conn_handle = link.handle;
    description.conn_itvl = link.interval;
    description.conn_latency = link.latency;
    description.supervision_timeout = link.timeout;
    description.role = link.central ? 0 : 1;
    return description;
}

// The registered characteristic a loopback access reaches, by value handle.
const Characteristic *characteristic(uint16_t value_handle) {
    for (const auto &service : host().services)
        for (const auto &found : service.characteristics)
            if (found.value_handle == value_handle) return &found;
    return nullptr;
}

const Characteristic *only_characteristic() {
    const Characteristic *found = nullptr;
    for (const auto &service : host().services)
        for (const auto &candidate : service.characteristics) {
            if (found) fail("the GATT database has more than one characteristic");
            found = &candidate;
        }
    if (!found) fail("the GATT database has no characteristic");
    return found;
}

int access(uint16_t conn_handle, const Characteristic &target, uint8_t op, os_mbuf &packet, Access &seen) {
    seen.conn_handle = conn_handle;
    seen.op = op;
    if (op == BLE_GATT_ACCESS_OP_WRITE_CHR) seen.value.assign(reinterpret_cast<const char *>(packet.om_databuf), packet.omp_len);
    if (host().before) host().before(seen);
    ble_gatt_access_ctxt context{};
    context.op = op;
    context.om = &packet;
    context.chr = target.definition;
    seen.rc = target.definition->access_cb(conn_handle, target.value_handle, &context, target.definition->arg);
    if (op == BLE_GATT_ACCESS_OP_READ_CHR) seen.value.assign(reinterpret_cast<const char *>(packet.om_databuf), packet.omp_len);
    if (host().after) host().after(seen);
    return seen.rc;
}

os_mbuf packet(std::string_view value) {
    os_mbuf result{};
    result.om_capacity = sizeof(result.om_databuf);
    if (value.size() > result.om_capacity) fail("an ATT value larger than the fake packet buffer");
    std::memcpy(result.om_databuf, value.data(), value.size());
    result.omp_len = uint16_t(value.size());
    return result;
}

void deliver_disconnect(const Link &link, int reason) {
    if (!link.callback) return;
    ble_gap_event event{};
    event.type = BLE_GAP_EVENT_DISCONNECT;
    event.disconnect.reason = reason;
    event.disconnect.conn = descriptor(link);
    link.callback(&event, link.argument);
}

void deliver_connect(ble_gap_event_fn *callback, void *argument, int status, uint16_t handle) {
    ble_gap_event event{};
    event.type = BLE_GAP_EVENT_CONNECT;
    event.connect.status = status;
    event.connect.conn_handle = handle;
    callback(&event, argument);
}

int64_t procedure_delay() {
    auto &next = host().next_delays;
    if (next.empty()) return host().att_delay;
    const int64_t delay = next.front();
    next.pop_front();
    return delay;
}

const uint8_t own[6] = {0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6};

}  // namespace

namespace amity_fake::ble {

void after(int64_t delay_ms, std::function<void()> action) {
    if (delay_ms < 0) fail("an event cannot be scheduled in the past");
    auto item = std::make_unique<Scheduled>();
    item->action = std::move(action);
    esp_timer_create_args_t arguments{};
    arguments.callback = [](void *argument) {
        auto *scheduled = static_cast<Scheduled *>(argument);
        auto run = std::move(scheduled->action);
        scheduled->action = nullptr;
        run();
    };
    arguments.arg = item.get();
    arguments.name = "ble_host";
    if (esp_timer_create(&arguments, &item->timer) != ESP_OK ||
        esp_timer_start_once(item->timer, uint64_t(delay_ms) * 1000) != ESP_OK)
        fail("the fake BLE host could not schedule an event");
    host().scheduled.push_back(std::move(item));
}

std::vector<std::string> calls() { return host().calls; }
std::size_t count(std::string_view name) {
    return std::size_t(std::count(host().calls.begin(), host().calls.end(), name));
}
// A scripted outcome for a function that cannot have it fails the scenario,
// rather than waiting for a call that never takes it.
void fail_next(std::string_view name, int rc) {
    static const std::vector<std::string_view> failing{
        "ConfigureExtraServices", "ble_gap_adv_set_fields", "ble_gap_adv_start", "ble_gap_adv_stop",
        "ble_gap_disc", "ble_gap_disc_cancel", "ble_gap_connect", "ble_gattc_exchange_mtu",
        "ble_gattc_disc_svc_by_uuid", "ble_gattc_disc_chrs_by_uuid", "ble_gattc_write_flat", "ble_gattc_read"};
    if (std::find(failing.begin(), failing.end(), name) == failing.end())
        fail("the fake BLE host cannot fail " + std::string(name));
    host().failures[std::string(name)] = rc;
}
void withhold_next(std::string_view name) {
    static const std::vector<std::string_view> procedures{"ble_gattc_exchange_mtu", "ble_gattc_disc_svc_by_uuid",
                                                          "ble_gattc_disc_chrs_by_uuid", "ble_gattc_write_flat",
                                                          "ble_gattc_read"};
    if (std::find(procedures.begin(), procedures.end(), name) == procedures.end())
        fail("the fake BLE host cannot withhold " + std::string(name));
    host().withheld.emplace_back(name);
}
void empty_next(std::string_view name) {
    if (name != "ble_gattc_disc_svc_by_uuid" && name != "ble_gattc_disc_chrs_by_uuid")
        fail("the fake BLE host cannot empty " + std::string(name));
    host().emptied.emplace_back(name);
}

std::vector<Service> services() { return host().services; }

std::vector<Advertisement> advertisements() { return host().advertisements; }
bool advertising() { return host().advertising; }

ble_addr_t own_address() {
    ble_addr_t address{};
    std::memcpy(address.val, own, sizeof(own));
    return address;
}
ble_addr_t address(uint8_t last) {
    ble_addr_t result{};
    result.val[0] = last;
    result.val[5] = 0xc0;
    return result;
}

std::vector<uint8_t> advertising_payload(const std::array<uint8_t, 16> *service, std::string_view name, bool flags) {
    std::vector<uint8_t> data;
    if (flags) data = {2, BLE_HS_ADV_TYPE_FLAGS, BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP};
    if (service) {
        data.push_back(17);
        data.push_back(BLE_HS_ADV_TYPE_COMP_UUIDS128);
        data.insert(data.end(), service->begin(), service->end());
    }
    data.push_back(uint8_t(name.size() + 1));
    data.push_back(BLE_HS_ADV_TYPE_COMP_NAME);
    data.insert(data.end(), name.begin(), name.end());
    if (data.size() > BLE_HS_ADV_MAX_SZ) fail("advertising data longer than a legacy advertisement");
    return data;
}

Report own_report() {
    if (host().data.empty()) fail("the board has set no advertising data");
    const auto &data = host().data.back();
    return {own_address(), advertising_payload(data.uuids128.empty() ? nullptr : &data.uuids128.front(), data.name)};
}

std::vector<Scan> scans() {
    std::vector<Scan> result;
    for (const auto &state : host().scans) result.push_back(state.scan);
    return result;
}
bool scanning() {
    return std::any_of(host().scans.begin(), host().scans.end(), [](const ScanState &state) { return state.running; });
}
void discover(const Report &report) {
    for (auto &state : host().scans) {
        if (!state.running) continue;
        ble_gap_event event{};
        event.type = BLE_GAP_EVENT_DISC;
        event.disc.addr = report.address;
        event.disc.data = report.data.data();
        event.disc.length_data = uint8_t(report.data.size());
        state.scan.callback(&event, state.scan.argument);
        return;
    }
}
int deliver(const Scan &scan, ble_gap_event &event) { return scan.callback(&event, scan.argument); }
void drop_scan_completion() { host().drop_completion = true; }
void on_disc_cancel(std::function<void()> action) { host().cancel_action = std::move(action); }

std::vector<Connect> connects() { return host().connects; }
void connect_outcome(Outcome outcome) { host().outcome = outcome; }

bool linked(uint16_t handle) { return find_link(handle) != nullptr; }
Link link(uint16_t handle) {
    const Link *found = find_link(handle);
    if (!found) fail("no link with handle " + std::to_string(handle));
    return *found;
}

void accept(uint16_t handle, int status) {
    if (host().advertisements.empty()) fail("a central connected to a board that never advertised");
    const auto advertisement = host().advertisements.back();
    host().advertising = false;
    ++host().advertising_generation;
    if (!status) {
        if (find_link(handle)) fail("a link with handle " + std::to_string(handle) + " already exists");
        host().links[handle] = Link{handle, BLE_HS_CONN_HANDLE_NONE, false, BLE_ATT_MTU_DFLT, 24, 0, 400, false, false,
                                    advertisement.callback, advertisement.argument};
    }
    deliver_connect(advertisement.callback, advertisement.argument, status, status ? BLE_HS_CONN_HANDLE_NONE : handle);
}

void disconnect(uint16_t handle, int reason) {
    const Link *found = find_link(handle);
    if (!found) fail("no link with handle " + std::to_string(handle) + " to disconnect");
    const Link ended = *found;
    host().links.erase(handle);
    std::optional<Link> peer;
    if (ended.peer != BLE_HS_CONN_HANDLE_NONE && find_link(ended.peer)) {
        peer = *find_link(ended.peer);
        host().links.erase(ended.peer);
    }
    deliver_disconnect(ended, reason);
    if (peer) deliver_disconnect(*peer, BLE_HS_HCI_ERR(BLE_ERR_CONN_TERM_LOCAL));
}

int link_event(uint16_t handle, ble_gap_event &event) {
    const Link found = link(handle);
    return found.callback(&event, found.argument);
}

int mtu_event(uint16_t handle, uint16_t conn_handle, uint16_t channel, uint16_t value, void *argument) {
    const Link found = link(handle);
    ble_gap_event event{};
    event.type = BLE_GAP_EVENT_MTU;
    event.mtu.conn_handle = conn_handle;
    event.mtu.channel_id = channel;
    event.mtu.value = value;
    return found.callback(&event, argument);
}

void set_mtu(uint16_t handle, uint16_t mtu) {
    Link *found = find_link(handle);
    if (!found) fail("no link with handle " + std::to_string(handle) + " for an MTU");
    found->mtu = mtu;
    if (Link *peer = find_link(found->peer)) peer->mtu = mtu;
}

std::vector<Termination> terminations() { return host().terminations; }

void preferred_mtu(uint16_t mtu) { host().preferred_mtu = mtu; }
void on_exchange_mtu(ExchangeMtu procedure) { host().exchange = std::move(procedure); }

// As NimBLE: the request is sent once per link; the response sets the MTU,
// then the link's callback gets BLE_GAP_EVENT_MTU, then the procedure's own.
int default_exchange_mtu(uint16_t conn_handle, ble_gatt_mtu_fn *cb, void *cb_arg) {
    Link *found = find_link(conn_handle);
    if (!found) return BLE_HS_ENOTCONN;
    if (found->mtu_requested) return BLE_HS_EALREADY;
    found->mtu_requested = true;
    if (withheld("ble_gattc_exchange_mtu")) return 0;
    after(procedure_delay(), [conn_handle, cb, cb_arg] {
        const Link *now = find_link(conn_handle);
        if (!now) {
            const ble_gatt_error error{BLE_HS_ENOTCONN, 0};
            cb(conn_handle, &error, 0, cb_arg);
            return;
        }
        set_mtu(conn_handle, host().preferred_mtu);
        mtu_event(conn_handle, conn_handle, BLE_L2CAP_CID_ATT, host().preferred_mtu, now->argument);
        if (const Link *peer = find_link(now->peer))
            mtu_event(peer->handle, peer->handle, BLE_L2CAP_CID_ATT, host().preferred_mtu, peer->argument);
        const ble_gatt_error ok{0, 0};
        cb(conn_handle, &ok, host().preferred_mtu, cb_arg);
    });
    return 0;
}

void att_delay(int64_t milliseconds) { host().att_delay = milliseconds; }
void next_att_delays(std::vector<int64_t> milliseconds) {
    host().next_delays.assign(milliseconds.begin(), milliseconds.end());
}
std::vector<std::size_t> written_lengths() { return host().written; }
std::vector<std::size_t> read_lengths() { return host().read; }
void on_read_value(std::function<void(std::string &)> edit) { host().edit_read = std::move(edit); }

void before_server_access(std::function<void(const Access &)> hook) { host().before = std::move(hook); }
void after_server_access(std::function<void(const Access &)> hook) { host().after = std::move(hook); }

int server_write(uint16_t conn_handle, std::string_view value) {
    auto data = packet(value);
    Access seen{};
    return access(conn_handle, *only_characteristic(), BLE_GATT_ACCESS_OP_WRITE_CHR, data, seen);
}

int server_read(uint16_t conn_handle, std::string &value) {
    auto data = packet({});
    Access seen{};
    const int rc = access(conn_handle, *only_characteristic(), BLE_GATT_ACCESS_OP_READ_CHR, data, seen);
    host().read.push_back(data.omp_len);
    value = seen.value;
    return rc;
}

}  // namespace amity_fake::ble

// The CHIP BLE manager: extra services can be configured once, as in the SDK.
namespace chip::DeviceLayer::Internal {

CHIP_ERROR BLEManagerImpl::ConfigureExtraServices(std::vector<struct ble_gatt_svc_def> &extGattSvcs,
                                                  bool afterMatterSvc) {
    if (const auto rc = enter("ConfigureExtraServices")) return CHIP_ERROR(uint32_t(*rc));
    if (host().configured) return CHIP_ERROR_INCORRECT_STATE;
    host().configured = true;
    host().definitions = extGattSvcs;
    // As in CHIP, afterMatterSvc puts the extra services before the CHIPoBLE
    // service and its characteristics; handles follow registration order.
    uint16_t next = afterMatterSvc ? 1 : 9;
    for (const auto &definition : host().definitions) {
        if (definition.type == BLE_GATT_SVC_TYPE_END) break;
        Service service{&definition, next++, 0, {}};
        for (const ble_gatt_chr_def *chr = definition.characteristics; chr && chr->uuid; ++chr) {
            Characteristic found{chr, next, uint16_t(next + 1)};
            next = uint16_t(next + 2);
            if (chr->val_handle) *chr->val_handle = found.value_handle;
            service.characteristics.push_back(found);
        }
        service.end_handle = uint16_t(next - 1);
        host().services.push_back(service);
    }
    return CHIP_NO_ERROR;
}

BLEManagerImpl &BLEMgrImpl() {
    static BLEManagerImpl instance;
    return instance;
}

}  // namespace chip::DeviceLayer::Internal

extern "C" {

int ble_uuid_cmp(const ble_uuid_t *uuid1, const ble_uuid_t *uuid2) {
    if (uuid1->type != uuid2->type) return int(uuid1->type) - int(uuid2->type);
    switch (uuid1->type) {
    case BLE_UUID_TYPE_16:
        return int(reinterpret_cast<const ble_uuid16_t *>(uuid1)->value) -
               int(reinterpret_cast<const ble_uuid16_t *>(uuid2)->value);
    case BLE_UUID_TYPE_32: {
        const auto a = reinterpret_cast<const ble_uuid32_t *>(uuid1)->value;
        const auto b = reinterpret_cast<const ble_uuid32_t *>(uuid2)->value;
        return a < b ? -1 : a > b;
    }
    case BLE_UUID_TYPE_128:
        return std::memcmp(reinterpret_cast<const ble_uuid128_t *>(uuid1)->value,
                           reinterpret_cast<const ble_uuid128_t *>(uuid2)->value, 16);
    default: fail("ble_uuid_cmp of an unknown UUID type");
    }
}

// As NimBLE: AD structures are length, type, data; the 128-bit UUIDs go into
// the parser's own storage and the name points into the source.
int ble_hs_adv_parse_fields(struct ble_hs_adv_fields *adv_fields, const uint8_t *src, uint8_t src_len) {
    static ble_uuid128_t uuids[2];
    std::memset(adv_fields, 0, sizeof *adv_fields);
    while (src_len > 0) {
        const uint8_t total = uint8_t(src[0] + 1);
        if (total < 2) return BLE_HS_EBADDATA;
        if (src_len < total) return BLE_HS_EMSGSIZE;
        const uint8_t type = src[1];
        const uint8_t *data = src + 2;
        const uint8_t length = uint8_t(total - 2);
        if (length > BLE_HS_ADV_MAX_FIELD_SZ) return BLE_HS_EBADDATA;
        switch (type) {
        case BLE_HS_ADV_TYPE_FLAGS:
            if (length != 1) return BLE_HS_EBADDATA;
            adv_fields->flags = *data;
            break;
        case BLE_HS_ADV_TYPE_INCOMP_UUIDS128:
        case BLE_HS_ADV_TYPE_COMP_UUIDS128:
            if (length % 16 || length / 16 > 2) return BLE_HS_EBADDATA;
            for (unsigned i = 0; i < length / 16u; ++i) {
                uuids[i].u.type = BLE_UUID_TYPE_128;
                std::memcpy(uuids[i].value, data + 16 * i, 16);
            }
            adv_fields->uuids128 = uuids;
            adv_fields->num_uuids128 = uint8_t(length / 16);
            adv_fields->uuids128_is_complete = type == BLE_HS_ADV_TYPE_COMP_UUIDS128;
            break;
        case BLE_HS_ADV_TYPE_INCOMP_NAME:
        case BLE_HS_ADV_TYPE_COMP_NAME:
            adv_fields->name = data;
            adv_fields->name_len = length;
            adv_fields->name_is_complete = type == BLE_HS_ADV_TYPE_COMP_NAME;
            break;
        default: break;
        }
        src += total;
        src_len = uint8_t(src_len - total);
    }
    return 0;
}

int os_mbuf_copydata(const struct os_mbuf *m, int off, int len, void *dst) {
    if (off < 0 || len < 0 || off + len > m->omp_len) return -1;
    std::memcpy(dst, m->om_databuf + off, std::size_t(len));
    return 0;
}

int os_mbuf_append(struct os_mbuf *m, const void *data, uint16_t len) {
    if (m->omp_len + len > m->om_capacity) return OS_ENOMEM;
    std::memcpy(m->om_databuf + m->omp_len, data, len);
    m->omp_len = uint16_t(m->omp_len + len);
    return 0;
}

uint16_t ble_att_mtu(uint16_t conn_handle) {
    record("ble_att_mtu");
    const Link *found = find_link(conn_handle);
    return found ? found->mtu : 0;
}

int ble_gap_adv_set_fields(const struct ble_hs_adv_fields *rsp_fields) {
    if (const auto rc = enter("ble_gap_adv_set_fields")) return *rc;
    AdvertisingData data{rsp_fields->flags, {}, bool(rsp_fields->uuids128_is_complete), {}, bool(rsp_fields->name_is_complete)};
    for (unsigned i = 0; i < rsp_fields->num_uuids128; ++i) {
        std::array<uint8_t, 16> uuid{};
        std::memcpy(uuid.data(), rsp_fields->uuids128[i].value, 16);
        data.uuids128.push_back(uuid);
    }
    if (rsp_fields->name) data.name.assign(reinterpret_cast<const char *>(rsp_fields->name), rsp_fields->name_len);
    host().data.push_back(data);
    return 0;
}

int ble_gap_adv_start(uint8_t own_addr_type, const ble_addr_t *, int32_t duration_ms,
                      const struct ble_gap_adv_params *adv_params, ble_gap_event_fn *cb, void *cb_arg) {
    if (const auto rc = enter("ble_gap_adv_start")) return *rc;
    if (host().advertising) return BLE_HS_EALREADY;
    if (!cb || !adv_params) return BLE_HS_EINVAL;
    host().advertisements.push_back({own_addr_type, duration_ms, *adv_params, cb, cb_arg});
    host().advertising = true;
    const auto generation = ++host().advertising_generation;
    if (duration_ms != BLE_HS_FOREVER)
        after(duration_ms, [generation, cb, cb_arg] {
            if (host().advertising_generation != generation || !host().advertising) return;
            host().advertising = false;
            ble_gap_event event{};
            event.type = BLE_GAP_EVENT_ADV_COMPLETE;
            event.adv_complete.reason = BLE_HS_ETIMEOUT;
            cb(&event, cb_arg);
        });
    return 0;
}

int ble_gap_adv_stop(void) {
    if (const auto rc = enter("ble_gap_adv_stop")) return *rc;
    if (!host().advertising) return BLE_HS_EALREADY;
    host().advertising = false;
    ++host().advertising_generation;
    return 0;
}

int ble_gap_adv_active(void) {
    record("ble_gap_adv_active");
    return host().advertising;
}

int ble_gap_disc(uint8_t own_addr_type, int32_t duration_ms, const struct ble_gap_disc_params *disc_params,
                 ble_gap_event_fn *cb, void *cb_arg) {
    if (const auto rc = enter("ble_gap_disc")) return *rc;
    if (scanning()) return BLE_HS_EALREADY;
    if (!cb || !disc_params) return BLE_HS_EINVAL;
    host().scans.push_back({{own_addr_type, duration_ms, *disc_params, cb, cb_arg}, true});
    const std::size_t index = host().scans.size() - 1;
    after(duration_ms, [index] {
        auto &state = host().scans[index];
        if (!state.running) return;
        state.running = false;
        if (host().drop_completion) {
            host().drop_completion = false;
            return;
        }
        ble_gap_event event{};
        event.type = BLE_GAP_EVENT_DISC_COMPLETE;
        event.disc_complete.reason = 0;
        state.scan.callback(&event, state.scan.argument);
    });
    return 0;
}

int ble_gap_disc_cancel(void) {
    record("ble_gap_disc_cancel");
    if (host().cancel_action) {
        auto action = std::move(host().cancel_action);
        host().cancel_action = nullptr;
        action();
    }
    auto &failures = host().failures;
    if (const auto found = failures.find(std::string_view("ble_gap_disc_cancel")); found != failures.end()) {
        const int rc = found->second;
        failures.erase(found);
        return rc;
    }
    for (auto &state : host().scans) {
        if (!state.running) continue;
        state.running = false;
        return 0;
    }
    return BLE_HS_EALREADY;
}

int ble_gap_disc_active(void) {
    record("ble_gap_disc_active");
    return scanning();
}

int ble_gap_connect(uint8_t own_addr_type, const ble_addr_t *peer_addr, int32_t duration_ms,
                    const struct ble_gap_conn_params *params, ble_gap_event_fn *cb, void *cb_arg) {
    if (const auto rc = enter("ble_gap_connect")) return *rc;
    if (host().connecting) return BLE_HS_EALREADY;
    if (scanning()) return BLE_HS_EBUSY;
    if (!peer_addr || !params || !cb) return BLE_HS_EINVAL;
    host().connects.push_back({own_addr_type, *peer_addr, duration_ms, *params, cb, cb_arg});
    host().connecting = true;
    const auto generation = ++host().connect_generation;
    const Outcome outcome = host().outcome;
    host().outcome = Outcome{};
    if (outcome.delay_ms < 0) return 0;  // the controller never reports
    const bool loopback = std::memcmp(peer_addr->val, own, sizeof(own)) == 0;
    const bool timed_out = outcome.delay_ms > duration_ms;
    const ble_gap_conn_params chosen = *params;
    after(timed_out ? duration_ms : outcome.delay_ms, [=] {
        if (host().connect_generation != generation || !host().connecting) return;
        host().connecting = false;
        if (timed_out || outcome.status) {
            deliver_connect(cb, cb_arg, timed_out ? BLE_HS_ETIMEOUT : outcome.status, BLE_HS_CONN_HANDLE_NONE);
            return;
        }
        if (find_link(outcome.handle)) fail("a link with handle " + std::to_string(outcome.handle) + " already exists");
        const uint16_t interval = uint16_t(outcome.interval >= 0 ? outcome.interval : chosen.itvl_max);
        const uint16_t latency = uint16_t(outcome.latency >= 0 ? outcome.latency : chosen.latency);
        host().links[outcome.handle] = Link{outcome.handle, BLE_HS_CONN_HANDLE_NONE, true, BLE_ATT_MTU_DFLT, interval,
                                            latency, chosen.supervision_timeout, false, false, cb, cb_arg};
        if (loopback) {
            if (!host().advertising) fail("a loopback connection to a board that is not advertising");
            accept(loopback_handle);
            host().links[outcome.handle].peer = loopback_handle;
            host().links[loopback_handle].peer = outcome.handle;
            host().links[loopback_handle].interval = interval;
            host().links[loopback_handle].latency = latency;
        }
        deliver_connect(cb, cb_arg, 0, outcome.handle);
        if (outcome.connected) outcome.connected(outcome.handle);
    });
    return 0;
}

int ble_gap_conn_cancel(void) {
    record("ble_gap_conn_cancel");
    if (!host().connecting) return BLE_HS_EALREADY;
    host().connecting = false;
    const auto connect = host().connects.back();
    after(0, [connect] { deliver_connect(connect.callback, connect.argument, BLE_HS_EAPP, BLE_HS_CONN_HANDLE_NONE); });
    return 0;
}

int ble_gap_conn_active(void) {
    record("ble_gap_conn_active");
    return host().connecting;
}

// The link stays until its DISCONNECT, which both ends get at the next turn.
int ble_gap_terminate(uint16_t conn_handle, uint8_t hci_reason) {
    record("ble_gap_terminate");
    Link *found = find_link(conn_handle);
    if (!found) return BLE_HS_ENOTCONN;
    if (found->terminating) return BLE_HS_EALREADY;
    found->terminating = true;
    host().terminations.push_back({conn_handle, hci_reason});
    after(0, [conn_handle, hci_reason] {
        const Link *ended = find_link(conn_handle);
        if (!ended) return;
        const Link local = *ended;
        host().links.erase(conn_handle);
        std::optional<Link> peer;
        if (const Link *other = find_link(local.peer)) {
            peer = *other;
            host().links.erase(local.peer);
        }
        deliver_disconnect(local, BLE_HS_HCI_ERR(BLE_ERR_CONN_TERM_LOCAL));
        if (peer) deliver_disconnect(*peer, BLE_HS_HCI_ERR(hci_reason));
    });
    return 0;
}

int ble_gap_conn_find(uint16_t handle, struct ble_gap_conn_desc *out_desc) {
    record("ble_gap_conn_find");
    const Link *found = find_link(handle);
    if (!found) return BLE_HS_ENOTCONN;
    if (out_desc) *out_desc = descriptor(*found);
    return 0;
}

int ble_gattc_exchange_mtu(uint16_t conn_handle, ble_gatt_mtu_fn *cb, void *cb_arg) {
    if (const auto rc = enter("ble_gattc_exchange_mtu")) return *rc;
    if (host().exchange) return host().exchange(conn_handle, cb, cb_arg);
    return default_exchange_mtu(conn_handle, cb, cb_arg);
}

// Discovery by UUID reports each match, then BLE_HS_EDONE; a callback that
// returns non-zero for a match ends the procedure, as in NimBLE.
int ble_gattc_disc_svc_by_uuid(uint16_t conn_handle, const ble_uuid_t *uuid, ble_gatt_disc_svc_fn *cb, void *cb_arg) {
    if (const auto rc = enter("ble_gattc_disc_svc_by_uuid")) return *rc;
    if (!find_link(conn_handle)) return BLE_HS_ENOTCONN;
    if (withheld("ble_gattc_disc_svc_by_uuid")) return 0;
    const bool none = emptied("ble_gattc_disc_svc_by_uuid");
    after(procedure_delay(), [conn_handle, uuid, cb, cb_arg, none] {
        for (const auto &service : host().services) {
            if (none || ble_uuid_cmp(service.definition->uuid, uuid) != 0) continue;
            ble_gatt_svc found{};
            found.start_handle = service.start_handle;
            found.end_handle = service.end_handle;
            std::memcpy(&found.uuid, service.definition->uuid, sizeof(ble_uuid128_t));
            const ble_gatt_error ok{0, 0};
            if (cb(conn_handle, &ok, &found, cb_arg)) return;
        }
        const ble_gatt_error done{BLE_HS_EDONE, 0};
        cb(conn_handle, &done, nullptr, cb_arg);
    });
    return 0;
}

int ble_gattc_disc_chrs_by_uuid(uint16_t conn_handle, uint16_t start_handle, uint16_t end_handle,
                                const ble_uuid_t *uuid, ble_gatt_chr_fn *cb, void *cb_arg) {
    if (const auto rc = enter("ble_gattc_disc_chrs_by_uuid")) return *rc;
    if (!find_link(conn_handle)) return BLE_HS_ENOTCONN;
    if (withheld("ble_gattc_disc_chrs_by_uuid")) return 0;
    const bool none = emptied("ble_gattc_disc_chrs_by_uuid");
    after(procedure_delay(), [conn_handle, start_handle, end_handle, uuid, cb, cb_arg, none] {
        for (const auto &service : host().services)
            for (const auto &found : service.characteristics) {
                if (none || found.definition_handle < start_handle || found.value_handle > end_handle ||
                    ble_uuid_cmp(found.definition->uuid, uuid) != 0)
                    continue;
                ble_gatt_chr reported{};
                reported.def_handle = found.definition_handle;
                reported.val_handle = found.value_handle;
                reported.properties = uint8_t(found.definition->flags);
                std::memcpy(&reported.uuid, found.definition->uuid, sizeof(ble_uuid128_t));
                const ble_gatt_error ok{0, 0};
                if (cb(conn_handle, &ok, &reported, cb_arg)) return;
            }
        const ble_gatt_error done{BLE_HS_EDONE, 0};
        cb(conn_handle, &done, nullptr, cb_arg);
    });
    return 0;
}

// A loopback write reaches the board's own server; NimBLE truncates the
// request to the ATT MTU, three bytes of which are the opcode and handle.
int ble_gattc_write_flat(uint16_t conn_handle, uint16_t attr_handle, const void *data, uint16_t data_len,
                         ble_gatt_attr_fn *cb, void *cb_arg) {
    if (const auto rc = enter("ble_gattc_write_flat")) return *rc;
    const Link *found = find_link(conn_handle);
    if (!found) return BLE_HS_ENOTCONN;
    host().written.push_back(data_len);
    const std::size_t limit = found->mtu > 3 ? std::size_t(found->mtu - 3) : 0;
    const std::string value(static_cast<const char *>(data), std::min<std::size_t>(data_len, limit));
    if (withheld("ble_gattc_write_flat")) return 0;
    after(procedure_delay(), [conn_handle, attr_handle, value, cb, cb_arg] {
        int status = BLE_HS_ATT_ERR(BLE_ATT_ERR_INVALID_HANDLE);
        const Link *now = find_link(conn_handle);
        if (!now) status = BLE_HS_ENOTCONN;
        else if (const Characteristic *target = characteristic(attr_handle); target && now->peer != BLE_HS_CONN_HANDLE_NONE) {
            auto request = packet(value);
            Access seen{};
            status = BLE_HS_ATT_ERR(access(now->peer, *target, BLE_GATT_ACCESS_OP_WRITE_CHR, request, seen));
        }
        const ble_gatt_error error{uint16_t(status), attr_handle};
        ble_gatt_attr attribute{attr_handle, 0, nullptr};
        cb(conn_handle, &error, &attribute, cb_arg);
    });
    return 0;
}

// A loopback read reaches the board's own server; NimBLE truncates the
// response to the ATT MTU, one byte of which is the opcode.
int ble_gattc_read(uint16_t conn_handle, uint16_t attr_handle, ble_gatt_attr_fn *cb, void *cb_arg) {
    if (const auto rc = enter("ble_gattc_read")) return *rc;
    if (!find_link(conn_handle)) return BLE_HS_ENOTCONN;
    if (withheld("ble_gattc_read")) return 0;
    after(procedure_delay(), [conn_handle, attr_handle, cb, cb_arg] {
        const Link *now = find_link(conn_handle);
        const Characteristic *target = characteristic(attr_handle);
        if (!now || !target || now->peer == BLE_HS_CONN_HANDLE_NONE) {
            const ble_gatt_error error{uint16_t(now ? BLE_HS_ATT_ERR(BLE_ATT_ERR_INVALID_HANDLE) : BLE_HS_ENOTCONN), attr_handle};
            cb(conn_handle, &error, nullptr, cb_arg);
            return;
        }
        auto response = packet({});
        Access seen{};
        const int rc = access(now->peer, *target, BLE_GATT_ACCESS_OP_READ_CHR, response, seen);
        host().read.push_back(response.omp_len);
        if (rc) {
            const ble_gatt_error error{uint16_t(BLE_HS_ATT_ERR(rc)), attr_handle};
            cb(conn_handle, &error, nullptr, cb_arg);
            return;
        }
        std::string value(reinterpret_cast<const char *>(response.om_databuf), response.omp_len);
        if (now->mtu > 1 && value.size() > std::size_t(now->mtu - 1)) value.resize(now->mtu - 1);
        if (host().edit_read) host().edit_read(value);
        auto received = packet(value);
        const ble_gatt_error ok{0, attr_handle};
        ble_gatt_attr attribute{attr_handle, 0, &received};
        cb(conn_handle, &ok, &attribute, cb_arg);
    });
    return 0;
}

}  // extern "C"
