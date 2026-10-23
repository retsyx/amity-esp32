/*
 * Copyright 2026.
 * This file is part of Amity-esp32, licensed under GPL-3.0-or-later.
 */
#include <cassert>
#include <climits>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <utility>
#include <vector>
extern "C" {
#include "amity_hap_input.h"
#include "esp_hap_database.h"
#include "esp_hap_main.h"
#include "esp_hap_pairings.h"
hap_priv_t hap_priv;
}

namespace {
int saves, removals, events, timer_starts, closes;
using Bytes = std::vector<uint8_t>;

void reset() {
    std::memset(&hap_priv, 0, sizeof(hap_priv));
    auto &admin = hap_priv.controllers[0];
    std::strcpy(admin.info.id, "admin");
    admin.info.perms = 1;
    admin.valid = true;
    saves = removals = events = timer_starts = closes = 0;
}

void tlv(Bytes &bytes, uint8_t type, const Bytes &value) {
    assert(value.size() <= 255);
    bytes.push_back(type);
    bytes.push_back(static_cast<uint8_t>(value.size()));
    bytes.insert(bytes.end(), value.begin(), value.end());
}

Bytes bytes(const std::string &text) { return Bytes(text.begin(), text.end()); }

Bytes request(uint8_t method, const Bytes &id, const Bytes &key = Bytes(ED_KEY_LEN, 0x42),
              const Bytes &permissions = {0}, const Bytes &state = {STATE_M1},
              bool include_id = true, bool include_key = true, bool include_permissions = true) {
    Bytes input;
    tlv(input, kTLVType_State, state);
    tlv(input, kTLVType_Method, {method});
    if (include_id) tlv(input, kTLVType_Identifier, id);
    if (method == HAP_METHOD_ADD_PAIRING) {
        if (include_key) tlv(input, kTLVType_PublicKey, key);
        if (include_permissions) tlv(input, kTLVType_Permissions, permissions);
    }
    return input;
}

void process(const Bytes &input, bool success, bool authorized = true) {
    uint8_t buffer[2048]{};
    assert(input.size() <= sizeof(buffer));
    std::memcpy(buffer, input.data(), input.size());
    hap_secure_session_t session{};
    session.state = authorized ? STATE_VERIFIED : STATE_M2;
    session.ctrl = &hap_priv.controllers[0];
    const hap_priv_t before = hap_priv;
    const int changes = saves + removals;
    int outlen = 0;
    const int result = hap_pairings_process(&session, buffer, static_cast<int>(input.size()),
                                           sizeof(buffer), &outlen);
    assert((result == HAP_SUCCESS) == success);
    assert(outlen > 0 && outlen <= static_cast<int>(sizeof(buffer)));
    uint8_t state = 0, error = 0;
    assert(get_value_from_tlv(buffer, outlen, kTLVType_State, &state, 1) == 1);
    assert(state == STATE_M2);
    if (success) {
        assert(get_value_from_tlv(buffer, outlen, kTLVType_Error, &error, 1) < 0);
    } else {
        assert(get_value_from_tlv(buffer, outlen, kTLVType_Error, &error, 1) == 1);
        assert(std::memcmp(&before, &hap_priv, sizeof(hap_priv)) == 0);
        assert(saves + removals == changes);
    }
}

void identifier_tests() {
    reset();
    for (const std::string &id : {std::string("controller"), std::string(HAP_CTRL_ID_LEN - 1, 'a')}) {
        process(request(HAP_METHOD_ADD_PAIRING, bytes(id)), true);
        auto *controller = hap_get_controller(const_cast<char *>(id.c_str()));
        assert(controller && std::strlen(controller->info.id) == id.size());
        for (uint8_t value : controller->info.ltpk) assert(value == 0x42);
        assert(controller->info.perms == 0);
        process(request(HAP_METHOD_REMOVE_PAIRING, bytes(id)), true);
        assert(!hap_get_controller(const_cast<char *>(id.c_str())));
    }
    for (uint8_t method : {HAP_METHOD_ADD_PAIRING, HAP_METHOD_REMOVE_PAIRING}) {
        for (const Bytes &id : {Bytes{}, Bytes(HAP_CTRL_ID_LEN, 'a'), Bytes(HAP_CTRL_ID_LEN + 1, 'b'),
                               Bytes{'a', 0, 'b'}, Bytes{0}}) {
            process(request(method, id), false);
        }
        process(request(method, {}, Bytes(ED_KEY_LEN, 0x42), {0}, {STATE_M1}, false), false);
    }
    assert(hap_priv.controllers[0].valid);
    process(request(HAP_METHOD_REMOVE_PAIRING, bytes("not-present")), true);
    process(request(HAP_METHOD_LIST_PAIRINGS, {}, {}, {}, {STATE_M1}, false), true);
}

void field_tests() {
    reset();
    for (int length : {0, 1, ED_KEY_LEN - 1, ED_KEY_LEN + 1}) {
        process(request(HAP_METHOD_ADD_PAIRING, bytes("controller"), Bytes(length, 0x42)), false);
    }
    process(request(HAP_METHOD_ADD_PAIRING, bytes("controller"), {}, {0}, {STATE_M1}, true, false), false);
    for (const Bytes &perms : {Bytes{}, Bytes{0, 1}, Bytes{2}, Bytes{0xff}}) {
        process(request(HAP_METHOD_ADD_PAIRING, bytes("controller"), Bytes(ED_KEY_LEN, 0x42), perms), false);
    }
    process(request(HAP_METHOD_ADD_PAIRING, bytes("controller"), Bytes(ED_KEY_LEN, 0x42), {},
                    {STATE_M1}, true, true, false), false);
    for (const Bytes &state : {Bytes{}, Bytes{STATE_M1, STATE_M1}, Bytes{STATE_M2}}) {
        process(request(HAP_METHOD_ADD_PAIRING, bytes("controller"), Bytes(ED_KEY_LEN, 0x42), {0}, state), false);
    }
    for (const Bytes &method : {Bytes{}, Bytes{HAP_METHOD_ADD_PAIRING, HAP_METHOD_REMOVE_PAIRING}, Bytes{0xff}}) {
        Bytes input;
        tlv(input, kTLVType_State, {STATE_M1});
        tlv(input, kTLVType_Method, method);
        process(input, false);
    }
    process(request(HAP_METHOD_ADD_PAIRING, bytes("controller")), false, false);
    auto truncated = request(HAP_METHOD_ADD_PAIRING, bytes("controller"));
    truncated.pop_back();
    process(truncated, false);
    process(request(HAP_METHOD_ADD_PAIRING, bytes("controller"), Bytes(ED_KEY_LEN, 0x42), {1}), true);
    auto *controller = hap_get_controller(const_cast<char *>("controller"));
    assert(controller && controller->info.perms == 1);
    process(request(HAP_METHOD_ADD_PAIRING, bytes("controller"), Bytes(ED_KEY_LEN, 0x42), {0}), true);
    assert(controller->info.perms == 0);
    process(request(HAP_METHOD_ADD_PAIRING, bytes("controller"), Bytes(ED_KEY_LEN, 0x43), {1}), false);
}

bool characteristic_list(const char *text, std::vector<std::pair<int, int>> *pairs = nullptr) {
    const char *cursor = text;
    int aid = 0, iid = 0, result, count = 0;
    while ((result = hap_parse_characteristic_id(&cursor, &aid, &iid)) > 0) {
        ++count;
        if (pairs) pairs->emplace_back(aid, iid);
    }
    return result == 0 && count > 0;
}

void characteristic_tests() {
    std::vector<std::pair<int, int>> pairs;
    assert(characteristic_list("1.2,3.4,2147483647.2147483647", &pairs));
    assert((pairs == std::vector<std::pair<int, int>>{{1, 2}, {3, 4}, {INT_MAX, INT_MAX}}));
    assert(characteristic_list("01.002"));
    for (const char *text : {"", "1", "1.", ".1", ".", "0.1", "1.0", "1.2,", ",1.2",
                             "1.2,,3.4", "1.2,3", "1.2.3", "1.2x", "1.2  ", " 1.2", "1.-2",
                             "+1.2", "-1.2", "1.+2", "2147483648.1", "1.2147483648",
                             "9999999999999999999999999999.1", "1.2,3.4x"}) {
        assert(!characteristic_list(text));
    }
    assert(!characteristic_list(nullptr));
    const char *cursor = "1.2";
    int value;
    assert(hap_parse_characteristic_id(nullptr, &value, &value) == -1);
    assert(hap_parse_characteristic_id(&cursor, nullptr, &value) == -1);

    std::mt19937 random(0x484150);
    constexpr char alphabet[] = "0123456789.,+- x";
    for (int trial = 0; trial < 20000; ++trial) {
        std::string input;
        const unsigned length = random() % 80;
        for (unsigned i = 0; i < length; ++i) input += alphabet[random() % (sizeof(alphabet) - 1)];
        characteristic_list(input.c_str());
    }
}
}

extern "C" {
hap_ctrl_data_t *hap_get_controller(char *id) {
    for (auto &controller : hap_priv.controllers) {
        if (controller.valid && std::strcmp(controller.info.id, id) == 0) return &controller;
    }
    return nullptr;
}
hap_ctrl_data_t *hap_controller_get_empty_loc() {
    for (auto &controller : hap_priv.controllers) if (!controller.valid) return &controller;
    return nullptr;
}
int hap_controller_save(hap_ctrl_data_t *controller) {
    controller->valid = true;
    ++saves;
    return HAP_SUCCESS;
}
void hap_controller_remove(hap_ctrl_data_t *controller) {
    if (controller) {
        std::memset(controller, 0, sizeof(*controller));
        ++removals;
    }
}
bool is_admin_paired() {
    for (auto &controller : hap_priv.controllers) if (controller.valid && controller.info.perms == 1) return true;
    return false;
}
void hap_close_sessions_of_ctrl(hap_ctrl_data_t *controller) { if (controller) ++closes; }
int hap_send_event(int) { ++events; return HAP_SUCCESS; }
void hap_start_pairing_mode_timer() { ++timer_starts; }
}

int main() {
    characteristic_tests();
    identifier_tests();
    field_tests();
    puts("HomeKit characteristic IDs and pairing TLV validation passed");
}
