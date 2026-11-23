/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

// The HomeKit SDK functions homekit.cpp calls; see hap_fake.hpp.
#include "hap_fake.hpp"
#include "scenario_main.hpp"
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>

namespace {

using namespace amity_fake::hap;

[[noreturn]] void fail_scenario(const std::string &message) { amity_test::fail(__FILE__, __LINE__, message.c_str()); }

struct State {
    std::recursive_mutex mutex;
    std::deque<std::unique_ptr<Accessory>> accessories;
    std::deque<std::unique_ptr<Service>> services;
    std::deque<std::unique_ptr<Characteristic>> characteristics;
    std::map<std::string, unsigned> failures;
    SetupCalls calls;
    // The SDK's defaults, from esp_hap_database.c.
    hap_cfg_t configuration{4 * 1024, 7, 8, UNIQUE_SSID, 10, 10, 1200, false};
    hap_transport_t transport{};
    bool initialized = false, started = false;
    std::string setup_id;
    hap_setup_info_t setup_info{};
    int paired = 0;
    std::vector<PayloadRequest> payload_requests;
    std::string payload = "X-HM://00ABCDEFGAMTY";
};

State &state() {
    static State instance;
    return instance;
}

bool failing(const char *function) {
    auto &failures = state().failures;
    const auto found = failures.find(function);
    if (found == failures.end() || !found->second) return false;
    --found->second;
    return true;
}

// The handles are all size_t, so each kind has its own lookup.
Characteristic *characteristic_of(hap_char_t *hc) {
    if (!hc) fail_scenario("a HomeKit call on a null characteristic");
    return reinterpret_cast<Characteristic *>(hc);
}
Service *service_of(hap_serv_t *hs) {
    if (!hs) fail_scenario("a HomeKit call on a null service");
    return reinterpret_cast<Service *>(hs);
}
Accessory *accessory_of(hap_acc_t *ha) {
    if (!ha) fail_scenario("a HomeKit call on a null accessory");
    return reinterpret_cast<Accessory *>(ha);
}
hap_char_t *handle(const Characteristic &characteristic) {
    return reinterpret_cast<hap_char_t *>(const_cast<Characteristic *>(&characteristic));
}

void set_text(Characteristic &characteristic, const char *text) {
    characteristic.text = text;
    characteristic.value.s = characteristic.text.data();
}

hap_char_t *create(const char *function, char *type, uint16_t permissions, Format format, hap_val_t value) {
    std::lock_guard<std::recursive_mutex> lock(state().mutex);
    if (!type) fail_scenario("a characteristic without a type");
    if (failing(function)) return nullptr;
    auto characteristic = std::make_unique<Characteristic>();
    characteristic->uuid = type;
    characteristic->permissions = permissions;
    characteristic->format = format;
    characteristic->value = value;
    if (format == Format::string) set_text(*characteristic, value.s ? value.s : "");
    state().characteristics.push_back(std::move(characteristic));
    return handle(*state().characteristics.back());
}

}  // namespace

namespace amity_fake::hap {

uint32_t Characteristic::number() const {
    switch (format) {
    case Format::boolean: return value.b ? 1 : 0;
    case Format::uint8:
    case Format::uint32: return value.u;
    default: fail_scenario("characteristic " + uuid + " has no numeric value");
    }
}

std::vector<uint8_t> Characteristic::bytes() const {
    if (format != Format::tlv8) fail_scenario("characteristic " + uuid + " has no TLV8 value");
    return {value.t.buf, value.t.buf + value.t.buflen};
}

Characteristic &Service::characteristic(const std::string &type) const {
    Characteristic *found = nullptr;
    for (auto *characteristic : characteristics) {
        if (characteristic->uuid != type) continue;
        if (found) fail_scenario("service " + uuid + " has two characteristics of type " + type);
        found = characteristic;
    }
    if (!found) fail_scenario("service " + uuid + " has no characteristic of type " + type);
    return *found;
}

std::vector<Accessory *> accessories() {
    std::lock_guard<std::recursive_mutex> lock(state().mutex);
    std::vector<Accessory *> result;
    for (const auto &accessory : state().accessories) result.push_back(accessory.get());
    return result;
}

std::vector<Service *> services() {
    std::lock_guard<std::recursive_mutex> lock(state().mutex);
    std::vector<Service *> result;
    for (const auto &service : state().services) result.push_back(service.get());
    return result;
}

Service &service(const std::string &type, unsigned index) {
    unsigned seen = 0;
    for (auto *service : services()) {
        if (service->uuid == type && seen++ == index) return *service;
    }
    fail_scenario("no service " + type + " number " + std::to_string(index));
}

hap_status_t read(const Characteristic &characteristic) {
    if (!characteristic.service || !characteristic.service->read)
        fail_scenario("characteristic " + characteristic.uuid + " has no service read callback");
    hap_status_t status = HAP_STATUS_SUCCESS;
    hap_read_data_t data{handle(characteristic), &status};
    if (characteristic.service->read(&data, 1, nullptr, nullptr) != HAP_SUCCESS)
        fail_scenario("the read callback failed instead of reporting a status");
    return status;
}

hap_status_t write(const Characteristic &characteristic, hap_val_t value) {
    if (!characteristic.service || !characteristic.service->write)
        fail_scenario("characteristic " + characteristic.uuid + " has no service write callback");
    // HomeKit writes only characteristics with write permission.
    if (!(characteristic.permissions & HAP_CHAR_PERM_PW))
        fail_scenario("characteristic " + characteristic.uuid + " is not writable");
    hap_status_t status = HAP_STATUS_SUCCESS;
    hap_write_data_t data{};
    data.hc = handle(characteristic);
    data.val = value;
    data.status = &status;
    if (characteristic.service->write(&data, 1, nullptr, nullptr) != HAP_SUCCESS)
        fail_scenario("the write callback failed instead of reporting a status");
    return status;
}

SetupCalls setup_calls() {
    std::lock_guard<std::recursive_mutex> lock(state().mutex);
    return state().calls;
}
hap_cfg_t configuration() {
    std::lock_guard<std::recursive_mutex> lock(state().mutex);
    return state().configuration;
}
hap_transport_t transport() {
    std::lock_guard<std::recursive_mutex> lock(state().mutex);
    return state().transport;
}
std::string setup_id() {
    std::lock_guard<std::recursive_mutex> lock(state().mutex);
    return state().setup_id;
}
hap_setup_info_t setup_info() {
    std::lock_guard<std::recursive_mutex> lock(state().mutex);
    return state().setup_info;
}
void fail(const std::string &function, unsigned count) {
    std::lock_guard<std::recursive_mutex> lock(state().mutex);
    state().failures[function] = count;
}
void set_paired_controllers(int count) {
    std::lock_guard<std::recursive_mutex> lock(state().mutex);
    state().paired = count;
}
std::vector<PayloadRequest> payload_requests() {
    std::lock_guard<std::recursive_mutex> lock(state().mutex);
    return state().payload_requests;
}
std::string payload() {
    std::lock_guard<std::recursive_mutex> lock(state().mutex);
    return state().payload;
}

}  // namespace amity_fake::hap

extern "C" {

hap_char_t *hap_char_bool_create(char *type_uuid, uint16_t perms, bool val) {
    hap_val_t value{};
    value.b = val;
    return create("hap_char_bool_create", type_uuid, perms, Format::boolean, value);
}
hap_char_t *hap_char_uint8_create(char *type_uuid, uint16_t perms, uint8_t val) {
    hap_val_t value{};
    value.u = val;
    return create("hap_char_uint8_create", type_uuid, perms, Format::uint8, value);
}
hap_char_t *hap_char_uint32_create(char *type_uuid, uint16_t perms, uint32_t val) {
    hap_val_t value{};
    value.u = val;
    return create("hap_char_uint32_create", type_uuid, perms, Format::uint32, value);
}
hap_char_t *hap_char_string_create(char *type_uuid, uint16_t perms, char *val) {
    if (val && std::strlen(val) > HAP_CHAR_STRING_MAX_LEN) return nullptr;
    hap_val_t value{};
    value.s = val;
    return create("hap_char_string_create", type_uuid, perms, Format::string, value);
}
hap_char_t *hap_char_tlv8_create(char *type_uuid, uint16_t perms, hap_tlv8_val_t *val) {
    hap_val_t value{};
    if (val) value.t = *val;
    return create("hap_char_tlv8_create", type_uuid, perms, Format::tlv8, value);
}

void hap_char_int_set_constraints(hap_char_t *hc, int min, int max, int step) {
    std::lock_guard<std::recursive_mutex> lock(state().mutex);
    auto *characteristic = characteristic_of(hc);
    characteristic->constrained = true;
    characteristic->min = min;
    characteristic->max = max;
    characteristic->step = step;
}
void hap_char_string_set_maxlen(hap_char_t *hc, int maxlen) {
    std::lock_guard<std::recursive_mutex> lock(state().mutex);
    characteristic_of(hc)->maxlen = maxlen > HAP_CHAR_STRING_MAX_LEN ? HAP_CHAR_STRING_MAX_LEN : maxlen;
}

int hap_char_update_val(hap_char_t *hc, hap_val_t *val) {
    std::lock_guard<std::recursive_mutex> lock(state().mutex);
    if (!hc || !val) return HAP_FAIL;
    auto &characteristic = *characteristic_of(hc);
    ++characteristic.updates;
    bool changed = false;
    switch (characteristic.format) {
    case Format::boolean:
        changed = characteristic.value.b != val->b;
        characteristic.value.b = val->b;
        break;
    case Format::uint8:
    case Format::uint32:
        if (characteristic.constrained) {
            const uint32_t value = val->u, min = uint32_t(characteristic.min), max = uint32_t(characteristic.max);
            if (value > max || value < min) return HAP_FAIL;
            if (characteristic.step && (value - min) % uint32_t(characteristic.step)) return HAP_FAIL;
        }
        changed = characteristic.value.i != val->i;
        characteristic.value.i = val->i;
        break;
    case Format::string:
        changed = !val->s || characteristic.text != val->s;
        set_text(characteristic, val->s ? val->s : "");
        break;
    case Format::tlv8:
        characteristic.value.t = val->t;
        changed = true;
        break;
    }
    if (changed) ++characteristic.notifications;
    return HAP_SUCCESS;
}

const hap_val_t *hap_char_get_val(hap_char_t *hc) {
    std::lock_guard<std::recursive_mutex> lock(state().mutex);
    auto &characteristic = *characteristic_of(hc);
    ++characteristic.gets;
    return &characteristic.value;
}

hap_serv_t *hap_serv_create(char *type_uuid) {
    std::lock_guard<std::recursive_mutex> lock(state().mutex);
    if (!type_uuid) fail_scenario("a service without a type");
    if (failing("hap_serv_create")) return nullptr;
    state().services.push_back(std::make_unique<Service>());
    state().services.back()->uuid = type_uuid;
    return reinterpret_cast<hap_serv_t *>(state().services.back().get());
}

int hap_serv_add_char(hap_serv_t *hs, hap_char_t *hc) {
    std::lock_guard<std::recursive_mutex> lock(state().mutex);
    auto *service = service_of(hs);
    auto *characteristic = characteristic_of(hc);
    if (characteristic->service) fail_scenario("characteristic " + characteristic->uuid + " was added twice");
    if (failing("hap_serv_add_char")) return HAP_FAIL;
    characteristic->service = service;
    service->characteristics.push_back(characteristic);
    return HAP_SUCCESS;
}

void hap_serv_set_bulk_read_cb(hap_serv_t *hs, hap_serv_bulk_read_t read) {
    std::lock_guard<std::recursive_mutex> lock(state().mutex);
    service_of(hs)->read = read;
}
void hap_serv_set_write_cb(hap_serv_t *hs, hap_serv_write_t write) {
    std::lock_guard<std::recursive_mutex> lock(state().mutex);
    service_of(hs)->write = write;
}
void hap_serv_mark_primary(hap_serv_t *hs) {
    std::lock_guard<std::recursive_mutex> lock(state().mutex);
    service_of(hs)->primary = true;
}
int hap_serv_link_serv(hap_serv_t *hs, hap_serv_t *linked_serv) {
    std::lock_guard<std::recursive_mutex> lock(state().mutex);
    service_of(hs)->linked.push_back(service_of(linked_serv));
    return HAP_SUCCESS;
}

hap_acc_t *hap_acc_create(hap_acc_cfg_t *acc_cfg) {
    std::lock_guard<std::recursive_mutex> lock(state().mutex);
    if (!acc_cfg) fail_scenario("an accessory without a configuration");
    if (failing("hap_acc_create")) return nullptr;
    auto accessory = std::make_unique<Accessory>();
    const auto copy = [](const char *text) { return std::string(text ? text : ""); };
    accessory->name = copy(acc_cfg->name);
    accessory->model = copy(acc_cfg->model);
    accessory->manufacturer = copy(acc_cfg->manufacturer);
    accessory->serial = copy(acc_cfg->serial_num);
    accessory->firmware = copy(acc_cfg->fw_rev);
    accessory->hardware = copy(acc_cfg->hw_rev);
    accessory->protocol = copy(acc_cfg->pv);
    accessory->category = acc_cfg->cid;
    accessory->identify = acc_cfg->identify_routine;
    state().accessories.push_back(std::move(accessory));
    return reinterpret_cast<hap_acc_t *>(state().accessories.back().get());
}
int hap_acc_add_serv(hap_acc_t *ha, hap_serv_t *hs) {
    std::lock_guard<std::recursive_mutex> lock(state().mutex);
    auto *accessory = accessory_of(ha);
    auto *service = service_of(hs);
    if (service->accessory) fail_scenario("service " + service->uuid + " was added twice");
    service->accessory = accessory;
    accessory->services.push_back(service);
    return HAP_SUCCESS;
}
void hap_add_accessory(hap_acc_t *ha) {
    std::lock_guard<std::recursive_mutex> lock(state().mutex);
    auto *accessory = accessory_of(ha);
    if (accessory->added) fail_scenario("the accessory was added twice");
    accessory->added = true;
}

int hap_get_config(hap_cfg_t *cfg) {
    std::lock_guard<std::recursive_mutex> lock(state().mutex);
    ++state().calls.get_config;
    if (!cfg) return HAP_FAIL;
    *cfg = state().configuration;
    return HAP_SUCCESS;
}
int hap_set_config(const hap_cfg_t *cfg) {
    std::lock_guard<std::recursive_mutex> lock(state().mutex);
    ++state().calls.set_config;
    if (!cfg || failing("hap_set_config")) return HAP_FAIL;
    if (state().initialized) fail_scenario("hap_set_config after hap_init");
    state().configuration = *cfg;
    return HAP_SUCCESS;
}
int hap_init(hap_transport_t transport) {
    std::lock_guard<std::recursive_mutex> lock(state().mutex);
    ++state().calls.init;
    if (failing("hap_init")) return HAP_FAIL;
    if (state().initialized) fail_scenario("hap_init after hap_init");
    state().initialized = true;
    state().transport = transport;
    return HAP_SUCCESS;
}
int hap_start(void) {
    std::lock_guard<std::recursive_mutex> lock(state().mutex);
    ++state().calls.start;
    if (failing("hap_start")) return HAP_FAIL;
    if (state().accessories.empty() || !state().accessories.front()->added)
        fail_scenario("hap_start before the accessory was added");
    if (state().started) fail_scenario("hap_start after hap_start");
    state().started = true;
    return HAP_SUCCESS;
}
int hap_set_setup_id(const char *setup_id) {
    std::lock_guard<std::recursive_mutex> lock(state().mutex);
    ++state().calls.set_setup_id;
    if (!setup_id || failing("hap_set_setup_id")) return HAP_FAIL;
    state().setup_id = setup_id;
    return HAP_SUCCESS;
}
int hap_set_setup_info(const hap_setup_info_t *setup_info) {
    std::lock_guard<std::recursive_mutex> lock(state().mutex);
    ++state().calls.set_setup_info;
    if (!setup_info || failing("hap_set_setup_info")) return HAP_FAIL;
    state().setup_info = *setup_info;
    return HAP_SUCCESS;
}
int hap_get_paired_controller_count() {
    std::lock_guard<std::recursive_mutex> lock(state().mutex);
    if (!state().initialized) fail_scenario("hap_get_paired_controller_count before hap_init");
    return state().paired;
}
char *esp_hap_get_setup_payload(char *setup_code, char *setup_id, bool wac_support, hap_cid_t cid) {
    std::lock_guard<std::recursive_mutex> lock(state().mutex);
    ++state().calls.payloads;
    state().payload_requests.push_back({setup_code ? setup_code : "", setup_id ? setup_id : "", wac_support, cid});
    if (failing("esp_hap_get_setup_payload")) return nullptr;
    char *payload = static_cast<char *>(std::malloc(state().payload.size() + 1));
    if (!payload) fail_scenario("out of memory");
    std::memcpy(payload, state().payload.c_str(), state().payload.size() + 1);
    return payload;
}

}  // extern "C"
