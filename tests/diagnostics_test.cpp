/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#include "diagnostics_support.hpp"
#include <cassert>
#include <iostream>
#include <string>

using namespace amity_diagnostics;

int main() {
    LogSessions logs;
    std::string live = "old log\nnew log\n";
    const std::string snapshot = live;
    const auto first = logs.begin(live);
    live.assign("ring has wrapped");
    const auto second = logs.begin("independent");
    assert(first != LogSessions::invalid && second != first);
    assert(logs.begin("overflow") == LogSessions::invalid);
    // Chunk boundaries reconstruct the exact snapshot, independently of other
    // clients and any later modifications to the original live buffer.
    std::string result;
    bool done = false;
    while (!done) {
        char buffer[3];
        size_t size = sizeof(buffer);
        assert(logs.collect(first, buffer, size, done));
        result.append(buffer, size);
    }
    assert(result == snapshot);
    char buffer[32];
    size_t size = sizeof(buffer);
    assert(logs.collect(first, buffer, size, done) && size == 0 && done);
    assert(!logs.collect(LogSessions::invalid, buffer, size, done));
    size = sizeof(buffer);
    assert(logs.collect(second, buffer, size, done) && done);
    assert(std::string(buffer, size) == "independent");
    assert(logs.end(first));
    assert(!logs.end(first));
    assert(!logs.collect(first, buffer, size, done));
    const auto replacement = logs.begin("after cancellation");
    assert(replacement != first && replacement != second);
    assert(!logs.collect(first, buffer, size, done));
    assert(logs.end(second));
    assert(logs.end(replacement));
    const auto empty = logs.begin({});
    size = 0;
    assert(logs.collect(empty, nullptr, size, done) && done);
    assert(logs.end(empty));
    // Handle wrap must never emit the invalid handle or collide with a client
    // that has kept its session open through other clients' entire lifecycle.
    const auto held = logs.begin("held");
    for (unsigned i = 0; i < 70000; ++i) {
        const auto handle = logs.begin("new");
        assert(handle != LogSessions::invalid && handle != held);
        assert(logs.end(handle));
    }
    assert(logs.end(held));

    size = copy_recent(snapshot, buffer, 4);
    assert(size == 4 && std::string(buffer, size) == "log\n");
    size = copy_recent(snapshot, buffer, sizeof(buffer));
    assert(std::string(buffer, size) == snapshot);
    assert(copy_recent({}, nullptr, 0) == 0);
    assert(copy_recent(snapshot, nullptr, 0) == 0);

    assert(operational_hours(0, 3599) == 0);
    assert(operational_hours(0, 3600) == 1);
    assert(operational_hours(3599, 1) == 1);
    assert(operational_hours(3600, 3600) == 2);
    assert(operational_hours(3601, 3599) == 2);
    assert(operational_hours(uint64_t{UINT32_MAX} * 3600, 100) == UINT32_MAX);
    assert(operational_hours(UINT64_MAX, UINT64_MAX) == UINT32_MAX);
    assert(operational_hours(0, UINT64_MAX) == UINT32_MAX);
    // Persist fractional hours, so frequent orderly restarts still accumulate.
    uint64_t seconds = 0;
    for (unsigned reboot = 0; reboot < 60; ++reboot) seconds = operational_seconds(seconds, 60);
    assert(operational_hours(seconds, 0) == 1);
    std::cout << "PASS: independent/chunked log snapshots, capacity/cancellation/stale handles, handle wrap, cumulative hours and overflow\n";
}
