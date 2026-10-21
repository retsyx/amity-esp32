/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#include "amity/hub_service.hpp"
#include "amity/activity_settling.hpp"
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#include "esp_heap_caps.h"
#endif
#ifndef CONFIG_AMITY_ACTIVITY_SETTLE_SECONDS
#define CONFIG_AMITY_ACTIVITY_SETTLE_SECONDS 5
#endif
#include "cec/bus.hpp"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <new>
#include <array>
#include <deque>
#include <atomic>
#include <cstdlib>
#include <utility>

namespace amity {
namespace { constexpr const char *TAG = "amity_hub"; }
struct HubService::Impl : Transport {
#ifdef ESP_PLATFORM
    static void *operator new(size_t size) {
        auto *memory = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!memory) std::abort();
        return memory;
    }
    static void operator delete(void *memory) { heap_caps_free(memory); }
#endif
    enum class Kind { Configure, Select, Key, Release, Standby, Discover };
    struct Command {
        explicit Command(Kind command_kind, int command_value = 0, bool command_repeat = false)
            : kind(command_kind), value(command_value), repeat(command_repeat) {}
        Kind kind;
        int value = 0;
        bool repeat = false;
        uint32_t key_generation = 0, release_generation = 0;
        bool activity_bound = false;
        uint32_t activity_generation = 0;
        uint32_t discovery_generation = 0;
        std::vector<Activity> activities;
        std::vector<Quirk> quirks;
    };
    // Emergency release epochs are only a queue-overflow fallback. Normal
    // button-up removes that button's state on the owner, as in the reference.
    // Completion storage and ISR-visible guards must outlive the buses.
    std::atomic<uint32_t> key_generation{0};
    std::atomic<uint32_t> activity_selection{0};
    std::array<std::atomic<uint32_t>, 256> release_generations{};
    struct HeldKey {
        bool active = false, failed = false, waiting = false;
        uint32_t instance = 0, release_generation = 0;
    };
    std::array<HeldKey, 256> keys{};
    struct Flight {
        cec::TxCompletion completion;
        bool used = false;
        int key = -1;
        uint32_t instance = 0, setup = 0;
    };
    // At most nine messages per wire are admitted. Extra records allow
    // terminal results to remain available until the next owner iteration.
    std::array<Flight, 32> flights{};
    cec::TxCompletion discovery_completion;
    cec::Bus front, back;
    Hub hub{*this};
    QueueHandle_t commands = nullptr;
    SemaphoreHandle_t lock = nullptr;
    SemaphoreHandle_t command_slots = nullptr, configuration_gate = nullptr;
    TaskHandle_t task = nullptr;
    std::deque<std::unique_ptr<Command>> deferred;
    Snapshot cached;
    std::function<void(const Snapshot &)> callback;
    uint8_t front_address = 15, back_address = 15;
    uint16_t fallback_physical = 0x1000;
    bool scan_requested = true;
    bool scan_running = false;
    int64_t last_scan = -120000000;
    int64_t last_claim = -5000000;
    uint32_t dropped_tx = 0, discovery_generation = 0;
    uint32_t applied_key_generation = 0;
    Flight *sending_key = nullptr;
    bool setting_activity = false;
    uint32_t setup_generation = 0;
    unsigned setup_outstanding = 0;
    bool setup_pending = false;
    enum class DiscoveryPhase { Idle, ClaimBack, ScanBack, BackReplies, ClaimFront, ScanFront, FinalReplies };
    struct DiscoveryState {
        DiscoveryPhase phase = DiscoveryPhase::Idle;
        bool waiting = false, claim_only = false, claimed = false;
        uint8_t candidate = 0, nacks = 0, address = 0;
        int64_t deadline = 0, quiet_since = 0;
    } discovery;
    ActivitySettling settling{CONFIG_AMITY_ACTIVITY_SETTLE_SECONDS};

    Impl() { lock = xSemaphoreCreateMutex(); configuration_gate = xSemaphoreCreateMutex(); }
    esp_err_t start_buses(int front_gpio, int back_gpio) {
#if defined(ESP_PLATFORM) && !CONFIG_FREERTOS_UNICORE
        // ESP-IDF allocates a peripheral interrupt on the calling CPU.
        // Keep both 20 kHz CEC timers off the Wi-Fi/BLE controller's core 0.
        // A temporary task preserves synchronous error reporting and leaves
        // the generic GPIO transport independent of the application's layout.
        struct Startup { Impl *hub; int front_gpio, back_gpio; SemaphoreHandle_t done; esp_err_t error; };
        Startup startup{this, front_gpio, back_gpio, xSemaphoreCreateBinary(), ESP_FAIL};
        if (!startup.done) return ESP_ERR_NO_MEM;
        const auto init = [](void *context) {
            auto &s = *static_cast<Startup *>(context);
            s.error = s.hub->front.start(s.front_gpio);
            if (s.error == ESP_OK) s.error = s.hub->back.start(s.back_gpio);
            xSemaphoreGive(s.done);
            vTaskDelete(nullptr);
        };
        if (xTaskCreatePinnedToCore(init, "cec_init", 4096, &startup, 5, nullptr, 1) != pdPASS) {
            vSemaphoreDelete(startup.done); return ESP_ERR_NO_MEM;
        }
        xSemaphoreTake(startup.done, portMAX_DELAY);
        vSemaphoreDelete(startup.done);
        return startup.error;
#else
        auto error = front.start(front_gpio);
        return error == ESP_OK ? back.start(back_gpio) : error;
#endif
    }
    ~Impl() {
        if (task) vTaskDelete(task);
        if (commands) {
            Command *command;
            while (xQueueReceive(commands, &command, 0) == pdTRUE) delete command;
            vQueueDelete(commands);
        }
        if (lock) vSemaphoreDelete(lock);
        if (command_slots) vSemaphoreDelete(command_slots);
        if (configuration_gate) vSemaphoreDelete(configuration_gate);
    }
    esp_err_t submit(Command *command, const std::function<esp_err_t()> &commit = {}) {
        std::unique_ptr<Command> owned(command);
        if (!command) return ESP_ERR_NO_MEM;
        if (!commands || !command_slots || !task) return ESP_ERR_INVALID_STATE;
        const bool configuration = command->kind == Kind::Configure;
        // Concurrent configuration callers must commit and enqueue in the
        // same order. Contention is rejected without blocking or persistence.
        if (configuration && xSemaphoreTake(configuration_gate, 0) != pdTRUE) return ESP_ERR_TIMEOUT;
        const auto unlock = [&] { if (configuration) xSemaphoreGive(configuration_gate); };
        if (xSemaphoreTake(command_slots, 0) != pdTRUE) { unlock(); return ESP_ERR_NO_MEM; }
        const auto error = commit ? commit() : ESP_OK;
        if (error != ESP_OK) { xSemaphoreGive(command_slots); unlock(); return error; }
        if (configuration) activity_selection.fetch_add(1);
        // All producers reserve a slot, so queued + deferred + reserved never
        // exceeds capacity. A reservation is freed when its command executes
        // or is cancelled. Enqueue cannot fail after an external commit.
        command = owned.release();
        if (xQueueSend(commands, &command, 0) != pdTRUE) std::abort();
        unlock();
        return ESP_OK;
    }
    bool enqueue(Command *command) { return submit(command) == ESP_OK; }
    void apply_emergency_release() {
        const auto generation = key_generation.load();
        if (generation != applied_key_generation) {
            stop_holds();
            applied_key_generation = generation;
        }
        for (size_t key = 0; key < keys.size(); ++key) {
            const auto released = release_generations[key].load();
            if (released == keys[key].release_generation) continue;
            stop_key(static_cast<uint8_t>(key));
            keys[key].release_generation = released;
        }
        // Cancelled lookups must return their command reservations immediately,
        // rather than filling the queue until an unrelated scan completes.
        for (auto i = deferred.begin(); i != deferred.end();) {
            if ((*i)->kind != Kind::Key || !stale_key(**i)) { ++i; continue; }
            i = deferred.erase(i); xSemaphoreGive(command_slots);
        }
    }
    void stop_key(uint8_t key) {
        auto &state = keys[key];
        state.active = state.failed = false;
        state.waiting = false;
        ++state.instance;
        hub.release_key(key);
    }
    void stop_holds() {
        for (auto &state : keys) {
            state.active = state.failed = false;
            state.waiting = false;
            ++state.instance;
        }
        hub.release_key();
    }
    Flight *reserve_flight() {
        for (auto &flight : flights) if (!flight.used) {
            flight.used = true;
            flight.key = -1; flight.instance = flight.setup = 0;
            return &flight;
        }
        return nullptr;
    }
    void send_held_key(uint8_t key) {
        auto &state = keys[key];
        auto *flight = reserve_flight();
        if (!flight) { state.active = false; state.failed = true; hub.release_key(key); return; }
        flight->key = key; flight->instance = state.instance;
        sending_key = flight;
        const bool accepted = hub.press_key(key, true);
        sending_key = nullptr;
        if (accepted) state.waiting = true;
        else {
            flight->used = false;
            state.active = false; state.failed = true;
            hub.release_key(key);
        }
    }
    bool complete_transmissions() {
        bool changed = false;
        for (auto &flight : flights) {
            if (!flight.used) continue;
            const auto result = flight.completion.result.load(std::memory_order_acquire);
            if (result == cec::TxResult::Pending) continue;
            flight.used = false;
            if (flight.key >= 0) {
                auto &state = keys[static_cast<size_t>(flight.key)];
                if (state.instance == flight.instance) {
                    state.waiting = false;
                    if (result != cec::TxResult::Success) {
                        state.active = false; state.failed = true;
                        hub.release_key(static_cast<uint8_t>(flight.key));
                    }
                }
            }
            if (flight.setup && flight.setup == setup_generation) --setup_outstanding;
            changed = true;
        }
        if (setup_pending && !setting_activity && !setup_outstanding) {
            xSemaphoreTake(lock, portMAX_DELAY);
            settling.finish(esp_timer_get_time());
            xSemaphoreGive(lock);
            setup_pending = false;
        }
        return changed;
    }
    bool transmissions_pending() const {
        for (const auto &flight : flights) if (flight.used) return true;
        return false;
    }
    void continue_keys() {
        for (size_t key = 0; key < keys.size(); ++key)
            if (keys[key].active && !keys[key].waiting) send_held_key(static_cast<uint8_t>(key));
    }
    bool update_settling() {
        xSemaphoreTake(lock, portMAX_DELAY);
        const bool changed = cached.activity_settling != settling.active(esp_timer_get_time());
        xSemaphoreGive(lock); return changed;
    }
    void apply_activity(int index) {
        const auto before = hub.snapshot();
        // A configuration queued before this request may have removed its index.
        if (index < -1 || index >= static_cast<int>(before.activities.size())) {
            xSemaphoreTake(lock, portMAX_DELAY);
            if (settling.target() == index) settling.cancel();
            xSemaphoreGive(lock); return;
        }
        const bool change = before.current_activity != index;
        if (change) {
            std::vector<std::string> names;
            for (int activity : {before.current_activity, index}) {
                if (activity < 0 || activity >= static_cast<int>(before.activities.size())) continue;
                const auto &a = before.activities[activity];
                for (const auto &name : {a.display, a.source, a.audio, a.switch_device}) if (!name.empty()) names.push_back(name);
            }
            std::vector<ActivitySettling::Peer> peers;
            for (const auto &d : before.devices)
                if (std::find(names.begin(), names.end(), d.name) != names.end()) peers.push_back({d.bus, d.address, d.physical_address});
            xSemaphoreTake(lock, portMAX_DELAY); settling.start(index, std::move(peers)); xSemaphoreGive(lock);
        }
        stop_holds();
        if (change) { ++setup_generation; setup_outstanding = 0; setup_pending = true; }
        setting_activity = change && CONFIG_AMITY_ACTIVITY_SETTLE_SECONDS != 0;
        if (index < 0) hub.standby(); else hub.select_activity(index);
        setting_activity = false;
        if (change && !setup_outstanding) {
            xSemaphoreTake(lock, portMAX_DELAY); settling.finish(esp_timer_get_time()); xSemaphoreGive(lock);
            setup_pending = false;
        }
    }
    bool send(BusSide side, uint8_t destination, const uint8_t *data, size_t size) override {
        if (size > 15 || !size || destination > 15) return false;
        const uint8_t address = side == BusSide::Front ? front_address : back_address;
        // Give Physical Address accepts an unregistered initiator because its
        // response is broadcast. This still permits startup determination if
        // all back logical-address candidates are occupied or allocation is busy.
        if (address == 15 && !(side == BusSide::Back && destination < 15 && size == 1 && data[0] == 0x83)) return false;
        cec::Frame frame; frame.size = static_cast<uint8_t>(size + 1);
        frame.data[0] = static_cast<uint8_t>((address << 4) | destination);
        for (size_t i = 0; i < size; ++i) frame.data[i + 1] = data[i];
        // Every command is admitted without a wire/completion/capacity wait.
        auto &bus = side == BusSide::Front ? front : back;
        cec::ExecutionFence fence{};
        if (data[0] == 0x44 && !setting_activity) {
            fence = {&key_generation, applied_key_generation};
        }
        const unsigned attempts = data[0] == 0x44 ? 1 : 3;
        auto *flight = sending_key && data[0] == 0x44 ? sending_key : setting_activity ? reserve_flight() : nullptr;
        const bool sent = flight ? bus.send_async_guarded(frame, fence, flight->completion, attempts) :
            !setting_activity && bus.send_guarded(frame, fence, attempts);
        if (flight && !sent) flight->used = false;
        if (sent && setting_activity) {
            flight->setup = setup_generation;
            ++setup_outstanding;
            xSemaphoreTake(lock, portMAX_DELAY); settling.expect(side, destination, data, size); xSemaphoreGive(lock);
        }
        if (!sent) ++dropped_tx;
        return sent;
    }
    void request_scan() override {
        if (esp_timer_get_time() - last_scan >= 120000000) scan_requested = true;
    }
    bool receive() {
        bool received = false;
        cec::Frame frame;
        for (const auto side : {BusSide::Front, BusSide::Back}) {
            auto &bus = side == BusSide::Front ? front : back;
            // Bounded draining prevents a noisy bus starving user commands.
            for (unsigned n = 0; n < 32 && bus.receive(frame); ++n) {
                ESP_LOGD(TAG, "%s RX %02x:%02x (%u)", side == BusSide::Front ? "front" : "back",
                         frame.data[0], frame.size > 1 ? frame.data[1] : 0, frame.size);
                xSemaphoreTake(lock, portMAX_DELAY);
                settling.received(side, frame, side == BusSide::Front ? front_address : back_address, esp_timer_get_time());
                xSemaphoreGive(lock);
                hub.receive(side, frame); received = true;
            }
        }
        return received;
    }
    void publish() {
        auto state = hub.snapshot();
        state.discovering = scan_running;
        state.discovery_generation = discovery_generation;
        const auto f = front.statistics(), b = back.statistics();
        state.tx_errors = f.tx_errors + b.tx_errors + dropped_tx;
        state.rx_dropped = f.rx_dropped + b.rx_dropped;
        xSemaphoreTake(lock, portMAX_DELAY);
        state.activity_settling = settling.active(esp_timer_get_time());
        cached = state;
        auto cb = callback;
        xSemaphoreGive(lock);
        if (cb) cb(state);
    }
    bool discovery_pending() const {
        return scan_requested || discovery.phase != DiscoveryPhase::Idle;
    }
    void begin_claim(BusSide side) {
        discovery.phase = side == BusSide::Back ? DiscoveryPhase::ClaimBack : DiscoveryPhase::ClaimFront;
        discovery.candidate = 0;
        discovery.nacks = 0;
    }
    void start_discovery() {
        if (discovery.phase != DiscoveryPhase::Idle) { scan_requested = true; return; }
        scan_requested = false;
        scan_running = true;
        last_scan = last_claim = esp_timer_get_time();
        discovery.claim_only = false;
        discovery.claimed = false;
        hub.begin_discovery(); // The existing cache remains usable until finish.
        begin_claim(BusSide::Back);
        ESP_LOGI(TAG, "Discovering devices on both HDMI-CEC buses");
        publish();
    }
    void finish_claim(BusSide side) {
        if (side == BusSide::Back) {
            if (discovery.claim_only) begin_claim(BusSide::Front);
            else { discovery.phase = DiscoveryPhase::ScanBack; discovery.address = 0; }
        } else if (discovery.claim_only) {
            discovery.phase = DiscoveryPhase::Idle;
            if (discovery.claimed) scan_requested = true;
        } else {
            discovery.phase = DiscoveryPhase::ScanFront;
            discovery.address = 0;
        }
    }
    void claim_tick(BusSide side) {
        static constexpr uint8_t back_candidates[] = {0, 14};
        static constexpr uint8_t front_candidates[] = {4, 8, 11, 12, 13};
        const auto *candidates = side == BusSide::Back ? back_candidates : front_candidates;
        const unsigned count = side == BusSide::Back ? sizeof(back_candidates) : sizeof(front_candidates);
        auto &address = side == BusSide::Back ? back_address : front_address;
        auto &bus = side == BusSide::Back ? back : front;
        const auto physical = hub.snapshot().front_physical_address;
        if (address != 15 || (side == BusSide::Front && physical == 0xffff)) {
            finish_claim(side); return;
        }
        if (discovery.waiting) {
            const auto result = discovery_completion.result.load(std::memory_order_acquire);
            if (result == cec::TxResult::Pending) return;
            discovery.waiting = false;
            // Only two unacknowledged polls of this candidate prove vacancy.
            // Arbitration loss and wire errors advance to another candidate.
            if (result == cec::TxResult::Nack && ++discovery.nacks == 2) {
                address = candidates[discovery.candidate];
                bus.set_address(address);
                hub.set_addresses(front_address, back_address, physical);
                hub.announce(side);
                discovery.claimed = true;
                finish_claim(side);
                publish();
                return;
            }
            if (result != cec::TxResult::Nack) {
                ++discovery.candidate;
                discovery.nacks = 0;
            }
        }
        if (discovery.candidate == count) { finish_claim(side); return; }
        const auto candidate = candidates[discovery.candidate];
        cec::Frame poll; poll.size = 1; poll.data[0] = (candidate << 4) | candidate;
        discovery.waiting = bus.send_async_guarded(poll, {}, discovery_completion, 1);
    }
    void begin_reply_window(bool back_side, int64_t now) {
        discovery.phase = back_side ? DiscoveryPhase::BackReplies : DiscoveryPhase::FinalReplies;
        discovery.deadline = now + 5000000;
        discovery.quiet_since = now;
    }
    void scan_tick(BusSide side, int64_t now) {
        auto &bus = side == BusSide::Back ? back : front;
        const auto own = side == BusSide::Back ? back_address : front_address;
        const bool determining = side == BusSide::Back && hub.snapshot().front_physical_address == 0xffff;
        if (own == 15 && !determining) { begin_reply_window(side == BusSide::Back, now); return; }
        if (discovery.waiting) {
            const auto result = discovery_completion.result.load(std::memory_order_acquire);
            if (result == cec::TxResult::Pending) return;
            discovery.waiting = false;
            if (result == cec::TxResult::Success) hub.discovered(side, discovery.address);
            ++discovery.address;
        }
        if (discovery.address == own) ++discovery.address;
        if (discovery.address >= 15) { begin_reply_window(side == BusSide::Back, now); return; }
        cec::Frame poll; poll.size = 1; poll.data[0] = (own << 4) | discovery.address;
        // Full FIFOs postpone this probe; they never delay command handling.
        discovery.waiting = bus.send_async_guarded(poll, {}, discovery_completion, 1);
    }
    void discovery_tick(bool received) {
        const auto now = esp_timer_get_time();
        if (discovery.phase == DiscoveryPhase::Idle) {
            if (scan_requested) start_discovery();
            else if ((front_address == 15 || back_address == 15) && now - last_claim >= 5000000) {
                last_claim = now;
                discovery.claim_only = true;
                discovery.claimed = false;
                begin_claim(BusSide::Back);
            } else return;
        }
        switch (discovery.phase) {
        case DiscoveryPhase::ClaimBack: claim_tick(BusSide::Back); break;
        case DiscoveryPhase::ClaimFront: claim_tick(BusSide::Front); break;
        case DiscoveryPhase::ScanBack: scan_tick(BusSide::Back, now); break;
        case DiscoveryPhase::ScanFront: scan_tick(BusSide::Front, now); break;
        case DiscoveryPhase::BackReplies:
        case DiscoveryPhase::FinalReplies:
            // Metadata requests and replies drain between owner iterations.
            // Keep the response window bounded even during other CEC traffic.
            if (received || !front.idle() || !back.idle()) discovery.quiet_since = now;
            if (now < discovery.deadline && now - discovery.quiet_since < 1100000) break;
            if (discovery.phase == DiscoveryPhase::BackReplies) {
                hub.determine_front_physical_address(fallback_physical);
                hub.set_addresses(front_address, back_address, hub.snapshot().front_physical_address);
                begin_claim(BusSide::Front);
            } else {
                hub.finish_discovery();
                ++discovery_generation;
                scan_running = false;
                discovery.phase = DiscoveryPhase::Idle;
                publish();
            }
            break;
        case DiscoveryPhase::Idle: break;
        }
    }
    bool needs_discovery(const Command &command) const {
        switch (command.kind) {
        case Kind::Select: return hub.activity_needs_discovery(command.value);
        case Kind::Key: return hub.key_needs_discovery(static_cast<uint8_t>(command.value));
        case Kind::Configure:
        case Kind::Standby: return hub.activity_needs_discovery(-1);
        default: return false;
        }
    }
    bool stale_key(const Command &command) const {
        return command.key_generation != applied_key_generation ||
            command.release_generation != keys[static_cast<uint8_t>(command.value)].release_generation ||
            (command.activity_bound && command.activity_generation != hub.activity_generation());
    }
    void execute(Command &command) {
        switch (command.kind) {
        case Kind::Configure:
            stop_holds();
            hub.configure(std::move(command.activities), std::move(command.quirks));
            ++setup_generation; setup_outstanding = 0; setup_pending = false;
            xSemaphoreTake(lock, portMAX_DELAY); settling.cancel(); xSemaphoreGive(lock);
            break;
        case Kind::Select: apply_activity(command.value); break;
        case Kind::Key:
            if (stale_key(command)) break;
            if (command.value == 0x40) { apply_activity(-1); break; }
            if (command.repeat) {
                auto &state = keys[static_cast<uint8_t>(command.value)];
                if (state.active || state.failed) break;
                state.active = true; ++state.instance;
                // Initial submission never waits for another key's completion.
                send_held_key(static_cast<uint8_t>(command.value));
            } else hub.press_key(static_cast<uint8_t>(command.value));
            break;
        case Kind::Release:
            if (command.value < 0) {
                stop_holds();
                // Disconnect/input loss also cancels key operations still
                // awaiting device lookup, before they can reach the wire.
                for (auto i = deferred.begin(); i != deferred.end();) {
                    if ((*i)->kind != Kind::Key) { ++i; continue; }
                    i = deferred.erase(i); xSemaphoreGive(command_slots);
                }
            } else {
                stop_key(static_cast<uint8_t>(command.value));
                // A short tap whose recipient is still being discovered is
                // finite: lookup may finish later, but cannot resurrect a hold.
                for (auto &pending : deferred)
                    if (pending->kind == Kind::Key && pending->value == command.value) pending->repeat = false;
            }
            break;
        case Kind::Standby: apply_activity(-1); break;
        case Kind::Discover: scan_requested = true; break;
        }
    }
    void accept(Command *raw) {
        std::unique_ptr<Command> command(raw);
        if (needs_discovery(*command)) {
            request_scan();
            if (scan_running || scan_requested) {
                command->discovery_generation = discovery_generation;
                deferred.push_back(std::move(command));
                return;
            }
        }
        xSemaphoreGive(command_slots);
        execute(*command);
    }
    bool resume_discovered() {
        bool changed = false;
        for (auto i = deferred.begin(); i != deferred.end();) {
            if (scan_running || (*i)->discovery_generation == discovery_generation) { ++i; continue; }
            auto command = std::move(*i);
            i = deferred.erase(i);
            xSemaphoreGive(command_slots);
            execute(*command);
            changed = true;
        }
        return changed;
    }
    static void run(void *arg) {
        auto &p = *static_cast<Impl *>(arg);
        for (;;) {
            p.apply_emergency_release();
            p.hub.tick(esp_timer_get_time());
            bool changed = p.receive();
            // Discovery and logical address allocation advance through short
            // steps. They never hold this task waiting for a CEC result.
            p.discovery_tick(changed);
            changed = p.resume_discovered() || changed;
            Command *command = nullptr;
            const TickType_t wait = p.transmissions_pending() || p.discovery_pending() ? 1 : pdMS_TO_TICKS(10);
            if (xQueueReceive(p.commands, &command, wait) == pdTRUE) {
                p.accept(command);
                changed = true;
            }
            // A completion belongs to one operation. Processing it cannot
            // stall command admission or another key's continuation.
            changed = p.complete_transmissions() || changed;
            p.continue_keys();
            changed = p.update_settling() || changed;
            if (changed) p.publish();
        }
    }
};
HubService::HubService() : impl_(new Impl) {}
HubService::~HubService() = default;
esp_err_t HubService::start(int front_gpio, int back_gpio, uint16_t front_physical) {
    auto &p = *impl_;
    if (!p.lock || !p.configuration_gate) return ESP_ERR_NO_MEM;
    if (p.commands) return ESP_ERR_INVALID_STATE;
    if (front_gpio == back_gpio) return ESP_ERR_INVALID_ARG;
    p.fallback_physical = front_physical;
    auto err = p.start_buses(front_gpio, back_gpio); if (err != ESP_OK) return err;
    p.commands = xQueueCreate(32, sizeof(Impl::Command *));
    p.command_slots = xSemaphoreCreateCounting(32, 32);
    if (!p.commands || !p.command_slots) return ESP_ERR_NO_MEM;
    if (xTaskCreate(Impl::run, "amity_hub", 6144, &p, 5, &p.task) != pdPASS) return ESP_ERR_NO_MEM;
    return ESP_OK;
}
bool HubService::configure(std::vector<Activity> activities, std::vector<Quirk> quirks) {
    return configure_with_commit(std::move(activities), std::move(quirks), {}) == ESP_OK;
}
esp_err_t HubService::configure_with_commit(std::vector<Activity> activities, std::vector<Quirk> quirks,
                                           std::function<esp_err_t()> commit) {
    auto *c = new (std::nothrow) Impl::Command{Impl::Kind::Configure};
    if (!c) return ESP_ERR_NO_MEM;
    c->activities = std::move(activities); c->quirks = std::move(quirks);
    return impl_->submit(c, commit);
}
esp_err_t HubService::request_activity(int index) { return request_activity(index, true); }
esp_err_t HubService::repair_activity() { return request_activity(activity_target(), false); }
uint32_t HubService::activity_selection() const { return impl_->activity_selection.load(); }
esp_err_t HubService::request_activity(int index, bool notify_selection) {
    auto &p = *impl_; if (!p.lock) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(p.lock, portMAX_DELAY);
    if (index < -1 || index >= static_cast<int>(p.cached.activities.size())) { xSemaphoreGive(p.lock); return ESP_ERR_INVALID_ARG; }
    const bool active = p.settling.active(esp_timer_get_time());
    const int target = active ? p.settling.target() : p.cached.current_activity;
    if (active && index != target) { xSemaphoreGive(p.lock); return ESP_ERR_TIMEOUT; }
    auto *command = new (std::nothrow) Impl::Command{index < 0 ? Impl::Kind::Standby : Impl::Kind::Select, index};
    const auto error = p.submit(command, [&] {
        if (index != target) p.settling.reserve(index);
        if (notify_selection) p.activity_selection.fetch_add(1);
        return ESP_OK;
    });
    xSemaphoreGive(p.lock); return error;
}
int HubService::activity_target() const {
    auto &p = *impl_; if (!p.lock) return -1;
    xSemaphoreTake(p.lock, portMAX_DELAY);
    const auto target = p.settling.active(esp_timer_get_time()) ? p.settling.target() : p.cached.current_activity;
    xSemaphoreGive(p.lock); return target;
}
bool HubService::select_activity(int index) { return request_activity(index) == ESP_OK; }
bool HubService::press_key(uint8_t key, bool repeat) {
    if (key == 0x40) return standby();
    auto *command = new (std::nothrow) Impl::Command{Impl::Kind::Key, key, repeat};
    if (command) {
        command->key_generation = impl_->key_generation.load();
        command->release_generation = impl_->release_generations[key].load();
    }
    return impl_->enqueue(command);
}
bool HubService::press_key_for_activity(uint8_t key, uint32_t activity_generation, bool repeat) {
    auto *command = new (std::nothrow) Impl::Command{Impl::Kind::Key, key, repeat};
    if (command) {
        command->key_generation = impl_->key_generation.load();
        command->release_generation = impl_->release_generations[key].load();
        command->activity_bound = true;
        command->activity_generation = activity_generation;
    }
    return impl_->enqueue(command);
}
uint32_t HubService::activity_generation() const {
    auto &p = *impl_; if (!p.lock) return 0;
    xSemaphoreTake(p.lock, portMAX_DELAY); const auto generation = p.cached.activity_generation;
    xSemaphoreGive(p.lock); return generation;
}
bool HubService::release_key(uint8_t key) {
    if (impl_->enqueue(new (std::nothrow) Impl::Command{Impl::Kind::Release, key})) return true;
    if (!impl_->task) return false;
    // Queue saturation still ends only this button. New presses retain the
    // new per-key epoch; unrelated buttons and commands are unaffected.
    impl_->release_generations[key].fetch_add(1);
    return true;
}
bool HubService::release_key() {
    if (impl_->enqueue(new (std::nothrow) Impl::Command{Impl::Kind::Release, -1})) return true;
    if (!impl_->task) return false;
    impl_->key_generation.fetch_add(1);
    return true;
}
bool HubService::standby() { return request_activity(-1) == ESP_OK; }
bool HubService::discover() { return impl_->enqueue(new (std::nothrow) Impl::Command{Impl::Kind::Discover}); }
Snapshot HubService::snapshot() const {
    auto &p = *impl_; if (!p.lock) return {};
    xSemaphoreTake(p.lock, portMAX_DELAY); auto state = p.cached; xSemaphoreGive(p.lock); return state;
}
void HubService::on_change(std::function<void(const Snapshot &)> callback) {
    auto &p = *impl_; if (!p.lock) return;
    xSemaphoreTake(p.lock, portMAX_DELAY); p.callback = std::move(callback); xSemaphoreGive(p.lock);
}
}
