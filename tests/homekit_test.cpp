/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

// HomeKit's worker task, lifecycle and accessory, built from the whole of
// components/amity_homekit/homekit.cpp against the core fakes, a recording
// fake of the HomeKit SDK and strict seams for everything else it calls.
#include "homekit.cpp"
#include "homekit_harness.hpp"
#include "scenario_main.hpp"
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace hap = amity_fake::hap;
using namespace homekit_harness;
using amity_seam::count;
using amity_seam::install;

constexpr int64_t second_us = 1000000;
// HomeKit's Television category, which the SDK names no constant for.
constexpr auto television_cid = static_cast<hap_cid_t>(31);
constexpr auto external_index = static_cast<std::optional<unsigned> (amity::SystemControl::*)(const amity::PlaybackState &)
                                                const>(&amity::SystemControl::external_activity_index);

// What HomeKit writes reach through the Matter callbacks main.cpp passes to start().
struct Handlers {
    std::vector<std::string> calls;
    esp_err_t result = ESP_OK;
};
Handlers handlers;

esp_err_t handled(void *context, std::string call) {
    CHECK(context == &handlers);
    handlers.calls.push_back(std::move(call));
    return handlers.result;
}

amity_matter_callbacks_t callbacks() {
    amity_matter_callbacks_t value{};
    value.power = [](bool on, void *context) { return handled(context, on ? "power on" : "power off"); };
    value.activity = [](uint8_t index, void *context) { return handled(context, "activity " + std::to_string(index)); };
    value.key = [](uint8_t key, void *context) { return handled(context, "key " + std::to_string(key)); };
    value.mute = [](bool muted, void *context) { return handled(context, muted ? "mute" : "unmute"); };
    value.volume_step = [](int16_t steps, void *context) { return handled(context, "volume " + std::to_string(steps)); };
    value.context = &handlers;
    return value;
}

// The SystemControl members homekit.cpp calls, for a hub with no saved
// configuration and the given number of activities.
void install_hub(const amity::SystemControl &hub, unsigned activities = 0) {
    install<external_index>([&hub](const amity::SystemControl &self, const amity::PlaybackState &playback) {
        CHECK(&self == &hub);
        return playback.current_activity < 0 ? std::nullopt : std::optional<unsigned>(unsigned(playback.current_activity));
    });
    install<&amity::HubService::snapshot>([activities](const amity::HubService &) {
        amity::Snapshot snapshot;
        snapshot.activities.resize(activities);
        return snapshot;
    });
}

std::vector<int64_t> &deadlines() {
    static std::vector<int64_t> recorded;
    return recorded;
}
void record_deadlines() {
    install<&hap_amity_set_pairing_deadline>([](int64_t deadline) { deadlines().push_back(deadline); });
}

// The worker task left when start() returned, so this runs worker_loop()
// again on this thread for the passes the task would have gone on with: it
// repeats the startup, then leaves at the pass after the last of passes.
void run_worker(std::deque<std::function<void()>> passes) {
    state().passes = std::move(passes);
    try {
        amity_homekit::worker_loop();
    } catch (const amity_fake::LeaveTask &) {
        return;
    }
}

bool cleared(const amity_homekit::Status &status) { return !status.manual_code[0] && !status.qr_payload[0]; }

// start() creates the homekit task with an 8192-byte stack in PSRAM at
// priority 3, waits for its startup and returns the worker's result. A
// second start is refused.
void start_worker() {
    install_defaults();
    amity::SystemControl hub;
    // Nothing is published before the worker's first pass.
    CHECK(start(hub, callbacks(), {[] { CHECK(!amity_homekit::status().available); }}) == ESP_OK);
    const auto tasks = amity_fake::tasks();
    CHECK(tasks.size() == 1);
    CHECK(tasks[0].name == "homekit" && tasks[0].stack_bytes == 8192 && tasks[0].priority == 3);
    CHECK(tasks[0].argument == nullptr && tasks[0].core == tskNO_AFFINITY);
    CHECK(tasks[0].with_caps && tasks[0].caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    // The worker ran its startup before start() returned, and left at the
    // scenario's request; it never returns or deletes itself.
    CHECK(tasks[0].state == amity_fake::Task::State::left);
    CHECK(count("hap_amity_init") == 1 && count("config_store::read") == 1);
    // Each pass without a command waits a second for one.
    CHECK(amity_fake::now_us() == 1000 + 2 * second_us);
    const auto queues = amity_fake::queues();
    CHECK(queues.size() == 3);
    CHECK(queues[0].kind == amity_fake::Queue::Kind::mutex);
    CHECK(queues[1].kind == amity_fake::Queue::Kind::queue && queues[1].length == 1 &&
          queues[1].item_size == sizeof(amity_homekit::Command));
    CHECK(queues[2].kind == amity_fake::Queue::Kind::binary);
    // The first pass published the network, with nothing started.
    const auto status = amity_homekit::status();
    CHECK(status.available && !status.started && !status.pairing && !status.busy && status.error == ESP_OK);
    CHECK(hap::setup_calls().init == 0);
    CHECK(start(hub, callbacks()) == ESP_ERR_INVALID_STATE);
    CHECK(amity_fake::tasks().size() == 1 && count("hap_amity_init") == 1);
}

// Without the HAP lock (hap_amity_init failed) start() fails before creating
// the task; stop() still stops the SDK side.
void init_failure() {
    install_defaults();
    install<&hap_amity_init>([](const char *) { return HAP_FAIL; });
    amity::SystemControl hub;
    CHECK(start(hub, callbacks()) == ESP_ERR_NO_MEM);
    CHECK(amity_fake::tasks().empty() && amity_fake::now_us() == 0);
    CHECK(amity_homekit::stop() == ESP_OK && count("hap_amity_stop") == 1);
}

// A task that cannot be created fails start() without waiting, and nothing
// is started.
void task_creation_failure() {
    install_defaults();
    amity::SystemControl hub;
    amity_fake::fail_task_creations(1);
    CHECK(start(hub, callbacks()) == ESP_ERR_NO_MEM);
    CHECK(amity_fake::tasks().empty() && amity_fake::now_us() == 0);
    CHECK(hap::setup_calls().init == 0 && hap::setup_calls().start == 0);
    CHECK(!amity_homekit::status().started);
}

// A worker that never signals its startup fails start() after 30 seconds.
void startup_timeout() {
    install_defaults();
    amity::SystemControl hub;
    CHECK(amity_homekit::start(hub, callbacks()) == ESP_ERR_TIMEOUT);
    CHECK(amity_fake::now_us() == 30 * second_us);
    const auto tasks = amity_fake::tasks();
    CHECK(tasks.size() == 1 && tasks[0].state == amity_fake::Task::State::created);
}

// start() returns the worker's startup result: a failed read of the saved
// setup, or a saved setup HomeKit cannot start with.
void startup_error() {
    install_defaults();
    amity::SystemControl hub;
    state().read_errors["hap_setup"] = ESP_ERR_NVS_NOT_INITIALIZED;
    CHECK(start(hub, callbacks()) == ESP_ERR_NVS_NOT_INITIALIZED);
    CHECK(hap::setup_calls().init == 0);
}
void startup_initialize_error() {
    install_defaults();
    amity::SystemControl hub;
    state().saved["hap_setup"] = setup_record().substr(0, 403);
    CHECK(start(hub, callbacks()) == ESP_ERR_INVALID_SIZE);
    CHECK(hap::setup_calls().init == 0);
    const auto status = amity_homekit::status();
    CHECK(!status.started && status.error == ESP_ERR_INVALID_SIZE);
}

// stop() before start() disables HomeKit for good without touching the SDK.
void stop_before_start() {
    install_defaults();
    amity::SystemControl hub;
    CHECK(amity_homekit::stop() == ESP_OK && count("hap_amity_stop") == 0);
    CHECK(start(hub, callbacks()) == ESP_ERR_INVALID_STATE);
    CHECK(amity_fake::tasks().empty() && count("hap_amity_init") == 0);
    CHECK(amity_homekit::open_pairing() == ESP_ERR_INVALID_STATE);
    const auto status = amity_homekit::status();
    CHECK(!status.started && !status.available && !status.pairing);
}

// stop() from another task fences HomeKit before it waits for a start() in
// progress: the worker, starting after the fence, starts nothing and
// publishes nothing as started or available, and stop() finishes once
// start() has returned.
void stop_during_start() {
    install_defaults();
    amity::SystemControl hub;
    install_hub(hub);
    state().saved["hap_setup"] = setup_record();
    struct Stopper {
        std::thread task;
        esp_err_t result = ESP_FAIL;
    } stopper;
    // Before the worker runs, the other task calls stop(), which waits for
    // the lifecycle lock start() holds.
    esp_timer_create_args_t args{};
    args.callback = [](void *argument) {
        auto &other = *static_cast<Stopper *>(argument);
        other.task = std::thread([&other] { other.result = amity_homekit::stop(); });
        while (!amity_homekit::disabled.load()) std::this_thread::yield();
    };
    args.arg = &stopper;
    args.name = "stopper";
    esp_timer_handle_t timer = nullptr;
    CHECK(esp_timer_create(&args, &timer) == ESP_OK && esp_timer_start_once(timer, 500) == ESP_OK);
    CHECK(start(hub, callbacks(), {[] {
              const auto status = amity_homekit::status();
              CHECK(!status.started && !status.available && status.error == ESP_ERR_INVALID_STATE);
          }}) == ESP_ERR_INVALID_STATE);
    stopper.task.join();
    CHECK(stopper.result == ESP_OK && count("hap_amity_stop") == 1 && hap::setup_calls().init == 0);
    const auto status = amity_homekit::status();
    CHECK(!status.started && !status.available && status.error == ESP_OK);
    CHECK(esp_timer_delete(timer) == ESP_OK);
}

// A saved setup is restored at startup: HomeKit is configured for 16
// notifying characteristics, started on Wi-Fi with the saved setup ID and
// verifier, and its status published before start() returns.
void restore_saved_setup() {
    install_defaults();
    amity::SystemControl hub;
    install_hub(hub);
    state().saved["hap_setup"] = setup_record("SAVE", 9);
    CHECK(start(hub, callbacks()) == ESP_OK);
    const auto calls = hap::setup_calls();
    CHECK(calls.get_config == 1 && calls.set_config == 1 && calls.init == 1 && calls.start == 1);
    CHECK(calls.set_setup_id == 1 && calls.set_setup_info == 1);
    const auto configuration = hap::configuration();
    CHECK(configuration.max_event_notif_chars == 16);
    CHECK(configuration.task_stack_size == 4096 && configuration.task_priority == 7 &&
          configuration.unique_param == UNIQUE_SSID && configuration.sw_token_max_len == 1200);
    CHECK(hap::transport() == HAP_TRANSPORT_WIFI && hap::setup_id() == "SAVE");
    const auto info = hap::setup_info();
    const auto saved = setup_info(9);
    CHECK(std::memcmp(&info, &saved, sizeof(info)) == 0);
    const auto status = amity_homekit::status();
    CHECK(status.started && status.available && !status.pairing && !status.paired && !status.busy);
    CHECK(status.controllers == 0 && status.error == ESP_OK && cleared(status));
    CHECK(state().hap_lock == 0 && state().writes.empty());
    CHECK(television().characteristic("E3").text == "Amity");
    // The restored values came from the hub start() was given.
    CHECK(count("amity::SystemControl::external_activity_index") >= 1);
}

// The accessory: a television with a speaker, a remote battery and 16 input
// sources, each with its read and write callbacks.
void accessory() {
    install_defaults();
    amity::SystemControl hub;
    install_hub(hub);
    state().saved["hap_setup"] = setup_record();
    state().saved["hap_name"] = "Den";
    CHECK(start(hub, callbacks()) == ESP_OK);
    const auto accessories = hap::accessories();
    CHECK(accessories.size() == 1);
    const auto &record = *accessories[0];
    CHECK(record.name == "Amity" && record.model == "Amity Board / Fin" && record.manufacturer == "Amity");
    CHECK(record.serial == "246F28A1B2C3" && record.firmware == esp_app_get_description()->version);
    CHECK(record.hardware == "1" && record.protocol == "1.1" && record.added);
    CHECK(record.category == television_cid);
    CHECK(record.identify && record.identify(nullptr) == HAP_SUCCESS);
    const auto services = hap::services();
    CHECK(services.size() == 19 && record.services == services);
    CHECK(services[0]->uuid == "D8" && services[1]->uuid == "113" && services[2]->uuid == "96");
    for (const auto *service : services)
        CHECK(service->read == amity_homekit::read && service->write == amity_homekit::write);
    auto &tv = television();
    CHECK(tv.primary && tv.linked.size() == 17 && tv.linked[0] == &speaker());
    std::vector<std::string> types;
    for (const auto *characteristic : tv.characteristics) types.push_back(characteristic->uuid);
    CHECK((types == std::vector<std::string>{"B0", "E7", "E3", "E1", "E8", "136"}));
    const auto permissions = [](const hap::Characteristic &c) { return c.permissions; };
    const uint16_t notify = HAP_CHAR_PERM_PR | HAP_CHAR_PERM_EV, writable = notify | HAP_CHAR_PERM_PW;
    CHECK(permissions(tv.characteristic("B0")) == writable && tv.characteristic("B0").max == 1);
    CHECK(permissions(tv.characteristic("E7")) == writable && tv.characteristic("E7").format == hap::Format::uint32);
    CHECK(tv.characteristic("E3").text == "Den" && tv.characteristic("E3").maxlen == 63);
    CHECK(permissions(tv.characteristic("E1")) == HAP_CHAR_PERM_PW && tv.characteristic("E1").max == 16);
    CHECK(permissions(tv.characteristic("E8")) == notify && tv.characteristic("E8").number() == 1);
    CHECK(permissions(tv.characteristic("136")) == notify && tv.characteristic("136").format == hap::Format::tlv8);
    auto &sound = speaker();
    CHECK(permissions(sound.characteristic("B0")) == notify);
    CHECK(permissions(sound.characteristic("11A")) == writable && sound.characteristic("11A").format == hap::Format::boolean);
    CHECK(sound.characteristic("E9").number() == 1 && sound.characteristic("E9").max == 3);
    CHECK(permissions(sound.characteristic("EA")) == HAP_CHAR_PERM_PW && sound.characteristic("EA").max == 1);
    auto &remote = battery();
    CHECK(remote.characteristic("68").max == 100 && remote.characteristic("79").max == 1);
    CHECK(remote.characteristic("8F").max == 2 && remote.characteristic("23").text == "Remote battery");
    for (unsigned slot = 0; slot < 16; ++slot) {
        auto &source = input(slot);
        CHECK(tv.linked[slot + 1] == &source);
        CHECK(source.characteristic("E6").number() == slot + 1 && permissions(source.characteristic("E6")) == HAP_CHAR_PERM_PR);
        CHECK(source.characteristic("E3").text == "Unused " + std::to_string(slot + 1));
        CHECK(permissions(source.characteristic("E3")) == writable && source.characteristic("E3").maxlen == 63);
        CHECK(source.characteristic("23").text == source.characteristic("E3").text);
        CHECK(source.characteristic("D6").number() == 0 && source.characteristic("135").number() == 1);
        CHECK(permissions(source.characteristic("134")) == writable && source.characteristic("134").number() == 1);
        CHECK(source.characteristic("DB").number() == 3);
    }
    // The display order lists every input identifier, in identifier order
    // while nothing is saved.
    std::vector<uint8_t> order;
    for (uint8_t identifier = 1; identifier <= 16; ++identifier) {
        if (identifier > 1) order.insert(order.end(), {0, 0});
        order.insert(order.end(), {1, 1, identifier});
    }
    CHECK(tv.characteristic("136").bytes() == order);
}

// Open publishes a new setup code and QR payload for 300 seconds and
// refreshes discovery; Remove and Close end the pairing window. Each pass
// refreshes the published values while HomeKit runs.
void open_pairing() {
    install_defaults();
    amity::SystemControl hub;
    install_hub(hub);
    record_deadlines();
    // Codes HomeKit forbids are drawn again; the setup ID is drawn afresh.
    state().random = {12345678, 87654321, 22222222, 0, 4111222333u, 10, 22, 29, 41};
    int64_t opened = 0;
    std::deque<std::function<void()>> passes;
    // Pairing cannot open before the network is published.
    passes.push_back([] { CHECK(amity_homekit::open_pairing() == ESP_ERR_INVALID_STATE); });
    passes.push_back([] {
        CHECK(amity_homekit::open_pairing() == ESP_OK && amity_homekit::status().busy);
        CHECK(amity_homekit::open_pairing() == ESP_ERR_INVALID_STATE);
    });
    passes.push_back([&] { opened = amity_fake::now_us(); });
    passes.push_back([&] {
        CHECK((deadlines() == std::vector<int64_t>{opened + 300 * second_us}));
        CHECK(count("hap_amity_refresh_mdns") == 1);
        const auto status = amity_homekit::status();
        CHECK(status.started && status.pairing && !status.busy && status.error == ESP_OK);
        CHECK(std::string(status.manual_code) == "112-22-333" && status.qr_payload == hap::payload());
        const auto requests = hap::payload_requests();
        CHECK(requests.size() == 1 && requests[0].code == "112-22-333" && requests[0].id == "AMT5");
        CHECK(!requests[0].wac && requests[0].category == television_cid);
        hap_setup_info_t generated = setup_info(uint8_t('1' + '3'));
        std::memcpy(generated.verifier, "112-22-333", 10);
        const auto info = hap::setup_info();
        CHECK(std::memcmp(&info, &generated, sizeof(info)) == 0 && hap::setup_id() == "AMT5");
        CHECK(state().saved["hap_setup"] == "AMT5" + std::string(reinterpret_cast<const char *>(&generated), sizeof(generated)));
        CHECK(hap::setup_calls().start == 1 && television().characteristic("B0").number() == 0);
        state().playback.current_activity = 2;
        CHECK(amity_homekit::remove_pairings() == ESP_OK);
    });
    passes.push_back([] {});
    passes.push_back([] {
        CHECK(count("hap_amity_remove_pairings") == 1 && deadlines().back() == 0);
        const auto status = amity_homekit::status();
        CHECK(!status.pairing && !status.busy && cleared(status));
        CHECK(television().characteristic("B0").number() == 1 && television().characteristic("E7").number() == 3);
        CHECK(amity_homekit::close_pairing() == ESP_OK);
    });
    passes.push_back([] {});
    passes.push_back([] {
        CHECK(count("hap_amity_remove_pairings") == 1 && count("hap_amity_refresh_mdns") == 2);
        CHECK(deadlines().size() == 3 && deadlines().back() == 0 && !amity_homekit::status().busy);
    });
    CHECK(start(hub, callbacks(), std::move(passes)) == ESP_OK);
    CHECK(state().loop_passes == 9 && count("config_store::write") == 1);
}

// A command that finds the queue full is refused at once with
// ESP_ERR_NO_MEM, without waiting for the worker, and leaves HomeKit free to
// take the next one.
void full_command_queue() {
    install_defaults();
    amity::SystemControl hub;
    install_hub(hub);
    std::deque<std::function<void()>> passes;
    passes.push_back([] {});
    passes.push_back([] {
        const auto queued = amity_homekit::Command::Close;
        CHECK(xQueueSend(amity_homekit::commands, &queued, 0) == pdTRUE);
        const int64_t before = amity_fake::now_us();
        CHECK(amity_homekit::open_pairing() == ESP_ERR_NO_MEM && amity_fake::now_us() == before);
        const auto status = amity_homekit::status();
        CHECK(!status.busy && !status.pairing && status.error == ESP_OK);
    });
    // This pass's wait took the queued Close, so there is room again.
    passes.push_back([] { CHECK(amity_homekit::open_pairing() == ESP_OK && amity_homekit::status().busy); });
    passes.push_back([] {});
    passes.push_back([] {
        const auto status = amity_homekit::status();
        CHECK(status.pairing && !status.busy && status.error == ESP_OK);
    });
    CHECK(start(hub, callbacks(), std::move(passes)) == ESP_OK);
    CHECK(state().loop_passes == 6);
}

// The pairing window ends when its 300 seconds are up: status() hides it at
// once, and the worker's next pass closes it and refreshes discovery.
void pairing_expiry() {
    install_defaults();
    amity::SystemControl hub;
    install_hub(hub);
    record_deadlines();
    std::deque<std::function<void()>> passes;
    passes.push_back([] {});
    passes.push_back([] { CHECK(amity_homekit::open_pairing() == ESP_OK); });
    passes.push_back([] {});
    passes.push_back([] {
        const int64_t deadline = deadlines().at(0);
        amity_fake::advance_us(deadline - 1 - amity_fake::now_us());
        CHECK(amity_homekit::status().pairing);
        amity_fake::advance_us(1);
        const auto status = amity_homekit::status();
        CHECK(!status.pairing && cleared(status));
    });
    passes.push_back([] {
        CHECK(deadlines().size() == 2 && deadlines().back() == 0 && count("hap_amity_refresh_mdns") == 2);
        CHECK(!amity_homekit::status().pairing);
    });
    CHECK(start(hub, callbacks(), std::move(passes)) == ESP_OK);
}

// A controller that pairs ends the window at the next pass, and an Open
// that arrives once a controller is paired is refused.
void paired_closes_window() {
    install_defaults();
    amity::SystemControl hub;
    install_hub(hub);
    record_deadlines();
    std::deque<std::function<void()>> passes;
    passes.push_back([] {});
    passes.push_back([] { CHECK(amity_homekit::open_pairing() == ESP_OK); });
    passes.push_back([] {});
    passes.push_back([] { hap::set_paired_controllers(1); });
    passes.push_back([] {
        CHECK(deadlines().size() == 2 && deadlines().back() == 0 && count("hap_amity_refresh_mdns") == 2);
        const auto status = amity_homekit::status();
        CHECK(status.paired && status.controllers == 1 && !status.pairing && cleared(status));
        CHECK(amity_homekit::open_pairing() == ESP_ERR_INVALID_STATE);
    });
    CHECK(start(hub, callbacks(), std::move(passes)) == ESP_OK);
}
void open_while_paired() {
    install_defaults();
    amity::SystemControl hub;
    install_hub(hub);
    state().saved["hap_setup"] = setup_record();
    std::deque<std::function<void()>> passes;
    passes.push_back([] {
        CHECK(amity_homekit::open_pairing() == ESP_OK);
        hap::set_paired_controllers(2);
    });
    passes.push_back([] {});
    passes.push_back([] {
        const auto status = amity_homekit::status();
        CHECK(status.error == ESP_ERR_INVALID_STATE && !status.busy && !status.pairing && status.controllers == 2);
        CHECK(count("esp_random") == 0 && hap::setup_calls().payloads == 0);
        CHECK(amity_fake::logged("HomeKit operation: ESP_ERR_INVALID_STATE"));
    });
    CHECK(start(hub, callbacks(), std::move(passes)) == ESP_OK);
}

// An Open is refused without a network or a setup code; the error stays
// published until the next command.
void open_refused() {
    install_defaults();
    amity::SystemControl hub;
    install_hub(hub);
    record_deadlines();
    const auto generate = amity_seam::behaviour<&amity_homekit_generate_setup>();
    std::deque<std::function<void()>> passes;
    passes.push_back([] {});
    passes.push_back([] { CHECK(amity_homekit::open_pairing() == ESP_OK); });
    passes.push_back([] { state().network = false; });
    passes.push_back([] {
        const auto status = amity_homekit::status();
        CHECK(status.error == ESP_ERR_INVALID_STATE && !status.available && !status.busy);
        CHECK(deadlines().empty() && hap::setup_calls().payloads == 0);
        state().network = true;
    });
    passes.push_back([] {
        const auto status = amity_homekit::status();
        CHECK(status.error == ESP_ERR_INVALID_STATE && status.available);
        install<&amity_homekit_generate_setup>([](const char *, hap_setup_info_t *) { return ESP_ERR_NO_MEM; });
        CHECK(amity_homekit::open_pairing() == ESP_OK);
    });
    passes.push_back([] {});
    passes.push_back([&generate] {
        const auto status = amity_homekit::status();
        CHECK(status.error == ESP_ERR_NO_MEM && !status.busy && !status.pairing);
        CHECK(deadlines().empty() && state().writes.empty() && hap::setup_calls().init == 0);
        install<&amity_homekit_generate_setup>(generate);
        state().write_errors["hap_setup"] = ESP_ERR_NVS_NOT_INITIALIZED;
        CHECK(amity_homekit::open_pairing() == ESP_OK);
    });
    passes.push_back([] {});
    passes.push_back([] {
        CHECK(amity_homekit::status().error == ESP_ERR_NVS_NOT_INITIALIZED);
        CHECK(deadlines().empty() && hap::setup_calls().init == 0);
    });
    CHECK(start(hub, callbacks(), std::move(passes)) == ESP_OK);
}

// An Open whose HomeKit start, setup payload or discovery refresh fails ends
// the window it opened and reports the error; the next command clears it.
void open_failures() {
    install_defaults();
    amity::SystemControl hub;
    install_hub(hub);
    record_deadlines();
    std::deque<std::function<void()>> passes;
    passes.push_back([] {});
    passes.push_back([] {
        CHECK(amity_homekit::open_pairing() == ESP_OK);
        hap::fail("hap_init");
    });
    passes.push_back([] {});
    passes.push_back([] {
        const auto status = amity_homekit::status();
        CHECK(status.error == ESP_FAIL && !status.pairing && !status.busy && !status.started);
        CHECK(deadlines().size() == 2 && deadlines().back() == 0 && count("hap_amity_refresh_mdns") == 1);
        CHECK(hap::setup_calls().payloads == 0);
        CHECK(amity_homekit::open_pairing() == ESP_OK);
        hap::fail("esp_hap_get_setup_payload");
    });
    passes.push_back([] {});
    passes.push_back([] {
        const auto status = amity_homekit::status();
        CHECK(status.error == ESP_ERR_NO_MEM && status.started && !status.pairing && cleared(status));
        CHECK(deadlines().size() == 4 && deadlines().back() == 0 && count("hap_amity_refresh_mdns") == 2);
        CHECK(hap::setup_calls().payloads == 1);
        install<&hap_amity_refresh_mdns>([] { return HAP_FAIL; });
        CHECK(amity_homekit::open_pairing() == ESP_OK);
    });
    passes.push_back([] {});
    passes.push_back([] {
        const auto status = amity_homekit::status();
        CHECK(status.error == ESP_FAIL && !status.pairing && cleared(status));
        CHECK(deadlines().size() == 6 && deadlines().back() == 0 && count("hap_amity_refresh_mdns") == 4);
        install<&hap_amity_refresh_mdns>([] { return HAP_SUCCESS; });
        CHECK(amity_homekit::close_pairing() == ESP_OK);
    });
    passes.push_back([] {});
    passes.push_back([] {
        const auto status = amity_homekit::status();
        CHECK(status.error == ESP_OK && !status.busy && count("hap_amity_refresh_mdns") == 5);
    });
    CHECK(start(hub, callbacks(), std::move(passes)) == ESP_OK);
}

// A saved setup keeps its setup ID when pairing reopens, and HomeKit, already
// started, takes the new verifier without starting again.
void reopen_with_saved_setup() {
    install_defaults();
    amity::SystemControl hub;
    install_hub(hub);
    state().saved["hap_setup"] = setup_record("KEEP", 3);
    std::deque<std::function<void()>> passes;
    passes.push_back([] { CHECK(amity_homekit::open_pairing() == ESP_OK); });
    passes.push_back([] {});
    passes.push_back([] {
        const auto status = amity_homekit::status();
        CHECK(status.pairing && hap::setup_id() == "KEEP" && state().saved["hap_setup"].substr(0, 4) == "KEEP");
        const auto calls = hap::setup_calls();
        CHECK(calls.init == 1 && calls.start == 1 && calls.set_setup_id == 2 && calls.set_setup_info == 2);
        const auto requests = hap::payload_requests();
        CHECK(requests.size() == 1 && requests[0].id == "KEEP");
        // The setup ID came from the saved record, not the random generator.
        CHECK(count("esp_random") == 1);
    });
    CHECK(start(hub, callbacks(), std::move(passes)) == ESP_OK);
}

// stop() clears a published pairing window and its codes at once.
void stop_clears_pairing() {
    install_defaults();
    amity::SystemControl hub;
    install_hub(hub);
    CHECK(start(hub, callbacks(), {[] {}, [] { CHECK(amity_homekit::open_pairing() == ESP_OK); }, [] {}}) == ESP_OK);
    auto status = amity_homekit::status();
    CHECK(status.pairing && status.started && status.manual_code[0] && status.qr_payload[0]);
    CHECK(amity_homekit::stop() == ESP_OK);
    status = amity_homekit::status();
    CHECK(!status.pairing && !status.started && cleared(status) && status.error == ESP_OK);
}

// stop() fences the worker: an Open queued before stop() is ignored, with
// nothing opened or published.
void stop_ignores_queued_open() {
    install_defaults();
    amity::SystemControl hub;
    install_hub(hub);
    record_deadlines();
    CHECK(start(hub, callbacks(), {[] {}}) == ESP_OK);
    CHECK(amity_homekit::status().available && amity_homekit::open_pairing() == ESP_OK);
    CHECK(amity_homekit::stop() == ESP_OK && count("hap_amity_stop") == 1);
    auto status = amity_homekit::status();
    CHECK(!status.started && !status.available && !status.pairing && !status.busy && cleared(status));
    CHECK(amity_homekit::open_pairing() == ESP_ERR_INVALID_STATE);
    const auto random = count("esp_random");
    run_worker({[] {}});
    CHECK(count("esp_random") == random && count("hap_amity_refresh_mdns") == 0 && deadlines().empty());
    CHECK(hap::setup_calls().payloads == 0 && hap::setup_calls().init == 0);
    status = amity_homekit::status();
    CHECK(!status.available && !status.busy && !status.pairing && !status.started);
}

// A failed SDK stop leaves HomeKit running but disabled and reports the
// error; a later stop finishes it.
void failed_stop() {
    install_defaults();
    amity::SystemControl hub;
    install_hub(hub);
    state().saved["hap_setup"] = setup_record();
    CHECK(start(hub, callbacks()) == ESP_OK && amity_homekit::status().started);
    install<&hap_amity_stop>([] {
        CHECK(state().hap_lock == 0);
        return HAP_FAIL;
    });
    CHECK(amity_homekit::stop() == ESP_FAIL && amity_homekit::running);
    auto status = amity_homekit::status();
    CHECK(status.error == ESP_FAIL && !status.started && !status.available && !status.pairing && !status.busy);
    CHECK(amity_homekit::remove_pairings() == ESP_ERR_INVALID_STATE && amity_homekit::open_pairing() == ESP_ERR_INVALID_STATE);
    CHECK(start(hub, callbacks()) == ESP_ERR_INVALID_STATE && amity_fake::tasks().size() == 1);
    // The worker still running publishes nothing as started or available.
    run_worker({[] {}});
    status = amity_homekit::status();
    CHECK(!status.started && !status.available && !status.pairing && amity_homekit::running);
    install<&hap_amity_stop>([] { return HAP_SUCCESS; });
    CHECK(amity_homekit::stop() == ESP_OK && !amity_homekit::running);
    status = amity_homekit::status();
    CHECK(status.error == ESP_OK && !status.started);
    CHECK(amity_homekit::stop() == ESP_OK && count("hap_amity_stop") == 3);
}

// The remote's battery: an unpaired remote reads full and not charging; a
// paired remote's level and charging are absent until it reports them, and
// 20 percent or less is low.
void remote_battery() {
    install_defaults();
    amity::SystemControl hub;
    install_hub(hub);
    state().saved["hap_setup"] = setup_record();
    CHECK(start(hub, callbacks()) == ESP_OK);
    auto &remote = battery();
    auto &level = remote.characteristic("68"), &low = remote.characteristic("79"), &charging = remote.characteristic("8F");
    const auto read_all = [&] {
        return std::vector<hap_status_t>{hap::read(level), hap::read(low), hap::read(charging)};
    };
    const std::vector<hap_status_t> known(3, HAP_STATUS_SUCCESS), absent(3, HAP_STATUS_RES_ABSENT);
    CHECK(read_all() == known && level.number() == 100 && low.number() == 0 && charging.number() == 0);
    state().remote = {};
    state().remote.bonded = true;
    state().remote.battery_percent = 75;
    state().remote.charging = 1;
    CHECK(read_all() == absent && level.number() == 100 && charging.number() == 0);
    state().remote.connected = true;
    CHECK(read_all() == known && level.number() == 75 && low.number() == 0 && charging.number() == 1);
    state().remote.battery_percent = 21;
    CHECK(read_all() == known && low.number() == 0);
    state().remote.battery_percent = 20;
    CHECK(read_all() == known && level.number() == 20 && low.number() == 1);
    state().remote.battery_percent = 10;
    state().remote.charging = -1;
    CHECK((read_all() == std::vector<hap_status_t>{HAP_STATUS_SUCCESS, HAP_STATUS_SUCCESS, HAP_STATUS_RES_ABSENT}));
    CHECK(level.number() == 10 && low.number() == 1 && charging.number() == 1);
    state().remote.battery_percent = -1;
    state().remote.charging = 0;
    CHECK((read_all() == std::vector<hap_status_t>{HAP_STATUS_RES_ABSENT, HAP_STATUS_RES_ABSENT, HAP_STATUS_SUCCESS}));
    CHECK(level.number() == 10 && charging.number() == 0);
    state().remote.battery_percent = 150;
    CHECK(read_all() == known && level.number() == 100 && low.number() == 0);
    // An empty battery is a level, and low.
    state().remote.battery_percent = 0;
    CHECK(read_all() == known && level.number() == 0 && low.number() == 1);
}

// Reads refresh the published values from the hub: power and the selected
// input from playback, and mute only while the audio status is known.
void read_values() {
    install_defaults();
    amity::SystemControl hub;
    install_hub(hub);
    state().saved["hap_setup"] = setup_record();
    CHECK(start(hub, callbacks()) == ESP_OK);
    auto &active = television().characteristic("B0"), &selected = television().characteristic("E7");
    auto &mute = speaker().characteristic("11A"), &sound = speaker().characteristic("B0");
    CHECK(hap::read(mute) == HAP_STATUS_RES_ABSENT && hap::read(active) == HAP_STATUS_SUCCESS);
    CHECK(active.number() == 0 && selected.number() == 0 && sound.number() == 0);
    state().playback.current_activity = 4;
    state().playback.audio_status_known = true;
    state().playback.muted = true;
    CHECK(hap::read(mute) == HAP_STATUS_SUCCESS && mute.number() == 1);
    CHECK(active.number() == 1 && selected.number() == 5 && sound.number() == 1);
    state().playback.audio_status_known = false;
    state().playback.muted = false;
    CHECK(hap::read(mute) == HAP_STATUS_RES_ABSENT && mute.number() == 1);
}

// Writes go to the hub through the Matter callbacks; power, input and mute
// then show observed hub state rather than the written value.
void write_dispatch() {
    install_defaults();
    amity::SystemControl hub;
    install_hub(hub, 3);
    state().saved["hap_setup"] = setup_record();
    CHECK(start(hub, callbacks()) == ESP_OK);
    auto &active = television().characteristic("B0"), &selected = television().characteristic("E7");
    auto &key = television().characteristic("E1"), &name = television().characteristic("E3");
    auto &mute = speaker().characteristic("11A"), &volume = speaker().characteristic("EA");
    const unsigned notified = active.notifications + selected.notifications + mute.notifications;
    CHECK(hap::write(active, integer(1)) == HAP_STATUS_SUCCESS && hap::write(active, integer(0)) == HAP_STATUS_SUCCESS);
    CHECK(hap::write(active, integer(2)) == HAP_STATUS_VAL_INVALID);
    CHECK(hap::write(selected, integer(3)) == HAP_STATUS_SUCCESS && hap::write(selected, integer(1)) == HAP_STATUS_SUCCESS);
    CHECK(hap::write(selected, integer(4)) == HAP_STATUS_VAL_INVALID && hap::write(selected, integer(0)) == HAP_STATUS_VAL_INVALID);
    CHECK(hap::write(mute, flag(true)) == HAP_STATUS_SUCCESS);
    CHECK(active.number() == 0 && selected.number() == 0 && mute.number() == 0);
    CHECK(active.notifications + selected.notifications + mute.notifications == notified);
    CHECK(hap::write(volume, integer(0)) == HAP_STATUS_SUCCESS && hap::write(volume, integer(1)) == HAP_STATUS_SUCCESS);
    CHECK(hap::write(volume, integer(2)) == HAP_STATUS_VAL_INVALID && volume.number() == 1);
    CHECK((handlers.calls == std::vector<std::string>{"power on", "power off", "activity 2", "activity 0", "mute",
                                                       "volume 1", "volume -1"}));
    handlers.calls.clear();
    const std::vector<std::pair<int, int>> keys{{0, 0x48}, {1, 0x49}, {2, 0x4b}, {3, 0x4c},  {4, 0x01},  {5, 0x02},
                                                {6, 0x03}, {7, 0x04}, {8, 0x00}, {9, 0x0d},  {10, 0x0d}, {11, 0x61},
                                                {15, 0x35}};
    std::vector<std::string> expected;
    for (const auto &[button, code] : keys) {
        CHECK(hap::write(key, integer(button)) == HAP_STATUS_SUCCESS);
        expected.push_back("key " + std::to_string(code));
    }
    for (const int button : {12, 13, 14, 16}) CHECK(hap::write(key, integer(button)) == HAP_STATUS_VAL_INVALID);
    CHECK(handlers.calls == expected);
    // A failing hub reports a communication error; a refused value is invalid.
    handlers.calls.clear();
    handlers.result = ESP_ERR_TIMEOUT;
    CHECK(hap::write(active, integer(1)) == HAP_STATUS_COMM_ERR);
    handlers.result = ESP_ERR_INVALID_ARG;
    CHECK(hap::write(key, integer(4)) == HAP_STATUS_VAL_INVALID && handlers.calls.size() == 2);
    handlers.result = ESP_OK;
    // The television name is saved; an input name needs a saved configuration.
    const std::string longest(63, 'n');
    CHECK(hap::write(name, text("Living room")) == HAP_STATUS_SUCCESS && name.text == "Living room");
    CHECK(state().saved["hap_name"] == "Living room");
    CHECK(hap::write(name, text(longest.c_str())) == HAP_STATUS_SUCCESS && name.text == longest);
    CHECK(hap::write(name, text((longest + "n").c_str())) == HAP_STATUS_VAL_INVALID);
    CHECK(hap::write(name, text("")) == HAP_STATUS_VAL_INVALID && name.text == longest);
    state().write_errors["hap_name"] = ESP_FAIL;
    CHECK(hap::write(name, text("Den")) == HAP_STATUS_COMM_ERR && name.text == longest);
    CHECK(hap::write(input(0).characteristic("E3"), text("Couch")) == HAP_STATUS_COMM_ERR);
    CHECK(count("amity::activate_distributed_configuration") == 0);
}

// After stop() the callbacks answer every read and write with a
// communication error, without reaching the hub.
void disabled_callbacks() {
    install_defaults();
    amity::SystemControl hub;
    install_hub(hub);
    state().saved["hap_setup"] = setup_record();
    CHECK(start(hub, callbacks()) == ESP_OK);
    CHECK(amity_homekit::stop() == ESP_OK);
    const auto playback = count("amity::HubService::playback_state");
    CHECK(hap::read(television().characteristic("B0")) == HAP_STATUS_COMM_ERR);
    CHECK(hap::read(battery().characteristic("68")) == HAP_STATUS_COMM_ERR);
    CHECK(hap::write(television().characteristic("B0"), integer(1)) == HAP_STATUS_COMM_ERR);
    CHECK(hap::write(input(0).characteristic("134"), integer(0)) == HAP_STATUS_COMM_ERR);
    CHECK(handlers.calls.empty() && count("amity::HubService::playback_state") == playback);
}

// A hostname change moves HomeKit discovery only while HomeKit runs.
void rename_hostname() {
    install_defaults();
    amity::SystemControl hub;
    install_hub(hub);
    std::vector<std::string> names;
    install<&hap_amity_rename_hostname>([&](const char *name) {
        names.push_back(name);
        return names.size() == 2 ? HAP_FAIL : HAP_SUCCESS;
    });
    CHECK(amity_homekit::rename_hostname("early") == ESP_OK && names.empty());
    CHECK(start(hub, callbacks()) == ESP_OK);
    CHECK(amity_homekit::rename_hostname("living-room") == ESP_OK);
    CHECK(amity_homekit::rename_hostname("den") == ESP_FAIL);
    CHECK(amity_homekit::stop() == ESP_OK && amity_homekit::rename_hostname("late") == ESP_OK);
    CHECK((names == std::vector<std::string>{"living-room", "den"}));
}

}  // namespace

int main(int argc, char **argv) {
    return amity_test::run(argc, argv,
                           {{"start-worker", start_worker},
                            {"init-failure", init_failure},
                            {"task-creation-failure", task_creation_failure},
                            {"startup-timeout", startup_timeout},
                            {"startup-error", startup_error},
                            {"startup-initialize-error", startup_initialize_error},
                            {"stop-before-start", stop_before_start},
                            {"stop-during-start", stop_during_start},
                            {"restore-saved-setup", restore_saved_setup},
                            {"accessory", accessory},
                            {"open-pairing", open_pairing},
                            {"full-command-queue", full_command_queue},
                            {"pairing-expiry", pairing_expiry},
                            {"paired-closes-window", paired_closes_window},
                            {"open-while-paired", open_while_paired},
                            {"open-refused", open_refused},
                            {"open-failures", open_failures},
                            {"reopen-with-saved-setup", reopen_with_saved_setup},
                            {"stop-clears-pairing", stop_clears_pairing},
                            {"stop-ignores-queued-open", stop_ignores_queued_open},
                            {"failed-stop", failed_stop},
                            {"remote-battery", remote_battery},
                            {"read-values", read_values},
                            {"write-dispatch", write_dispatch},
                            {"disabled-callbacks", disabled_callbacks},
                            {"rename-hostname", rename_hostname}});
}
