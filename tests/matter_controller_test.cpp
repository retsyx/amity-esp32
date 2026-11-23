/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

// The Matter controller logic of components/amity_matter/controller.cpp, built
// whole. The test implements the CHIP port: a CHIP task that runs queued work
// holding a stack lock, attribute reports, and the commissioning and service
// calls, all recorded. The file is included so the scenarios can check that
// no publication lock is held across CHIP calls.
#include "../components/amity_matter/controller.cpp"
#include "core_fakes.hpp"
#include "scenario_main.hpp"
#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <future>
#include <string>
#include <thread>
#include <vector>

namespace controller_test {

using amity_matter::Attribute;

// The CHIP stack lock, and whether this thread holds it or runs as the CHIP task.
std::recursive_mutex chip_lock;
thread_local unsigned chip_depth = 0;
thread_local bool chip_task = false;
// Set while another task holds the stack lock: taking it then would block.
std::atomic<bool> held_elsewhere{false};
unsigned stack_locks = 0;

// Work queued for the CHIP task.
std::deque<void (*)(intptr_t)> work;
unsigned schedules = 0, refusals = 0;
// Runs the work before schedule() returns, as the CHIP task.
bool immediate = false;
std::function<void()> during_schedule;

std::vector<Attribute> reports;
std::function<void()> during_report;

// The commissioning state the enrollment check reads, and the calls it made.
struct Commissioning {
    bool disarmed = true, fabric = true, provisioned = true, connected = true, commit = true;
    std::vector<std::string> calls;
} commissioning;

// The service calls of a revocation, in order.
std::vector<std::string> services;
unsigned dns_starts = 0;
std::vector<std::pair<uint8_t, void *>> keys;

// Whether another thread could take the publication lock now.
bool mailbox_free() {
    return std::async(std::launch::async, [] {
               if (!state_publication_mutex.try_lock()) return false;
               state_publication_mutex.unlock();
               return true;
           }).get();
}

void run_as_chip_task(void (*item)(intptr_t)) {
    std::lock_guard<std::recursive_mutex> guard(chip_lock);
    CHECK(!chip_task);
    chip_task = true;
    ++chip_depth;
    item(0);
    --chip_depth;
    chip_task = false;
}

size_t queued() { return work.size(); }

void run_one() {
    CHECK(!work.empty());
    const auto item = work.front();
    work.pop_front();
    run_as_chip_task(item);
}

std::size_t count(Attribute attribute) { return std::size_t(std::count(reports.begin(), reports.end(), attribute)); }

}  // namespace controller_test

namespace amity_matter::chip_port {

using namespace controller_test;

bool schedule(void (*item)(intptr_t)) {
    CHECK(mailbox_free());
    ++schedules;
    if (during_schedule) {
        const auto hook = std::move(during_schedule);
        during_schedule = {};
        hook();
    }
    if (refusals) {
        --refusals;
        return false;
    }
    if (immediate)
        run_as_chip_task(item);
    else
        work.push_back(item);
    return true;
}

void report(Attribute attribute) {
    CHECK((chip_task || chip_depth) && started.load() && controller_enabled.load());
    CHECK(mailbox_free());
    reports.push_back(attribute);
    if (during_report) {
        const auto hook = std::move(during_report);
        during_report = {};
        hook();
    }
}

void with_stack_lock(void (*fn)()) {
    CHECK(!held_elsewhere.load());
    std::lock_guard<std::recursive_mutex> guard(chip_lock);
    ++stack_locks;
    ++chip_depth;
    fn();
    --chip_depth;
}

bool fail_safe_disarmed() {
    CHECK(chip_task);
    commissioning.calls.push_back("fail_safe_disarmed");
    return commissioning.disarmed;
}
bool has_fabric() {
    CHECK(chip_task);
    commissioning.calls.push_back("has_fabric");
    return commissioning.fabric;
}
bool station_provisioned() {
    CHECK(chip_task);
    commissioning.calls.push_back("station_provisioned");
    return commissioning.provisioned;
}
bool station_connected() {
    CHECK(chip_task);
    commissioning.calls.push_back("station_connected");
    return commissioning.connected;
}
bool commit_wifi() {
    CHECK(chip_task);
    commissioning.calls.push_back("commit_wifi");
    return commissioning.commit;
}

void stop_dns_sd() {
    CHECK(chip_depth);
    services.push_back("stop_dns_sd");
}
void unregister_attribute_access() {
    CHECK(chip_depth);
    services.push_back("unregister_attribute_access");
}
void shutdown_server() {
    CHECK(chip_depth);
    services.push_back("shutdown_server");
}

}  // namespace amity_matter::chip_port

namespace amity_matter {
void stop_apple_fabric_cleanup() {
    CHECK(controller_test::chip_depth);
    controller_test::services.push_back("stop_apple_fabric_cleanup");
}
}  // namespace amity_matter

extern "C" void __real__ZN4chip3app11DnssdServer11StartServerEv(void *self) {
    CHECK(self == reinterpret_cast<void *>(0x5d));
    ++controller_test::dns_starts;
}

namespace controller_test {

amity_matter_state_t observation(uint8_t input) { return {true, input, true, true, true, 10, 1, 1}; }

// Before Matter starts an update stores the state at once, leaving nothing to
// publish once it starts; a null state and volume 255 are refused.
void before_start() {
    const amity_matter_state_t initial{false, 1, false, false, false, 0, 1, 1};
    CHECK(amity_matter_update_state(&initial) == ESP_OK && state.current_input == 1 && queued() == 0 && reports.empty());
    auto invalid = initial;
    invalid.volume_known = true;
    invalid.volume = 255;
    CHECK(amity_matter_update_state(nullptr) == ESP_ERR_INVALID_ARG);
    CHECK(amity_matter_update_state(&invalid) == ESP_ERR_INVALID_ARG && state.current_input == 1);
    invalid.volume = 254;
    CHECK(amity_matter_update_state(&invalid) == ESP_OK && state.volume == 254);
    invalid.volume_known = false;
    invalid.volume = 255;
    CHECK(amity_matter_update_state(&invalid) == ESP_OK && state.volume == 255);
    CHECK(amity_matter_flush_state() == ESP_OK && schedules == 0 && stack_locks == 0);
    started.store(true);
    CHECK(amity_matter_flush_state() == ESP_OK && schedules == 0 && queued() == 0);
}

// An update while other CHIP work holds the stack lock returns without taking
// it. Updates coalesce into one queued publication of the final observation,
// which reports only the changed attributes.
void coalesced() {
    const amity_matter_state_t initial{false, 1, false, false, false, 0, 1, 1};
    CHECK(amity_matter_update_state(&initial) == ESP_OK);
    started.store(true);
    auto next = observation(2);
    held_elsewhere.store(true);
    {
        std::lock_guard<std::recursive_mutex> blocked(chip_lock);
        std::thread([&] { CHECK(amity_matter_update_state(&next) == ESP_OK); }).join();
    }
    held_elsewhere.store(false);
    CHECK(stack_locks == 0 && state.current_input == 1 && queued() == 1 && reports.empty());
    next.current_input = 3;
    CHECK(amity_matter_update_state(&next) == ESP_OK);
    next.current_input = 4;
    next.volume = 120;
    CHECK(amity_matter_update_state(&next) == ESP_OK);
    CHECK(queued() == 1 && schedules == 1 && state.current_input == 1);
    run_one();
    CHECK(state.current_input == 4 && state.volume == 120 && queued() == 0);
    CHECK(reports == (std::vector<Attribute>{Attribute::power, Attribute::current_input, Attribute::mute, Attribute::volume}));
    reports.clear();
    CHECK(amity_matter_update_state(&next) == ESP_OK);
    run_one();
    CHECK(reports.empty() && stack_locks == 0);
    // Each attribute is reported when only it changes.
    const std::vector<std::pair<void (*)(amity_matter_state_t &), Attribute>> changes{
        {[](amity_matter_state_t &s) { s.power = !s.power; }, Attribute::power},
        {[](amity_matter_state_t &s) { ++s.current_input; }, Attribute::current_input},
        {[](amity_matter_state_t &s) { ++s.config_revision; }, Attribute::input_list},
        {[](amity_matter_state_t &s) { ++s.presentation_revision; }, Attribute::input_list},
        {[](amity_matter_state_t &s) { s.muted = !s.muted; }, Attribute::mute},
        {[](amity_matter_state_t &s) { s.mute_known = !s.mute_known; }, Attribute::mute},
        {[](amity_matter_state_t &s) { ++s.volume; }, Attribute::volume},
        {[](amity_matter_state_t &s) { s.volume_known = !s.volume_known; }, Attribute::volume}};
    for (const auto &[change, attribute] : changes) {
        change(next);
        CHECK(amity_matter_update_state(&next) == ESP_OK);
        run_one();
        CHECK(reports == std::vector<Attribute>{attribute});
        reports.clear();
    }
}

// A refused publication keeps the final state dirty for the background flush,
// also when a newer update races the refusal.
void refused_publication() {
    started.store(true);
    auto next = observation(5);
    refusals = 1;
    CHECK(amity_matter_update_state(&next) == ESP_FAIL && queued() == 0 && state.current_input == 0);
    CHECK(amity_matter_flush_state() == ESP_OK && queued() == 1);
    run_one();
    CHECK(state.current_input == 5 && count(Attribute::current_input) == 1);
    reports.clear();
    next.current_input = 6;
    refusals = 1;
    during_schedule = [&] {
        auto newer = next;
        newer.current_input = 7;
        CHECK(amity_matter_update_state(&newer) == ESP_OK);
    };
    CHECK(amity_matter_update_state(&next) == ESP_FAIL && queued() == 0);
    CHECK(amity_matter_flush_state() == ESP_OK);
    run_one();
    CHECK(state.current_input == 7 && reports == std::vector<Attribute>{Attribute::current_input});
    CHECK(amity_matter_flush_state() == ESP_OK && schedules == 4 && queued() == 0);
}

// The CHIP task may run the publication before schedule() returns, and an
// update may arrive during a report: no publication lock is held then.
void reentrant_publication() {
    started.store(true);
    immediate = true;
    auto next = observation(8);
    CHECK(amity_matter_update_state(&next) == ESP_OK && state.current_input == 8 && queued() == 0);
    immediate = false;
    reports.clear();
    next.current_input = 9;
    during_report = [&] {
        auto newer = next;
        newer.current_input = 10;
        CHECK(amity_matter_update_state(&newer) == ESP_OK);
    };
    CHECK(amity_matter_update_state(&next) == ESP_OK);
    run_one();
    CHECK(state.current_input == 9 && queued() == 1);
    run_one();
    CHECK(state.current_input == 10 && reports.size() == 2);
}

// Revocation fences queued publications and later updates.
void revoked_publication() {
    started.store(true);
    auto next = observation(11);
    CHECK(amity_matter_update_state(&next) == ESP_OK && queued() == 1);
    controller_enabled.store(false);
    run_one();
    CHECK(state.current_input == 0 && reports.empty());
    controller_enabled.store(true);
    CHECK(amity_matter_flush_state() == ESP_OK && queued() == 0);
    controller_enabled.store(false);
    next.current_input = 12;
    CHECK(amity_matter_update_state(&next) == ESP_OK && amity_matter_flush_state() == ESP_OK);
    CHECK(state.current_input == 0 && queued() == 0 && reports.empty() && stack_locks == 0);
    started.store(false);
    CHECK(amity_matter_update_state(&next) == ESP_OK && state.current_input == 0);
}

// A state left dirty by a refused publication is not retried once the
// controller is revoked, even while its services are still stopping.
void revoked_retry() {
    started.store(true);
    auto next = observation(13);
    refusals = 1;
    CHECK(amity_matter_update_state(&next) == ESP_FAIL && schedules == 1 && queued() == 0);
    controller_enabled.store(false);
    CHECK(amity_matter_flush_state() == ESP_OK && schedules == 1 && queued() == 0);
    CHECK(state.current_input == 0 && reports.empty());
}

// Inputs come from the saved configuration through the callback for each
// read, bounded by the list's capacity. A configuration change republishes
// the input list once; a playback change does not.
void input_list() {
    started.store(true);
    callbacks.inputs = [](amity_matter_input_t *inputs, size_t capacity, void *context) -> size_t {
        CHECK(capacity == AMITY_MATTER_MAX_INPUTS && context == reinterpret_cast<void *>(0x1c));
        std::strcpy(inputs[0].name, "Movies");
        inputs[0].index = 1;
        std::strcpy(inputs[1].name, "Music");
        inputs[1].index = 2;
        return 2;
    };
    callbacks.context = reinterpret_cast<void *>(0x1c);
    CHECK(current_inputs() == 2 && std::strcmp(listed_inputs[0].name, "Movies") == 0 && listed_inputs[1].index == 2);
    callbacks.inputs = [](amity_matter_input_t *, size_t capacity, void *) { return capacity + 1; };
    CHECK(current_inputs() == 0);
    callbacks.inputs = [](amity_matter_input_t *, size_t capacity, void *) { return capacity; };
    CHECK(current_inputs() == AMITY_MATTER_MAX_INPUTS);
    callbacks.inputs = nullptr;
    CHECK(current_inputs() == 0);
    auto next = observation(0);
    for (const auto change : {+[](amity_matter_state_t &s) { ++s.config_revision; },
                              +[](amity_matter_state_t &s) { ++s.presentation_revision; }}) {
        change(next);
        CHECK(amity_matter_update_state(&next) == ESP_OK);
        run_one();
        CHECK(count(Attribute::input_list) == 1);
        reports.clear();
    }
    ++next.current_input;
    CHECK(amity_matter_update_state(&next) == ESP_OK);
    run_one();
    CHECK(!reports.empty() && count(Attribute::input_list) == 0);
}

// Keys reach the callback only while the controller is enabled; attribute
// reports and the DNS-SD start only while it runs.
void fenced_entry_points() {
    CHECK(dispatch_key(0x44) == ESP_ERR_INVALID_STATE);
    callbacks.key = [](uint8_t key, void *context) {
        controller_test::keys.emplace_back(key, context);
        return key == 0x45 ? ESP_FAIL : ESP_OK;
    };
    callbacks.context = reinterpret_cast<void *>(0x2b);
    CHECK(dispatch_key(0x44) == ESP_OK && dispatch_key(0x45) == ESP_FAIL);
    CHECK(keys == (std::vector<std::pair<uint8_t, void *>>{{0x44, callbacks.context}, {0x45, callbacks.context}}));
    __wrap__ZN4chip3app11DnssdServer11StartServerEv(reinterpret_cast<void *>(0x5d));
    CHECK(dns_starts == 1);
    {
        std::lock_guard<std::recursive_mutex> guard(chip_lock);
        ++chip_depth;
        amity_matter::report(Attribute::playback_state);
        CHECK(reports.empty());
        started.store(true);
        amity_matter::report(Attribute::playback_state);
        CHECK(reports == std::vector<Attribute>{Attribute::playback_state});
        controller_enabled.store(false);
        amity_matter::report(Attribute::power);
        CHECK(reports.size() == 1);
        --chip_depth;
    }
    CHECK(dispatch_key(0x44) == ESP_ERR_INVALID_STATE && keys.size() == 2);
    __wrap__ZN4chip3app11DnssdServer11StartServerEv(reinterpret_cast<void *>(0x5d));
    CHECK(dns_starts == 1);
}

// Enrollment completes only when the controller runs, the fail-safe is
// disarmed, a fabric exists, the station is provisioned and connected, and
// the Wi-Fi commit succeeds. One check is queued at a time, and its result is
// latched instead of being derived again.
void enrollment() {
    CHECK(!amity_matter_controller_enrollment_complete() && schedules == 0);
    started.store(true);
    const auto check = [](bool expected) {
        commissioning.calls.clear();
        CHECK(!amity_matter_controller_enrollment_complete() && queued() == 1);
        CHECK(!amity_matter_controller_enrollment_complete() && queued() == 1);
        run_one();
        CHECK(amity_matter_controller_enrollment_complete() == expected);
    };
    commissioning.disarmed = false;
    check(false);
    CHECK(commissioning.calls == std::vector<std::string>{"fail_safe_disarmed"});
    commissioning.disarmed = true;
    commissioning.fabric = false;
    check(false);
    CHECK(commissioning.calls == (std::vector<std::string>{"fail_safe_disarmed", "has_fabric"}));
    commissioning.fabric = true;
    commissioning.provisioned = false;
    check(false);
    CHECK(commissioning.calls.back() == "station_provisioned");
    commissioning.provisioned = true;
    commissioning.connected = false;
    check(false);
    CHECK(commissioning.calls.back() == "station_connected" && !amity_fake::logged("commit failed"));
    commissioning.connected = true;
    commissioning.commit = false;
    check(false);
    CHECK(commissioning.calls.back() == "commit_wifi");
    CHECK(amity_fake::logged("Controller enrollment Wi-Fi commit failed; user interfaces remain closed"));
    commissioning.commit = true;
    check(true);
    CHECK(commissioning.calls.size() == 5);
    // The latch holds even when the fabric goes away; nothing more is queued.
    commissioning.fabric = false;
    const auto before = schedules;
    CHECK(amity_matter_controller_enrollment_complete() && schedules == before);
    controller_enabled.store(false);
    CHECK(!amity_matter_controller_enrollment_complete());
}

// A refused check can be queued again, and queued work does nothing once the
// controller stops.
void enrollment_fenced() {
    started.store(true);
    refusals = 1;
    CHECK(!amity_matter_controller_enrollment_complete() && queued() == 0 && schedules == 1);
    CHECK(!amity_matter_controller_enrollment_complete() && queued() == 1);
    controller_enabled.store(false);
    run_one();
    CHECK(commissioning.calls.empty());
    controller_enabled.store(true);
    started.store(false);
    CHECK(!amity_matter_controller_enrollment_complete() && queued() == 0);
    started.store(true);
    CHECK(!amity_matter_controller_enrollment_complete() && queued() == 1);
    started.store(false);
    run_one();
    CHECK(commissioning.calls.empty());
}

// Disabling revokes the controller and latches the revocation. A running
// controller stops DNS-SD and attribute access and Apple cleanup before the
// server shuts down, all under the stack lock, and the shared host stays.
void revocation() {
    CHECK(amity_matter_set_controller_enabled(true) == ESP_OK && controller_enabled.load());
    stack_started.store(true);
    started.store(true);
    amity_fake::on_log([](const amity_fake::LogLine &line) { CHECK(chip_depth); (void)line; });
    CHECK(amity_matter_set_controller_enabled(false) == ESP_OK && !controller_enabled.load() && !started.load());
    CHECK(services == (std::vector<std::string>{"stop_dns_sd", "unregister_attribute_access", "stop_apple_fabric_cleanup",
                                                "shutdown_server"}));
    CHECK(stack_locks == 1 && stack_started.load());
    CHECK(amity_fake::logged("Matter user services stopped; fabrics and shared network/BLE host retained"));
    CHECK(amity_matter_set_controller_enabled(false) == ESP_OK && services.size() == 4 && stack_locks == 1);
    CHECK(amity_matter_set_controller_enabled(true) == ESP_ERR_INVALID_STATE && !controller_enabled.load());
}

// Disabling before the stack or the controller starts revokes without
// stopping services; enabling stays refused.
void revocation_before_start() {
    started.store(true);
    CHECK(amity_matter_set_controller_enabled(false) == ESP_OK && services.empty() && stack_locks == 0);
    CHECK(started.load() && !controller_enabled.load());
    CHECK(amity_matter_set_controller_enabled(true) == ESP_ERR_INVALID_STATE);
    stack_started.store(true);
    started.store(false);
    CHECK(amity_matter_set_controller_enabled(false) == ESP_OK && services.empty() && stack_locks == 0);
}

// Disabling during startup revokes at once, then waits for startup to finish
// before stopping what it started.
void revocation_during_startup() {
    std::mutex gate;
    std::condition_variable changed;
    bool starting = false, finish = false;
    std::thread startup([&] {
        std::lock_guard<std::mutex> lifecycle(lifecycle_mutex);
        {
            std::unique_lock<std::mutex> lock(gate);
            starting = true;
            changed.notify_all();
            changed.wait(lock, [&] { return finish; });
        }
        stack_started.store(true);
        started.store(true);
    });
    {
        std::unique_lock<std::mutex> lock(gate);
        changed.wait(lock, [&] { return starting; });
    }
    std::atomic<bool> done{false};
    std::thread revoke([&] {
        CHECK(amity_matter_set_controller_enabled(false) == ESP_OK);
        done.store(true);
    });
    while (controller_enabled.load()) std::this_thread::yield();
    CHECK(!done.load() && services.empty());
    {
        std::lock_guard<std::mutex> lock(gate);
        finish = true;
    }
    changed.notify_all();
    startup.join();
    revoke.join();
    CHECK(done.load() && !started.load() && services.size() == 4);
}

}  // namespace controller_test

int main(int argc, char **argv) {
    using namespace controller_test;
    return amity_test::run(argc, argv,
                           {{"before-start", before_start},
                            {"coalesced", coalesced},
                            {"refused-publication", refused_publication},
                            {"reentrant-publication", reentrant_publication},
                            {"revoked-publication", revoked_publication},
                            {"revoked-retry", revoked_retry},
                            {"input-list", input_list},
                            {"fenced-entry-points", fenced_entry_points},
                            {"enrollment", enrollment},
                            {"enrollment-fenced", enrollment_fenced},
                            {"revocation", revocation},
                            {"revocation-before-start", revocation_before_start},
                            {"revocation-during-startup", revocation_during_startup}});
}
