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
#include <array>
#include <limits>
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "driver/gpio.h"
#else
#include <random>
#endif

namespace amity {
namespace {
constexpr const char *TAG = "amity_hub";
// Scoped activity dispatch and owner-local execution share this task. Allow
// headroom for nested typed requests, authorization and CEC/runtime frames.
constexpr uint32_t hub_task_stack_bytes = 12 * 1024;
template<class T> constexpr std::array<T, amity::max_tentacles> filled_tentacles(T value) {
    std::array<T, amity::max_tentacles> result{};
    for(auto& item:result)item=value;
    return result;
}
}
struct HubService::Impl : Transport {
#ifdef ESP_PLATFORM
    static void *operator new(size_t size) { auto *p=heap_caps_malloc(size,MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT); if(!p)std::abort(); return p; }
    static void operator delete(void *p) { heap_caps_free(p); }
#endif
    enum class Kind { Configure, Select, Key, Release, Standby, Discover, AuthorityTransition, Retire, MaintenanceBegin, MaintenanceEnd, SettingsCommit,
        ScopedConfigure, Owned, OwnedBind, OwnedFence, OwnedDiscover, OwnedInputResult, OwnedContext, Received };
    struct Reply {
        SemaphoreHandle_t done=xSemaphoreCreateCounting(1,0);
        std::atomic<bool> canceled{false}; std::atomic<uint8_t> settings_effect{0};
        esp_err_t settings_result=ESP_FAIL;
        system::AdmissionResult result{system::AdmissionCode::overloaded,"owner response unavailable"};
        ~Reply(){if(done)vSemaphoreDelete(done);}
    };
    using OwnedSender=Hub::OwnedSender;
    using OwnedObserver=std::function<void(const system::OwnedAdapterSnapshot&)>;
    using OwnedAuthorizer=std::function<bool(const system::OwnedAuthorization&)>;
    using OwnedDeadline=std::function<uint64_t(const system::OwnedAuthorization&)>;
    using OwnedInputObserver=std::function<void(const system::OwnedIntent&,const system::OwnedAuthorization&,const system::AdmissionResult&)>;
    struct Command {
        explicit Command(Kind k,int v=0,bool r=false):kind(k),value(v),repeat(r){}
        Kind kind; int value=0; bool repeat=false,cleanup_reservation=false,activity_bound=false,route_transition=false,owned_deferred=false,owned_received=false;
        bool activity_started=false,owned_waiting=false,configuration_applied=false,activity_reservation=false,selection_reservation=false,setup_blocked=false;
        system::AdmissionResult owned_result;
        uint32_t key_generation=0,release_generation=0,activity_generation=0,intent_generation=0,activity_receipt=0;
        uint8_t discovery_sides=0;
        cec::Frame received_frame;BusSide received_side=BusSide::Back;uint8_t received_adapter=0;
        Hub::ReceiveStep received_step=Hub::ReceiveStep::Frame;
        uint64_t received_generation=0,received_presence=0,received_deadline=0;uint32_t received_activity=0,received_binding=0,received_wire=0;
        system::BootId received_boot;
        std::vector<Activity> activities;
        std::vector<system::ScopedSelector> selectors;OwnedSender sender;
        system::OwnedIntent owned;system::OwnedAuthorization auth;
        system::OctopusId controller;system::LinkId link;uint64_t config_revision=0,presentation_revision=1;
        std::shared_ptr<Reply> reply;std::function<esp_err_t()> persist;
        system::BootId settings_boot;std::vector<std::pair<uint8_t,uint64_t>> settings_presence;uint64_t settings_deadline=0;
    };
    std::atomic<uint32_t> intent_generation{0},key_generation{0},activity_selection{0},owned_wire_generation{1};
    std::atomic<unsigned> owner_activity_commands{0};
    std::atomic<unsigned> activity_requests{0};std::atomic<bool> setup_running{false};
    std::atomic<uint32_t> binding_wire_generation[2][15]{};
    uint32_t release_binding_generation[2][15]{};
    std::array<std::atomic<uint32_t>,256> release_generations{};
    struct HeldKey {bool active=false,failed=false,waiting=false;uint32_t instance=0,release_generation=0;};
    std::array<HeldKey,256> keys{};
    struct Flight {cec::TxCompletion completion;bool used=false,activity=false;int key=-1;uint32_t instance=0,setup=0;};
    std::array<Flight,32> flights{};cec::TxCompletion discovery_completion;
    cec::Bus front,back;
    struct NativePort {uint8_t tentacle;system::Side side;cec::Bus*bus;int gpio;};
    std::array<NativePort,amity::max_cec_ports> native_ports{};size_t native_port_count=0;
    std::array<std::unique_ptr<Impl>,amity::max_tentacles-1> adapters;
    Impl *parent=nullptr;uint8_t adapter_id=0;
    std::optional<cec::BoardProfile> hardware_profile;
    std::array<std::optional<bool>,amity::max_tentacles> presence{};
    std::array<uint64_t,amity::max_tentacles> presence_generations=filled_tentacles(uint64_t{1});
    std::array<AdapterRuntimeState,amity::max_tentacles> cached_adapters{};
    std::function<int(int)> presence_reader;
    std::array<uint16_t,amity::max_tentacles> front_defaults=filled_tentacles(uint16_t{0x1000});
    system::ConsortiumId consortium;system::OctopusId octopus;system::BootId local_boot;
    std::atomic<bool> local_coordinator{true},authority_transition{false},maintenance{false},maintenance_applied{false},settings_gate{false},retiring{false};
    uint64_t coordinator_epoch=1;
    std::shared_ptr<Reply> pending_settings;system::BootId settings_boot;std::vector<std::pair<uint8_t,uint64_t>> settings_presence;
    uint64_t settings_deadline=0;std::function<esp_err_t()> settings_persist;bool settings_paused=false,owner_started=false,legacy_enabled=false;
    std::atomic<bool> maintenance_started{false};
    Hub hub{*this},activity_hub{*this};
    QueueHandle_t commands=nullptr;SemaphoreHandle_t lock=nullptr,command_slots=nullptr,cleanup_slots=nullptr,configuration_gate=nullptr;TaskHandle_t task=nullptr;
    std::deque<std::unique_ptr<Command>> deferred;Snapshot cached;
    std::function<void(const Snapshot&)>callback;
    uint8_t front_address=15,back_address=15;uint16_t fallback_physical=0x1000;
    // Boot enumerates both wires once. Only explicit UI requests and real
    // recipient-cache misses may request another enumeration afterwards.
    uint8_t requested_scan_sides=3;bool scan_running=false;
    std::array<int64_t,2> last_scan{{-120000000,-120000000}};int64_t last_claim=-5000000;
    uint32_t dropped_tx=0,discovery_generation=0,applied_key_generation=0;Flight*sending_key=nullptr;
    bool setting_activity=false,pacing_receive=false;uint32_t setup_generation=0;unsigned setup_outstanding=0,activity_outstanding=0;bool setup_pending=false,setup_finishes_settling=false,setup_gates_input=false;
    uint64_t setup_last_sequence=0;
    struct SetupOwner {system::OctopusId owner;uint8_t adapter;uint64_t sequence;};
    std::vector<SetupOwner> setup_owners;
    std::vector<std::pair<uint64_t,uint32_t>> input_setups;
    enum class DiscoveryPhase {Idle,ClaimBack,ScanBack,BackReplies,ClaimFront,ScanFront,FinalReplies};
    struct DiscoveryState {DiscoveryPhase phase=DiscoveryPhase::Idle;bool waiting=false,claim_only=false,claimed=false;uint8_t candidate=0,nacks=0,address=0,sides=0;int64_t deadline=0,quiet_since=0;}discovery;
    std::string last_activity_error;ActivitySettling settling{CONFIG_AMITY_ACTIVITY_SETTLE_SECONDS};
    ActivitySettling owned_settling{CONFIG_AMITY_ACTIVITY_SETTLE_SECONDS};
    uint32_t owned_setup_generation=0;
    bool scoped_mode=false;system::OctopusId owned_controller;std::atomic<uint64_t> owned_config_revision{0};uint64_t owned_presentation_revision=0,cached_revision=0,owned_sequence=0,received_sequence=0,received_generation=0;
    uint32_t processed_selection=0,pending_selection=0;
    OwnedSender owned_sender;
    std::vector<system::OwnedAdapterSnapshot> owned_reports;
    // Cross-task observations are latest state, rather than ordinary work in
    // the input FIFO. Coalescing cannot lose a final completion under input
    // pressure, and never retains outgoing CEC frames.
    std::vector<system::OwnedAdapterSnapshot> pending_reports;
    static constexpr size_t report_capacity=64*amity::max_tentacles;
    std::vector<std::optional<system::OwnedIntent>> sent_keys,owned_keys;
    std::vector<system::OwnedAuthorization> owned_key_auth;
    const system::OwnedAuthorization *executing_auth=nullptr;
    system::BootId received_controller_boot;system::LinkId received_link;
    std::vector<system::ScopedSelector> active_selectors;
    std::shared_ptr<const OwnedObserver> observer_slot;
    std::shared_ptr<const OwnedAuthorizer> authorizer_slot;
    std::shared_ptr<const OwnedDeadline> deadline_slot;
    std::shared_ptr<const OwnedInputObserver> input_observer_slot;
    std::string last_report;
    Impl(){lock=xSemaphoreCreateMutex();configuration_gate=xSemaphoreCreateMutex();}
    Hub& control_hub(){return scoped_mode?activity_hub:hub;}
    const Hub& control_hub()const{return scoped_mode?activity_hub:hub;}
    Impl* adapter(uint8_t id){return id==0?this:id<amity::max_tentacles?adapters[id-1].get():nullptr;}

    static bool cleanup(const Command& command) {
        return command.kind==Kind::Release || command.kind==Kind::OwnedFence ||
               (command.kind==Kind::Owned && command.owned.kind==system::OwnedIntentKind::key_up);
    }
    bool wires_idle() const {
        for (size_t i = 0; i < native_port_count; ++i) if (!native_ports[i].bus->idle()) return false;
        return true;
    }

    void passive_ports() {
        for (size_t i = 0; i < native_port_count; ++i) native_ports[i].bus->install_receive_policy({true, 15});
    }

    bool connector_configured(uint8_t id) const {
        if (id >= amity::max_tentacles) return false;
        if (hardware_profile) return cec::connector_enabled(*hardware_profile, id);
        return id == 0 && native_port_count == 2;
    }

    void poll_presence(uint64_t now) {
        (void)now; if (!hardware_profile) return;
        bool any_changed = false;
        for (uint8_t id = 0; id < amity::max_tentacles; ++id) {
            std::optional<bool> present;
            const auto& pin = hardware_profile->presence[id];
            if (connector_configured(id)) {
                if (pin.gpio >= 0) {
                    int level = -1;
                    if (presence_reader) level = presence_reader(pin.gpio);
#ifdef ESP_PLATFORM
                    else level = gpio_get_level(static_cast<gpio_num_t>(pin.gpio));
#endif
                    if (level >= 0) present = ((level != 0) == pin.present_level);
                } else if (id == 0 && (hardware_profile->id == cec::BoardProfileId::LegacyBoard2 ||
                                      hardware_profile->id == cec::BoardProfileId::LegacyButtonless)) present = true;
            }
            xSemaphoreTake(lock, portMAX_DELAY);
            const bool changed = presence[id] != present;
            any_changed = any_changed || changed;
            if (changed) {
                if (presence_generations[id] == std::numeric_limits<uint64_t>::max()) present.reset();
                else ++presence_generations[id];
                presence[id] = present;
            }
            xSemaphoreGive(lock);
            if (changed && present == std::optional<bool>{false}) if (auto* engine=adapter(id)) { engine->owned_wire_generation.fetch_add(1); engine->stop_holds(); }
        }
        if (any_changed && owner_started) publish();
    }

    esp_err_t start_buses_here() {
        for (size_t i = 0; i < native_port_count; ++i) {
            const auto result = native_ports[i].bus->start(native_ports[i].gpio,
                hardware_profile ? cec::Bus::QueueProfile::Native : cec::Bus::QueueProfile::Legacy);
            if (result != ESP_OK) {
                for (size_t j = 0; j < native_port_count; ++j) native_ports[j].bus->stop();
                return result;
            }
        }
        return ESP_OK;
    }

    esp_err_t start_buses() {
#if defined(ESP_PLATFORM) && !CONFIG_FREERTOS_UNICORE
        // ESP-IDF allocates a peripheral interrupt on the calling CPU.
        // Keep the shared 20 kHz CEC timer off the Wi-Fi/BLE controller's core 0.
        // A temporary task preserves synchronous error reporting and leaves
        // the generic GPIO transport independent of the application's layout.
        struct Startup { Impl *hub; SemaphoreHandle_t done; esp_err_t error; };
        Startup startup{this, xSemaphoreCreateBinary(), ESP_FAIL};
        if (!startup.done) return ESP_ERR_NO_MEM;
        const auto init = [](void *context) {
            auto &s = *static_cast<Startup *>(context);
            s.error = s.hub->start_buses_here();
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
        return start_buses_here();
#endif
    }

    esp_err_t start_owner() {
        auto err = start_buses(); if (err != ESP_OK) { reset_startup(); return err; }
        // Token state is task data. Allocate after PSRAM setup; only the
        // ISR-visible generation counters need the Impl's internal memory.
        for(uint8_t id=0;id<amity::max_tentacles;++id)if(auto* engine=adapter(id)){
            engine->owned_keys.resize(256);engine->owned_key_auth.resize(256);engine->sent_keys.resize(256);
        }
        // The same IPC FIFO preserves down/up order. Cleanup has its own
        // reservations so ordinary input pressure cannot lose a physical up.
        commands = xQueueCreate(96, sizeof(Impl::Command *));
        command_slots = xSemaphoreCreateCounting(32, 32);
        cleanup_slots = xSemaphoreCreateCounting(64, 64);
        if (!commands || !command_slots || !cleanup_slots || xTaskCreate(Impl::run, "amity_hub", hub_task_stack_bytes, this, 5, &task) != pdPASS) {
            reset_startup(); return ESP_ERR_NO_MEM;
        }
        return ESP_OK;
    }

    void reset_startup() {
        for (size_t i = 0; i < native_port_count; ++i) native_ports[i].bus->stop();
        if (commands) { vQueueDelete(commands); commands = nullptr; }
        if (command_slots) { vSemaphoreDelete(command_slots); command_slots = nullptr; }
        if (cleanup_slots) { vSemaphoreDelete(cleanup_slots); cleanup_slots = nullptr; }
        for (auto& engine : adapters) engine.reset();
        native_port_count = 0; hardware_profile.reset(); presence.fill({});
        presence_generations.fill(1);
    }

    ~Impl() {
        if (task) vTaskDelete(task);
        // Detach every wire before owner/ISR-visible guards are destroyed.
        for (size_t i = 0; i < native_port_count; ++i) native_ports[i].bus->stop();
        if (commands) {
            Command *command;
            while (xQueueReceive(commands, &command, 0) == pdTRUE) delete command;
            vQueueDelete(commands);
        }
        if (lock) vSemaphoreDelete(lock);
        if (command_slots) vSemaphoreDelete(command_slots);
        if (cleanup_slots) vSemaphoreDelete(cleanup_slots);
        if (configuration_gate) vSemaphoreDelete(configuration_gate);
    }

    void free_reservation(const Command &command) {
        if(command.activity_reservation)--owner_activity_commands;
        if(command.selection_reservation)--activity_requests;
        xSemaphoreGive(command.cleanup_reservation ? cleanup_slots : command_slots);
    }

    esp_err_t submit(Command *command, const std::function<esp_err_t()> &commit = {}) {
        std::unique_ptr<Command> owned(command);
        if (!command) return ESP_ERR_NO_MEM;
        if (!commands || !command_slots || !task) return ESP_ERR_INVALID_STATE;
        if (settings_gate.load() && command->kind != Kind::SettingsCommit && command->kind != Kind::Retire &&
            command->kind != Kind::AuthorityTransition && !cleanup(*command)) return ESP_ERR_INVALID_STATE;
        if (maintenance.load() && command->kind != Kind::MaintenanceBegin && command->kind != Kind::MaintenanceEnd && command->kind != Kind::Retire && !cleanup(*command))
            return ESP_ERR_INVALID_STATE;
        if (authority_transition.load() && command->kind != Kind::AuthorityTransition && !cleanup(*command)) return ESP_ERR_INVALID_STATE;
        const bool configuration = command->kind == Kind::Configure || command->kind == Kind::ScopedConfigure;
        // Concurrent configuration callers must commit and enqueue in the
        // same order. Contention is rejected without blocking or persistence.
        if (configuration && xSemaphoreTake(configuration_gate, 0) != pdTRUE) return ESP_ERR_TIMEOUT;
        const auto unlock = [&] { if (configuration) xSemaphoreGive(configuration_gate); };
        command->cleanup_reservation = command->kind == Kind::OwnedFence ||
            (command->kind == Kind::Owned && command->owned.kind == system::OwnedIntentKind::key_up);
        if (xSemaphoreTake(command->cleanup_reservation ? cleanup_slots : command_slots,0)!=pdTRUE){unlock();return ESP_ERR_NO_MEM;}
        const auto error = commit ? commit() : ESP_OK;
        if (error != ESP_OK) { free_reservation(*command); unlock(); return error; }
        if(command->kind==Kind::Key||(command->kind==Kind::Owned&&command->owned.kind==system::OwnedIntentKind::key_down))
            command->setup_blocked=activity_setup_in_progress();
        // Reservations include deferred discovery requests. No CEC capacity
        // or completion wait may delay admission of another command.
        if (configuration || command->route_transition) {
            // Until compiled by the owner, a newer activity intent supersedes
            // this request. Already admitted frames retain their FIFO order.
            command->intent_generation = intent_generation.fetch_add(1) + 1;
        } else {
            command->intent_generation = intent_generation.load();
        }
        if (configuration) command->activity_receipt=activity_selection.fetch_add(1)+1;
        if(owner_activity(*command)){command->activity_reservation=true;++owner_activity_commands;}
        if(activity_request(*command)&&command->activity_receipt){command->selection_reservation=true;++activity_requests;}
        command = owned.release();
        if (xQueueSend(commands, &command, 0) != pdTRUE) std::abort();
        unlock();
        return ESP_OK;
    }

    static void finish(const std::shared_ptr<Reply>& reply, system::AdmissionResult result) {
        if (!reply || reply->canceled.load()) return;
        reply->result = std::move(result); xSemaphoreGive(reply->done);
    }

    system::AdmissionResult request(Command* command, uint32_t timeout = 1000) {
        if (!command) return {system::AdmissionCode::overloaded, "command allocation failed"};
        auto reply = std::make_shared<Reply>();
        if (!reply->done) { delete command; return {system::AdmissionCode::overloaded, "response reservation failed"}; }
        command->reply = reply;
        const auto result = submit(command);
        if (result != ESP_OK) return {system::AdmissionCode::overloaded, "owner queue admission failed"};
        if (xSemaphoreTake(reply->done, pdMS_TO_TICKS(std::min<uint32_t>(timeout, 5000))) != pdTRUE) {
            reply->canceled.store(true); return {system::AdmissionCode::expired, "owner validation deadline elapsed"};
        }
        return reply->result;
    }

    void complete_settings(uint64_t now) {
        if (!pending_settings) return;
        auto fail = [&](esp_err_t error, const char* reason) {
            pending_settings->settings_result = error;
            finish(pending_settings, {error == ESP_ERR_TIMEOUT ? system::AdmissionCode::expired : system::AdmissionCode::stale, reason});
            pending_settings.reset(); settings_persist = {}; settings_presence.clear();
            if (settings_paused && !retiring.load() && !authority_transition.load() && !maintenance.load()) {

                maintenance_started = false;
            }
            settings_paused = false; settings_gate.store(false);
        };
        if (pending_settings->canceled.load() || now >= settings_deadline) { fail(ESP_ERR_TIMEOUT, "Settings request expired before persistence"); return; }
        if (!settings_boot || settings_boot != local_boot || retiring.load() || authority_transition.load()) { fail(ESP_ERR_INVALID_STATE, "Amity identity changed before settings apply"); return; }
        poll_presence(now); // Re-read hardware, not the dialog's cached presence.
        for (const auto& expected : settings_presence) if (!connector_configured(expected.first) ||
            presence[expected.first] != std::optional<bool>{true} || presence_generations[expected.first] != expected.second) {
            fail(ESP_ERR_INVALID_STATE, "HDMI adapter changed; reopen Settings"); return;
        }
        if (!wires_idle() || false) return;
        uint8_t waiting = 0;
        if (!pending_settings->settings_effect.compare_exchange_strong(waiting, 1)) { fail(ESP_ERR_TIMEOUT, "Settings request was canceled"); return; }
        const auto result = settings_persist ? settings_persist() : ESP_ERR_INVALID_ARG;
        pending_settings->settings_effect.store(3);
        if (result != ESP_OK) { fail(result, "Settings were not saved"); return; }
        pending_settings->settings_result = ESP_OK;
        // Durable success cannot reopen old authority: the service sends its
        // acknowledgment, then restarts this selected device.
        maintenance.store(true); maintenance_applied.store(true);
        passive_ports(); finish(pending_settings, {system::AdmissionCode::accepted, {}});
        pending_settings.reset(); settings_persist = {}; settings_presence.clear();
    }

    bool enqueue(Command *command) { return submit(command) == ESP_OK; }

    void invalidate_binding(BusSide side, uint8_t address) override {
        if (address < 15) binding_wire_generation[side == BusSide::Front ? 0 : 1][address].fetch_add(1);
        for(size_t key=0;key<keys.size();++key)if(hub.held_recipient(static_cast<uint8_t>(key),side,address)){
            auto& state=keys[key];state.active=false;state.failed=true;state.waiting=false;++state.instance;
        }
    }

    void apply_emergency_release() {
        const auto generation = key_generation.load();
        if (generation != applied_key_generation) {
            if(scoped_mode)activity_hub.release_key();else stop_holds();
            applied_key_generation = generation;
        }
        for (size_t key = 0; key < keys.size(); ++key) {
            const auto released = release_generations[key].load();
            if (released == keys[key].release_generation) continue;
            if(scoped_mode)activity_hub.release_key(static_cast<uint8_t>(key));else stop_key(static_cast<uint8_t>(key));
            keys[key].release_generation = released;
        }
        // Cancelled lookups must return their command reservations immediately,
        // rather than filling the queue until an unrelated scan completes.
        for (auto i = deferred.begin(); i != deferred.end();) {
            if ((*i)->kind != Kind::Key || !stale_key(**i)) { ++i; continue; }
            i = deferred.erase(i); xSemaphoreGive(command_slots);
        }
    }

    bool any_keys_active() const {
        return std::any_of(keys.begin(), keys.end(), [](const HeldKey &key) { return key.active; });
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
            flight.key = -1; flight.instance = flight.setup = 0;flight.activity=false;
            return &flight;
        }
        return nullptr;
    }

    void send_held_key(uint8_t key) {
        auto &state = keys[key];
        if(owned_keys[key] && !owner_authorized(owned_key_auth[key],false)) { stop_key(key);owned_keys[key].reset();return; }
        auto *flight = reserve_flight();
        if (!flight) { state.active = false; state.failed = true; hub.release_key(key); return; }
        flight->key = key; flight->instance = state.instance;
        sending_key = flight;
        const auto* previous=executing_auth;
        if(owned_keys[key]&&!previous)executing_auth=&owned_key_auth[key];
        const bool accepted=owned_keys[key]?hub.execute_owned(*owned_keys[key]):hub.press_key(key,true);
        executing_auth=previous;
        sending_key = nullptr;
        if (accepted) state.waiting = true;
        else {
            flight->used = false;
            state.active = false; state.failed = !hub.pending_resolution();
            if (state.failed) hub.release_key(key);
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
            if (flight.activity) --activity_outstanding;
            changed = true;
        }
        if(scoped_mode)settling.external_replies(owned_replies_pending(),esp_timer_get_time());
        if (setup_pending && !setting_activity && setup_ready()) {
            xSemaphoreTake(lock, portMAX_DELAY);
            if(setup_finishes_settling)settling.finish(esp_timer_get_time());
            xSemaphoreGive(lock);
            setup_pending = false;
        }
        return changed;
    }

    bool transmissions_pending() const {
        for (const auto &flight : flights) if (flight.used) return true;
        return false;
    }

    bool activity_step_pending() const override {
        return std::any_of(flights.begin(),flights.end(),[](const auto& flight){
            return flight.used&&flight.activity&&flight.completion.result.load(std::memory_order_acquire)==cec::TxResult::Pending;
        });
    }
    bool owner_activity_pending() const {
        const auto& root=parent?*parent:*this;
        for(uint8_t id=0;id<amity::max_tentacles;++id)
            if(auto* engine=const_cast<Impl&>(root).adapter(id))if(engine->activity_step_pending())return true;
        return false;
    }
    static bool owner_activity(const system::OwnedIntent& intent) {
        using K=system::OwnedIntentKind;
        return intent.setup_generation&&intent.kind!=K::key_down&&intent.kind!=K::key_up&&intent.kind!=K::discover;
    }
    static bool owner_activity(const Command& command) { return command.kind==Kind::Owned&&owner_activity(command.owned); }
    static bool activity_request(const Command& command) {
        return command.kind==Kind::Select||command.kind==Kind::Standby||command.kind==Kind::Configure||command.kind==Kind::ScopedConfigure||
            (command.kind==Kind::Key&&command.value==0x40&&command.activity_started);
    }
    bool queued_owner_activity() const {
        return std::any_of(deferred.begin(),deferred.end(),[](const auto& command){return owner_activity(*command);});
    }
    bool activity_setup_in_progress() const {
        const auto& root=parent?*parent:*this;
        return root.activity_requests.load()||root.setup_running.load();
    }
    void refresh_activity_setup() {
        setup_running.store(setup_gates_input&&(control_hub().activity_pending()||(setup_pending&&!setup_ready())));
    }

    void continue_keys() {
        for (size_t key = 0; key < keys.size(); ++key)
            if (keys[key].active && !keys[key].waiting) send_held_key(static_cast<uint8_t>(key));
    }

    bool update_settling() {
        xSemaphoreTake(lock, portMAX_DELAY);
        const bool changed = cached.activity_settling != activity_settling(esp_timer_get_time());
        xSemaphoreGive(lock); return changed;
    }

    void fail_activity(int index, const char *reason) {
        last_activity_error = reason;
        xSemaphoreTake(lock, portMAX_DELAY);
        if (settling.target() == index) settling.cancel();
        xSemaphoreGive(lock);
    }

    bool compile_activity_intent(const Command &command) const {
        return command.intent_generation == intent_generation.load();
    }

    bool apply_activity(int index,bool gate_input=true) {
        const auto before = control_hub().snapshot();
        // A configuration queued before this request may have removed its index.
        if (index < -1 || index >= static_cast<int>(before.activities.size())) {
            fail_activity(index, "Activity configuration changed; choose the activity again.");
            return true;
        }
        const bool change = before.current_activity != index;
        const auto ready = [&] {
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
        if (scoped_mode) activity_hub.release_key(); else stop_holds();
        ++setup_generation; setup_outstanding = 0; setup_pending = true; setup_owners.clear();input_setups.clear();
        setup_finishes_settling=change;setup_gates_input=gate_input;setup_running.store(gate_input);
        setting_activity = true;
        };
        // Resuming the original recipe must retain completion tracking without
        // replaying its initial key cleanup, setup generation or settling gate.
        setting_activity=control_hub().activity_pending();
        const bool applied = index < 0 ? control_hub().standby(ready) : control_hub().select_activity(index,ready);
        setting_activity = false;
        if (control_hub().pending_resolution()||control_hub().activity_pending()) return false;
        if (!applied) {
            fail_activity(index, "Required equipment is missing or ambiguous; run discovery and retry.");
            return true;
        }
        last_activity_error.clear();
        if (change && setup_ready()) {
            xSemaphoreTake(lock, portMAX_DELAY); settling.finish(esp_timer_get_time()); xSemaphoreGive(lock);
            setup_pending = false;
        }
        return true;
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
        // Admission is immediate. All messages drop when the eight-slot FIFO is full.
        auto &bus = side == BusSide::Front ? front : back;
        // Once admitted, work finishes in FIFO order through activity changes.
        // Only a changed physical endpoint can invalidate that recipient. Key
        // input-loss fallback additionally fences presses by its own epoch.
        const unsigned port = side == BusSide::Front ? 0 : 1;
        cec::ExecutionFence fence{};
        if (destination < 15) {
            const auto generation = data[0] == 0x45 ? release_binding_generation[port][destination] :
                binding_wire_generation[port][destination].load();
            fence = {&binding_wire_generation[port][destination], generation};
        }
        if (data[0] == 0x44 && !setting_activity) {
            fence.secondary_generation = &key_generation;
            fence.expected_secondary_generation = applied_key_generation;
        }
        if(executing_auth) {
            fence.secondary_generation=&owned_wire_generation;
            fence.expected_secondary_generation=owned_wire_generation.load();
            uint64_t deadline=executing_auth->deadline_ms;
            const auto& root=parent?*parent:*this;
            auto current=std::atomic_load(&root.deadline_slot);
            if(current){
                const auto authority_deadline=(*current)(*executing_auth);
                if(!authority_deadline)return false;
                if(authority_deadline!=UINT64_MAX)deadline=deadline?std::min(deadline,authority_deadline):authority_deadline;
            }
            if(deadline&&deadline!=UINT64_MAX&&data[0]!=0x45)fence.expires_at_us=deadline>UINT64_MAX/1000?UINT64_MAX:deadline*1000;
        }
        const unsigned attempts = data[0] == 0x44 ? 1 : 3;
        const bool paced=setting_activity||pacing_receive;
        auto *flight = sending_key && data[0] == 0x44 ? sending_key : paced ? reserve_flight() : nullptr;
        const bool sent = flight ? bus.send_async_guarded(frame, fence, flight->completion, attempts) :
            !paced && bus.send_guarded(frame, fence, attempts);
        if (flight && !sent) flight->used = false;
        if (sent && data[0] == 0x44 && destination < 15)
            release_binding_generation[port][destination] = binding_wire_generation[port][destination].load();
        if(sent&&paced){flight->activity=true;++activity_outstanding;}
        if (sent && setting_activity) {
            flight->setup = setup_generation;
            if(flight->setup)++setup_outstanding;
            xSemaphoreTake(lock, portMAX_DELAY);
            if(executing_auth)owned_settling.expect(side,destination,data,size);else settling.expect(side,destination,data,size);
            xSemaphoreGive(lock);
        }
        if (!sent) ++dropped_tx;
        return sent;
    }

    static uint8_t scan_side(BusSide side) { return side == BusSide::Front ? 1 : 2; }
    uint8_t pending_scan_sides() const { return requested_scan_sides | (scan_running ? discovery.sides : 0); }
    bool request_lookup_scan(BusSide side) {
        const auto mask = scan_side(side);
        if (pending_scan_sides() & mask) return true;
        const auto index = side == BusSide::Front ? 0 : 1;
        if (esp_timer_get_time() - last_scan[index] < 120000000) return false;
        requested_scan_sides |= mask;
        return true;
    }
    bool request_scan(BusSide side) override { return request_lookup_scan(side); }
    void request_scan() override {
        // The owner-local qualified path always supplies a side. Retain the
        // unqualified transport entry for the original direct Hub callers.
        request_lookup_scan(BusSide::Front); request_lookup_scan(BusSide::Back);
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
                owned_settling.received(side,frame,side==BusSide::Front?front_address:back_address,esp_timer_get_time(),true);
                xSemaphoreGive(lock);
                const bool previous=pacing_receive;pacing_receive=true;
                const auto pending = hub.receive(side, frame);
                pacing_receive=previous;
                if (pending) {
                    auto& root=parent?*parent:*this;
                    if (xSemaphoreTake(root.command_slots,0)==pdTRUE) {
                        auto command=std::unique_ptr<Command>(new(std::nothrow)Command(Kind::Received));
                        if(!command){xSemaphoreGive(root.command_slots);ESP_LOGW(TAG,"Received HDMI-CEC lookup dropped because owner allocation failed");received=true;continue;}
                        command->received_frame=frame;command->received_side=side;command->received_adapter=adapter_id;
                        command->received_step=hub.receive_step();
                        command->received_generation=received_generation;command->received_activity=hub.activity_generation();
                        command->intent_generation=root.intent_generation.load();
                        command->received_boot=root.local_boot;command->received_presence=root.presence_generations[adapter_id];
                        command->received_binding=frame.source()<15?binding_wire_generation[side==BusSide::Front?0:1][frame.source()].load():0;
                        command->received_wire=owned_wire_generation.load();
                        command->received_deadline=uint64_t(esp_timer_get_time())+120000000;
                        command->discovery_sides=pending;root.deferred.push_back(std::move(command));
                    } else ESP_LOGW(TAG,"Received HDMI-CEC lookup dropped because owner IPC is full");
                }
                received = true;
            }
        }
        return received;
    }

    static bool secure_random(uint8_t* bytes, size_t size) {
#ifdef ESP_PLATFORM
        esp_fill_random(bytes, size);
#else
        std::random_device rng; for (size_t i = 0; i < size; ++i) bytes[i] = static_cast<uint8_t>(rng());
#endif
        return true;
    }

    bool discovery_pending() const {
        return requested_scan_sides || discovery.phase != DiscoveryPhase::Idle;
    }

    void begin_claim(BusSide side) {
        discovery.phase = side == BusSide::Back ? DiscoveryPhase::ClaimBack : DiscoveryPhase::ClaimFront;
        discovery.candidate = 0;
        discovery.nacks = 0;
    }

    void start_discovery() {
        if (discovery.phase != DiscoveryPhase::Idle) return;
        discovery.sides = requested_scan_sides;
        requested_scan_sides = 0;
        scan_running = true;
        last_claim = esp_timer_get_time();
        for (const auto side : {BusSide::Front, BusSide::Back}) if (discovery.sides & scan_side(side))
            last_scan[side == BusSide::Front ? 0 : 1] = last_claim;
        discovery.claim_only = false;
        discovery.claimed = false;
        hub.begin_discovery(discovery.sides); // Existing cache remains usable until finish.
        begin_claim(discovery.sides & 2 ? BusSide::Back : BusSide::Front);
        ESP_LOGI(TAG, "Discovering devices on HDMI-CEC %s", discovery.sides == 3 ? "both buses" : discovery.sides == 1 ? "front bus" : "back bus");
        (parent?parent:this)->publish();
    }

    void finish_claim(BusSide side) {
        if (side == BusSide::Back) {
            if (discovery.claim_only) begin_claim(BusSide::Front);
            else { discovery.phase = DiscoveryPhase::ScanBack; discovery.address = 0; }
        } else if (discovery.claim_only) {
            discovery.phase = DiscoveryPhase::Idle;
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
                (parent?parent:this)->publish();
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
            if (requested_scan_sides) start_discovery();
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
                if (discovery.sides & 1) begin_claim(BusSide::Front);
                else finish_discovery();
            } else {
                finish_discovery();
            }
            break;
        case DiscoveryPhase::Idle: break;
        }
    }

    void finish_discovery() {
        hub.finish_discovery();
        ++discovery_generation;
        scan_running = false;
        discovery.phase = DiscoveryPhase::Idle;
        (parent?parent:this)->publish();
    }

    bool stale_key(const Command &command) const {
        return command.key_generation != applied_key_generation ||
            command.release_generation != keys[static_cast<uint8_t>(command.value)].release_generation ||
            (command.activity_bound && command.activity_generation != control_hub().activity_generation());
    }
    bool current_received(const Command& command) {
        auto* engine=adapter(command.received_adapter);
        return engine&&command.intent_generation==intent_generation.load()&&command.received_boot==local_boot&&
            command.received_presence==presence_generations[command.received_adapter]&&command.received_generation==engine->received_generation&&
            command.received_activity==engine->hub.activity_generation()&&command.received_wire==engine->owned_wire_generation.load()&&
            command.received_binding==(command.received_frame.source()<15?engine->binding_wire_generation[command.received_side==BusSide::Front?0:1][command.received_frame.source()].load():0)&&
            uint64_t(esp_timer_get_time())<command.received_deadline;
    }

    bool execute(Command &command) {
        if(command.kind==Kind::Owned){
            auto* engine=adapter(command.owned.target.port.tentacle_id);
            if(command.owned_waiting){
                if(owner_activity_pending())return false;
                complete_owned(command,engine&&engine->owner_authorized(command.auth,false)?command.owned_result:
                    system::AdmissionResult{system::AdmissionCode::unauthorized,"Original controller authority is unavailable"});return true;
            }
            const auto result=engine?engine->execute_owned(command.owned,command.auth,false,command.owned_received,command.setup_blocked):
                system::AdmissionResult{system::AdmissionCode::invalid,"Unknown HDMI adapter"};
            command.owned_received=true;
            if(result.code==system::AdmissionCode::pending){command.discovery_sides=engine->hub.pending_resolution();return false;}
            if(owner_activity(command)&&owner_activity_pending()){
                command.owned_result=result;command.owned_waiting=true;command.discovery_sides=0;
                return false;
            }
            complete_owned(command,result);return true;
        }
        if(command.kind==Kind::ScopedConfigure){
            // Keep the old selectors/revision through this Amity's original
            // cleanup. Other owners progress independently on their own lane.
            if(!command.configuration_applied){
                setup_gates_input=true;
                setting_activity=true;
                const bool applied=activity_hub.configure(command.activities,[&]{activity_hub.release_key();});
                setting_activity=false;
                if(!applied){command.discovery_sides=activity_hub.pending_resolution();return false;}
                command.configuration_applied=true;
            }
            if(owner_activity_pending()||owner_activity_commands.load())return false;
        }
        if (owned_command(command)) return true;
        switch (command.kind) {
        case Kind::Configure:
            setup_gates_input=true;
            setting_activity=true;
            if (!control_hub().configure(command.activities,[&]{if(scoped_mode)activity_hub.release_key();else stop_holds();})) {
                setting_activity=false;
                command.discovery_sides=control_hub().pending_resolution();return false;
            }
            setting_activity=false;
            ++setup_generation; setup_outstanding = 0; setup_pending = false;
            xSemaphoreTake(lock, portMAX_DELAY); settling.cancel(); xSemaphoreGive(lock);
            break;
        case Kind::Select:
            if (command.activity_started||compile_activity_intent(command)) { command.activity_started=true;if(!apply_activity(command.value,command.activity_receipt!=0)){command.discovery_sides=control_hub().pending_resolution();return false;} }
            else fail_activity(command.value, "Activity request was superseded by a newer change.");
            break;
        case Kind::Key:
            if (stale_key(command)) break;
            if(command.activity_started){if(!apply_activity(-1)){command.discovery_sides=control_hub().pending_resolution();return false;}break;}
            if(command.setup_blocked||activity_setup_in_progress()){
                break;
            }
            if (command.value == 0x40) { command.activity_started=true;if(!apply_activity(-1)){command.discovery_sides=control_hub().pending_resolution();return false;} break; }
            if (scoped_mode) { activity_hub.press_key(static_cast<uint8_t>(command.value),command.repeat);break; }
            if (command.repeat) {
                auto &state = keys[static_cast<uint8_t>(command.value)];
                if (state.active || state.failed) break;
                state.active = true; ++state.instance;
                // Initial submission never waits for another key's completion.
                send_held_key(static_cast<uint8_t>(command.value));
            } else hub.press_key(static_cast<uint8_t>(command.value));
            if(hub.pending_resolution()){command.discovery_sides=hub.pending_resolution();return false;}
            break;
        case Kind::Release:
            if (command.value < 0) {
                if(scoped_mode)activity_hub.release_key();else stop_holds();
                // Disconnect/input loss also cancels key operations still
                // awaiting device lookup, before they can reach the wire.
                for (auto i = deferred.begin(); i != deferred.end();) {
                    if ((*i)->kind != Kind::Key) { ++i; continue; }
                    i = deferred.erase(i); xSemaphoreGive(command_slots);
                }
            } else {
                if(scoped_mode)activity_hub.release_key(static_cast<uint8_t>(command.value));else stop_key(static_cast<uint8_t>(command.value));
                // A short tap whose recipient is still being discovered is
                // finite: lookup may finish later, but cannot resurrect a hold.
                for (auto &pending : deferred)
                    if (pending->kind == Kind::Key && pending->value == command.value) pending->repeat = false;
            }
            break;
        case Kind::Standby:
            if (command.activity_started||compile_activity_intent(command)) { command.activity_started=true;if(!apply_activity(-1,command.activity_receipt!=0)){command.discovery_sides=control_hub().pending_resolution();return false;} }
            else fail_activity(-1, "Activity request was superseded by a newer change.");
            break;
        case Kind::Discover: requested_scan_sides = 3; if(scoped_mode)request_owned_discovery(); break;
        case Kind::Received: {
            auto* engine=adapter(command.received_adapter);
            if(!current_received(command))break;
            const bool previous=engine->pacing_receive;engine->pacing_receive=true;
            command.discovery_sides=engine->hub.receive(command.received_side,command.received_frame,command.received_step);
            engine->pacing_receive=previous;
            command.received_step=engine->hub.receive_step();
            if(command.discovery_sides)return false;
            break;
        }
        default: break; // Handled by remote_command above.
        }
        record_selection(command);
        return true;
    }
    void record_selection(const Command& command){
        if(!command.activity_receipt)return;
        if(scoped_mode&&activity_hub.input_pending())pending_selection=command.activity_receipt;
        else processed_selection=command.activity_receipt;
    }
    void finish_input_selection(){
        if(!activity_hub.input_pending()&&pending_selection){processed_selection=pending_selection;pending_selection=0;}
    }

    void accept(Command *raw) {
        std::unique_ptr<Command> command(raw);
        if(command->reply&&command->reply->canceled.load()&&command->kind!=Kind::SettingsCommit){free_reservation(*command);return;}
        if(command->kind==Kind::Owned){
            auto* engine=adapter(command->owned.target.port.tentacle_id);
            if(command->owned.kind==system::OwnedIntentKind::key_up)for(auto& pending:deferred)
                if(pending->kind==Kind::Owned&&pending->owned.kind==system::OwnedIntentKind::key_down&&
                   pending->owned.token==command->owned.token&&pending->owned.target==command->owned.target)pending->owned.repeat=false;
            if(owner_activity(*command)&&(owner_activity_pending()||queued_owner_activity())){
                const auto result=engine?engine->execute_owned(command->owned,command->auth,true,command->owned_received):
                    system::AdmissionResult{system::AdmissionCode::invalid,"Unknown HDMI adapter"};
                if(!result){free_reservation(*command);complete_owned(*command,result);return;}
                command->owned_received=true;
            }else if(execute(*command)){free_reservation(*command);return;}
            if(!command->owned_deferred)finish(command->reply,{system::AdmissionCode::pending,"Original owner action is in progress"});
            command->owned_deferred=true;
            if(command->discovery_sides||command->owned_waiting)command->auth.deadline_ms=0;
            deferred.push_back(std::move(command));return;
        }
        if(command->kind==Kind::OwnedBind&&(owner_activity_pending()||owner_activity_commands.load()||hub.activity_pending())){
            deferred.push_back(std::move(command));return;
        }
        const bool earlier_activity=std::any_of(deferred.begin(),deferred.end(),[](const auto& pending){return activity_request(*pending);});
        if(activity_request(*command)&&(earlier_activity||control_hub().activity_pending()||
            ((command->kind==Kind::Select||command->kind==Kind::Standby)&&
             ((scoped_mode&&activity_hub.input_pending())||(setup_pending&&!setup_ready()))))){
            deferred.push_back(std::move(command));return;
        }
        if(!execute(*command)){deferred.push_back(std::move(command));return;}
        free_reservation(*command);
    }

    bool resume_discovered() {
        bool changed = false,owner_blocked=false,activity_blocked=false;
        for (auto i = deferred.begin(); i != deferred.end();) {
            const bool owner_step=owner_activity(**i),activity=activity_request(**i);
            if(owner_step&&owner_blocked){++i;continue;}
            if(activity&&activity_blocked){++i;continue;}
            if(owner_step)owner_blocked=true;
            if(activity)activity_blocked=true;
            if((*i)->kind==Kind::Owned){
                auto* engine=adapter((*i)->owned.target.port.tentacle_id);
                if(owner_step&&!(*i)->owned_waiting&&owner_activity_pending()){++i;continue;}
                if(engine&&(engine->pending_scan_sides()&(*i)->discovery_sides)){++i;continue;}
            }else if((*i)->kind==Kind::Received){
                if(!current_received(**i)){free_reservation(**i);i=deferred.erase(i);changed=true;continue;}
                auto* engine=adapter((*i)->received_adapter);
                if(engine&&(engine->pending_scan_sides()&(*i)->discovery_sides)){++i;continue;}
            }else if((*i)->kind==Kind::OwnedBind){
                if(owner_activity_pending()||owner_activity_commands.load()||hub.activity_pending()){++i;continue;}
            }else if(activity){
                if(((*i)->kind==Kind::Select||(*i)->kind==Kind::Standby)&&
                   ((scoped_mode&&activity_hub.input_pending())||(setup_pending&&!setup_ready()))&&!control_hub().activity_pending()){++i;continue;}
                if(!scoped_mode&&(pending_scan_sides()&(*i)->discovery_sides)){++i;continue;}
            }else if(!scoped_mode&&(pending_scan_sides()&(*i)->discovery_sides)){++i;continue;}
            auto command = std::move(*i);
            i = deferred.erase(i);
            if(!execute(*command)){
                // Keep the semantic program counter in arrival order. Keys
                // and received frames still advance independently past it.
                i=deferred.insert(i,std::move(command));++i;continue;
            }
            free_reservation(*command);
            if(owner_step)owner_blocked=false;
            if(activity)activity_blocked=false;
            changed = true;
        }
        return changed;
    }

    bool owner_authorized(const system::OwnedAuthorization& auth,bool arrival=true) const {
        const auto& root=parent?*parent:*this;
        if(!root.owned_controller || auth.controller_id!=root.owned_controller || auth.owner_boot!=root.local_boot ||
           auth.config_revision!=root.owned_config_revision)return false;
        if(arrival && auth.deadline_ms && uint64_t(esp_timer_get_time())/1000>=auth.deadline_ms)return false;
        auto check=std::atomic_load(&root.authorizer_slot);
        auto current=auth;if(!arrival)current.deadline_ms=0;
        return !check || (*check)(current);
    }
    system::AdmissionResult execute_owned(const system::OwnedIntent& intent,const system::OwnedAuthorization& auth,bool validate_only=false,bool received=false,bool setup_blocked=false) {
        auto& root=parent?*parent:*this;
        using K=system::OwnedIntentKind;
        const auto expected=intent.kind==K::key_up?system::OwnedAuthority::cleanup:intent.kind==K::key_down?system::OwnedAuthority::realtime:system::OwnedAuthority::control;
        if(auth.authority!=expected)return {system::AdmissionCode::unauthorized,"Original action has the wrong authority class"};
        if(!owner_authorized(auth))return {system::AdmissionCode::unauthorized,"Original controller authority is unavailable"};
        if(intent.target.port.octopus_id!=root.octopus || intent.target.port.consortium_id!=root.consortium ||
           intent.target.port.tentacle_id!=adapter_id || !root.connector_configured(adapter_id) || root.presence[adapter_id]==std::optional<bool>{false})
            return {system::AdmissionCode::invalid,"The selected HDMI adapter is unavailable"};
        if(root.maintenance.load()||root.retiring.load())return {system::AdmissionCode::unauthorized,"This Amity is unavailable while updating or removed"};
        if(!intent.generation||!intent.sequence)return {system::AdmissionCode::invalid,"Missing original input generation"};
        if(received_controller_boot!=auth.controller_boot || received_link!=auth.link_id) {
            stop_holds();std::fill(owned_keys.begin(),owned_keys.end(),std::nullopt);received_sequence=received_generation=0;
            xSemaphoreTake(root.lock,portMAX_DELAY);
            received_controller_boot=auth.controller_boot;received_link=auth.link_id;
            xSemaphoreGive(root.lock);
        }
        if(!received){
            if(intent.sequence<=received_sequence)return {system::AdmissionCode::stale,"Original input was already consumed"};
            received_sequence=intent.sequence;
        }
        if(validate_only){
            if(intent.kind!=K::key_up){
                if(intent.generation<received_generation)return {system::AdmissionCode::stale,"Activity input was superseded"};
            }
            return {system::AdmissionCode::accepted,{}};
        }
        if(intent.kind==K::key_up) {
            auto& retained=owned_keys[intent.key];
            if(!retained || retained->token!=intent.token)return {system::AdmissionCode::accepted,{}};
            stop_key(intent.key);retained.reset();return {system::AdmissionCode::accepted,{}};
        }
        if(intent.generation<received_generation)return {system::AdmissionCode::stale,"Activity input was superseded"};
        if(intent.kind==K::key_down&&(setup_blocked||root.activity_setup_in_progress())){
            // No pressed registration is made for a discarded transition
            // down. Its later up therefore uses the ordinary no-token NOP.
            return {system::AdmissionCode::accepted,{}};
        }
        received_generation=intent.generation;
        if(intent.kind==K::context&&intent.selected&&intent.source&&intent.source->port.octopus_id==intent.target.port.octopus_id&&intent.source->port.tentacle_id==intent.target.port.tentacle_id){
            hub.find_device(intent.source->osd_name,intent.source->port.side==system::Side::front?BusSide::Front:BusSide::Back);
            if(hub.pending_resolution())return {system::AdmissionCode::pending,"Original owner lookup is in progress"};
        }
        if(intent.kind==K::power||intent.kind==K::source_input||intent.kind==K::switch_input||intent.kind==K::query_audio||intent.kind==K::key_down){
            const auto* device=hub.find_device(intent.target.osd_name,intent.target.port.side==system::Side::front?BusSide::Front:BusSide::Back);
            if(hub.pending_resolution())return {system::AdmissionCode::pending,"Original owner lookup is in progress"};
            if(!device)return intent.kind==K::source_input?
                system::AdmissionResult{system::AdmissionCode::unresolved,"The named source is absent from this owner",false}:
                system::AdmissionResult{system::AdmissionCode::overloaded,"Original action could not resolve its equipment"};
        }
        struct SettingScope {bool& value;bool previous;~SettingScope(){value=previous;}} setting_scope{setting_activity,setting_activity};
        if(owner_activity(intent))setting_activity=true;
        if(intent.setup_generation){
            if(owned_setup_generation!=intent.setup_generation){
                owned_setup_generation=intent.setup_generation;
                const auto before=hub.snapshot();
                // Only actual original requests create reply expectations;
                // every matching recipient is resolved in this owner's cache.
                std::vector<ActivitySettling::Peer> peers;for(const auto& d:before.devices)peers.push_back({d.bus,d.address,d.physical_address});
                owned_settling.start(intent.selected?0:-1,std::move(peers));
            }
            if(setup_generation!=intent.setup_generation){setup_generation=intent.setup_generation;setup_outstanding=0;}
            setup_last_sequence=intent.sequence;setup_pending=true;setting_activity=true;
        }
        if(intent.kind==K::discover){requested_scan_sides=3;return {system::AdmissionCode::accepted,{}};}
        executing_auth=&auth;
        bool admitted=false;
        if(intent.kind==K::key_down&&intent.repeat) {
            auto& retained=owned_keys[intent.key];auto& state=keys[intent.key];
            if(retained) {
                executing_auth=nullptr;
                return retained->token==intent.token?system::AdmissionResult{system::AdmissionCode::accepted,{}}:
                    system::AdmissionResult{system::AdmissionCode::stale,"Matching button-up is required before another down"};
            }
            retained=intent;owned_key_auth[intent.key]=auth;
            // The received command deadline bounds admission, not the hold.
            // Subsequent attempts use current authenticated link authority.
            owned_key_auth[intent.key].deadline_ms=0;
            state.active=true;state.failed=false;++state.instance;
            send_held_key(intent.key);admitted=!state.failed;
        } else admitted=hub.execute_owned(intent);
        executing_auth=nullptr;
        if(hub.pending_resolution())return {system::AdmissionCode::pending,"Original owner lookup is in progress"};
        const auto present=intent.kind==K::source_input?std::optional<bool>{true}:std::nullopt;
        return admitted?system::AdmissionResult{system::AdmissionCode::accepted,{},present}:
            system::AdmissionResult{system::AdmissionCode::overloaded,"Original action could not enter the CEC FIFO or resolve its equipment",present};
    }
    std::optional<Device> lookup_owned(const system::QualifiedDeviceName& name) const {
        for(const auto& report:owned_reports)if(report.owner==name.port.octopus_id&&report.adapter==name.port.tentacle_id){
            if(!name.osd_name.empty())for(const auto& device:report.devices)if(device.side==name.port.side&&device.osd_name==name.osd_name)
                return Device{name.osd_name,name.port.side==system::Side::front?BusSide::Front:BusSide::Back,
                    device.logical_address,device.physical_address,device.device_type,device.vendor_id,true};
        }
        return {};
    }
    system::OwnedDispatchResult dispatch_owned(system::OwnedIntent& intent) {
        using K=system::OwnedIntentKind;
        if(owned_sequence==UINT64_MAX)return false;
        intent.sequence=++owned_sequence;
        intent.config_revision=owned_config_revision;
        if(setting_activity)intent.setup_generation=setup_generation;
        if(intent.kind==K::key_down&&intent.repeat){
            auto& retained=sent_keys[intent.key];
            if(retained){intent.token=retained->token;return true;}
            intent.token=system::HoldToken::generate(secure_random).value_or(system::HoldToken{});
            if(!intent.token)return false;
        }else if(intent.kind==K::key_down){
            intent.token=system::HoldToken::generate(secure_random).value_or(system::HoldToken{});
            if(!intent.token)return false;
        }else if(intent.kind==K::key_up){
            auto& retained=sent_keys[intent.key];if(!retained)return true;
            const auto sequence=intent.sequence;intent=*retained;intent.sequence=sequence;intent.kind=K::key_up;retained.reset();
        }
        const auto dispatched=[&](bool admitted,std::optional<bool> present={}){
            // Only consumed downs have a matching coordinator hold/up. A
            // rejected local request must not leave a token that suppresses
            // the next physical press after its adapter or authority returns.
            if(admitted&&intent.kind==K::key_down&&intent.repeat)sent_keys[intent.key]=intent;
            return system::OwnedDispatchResult{admitted,present,intent.sequence};
        };
        if(intent.target.port.octopus_id==octopus){
            auto* engine=adapter(intent.target.port.tentacle_id);if(!engine)return false;
            if(intent.kind==K::key_up)for(auto& pending:deferred)
                if(pending->kind==Kind::Owned&&pending->owned.kind==K::key_down&&pending->owned.token==intent.token&&pending->owned.target==intent.target)pending->owned.repeat=false;
            system::OwnedAuthorization auth;auth.controller_id=owned_controller;auth.controller_boot=auth.owner_boot=local_boot;auth.config_revision=owned_config_revision;
            auth.authority=intent.kind==K::key_up?system::OwnedAuthority::cleanup:intent.kind==K::key_down?system::OwnedAuthority::realtime:system::OwnedAuthority::control;
            const auto validated=engine->execute_owned(intent,auth,true);
            if(!validated)return {false,{},intent.sequence};
            if(owner_activity(intent)&&(owner_activity_pending()||owner_activity_commands.load())){
                auto* request=new(std::nothrow)Command{Kind::Owned};if(!request)return false;
                request->owned=intent;request->auth=auth;request->owned_received=true;
                const bool admitted=submit(request)==ESP_OK;
                if(admitted&&intent.kind==K::source_input)input_setups.push_back({intent.sequence,intent.setup_generation});
                if(admitted&&intent.setup_generation)expect_setup(intent);
                return dispatched(admitted);
            }
            const auto result=engine->execute_owned(intent,auth,false,true);
            if(result.code==system::AdmissionCode::pending||(owner_activity(intent)&&owner_activity_pending())){
                auto* request=new(std::nothrow)Command{Kind::Owned};if(!request)return false;
                request->owned=intent;request->auth=auth;request->owned_received=true;
                if(result.code!=system::AdmissionCode::pending){request->owned_waiting=true;request->owned_result=result;}
                const bool admitted=submit(request)==ESP_OK;
                if(admitted&&intent.kind==K::source_input)input_setups.push_back({intent.sequence,intent.setup_generation});
                if(admitted&&intent.setup_generation)expect_setup(intent);
                return dispatched(admitted);
            }
            // Original public input admission is asynchronous. A consumed down
            // remains paired with its eventual physical up after wire failure.
            const bool admitted=result.code==system::AdmissionCode::accepted||result.code==system::AdmissionCode::overloaded||result.code==system::AdmissionCode::unresolved;
            const auto present=result.source_present;
            if(admitted&&intent.setup_generation)expect_setup(intent);
            return dispatched(admitted,present);
        }
        const bool admitted=owned_sender&&owned_sender(intent);
        if(admitted&&intent.setup_generation)expect_setup(intent);
        if(admitted&&intent.kind==K::source_input)input_setups.push_back({intent.sequence,intent.setup_generation});
        return dispatched(admitted);
    }
    void expect_setup(const system::OwnedIntent& intent){
        auto found=std::find_if(setup_owners.begin(),setup_owners.end(),[&](const auto& owner){return owner.owner==intent.target.port.octopus_id&&owner.adapter==intent.target.port.tentacle_id;});
        if(found==setup_owners.end())setup_owners.push_back({intent.target.port.octopus_id,intent.target.port.tentacle_id,intent.sequence});
        else found->sequence=intent.sequence;
    }
    bool setup_ready()const{
        if(setup_outstanding)return false;
        if(control_hub().activity_pending())return false;
        if(!scoped_mode)return true;
        if(activity_hub.input_pending())return false;
        for(const auto& expected:setup_owners){
            if(expected.owner==octopus){auto* engine=const_cast<Impl*>(this)->adapter(expected.adapter);
                if(!engine||engine->setup_generation!=setup_generation||engine->setup_last_sequence<expected.sequence||engine->setup_outstanding)return false;
            }else{
                const auto found=std::find_if(owned_reports.begin(),owned_reports.end(),[&](const auto& report){return report.owner==expected.owner&&report.adapter==expected.adapter;});
                if(found==owned_reports.end()||found->setup_generation!=setup_generation||found->setup_last_sequence<expected.sequence||found->setup_pending)return false;
            }
        }
        return true;
    }
    bool owned_replies_pending()const{
        for(const auto& expected:setup_owners){
            if(expected.owner==octopus){auto* engine=const_cast<Impl*>(this)->adapter(expected.adapter);if(engine&&engine->owned_settling.pending_replies())return true;}
            else{auto report=std::find_if(owned_reports.begin(),owned_reports.end(),[&](const auto& r){return r.owner==expected.owner&&r.adapter==expected.adapter;});if(report!=owned_reports.end()&&report->setup_generation==setup_generation&&report->setup_replies_pending)return true;}
        }
        return false;
    }
    bool activity_settling(uint64_t now){return settling.active(now);}
    void request_owned_discovery(){
        std::vector<system::CecPortRef> ports;
        for(const auto& selector:active_selectors){auto port=selector.target.port;port.side=system::Side::front;
            if(std::find(ports.begin(),ports.end(),port)!=ports.end())continue;
            ports.push_back(port);system::OwnedIntent intent;intent.kind=system::OwnedIntentKind::discover;
            intent.target.port=port;intent.generation=control_hub().scoped_generation();dispatch_owned(intent);
        }
    }
    system::OwnedAdapterSnapshot adapter_report(Impl& engine) const {
        auto state=engine.hub.snapshot();system::OwnedAdapterSnapshot report;
        report.owner=octopus;report.boot=local_boot;report.adapter=engine.adapter_id;
        report.front_address=state.front_address;report.back_address=state.back_address;report.front_physical_address=state.front_physical_address;
        report.ready=state.ready;report.discovering=engine.scan_running;report.discovery_generation=engine.discovery_generation;
        report.setup_generation=engine.setup_generation;report.setup_last_sequence=engine.setup_last_sequence;report.setup_pending=engine.setup_outstanding;
        report.setup_replies_pending=engine.owned_settling.pending_replies();
        report.audio_status_known=state.audio_status_known;report.muted=state.muted;report.volume=state.volume;
        if(presence[engine.adapter_id]!=std::optional<bool>{false})for(const auto& device:state.devices){
            report.devices.push_back({device.bus==BusSide::Front?system::Side::front:system::Side::back,device.name,device.address,device.type,device.physical_address,device.vendor_id,!device.name.empty()});
        }
        return report;
    }
    static std::string report_identity(const system::OwnedAdapterSnapshot& report){
        std::string value=std::to_string(report.front_address)+":"+std::to_string(report.back_address)+":"+std::to_string(report.front_physical_address)+":"+
            std::to_string(report.discovering)+":"+std::to_string(report.discovery_generation)+":"+std::to_string(report.audio_status_known)+":"+std::to_string(report.muted)+":"+std::to_string(report.volume);
        value+=":"+std::to_string(report.setup_generation)+":"+std::to_string(report.setup_last_sequence)+":"+std::to_string(report.setup_pending)+":"+std::to_string(report.setup_replies_pending);
        for(const auto& d:report.devices)value+=":"+std::to_string(int(d.side))+":"+d.osd_name+":"+std::to_string(d.logical_address)+":"+std::to_string(d.physical_address)+":"+std::to_string(d.device_type)+":"+std::to_string(d.vendor_id)+":"+std::to_string(d.resolved);
        return value;
    }
    void save_report(system::OwnedAdapterSnapshot report){
        auto old=std::find_if(owned_reports.begin(),owned_reports.end(),[&](const auto& r){return r.owner==report.owner&&r.adapter==report.adapter;});
        if(old==owned_reports.end())owned_reports.push_back(std::move(report));else *old=std::move(report);
    }
    bool receive_reports(){
        xSemaphoreTake(lock,portMAX_DELAY);
        const bool changed=!pending_reports.empty();
        for(auto& report:pending_reports)save_report(std::move(report));
        pending_reports.clear();
        xSemaphoreGive(lock);
        return changed;
    }
    void publish(){
        if(parent)return;
        std::vector<system::OwnedAdapterSnapshot> changed;
        for(uint8_t id=0;id<amity::max_tentacles;++id)if(auto* engine=adapter(id))if(connector_configured(id)){
            auto report=adapter_report(*engine);const auto identity=report_identity(report);
            if(identity!=engine->last_report){engine->last_report=identity;changed.push_back(report);}
            xSemaphoreTake(lock,portMAX_DELAY);save_report(std::move(report));xSemaphoreGive(lock);
        }
        auto state=control_hub().snapshot();
        state.discovering=false;state.discovery_generation=0;state.tx_errors=state.rx_dropped=0;
        for(uint8_t id=0;id<amity::max_tentacles;++id)if(auto* engine=adapter(id))if(connector_configured(id)){
            state.discovering=state.discovering||engine->scan_running;state.discovery_generation+=engine->discovery_generation;
            state.tx_errors+=engine->dropped_tx+engine->front.statistics().tx_errors+engine->back.statistics().tx_errors;
            state.rx_dropped+=engine->front.statistics().rx_dropped+engine->back.statistics().rx_dropped;
        }
        if(scoped_mode){
            state.ready=std::any_of(owned_reports.begin(),owned_reports.end(),[](const auto& report){return report.ready;});
            state.devices.clear();for(const auto& selector:active_selectors)if(auto device=lookup_owned(selector.target)){device->name=selector.value;state.devices.push_back(*device);}
            if(state.current_activity>=0&&static_cast<size_t>(state.current_activity)<state.activities.size()){
                const auto& audio=state.activities[state.current_activity].audio;
                for(const auto& selector:active_selectors)if(selector.value==audio)for(const auto& report:owned_reports)if(report.owner==selector.target.port.octopus_id&&report.adapter==selector.target.port.tentacle_id){
                    activity_hub.scoped_audio_status(report.audio_status_known,report.muted,report.volume);state.audio_status_known=report.audio_status_known;state.muted=report.muted;state.volume=report.volume;
                }
            }
        }
        state.activity_settling=activity_settling(esp_timer_get_time());state.activity_error=last_activity_error;
        state.applied_config_revision=owned_config_revision;state.applied_presentation_revision=owned_presentation_revision;state.processed_activity_selection=processed_selection;
        xSemaphoreTake(lock,portMAX_DELAY);cached=state;cached_revision=owned_config_revision;
        for(uint8_t id=0;id<amity::max_tentacles;++id){auto& a=cached_adapters[id];a.tentacle_id=id;a.configured=connector_configured(id);a.present=presence[id];a.presence_generation=presence_generations[id];a.observed_front_physical.reset();a.effective_front_physical.reset();
            if(a.configured&&presence[id]!=std::optional<bool>{false})if(auto* engine=adapter(id)){const auto pa=engine->hub.snapshot().front_physical_address;if(pa!=0xffff)a.effective_front_physical=pa;}
        }
        auto observer=std::atomic_load(&observer_slot);auto cb=callback;xSemaphoreGive(lock);
        if(observer)for(const auto& report:changed)(*observer)(report);
        if(cb)cb(state);
    }
    void complete_owned(Command& command,const system::AdmissionResult& result){
        if(command.owned_deferred&&command.owned.kind==system::OwnedIntentKind::source_input){
            if(command.auth.controller_id==octopus){
                if(result.code==system::AdmissionCode::accepted||result.code==system::AdmissionCode::overloaded||result.code==system::AdmissionCode::unresolved){
                    const auto pending=std::find_if(input_setups.begin(),input_setups.end(),[&](const auto& input){return input.first==command.owned.sequence;});
                    const bool previous=setting_activity;if(pending!=input_setups.end()){setting_activity=pending->second!=0;input_setups.erase(pending);}
                    activity_hub.owned_input_result(octopus,command.owned.generation,command.owned.sequence,result.code!=system::AdmissionCode::unresolved);finish_input_selection();setting_activity=previous;
                }
            }else{auto observer=std::atomic_load(&input_observer_slot);if(observer)(*observer)(command.owned,command.auth,result);}
        }
        if(!command.owned_deferred)finish(command.reply,result);
    }
    bool owned_command(Command& command){
        using K=Kind;
        if(command.reply&&command.reply->canceled.load()&&command.kind!=K::SettingsCommit)return true;
        switch(command.kind){
        case K::ScopedConfigure:
            // Original configure cleanup uses the original selected recipe and
            // selectors before the replacement ownership map is installed.
            ++setup_generation;setup_outstanding=0;setup_pending=false;setup_owners.clear();input_setups.clear();
            xSemaphoreTake(lock,portMAX_DELAY);settling.cancel();xSemaphoreGive(lock);
            std::fill(sent_keys.begin(),sent_keys.end(),std::nullopt);scoped_mode=true;legacy_enabled=true;
            owned_controller=command.controller;owned_config_revision=command.config_revision;owned_sender=std::move(command.sender);active_selectors=command.selectors;
            owned_presentation_revision=command.presentation_revision;
            // Discovery belongs to the physical owner/adapter, independently
            // of which saved activities currently reference its equipment.
            activity_hub.scoped_transport(command.selectors,[this](auto& intent){return dispatch_owned(intent);},[this](const auto& name){return lookup_owned(name);},system::CecPortRef{consortium,octopus,0,system::Side::front});
            for(uint8_t id=0;id<amity::max_tentacles;++id)if(auto* engine=adapter(id))engine->hub.qualified_names();
            record_selection(command);
            finish(command.reply,{system::AdmissionCode::accepted,{}});return true;
        case K::OwnedInputResult:
            {const auto pending=std::find_if(input_setups.begin(),input_setups.end(),[&](const auto& input){return input.first==command.owned.sequence;});
            const bool previous=setting_activity;
            if(pending!=input_setups.end()){setting_activity=pending->second!=0;input_setups.erase(pending);}
            activity_hub.owned_input_result(command.controller,command.owned.generation,command.owned.sequence,command.owned.on);
            finish_input_selection();
            setting_activity=previous;return true;}
        case K::OwnedDiscover:{system::OwnedIntent intent;intent.kind=system::OwnedIntentKind::discover;
            intent.target.port={consortium,command.controller,static_cast<uint8_t>(command.value),system::Side::front};
            intent.generation=control_hub().scoped_generation();dispatch_owned(intent);return true;}
        case K::OwnedContext:
            finish(command.reply,activity_hub.refresh_owned_context(command.controller)?system::AdmissionResult{system::AdmissionCode::accepted,{}}:
                system::AdmissionResult{system::AdmissionCode::overloaded,"Current equipment context could not enter the owner transport"});
            return true;
        case K::OwnedBind:
            if(owned_controller!=command.controller||owned_config_revision!=command.config_revision)for(uint8_t id=0;id<amity::max_tentacles;++id)if(auto* engine=adapter(id)){engine->owned_wire_generation.fetch_add(1);engine->stop_holds();std::fill(engine->owned_keys.begin(),engine->owned_keys.end(),std::nullopt);engine->received_sequence=engine->received_generation=0;}
            owned_controller=command.controller;owned_config_revision=command.config_revision;legacy_enabled=true;
            for(uint8_t id=0;id<amity::max_tentacles;++id)if(auto* engine=adapter(id))engine->hub.qualified_names();
            finish(command.reply,{system::AdmissionCode::accepted,{}});return true;
        case K::OwnedFence:
            activity_hub.fence_owned_owner(command.controller);
            if((pending_selection&&!activity_hub.input_pending())||std::any_of(setup_owners.begin(),setup_owners.end(),[&](const auto& owner){return !command.controller||owner.owner==command.controller;})){
                if(activity_settling(esp_timer_get_time())||pending_selection)last_activity_error="The equipment owner disconnected before activity setup completed";
                settling.cancel();setup_pending=false;setup_owners.clear();finish_input_selection();
            }
            if(!command.controller||command.controller==owned_controller)for(uint8_t id=0;id<amity::max_tentacles;++id)if(auto* engine=adapter(id))if(!command.link||command.link==engine->received_link){engine->stop_holds();engine->owned_settling.cancel();std::fill(engine->owned_keys.begin(),engine->owned_keys.end(),std::nullopt);}
            finish(command.reply,{system::AdmissionCode::accepted,{}});
            return true;
        case K::AuthorityTransition:
            activity_hub.release_key();for(uint8_t id=0;id<amity::max_tentacles;++id)if(auto* engine=adapter(id))engine->stop_holds();
            local_coordinator.store(command.repeat);coordinator_epoch=command.config_revision;authority_transition.store(false);
            finish(command.reply,{system::AdmissionCode::accepted,{}});return true;
        case K::Retire:
            activity_hub.release_key();legacy_enabled=false;for(uint8_t id=0;id<amity::max_tentacles;++id)if(auto* engine=adapter(id))engine->stop_holds();passive_ports();
            finish(command.reply,{system::AdmissionCode::accepted,{}});return true;
        case K::MaintenanceBegin:
            activity_hub.release_key();for(uint8_t id=0;id<amity::max_tentacles;++id)if(auto* engine=adapter(id))engine->stop_holds();maintenance_started=true;maintenance_applied.store(wires_idle());
            finish(command.reply,{system::AdmissionCode::accepted,{}});return true;
        case K::MaintenanceEnd:maintenance_started=false;maintenance.store(false);maintenance_applied.store(false);finish(command.reply,{system::AdmissionCode::accepted,{}});return true;
        case K::SettingsCommit:
            if(pending_settings||maintenance_started||control_hub().snapshot().current_activity>=0||any_keys_active()){command.reply->settings_result=ESP_ERR_INVALID_STATE;settings_gate.store(false);finish(command.reply,{system::AdmissionCode::overloaded,"Stop the activity before changing settings"});return true;}
            pending_settings=command.reply;settings_boot=command.settings_boot;settings_presence=command.settings_presence;settings_deadline=command.settings_deadline;settings_persist=std::move(command.persist);settings_paused=true;maintenance_started=true;return true;
        default:return false;
        }
    }
    static void run(void* context){
        auto& p=*static_cast<Impl*>(context);
        if(!p.owner_started){p.owner_started=true;if(p.local_coordinator.load()&&(!p.consortium||!p.octopus))p.legacy_enabled=true;else p.passive_ports();}
        for(;;){
            p.poll_presence(uint64_t(esp_timer_get_time())/1000);
            if(p.pending_settings)p.complete_settings(uint64_t(esp_timer_get_time())/1000);
            if(p.maintenance_started)p.maintenance_applied.store(p.wires_idle());
            bool changed=p.receive_reports(),pending=false;
            if(p.legacy_enabled&&!p.maintenance_started&&!p.retiring.load()){
                p.apply_emergency_release();
                // A reply may already be waiting when its preceding TX
                // completes. Resume that original activity step first, so RX
                // observes the activity established by the completed command.
                for(uint8_t id=0;id<amity::max_tentacles;++id)if(auto* engine=p.adapter(id))
                    changed=engine->complete_transmissions()||changed;
                changed=p.resume_discovered()||changed;
                for(uint8_t id=0;id<amity::max_tentacles;++id)if(auto* engine=p.adapter(id))if(p.connector_configured(id)&&p.presence[id]!=std::optional<bool>{false}){
                    engine->hub.tick(esp_timer_get_time());const bool received=engine->receive();
                    const auto phase=engine->discovery.phase;const auto generation=engine->discovery_generation;
                    engine->discovery_tick(received);
                    changed=received||phase!=engine->discovery.phase||generation!=engine->discovery_generation||changed;
                    pending=pending||engine->transmissions_pending()||engine->discovery_pending();
                }
                if(p.resume_discovered()){changed=true;p.publish();}
            }
            Command* command=nullptr;
            p.refresh_activity_setup();
            if(xQueueReceive(p.commands,&command,pending?1:pdMS_TO_TICKS(10))==pdTRUE){
                const bool initial_configuration=command->kind==Kind::ScopedConfigure||
                    (command->kind==Kind::Configure&&p.local_coordinator.load());
                if((p.legacy_enabled||initial_configuration)&&!p.maintenance_started&&!p.retiring.load()){
                    if(initial_configuration)p.legacy_enabled=true;
                    p.accept(command);
                }
                else{p.free_reservation(*command);if(!p.owned_command(*command)&&command->kind==Kind::Configure&&p.local_coordinator.load()&&!p.maintenance_started){p.legacy_enabled=true;p.hub.configure(std::move(command->activities));}delete command;}
                changed=true;
            }
            for(uint8_t id=0;id<amity::max_tentacles;++id)if(auto* engine=p.adapter(id)){
                changed=engine->complete_transmissions()||changed;
                if(p.legacy_enabled&&!p.maintenance_started&&!p.retiring.load())engine->continue_keys();
            }
            changed=p.update_settling()||changed;
            p.refresh_activity_setup();
            if(changed)p.publish();
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
    p.front_defaults[0] = front_physical;
    p.native_ports[0] = {0, system::Side::front, &p.front, front_gpio};
    p.native_ports[1] = {0, system::Side::back, &p.back, back_gpio};
    p.native_port_count = 2;
    return p.start_owner();
}
esp_err_t HubService::start(const cec::BoardProfile& profile, uint16_t front_physical) {
    auto defaults = filled_tentacles(uint16_t{0x1000});
    defaults[0] = front_physical;
    return start(profile, defaults);
}
esp_err_t HubService::start(const cec::BoardProfile& profile, const std::array<uint16_t, amity::max_tentacles>& front_physical) {
    auto& p = *impl_;
    if (!p.lock || !p.configuration_gate) return ESP_ERR_NO_MEM;
    if (p.commands || p.native_port_count) return ESP_ERR_INVALID_STATE;
    const auto valid_gpio = [](int pin) {
#ifdef ESP_PLATFORM
        return GPIO_IS_VALID_OUTPUT_GPIO(pin);
#else
        return pin >= 0 && pin < 64;
#endif
    };
    const auto valid_input = [](int pin) {
#ifdef ESP_PLATFORM
        return GPIO_IS_VALID_GPIO(pin);
#else
        return pin >= 0 && pin < 64;
#endif
    };
    if (cec::validate_board_profile(profile, valid_gpio, nullptr, 0, valid_input) != cec::ProfileError::None) return ESP_ERR_INVALID_ARG;
    p.hardware_profile = profile; p.front_defaults = front_physical; p.fallback_physical = front_physical[0];
    for (size_t i = 0; i < 2u * profile.tentacle_count; ++i) {
        const auto& pin = profile.ports[i]; if (pin.gpio < 0) continue;
        const auto side = pin.side == cec::Side::Front ? system::Side::front : system::Side::back;
        cec::Bus* bus;
        if (pin.tentacle_id == 0) bus = side == system::Side::front ? &p.front : &p.back;
        else {
            auto& engine=p.adapters[pin.tentacle_id-1u];
            if(!engine){engine.reset(new Impl);engine->parent=&p;engine->adapter_id=pin.tentacle_id;engine->fallback_physical=front_physical[pin.tentacle_id];}
            bus=side==system::Side::front?&engine->front:&engine->back;
        }
        p.native_ports[p.native_port_count++] = {pin.tentacle_id, side, bus, pin.gpio};
    }
#ifdef ESP_PLATFORM
    for (uint8_t id = 0; id < profile.tentacle_count; ++id) if (profile.presence[id].gpio >= 0) {
        const auto& pin = profile.presence[id]; gpio_config_t input{};
        input.pin_bit_mask = uint64_t{1} << pin.gpio; input.mode = GPIO_MODE_INPUT;
        input.pull_up_en = pin.pullup ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE;
        input.pull_down_en = pin.pulldown ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE;
        const auto err = gpio_config(&input); if (err != ESP_OK) { p.reset_startup(); return err; }
    }
#endif
    p.poll_presence(uint64_t(esp_timer_get_time()) / 1000);
    return p.start_owner();
}
esp_err_t HubService::set_presence_reader(std::function<int(int)> reader) {
    if (impl_->commands || impl_->native_port_count) return ESP_ERR_INVALID_STATE;
    impl_->presence_reader = std::move(reader); return ESP_OK;
}
std::optional<bool> HubService::connector_presence(uint8_t id) const {
    if (id >= amity::max_tentacles) return {};
    auto& p = *impl_; xSemaphoreTake(p.lock, portMAX_DELAY);
    const auto present = p.presence[id]; xSemaphoreGive(p.lock); return present;
}
std::vector<AdapterRuntimeState> HubService::adapter_runtime_snapshot() const {
    auto& p = *impl_; xSemaphoreTake(p.lock, portMAX_DELAY);
    std::vector<AdapterRuntimeState> snapshot(p.cached_adapters.begin(), p.cached_adapters.end());
    xSemaphoreGive(p.lock); return snapshot;
}
esp_err_t HubService::commit_adapter_settings(system::BootId boot,
        std::vector<std::pair<uint8_t, uint64_t>> presence, uint64_t deadline,
        std::function<esp_err_t()> persist) {
    auto& p = *impl_;
    if (!boot || !persist || presence.empty() || presence.size() > amity::max_tentacles) return ESP_ERR_INVALID_ARG;
    bool used[amity::max_tentacles]{};
    for (const auto& item : presence) {
        if (item.first >= amity::max_tentacles || !item.second || used[item.first]) return ESP_ERR_INVALID_ARG;
        used[item.first] = true;
    }
    const auto now = uint64_t(esp_timer_get_time()) / 1000;
    if (deadline <= now) return ESP_ERR_TIMEOUT;
    auto reply = std::make_shared<Impl::Reply>();
    if (!reply->done) return ESP_ERR_NO_MEM;
    auto command = std::unique_ptr<Impl::Command>(new (std::nothrow) Impl::Command{Impl::Kind::SettingsCommit});
    if (!command) return ESP_ERR_NO_MEM;
    command->reply = reply; command->settings_boot = boot;
    command->settings_presence = std::move(presence); command->settings_deadline = deadline; command->persist = std::move(persist);
    if (xSemaphoreTake(p.configuration_gate, 0) != pdTRUE) return ESP_ERR_INVALID_STATE;
    const bool reserved = !p.maintenance.load() && !p.authority_transition.load() && !p.retiring.load() && !p.settings_gate.exchange(true);
    xSemaphoreGive(p.configuration_gate);
    if (!reserved) return ESP_ERR_INVALID_STATE;
    const auto admission = p.submit(command.release());
    if (admission != ESP_OK) { p.settings_gate.store(false); return admission; }
    const auto wait = static_cast<uint32_t>(std::min<uint64_t>(5000, deadline - now));
    if (xSemaphoreTake(reply->done, pdMS_TO_TICKS(wait)) != pdTRUE) {
        uint8_t waiting = 0;
        if (reply->settings_effect.compare_exchange_strong(waiting, 2)) {
            reply->canceled.store(true); return ESP_ERR_TIMEOUT;
        }
        // Once the bounded persistence callback starts, cancellation cannot
        // undo its write. Obtain its real result so durable success still gets
        // acknowledged and restarted, even if storage crosses the deadline.
        if (waiting == 1 || waiting == 3) {
            xSemaphoreTake(reply->done, portMAX_DELAY);
            return reply->settings_result;
        }
        reply->canceled.store(true); return ESP_ERR_TIMEOUT;
    }
    return reply->settings_result;
}
bool HubService::configure(std::vector<Activity> activities) {
    return configure_with_commit(std::move(activities), {}) == ESP_OK;
}
esp_err_t HubService::configure_with_commit(std::vector<Activity> activities,
                                           std::function<esp_err_t()> commit) {
    auto *c = new (std::nothrow) Impl::Command{Impl::Kind::Configure};
    if (!c) return ESP_ERR_NO_MEM;
    c->activities = std::move(activities);
    return impl_->submit(c, commit);
}
esp_err_t HubService::request_activity(int index,uint32_t* accepted) { return request_activity(index,true,accepted); }
esp_err_t HubService::repair_activity() { return request_activity(activity_target(),false,nullptr); }
uint32_t HubService::activity_selection() const { return impl_->activity_selection.load(); }
esp_err_t HubService::request_activity(int index, bool notify_selection,uint32_t* accepted_selection) {
    uint32_t receipt=0;
    auto &p = *impl_; if (!p.lock) return ESP_ERR_INVALID_STATE;
    if (!p.local_coordinator.load()) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(p.lock, portMAX_DELAY);
    if (index < -1 || index >= static_cast<int>(p.cached.activities.size())) { xSemaphoreGive(p.lock); return ESP_ERR_INVALID_ARG; }
    const bool active = p.activity_settling(esp_timer_get_time());
    const int target = active ? p.settling.target() : p.cached.current_activity;
    if (active && index != target) { xSemaphoreGive(p.lock); return ESP_ERR_TIMEOUT; }
    auto *command = new (std::nothrow) Impl::Command{index < 0 ? Impl::Kind::Standby : Impl::Kind::Select, index};
    if (command) command->route_transition = index != target;
    const auto error = p.submit(command, [&] {
        if (index != target) p.settling.reserve(index);
        if (notify_selection) command->activity_receipt=receipt=p.activity_selection.fetch_add(1)+1;
        return ESP_OK;
    });
    if(error==ESP_OK&&accepted_selection)*accepted_selection=receipt;
    xSemaphoreGive(p.lock); return error;
}
int HubService::activity_target() const {
    auto &p = *impl_; if (!p.lock) return -1;
    xSemaphoreTake(p.lock, portMAX_DELAY);
    const auto target = p.activity_settling(esp_timer_get_time()) ? p.settling.target() : p.cached.current_activity;
    xSemaphoreGive(p.lock); return target;
}
bool HubService::select_activity(int index) { return request_activity(index) == ESP_OK; }
bool HubService::activity_setup_in_progress()const{return impl_->activity_setup_in_progress();}
bool HubService::press_key(uint8_t key, bool repeat) {
    if (!impl_->local_coordinator.load()) return false;
    if(impl_->activity_setup_in_progress())return true;
    if (key == 0x40) return standby();
    auto *command = new (std::nothrow) Impl::Command{Impl::Kind::Key, key, repeat};
    if (command) {
        command->key_generation = impl_->key_generation.load();
        command->release_generation = impl_->release_generations[key].load();
    }
    return impl_->enqueue(command);
}
bool HubService::press_key_for_activity(uint8_t key, uint32_t activity_generation, bool repeat) {
    if (!impl_->local_coordinator.load()) return false;
    if(impl_->activity_setup_in_progress())return true;
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
void HubService::set_system_identity(system::ConsortiumId consortium, system::OctopusId octopus) {
    if (!impl_->task) { impl_->consortium = consortium; impl_->octopus = octopus; }
}
void HubService::set_boot_identity(system::BootId boot) { if (!impl_->task && boot) impl_->local_boot = boot; }
void HubService::set_coordinator_authority(bool local, uint64_t epoch) {
    if (!impl_->task) { impl_->local_coordinator.store(local); impl_->coordinator_epoch = epoch ? epoch : 1; }
}
esp_err_t HubService::transition_coordinator_authority(bool local, uint64_t epoch) {
    if (!epoch || impl_->maintenance.load() || impl_->authority_transition.exchange(true)) return ESP_ERR_INVALID_STATE;
    auto* command = new (std::nothrow) Impl::Command{Impl::Kind::AuthorityTransition};
    if (command) { command->repeat = local; command->config_revision = epoch; }
    const auto result = impl_->request(command, 5000);
    if (!result) { if (result.code != system::AdmissionCode::expired) impl_->authority_transition.store(false); return ESP_ERR_INVALID_STATE; }
    return ESP_OK;
}
esp_err_t HubService::retire_membership() {
    impl_->retiring.store(true);
    impl_->local_coordinator.store(false);
    const auto result = impl_->request(new (std::nothrow) Impl::Command{Impl::Kind::Retire}, 5000);
    return result ? ESP_OK : result.code == system::AdmissionCode::expired ? ESP_ERR_TIMEOUT : ESP_ERR_INVALID_STATE;
}
esp_err_t HubService::begin_maintenance() {
    if (xSemaphoreTake(impl_->configuration_gate, 0) != pdTRUE) return ESP_ERR_INVALID_STATE;
    if (impl_->settings_gate.load()) { xSemaphoreGive(impl_->configuration_gate); return ESP_ERR_INVALID_STATE; }
    impl_->maintenance.store(true);
    xSemaphoreGive(impl_->configuration_gate);
    if (impl_->maintenance_applied.load()) return ESP_OK;
    const auto result = impl_->request(new (std::nothrow) Impl::Command{Impl::Kind::MaintenanceBegin}, 5000);
    if(!result&&!impl_->maintenance_started.load())impl_->maintenance.store(false);
    return result ? ESP_OK : result.code == system::AdmissionCode::expired ? ESP_ERR_TIMEOUT : ESP_ERR_INVALID_STATE;
}
void HubService::end_maintenance() {
    impl_->request(new (std::nothrow) Impl::Command{Impl::Kind::MaintenanceEnd});
}
bool HubService::maintenance_ready() const { return impl_->maintenance_applied.load(); }

esp_err_t HubService::configure_scoped(std::vector<Activity> activities,std::vector<system::ScopedSelector> selectors,
        OwnedSender sender,system::OctopusId controller,uint64_t revision,std::function<esp_err_t()> commit,uint64_t presentation_revision){
    if(!controller||!revision)return ESP_ERR_INVALID_ARG;
    auto* c=new(std::nothrow)Impl::Command{Impl::Kind::ScopedConfigure};if(!c)return ESP_ERR_NO_MEM;
    c->activities=std::move(activities);c->selectors=std::move(selectors);c->sender=std::move(sender);c->controller=controller;c->config_revision=revision;c->presentation_revision=presentation_revision;
    return impl_->submit(c,commit);
}
system::AdmissionResult HubService::submit_owned(const system::OwnedIntent& intent,const system::OwnedAuthorization& auth,uint32_t timeout){
    auto* c=new(std::nothrow)Impl::Command{Impl::Kind::Owned};if(!c)return {system::AdmissionCode::overloaded,"Owner request allocation failed"};
    // The queued command owns the request before either caller reference expires.
    c->owned=intent;c->auth=auth;
    if(!timeout){const auto result=impl_->submit(c);return result==ESP_OK?system::AdmissionResult{system::AdmissionCode::accepted,{}}:system::AdmissionResult{system::AdmissionCode::overloaded,"Owner input IPC is full"};}
    return impl_->request(c,timeout);
}
esp_err_t HubService::bind_owned_controller(system::OctopusId controller,uint64_t revision){
    if(!controller||!revision)return ESP_ERR_INVALID_ARG;
    auto* c=new(std::nothrow)Impl::Command{Impl::Kind::OwnedBind};if(!c)return ESP_ERR_NO_MEM;c->controller=controller;c->config_revision=revision;
    const auto result=impl_->request(c,5000);return result?ESP_OK:ESP_ERR_INVALID_STATE;
}
void HubService::set_owned_observer(std::function<void(const system::OwnedAdapterSnapshot&)> observer){
    std::atomic_store(&impl_->observer_slot,observer?std::make_shared<const Impl::OwnedObserver>(std::move(observer)):std::shared_ptr<const Impl::OwnedObserver>{});
}
void HubService::set_owned_input_observer(std::function<void(const system::OwnedIntent&,const system::OwnedAuthorization&,const system::AdmissionResult&)> observer){
    std::atomic_store(&impl_->input_observer_slot,observer?std::make_shared<const Impl::OwnedInputObserver>(std::move(observer)):std::shared_ptr<const Impl::OwnedInputObserver>{});
}
void HubService::set_owned_authorizer(std::function<bool(const system::OwnedAuthorization&)> check){
    std::atomic_store(&impl_->authorizer_slot,check?std::make_shared<const Impl::OwnedAuthorizer>(std::move(check)):std::shared_ptr<const Impl::OwnedAuthorizer>{});
}
void HubService::set_owned_deadline(std::function<uint64_t(const system::OwnedAuthorization&)> deadline){
    std::atomic_store(&impl_->deadline_slot,deadline?std::make_shared<const Impl::OwnedDeadline>(std::move(deadline)):std::shared_ptr<const Impl::OwnedDeadline>{});
}
bool HubService::update_owned_snapshot(system::OwnedAdapterSnapshot report){
    if(!report.owner||report.adapter>=amity::max_tentacles)return false;
    auto& p=*impl_;xSemaphoreTake(p.lock,portMAX_DELAY);
    auto old=std::find_if(p.pending_reports.begin(),p.pending_reports.end(),[&](const auto& current){return current.owner==report.owner&&current.adapter==report.adapter;});
    const bool known=std::any_of(p.owned_reports.begin(),p.owned_reports.end(),[&](const auto& current){return current.owner==report.owner&&current.adapter==report.adapter;});
    const bool admitted=old!=p.pending_reports.end()||(p.pending_reports.size()<Impl::report_capacity&&(known||p.owned_reports.size()+p.pending_reports.size()<Impl::report_capacity));
    if(old!=p.pending_reports.end())*old=std::move(report);
    else if(admitted)p.pending_reports.push_back(std::move(report));
    xSemaphoreGive(p.lock);return admitted;
}
bool HubService::refresh_owned_context(system::OctopusId owner){
    if(!owner)return false;
    auto* c=new(std::nothrow)Impl::Command{Impl::Kind::OwnedContext};if(!c)return false;c->controller=owner;
    return bool(impl_->request(c));
}
std::vector<system::OwnedAdapterSnapshot> HubService::owned_snapshots()const{
    auto& p=*impl_;xSemaphoreTake(p.lock,portMAX_DELAY);auto reports=p.owned_reports;xSemaphoreGive(p.lock);return reports;
}
bool HubService::fence_owned(system::OctopusId controller,system::LinkId link){
    auto& p=*impl_;
    // Frame-start guards change synchronously, before the IPC cleanup runs.
    xSemaphoreTake(p.lock,portMAX_DELAY);
    if(!controller||controller==p.owned_controller)for(uint8_t id=0;id<amity::max_tentacles;++id)if(auto* engine=p.adapter(id))
        if(!link||link==engine->received_link)engine->owned_wire_generation.fetch_add(1);
    xSemaphoreGive(p.lock);
    auto* c=new(std::nothrow)Impl::Command{Impl::Kind::OwnedFence};if(!c)return false;c->controller=controller;c->link=link;
    return bool(p.request(c));
}
bool HubService::discover_owned(system::OctopusId owner,uint8_t adapter){
    if(!owner||adapter>=amity::max_tentacles)return false;
    auto* c=new(std::nothrow)Impl::Command{Impl::Kind::OwnedDiscover,adapter};if(!c)return false;c->controller=owner;return impl_->submit(c)==ESP_OK;
}
uint64_t HubService::applied_configuration_revision()const{
    auto& p=*impl_;xSemaphoreTake(p.lock,portMAX_DELAY);const auto revision=p.cached_revision;xSemaphoreGive(p.lock);return revision;
}
uint64_t HubService::executing_configuration_revision()const{return impl_->owned_config_revision.load();}
bool HubService::owned_input_result(system::OctopusId owner,uint64_t generation,uint64_t sequence,bool present){
    auto* c=new(std::nothrow)Impl::Command{Impl::Kind::OwnedInputResult};if(!c)return false;
    c->controller=owner;c->owned.generation=generation;c->owned.sequence=sequence;c->owned.on=present;return impl_->submit(c)==ESP_OK;
}

}
