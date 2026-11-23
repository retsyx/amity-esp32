/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

// Admission records over GATT: gatt_transport.cpp built whole on the fake
// NimBLE host, with the board's client exchanging records with its own server
// over a loopback link. The real access callback, work queue and worker carry
// each record; the admission channel is a transparent strict seam, so the
// records keep their bytes.
#include "../components/consortium/gatt_transport.cpp"
#include "consortium_gatt_fixture.hpp"
#include <algorithm>

namespace {

using gatt::AdmissionServer;

const auto id = *amity::system::OctopusId::parse("0123456789abcdef0123456789a1b2c3");

// What the server's channel and handler reply, and every request they got.
std::string answer = "welcome";
esp_err_t handler_result = ESP_OK;
std::vector<std::string> handled;
// The channel that completed a handshake, by construction count.
std::size_t handshaken = 0;

std::size_t channels() { return amity_seam::count("AdmissionServer::AdmissionServer"); }

void install_channel() {
    amity_seam::install<&AdmissionServer::handshake>([](AdmissionServer &, std::string_view input, std::string &output) {
        handled.emplace_back(input);
        output = answer;
        handshaken = channels();
        return true;
    });
    amity_seam::install<&AdmissionServer::authenticated>([](const AdmissionServer &) { return handshaken == channels(); });
    amity_seam::install<&AdmissionServer::decrypt>([](AdmissionServer &, std::string_view input, std::string &output) {
        output.assign(input);
        return true;
    });
    amity_seam::install<&AdmissionServer::encrypt>([](AdmissionServer &, std::string_view input, std::string &output) {
        output.assign(input);
        return true;
    });
}

// When the server's worker runs: after each request fragment it receives,
// except that the record's last fragment is held for `held` of the client's
// reads, as when the reply takes that long to prepare.
unsigned held = 0, held_reads = 0;
bool holding = false;

void schedule_worker() {
    ble::after_server_access([](const ble::Access &access) {
        if (access.op != BLE_GATT_ACCESS_OP_WRITE_CHR || access.value.size() <= 10 || access.value[0] != 1) return;
        const auto byte = [&](size_t i) { return size_t(uint8_t(access.value[i])); };
        const bool last = (byte(8) << 8 | byte(9)) + access.value.size() - 10 == (byte(6) << 8 | byte(7));
        if (last && held) {
            holding = true;
            held_reads = 0;
            return;
        }
        run_worker();
    });
    ble::before_server_access([](const ble::Access &access) {
        if (access.op != BLE_GATT_ACCESS_OP_READ_CHR || !holding) return;
        if (held_reads++ < held) return;
        holding = false;
        run_worker();
    });
}

// Connects the board's client to its own server at this MTU.
void connect(uint64_t generation, uint16_t mtu) {
    ble::preferred_mtu(mtu);
    const auto found = discover_one(generation, ble::own_report());
    CHECK(gatt::gatt_client_connect_candidate(found.front().token, generation) == ESP_OK);
    CHECK(gatt::server_connection == ble::loopback_handle && ble::link(31).mtu == mtu);
}

void begin(uint16_t mtu = 527) {
    install_platform();
    watch_timing_lines();
    install_channel();
    schedule_worker();
    CHECK(gatt::gatt_register() == ESP_OK);
    CHECK(gatt::gatt_server_open(9, id, [](std::string_view request, std::string &reply) {
              handled.emplace_back(request);
              reply = answer;
              return handler_result;
          }) == ESP_OK);
    connect(7, mtu);
}

// Runs one exchange with the server's handshake on endpoint 0.
void handshake() {
    std::string response;
    answer = "welcome";
    CHECK(gatt::gatt_client_exchange(false, "hello", response) && response == "welcome");
}

std::string pattern(size_t size, unsigned seed) {
    std::string value(size, '\0');
    for (size_t i = 0; i < size; ++i) value[i] = char((i * 7 + seed) % 251);
    return value;
}

std::size_t largest(const std::vector<std::size_t> &lengths, std::size_t from) {
    std::size_t found = 0;
    for (std::size_t i = from; i < lengths.size(); ++i) found = std::max(found, lengths[i]);
    return found;
}

std::vector<std::string> exchange_lines() {
    std::vector<std::string> found;
    for (const auto &line : timing_lines())
        if (line.rfind("area=gatt stage=exchange ", 0) == 0) found.push_back(line);
    return found;
}

struct Expected {
    uint64_t at, elapsed;
    const char *result, *failure;
    unsigned record, endpoint;
    uint64_t request, reply;
    unsigned writes, cursors, reads, pending, fragments;
    uint64_t wait, longest;
    unsigned mtu, interval;
    int status;
};

std::string exchange_line(const Expected &e) {
    return "area=gatt stage=exchange at_ms=" + std::to_string(e.at) + " elapsed_ms=" + std::to_string(e.elapsed) +
           " result=" + e.result + " failure=" + e.failure + " record=" + std::to_string(e.record) +
           " endpoint=" + std::to_string(e.endpoint) + " request_ms=" + std::to_string(e.request) +
           " reply_ms=" + std::to_string(e.reply) + " writes=" + std::to_string(e.writes) +
           " cursors=" + std::to_string(e.cursors) + " reads=" + std::to_string(e.reads) +
           " pending=" + std::to_string(e.pending) + " fragments=" + std::to_string(e.fragments) +
           " att_wait_ms=" + std::to_string(e.wait) + " att_max_ms=" + std::to_string(e.longest) +
           " mtu=" + std::to_string(e.mtu) + " interval_units=" + std::to_string(e.interval) +
           " latency=0 status=" + std::to_string(e.status);
}

// At every MTU from the default 23 to 527, a 4096-byte request and a
// 1700-byte reply cross whole and in order, every write fits the MTU and
// the 512-byte ATT value limit, and so does every read.
void mtu_framing() {
    begin(23);
    uint64_t generation = 8;
    for (const uint16_t mtu : {23, 24, 64, 247, 512, 515, 516, 517, 527}) {
        if (mtu != 23) connect(generation++, mtu);
        handshake();
        const auto request = pattern(4096, mtu);
        answer = pattern(1700, mtu + 1u);
        const auto writes = ble::written_lengths().size(), reads = ble::read_lengths().size();
        std::string response;
        CHECK(gatt::gatt_client_exchange(true, request, response));
        CHECK(response == answer && handled.back() == request);
        const auto written = largest(ble::written_lengths(), writes), read = largest(ble::read_lengths(), reads);
        CHECK(written <= 512 && written <= mtu - 3u);
        CHECK(read <= 512 && read <= mtu - 1u);
        gatt::gatt_client_disconnect();
        amity_fake::advance_ms(1);
        CHECK(!ble::linked(31) && ble::advertising());
    }
}

// One timing line per exchange, with counters and times that match the ATT
// operations: a reply published late, after two pending reads, keeps the
// cursor the client wrote before it, and only an advanced position is
// written again. The line carries no request or reply bytes.
void exchange_timing() {
    begin();
    ble::att_delay(18);
    const std::string secret = "private-password-SSID-token-certificate";
    timing_secrets = {secret};
    answer.assign(900, 'z');
    held = 2;
    unsigned record = 0;
    for (const bool encrypted : {false, true}) {
        const auto writes = ble::count("ble_gattc_write_flat"), reads = ble::count("ble_gattc_read");
        const auto lines = exchange_lines().size();
        const auto start = now();
        std::string response;
        // The request write takes 30 ms; every later ATT operation 18 ms.
        ble::next_att_delays({30});
        CHECK(gatt::gatt_client_exchange(encrypted, secret, response));
        CHECK(response == answer && handled.back() == secret);
        const auto made = exchange_lines();
        CHECK(made.size() == lines + 1);
        CHECK(made.back() == exchange_line({start + 178, 178, "ok", "none", ++record, unsigned(encrypted), 30, 148, 1, 2, 4,
                                            2, 2, 138, 30, 527, 40, 0}));
        CHECK(made.back().find(answer.substr(0, 16)) == std::string::npos);
        CHECK(ble::count("ble_gattc_write_flat") - writes == 3 && ble::count("ble_gattc_read") - reads == 4);
        CHECK(gatt::response_record == record && gatt::response_offset == 500);
    }
}

// A reply already published when the client positions its cursor resets the
// previous record's cursor before the first read.
void published_before_cursor() {
    begin();
    handshake();
    answer.assign(900, 'z');
    std::string response;
    CHECK(gatt::gatt_client_exchange(true, "first", response) && response == answer);
    CHECK(gatt::response_offset == 500);
    held = 0;
    answer.assign(900, 'y');
    const auto writes = ble::count("ble_gattc_write_flat"), reads = ble::count("ble_gattc_read");
    CHECK(gatt::gatt_client_exchange(true, "second", response) && response == answer);
    CHECK(ble::count("ble_gattc_write_flat") - writes == 3 && ble::count("ble_gattc_read") - reads == 2);
    CHECK(gatt::response_offset == 500 && gatt::response_record == 3);
}

// Checks the failed exchange's one timing line.
void check_failure(uint64_t start, const char *failure) {
    const auto lines = exchange_lines();
    CHECK(lines.size() == 2);
    const auto &line = lines.back();
    CHECK(line.rfind("area=gatt stage=exchange at_ms=" + std::to_string(now()) + " elapsed_ms=" +
                         std::to_string(now() - start) + " result=failed failure=" + failure + " ",
                     0) == 0);
}

// A request write the host refuses fails the exchange at once, untried again.
void request_write_failure() {
    begin();
    handshake();
    const auto writes = ble::count("ble_gattc_write_flat"), reads = ble::count("ble_gattc_read");
    ble::fail_next("ble_gattc_write_flat", BLE_HS_ENOMEM);
    const auto start = now();
    std::string response = "unchanged";
    CHECK(!gatt::gatt_client_exchange(true, "request", response));
    check_failure(start, "request_write");
    CHECK(exchange_lines().back().find(" status=" + std::to_string(BLE_HS_ENOMEM)) != std::string::npos);
    CHECK(ble::count("ble_gattc_write_flat") - writes == 1 && ble::count("ble_gattc_read") == reads);
}

// A request write that is never answered fails after 5 s; one the server
// rejects fails with its ATT error. Neither is tried again.
void request_wait_failure() {
    begin();
    handshake();
    auto writes = ble::count("ble_gattc_write_flat");
    const auto reads = ble::count("ble_gattc_read");
    ble::withhold_next("ble_gattc_write_flat");
    auto start = now();
    std::string response;
    CHECK(!gatt::gatt_client_exchange(true, "request", response));
    CHECK(now() - start == 5000);
    check_failure(start, "request_write");
    CHECK(ble::count("ble_gattc_write_flat") - writes == 1 && ble::count("ble_gattc_read") == reads);
    // The setup window's deadline passes; the server refuses the next write.
    amity_fake::advance_ms(180000);
    writes = ble::count("ble_gattc_write_flat");
    CHECK(!gatt::gatt_client_exchange(true, "request", response));
    const auto lines = exchange_lines();
    CHECK(lines.size() == 3 && lines.back().find(" result=failed failure=request_write ") != std::string::npos);
    CHECK(lines.back().find(" status=" + std::to_string(BLE_HS_ATT_ERR(BLE_ATT_ERR_INSUFFICIENT_AUTHEN))) !=
          std::string::npos);
    CHECK(ble::count("ble_gattc_write_flat") - writes == 1 && ble::count("ble_gattc_read") == reads);
}

// The cursor write and the read give up after 5 s, as the request write does.
void att_wait_bounds() {
    begin();
    handshake();
    for (const char *operation : {"ble_gattc_write_flat", "ble_gattc_read"}) {
        const auto start = now();
        std::string response;
        if (std::string_view(operation) == "ble_gattc_write_flat") {
            // The request write completes; the cursor write after it does not.
            ble::after_server_access([](const ble::Access &access) {
                if (access.op == BLE_GATT_ACCESS_OP_WRITE_CHR && access.value[0] == 1) {
                    run_worker();
                    ble::withhold_next("ble_gattc_write_flat");
                }
            });
            CHECK(!gatt::gatt_client_exchange(true, "request", response));
            CHECK(now() - start == 5000 && exchange_lines().back().find(" failure=cursor_write ") != std::string::npos);
        } else {
            ble::withhold_next(operation);
            CHECK(!gatt::gatt_client_exchange(true, "request", response));
            CHECK(now() - start == 5000 && exchange_lines().back().find(" failure=response_read ") != std::string::npos);
        }
        schedule_worker();
    }
}

// A reply the server rejects fails the exchange once.
void reply_rejected() {
    begin();
    handshake();
    handler_result = ESP_FAIL;
    const auto reads = ble::count("ble_gattc_read");
    const auto start = now();
    std::string response;
    CHECK(!gatt::gatt_client_exchange(true, "request", response));
    check_failure(start, "response_rejected");
    CHECK(ble::count("ble_gattc_read") - reads == 1 && handled.back() == "request");
}

// A reply whose framing does not continue at the client's cursor fails the
// exchange once.
void reply_malformed() {
    begin();
    handshake();
    ble::on_read_value([](std::string &value) { value[10] = 1; });
    const auto reads = ble::count("ble_gattc_read");
    const auto start = now();
    std::string response;
    CHECK(!gatt::gatt_client_exchange(true, "request", response));
    check_failure(start, "response_frame");
    CHECK(ble::count("ble_gattc_read") - reads == 1);
}

// A reply that is never published fails the exchange 45 s after its request
// was written, before 50 s.
void reply_deadline() {
    begin();
    handshake();
    held = 1000000;
    ble::att_delay(1000);
    const auto start = now();
    std::string response;
    CHECK(!gatt::gatt_client_exchange(true, "request", response));
    check_failure(start, "response_deadline");
    CHECK(now() - start >= 45000 && now() - start < 50000);
}

// An empty request is not sent, and leaves the response alone.
void empty_request() {
    begin();
    const auto calls = ble::calls().size();
    const auto start = now();
    std::string response = "unchanged";
    CHECK(!gatt::gatt_client_exchange(true, "", response) && response == "unchanged");
    CHECK(ble::calls().size() == calls);
    const auto lines = exchange_lines();
    CHECK(lines.size() == 1 && lines[0] == exchange_line({start, 0, "failed", "precondition", 0, 1, 0, 0, 0, 0, 0, 0, 0,
                                                          0, 0, 0, 0, 0}));
}

}  // namespace

int main(int argc, char **argv) {
    return finish(amity_test::run(argc, argv,
                                  {{"mtu-framing", mtu_framing},
                                   {"exchange-timing", exchange_timing},
                                   {"published-before-cursor", published_before_cursor},
                                   {"request-write-failure", request_write_failure},
                                   {"request-wait-failure", request_wait_failure},
                                   {"att-wait-bounds", att_wait_bounds},
                                   {"reply-rejected", reply_rejected},
                                   {"reply-malformed", reply_malformed},
                                   {"reply-deadline", reply_deadline},
                                   {"empty-request", empty_request}}));
}
