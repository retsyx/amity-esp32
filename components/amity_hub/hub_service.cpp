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
#include <atomic>
#include <cstdlib>
#include <utility>

namespace amity {
namespace { constexpr const char *TAG = "amity_hub"; }
struct HubService::Impl : Transport {
    enum class Kind { Configure, Select, Key, Release, Standby, Discover };
    struct Command {
        explicit Command(Kind command_kind, int command_value = 0, bool command_repeat = false)
            : kind(command_kind), value(command_value), repeat(command_repeat) {}
        Kind kind;
        int value = 0;
        bool repeat = false;
        uint32_t key_generation = 0, hold_generation = 0;
        bool activity_bound = false;
        uint32_t activity_generation = 0;
        std::vector<Activity> activities;
        std::vector<Quirk> quirks;
    };
    cec::Bus front, back;
    Hub hub{*this};
    QueueHandle_t commands = nullptr;
    SemaphoreHandle_t lock = nullptr;
    SemaphoreHandle_t command_slots = nullptr, configuration_gate = nullptr;
    TaskHandle_t task = nullptr;
    Snapshot cached;
    std::function<void(const Snapshot &)> callback;
    uint8_t front_address = 15, back_address = 15;
    uint16_t fallback_physical = 0x1000;
    bool scan_requested = false;
    bool scan_running = false;
    int64_t last_scan = -120000000;
    int64_t last_claim = -5000000;
    uint32_t dropped_tx = 0, discovery_generation = 0;
    std::atomic<uint32_t> key_generation{0};
    uint32_t applied_key_generation = 0;
    std::atomic<uint32_t> hold_generation{0};
    uint32_t repeating_generation = 0;
    int repeating_key = -1;
    bool sending_held_key = false, setting_activity = false;
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
        // All producers reserve a slot, so queued + reserved <= capacity.
        // The worker frees reservations immediately on dequeue. Queue send
        // therefore cannot fail after a successful external commit.
        // Cancel continuation immediately, even if the owner is waiting for
        // a wire attempt or finishing discovery. A short tap still keeps its
        // initial queued press and subsequent release.
        if (configuration || command->kind == Kind::Select || command->kind == Kind::Standby) hold_generation.fetch_add(1);
        command = owned.release();
        if (xQueueSend(commands, &command, 0) != pdTRUE) std::abort();
        unlock();
        return ESP_OK;
    }
    bool enqueue(Command *command) { return submit(command) == ESP_OK; }
    void apply_emergency_release() {
        const auto generation = key_generation.load();
        if (generation == applied_key_generation) return;
        stop_hold();
        applied_key_generation = generation;
    }
    void stop_hold() { repeating_key = -1; hub.release_key(); }
    void check_hold() {
        if (repeating_key >= 0 && repeating_generation != hold_generation.load()) stop_hold();
    }
    void send_held_key() {
        sending_held_key = true;
        const bool accepted = hub.press_key(static_cast<uint8_t>(repeating_key), true);
        sending_held_key = false;
        if (!accepted) stop_hold();
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
        stop_hold();
        setting_activity = change && CONFIG_AMITY_ACTIVITY_SETTLE_SECONDS != 0;
        if (index < 0) hub.standby(); else hub.select_activity(index);
        setting_activity = false;
        if (change) { xSemaphoreTake(lock, portMAX_DELAY); settling.finish(esp_timer_get_time()); xSemaphoreGive(lock); }
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
        // Bound hub stalls while giving the wire worker time to make room.
        // Hub retains failed releases and retries them from its owner tick.
        auto &bus = side == BusSide::Front ? front : back;
        // Held presses join the same FIFO as routing, releases and replies.
        // Only one attempt is outstanding; the next is decided after completion.
        const bool sent = setting_activity ? bus.send_and_wait(frame, 1000, 3) :
            sending_held_key && data[0] == 0x44 ? bus.send_and_wait(frame, 1000) : bus.send(frame, 1000);
        if (sent && setting_activity) {
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
    uint8_t claim(cec::Bus &bus, std::initializer_list<uint8_t> candidates) {
        for (auto address : candidates) {
            cec::Frame poll; poll.size = 1; poll.data[0] = (address << 4) | address;
            // CEC allocates an address only after two unacknowledged polls;
            // a wire error or arbitration loss is never evidence of vacancy.
            if (bus.transmit(poll, 1) != cec::TxResult::Nack) continue;
            if (bus.transmit(poll, 1) != cec::TxResult::Nack) continue;
            bus.set_address(address);
            return address;
        }
        return 15;
    }
    void claim_addresses() {
        last_claim = esp_timer_get_time();
        const auto physical = hub.snapshot().front_physical_address;
        const bool claiming_front = front_address == 15 && physical != 0xffff, claiming_back = back_address == 15;
        if (claiming_back) back_address = claim(back, {0, 14});
        if (claiming_front) front_address = claim(front, {4, 8, 11, 12, 13});
        hub.set_addresses(front_address, back_address, physical);
        if (back_address == 15 || (physical != 0xffff && front_address == 15)) {
            ESP_LOGW(TAG, "CEC address allocation pending (front=%u back=%u); check pins and bus roles", front_address, back_address);
        } else if (physical != 0xffff) {
            ESP_LOGI(TAG, "CEC ready: front playback %u, back TV %u", front_address, back_address);
            if (!scan_running) scan_requested = true;
        }
        if (claiming_front) hub.announce(BusSide::Front);
        if (claiming_back) hub.announce(BusSide::Back);
        publish();
    }
    void await_scan_replies() {
        // Presence polls finish before queued identity requests. Wait for
        // their replies before choosing a physical address or using names.
        const auto deadline = esp_timer_get_time() + 5000000;
        auto quiet_since = esp_timer_get_time();
        while (esp_timer_get_time() < deadline) {
            if (receive() || !front.idle() || !back.idle()) quiet_since = esp_timer_get_time();
            if (esp_timer_get_time() - quiet_since >= 1100000) break;
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    void scan() {
        scan_requested = false; scan_running = true; last_scan = esp_timer_get_time();
        ESP_LOGI(TAG, "Discovering devices on both HDMI-CEC buses");
        hub.begin_discovery();
        publish();
        for (const auto side : {BusSide::Back, BusSide::Front}) {
            auto &bus = side == BusSide::Front ? front : back;
            const auto own = side == BusSide::Front ? front_address : back_address;
            const bool determining = side == BusSide::Back && hub.snapshot().front_physical_address == 0xffff;
            if (own == 15 && !determining) continue;
            for (uint8_t address = 0; address < 15; ++address) {
                if (address == own) continue;
                cec::Frame poll; poll.size = 1; poll.data[0] = (own << 4) | address;
                if (bus.transmit(poll, 1) == cec::TxResult::Success) hub.discovered(side, address);
                receive();
            }
            if (side == BusSide::Back && hub.snapshot().front_physical_address == 0xffff) {
                await_scan_replies();
                hub.determine_front_physical_address(fallback_physical);
                [[maybe_unused]] const auto physical = hub.snapshot().front_physical_address;
                ESP_LOGI(TAG, "Front physical address determined: %x.%x.%x.%x",
                         (physical >> 12) & 15, (physical >> 8) & 15, (physical >> 4) & 15, physical & 15);
                // The front playback device can now claim and advertise its
                // final address, then participate in the front-bus scan.
                claim_addresses();
            }
        }
        // Polling establishes presence before the queued identity requests
        // finish. Let them drain before applying an activity selected while
        // scanning, or a TV can still have its fallback name at selection.
        // Linux's Device() awaits these replies before publishing discovery.
        await_scan_replies();
        hub.finish_discovery();
        ++discovery_generation;
        scan_running = false;
        publish();
    }
    static void run(void *arg) {
        auto &p = *static_cast<Impl *>(arg);
        p.claim_addresses();
        p.scan_requested = true;
        for (;;) {
            p.apply_emergency_release();
            p.check_hold();
            p.hub.tick(esp_timer_get_time());
            bool changed = p.receive();
            changed = p.update_settling() || changed;
            Command *command = nullptr;
            if (xQueueReceive(p.commands, &command, p.repeating_key >= 0 ? 0 : pdMS_TO_TICKS(10)) == pdTRUE) {
                xSemaphoreGive(p.command_slots);
                // Python get_device waits for discovery and retries the name
                // lookup before continuing its original operation. Do that
                // before any power transition mutates the current activity.
                bool missing = false;
                switch (command->kind) {
                case Kind::Select: missing = p.hub.activity_needs_discovery(command->value); break;
                case Kind::Key: missing = p.hub.key_needs_discovery(static_cast<uint8_t>(command->value)); break;
                case Kind::Configure:
                case Kind::Standby: missing = p.hub.activity_needs_discovery(-1); break;
                default: break;
                }
                if (missing) p.request_scan();
                if (p.scan_requested) p.scan();
                p.apply_emergency_release();
                p.hub.tick(esp_timer_get_time());
                switch (command->kind) {
                case Kind::Configure:
                    p.stop_hold(); p.hub.configure(std::move(command->activities), std::move(command->quirks));
                    xSemaphoreTake(p.lock, portMAX_DELAY); p.settling.cancel(); xSemaphoreGive(p.lock);
                    break;
                case Kind::Select: p.apply_activity(command->value); break;
                case Kind::Key:
                    if (command->key_generation == p.applied_key_generation &&
                        (!command->activity_bound || command->activity_generation == p.hub.activity_generation())) {
                        p.stop_hold();
                        if (command->repeat) {
                            p.repeating_key = command->value;
                            p.repeating_generation = command->hold_generation;
                            p.send_held_key();
                        } else p.hub.press_key(static_cast<uint8_t>(command->value));
                    }
                    break;
                case Kind::Release: p.stop_hold(); break;
                case Kind::Standby: p.apply_activity(-1); break;
                case Kind::Discover: p.scan_requested = true; break;
                }
                delete command; changed = true;
            } else {
                // No timer and no queued repeats: completion of the preceding
                // attempt is what allows this next press. Commands win first.
                p.check_hold();
                if (p.repeating_key >= 0) p.send_held_key();
            }
            if (p.scan_requested) p.scan();
            if ((p.front_address == 15 || p.back_address == 15) && esp_timer_get_time() - p.last_claim >= 5000000)
                p.claim_addresses();
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
esp_err_t HubService::request_activity(int index) {
    auto &p = *impl_; if (!p.lock) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(p.lock, portMAX_DELAY);
    if (index < -1 || index >= static_cast<int>(p.cached.activities.size())) { xSemaphoreGive(p.lock); return ESP_ERR_INVALID_ARG; }
    const bool active = p.settling.active(esp_timer_get_time());
    const int target = active ? p.settling.target() : p.cached.current_activity;
    if (active && index != target) { xSemaphoreGive(p.lock); return ESP_ERR_TIMEOUT; }
    auto *command = new (std::nothrow) Impl::Command{index < 0 ? Impl::Kind::Standby : Impl::Kind::Select, index};
    const auto error = p.submit(command, [&] { if (index != target) p.settling.reserve(index); return ESP_OK; });
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
        command->hold_generation = impl_->hold_generation.load();
    }
    return impl_->enqueue(command);
}
bool HubService::press_key_for_activity(uint8_t key, uint32_t activity_generation, bool repeat) {
    auto *command = new (std::nothrow) Impl::Command{Impl::Kind::Key, key, repeat};
    if (command) {
        command->key_generation = impl_->key_generation.load();
        command->hold_generation = impl_->hold_generation.load();
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
bool HubService::release_key() {
    impl_->hold_generation.fetch_add(1);
    if (impl_->enqueue(new (std::nothrow) Impl::Command{Impl::Kind::Release})) return true;
    if (!impl_->task) return false;
    // Releasing must survive an overloaded command queue. The generation
    // cancels older queued presses while allowing later keys to proceed.
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
