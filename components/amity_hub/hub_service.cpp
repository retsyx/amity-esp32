/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#include "amity/hub_service.hpp"
#include "amity/activity_settling.hpp"
#include "amity/live_system.hpp"
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
    enum class Kind { Configure, Select, Key, Release, Standby, Discover, ConfigureV2, SelectV2, StopV2, Authorized,
        RemotePrepare, RemoteFence, RemoteQuiescent, RemoteActivate, RemoteAbort, MemberPrepare, MemberActivate, MemberAbort,
        RemoteHooks, RemoteOwner, RemotePlanResult, RemoteOperationResult, InputIntent, RemoteRenew, DiscoveryLease, AuthorityTransition, Retire, MaintenanceBegin, MaintenanceEnd, SettingsCommit };
    struct Reply {
        SemaphoreHandle_t done = xSemaphoreCreateCounting(1, 0);
        std::atomic<bool> canceled{false};
        std::atomic<uint8_t> settings_effect{0}; // waiting, started, canceled, complete
        esp_err_t settings_result = ESP_FAIL;
        system::AdmissionResult result{system::AdmissionCode::overloaded, "owner response unavailable"};
        ~Reply() { if (done) vSemaphoreDelete(done); }
    };
    struct Command {
        explicit Command(Kind command_kind, int command_value = 0, bool command_repeat = false)
            : kind(command_kind), value(command_value), repeat(command_repeat) {}
        Kind kind;
        int value = 0;
        bool repeat = false;
        uint32_t key_generation = 0, hold_generation = 0;
        bool activity_bound = false;
        bool presentation_only = false;
        bool route_transition = false;
        uint32_t activity_generation = 0;
        uint32_t wire_generation = 0;
        uint32_t intent_generation = 0;
        std::vector<Activity> activities;
        std::vector<Quirk> quirks;
        std::shared_ptr<system::LocalConfiguration> system_config;
        system::ZoneId zone;
        system::ActivityId activity;
        system::MessageId request;
        std::optional<uint64_t> expected_state;
        system::AuthorizedCecOperation authorized;
        system::Plan plan;
        std::vector<system::PortPolicySnapshot> policies;
        system::SessionId session;
        uint64_t route_generation = 0;
        uint32_t authority_ttl = 0;
        std::shared_ptr<Reply> reply;
        std::function<esp_err_t()> persist;
        std::string payload_hash;
        system::RemoteExecutionHooks remote_hooks;
        system::OctopusId remote_owner;
        system::RemotePlanPhase remote_phase = system::RemotePlanPhase::prepare;
        system::CommandId command_id;
        system::StepResult step_result = system::StepResult::unknown;
        std::string error;
        system::InputIntent input;
        system::BootId settings_boot;
        std::vector<std::pair<uint8_t, uint64_t>> settings_presence;
        uint64_t settings_deadline = 0;
    };
    // Declared before buses: both counters outlive every admitted wire work
    // item. Impl and these 32-bit counters reside in internal RAM on target.
    std::atomic<uint32_t> wire_generation{0}, key_generation{0}, intent_generation{0};
    std::atomic<uint32_t> binding_wire_generation[2][15]{};
    uint32_t release_binding_generation[2][15]{};
    cec::Bus front, back;
    struct NativePort { uint8_t tentacle; system::Side side; cec::Bus* bus; int gpio; };
    std::array<std::unique_ptr<cec::Bus>, 8> extra_buses;
    std::array<NativePort, 10> native_ports{};
    size_t native_port_count = 0;
    std::optional<cec::BoardProfile> hardware_profile;
    std::array<std::optional<bool>, 5> presence{};
    std::array<uint64_t, 5> presence_generations{{1, 1, 1, 1, 1}};
    std::array<AdapterRuntimeState, 5> cached_adapters{};
    std::function<int(int)> presence_reader;
    std::array<uint16_t, 5> front_defaults{{0x1000, 0x1000, 0x1000, 0x1000, 0x1000}};
    std::unique_ptr<LiveSystem> live;
    std::atomic<bool> v2_mode{false};
    system::ConsortiumId consortium;
    system::OctopusId octopus;
    system::BootId local_boot;
    std::atomic<bool> local_coordinator{true};
    uint64_t coordinator_epoch = 1;
    system::ExecutionSnapshot cached_execution;
    std::vector<system::PortPolicySnapshot> cached_policies;
    // Complete currently-down metadata only, never outgoing work or history.
    std::vector<system::AuthorizedCecOperation> cached_active_keys;
    bool key_snapshot_valid = false;
    std::shared_ptr<system::LocalConfiguration> pending_v2;
    std::shared_ptr<Reply> pending_v2_reply;
    std::function<esp_err_t()> pending_v2_persist;
    bool pending_v2_presentation = false;
    std::shared_ptr<system::LocalConfiguration> prepared_member;
    std::string prepared_member_hash;
    std::function<esp_err_t()> prepared_member_persist;
    system::RemoteExecutionHooks remote_hooks;
    system::InputContext cached_input_context;
    std::shared_ptr<Reply> pending_retirement, pending_authority;
    bool pending_local_authority = false;
    uint64_t pending_authority_epoch = 1;
    std::atomic<bool> authority_transition{false};
    std::shared_ptr<Reply> pending_maintenance;
    std::atomic<bool> maintenance{false}, maintenance_applied{false};
    std::atomic<bool> settings_gate{false}, retiring{false};
    std::shared_ptr<Reply> pending_settings;
    system::BootId settings_boot;
    std::vector<std::pair<uint8_t, uint64_t>> settings_presence;
    uint64_t settings_deadline = 0;
    std::function<esp_err_t()> settings_persist;
    bool settings_paused = false;
    bool maintenance_started = false;
    bool owner_started = false, legacy_enabled = false;
    std::string submitted_v2_hash;
    system::OctopusId submitted_member_coordinator;
    uint64_t submitted_member_epoch = 0;
    Hub hub{*this};
    QueueHandle_t commands = nullptr;
    SemaphoreHandle_t lock = nullptr;
    SemaphoreHandle_t command_slots = nullptr, configuration_gate = nullptr;
    TaskHandle_t task = nullptr;
    Snapshot cached;
    // Updated only under configuration_gate, in durable commit/enqueue order.
    std::vector<Activity> submitted_activities;
    std::vector<Quirk> submitted_quirks;
    std::function<void(const Snapshot &)> callback;
    uint8_t front_address = 15, back_address = 15;
    uint16_t fallback_physical = 0x1000;
    bool scan_requested = false;
    bool scan_running = false;
    int64_t last_scan = -120000000;
    int64_t last_claim = -5000000;
    uint32_t dropped_tx = 0, discovery_generation = 0;
    uint32_t applied_key_generation = 0;
    std::atomic<uint32_t> hold_generation{0};
    uint32_t repeating_generation = 0;
    int repeating_key = -1;
    int failed_key = -1;
    cec::TxResult held_tx_result = cec::TxResult::Pending;
    uint32_t active_wire_generation = 0, active_key_generation = 0;
    std::string last_activity_error;
    bool sending_held_key = false, sending_one_key = false, setting_activity = false;
    ActivitySettling settling{CONFIG_AMITY_ACTIVITY_SETTLE_SECONDS};

    Impl() { lock = xSemaphoreCreateMutex(); configuration_gate = xSemaphoreCreateMutex(); }
    bool wires_idle() const {
        for (size_t i = 0; i < native_port_count; ++i) if (!native_ports[i].bus->idle()) return false;
        return true;
    }
    void passive_ports() {
        for (size_t i = 0; i < native_port_count; ++i) native_ports[i].bus->install_receive_policy({true, 15});
    }
    bool connector_configured(uint8_t id) const {
        if (id >= 5) return false;
        if (hardware_profile) return cec::connector_enabled(*hardware_profile, id);
        return id == 0 && native_port_count == 2;
    }
    void poll_presence(uint64_t now) {
        if (!hardware_profile) return;
        bool any_changed = false;
        for (uint8_t id = 0; id < 5; ++id) {
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
            if (changed && live && (present || pin.gpio >= 0)) live->set_tentacle_present(id, present.value_or(false), now);
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
        commands = xQueueCreate(32, sizeof(Impl::Command *));
        command_slots = xSemaphoreCreateCounting(32, 32);
        if (!commands || !command_slots || xTaskCreate(Impl::run, "amity_hub", 6144, this, 5, &task) != pdPASS) {
            reset_startup(); return ESP_ERR_NO_MEM;
        }
        return ESP_OK;
    }
    void reset_startup() {
        for (size_t i = 0; i < native_port_count; ++i) native_ports[i].bus->stop();
        if (commands) { vQueueDelete(commands); commands = nullptr; }
        if (command_slots) { vSemaphoreDelete(command_slots); command_slots = nullptr; }
        for (auto& bus : extra_buses) bus.reset();
        native_port_count = 0; hardware_profile.reset(); presence.fill({});
        presence_generations.fill(1);
    }
    ~Impl() {
        if (task) vTaskDelete(task);
        // Engines can still read LiveSystem's internal generation counters.
        // Detach every wire before the semantic owner/guards are destroyed.
        for (size_t i = 0; i < native_port_count; ++i) native_ports[i].bus->stop();
        if (commands) {
            Command *command;
            while (xQueueReceive(commands, &command, 0) == pdTRUE) delete command;
            vQueueDelete(commands);
        }
        if (lock) vSemaphoreDelete(lock);
        if (command_slots) vSemaphoreDelete(command_slots);
        if (configuration_gate) vSemaphoreDelete(configuration_gate);
    }
    static bool cleanup(const Command& command) {
        return command.kind == Kind::Release || command.kind == Kind::RemoteQuiescent || command.kind == Kind::RemoteFence || command.kind == Kind::RemoteAbort ||
            (command.kind == Kind::Authorized && command.authorized.kind == system::OperationKind::release_key) ||
            (command.kind == Kind::InputIntent && command.input.action == system::InputAction::hold_release);
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
        const bool configuration = command->kind == Kind::Configure || command->kind == Kind::ConfigureV2;
        // Concurrent configuration callers must commit and enqueue in the
        // same order. Contention is rejected without blocking or persistence.
        if (configuration && xSemaphoreTake(configuration_gate, 0) != pdTRUE) return ESP_ERR_TIMEOUT;
        const auto unlock = [&] { if (configuration) xSemaphoreGive(configuration_gate); };
        if (xSemaphoreTake(command_slots, 0) != pdTRUE) { unlock(); return ESP_ERR_NO_MEM; }
        if (command->kind == Kind::ConfigureV2) command->presentation_only = command->system_config &&
            !submitted_v2_hash.empty() && command->system_config->executable_hash == submitted_v2_hash &&
            command->system_config->coordinator_id == submitted_member_coordinator && command->system_config->coordinator_epoch == submitted_member_epoch;
        else if (configuration) command->presentation_only = same_executable_configuration(
            submitted_activities, submitted_quirks, command->activities, command->quirks);
        // Allocate the retained comparison copy before persistence. No
        // allocation or fallible work may follow a successful commit.
        auto next_activities = configuration ? command->activities : std::vector<Activity>{};
        auto next_quirks = configuration ? command->quirks : std::vector<Quirk>{};
        const auto error = commit ? commit() : ESP_OK;
        if (error != ESP_OK) { xSemaphoreGive(command_slots); unlock(); return error; }
        // All producers reserve a slot, so queued + reserved <= capacity.
        // The worker frees reservations immediately on dequeue. Queue send
        // therefore cannot fail after a successful external commit.
        // Cancel continuation immediately, even if the owner is waiting for
        // a wire attempt or finishing discovery. A short tap still keeps its
        // initial queued press and subsequent release.
        if ((configuration && !command->presentation_only) || command->kind == Kind::Select || command->kind == Kind::Standby) hold_generation.fetch_add(1);
        if ((configuration && !command->presentation_only) || command->route_transition) {
            // An activity is still a semantic request until the owner compiles
            // it against the latest binding. New intents supersede it; binding
            // changes fence old wire work without discarding explicit intent.
            command->intent_generation = intent_generation.fetch_add(1) + 1;
            command->wire_generation = wire_generation.fetch_add(1) + 1;
        } else {
            command->intent_generation = intent_generation.load();
            command->wire_generation = wire_generation.load();
        }
        if (configuration) {
            submitted_activities = std::move(next_activities);
            submitted_quirks = std::move(next_quirks);
            if (command->system_config) {
                submitted_v2_hash = command->system_config->executable_hash;
                submitted_member_coordinator = command->system_config->coordinator_id; submitted_member_epoch = command->system_config->coordinator_epoch;
            }
        }
        command = owned.release();
        if (xQueueSend(commands, &command, 0) != pdTRUE) std::abort();
        unlock();
        return ESP_OK;
    }
    static void finish(const std::shared_ptr<Reply>& reply, system::AdmissionResult result) {
        if (!reply || reply->canceled.load()) return;
        reply->result = std::move(result); xSemaphoreGive(reply->done);
    }
    static bool same_key(const system::AuthorizedCecOperation& a, const system::AuthorizedCecOperation& b) {
        return a.command_id == b.command_id && a.component_id == b.component_id && a.binding_id == b.binding_id && a.target == b.target &&
            a.fence == b.fence && a.arguments.hold_token == b.arguments.hold_token && a.arguments.key == b.arguments.key;
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
    bool remote_command(Command& command, uint64_t now) {
        if (command.kind == Kind::SettingsCommit && command.reply && command.reply->canceled.load()) { settings_gate.store(false); return true; }
        if (command.reply && command.reply->canceled.load() && command.kind != Kind::Retire && command.kind != Kind::MaintenanceBegin && command.kind != Kind::AuthorityTransition) return true;
        if (maintenance_started && command.kind != Kind::SettingsCommit && command.kind != Kind::MaintenanceBegin && command.kind != Kind::MaintenanceEnd && command.kind != Kind::Retire && !cleanup(command)) {
            finish(command.reply, {system::AdmissionCode::unauthorized, "firmware maintenance closed execution admission"}); return true;
        }
        system::AdmissionResult result{system::AdmissionCode::unauthorized, "semantic runtime is unavailable"};
        switch (command.kind) {
        case Kind::SettingsCommit: {
            const auto state = live ? live->execution_snapshot() : system::ExecutionSnapshot{};
            bool grants = false;
            if (live) for (const auto& policy : live->local_policy_snapshot())
                grants = grants || (policy.authority_expires_at_ms > now && !policy.route_grants.empty());
            if (maintenance_started || maintenance.load() || retiring.load() || authority_transition.load() ||
                pending_v2 || prepared_member || !state.sessions.empty() || state.executing_plan || grants ||
                (live && !live->quiescent()) || (!live && (hub.snapshot().current_activity >= 0 || repeating_key >= 0))) {
                command.reply->settings_result = ESP_ERR_INVALID_STATE;
                settings_gate.store(false);
                finish(command.reply, {system::AdmissionCode::overloaded, "Amity is busy; stop its activity before changing settings"});
                return true;
            }
            pending_settings = command.reply; settings_boot = command.settings_boot;
            settings_presence = std::move(command.settings_presence); settings_deadline = command.settings_deadline;
            settings_persist = std::move(command.persist); settings_paused = true; maintenance_started = true;
            wire_generation.fetch_add(1); key_generation.fetch_add(1); hold_generation.fetch_add(1);
            if (live) live->maintenance(true); else stop_hold(true);
            scan_requested = false;
            return true;
        }
        case Kind::AuthorityTransition:
            local_coordinator.store(false); wire_generation.fetch_add(1); key_generation.fetch_add(1); hold_generation.fetch_add(1);
            if (live) live->maintenance(true); else stop_hold(true);
            finish(pending_v2_reply, {system::AdmissionCode::unauthorized, "authority changed before configuration activation"});
            pending_v2.reset(); pending_v2_reply.reset(); pending_v2_persist = {};
            prepared_member.reset(); prepared_member_hash.clear(); prepared_member_persist = {};
            finish(pending_authority, {system::AdmissionCode::stale, "authority transition superseded"});
            pending_authority = command.reply; pending_local_authority = command.repeat; pending_authority_epoch = command.route_generation;
            return true;
        case Kind::MaintenanceBegin:
            maintenance_started = true; maintenance_applied.store(false); wire_generation.fetch_add(1); key_generation.fetch_add(1); hold_generation.fetch_add(1);
            if (live) live->maintenance(true); else stop_hold(true);
            finish(pending_maintenance, {system::AdmissionCode::stale, "maintenance retry superseded response"}); pending_maintenance = command.reply;
            return true;
        case Kind::MaintenanceEnd:
            if (live) live->maintenance(false);
            maintenance_started = false; maintenance_applied.store(false); maintenance.store(false);
            result = {system::AdmissionCode::accepted, {}}; break;
        case Kind::Retire:
            local_coordinator.store(false); wire_generation.fetch_add(1); key_generation.fetch_add(1); hold_generation.fetch_add(1);
            if (live) live->maintenance(true); else stop_hold(true);
            finish(pending_retirement, {system::AdmissionCode::stale, "retirement retry superseded response"}); pending_retirement = command.reply;
            return true;
        case Kind::InputIntent:
            if (live && local_coordinator.load()) {
                if (command.input.action != system::InputAction::hold_release &&
                    (command.input.input_session_id != cached_input_context.input_session_id || command.input.expected_state_revision != cached_input_context.state_revision))
                    result = {system::AdmissionCode::stale, "input session changed before owner admission"};
                else result = live->input(command.input, now);
            }
            break;
        case Kind::RemoteHooks:
            remote_hooks = std::move(command.remote_hooks); if (live) live->remote_execution_hooks(remote_hooks);
            result = {system::AdmissionCode::accepted, {}}; break;
        case Kind::RemoteOwner:
            if (live && local_coordinator.load()) result = live->update_remote_owner(command.remote_owner, std::move(command.policies), command.repeat, command.authority_ttl, now);
            break;
        case Kind::RemotePlanResult:
            if (live && local_coordinator.load()) result = {live->remote_plan_result(command.remote_owner, command.plan.plan_id, command.remote_phase, command.repeat, command.error, now) ? system::AdmissionCode::accepted : system::AdmissionCode::stale, "plan correlation is stale"};
            break;
        case Kind::RemoteOperationResult:
            if (live && local_coordinator.load()) result = {live->remote_operation_result(command.remote_owner, command.command_id, command.step_result, command.error, now) ? system::AdmissionCode::accepted : system::AdmissionCode::stale, "command correlation is stale"};
            break;
        case Kind::MemberPrepare:
            if (local_coordinator.load() || !command.system_config || !command.system_config->member ||
                !command.system_config->coordinator_id || command.system_config->coordinator_epoch < coordinator_epoch ||
                command.payload_hash.size() != 64 ||
                command.system_config->port_templates.size() > 10 || command.system_config->topology.bindings.size() > 128)
                result = {system::AdmissionCode::invalid, "invalid member configuration, authority, or local HDMI adapter capacity"};
            else if (prepared_member && prepared_member_hash != command.payload_hash) result = {system::AdmissionCode::overloaded, "another member configuration is prepared"};
            else {
                prepared_member = std::move(command.system_config); prepared_member_hash = command.payload_hash; prepared_member_persist = std::move(command.persist);
                result = {system::AdmissionCode::accepted, {}};
            }
            break;
        case Kind::MemberActivate:
            if (!prepared_member || prepared_member_hash != command.payload_hash) result = {system::AdmissionCode::stale, "unknown prepared member configuration"};
            else {
                command.system_config = std::move(prepared_member); command.persist = std::move(prepared_member_persist);
                prepared_member_hash.clear(); queue_v2(command); return true;
            }
            break;
        case Kind::MemberAbort:
            if (prepared_member_hash == command.payload_hash) { prepared_member.reset(); prepared_member_hash.clear(); prepared_member_persist = {}; }
            result = {system::AdmissionCode::accepted, {}}; break;
        case Kind::Authorized: if (live && live->configured()) result = live->execute_authorized(command.authorized, now); break;
        case Kind::RemotePrepare: if (live) result = live->prepare_remote_plan(command.plan, command.policies, now); break;
        case Kind::RemoteFence: if (live) result = live->fence_remote_route(command.session, command.route_generation); break;
        case Kind::RemoteQuiescent: if (live) result = {live->remote_route_quiescent(command.session, command.route_generation) ? system::AdmissionCode::accepted : system::AdmissionCode::stale, "old route drain status"}; break;
        case Kind::RemoteActivate: if (live) result = live->activate_remote_plan(command.plan.plan_id, command.authority_ttl, now); break;
        case Kind::DiscoveryLease: if (live) result = live->discover_authorized(command.authority_ttl, now); break;
        case Kind::RemoteRenew: if (live) result = live->renew_remote_authority(command.plan.plan_id, command.session, command.route_generation, command.authority_ttl, now); break;
        case Kind::RemoteAbort: if (live) { live->abort_remote_plan(command.plan.plan_id); result = {system::AdmissionCode::accepted, {}}; } break;
        default: return false;
        }
        publish(); finish(command.reply, std::move(result)); return true;
    }
    void queue_v2(Command& command) {
        if (maintenance_started) { finish(command.reply, {system::AdmissionCode::unauthorized, "configuration installation is closed for firmware maintenance"}); return; }
        if (command.reply && command.reply->canceled.load()) return;
        if (command.system_config && command.system_config->member && (local_coordinator.load() ||
            !command.system_config->coordinator_id || !command.system_config->coordinator_epoch ||
            command.system_config->coordinator_epoch < coordinator_epoch)) {
            finish(command.reply, {system::AdmissionCode::unauthorized, "member slice has no current coordinator authority"}); return;
        }
        if (command.kind == Kind::MemberActivate && command.system_config) {
            command.presentation_only = !submitted_v2_hash.empty() && command.system_config->executable_hash == submitted_v2_hash &&
                command.system_config->coordinator_id == submitted_member_coordinator && command.system_config->coordinator_epoch == submitted_member_epoch;
            submitted_v2_hash = command.system_config->executable_hash; submitted_member_coordinator = command.system_config->coordinator_id;
            submitted_member_epoch = command.system_config->coordinator_epoch;
        }
        command.presentation_only = command.system_config && live && live->preserves_configuration(*command.system_config);
        if (live && !command.presentation_only) live->maintenance(true);
        finish(pending_v2_reply, {system::AdmissionCode::stale, "configuration superseded"});
        pending_v2 = std::move(command.system_config); pending_v2_reply = command.reply; pending_v2_persist = std::move(command.persist); pending_v2_presentation = command.presentation_only;
    }
    void complete_authority_transition() {
        if (!pending_authority || !wires_idle() || (live && !live->quiescent())) return;
        if (live) {
            xSemaphoreTake(lock, portMAX_DELAY); cached_execution = live->execution_snapshot(); xSemaphoreGive(lock);
            live->fault("local authority transition is passive"); live.reset();
        }
        xSemaphoreTake(lock, portMAX_DELAY); cached_policies.clear(); cached_active_keys.clear(); key_snapshot_valid = false; cached_input_context = {}; xSemaphoreGive(lock);
        submitted_v2_hash.clear();
        v2_mode.store(false); legacy_enabled = false; scan_requested = false;
        passive_ports(); front_address = back_address = 15;
        coordinator_epoch = pending_authority_epoch; local_coordinator.store(pending_local_authority);
        last_activity_error = "Fresh configuration installation is required after authority transition";
        authority_transition.store(false); publish();
        finish(pending_authority, {system::AdmissionCode::accepted, {}}); pending_authority.reset();
    }
    void complete_retirement() {
        if (!pending_retirement || !wires_idle() || (live && !live->quiescent())) return;
        if (live) live->fault("consortium membership retired; local execution is passive");
        passive_ports(); front_address = back_address = 15;
        finish(pending_v2_reply, {system::AdmissionCode::unauthorized, "membership retired before configuration activation"});
        pending_v2.reset(); pending_v2_reply.reset(); pending_v2_persist = {}; prepared_member.reset(); prepared_member_hash.clear(); prepared_member_persist = {};
        last_activity_error = "consortium membership retired; local execution is passive";
        publish(); finish(pending_retirement, {system::AdmissionCode::accepted, {}}); pending_retirement.reset();
    }
    void complete_maintenance() {
        if (!maintenance_started || !pending_maintenance || !wires_idle() || (live && !live->quiescent())) return;
        maintenance_applied.store(true); publish(); finish(pending_maintenance, {system::AdmissionCode::accepted, {}}); pending_maintenance.reset();
    }
    void complete_settings(uint64_t now) {
        if (!pending_settings) return;
        auto fail = [&](esp_err_t error, const char* reason) {
            pending_settings->settings_result = error;
            finish(pending_settings, {error == ESP_ERR_TIMEOUT ? system::AdmissionCode::expired : system::AdmissionCode::stale, reason});
            pending_settings.reset(); settings_persist = {}; settings_presence.clear();
            if (settings_paused && !retiring.load() && !authority_transition.load() && !maintenance.load()) {
                if (live) live->maintenance(false);
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
        if (!wires_idle() || (live && !live->quiescent())) return;
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
        wire_generation.fetch_add(1);
        hold_generation.fetch_add(1);
        applied_key_generation = key_generation.fetch_add(1) + 1;
        repeating_key = -1;
    }
    void apply_emergency_release() {
        const auto generation = key_generation.load();
        if (generation == applied_key_generation) return;
        stop_hold();
        applied_key_generation = generation;
    }
    void stop_hold(bool flush = false) {
        repeating_key = -1; failed_key = -1; hub.end_key_stream();
        if (flush) hub.release_key(); // Explicit quiescence must drain cleanup before its ACK.
    }
    void check_hold() {
        if (repeating_key >= 0 && repeating_generation != hold_generation.load()) stop_hold();
    }
    void send_held_key() {
        held_tx_result = cec::TxResult::Pending;
        sending_held_key = true;
        const bool accepted = hub.press_key(static_cast<uint8_t>(repeating_key), true);
        sending_held_key = false;
        if (!accepted || held_tx_result != cec::TxResult::Success) {
            // Failure ends the button state. Its output stream retains safe
            // final cleanup independently; a later physical up is a no-op.
            failed_key = repeating_key; repeating_key = -1;
            hub.end_key_stream();
        }
    }
    bool update_settling() {
        xSemaphoreTake(lock, portMAX_DELAY);
        const bool changed = cached.activity_settling != settling.active(esp_timer_get_time());
        xSemaphoreGive(lock); return changed;
    }
    void fail_activity(int index, const char *reason) {
        last_activity_error = reason;
        xSemaphoreTake(lock, portMAX_DELAY);
        if (settling.target() == index) settling.cancel();
        xSemaphoreGive(lock);
    }
    bool compile_activity_intent(const Command &command) {
        if (command.intent_generation != intent_generation.load()) return false;
        const auto fresh_wire_generation = wire_generation.load();
        // Intent invalidation precedes wire invalidation at admission. The
        // second check prevents an old intent from adopting a newer wire fence.
        if (command.intent_generation != intent_generation.load()) return false;
        active_wire_generation = fresh_wire_generation;
        return true;
    }
    void apply_activity(int index) {
        const auto before = hub.snapshot();
        // A configuration queued before this request may have removed its index.
        if (index < -1 || index >= static_cast<int>(before.activities.size())) {
            fail_activity(index, "Activity configuration changed; choose the activity again.");
            return;
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
        const bool applied = index < 0 ? (hub.standby(), true) : hub.select_activity(index);
        setting_activity = false;
        if (!applied) {
            fail_activity(index, "Required equipment is missing or ambiguous; run discovery and retry.");
            return;
        }
        last_activity_error.clear();
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
        // Bound ordinary admission waits. Final releases use only their
        // explicitly allowed ninth slot and are dropped if it is occupied.
        auto &bus = side == BusSide::Front ? front : back;
        // Held presses join the same FIFO as routing, releases and replies.
        // Only one attempt is outstanding; the next is decided after completion.
        // Discovery and identity service do not require a route. Unknown and
        // vendor recipes default to guarded work, including repair recipes.
        const bool passive = data[0] == 0x00 || data[0] == 0x46 || data[0] == 0x47 || data[0] == 0x71 ||
            data[0] == 0x83 || data[0] == 0x84 || data[0] == 0x87 || data[0] == 0x8c || data[0] == 0x8f ||
            data[0] == 0x90 || data[0] == 0x9e || data[0] == 0x9f || data[0] == 0xa5 || data[0] == 0xa6;
        cec::ExecutionFence fence{&wire_generation, active_wire_generation};
        const unsigned port = side == BusSide::Front ? 0 : 1;
        // Matching cleanup survives a route/config/key change. It must still
        // die if the old logical address is replaced by another appliance.
        if (data[0] == 0x45 && destination < 15)
            fence = {&binding_wire_generation[port][destination], release_binding_generation[port][destination]};
        if (!setting_activity && (sending_held_key || sending_one_key) && data[0] == 0x44) {
            fence.secondary_generation = &key_generation;
            fence.expected_secondary_generation = active_key_generation;
        }
        const unsigned attempts = data[0] == 0x44 ? 1 : 3;
        const bool allow_overflow = data[0] == 0x45;
        const bool sent = passive ? bus.send(frame, 1000) :
            setting_activity ? bus.send_and_wait_guarded(frame, fence, allow_overflow ? 0 : 1000, attempts, nullptr, allow_overflow) :
            sending_held_key && data[0] == 0x44 ? bus.send_and_wait_guarded(frame, fence, 1000, attempts, &held_tx_result) : bus.send_guarded(frame, fence, allow_overflow ? 0 : 1000, attempts, allow_overflow);
        if (sent && data[0] == 0x44 && destination < 15)
            release_binding_generation[port][destination] = binding_wire_generation[port][destination].load();
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
        if (live && live->configured()) state = live->snapshot(state);
        else if (v2_mode.load()) { state.current_activity = -1; state.ready = false; }
        if (!live || !live->configured()) { state.discovering = scan_running; state.discovery_generation = discovery_generation; }
        if (!live || !live->configured()) state.activity_error = last_activity_error;
        state.tx_errors = dropped_tx; state.rx_dropped = 0;
        for (size_t i = 0; i < native_port_count; ++i) {
            const auto stats = native_ports[i].bus->statistics();
            state.tx_errors += stats.tx_errors; state.rx_dropped += stats.rx_dropped;
        }
        xSemaphoreTake(lock, portMAX_DELAY);
        for (uint8_t id = 0; id < 5; ++id) {
            auto& adapter = cached_adapters[id]; adapter.tentacle_id = id;
            adapter.configured = connector_configured(id); adapter.present = presence[id];
            adapter.presence_generation = presence_generations[id];
            adapter.observed_front_physical.reset(); adapter.effective_front_physical.reset();
            const bool unknown_detector = !presence[id] && hardware_profile && hardware_profile->presence[id].gpio >= 0;
            if (adapter.configured && !unknown_detector && presence[id] != std::optional<bool>{false}) {
                if (live) {
                    const auto addresses = live->front_physical_addresses(id);
                    adapter.observed_front_physical = addresses.first; adapter.effective_front_physical = addresses.second;
                } else {
                    if (id == 0 && state.front_physical_address != 0xffff) adapter.effective_front_physical = state.front_physical_address;
                    else adapter.effective_front_physical = front_defaults[id];
                }
            }
        }
        state.activity_settling = settling.active(esp_timer_get_time());
        cached = state;
        key_snapshot_valid = live && live->configured();
        if (key_snapshot_valid) {
            cached_execution = live->execution_snapshot(); cached_policies = live->local_policy_snapshot();
            cached_active_keys = live->active_key_contexts();
        } else cached_active_keys.clear();
        if (local_coordinator.load() && (!cached_input_context.input_session_id || cached_input_context.state_revision != cached_execution.state_revision)) {
            cached_input_context = {system::InputSessionId::generate(secure_random).value_or(system::InputSessionId{}), cached_execution.state_revision};
        }
        auto cb = callback;
        xSemaphoreGive(lock);
        if (cb) cb(state);
    }
    static bool secure_random(uint8_t* bytes, size_t size) {
#ifdef ESP_PLATFORM
        esp_fill_random(bytes, size);
#else
        std::random_device rng; for (size_t i = 0; i < size; ++i) bytes[i] = static_cast<uint8_t>(rng());
#endif
        return true;
    }
    void install_v2(uint64_t now) {
        if (maintenance_started) return;
        if (pending_v2_reply && pending_v2_reply->canceled.load()) { pending_v2.reset(); pending_v2_reply.reset(); pending_v2_persist = {}; return; }
        if (!pending_v2 || (!pending_v2_presentation && (!wires_idle() || (live && !live->quiescent())))) return;
        if (pending_v2_presentation && live && !live->revision_rebase_ready(*pending_v2)) return;
        const bool semantic_rebase = pending_v2_presentation && live &&
            pending_v2->topology.config_revision != live->execution_snapshot().applied_config_revision;
        // Invalidating frame-start guards precedes the durable revision switch.
        // Exact matching cleanup uses independent binding guards and survives.
        if (semantic_rebase) live->fence_revision_work();
        if (pending_v2_persist) {
            auto persist = std::move(pending_v2_persist);
            if (persist() != ESP_OK) {
                last_activity_error = "configuration persistence failed; execution remains quiescent";
                if (live && (!pending_v2_presentation || semantic_rebase)) live->fault(last_activity_error);
                finish(pending_v2_reply, {system::AdmissionCode::invalid, last_activity_error});
                pending_v2.reset(); pending_v2_reply.reset(); publish(); return;
            }
        }
        if (!live) {
            if (!local_boot) local_boot = system::BootId::generate(secure_random).value_or(system::BootId{});
            live = std::make_unique<LiveSystem>(consortium, octopus, local_boot, secure_random, coordinator_epoch);
            live->remote_execution_hooks(remote_hooks);
            for (size_t i = 0; i < native_port_count; ++i) {
                const auto& p = native_ports[i];
                const auto identity = p.tentacle == 0 ? (p.side == system::Side::front ? front_address : back_address) : uint8_t{15};
                const auto physical = p.side == system::Side::back ? uint16_t{0} : front_defaults[p.tentacle];
                live->attach({consortium, octopus, p.tentacle, p.side}, *p.bus, identity, physical);
                if (presence[p.tentacle]) live->set_tentacle_present(p.tentacle, *presence[p.tentacle], now);
                else if (hardware_profile && hardware_profile->presence[p.tentacle].gpio >= 0) live->set_tentacle_present(p.tentacle, false, now);
            }
        }
        v2_mode.store(true);
        if (!live->configure(std::move(*pending_v2), now, last_activity_error)) {
            live->fault(last_activity_error);
            ESP_LOGW(TAG, "System configuration unavailable: %s", last_activity_error.c_str());
            xSemaphoreTake(lock, portMAX_DELAY); cached_execution.error = last_activity_error; xSemaphoreGive(lock);
        }
        if (live->configured()) { live->maintenance(false); v2_mode.store(true); xSemaphoreTake(lock, portMAX_DELAY); settling.cancel(); xSemaphoreGive(lock); }
        pending_v2.reset(); scan_requested = false; publish();
        finish(pending_v2_reply, {live->configured() ? system::AdmissionCode::accepted : system::AdmissionCode::invalid, last_activity_error}); pending_v2_reply.reset();
    }
    void run_v2_iteration() {
        const auto now = uint64_t(esp_timer_get_time()) / 1000;
        poll_presence(now);
        if (key_generation.load() != applied_key_generation) { live->release(now); applied_key_generation = key_generation.load(); }
        Command* command = nullptr;
        // Completed local holds repeat without a cadence timer. While their
        // asynchronous frame is in flight, yield for one scheduler tick; the
        // Bus has no owner-completion notification. Remote-owned holds use the
        // normal idle wait because their emitting owner performs repetition.
        const TickType_t wait = live->local_repeat_ready(now) ? 0 : live->local_repeat_in_flight() ? 1 : pdMS_TO_TICKS(10);
        const bool received_command = xQueueReceive(commands, &command, wait) == pdTRUE;
        if (received_command) {
            xSemaphoreGive(command_slots);
            if (!remote_command(*command, now)) switch (command->kind) {
            case Kind::ConfigureV2: queue_v2(*command); break;
            case Kind::Select: case Kind::Standby: live->select_slot(command->kind == Kind::Standby ? -1 : command->value, now, last_activity_error); break;
            case Kind::SelectV2: live->select(command->zone, command->activity, command->request, command->expected_state, now, last_activity_error); break;
            case Kind::StopV2: live->stop(command->zone, now); break;
            case Kind::Key:
                if (command->key_generation == key_generation.load() && (!command->activity_bound || command->activity_generation == cached.activity_generation))
                    live->key(static_cast<uint8_t>(command->value), command->repeat, now);
                break;
            case Kind::Release: live->release(now); break;
            case Kind::Discover: live->discover(now); break;
            case Kind::Authorized: case Kind::RemotePrepare: case Kind::RemoteFence: case Kind::RemoteQuiescent: case Kind::RemoteActivate: case Kind::RemoteAbort: case Kind::MemberPrepare: case Kind::MemberActivate: case Kind::MemberAbort: case Kind::RemoteHooks: case Kind::RemoteOwner: case Kind::RemotePlanResult: case Kind::RemoteOperationResult: case Kind::InputIntent: case Kind::RemoteRenew: case Kind::DiscoveryLease: case Kind::AuthorityTransition: case Kind::Retire: case Kind::MaintenanceBegin: case Kind::MaintenanceEnd: case Kind::SettingsCommit: break;
            case Kind::Configure: last_activity_error = "Legacy configuration cannot replace an active schema2 system"; break;
            }
            delete command;
        }
        // Match the original owner loop: queued commands, especially release,
        // win before an otherwise idle completion-paced held-key repeat.
        live->tick(now, !received_command);
        install_v2(now);
        complete_authority_transition();
        complete_retirement();
        complete_maintenance();
        if (pending_settings) complete_settings(uint64_t(esp_timer_get_time()) / 1000);
        publish();
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
            if (receive() || !wires_idle()) quiet_since = esp_timer_get_time();
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
                // A single lost poll under radio/flash load must not remove a
                // responding native appliance. Retry once, retaining a fixed
                // scan bound and requiring an actual ACK for presence.
                for (unsigned attempt = 0; attempt < 2; ++attempt) {
                    const auto status = bus.transmit(poll, 1);
                    receive();
                    if (status == cec::TxResult::Success) { hub.discovered(side, address); break; }
                }
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
        if (!p.owner_started) {
            p.owner_started = true;
            if (p.local_coordinator.load() && (!p.consortium || !p.octopus)) { p.legacy_enabled = true; p.claim_addresses(); p.scan_requested = true; }
            else { p.passive_ports(); p.publish(); }
        }
        for (;;) {
            if (p.hardware_profile && (!p.live || !p.v2_mode.load())) p.poll_presence(uint64_t(esp_timer_get_time()) / 1000);
            p.complete_authority_transition();
            p.complete_retirement();
            p.complete_maintenance();
            if (p.pending_settings) p.complete_settings(uint64_t(esp_timer_get_time()) / 1000);
            if (p.live && p.v2_mode.load()) { p.run_v2_iteration(); continue; }
            if (p.pending_v2) { p.install_v2(uint64_t(esp_timer_get_time()) / 1000); if (p.pending_v2) { vTaskDelay(pdMS_TO_TICKS(10)); continue; } if (p.live && p.live->configured()) continue; }
            if (!p.local_coordinator.load() || p.maintenance_started || !p.legacy_enabled) {
                // Passive membership/recovery never executes the legacy hub.
                // Only a locally queued authenticated slice can activate the
                // semantic agent; radio/network loss grants no local autonomy.
                cec::Frame observation;
                for (size_t i = 0; i < p.native_port_count; ++i)
                    for (unsigned n = 0; n < 32 && p.native_ports[i].bus->receive(observation); ++n) {}
                Command* command = nullptr;
                if (xQueueReceive(p.commands, &command, pdMS_TO_TICKS(10)) == pdTRUE) {
                    xSemaphoreGive(p.command_slots);
                    if (command->kind == Kind::ConfigureV2) p.queue_v2(*command);
                    else if (command->kind == Kind::Configure && p.local_coordinator.load() && !p.maintenance_started) {
                        // Legacy ownership is an explicit migration action.
                        // Typed startup starts monitor-only and never briefly
                        // acknowledges or responds using stale legacy roles.
                        p.legacy_enabled = true; p.claim_addresses();
                        p.hub.configure(std::move(command->activities), std::move(command->quirks));
                        p.scan_requested = true;
                    }
                    else if (!p.remote_command(*command, uint64_t(esp_timer_get_time()) / 1000)) p.last_activity_error = "Local control is unavailable without coordinator authority";
                    delete command; p.publish();
                }
                continue;
            }
            p.active_wire_generation = p.wire_generation.load();
            p.active_key_generation = p.key_generation.load();
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
                case Kind::Standby: missing = p.hub.activity_needs_discovery(-1); break;
                default: break;
                }
                if (missing) p.scan_requested = true;
                if (p.scan_requested) p.scan();
                p.apply_emergency_release();
                p.hub.tick(esp_timer_get_time());
                p.active_wire_generation = command->wire_generation;
                p.active_key_generation = command->key_generation;
                switch (command->kind) {
                case Kind::ConfigureV2: p.stop_hold(true); p.queue_v2(*command); p.scan_requested = false; break;
                case Kind::SelectV2: case Kind::StopV2: p.last_activity_error = "Schema2 configuration is not active"; break;
                case Kind::Authorized: case Kind::RemotePrepare: case Kind::RemoteFence: case Kind::RemoteQuiescent: case Kind::RemoteActivate: case Kind::RemoteAbort: case Kind::MemberPrepare: case Kind::MemberActivate: case Kind::MemberAbort: case Kind::RemoteHooks: case Kind::RemoteOwner: case Kind::RemotePlanResult: case Kind::RemoteOperationResult: case Kind::InputIntent: case Kind::RemoteRenew: case Kind::DiscoveryLease: case Kind::AuthorityTransition: case Kind::Retire: case Kind::MaintenanceBegin: case Kind::MaintenanceEnd: case Kind::SettingsCommit: p.remote_command(*command, uint64_t(esp_timer_get_time()) / 1000); break;
                case Kind::Configure:
                    if (!command->presentation_only) p.stop_hold();
                    p.hub.configure(std::move(command->activities), std::move(command->quirks));
                    if (!command->presentation_only) {
                        xSemaphoreTake(p.lock, portMAX_DELAY); p.settling.cancel(); xSemaphoreGive(p.lock);
                    }
                    break;
                case Kind::Select:
                    if (p.compile_activity_intent(*command)) p.apply_activity(command->value);
                    else p.fail_activity(command->value, "Activity request was superseded by a newer change.");
                    break;
                case Kind::Key:
                    if (command->wire_generation == p.wire_generation.load() && command->key_generation == p.applied_key_generation &&
                        (!command->activity_bound || command->activity_generation == p.hub.activity_generation())) {
                        if (command->repeat && command->value == p.failed_key) break;
                        p.stop_hold();
                        if (command->repeat) {
                            p.repeating_key = command->value;
                            p.repeating_generation = command->hold_generation;
                            p.send_held_key();
                        } else {
                            p.sending_one_key = true;
                            p.hub.press_key(static_cast<uint8_t>(command->value));
                            p.sending_one_key = false;
                        }
                    }
                    break;
                case Kind::Release: p.stop_hold(); break;
                case Kind::Standby:
                    if (p.compile_activity_intent(*command)) p.apply_activity(-1);
                    else p.fail_activity(-1, "Activity request was superseded by a newer change.");
                    break;
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
    p.front_defaults[0] = front_physical;
    p.native_ports[0] = {0, system::Side::front, &p.front, front_gpio};
    p.native_ports[1] = {0, system::Side::back, &p.back, back_gpio};
    p.native_port_count = 2;
    return p.start_owner();
}
esp_err_t HubService::start(const cec::BoardProfile& profile, uint16_t front_physical) {
    std::array<uint16_t, 5> defaults{{0x1000, 0x1000, 0x1000, 0x1000, 0x1000}};
    defaults[0] = front_physical;
    return start(profile, defaults);
}
esp_err_t HubService::start(const cec::BoardProfile& profile, const std::array<uint16_t, 5>& front_physical) {
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
            auto& extra = p.extra_buses[2u * (pin.tentacle_id - 1u) + (side == system::Side::back ? 1u : 0u)];
            extra.reset(new (std::nothrow) cec::Bus);
            if (!extra) { p.reset_startup(); return ESP_ERR_NO_MEM; }
            bus = extra.get();
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
    if (id >= 5) return {};
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
    if (!boot || !persist || presence.empty() || presence.size() > 5) return ESP_ERR_INVALID_ARG;
    bool used[5]{};
    for (const auto& item : presence) {
        if (item.first >= 5 || !item.second || used[item.first]) return ESP_ERR_INVALID_ARG;
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
    if (!p.local_coordinator.load()) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(p.lock, portMAX_DELAY);
    if (index < -1 || index >= static_cast<int>(p.cached.activities.size())) { xSemaphoreGive(p.lock); return ESP_ERR_INVALID_ARG; }
    if (p.v2_mode.load()) {
        if (index >= 0 && p.cached.activities[index].name.empty()) { xSemaphoreGive(p.lock); return ESP_ERR_INVALID_ARG; }
        if (p.cached.activity_settling && index != p.cached.current_activity) { xSemaphoreGive(p.lock); return ESP_ERR_TIMEOUT; }
        auto* command = new (std::nothrow) Impl::Command{index < 0 ? Impl::Kind::Standby : Impl::Kind::Select, index};
        if (command) command->route_transition = index != p.cached.current_activity;
        const auto result = p.submit(command); xSemaphoreGive(p.lock); return result;
    }
    const bool active = p.settling.active(esp_timer_get_time());
    const int target = active ? p.settling.target() : p.cached.current_activity;
    if (active && index != target) { xSemaphoreGive(p.lock); return ESP_ERR_TIMEOUT; }
    auto *command = new (std::nothrow) Impl::Command{index < 0 ? Impl::Kind::Standby : Impl::Kind::Select, index};
    if (command) command->route_transition = index != target;
    const auto error = p.submit(command, [&] { if (index != target) p.settling.reserve(index); return ESP_OK; });
    xSemaphoreGive(p.lock); return error;
}
int HubService::activity_target() const {
    auto &p = *impl_; if (!p.lock) return -1;
    xSemaphoreTake(p.lock, portMAX_DELAY);
    const auto target = !p.v2_mode.load() && p.settling.active(esp_timer_get_time()) ? p.settling.target() : p.cached.current_activity;
    xSemaphoreGive(p.lock); return target;
}
bool HubService::select_activity(int index) { return request_activity(index) == ESP_OK; }
bool HubService::press_key(uint8_t key, bool repeat) {
    if (!impl_->local_coordinator.load()) return false;
    if (key == 0x40) return standby();
    auto *command = new (std::nothrow) Impl::Command{Impl::Kind::Key, key, repeat};
    if (command) {
        command->key_generation = impl_->key_generation.load();
        command->hold_generation = impl_->hold_generation.load();
    }
    return impl_->enqueue(command);
}
bool HubService::press_key_for_activity(uint8_t key, uint32_t activity_generation, bool repeat) {
    if (!impl_->local_coordinator.load()) return false;
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
    if (command) { command->repeat = local; command->route_generation = epoch; }
    const auto result = impl_->request(command, 5000);
    if (!result) { if (result.code != system::AdmissionCode::expired) impl_->authority_transition.store(false); return ESP_ERR_INVALID_STATE; }
    return ESP_OK;
}
esp_err_t HubService::configure_v2(system::LocalConfiguration config, std::function<esp_err_t()> commit) {
    if (!impl_->consortium || !impl_->octopus || !impl_->local_coordinator.load()) return ESP_ERR_INVALID_STATE;
    auto* command = new (std::nothrow) Impl::Command{Impl::Kind::ConfigureV2}; if (!command) return ESP_ERR_NO_MEM;
    command->system_config = std::make_shared<system::LocalConfiguration>(std::move(config));
    command->persist = std::move(commit);
    const auto result = impl_->request(command, 5000);
    return result ? ESP_OK : result.code == system::AdmissionCode::expired ? ESP_ERR_TIMEOUT : ESP_ERR_INVALID_STATE;
}
esp_err_t HubService::request_activity_v2(system::ZoneId zone, system::ActivityId activity, system::MessageId request,
                                       std::optional<uint64_t> expected, bool repair) {
    if (!zone || !activity || !request) return ESP_ERR_INVALID_ARG;
    if (!impl_->local_coordinator.load()) return ESP_ERR_INVALID_STATE;
    auto* command = new (std::nothrow) Impl::Command{Impl::Kind::SelectV2}; if (!command) return ESP_ERR_NO_MEM;
    command->zone = zone; command->activity = activity; command->request = request; command->expected_state = expected; command->repeat = repair; command->route_transition = true;
    return impl_->submit(command);
}
esp_err_t HubService::stop_zone_v2(system::ZoneId zone) {
    if (!zone) return ESP_ERR_INVALID_ARG;
    if (!impl_->local_coordinator.load()) return ESP_ERR_INVALID_STATE;
    auto* command = new (std::nothrow) Impl::Command{Impl::Kind::StopV2}; if (!command) return ESP_ERR_NO_MEM;
    command->zone = zone; command->route_transition = true; return impl_->submit(command);
}
esp_err_t HubService::execute_authorized(system::AuthorizedCecOperation operation) {
    auto* command = new (std::nothrow) Impl::Command{Impl::Kind::Authorized}; if (!command) return ESP_ERR_NO_MEM;
    command->authorized = std::move(operation); return impl_->submit(command);
}
system::AdmissionResult HubService::admit_authorized(system::AuthorizedCecOperation operation, uint32_t timeout) {
    auto* command = new (std::nothrow) Impl::Command{Impl::Kind::Authorized};
    if (command) command->authorized = std::move(operation);
    return impl_->request(command, timeout);
}
std::optional<bool> HubService::remote_key_active(const system::AuthorizedCecOperation& operation, bool synchronously_admitted) const {
    if (operation.kind != system::OperationKind::press_key || !operation.arguments.repeat || !operation.arguments.hold_token) return {};
    auto& p = *impl_; xSemaphoreTake(p.lock, portMAX_DELAY);
    std::optional<bool> result;
    if (p.key_snapshot_valid && std::any_of(p.cached_policies.begin(),p.cached_policies.end(),[&](const auto& policy) {
        return policy.port == operation.target.port && policy.owner_boot_id == operation.fence.owner_boot_id;
    })) {
        if (std::any_of(p.cached_active_keys.begin(),p.cached_active_keys.end(),[&](const auto& active){return Impl::same_key(active,operation);})) result = true;
        else if (synchronously_admitted) result = false;
    }
    xSemaphoreGive(p.lock); return result;
}
esp_err_t HubService::configure_member_v2(system::LocalConfiguration config, std::function<esp_err_t()> persist) {
    if (impl_->local_coordinator.load() || !config.member) return ESP_ERR_INVALID_STATE;
    auto* command = new (std::nothrow) Impl::Command{Impl::Kind::ConfigureV2};
    if (command) { command->system_config = std::make_shared<system::LocalConfiguration>(std::move(config)); command->persist = std::move(persist); }
    const auto result = impl_->request(command, 5000);
    return result ? ESP_OK : result.code == system::AdmissionCode::expired ? ESP_ERR_TIMEOUT : ESP_ERR_INVALID_STATE;
}
system::AdmissionResult HubService::prepare_member_v2(system::LocalConfiguration config, std::string hash, std::function<esp_err_t()> persist) {
    auto* command = new (std::nothrow) Impl::Command{Impl::Kind::MemberPrepare};
    if (command) {
        command->system_config = std::make_shared<system::LocalConfiguration>(std::move(config)); command->payload_hash = std::move(hash); command->persist = std::move(persist);
    }
    return impl_->request(command);
}
esp_err_t HubService::activate_member_v2(std::string hash) {
    auto* command = new (std::nothrow) Impl::Command{Impl::Kind::MemberActivate}; if (command) command->payload_hash = std::move(hash);
    const auto result = impl_->request(command, 5000); return result ? ESP_OK : result.code == system::AdmissionCode::expired ? ESP_ERR_TIMEOUT : ESP_ERR_INVALID_STATE;
}
void HubService::abort_member_v2(std::string hash) {
    auto* command = new (std::nothrow) Impl::Command{Impl::Kind::MemberAbort}; if (command) command->payload_hash = std::move(hash);
    impl_->request(command);
}
system::AdmissionResult HubService::prepare_remote_plan(system::Plan plan, std::vector<system::PortPolicySnapshot> policies) {
    auto* command = new (std::nothrow) Impl::Command{Impl::Kind::RemotePrepare};
    if (command) { command->plan = std::move(plan); command->policies = std::move(policies); }
    return impl_->request(command);
}
system::AdmissionResult HubService::fence_remote_route(system::SessionId session, uint64_t generation) {
    auto* command = new (std::nothrow) Impl::Command{Impl::Kind::RemoteFence};
    if (command) { command->session = session; command->route_generation = generation; }
    return impl_->request(command);
}
bool HubService::remote_route_quiescent(system::SessionId session, uint64_t generation) const {
    auto* command = new (std::nothrow) Impl::Command{Impl::Kind::RemoteQuiescent};
    if (command) { command->session = session; command->route_generation = generation; }
    return bool(impl_->request(command));
}
system::AdmissionResult HubService::activate_remote_plan(system::PlanId id, uint32_t ttl) {
    auto* command = new (std::nothrow) Impl::Command{Impl::Kind::RemoteActivate};
    if (command) { command->plan.plan_id = id; command->authority_ttl = ttl; }
    return impl_->request(command);
}
system::AdmissionResult HubService::abort_remote_plan(system::PlanId id) {
    auto* command = new (std::nothrow) Impl::Command{Impl::Kind::RemoteAbort};
    if (command) command->plan.plan_id = id;
    return impl_->request(command);
}
system::AdmissionResult HubService::renew_remote_authority(system::PlanId id, system::SessionId session, uint64_t generation, uint32_t ttl) {
    auto* command = new (std::nothrow) Impl::Command{Impl::Kind::RemoteRenew};
    if (command) { command->plan.plan_id = id; command->session = session; command->route_generation = generation; command->authority_ttl = ttl; }
    return impl_->request(command);
}
system::AdmissionResult HubService::discover_authorized(uint32_t ttl) {
    auto* command = new (std::nothrow) Impl::Command{Impl::Kind::DiscoveryLease};
    if (command) command->authority_ttl = ttl;
    return impl_->request(command);
}
std::vector<system::PortPolicySnapshot> HubService::local_policy_snapshot() const {
    xSemaphoreTake(impl_->lock, portMAX_DELAY); auto policies = impl_->cached_policies; xSemaphoreGive(impl_->lock); return policies;
}
void HubService::set_remote_execution_hooks(system::RemoteExecutionHooks hooks) {
    if (!impl_->task) { impl_->remote_hooks = std::move(hooks); return; }
    auto* command = new (std::nothrow) Impl::Command{Impl::Kind::RemoteHooks};
    if (command) command->remote_hooks = std::move(hooks);
    impl_->submit(command);
}
system::AdmissionResult HubService::update_remote_owner(system::OctopusId owner, std::vector<system::PortPolicySnapshot> policies, bool ready, uint32_t ttl) {
    auto* command = new (std::nothrow) Impl::Command{Impl::Kind::RemoteOwner};
    if (command) { command->remote_owner = owner; command->policies = std::move(policies); command->repeat = ready; command->authority_ttl = ttl; }
    return impl_->request(command);
}
system::AdmissionResult HubService::remote_plan_result(system::OctopusId owner, system::PlanId id, system::RemotePlanPhase phase, bool accepted, std::string error) {
    auto* command = new (std::nothrow) Impl::Command{Impl::Kind::RemotePlanResult};
    if (command) { command->remote_owner = owner; command->plan.plan_id = id; command->remote_phase = phase; command->repeat = accepted; command->error = std::move(error); }
    return impl_->request(command);
}
system::AdmissionResult HubService::remote_operation_result(system::OctopusId owner, system::CommandId id, system::StepResult result, std::string error) {
    auto* command = new (std::nothrow) Impl::Command{Impl::Kind::RemoteOperationResult};
    if (command) { command->remote_owner = owner; command->command_id = id; command->step_result = result; command->error = std::move(error); }
    return impl_->request(command);
}
system::InputContext HubService::input_context() const {
    xSemaphoreTake(impl_->lock, portMAX_DELAY); auto context = impl_->cached_input_context; xSemaphoreGive(impl_->lock); return context;
}
system::AdmissionResult HubService::submit_input_intent(system::InputIntent input) {
    if (!input.admissible_until_ms && input.action != system::InputAction::hold_release)
        input.admissible_until_ms = uint64_t(esp_timer_get_time()) / 1000 + 1000;
    auto* command = new (std::nothrow) Impl::Command{Impl::Kind::InputIntent}; if (command) command->input = std::move(input);
    return impl_->request(command);
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
    return result ? ESP_OK : result.code == system::AdmissionCode::expired ? ESP_ERR_TIMEOUT : ESP_ERR_INVALID_STATE;
}
void HubService::end_maintenance() {
    impl_->request(new (std::nothrow) Impl::Command{Impl::Kind::MaintenanceEnd});
}
bool HubService::maintenance_ready() const { return impl_->maintenance_applied.load(); }
system::ExecutionSnapshot HubService::execution_snapshot() const {
    xSemaphoreTake(impl_->lock, portMAX_DELAY); auto state = impl_->cached_execution; xSemaphoreGive(impl_->lock); return state;
}
void HubService::on_change(std::function<void(const Snapshot &)> callback) {
    auto &p = *impl_; if (!p.lock) return;
    xSemaphoreTake(p.lock, portMAX_DELAY); p.callback = std::move(callback); xSemaphoreGive(p.lock);
}
}
