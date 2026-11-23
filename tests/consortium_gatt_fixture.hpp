/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
// Scenario support for the tests that build components/consortium/gatt_transport.cpp
// on the fake NimBLE host. Include it after that file: it reaches the file's
// internals through the gatt namespace alias.
#include "core_fakes.hpp"
#include "gatt_fakes.hpp"
#include "scenario_main.hpp"
#include "seams.hpp"
#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace gatt = amity::consortium::platform;
namespace ble = amity_fake::ble;
using amity::consortium::RuntimeAdmissionCandidate;

inline uint64_t now() { return uint64_t(amity_fake::now_us() / 1000); }

// The transport's platform clock, and random tokens that are distinct hex
// strings of the requested size. The board has been up for a second: the
// transport takes a time of 0 to mean none.
inline void install_platform() {
    amity_fake::advance_ms(1000);
    amity_seam::install<&gatt::now_ms>([] { return now(); });
    amity_seam::install<&gatt::random_hex>([](size_t bytes) {
        static unsigned issued = 0;
        char word[9];
        std::snprintf(word, sizeof(word), "%08x", 0xc0ffee00u + ++issued);
        std::string value;
        while (value.size() < 2 * bytes) value += word;
        value.resize(2 * bytes);
        return value;
    });
}

// Text no timing line may contain.
inline std::vector<std::string> timing_secrets;

// Every timing line is an info line under setup_timing, shorter than 384
// bytes, without a secret, and logged with neither transport lock held.
inline void watch_timing_lines() {
    amity_fake::on_log([](const amity_fake::LogLine &line) {
        bool unlocked = false;
        std::thread probe([&] {
            if (!gatt::client_mutex.try_lock()) return;
            gatt::client_mutex.unlock();
            if (!gatt::state_mutex.try_lock()) return;
            gatt::state_mutex.unlock();
            unlocked = true;
        });
        probe.join();
        CHECK(unlocked);
        CHECK(line.level == ESP_LOG_INFO && line.tag == "setup_timing");
        CHECK(line.message.size() < 384);
        for (const auto &secret : timing_secrets) CHECK(line.message.find(secret) == std::string::npos);
    });
}

inline std::vector<std::string> timing_lines() {
    std::vector<std::string> lines;
    for (const auto &line : amity_fake::log_lines())
        if (line.tag == "setup_timing") lines.push_back(line.message);
    return lines;
}

// The admission service's UUID as an advertisement lists it.
inline std::array<uint8_t, 16> service_uuid() {
    std::array<uint8_t, 16> value{};
    std::memcpy(value.data(), gatt::service_uuid.value, value.size());
    return value;
}

// A board's advertisement: its name, with or without the admission service.
inline ble::Report advert(std::string_view name, uint8_t address, bool with_service = true) {
    const auto uuid = service_uuid();
    return {ble::address(address), ble::advertising_payload(with_service ? &uuid : nullptr, name)};
}

// Starts a discovery that hears report 10 ms in; returns what it found.
inline std::vector<RuntimeAdmissionCandidate> discover_one(uint64_t generation, const ble::Report &report) {
    ble::after(10, [report] { ble::discover(report); });
    std::vector<RuntimeAdmissionCandidate> found;
    CHECK(gatt::gatt_client_discover(generation, found) == ESP_OK);
    CHECK(found.size() == 1);
    return found;
}

// Ends a scenario's process: the admission channel, a firmware global, is
// released while the seams and fakes its destructor reaches still exist.
inline int finish(int status) {
    gatt::server_channel.reset();
    return status;
}

// Thrown by the turn that ends run_worker().
struct WorkerWaiting {};
inline unsigned worker_turns = 0, running_worker_turn = 0;

// Runs the real worker loop until it waits for work again, and leaves it
// waiting there: a turn due at that time ends the wait by throwing. With
// for_ms, the worker first waits that long, through its receive timeouts.
// The worker task itself stays created; worker() is the same loop.
inline void run_worker(int64_t for_ms = 0) {
    const unsigned turn = ++worker_turns;
    running_worker_turn = turn;
    ble::after(for_ms, [turn] {
        if (running_worker_turn == turn) throw WorkerWaiting{};
    });
    try {
        gatt::worker_work();
    } catch (const WorkerWaiting &) {
    }
    running_worker_turn = 0;
}

// The server's GATT protocol from the peer's side. A request fragment is
// {1, endpoint, record(4), total(2), offset(2), bytes}; a cursor is
// {2, record(4), offset(2)}; a response is {1, status, record(4), size(2),
// endpoint, offset(2), bytes}.
inline std::string fragment(uint32_t record, uint8_t endpoint, size_t total, size_t offset, std::string_view bytes) {
    std::string value{char(1), char(endpoint), char(record >> 24), char(record >> 16), char(record >> 8), char(record),
                      char(total >> 8), char(total), char(offset >> 8), char(offset)};
    return value.append(bytes);
}
inline std::string cursor(uint32_t record, size_t offset) {
    return {char(2), char(record >> 24), char(record >> 16), char(record >> 8), char(record), char(offset >> 8),
            char(offset)};
}
struct Response {
    uint8_t version, status;
    uint32_t record;
    size_t size;
    uint8_t endpoint;
    size_t offset;
    std::string bytes;
};
inline Response parse_response(std::string_view value) {
    CHECK(value.size() >= 11);
    const auto byte = [&](size_t i) { return uint8_t(value[i]); };
    return {byte(0),
            byte(1),
            uint32_t(byte(2)) << 24 | uint32_t(byte(3)) << 16 | uint32_t(byte(4)) << 8 | byte(5),
            size_t(byte(6)) << 8 | byte(7),
            byte(8),
            size_t(byte(9)) << 8 | byte(10),
            std::string(value.substr(11))};
}
