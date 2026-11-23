/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

// The consortium GATT transport's discovery, candidate connection and server
// GAP callbacks: gatt_transport.cpp built whole on the fake NimBLE host. The
// admission server is a strict seam, so no scenario here reaches its crypto.
#include "../components/consortium/gatt_transport.cpp"
#include "consortium_gatt_fixture.hpp"
#include <algorithm>
#include <functional>

namespace {

void begin() {
    install_platform();
    watch_timing_lines();
    CHECK(gatt::gatt_register() == ESP_OK);
}

// Wakes the client's waiter has not taken yet; a binary semaphore holds one.
unsigned wakes() { return unsigned(uxSemaphoreGetCount(gatt::client_done)); }
bool take_wake() { return xSemaphoreTake(gatt::client_done, 0) == pdTRUE; }

void *token(uint32_t cookie) { return reinterpret_cast<void *>(uintptr_t(cookie)); }
uint32_t cookie() {
    std::lock_guard<std::mutex> guard(gatt::client_mutex);
    return gatt::client_callback_cookie;
}

std::vector<RuntimeAdmissionCandidate> candidates(uint64_t generation, esp_err_t expected = ESP_OK) {
    std::vector<RuntimeAdmissionCandidate> found{{"stale", "stale"}};
    CHECK(gatt::gatt_client_candidates(generation, found) == expected);
    if (expected != ESP_OK) CHECK(found.empty());
    return found;
}

// The candidate procedures the client started, in order.
std::vector<std::string> steps() {
    std::vector<std::string> found;
    for (const auto &call : ble::calls())
        if (call == "ble_gap_connect" || call == "ble_gattc_exchange_mtu" || call == "ble_gattc_disc_svc_by_uuid" ||
            call == "ble_gattc_disc_chrs_by_uuid")
            found.push_back(call);
    return found;
}
const std::vector<std::string> all_steps{"ble_gap_connect", "ble_gattc_exchange_mtu", "ble_gattc_disc_svc_by_uuid",
                                         "ble_gattc_disc_chrs_by_uuid"};

// Discovers one advertiser and connects to it.
esp_err_t connect(uint64_t generation, uint8_t address = 42) {
    const auto found = discover_one(generation, advert("A-abcdef", address));
    timing_secrets.push_back(found.front().token);
    return gatt::gatt_client_connect_candidate(found.front().token, generation);
}

ble_gap_event event(uint8_t type) {
    ble_gap_event value{};
    value.type = type;
    return value;
}

// A DISC event for a report, which must outlive it.
ble_gap_event disc(const ble::Report &report) {
    auto value = event(BLE_GAP_EVENT_DISC);
    value.disc.addr = report.address;
    value.disc.data = report.data.data();
    value.disc.length_data = uint8_t(report.data.size());
    return value;
}

// Every service-UUID advertiser heard during the twenty-second scan is
// collected, labelled from its A-xxxxxx name, with an opaque token and its
// exact address; the first does not end the scan. The window's natural end
// wakes the waiter once.
void collect_candidates() {
    begin();
    ble::after(10, [] { ble::discover(advert("A-abcdef", 42)); });
    ble::after(20, [] { ble::discover(advert("A-123456", 43)); });
    const auto scan_started = now();
    std::vector<RuntimeAdmissionCandidate> found;
    CHECK(gatt::gatt_client_discover(7, found) == ESP_OK);
    CHECK(now() == scan_started + 100);
    CHECK(found.size() == 2 && found[0].label == "Amity abcdef" && found[1].label == "Amity 123456");
    CHECK(found[0].token.size() == 32 && found[0].token != found[1].token);
    CHECK(gatt::discovered.size() == 2 && gatt::discovered[0].address.val[0] == 42 &&
          gatt::discovered[1].address.val[0] == 43);
    CHECK(ble::scanning() && ble::count("ble_gap_disc_cancel") == 0 && wakes() == 0);
    const auto scan = ble::scans().front();
    CHECK(scan.duration_ms == 20000 && scan.params.passive == 0 && scan.params.filter_duplicates == 1);
    ble::discover(advert("A-a1b2c3", 44));
    CHECK(candidates(7).size() == 3);
    amity_fake::advance_ms(19900);
    CHECK(!ble::scanning() && take_wake() && gatt::client_error == 0 && !gatt::client_discovery_waiting);
    auto done = event(BLE_GAP_EVENT_DISC_COMPLETE);
    ble::deliver(scan, done);
    CHECK(wakes() == 0);
    // A report after the window ended is not collected.
    const auto report = advert("A-0d0e0f", 45);
    auto late = disc(report);
    ble::deliver(scan, late);
    CHECK(candidates(7).size() == 3);
}

// Only advertisers of the admission service count; one entry per address,
// which is its type and value; only an exact A-<six lowercase hex digits>
// name labels the candidate; at most eight.
void filter_and_bound() {
    begin();
    ble::after(10, [] {
        ble::discover(advert("A-abcdef", 40, false));
        std::array<uint8_t, 16> record{};
        std::memcpy(record.data(), gatt::record_uuid.value, record.size());
        ble::Report other{ble::address(41), ble::advertising_payload(&record, "A-abcdef")};
        ble::discover(other);
        ble::Report malformed = advert("A-abcdef", 39);
        malformed.data[0] = 0x40;
        ble::discover(malformed);
        // The name structure claims more bytes than the report holds, after
        // the service UUID parsed.
        ble::Report truncated = advert("A-abcdef", 38);
        truncated.data[21] = 0x20;
        ble::discover(truncated);
    });
    ble::after(20, [] {
        ble::discover(advert("A-abcdef", 42));
        ble::discover(advert("A-fedcba", 42));
    });
    std::vector<RuntimeAdmissionCandidate> found;
    CHECK(gatt::gatt_client_discover(7, found) == ESP_OK);
    CHECK(found.size() == 1 && found[0].label == "Amity abcdef");
    // The same value as a random address is another board.
    ble::Report random = advert("A-0a0b0c", 42);
    random.address.type = BLE_ADDR_RANDOM;
    ble::discover(random);
    found = candidates(7);
    CHECK(found.size() == 2 && found[1].label == "Amity 0a0b0c");
    // Without the flags, a nine-character name fits beside the service.
    const auto service = service_uuid();
    ble::discover({ble::address(43), ble::advertising_payload(&service, "A-abcdefa", false)});
    ble::discover(advert("<script>", 44));
    ble::discover(advert("A-ABCDEF", 45));
    ble::discover(advert("A-abcde", 46));
    ble::discover(advert("B-abcdef", 47));
    found = candidates(7);
    CHECK(found.size() == 7);
    for (size_t i = 2; i < found.size(); ++i) CHECK(found[i].label == "Amity");
    ble::discover(advert("A-abcdeg", 48));
    ble::discover(advert("A-123456", 49));
    found = candidates(7);
    CHECK(found.size() == 8 && found[7].label == "Amity" && gatt::discovered.back().address.val[0] == 48);
}

// A discovery's callbacks carry a 32-bit cookie of their own, not the radio
// generation: a new discovery fences the previous one's callbacks, and a
// generation wider than 32 bits is kept whole.
void callback_cookie() {
    begin();
    const uint64_t first = uint64_t(1) << 32 | 7, second = UINT64_MAX;
    discover_one(first, advert("A-abcdef", 42));
    const auto old_scan = ble::scans().back();
    gatt::gatt_client_disconnect();
    CHECK(take_wake());
    ble::after(10, [] { ble::discover(advert("A-123456", 43)); });
    std::vector<RuntimeAdmissionCandidate> found;
    CHECK(gatt::gatt_client_discover(second, found) == ESP_OK && found.size() == 1);
    const auto scan = ble::scans().back();
    CHECK(scan.argument == token(cookie()) && scan.argument != old_scan.argument);
    CHECK(candidates(second).size() == 1 && found[0].label == "Amity 123456");
    const auto report = advert("A-0a0b0c", 44);
    auto stale = disc(report);
    ble::deliver(old_scan, stale);
    ble_gap_event previous = stale;
    CHECK(gatt::client_gap(&previous, token(cookie() - 1)) == 0);
    CHECK(candidates(second).size() == 1 && wakes() == 0);
    ble::deliver(scan, stale);
    CHECK(candidates(second).size() == 2 && gatt::client_generation == second);
    auto done = event(BLE_GAP_EVENT_DISC_COMPLETE);
    ble::deliver(old_scan, done);
    CHECK(wakes() == 0 && gatt::client_discovery_waiting);
}

// gatt_client_disconnect cancels the scan and any connection attempt, drops
// the candidates, wakes the waiter with ENOTCONN and fences the old
// callbacks with a new cookie, leaving the radio generation alone.
void disconnect_cancels() {
    begin();
    discover_one(7, advert("A-abcdef", 42));
    const auto scan = ble::scans().back();
    const auto before = cookie();
    gatt::gatt_client_disconnect();
    CHECK(!ble::scanning() && ble::count("ble_gap_disc_cancel") == 1 && ble::count("ble_gap_conn_cancel") == 1);
    CHECK(ble::terminations().empty());
    CHECK(wakes() == 1 && gatt::client_error == BLE_HS_ENOTCONN);
    CHECK(!gatt::client_workflow && !gatt::client_discovery_waiting && gatt::client_stopping && gatt::discovered.empty());
    CHECK(cookie() == before + 1 && gatt::client_generation == 7);
    candidates(7, ESP_ERR_INVALID_STATE);
    CHECK(take_wake());
    ble::discover(advert("A-123456", 43));
    auto done = event(BLE_GAP_EVENT_DISC_COMPLETE);
    ble::deliver(scan, done);
    CHECK(wakes() == 0 && gatt::discovered.empty());
    gatt::gatt_client_disconnect();
    CHECK(wakes() == 0 && ble::count("ble_gap_disc_cancel") == 1 && cookie() == before + 1);
    // A connected workflow's link is terminated by its exact handle.
    CHECK(connect(8, 43) == ESP_OK);
    gatt::gatt_client_disconnect();
    const auto ended = ble::terminations();
    CHECK(ended.size() == 1 && ended[0].handle == 31 && ended[0].reason == BLE_ERR_REM_USER_CONN_TERM);
    CHECK(gatt::cancelled_client == 31 && gatt::client_connection == BLE_HS_CONN_HANDLE_NONE);
    CHECK(take_wake() && gatt::client_error == BLE_HS_ENOTCONN);
    // The link's DISCONNECT reaches the old callback and changes nothing.
    amity_fake::advance_ms(1);
    CHECK(!ble::linked(31) && wakes() == 0);
    gatt::gatt_client_disconnect();
    CHECK(ble::terminations().size() == 1 && wakes() == 0);
}

// Only the disconnect of the workflow's own link ends the workflow.
void own_disconnect() {
    begin();
    CHECK(connect(7) == ESP_OK);
    auto other = event(BLE_GAP_EVENT_DISCONNECT);
    other.disconnect.conn.conn_handle = 32;
    ble::link_event(31, other);
    CHECK(wakes() == 0 && gatt::client_connection == 31 && !gatt::client_stopping);
    ble::disconnect(31);
    CHECK(wakes() == 1 && gatt::client_stopping && gatt::client_error == BLE_HS_ENOTCONN);
    CHECK(gatt::client_connection == BLE_HS_CONN_HANDLE_NONE && !gatt::client_mtu_ready);
}

// The candidate's MTU is exchanged before its service and characteristic
// are discovered, for a peer that settles on 23 bytes or on 527.
void mtu_before_discovery() {
    begin();
    uint64_t generation = 7;
    for (const uint16_t mtu : {uint16_t(23), uint16_t(527)}) {
        ble::preferred_mtu(mtu);
        const auto before = steps().size();
        CHECK(connect(generation) == ESP_OK);
        auto made = steps();
        made.erase(made.begin(), made.begin() + long(before));
        CHECK(made == all_steps);
        CHECK(gatt::client_mtu_ready && !gatt::client_mtu_waiting && ble::link(31).mtu == mtu);
        CHECK(gatt::client_attribute == gatt::attribute_handle && gatt::client_attribute != 0);
        CHECK(gatt::client_service_start == ble::services().front().start_handle);
        gatt::gatt_client_disconnect();
        ++generation;
    }
}

// Each stage of the connection waits a bounded time for its completion: 11 s
// for the connection, then 5 s for the MTU, the service and the
// characteristic. A completion at the bound counts; none fails exactly there.
void bounded_stage_waits() {
    begin();
    uint64_t generation = 7;
    const auto attempt = [&](const std::function<void()> &script) {
        const auto found = discover_one(generation, advert("A-abcdef", 42));
        script();
        const auto started = now();
        const auto result = gatt::gatt_client_connect_candidate(found.front().token, generation++);
        const auto took = now() - started;
        gatt::gatt_client_disconnect();
        amity_fake::advance_ms(1);
        return std::make_pair(result, took);
    };
    // The attempt's own 10 s duration ends before the bound.
    ble::Outcome late;
    late.delay_ms = 11000;
    CHECK(attempt([&] { ble::connect_outcome(late); }).first == ESP_FAIL);
    late.delay_ms = -1;
    CHECK(attempt([&] { ble::connect_outcome(late); }) == std::make_pair(esp_err_t(ESP_FAIL), uint64_t(11000)));
    ble::Outcome at_bound;
    at_bound.delay_ms = 10000;
    CHECK(attempt([&] { ble::connect_outcome(at_bound); }).first == ESP_OK);
    for (const char *stage : {"ble_gattc_exchange_mtu", "ble_gattc_disc_svc_by_uuid", "ble_gattc_disc_chrs_by_uuid"}) {
        CHECK(attempt([&] { ble::withhold_next(stage); }) == std::make_pair(esp_err_t(ESP_FAIL), uint64_t(5000)));
        ble::att_delay(5000);
        const auto result = attempt([] {});
        ble::att_delay(0);
        CHECK(result.first == ESP_OK);
    }
}

// A peer whose database lacks the admission service, or its record
// characteristic, is not connected.
void missing_attributes() {
    begin();
    ble::empty_next("ble_gattc_disc_svc_by_uuid");
    auto calls = steps().size();
    CHECK(connect(7) == ESP_FAIL);
    auto made = steps();
    CHECK(made.size() == calls + 3 && made.back() == "ble_gattc_disc_svc_by_uuid");
    CHECK(gatt::client_service_start == 0 && gatt::client_attribute == 0);
    gatt::gatt_client_disconnect();
    amity_fake::advance_ms(1);
    ble::empty_next("ble_gattc_disc_chrs_by_uuid");
    calls = steps().size();
    CHECK(connect(8) == ESP_FAIL);
    made = steps();
    CHECK(made.size() == calls + 4 && made.back() == "ble_gattc_disc_chrs_by_uuid");
    CHECK(gatt::client_service_start != 0 && gatt::client_attribute == 0);
}

// An MTU request NimBLE reports as already sent is proven complete only by
// the exact link's ATT MTU event, whether it came before or comes after;
// a workflow cancelled meanwhile stops there.
void mtu_already_sent() {
    begin();
    uint64_t generation = 7;
    for (const bool before : {true, false})
        for (const uint16_t mtu : {uint16_t(23), uint16_t(527)}) {
            ble::on_exchange_mtu([before, mtu](uint16_t handle, ble_gatt_mtu_fn *, void *) {
                if (!before)
                    ble::after(30, [handle, mtu] {
                        ble::set_mtu(handle, mtu);
                        ble::mtu_event(handle, handle, BLE_L2CAP_CID_ATT, mtu, ble::link(handle).argument);
                    });
                return BLE_HS_EALREADY;
            });
            ble::Outcome outcome;
            if (before)
                outcome.connected = [mtu](uint16_t handle) {
                    ble::set_mtu(handle, mtu);
                    ble::mtu_event(handle, handle, BLE_L2CAP_CID_ATT, mtu, ble::link(handle).argument);
                };
            ble::connect_outcome(outcome);
            const auto calls = steps().size();
            CHECK(connect(generation++) == ESP_OK);
            CHECK(steps().size() == calls + 4);
            CHECK(gatt::client_mtu_ready && !gatt::client_mtu_waiting && gatt::client_attribute != 0);
            gatt::gatt_client_disconnect();
        }
    // A workflow cancelled while its request went out does not wait for it.
    ble::on_exchange_mtu([](uint16_t, ble_gatt_mtu_fn *, void *) {
        gatt::gatt_client_disconnect();
        return BLE_HS_EALREADY;
    });
    const auto started = now();
    CHECK(connect(generation) == ESP_FAIL);
    CHECK(now() == started + 100 && !gatt::client_mtu_waiting && !gatt::client_mtu_ready);
}

enum class MtuFailure { start_error, callback_error, invalid_mtu, no_reply, stale_cookie, wrong_connection, cancelled };

// An MTU exchange that cannot start, fails, reports an MTU below 23, never
// answers, answers a stale cookie or another link, or is cancelled, stops
// the connection before any discovery; a missing answer fails at 5 s.
void mtu_failures() {
    begin();
    uint64_t generation = 7;
    for (const auto failure : {MtuFailure::start_error, MtuFailure::callback_error, MtuFailure::invalid_mtu,
                               MtuFailure::no_reply, MtuFailure::stale_cookie, MtuFailure::wrong_connection,
                               MtuFailure::cancelled}) {
        ble::on_exchange_mtu([failure](uint16_t handle, ble_gatt_mtu_fn *callback, void *argument) {
            if (failure == MtuFailure::start_error) return BLE_HS_EINVAL;
            ble::after(40, [=] {
                ble_gatt_error error{0, 0};
                switch (failure) {
                case MtuFailure::callback_error:
                    error.status = BLE_HS_ENOTCONN;
                    callback(handle, &error, 527, argument);
                    break;
                case MtuFailure::invalid_mtu: callback(handle, &error, 22, argument); break;
                case MtuFailure::stale_cookie:
                    callback(handle, &error, 527, token(uint32_t(reinterpret_cast<uintptr_t>(argument)) - 1));
                    break;
                case MtuFailure::wrong_connection: callback(uint16_t(handle + 1), &error, 527, argument); break;
                case MtuFailure::cancelled: gatt::gatt_client_disconnect(); break;
                default: break;
                }
            });
            return 0;
        });
        const auto found = discover_one(generation, advert("A-abcdef", 42));
        const auto calls = steps().size();
        const auto started = now();
        CHECK(gatt::gatt_client_connect_candidate(found.front().token, generation++) == ESP_FAIL);
        const auto made = steps();
        CHECK(made.size() == calls + 2 && made.back() == "ble_gattc_exchange_mtu");
        CHECK(gatt::client_attribute == 0 && gatt::client_service_start == 0);
        const bool silent = failure == MtuFailure::no_reply || failure == MtuFailure::stale_cookie ||
                            failure == MtuFailure::wrong_connection;
        CHECK(now() - started == (failure == MtuFailure::start_error ? 0u : silent ? 5000u : 40u));
        gatt::gatt_client_disconnect();
        amity_fake::advance_ms(1);
    }
}

// The MTU callback completes only the current cookie's exchange on the
// workflow's own link, and needs a status; a stopping workflow ignores it.
void mtu_callback_fences() {
    begin();
    ble_gatt_mtu_fn *callback = nullptr;
    void *argument = nullptr;
    ble::on_exchange_mtu([&](uint16_t, ble_gatt_mtu_fn *mtu_callback, void *mtu_argument) {
        callback = mtu_callback;
        argument = mtu_argument;
        ble::after(25, [&] {
            const ble_gatt_error ok{0, 0};
            CHECK(callback(32, &ok, 527, argument) == BLE_HS_EDONE);
            CHECK(callback(31, &ok, 527, token(uint32_t(reinterpret_cast<uintptr_t>(argument)) - 1)) == BLE_HS_EDONE);
            CHECK(wakes() == 0 && !gatt::client_mtu_ready);
            gatt::client_stopping = true;
            CHECK(callback(31, &ok, 527, argument) == BLE_HS_EDONE);
            CHECK(wakes() == 0 && !gatt::client_mtu_ready);
            gatt::client_stopping = false;
            CHECK(callback(31, nullptr, 527, argument) == 0);
            CHECK(wakes() == 1 && gatt::client_error == BLE_HS_EINVAL && !gatt::client_mtu_ready);
        });
        return 0;
    });
    CHECK(connect(7) == ESP_FAIL);
    CHECK(callback && gatt::client_attribute == 0);
}

// While an already-sent exchange is pending, only the exact link's ATT event
// with a valid MTU completes it, once.
void mtu_event_fences() {
    begin();
    ble::on_exchange_mtu([](uint16_t handle, ble_gatt_mtu_fn *, void *) {
        ble::after(25, [handle] {
            void *argument = ble::link(handle).argument;
            void *stale = token(uint32_t(reinterpret_cast<uintptr_t>(argument)) - 1);
            ble::mtu_event(handle, uint16_t(handle + 1), BLE_L2CAP_CID_ATT, 527, argument);
            ble::mtu_event(handle, handle, BLE_L2CAP_CID_ATT, 527, stale);
            ble::mtu_event(handle, handle, 5, 527, argument);
            ble::mtu_event(handle, handle, BLE_L2CAP_CID_ATT, 22, argument);
            CHECK(wakes() == 0 && !gatt::client_mtu_ready && gatt::client_mtu_waiting);
            ble::mtu_event(handle, handle, BLE_L2CAP_CID_ATT, 23, argument);
            CHECK(wakes() == 1 && gatt::client_mtu_ready && !gatt::client_mtu_waiting);
        });
        return BLE_HS_EALREADY;
    });
    CHECK(connect(7) == ESP_OK);
    CHECK(wakes() == 0);
    ble::mtu_event(31, 31, BLE_L2CAP_CID_ATT, 527, ble::link(31).argument);
    CHECK(wakes() == 0);
}

const auto id = *amity::system::OctopusId::parse("0123456789abcdef0123456789a1b2c3");

esp_err_t open_server(uint64_t generation = 9) {
    return gatt::gatt_server_open(generation, id, [](std::string_view, std::string &) { return ESP_OK; });
}

// An open server advertises again for the rest of its window when its
// connection ends or a connection attempt fails.
void readvertise() {
    begin();
    CHECK(open_server() == ESP_OK && ble::advertising());
    auto adverts = ble::advertisements();
    CHECK(adverts.size() == 1 && adverts[0].duration_ms == 180000 && adverts[0].own_address_type == BLE_OWN_ADDR_PUBLIC);
    CHECK(adverts[0].params.conn_mode == BLE_GAP_CONN_MODE_UND && adverts[0].params.disc_mode == BLE_GAP_DISC_MODE_GEN);
    amity_fake::advance_ms(1000);
    ble::accept(41);
    amity_fake::advance_ms(2000);
    ble::disconnect(41);
    adverts = ble::advertisements();
    CHECK(adverts.size() == 2 && adverts[1].duration_ms == 177000 && ble::advertising());
    CHECK(adverts[1].argument == adverts[0].argument);
    amity_fake::advance_ms(500);
    ble::accept(42, BLE_HS_ETIMEOUT);
    adverts = ble::advertisements();
    CHECK(adverts.size() == 3 && adverts[2].duration_ms == 176500 && ble::advertising());
    CHECK(gatt::server_connection == BLE_HS_CONN_HANDLE_NONE && gatt::server_session == 1);
}

// A sealed prepared server keeps its connection, never advertises again and
// turns away any other central.
void seal_prepared() {
    begin();
    CHECK(open_server() == ESP_OK && ble::advertising());
    ble::accept(41);
    CHECK(gatt::server_connection == 41 && gatt::server_session == 1 && !ble::advertising());
    gatt::gatt_server_seal();
    CHECK(gatt::server_open && gatt::server_sealed && gatt::server_connection == 41);
    CHECK(ble::terminations().empty() && ble::count("ble_gap_adv_stop") == 0 && !ble::advertising());
    ble::disconnect(41);
    CHECK(gatt::server_connection == BLE_HS_CONN_HANDLE_NONE && ble::count("ble_gap_adv_start") == 1);
    gatt::Work queued;
    CHECK(uxQueueMessagesWaiting(gatt::work_queue) == 1 && xQueuePeek(gatt::work_queue, &queued, 0) == pdTRUE &&
          queued.close);
    ble::accept(42);
    const auto ended = ble::terminations();
    CHECK(ended.size() == 1 && ended[0].handle == 42 && ended[0].reason == BLE_ERR_REM_USER_CONN_TERM);
    CHECK(gatt::server_connection == BLE_HS_CONN_HANDLE_NONE && gatt::server_session == 1);
}

// Sealing stops a running advertisement.
void seal_stops_advertising() {
    begin();
    CHECK(open_server() == ESP_OK && ble::advertising());
    gatt::gatt_server_seal();
    CHECK(ble::count("ble_gap_adv_stop") == 1 && !ble::advertising() && gatt::server_open);
    ble::accept(43);
    CHECK(ble::terminations().size() == 1 && ble::terminations()[0].handle == 43);
    CHECK(gatt::server_connection == BLE_HS_CONN_HANDLE_NONE && ble::count("ble_gap_adv_start") == 1);
}

std::string stage_line(const char *stage, uint64_t at, uint64_t elapsed, const char *result, const char *failure,
                       uint64_t wait, unsigned mtu, unsigned interval, unsigned latency, int status) {
    return std::string("area=gatt stage=") + stage + " at_ms=" + std::to_string(at) + " elapsed_ms=" +
           std::to_string(elapsed) + " result=" + result + " failure=" + failure + " att_wait_ms=" +
           std::to_string(wait) + " mtu=" + std::to_string(mtu) + " interval_units=" + std::to_string(interval) +
           " latency=" + std::to_string(latency) + " status=" + std::to_string(status);
}

// Each connection stage logs one bounded timing line, after the transport
// locks are released, with the link's parameters and never the token.
void stage_timing() {
    begin();
    ble::Outcome outcome;
    outcome.delay_ms = 30;
    outcome.interval = 32;
    outcome.latency = 1;
    ble::connect_outcome(outcome);
    ble::att_delay(12);
    const auto found = discover_one(7, advert("A-abcdef", 42));
    timing_secrets.push_back(found.front().token);
    const auto start = now();
    CHECK(gatt::gatt_client_connect_candidate(found.front().token, 7) == ESP_OK);
    const std::vector<std::string> expected{
        stage_line("connect", start + 30, 30, "ok", "none", 30, 23, 32, 1, 0),
        stage_line("mtu", start + 42, 12, "ok", "none", 12, 527, 32, 1, 0),
        stage_line("service", start + 54, 12, "ok", "none", 12, 527, 32, 1, 0),
        stage_line("characteristic", start + 66, 12, "ok", "none", 12, 527, 32, 1, 0)};
    CHECK(timing_lines() == expected);
    gatt::gatt_client_disconnect();
    amity_fake::advance_ms(1);
    // A failed stage logs its failure and ends the connection's lines.
    ble::on_exchange_mtu([](uint16_t handle, ble_gatt_mtu_fn *callback, void *argument) {
        ble::after(12, [=] {
            const ble_gatt_error error{BLE_HS_ENOTCONN, 0};
            callback(handle, &error, 527, argument);
        });
        return 0;
    });
    const auto again = discover_one(8, advert("A-abcdef", 42));
    const auto second = now();
    CHECK(gatt::gatt_client_connect_candidate(again.front().token, 8) == ESP_FAIL);
    auto lines = timing_lines();
    CHECK(lines.size() == 6);
    CHECK(lines[4] == stage_line("connect", second, 0, "ok", "none", 0, 23, 40, 0, 0));
    CHECK(lines[5] == stage_line("mtu", second + 12, 12, "failed", "mtu", 12, 0, 0, 0, BLE_HS_ENOTCONN));
    gatt::gatt_client_disconnect();
    // A precondition failure is logged too, once its lock is released.
    const auto third = now();
    CHECK(gatt::gatt_client_connect_candidate(again.front().token, 8) == ESP_ERR_INVALID_STATE);
    lines = timing_lines();
    CHECK(lines.size() == 7 && lines[6] == stage_line("connect", third, 0, "failed", "precondition", 0, 0, 0, 0, 0));
}

// The first candidate is returned at once while the same scan goes on; later
// candidates extend the same token snapshot. A foreign generation or an
// unknown token is turned away without stopping the scan. Selecting a
// candidate stops the scan before connecting to its exact address, and the
// stopped scan's completion changes nothing.
void progressive_selection() {
    begin();
    const auto scan_started = now();
    const auto first = discover_one(7, advert("A-abcdef", 42));
    const auto old_scan = ble::scans().back();
    CHECK(now() == scan_started + 100 && ble::scanning() && gatt::client_discovery_waiting);
    CHECK(ble::count("ble_gap_disc_cancel") == 0);
    ble::after(500, [] { ble::discover(advert("A-123456", 43)); });
    amity_fake::advance_ms(400);
    CHECK(candidates(7).size() == 1);
    amity_fake::advance_ms(100);
    auto found = candidates(7);
    CHECK(found.size() == 2 && found[0].token == first[0].token && found[1].token != first[0].token);
    candidates(8, ESP_ERR_INVALID_STATE);
    CHECK(ble::scanning());
    CHECK(gatt::gatt_client_connect_candidate("unknown", 7) == ESP_ERR_INVALID_ARG && ble::scanning());
    found = candidates(7);
    const auto selected_at = now();
    CHECK(gatt::gatt_client_connect_candidate(found[1].token, 7) == ESP_OK);
    CHECK(selected_at < scan_started + 20000 && ble::count("ble_gap_disc_cancel") == 1 && !ble::scanning());
    CHECK(ble::connects().size() == 1 && ble::connects()[0].peer.val[0] == 43);
    const auto calls = ble::calls();
    const auto cancelled = std::find(calls.begin(), calls.end(), "ble_gap_disc_cancel");
    CHECK(cancelled < std::find(calls.begin(), calls.end(), "ble_gap_connect"));
    auto done = event(BLE_GAP_EVENT_DISC_COMPLETE);
    ble::deliver(old_scan, done);
    CHECK(wakes() == 0 && gatt::client_connection == 31 && gatt::client_mtu_ready);
}

// A scan that fails to stop, or a workflow cancelled while it stops, never
// starts the connection.
void scan_stop_failure() {
    begin();
    uint64_t generation = 7;
    for (const bool cancelled : {false, true}) {
        const auto first = discover_one(generation, advert("A-abcdef", 42));
        if (cancelled)
            ble::on_disc_cancel([] { gatt::gatt_client_disconnect(); });
        else
            ble::fail_next("ble_gap_disc_cancel", BLE_HS_EINVAL);
        CHECK(gatt::gatt_client_connect_candidate(first[0].token, generation++) != ESP_OK);
        CHECK(ble::count("ble_gap_connect") == 0);
        gatt::gatt_client_disconnect();
        CHECK(!ble::scanning());
    }
}

// A scan ends at its own bound; its candidates stay selectable without a
// cancellation.
void scan_natural_end() {
    begin();
    const auto first = discover_one(7, advert("A-abcdef", 42));
    amity_fake::advance_ms(19899);
    CHECK(ble::scanning());
    amity_fake::advance_ms(1);
    CHECK(!ble::scanning() && !gatt::client_discovery_waiting && ble::count("ble_gap_disc_cancel") == 0);
    CHECK(gatt::gatt_client_connect_candidate(first[0].token, 7) == ESP_OK && ble::count("ble_gap_disc_cancel") == 0);
}

// An empty window lasts its twenty seconds; a window whose completion never
// comes fails at twenty-one; a workflow cancelled during discovery returns
// no candidate.
void scan_window_bounds() {
    begin();
    std::vector<RuntimeAdmissionCandidate> found;
    auto start = now();
    CHECK(gatt::gatt_client_discover(7, found) == ESP_OK && found.empty() && now() - start == 20000);
    // A report the first window's scan delivers late is not the second's.
    const auto first = ble::scans().back();
    ble::after(500, [first] {
        const auto report = advert("A-abcdef", 42);
        auto late = disc(report);
        ble::deliver(first, late);
    });
    ble::drop_scan_completion();
    start = now();
    CHECK(gatt::gatt_client_discover(8, found) == ESP_FAIL && found.empty() && now() - start == 21000);
    CHECK(gatt::client_stopping && !gatt::client_workflow);
    ble::after(150, [] { gatt::gatt_client_disconnect(); });
    ble::after(300, [] { ble::discover(advert("A-abcdef", 42)); });
    CHECK(gatt::gatt_client_discover(9, found) == ESP_ERR_INVALID_STATE && found.empty());
    CHECK(ble::count("ble_gap_connect") == 0);
}

// Callback cookies stop at their largest value. A workflow that ended with
// the last cookie still ignores its callbacks, such as a cancelled
// connection's late CONNECT, and no workflow starts again.
void cookie_exhaustion() {
    begin();
    gatt::client_callback_cookie = UINT32_MAX - 2;
    const auto found = discover_one(7, advert("A-abcdef", 42));
    CHECK(cookie() == UINT32_MAX - 1);
    ble::Outcome slow;
    slow.delay_ms = 50;
    ble::connect_outcome(slow);
    ble::after(20, [] { gatt::gatt_client_disconnect(); });
    CHECK(gatt::gatt_client_connect_candidate(found[0].token, 7) == ESP_FAIL);
    CHECK(cookie() == UINT32_MAX && ble::count("ble_gap_conn_cancel") == 1 && wakes() == 0);
    amity_fake::advance_ms(100);
    CHECK(wakes() == 0 && gatt::client_connection == BLE_HS_CONN_HANDLE_NONE && !ble::linked(31));
    std::vector<RuntimeAdmissionCandidate> none;
    CHECK(gatt::gatt_client_discover(8, none) == ESP_ERR_INVALID_STATE && ble::scans().size() == 1);
}

}  // namespace

int main(int argc, char **argv) {
    return finish(amity_test::run(argc, argv,
                                  {{"collect-candidates", collect_candidates},
                                   {"filter-and-bound", filter_and_bound},
                                   {"callback-cookie", callback_cookie},
                                   {"disconnect-cancels", disconnect_cancels},
                                   {"own-disconnect", own_disconnect},
                                   {"mtu-before-discovery", mtu_before_discovery},
                                   {"bounded-stage-waits", bounded_stage_waits},
                                   {"missing-attributes", missing_attributes},
                                   {"mtu-already-sent", mtu_already_sent},
                                   {"mtu-failures", mtu_failures},
                                   {"mtu-callback-fences", mtu_callback_fences},
                                   {"mtu-event-fences", mtu_event_fences},
                                   {"readvertise", readvertise},
                                   {"seal-prepared", seal_prepared},
                                   {"seal-stops-advertising", seal_stops_advertising},
                                   {"stage-timing", stage_timing},
                                   {"progressive-selection", progressive_selection},
                                   {"scan-stop-failure", scan_stop_failure},
                                   {"scan-natural-end", scan_natural_end},
                                   {"scan-window-bounds", scan_window_bounds},
                                   {"cookie-exhaustion", cookie_exhaustion}}));
}
