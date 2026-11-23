/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

// The candidate's admission worker and its work queue: gatt_transport.cpp
// built whole on the fake NimBLE host, with the real admission handshake of
// admission_crypto.cpp over the OpenSSL-backed PSA fake. A scenario plays the
// admitting board through the server's GATT characteristic.
#include "../components/consortium/gatt_transport.cpp"
#include "consortium_gatt_fixture.hpp"
#include "crypto_fakes.hpp"
#include <algorithm>
#include <functional>

namespace {

using gatt::AdmissionClient;
using gatt::Work;

const auto id = *amity::system::OctopusId::parse("0123456789abcdef0123456789a1b2c3");

// Every plaintext the server's handler got; it replies with "reply:" and it.
std::vector<std::string> handled;
// The last record id a scenario wrote.
uint32_t records = 0;

void begin() {
    install_platform();
    CHECK(gatt::gatt_register() == ESP_OK);
}

// Opens the setup window and admits a central on this link.
void open(uint64_t generation = 9, uint16_t handle = 41) {
    CHECK(gatt::gatt_server_open(generation, id, [](std::string_view request, std::string &reply) {
              handled.emplace_back(request);
              reply = "reply:" + std::string(request);
              return ESP_OK;
          }) == ESP_OK);
    ble::accept(handle);
    ble::set_mtu(handle, 527);
    CHECK(gatt::server_connection == handle);
}

// Runs the worker until it waits for work again; nothing it took is left
// counted as outstanding or in progress.
void work(int64_t for_ms = 0) {
    run_worker(for_ms);
    CHECK(gatt::outstanding_work == 0 && !gatt::processing_record);
}

// Writes a record to the server in fragments of at most `piece` bytes.
void write_record(uint32_t record, uint8_t endpoint, std::string_view bytes, size_t piece = 400,
                  uint16_t handle = 41) {
    for (size_t offset = 0; offset < bytes.size(); offset += piece)
        CHECK(ble::server_write(handle, fragment(record, endpoint, bytes.size(), offset,
                                                 bytes.substr(offset, std::min(piece, bytes.size() - offset)))) == 0);
}

// Reads the server's whole response to a record.
Response read_response(uint32_t record, uint16_t handle = 41) {
    Response whole{};
    std::string bytes;
    do {
        CHECK(ble::server_write(handle, cursor(record, bytes.size())) == 0);
        std::string value;
        CHECK(ble::server_read(handle, value) == 0);
        whole = parse_response(value);
        if (whole.record != record || whole.status != 1) return whole;
        CHECK(whole.offset == bytes.size());
        bytes += whole.bytes;
    } while (bytes.size() < whole.size);
    whole.bytes = bytes;
    return whole;
}

// Runs the real handshake, each of the client's requests crossing as one
// record that the worker processes.
bool connect(AdmissionClient &client, uint16_t handle = 41) {
    return client.handshake([&](std::string_view request, std::string &response) {
        write_record(++records, 0, request, 400, handle);
        work();
        const auto reply = read_response(records, handle);
        if (reply.record != records || reply.status != 1) return false;
        response = reply.bytes;
        return true;
    });
}

// Sends one encrypted request and returns the decrypted reply, if any.
bool request(AdmissionClient &client, std::string_view plain, std::string &reply, uint16_t handle = 41) {
    std::string sealed;
    CHECK(client.encrypt(plain, sealed));
    write_record(++records, 1, sealed, 400, handle);
    work();
    const auto response = read_response(records, handle);
    return response.record == records && response.status == 1 && client.decrypt(response.bytes, reply);
}

// The work queue is the static 16-slot queue over the persistent storage and
// control objects; nothing creates a queue dynamically.
void static_queue() {
    begin();
    std::size_t item_queues = 0;
    for (const auto &queue : amity_fake::queues()) {
        if (queue.kind != amity_fake::Queue::Kind::queue) continue;
        ++item_queues;
        CHECK(queue.handle == gatt::work_queue && queue.is_static && !queue.with_caps);
        CHECK(queue.length == 16 && queue.item_size == sizeof(Work));
    }
    CHECK(item_queues == 1);
    CHECK(reinterpret_cast<void *>(gatt::work_queue) == &gatt::work_queue_control);
    CHECK(sizeof(gatt::work_queue_storage) == 16 * sizeof(Work));
    // A queued record is a copy in the persistent storage.
    open();
    Work work;
    work.record = 0x5a5a;
    std::fill(std::begin(work.bytes), std::end(work.bytes), uint8_t(0xa5));
    CHECK(gatt::post_work(work));
    Work stored;
    std::memcpy(&stored, gatt::work_queue_storage, sizeof(stored));
    CHECK(stored.record == 0x5a5a && stored.bytes[0] == 0xa5 && stored.bytes[511] == 0xa5);
}

// Callback records are copied in FIFO order; the seventeenth is refused and
// its count rolled back; the ring wraps and reuses its slots.
void queue_fifo() {
    begin();
    open();
    for (unsigned n = 0; n < 16; ++n) {
        Work work;
        work.record = n;
        work.length = sizeof(work.bytes);
        std::memset(work.bytes, int(n + 1), sizeof(work.bytes));
        CHECK(gatt::post_work(work));
        std::memset(work.bytes, 0, sizeof(work.bytes));
    }
    CHECK(gatt::outstanding_work == 16);
    const auto refused = now();
    CHECK(!gatt::post_work(Work{}) && gatt::outstanding_work == 16 && now() == refused);
    for (unsigned n = 0; n < 16; ++n) {
        Work work;
        CHECK(xQueueReceive(gatt::work_queue, &work, 0) == pdTRUE && work.record == n);
        CHECK(std::all_of(std::begin(work.bytes), std::end(work.bytes), [n](uint8_t byte) { return byte == n + 1; }));
        --gatt::outstanding_work;
    }
    for (unsigned n = 0; n < 12; ++n) {
        Work work;
        work.record = n;
        CHECK(gatt::post_work(work));
    }
    for (unsigned n = 0; n < 8; ++n) {
        Work work;
        CHECK(xQueueReceive(gatt::work_queue, &work, 0) == pdTRUE && work.record == n);
        --gatt::outstanding_work;
    }
    for (unsigned n = 12; n < 24; ++n) {
        Work work;
        work.record = n;
        CHECK(gatt::post_work(work));
    }
    for (unsigned n = 8; n < 24; ++n) {
        Work work;
        CHECK(xQueueReceive(gatt::work_queue, &work, 0) == pdTRUE && work.record == n);
        --gatt::outstanding_work;
    }
    CHECK(gatt::outstanding_work == 0);
}

// A queue that cannot be created fails registration, which a retry completes.
void queue_creation_failure() {
    install_platform();
    amity_fake::fail_queue_creations(1);
    CHECK(gatt::gatt_register() == ESP_ERR_NO_MEM);
    CHECK(ble::count("ConfigureExtraServices") == 0 && !gatt::registered);
    CHECK(gatt::gatt_server_open(9, id, [](std::string_view, std::string &) { return ESP_OK; }) ==
          ESP_ERR_INVALID_STATE);
    CHECK(gatt::gatt_register() == ESP_OK && gatt::registered);
}

// A worker with nothing to do runs no admission crypto.
void idle_worker() {
    begin();
    open();
    work(1000);
    CHECK(amity_fake::psa_keys() == 0 && !gatt::server_channel && !gatt::channel_live && gatt::response_record == 0);
    CHECK(amity_fake::tasks().size() == 1 && amity_fake::tasks()[0].state == amity_fake::Task::State::created);
}

// A record arrives in fragments; only the complete record runs the
// handshake. One handshake authenticates the connection, whose encrypted
// requests then reach the handler.
void fragmented_handshake() {
    begin();
    open();
    AdmissionClient client;
    bool first = true;
    CHECK(client.handshake([&](std::string_view request, std::string &response) {
        ++records;
        if (first) {
            first = false;
            const auto client_keys = amity_fake::psa_keys();
            const auto half = request.size() / 2;
            CHECK(ble::server_write(41, fragment(records, 0, request.size(), 0, request.substr(0, half))) == 0);
            work();
            CHECK(amity_fake::psa_keys() == client_keys && !gatt::server_channel && gatt::channel_live);
            CHECK(gatt::response_record == 0);
            CHECK(ble::server_write(41, fragment(records, 0, request.size(), half, request.substr(half))) == 0);
            work();
            CHECK(gatt::server_channel && amity_fake::psa_keys() == client_keys + 2 && gatt::response_status == 1);
        } else {
            write_record(records, 0, request);
            work();
        }
        const auto reply = read_response(records);
        CHECK(reply.record == records && reply.status == 1);
        response = reply.bytes;
        return true;
    }));
    CHECK(client.peer_id() == id.str() && gatt::server_channel->authenticated());
    std::string reply;
    CHECK(request(client, "secret", reply) && reply == "reply:secret");
    CHECK(handled == std::vector<std::string>{"secret"});
    CHECK(gatt::failed_proofs == 0);
}

// A new connection, or a new session on the link, gets a new admission
// channel: its handshake succeeds where the old channel would refuse one.
void new_session_new_channel() {
    begin();
    open();
    AdmissionClient first;
    CHECK(connect(first));
    ble::disconnect(41);
    ble::accept(42);
    ble::set_mtu(42, 527);
    AdmissionClient second;
    CHECK(connect(second, 42));
    std::string reply;
    CHECK(request(second, "after reconnect", reply, 42) && reply == "reply:after reconnect");
    // A session change the worker sees before any close, on the same link.
    {
        std::lock_guard<std::mutex> guard(gatt::state_mutex);
        ++gatt::server_session;
    }
    AdmissionClient third;
    CHECK(connect(third, 42));
    CHECK(gatt::failed_proofs == 0);
}

// The worker exits once the window has closed and its channel is done; it
// deletes its own task. Without a worker only a close is accepted, and
// that is not queued; the next window starts a new worker.
void worker_exits() {
    begin();
    open();
    const auto task = amity_fake::tasks().front();
    CHECK(task.name == "group_admission" && task.stack_bytes == 16384 && task.priority == 2 && task.with_caps);
    CHECK(task.caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    // A worker still running serves the next window.
    gatt::gatt_server_close();
    CHECK(gatt::gatt_server_open(10, id, [](std::string_view, std::string &) { return ESP_OK; }) == ESP_OK);
    CHECK(amity_fake::tasks().size() == 1);
    gatt::gatt_server_close();
    amity_fake::run_task(task.handle);
    CHECK(amity_fake::tasks().front().state == amity_fake::Task::State::deleted && !gatt::worker_running);
    Work late;
    late.close = true;
    CHECK(gatt::post_work(late) && gatt::outstanding_work == 0 && uxQueueMessagesWaiting(gatt::work_queue) == 0);
    late.close = false;
    CHECK(!gatt::post_work(late) && gatt::outstanding_work == 0);
    amity_fake::advance_ms(1);
    open(11, 43);
    CHECK(amity_fake::tasks().size() == 2 && gatt::worker_running);
}

// A valid first handshake request, from a client that is gone again.
std::string client_hello() {
    std::string hello;
    AdmissionClient client;
    CHECK(!client.handshake([&](std::string_view request, std::string &) {
        hello.assign(request);
        return false;
    }));
    CHECK(!hello.empty());
    return hello;
}

// The queued record never reached the handshake or the handler.
void check_untouched() {
    CHECK(amity_fake::psa_keys() == 0 && !gatt::server_channel && handled.empty() && gatt::response_record == 0);
}

// A record queued before its window's generation, session or link changed,
// before the window closed or before its deadline passed never reaches the
// handshake. Each change comes alone, so each of the worker's checks is
// exercised by itself: gatt_server_close() would also forget the link.
void stale_record(const std::function<void()> &change) {
    begin();
    open();
    write_record(1, 0, client_hello());
    change();
    work();
    check_untouched();
}
void stale_generation() {
    stale_record([] {
        std::lock_guard<std::mutex> guard(gatt::state_mutex);
        ++gatt::generation;
    });
}
void stale_session() {
    stale_record([] {
        std::lock_guard<std::mutex> guard(gatt::state_mutex);
        ++gatt::server_session;
    });
}
void stale_link() {
    stale_record([] {
        std::lock_guard<std::mutex> guard(gatt::state_mutex);
        ++gatt::server_connection;
    });
}
void closed_window() {
    stale_record([] {
        std::lock_guard<std::mutex> guard(gatt::state_mutex);
        gatt::server_open = false;
    });
}
void passed_deadline() { stale_record([] { amity_fake::advance_ms(180000); }); }

// An encrypted record before any handshake is refused without crypto.
void encrypted_first() {
    begin();
    open();
    write_record(1, 1, client_hello());
    work();
    CHECK(amity_fake::psa_keys() == 0 && !gatt::server_channel && !gatt::channel_live && handled.empty());
    CHECK(gatt::response_record == 1 && gatt::response_status == 2 && gatt::failed_proofs == 1);
}

// A record still assembling 30 s after its first fragment ends the
// connection without crypto, whether its next fragment or the idle worker
// finds it.
void assembly_expiry() {
    begin();
    open();
    const auto hello = client_hello();
    const auto half = hello.size() / 2;
    CHECK(ble::server_write(41, fragment(1, 0, hello.size(), 0, hello.substr(0, half))) == 0);
    work();
    amity_fake::advance_ms(30000);
    CHECK(ble::server_write(41, fragment(1, 0, hello.size(), half, hello.substr(half))) == 0);
    work();
    auto ended = ble::terminations();
    CHECK(ended.size() == 1 && ended[0].handle == 41 && ended[0].reason == BLE_ERR_REM_USER_CONN_TERM);
    CHECK(amity_fake::psa_keys() == 0 && !gatt::channel_live && gatt::failed_proofs == 1 && gatt::response_record == 0);
    amity_fake::advance_ms(1);
    ble::accept(42);
    ble::set_mtu(42, 527);
    CHECK(ble::server_write(42, fragment(2, 0, hello.size(), 0, hello.substr(0, half))) == 0);
    work(29900);
    CHECK(ble::terminations().size() == 1 && gatt::channel_live);
    work(200);
    ended = ble::terminations();
    CHECK(ended.size() == 2 && ended[1].handle == 42 && amity_fake::psa_keys() == 0 && !gatt::channel_live);
    CHECK(gatt::failed_proofs == 2);
}

// A fragment of another record while one is assembling ends the
// connection without crypto.
void interleaved_record() {
    begin();
    open();
    const auto hello = client_hello();
    CHECK(ble::server_write(41, fragment(1, 0, hello.size(), 0, hello.substr(0, 40))) == 0);
    CHECK(ble::server_write(41, fragment(2, 0, hello.size(), 0, hello.substr(0, 40))) == 0);
    work();
    const auto ended = ble::terminations();
    CHECK(ended.size() == 1 && ended[0].handle == 41 && gatt::failed_proofs == 1);
    CHECK(amity_fake::psa_keys() == 0 && !gatt::channel_live && gatt::response_record == 0);
}

// Five failed proofs close the window: the link ends and the server takes
// no more records.
void five_failures() {
    begin();
    open();
    for (uint32_t record = 1; record <= 5; ++record) {
        CHECK(ble::terminations().empty() && gatt::server_open);
        write_record(record, 1, std::string(64, 's'));
        work();
    }
    CHECK(gatt::failed_proofs == 5 && !gatt::server_open && ble::count("ble_gap_adv_stop") == 1);
    const auto ended = ble::terminations();
    CHECK(ended.size() == 1 && ended[0].handle == 41 && ended[0].reason == BLE_ERR_REM_USER_CONN_TERM);
    CHECK(ble::server_write(41, fragment(6, 0, 20, 0, std::string(20, 'h'))) == BLE_ATT_ERR_INSUFFICIENT_AUTHEN);
}

}  // namespace

int main(int argc, char **argv) {
    return finish(amity_test::run(argc, argv,
                                  {{"static-queue", static_queue},
                                   {"queue-fifo", queue_fifo},
                                   {"queue-creation-failure", queue_creation_failure},
                                   {"idle-worker", idle_worker},
                                   {"fragmented-handshake", fragmented_handshake},
                                   {"new-session-new-channel", new_session_new_channel},
                                   {"worker-exits", worker_exits},
                                   {"stale-generation", stale_generation},
                                   {"stale-session", stale_session},
                                   {"stale-link", stale_link},
                                   {"closed-window", closed_window},
                                   {"passed-deadline", passed_deadline},
                                   {"encrypted-first", encrypted_first},
                                   {"assembly-expiry", assembly_expiry},
                                   {"interleaved-record", interleaved_record},
                                   {"five-failures", five_failures}}));
}
