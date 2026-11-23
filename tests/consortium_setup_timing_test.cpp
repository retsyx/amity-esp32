/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

// Setup timings, collected by BoundedSetupTimings and logged by setup_timing.cpp
// through the core fakes.
#include "setup_timing.hpp"
#include "core_fakes.hpp"
#include "scenario_main.hpp"
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

namespace {

using amity::consortium::platform::BoundedSetupTimings;
using amity::consortium::platform::SetupTimingEntry;
using amity::consortium::platform::emit_setup_timing;
namespace model = amity::system;

// The timings live on the stacks of the functions they time.
static_assert(sizeof(BoundedSetupTimings<1>) <= 160 && sizeof(BoundedSetupTimings<5>) <= 432 &&
                  sizeof(BoundedSetupTimings<7>) <= 560 && sizeof(BoundedSetupTimings<12>) <= 880,
              "diagnostic stack footprint grew");

uint64_t now_ms() { return uint64_t(amity_fake::now_us() / 1000); }

// Stands in for the locks a timed function takes after declaring its timings.
bool held = false;
struct Guard {
    Guard() { held = true; }
    ~Guard() { held = false; }
};

// Every timing is an info line under setup_timing, logged with no guard held,
// shorter than 384 bytes and free of secrets.
void watch_lines() {
    amity_fake::on_log([](const amity_fake::LogLine &line) {
        CHECK(!held);
        CHECK(line.level == ESP_LOG_INFO && line.tag == "setup_timing");
        CHECK(line.message.size() < 384);
        for (const char *secret : {"password", "ssid", "token", "payload"})
            CHECK(line.message.find(secret) == std::string::npos);
    });
}

std::vector<std::string> lines() {
    std::vector<std::string> out;
    for (const auto &line : amity_fake::log_lines()) out.push_back(line.message);
    return out;
}

template <class Id> Id id(unsigned n) {
    char text[33];
    std::snprintf(text, sizeof(text), "%032x", n);
    return *Id::parse(text);
}

// The text of the peer, link and scan ids the scenarios use, and of an empty id.
const std::string peer_id = std::string(31, '0') + "3", link_id = std::string(31, '0') + "4",
                  scan_id = std::string(31, '0') + "1", zero_id(32, '0');

// Lines are logged when the timings go out of scope, after the guards, with
// each stage's completion time; a summed stage reports its sum.
void deferred_emission() {
    watch_lines();
    amity_fake::advance_ms(100);
    {
        BoundedSetupTimings<2> timing("invite", now_ms);
        timing.peer = id<model::OctopusId>(3);
        timing.link = id<model::LinkId>(4);
        const auto first = timing.begin("peer_validation");
        {
            Guard guard;
            amity_fake::advance_ms(11);
            timing.finish(first, true, 2, 9, 1, -3);
            CHECK(amity_fake::log_lines().empty());
        }
        amity_fake::advance_ms(9);
        timing.sum("verify_callback", 7, 3);
        CHECK(amity_fake::log_lines().empty());
    }
    const std::vector<std::string> expected{
        "area=invite stage=peer_validation at_ms=111 elapsed_ms=11 result=ok peer=" + peer_id + " link=" + link_id +
            " count=2 call_ms=9 wait_ms=1 code=-3 metric=span",
        "area=invite stage=verify_callback at_ms=120 elapsed_ms=7 result=ok peer=" + peer_id + " link=" + link_id +
            " count=3 call_ms=7 wait_ms=0 code=0 metric=sum"};
    CHECK(lines() == expected);
}

// A stage still open when the timings end failed. A timing anchored on a scan
// uses the scan format.
void unfinished_stage() {
    watch_lines();
    amity_fake::advance_ms(100);
    {
        BoundedSetupTimings<1> timing("admission", now_ms);
        timing.scan = id<model::MessageId>(1);
        timing.peer = id<model::OctopusId>(3);
        timing.begin("selected");
        Guard guard;
        amity_fake::advance_ms(3);
    }
    const std::vector<std::string> expected{"area=admission stage=selected at_ms=103 elapsed_ms=3 result=failed scan=" +
                                            scan_id + " peer=" + peer_id + " code=0"};
    CHECK(lines() == expected);
}

// Both formats carry every field of the entry, in order.
void line_formats() {
    watch_lines();
    SetupTimingEntry entry;
    entry.stage = "connect";
    entry.result = "failed";
    entry.at_ms = 1234;
    entry.elapsed_ms = 56;
    entry.call_ms = 7;
    entry.wait_ms = 8;
    entry.count = 9;
    entry.code = -10;
    emit_setup_timing("admission", entry, id<model::OctopusId>(3), id<model::LinkId>(4), id<model::MessageId>(1));
    entry.summed = true;
    emit_setup_timing("tls_handshake", entry, id<model::OctopusId>(3), id<model::LinkId>(4), {});
    const std::vector<std::string> expected{
        "area=admission stage=connect at_ms=1234 elapsed_ms=56 result=failed scan=" + scan_id + " peer=" + peer_id +
            " code=-10",
        "area=tls_handshake stage=connect at_ms=1234 elapsed_ms=56 result=failed peer=" + peer_id + " link=" + link_id +
            " count=9 call_ms=7 wait_ms=8 code=-10 metric=sum"};
    CHECK(lines() == expected);
}

// The longest values of every field still fit in a log line, in both formats.
void worst_case_line() {
    watch_lines();
    SetupTimingEntry entry;
    entry.stage = "verify_callback";
    entry.result = "failed";
    entry.at_ms = entry.elapsed_ms = entry.call_ms = entry.wait_ms = UINT64_MAX;
    entry.count = UINT32_MAX;
    entry.code = std::numeric_limits<int>::min();
    emit_setup_timing("tls_handshake", entry, {}, {}, {});
    emit_setup_timing("tls_handshake", entry, {}, {}, id<model::MessageId>(UINT32_MAX));
    const auto logged = lines();
    CHECK(logged.size() == 2);
    for (const auto &line : logged) CHECK(line.size() < 384);
}

// A full set of timings ignores further stages.
void capacity() {
    watch_lines();
    {
        BoundedSetupTimings<1> timing("startup", now_ms);
        CHECK(timing.begin("first") == 0);
        CHECK(timing.begin("second") == 1);
        timing.finish(1, true);
        timing.sum("third", 5, 1);
        CHECK(timing.step("fourth", [] { return true; }));
    }
    const std::vector<std::string> expected{"area=startup stage=first at_ms=0 elapsed_ms=0 result=failed peer=" +
                                            zero_id + " link=" + zero_id +
                                            " count=0 call_ms=0 wait_ms=0 code=0 metric=span"};
    CHECK(lines() == expected);
}

// A stage finishes once, and only stages that began can finish.
void finish_once() {
    watch_lines();
    {
        BoundedSetupTimings<2> timing("network", now_ms);
        const auto first = timing.begin("dhcp");
        timing.finish(1, false, 9);
        amity_fake::advance_ms(4);
        timing.finish(first, true, 1);
        amity_fake::advance_ms(6);
        timing.finish(first, false, 2);
        const auto second = timing.begin("address");
        amity_fake::advance_ms(2);
        timing.finish(second, true, 3);
    }
    const std::vector<std::string> expected{
        "area=network stage=dhcp at_ms=4 elapsed_ms=4 result=ok peer=" + zero_id + " link=" + zero_id +
            " count=1 call_ms=0 wait_ms=0 code=0 metric=span",
        "area=network stage=address at_ms=12 elapsed_ms=2 result=ok peer=" + zero_id + " link=" + zero_id +
            " count=3 call_ms=0 wait_ms=0 code=0 metric=span"};
    CHECK(lines() == expected);
}

// done() closes the open stages with its result; step() times one action and
// records its result.
void done_and_step() {
    watch_lines();
    {
        BoundedSetupTimings<4> timing("tls", now_ms);
        CHECK(timing.step("parse", [] {
            amity_fake::advance_ms(4);
            return true;
        }));
        CHECK(!timing.step("verify", [] {
            amity_fake::advance_ms(1);
            return false;
        }));
        const auto failed = timing.begin("connect");
        timing.finish(failed, false);
        timing.begin("handshake");
        amity_fake::advance_ms(2);
        CHECK(timing.done(true));
    }
    const auto line = [](const char *stage, unsigned at, unsigned elapsed, const char *result) {
        return std::string("area=tls stage=") + stage + " at_ms=" + std::to_string(at) + " elapsed_ms=" +
               std::to_string(elapsed) + " result=" + result + " peer=" + zero_id + " link=" + zero_id +
               " count=0 call_ms=0 wait_ms=0 code=0 metric=span";
    };
    const std::vector<std::string> expected{line("parse", 4, 4, "ok"), line("verify", 5, 1, "failed"),
                                            line("connect", 5, 0, "failed"), line("handshake", 7, 2, "ok")};
    CHECK(lines() == expected);
}

}  // namespace

int main(int argc, char **argv) {
    return amity_test::run(argc, argv,
                           {{"deferred-emission", deferred_emission},
                            {"unfinished-stage", unfinished_stage},
                            {"line-formats", line_formats},
                            {"worst-case-line", worst_case_line},
                            {"capacity", capacity},
                            {"finish-once", finish_once},
                            {"done-and-step", done_and_step}});
}
