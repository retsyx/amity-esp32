/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#include "amity/hub_service.hpp"
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
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
#include "driver/gpio.h"
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
struct HubService::Adapter : Transport {
    struct WireGuards {
#ifdef ESP_PLATFORM
        static void *operator new(size_t size) { auto* p=heap_caps_malloc(size,MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);if(!p)std::abort();return p; }
        static void operator delete(void* p){heap_caps_free(p);}
#endif
        std::atomic<uint32_t> owned{1},bindings[2][15]{};
    };
    Impl& owner;
    const uint8_t adapter_id;
    std::unique_ptr<WireGuards> guards{new WireGuards};
    std::atomic<uint32_t>& owned_wire_generation=guards->owned;
    std::atomic<uint32_t> (&binding_wire_generation)[2][15]=guards->bindings;
    struct Flight {cec::TxCompletion completion;bool used=false,activity=false,cleanup=false;int key=-1;uint32_t instance=0;};
    std::array<Flight,32> flights{};cec::TxCompletion discovery_completion;
    cec::Bus front,back;
    Hub hub{*this};
    uint8_t front_address=15,back_address=15;uint16_t fallback_physical=0x1000;
    uint8_t requested_scan_sides=3;bool scan_running=false;
    std::array<int64_t,2> last_scan{{-120000000,-120000000}};int64_t last_claim=-5000000;
    uint32_t dropped_tx=0,discovery_generation=0;Flight* sending_key=nullptr;
    bool setting_activity=false,pacing_receive=false,emitting_user_key=false,emitting_key_cleanup=false;
    unsigned activity_outstanding=0;
    enum class DiscoveryPhase {Idle,ClaimBack,ScanBack,BackReplies,ClaimFront,ScanFront,FinalReplies};
    struct DiscoveryState {DiscoveryPhase phase=DiscoveryPhase::Idle;bool waiting=false,claim_only=false,claimed=false;uint8_t candidate=0,nacks=0,address=0,sides=0;int64_t deadline=0,quiet_since=0;}discovery;
    const system::OwnedAuthorization* executing_auth=nullptr;
    bool executing_activity_step=false;uint32_t executing_activity_instance=0;
    uint64_t received_generation=0;
    system::BootId received_controller_boot;system::LinkId received_link;
    Adapter(Impl& controller,uint8_t id):owner(controller),adapter_id(id){}
    void stop_holds();
    void invalidate_binding(BusSide side, uint8_t address) override;
    Flight* reserve_flight();
    bool complete_transmissions();
    bool transmissions_pending() const;
    bool activity_step_pending() const override;
    bool send(BusSide side, uint8_t destination, const uint8_t *data, size_t size) override;
    static uint8_t scan_side(BusSide side);
    uint8_t pending_scan_sides() const;
    bool request_lookup_scan(BusSide side);
    bool request_scan(BusSide side) override;
    void request_scan() override;
    bool receive();
    bool discovery_pending() const;
    void begin_claim(BusSide side);
    void start_discovery();
    void finish_claim(BusSide side);
    void claim_tick(BusSide side);
    void begin_reply_window(bool back_side, int64_t now);
    void scan_tick(BusSide side, int64_t now);
    void discovery_tick(bool received);
    void finish_discovery();
    bool local_activity_authorized(const system::OwnedAuthorization& auth,uint32_t instance)const;
    bool owner_authorized(const system::OwnedAuthorization& auth,bool arrival=true) const;
    bool source_input_next(const system::OwnedIntent& intent,const system::AdmissionResult& result,bool input_announced=false) const;
    system::AdmissionResult execute_owned(const system::OwnedIntent& intent,const system::OwnedAuthorization& auth,bool validate_only=false,bool input_announced=false,uint32_t instance=0,bool local_step=false);
};
struct HubService::Impl : Adapter {
#ifdef ESP_PLATFORM
    static void *operator new(size_t size) { auto *p=heap_caps_malloc(size,MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT); if(!p)std::abort(); return p; }
    static void operator delete(void *p) { heap_caps_free(p); }
#endif
    enum class Kind:uint8_t { Configure, Select, Key, Release, Standby, Discover, AuthorityTransition, Retire, MaintenanceBegin, MaintenanceEnd, SettingsCommit,
        ScopedConfigure, Owned, OwnerActivity, OwnedBind, OwnedFence, OwnedDiscover, OwnedContext, Received, KeyCleanup, KeyRepeat, KeyEdges };
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
    struct ManagementCommand;struct ConfigurationCommand;struct ActionCommand;struct ReceivedCommand;struct InputCommand;
    // The common IPC header carries admission and continuation state, not an
    // activity recipe or configuration. Ordinary button edges need only this.
    struct Command {
        explicit Command(Kind k,int v=0,bool r=false)
            :kind(k),repeat(r),cleanup_reservation(false),activity_bound(false),route_transition(false),owned_deferred(false),
             local_activity_step(false),activity_canceled(false),activity_started(false),activity_completed(false),input_started(false),
             owned_waiting(false),owned_input_announced(false),configuration_applied(false),configuration_reservation(false),
             activity_reservation(false),selection_reservation(false),setup_blocked(false),selection_bound(false),selection_sent(false),
             value(static_cast<int16_t>(v)){}
        virtual ~Command()=default;
        virtual uint32_t receipt()const{return 0;}
        Kind kind;
        bool repeat:1,cleanup_reservation:1,activity_bound:1,route_transition:1,owned_deferred:1,local_activity_step:1;
        bool activity_canceled:1,activity_started:1,activity_completed:1,input_started:1,owned_waiting:1,owned_input_announced:1;
        bool configuration_applied:1,configuration_reservation:1,activity_reservation:1,selection_reservation:1,setup_blocked:1,selection_bound:1;
        bool selection_sent:1;
        int64_t input_admitted_us=0;
        std::shared_ptr<Reply> reply;
        uint32_t key_generation=0,release_generation=0,activity_generation=0,expected_selection=0;
        int16_t value=0;
        uint8_t discovery_sides=0,lookup_adapter=0,edge_cursor=0;
        ManagementCommand& management();const ManagementCommand& management()const;
        ConfigurationCommand& configuration();const ConfigurationCommand& configuration()const;
        ActionCommand& action();const ActionCommand& action()const;
        ReceivedCommand& received();const ReceivedCommand& received()const;
        InputCommand& input();const InputCommand& input()const;
    };
    struct ManagementCommand:Command {
        using Command::Command;
        system::OctopusId controller;system::LinkId link;uint64_t config_revision=0,presentation_revision=1;
        std::function<esp_err_t()> persist;std::function<void(system::OwnedContextResult)> context_completion;
        system::BootId settings_boot;std::vector<AdapterAddressChange> settings_adapters;uint64_t settings_deadline=0;
    };
    struct ConfigurationCommand:ManagementCommand {
        using ManagementCommand::ManagementCommand;
        uint32_t activity_receipt=0;
        uint32_t receipt()const override{return activity_receipt;}
        std::vector<Activity> activities;
        std::vector<system::ScopedSelector> selectors;OwnedSender sender;
        std::vector<system::OctopusId> owners;
    };
    struct ActionCommand:Command {
        using Command::Command;
        uint32_t activity_receipt=0,activity_instance=0;uint8_t context_cursor=0;
        uint32_t receipt()const override{return activity_receipt;}
        system::AdmissionResult owned_result;
        system::OwnedIntent owned;system::OwnedAuthorization auth;
    };
    struct ReceivedCommand:Command {
        using Command::Command;
        uint32_t intent_generation=0;
        cec::Frame received_frame;BusSide received_side=BusSide::Back;uint8_t received_adapter=0;
        Hub::ReceiveStep received_step=Hub::ReceiveStep::Frame;
        uint64_t received_generation=0,received_deadline=0;uint32_t received_activity=0,received_binding=0,received_wire=0;
        system::BootId received_boot;
    };
    struct InputState {
        system::OctopusId owner;system::ConsortiumId consortium;
        system::OwnedAuthorization auth;
        uint64_t generation=0;
        std::array<system::KeyEdge,2> edges{};uint8_t edge_count=0;
        InputState()=default;
        InputState(const system::OwnedIntent& intent,const system::OwnedAuthorization& authorization)
            :owner(intent.target.port.octopus_id),consortium(intent.target.port.consortium_id),auth(authorization),generation(intent.generation),edges(intent.edges),edge_count(intent.edge_count){}
    };
    struct InputCommand:Command {
        InputState state;
        InputCommand(const system::OwnedIntent& intent,const system::OwnedAuthorization& auth):Command(Kind::KeyEdges),state(intent,auth){}
    };
#ifdef ESP_PLATFORM
    static_assert(sizeof(Command)<=48,"Button IPC must not carry activity/configuration payloads");
    static_assert(sizeof(InputCommand)<=184,"Authenticated button IPC must remain compact");
#endif
    static Command* owned_request(const system::OwnedIntent& intent,const system::OwnedAuthorization& auth){
        if(intent.kind==system::OwnedIntentKind::key_edges)return new(std::nothrow)InputCommand(intent,auth);
        auto* command=new(std::nothrow)ActionCommand{Kind::Owned};
        if(command){command->owned=intent;command->auth=auth;}
        return command;
    }

    std::atomic<uint32_t> intent_generation{0},key_generation{0},activity_selection{0};
    std::atomic<unsigned> owner_activity_commands{0};
    std::atomic<unsigned> activity_requests{0};std::atomic<bool> setup_running{false};
    std::atomic<unsigned> scoped_configurations{0};bool scoped_publication_pending=false;

    std::unique_ptr<std::array<std::atomic<uint32_t>,256>> release_generations;
    enum class KeyPhase:uint8_t {up,down,failed};
    struct HeldKey {KeyPhase phase=KeyPhase::up;uint8_t mapped=0;bool waiting=false,emitted=false;uint32_t instance=0,release_generation=0;};
    // Only the owner task reads this state. Frame-start fences refer to the
    // separate internal generation counters, never to these entries.
    std::vector<HeldKey> keys;

    struct NativePort {uint8_t tentacle;system::Side side;cec::Bus*bus;int gpio;};
    std::array<NativePort,amity::max_cec_ports> native_ports{};size_t native_port_count=0;
    std::array<std::unique_ptr<Adapter>,amity::max_tentacles-1> adapters;
    std::optional<cec::BoardProfile> hardware_profile;
    std::array<AdapterRuntimeState,amity::max_tentacles> cached_adapters{};
    std::array<uint16_t,amity::max_tentacles> front_defaults=filled_tentacles(uint16_t{0x1000});
    system::ConsortiumId consortium;system::OctopusId octopus;system::BootId local_boot;
    std::atomic<bool> local_coordinator{true},authority_transition{false},maintenance{false},maintenance_applied{false},settings_gate{false},retiring{false};
    uint64_t coordinator_epoch=1;
    std::shared_ptr<Reply> pending_settings;system::BootId settings_boot;std::vector<AdapterAddressChange> settings_adapters;
    uint64_t settings_deadline=0;std::function<esp_err_t()> settings_persist;bool settings_paused=false,owner_started=false,legacy_enabled=false;
    std::atomic<bool> maintenance_started{false};
    Hub activity_hub{*this};
    QueueHandle_t commands=nullptr;SemaphoreHandle_t lock=nullptr,command_slots=nullptr,cleanup_slots=nullptr,configuration_gate=nullptr;TaskHandle_t task=nullptr;
    std::deque<std::unique_ptr<Command>> deferred;Snapshot cached;
    std::function<void(const Snapshot&)>callback;

    // Boot enumerates both wires once. Only explicit UI requests and real
    // recipient-cache misses may request another enumeration afterwards.


    uint32_t applied_key_generation=0;
    bool setup_gates_input=false;
    std::atomic<int> reserved_target{-1};
    struct StepResult {system::OwnedIntent request;system::AdmissionResult result;};
    std::optional<StepResult> pending_step;bool step_pending=false;uint64_t shared_generation=1;
    std::optional<system::OwnedIntent> shared_selection;
    // Retry owners whose latest normal selection could not enter transport.
    // A later ready capture also offers that selection: transport admission
    // alone does not establish that the owner accepted it.
    std::vector<system::OctopusId> unadmitted_selection_owners;
    uint32_t shared_selection_receipt=0;
    std::atomic<uint32_t> activity_instance{0};
    // Normal selections are idempotent across authenticated link recovery.
    // This receipt is independent of the currently executing owner recipe.
    system::BootId accepted_controller_boot;
    std::optional<system::OwnedIntent> accepted_selection;

    std::string last_activity_error;
    bool scoped_mode=false;system::OctopusId owned_controller;std::atomic<uint64_t> owned_config_revision{0};uint64_t owned_presentation_revision=0,cached_revision=0;
    uint32_t processed_selection=0;
    OwnedSender owned_sender;
    system::OwnedAuthorization activity_auth;
    std::vector<system::OwnedAdapterSnapshot> owned_reports;
    // Cross-task observations are latest state, rather than ordinary work in
    // the input FIFO. Coalescing cannot lose a final completion under input
    // pressure, and never retains outgoing CEC frames.
    std::vector<system::OwnedAdapterSnapshot> pending_reports;bool reports_dirty=false;
    static constexpr size_t report_capacity=64*amity::max_tentacles;
    std::vector<uint8_t> pressed_owner;
    std::vector<system::OctopusId> key_owners;
    system::OwnedAuthorization input_auth;
    std::optional<system::OwnedIntent> owner_context;
    uint64_t input_activity_generation=0;
    uint8_t key_cleanup_roles=0;bool key_cleanup_queued=false;
    std::vector<system::ScopedSelector> active_selectors;bool projection_dirty=false;
    std::vector<system::OctopusId> activity_owners;
    std::shared_ptr<const OwnedObserver> observer_slot;
    std::shared_ptr<const OwnedAuthorizer> authorizer_slot;
    std::shared_ptr<const OwnedDeadline> deadline_slot;

    Impl():Adapter(*this,0){lock=xSemaphoreCreateMutex();configuration_gate=xSemaphoreCreateMutex();}
    Hub& control_hub(){return scoped_mode?activity_hub:hub;}
    const Hub& control_hub()const{return scoped_mode?activity_hub:hub;}
    Adapter* adapter(uint8_t id){return id==0?this:id<amity::max_tentacles?adapters[id-1].get():nullptr;}

    static bool cleanup(const Command& command) {
        return command.kind==Kind::Release||command.kind==Kind::OwnedFence||command.kind==Kind::KeyCleanup||
            (command.kind==Kind::KeyEdges&&command.input().state.edge_count==1&&!command.input().state.edges[0].down);
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
        // Key state is task data. Allocate after PSRAM setup; only the
        // ISR-visible generation counters need the Impl's internal memory.
        keys.resize(256);release_generations.reset(new std::array<std::atomic<uint32_t>,256>{});if(local_coordinator.load())pressed_owner.assign(256,0xff);
#ifdef ESP_PLATFORM
        ESP_LOGI(TAG,"Owner key state bytes=%u external=%d; controller owners bytes=%u",
            static_cast<unsigned>(keys.size()*sizeof(HeldKey)),esp_ptr_external_ram(keys.data()),static_cast<unsigned>(pressed_owner.size()));
#endif
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
        native_port_count = 0; hardware_profile.reset();
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
        if(command.configuration_reservation&&!command.configuration_applied)--scoped_configurations;
        if(command.activity_reservation)--owner_activity_commands;
        if(command.selection_reservation)--activity_requests;
        xSemaphoreGive(command.cleanup_reservation ? cleanup_slots : command_slots);
    }

    esp_err_t submit(Command *command, const std::function<esp_err_t()> &commit = {},TickType_t reservation_wait=0) {
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
        command->cleanup_reservation=(command->kind==Kind::Release&&command->selection_bound)||command->kind==Kind::OwnedFence||command->kind==Kind::KeyCleanup||
            (command->kind==Kind::KeyEdges&&command->input().state.edge_count==1&&!command->input().state.edges[0].down);
        if (xSemaphoreTake(command->cleanup_reservation ? cleanup_slots : command_slots,reservation_wait)!=pdTRUE){unlock();return ESP_ERR_NO_MEM;}
        const auto error = commit ? commit() : ESP_OK;
        if (error != ESP_OK) { free_reservation(*command); unlock(); return error; }
        if(command->kind==Kind::Key||(command->kind==Kind::KeyEdges&&command->input().state.edges[0].down))
            command->setup_blocked=activity_setup_in_progress();
        if(command->kind==Kind::KeyEdges)command->key_generation=key_generation.load();
        // Reservations include deferred discovery requests. No CEC capacity
        // or completion wait may delay admission of another command.
        if ((configuration&&command->kind!=Kind::ScopedConfigure) || command->route_transition) {
            // Fence received work against configuration and route changes.
            // Activity requests themselves retain their original FIFO order.
            intent_generation.fetch_add(1);
        }
        if(configuration&&command->kind!=Kind::ScopedConfigure){command->configuration().activity_receipt=activity_selection.fetch_add(1)+1;key_generation.fetch_add(1);}
        if(owner_activity(*command)||command->kind==Kind::Select||command->kind==Kind::Standby||
           (command->kind==Kind::Owned&&command->action().owned.kind==system::OwnedIntentKind::activity)){
            command->activity_reservation=true;++owner_activity_commands;
        }
        if(activity_request(*command)&&command->receipt()){command->selection_reservation=true;++activity_requests;}
        if(command->kind==Kind::Key||(command->kind==Kind::Release&&command->value>=0&&!command->selection_bound))
            command->input_admitted_us=esp_timer_get_time();
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
            pending_settings.reset(); settings_persist = {}; settings_adapters.clear();
            if (settings_paused && !retiring.load() && !authority_transition.load() && !maintenance.load()) {

                maintenance_started = false;
            }
            settings_paused = false; settings_gate.store(false);
        };
        if (pending_settings->canceled.load() || now >= settings_deadline) { fail(ESP_ERR_TIMEOUT, "Settings request expired before persistence"); return; }
        if (!settings_boot || settings_boot != local_boot || retiring.load() || authority_transition.load()) { fail(ESP_ERR_INVALID_STATE, "Amity identity changed before settings apply"); return; }
        for (const auto& change : settings_adapters) if (!connector_configured(change.tentacle_id)) {
            fail(ESP_ERR_INVALID_STATE, "HDMI adapter is not configured; reopen Settings"); return;
        }
        if (!wires_idle() || false) return;
        uint8_t waiting = 0;
        if (!pending_settings->settings_effect.compare_exchange_strong(waiting, 1)) { fail(ESP_ERR_TIMEOUT, "Settings request was canceled"); return; }
        const auto result = settings_persist ? settings_persist() : ESP_ERR_INVALID_ARG;
        pending_settings->settings_effect.store(3);
        if (result != ESP_OK) { fail(result, "Settings were not saved"); return; }
        // Apply on the same task that owns the CEC identity. The latest scanned
        // address takes precedence over the new default, without another scan.
        // The new fallback also covers an initial scan that is still pending.
        for (const auto& change : settings_adapters) {
            front_defaults[change.tentacle_id] = change.default_physical;
            auto* engine = adapter(change.tentacle_id);
            engine->fallback_physical = change.default_physical;
            if (engine->hub.apply_default_front_physical_address(change.default_physical))
                engine->hub.announce(BusSide::Front);
        }
        pending_settings->settings_result = ESP_OK;
        maintenance_started = false; maintenance_applied.store(false);
        settings_paused = false;
        publish();
        settings_gate.store(false);
        finish(pending_settings, {system::AdmissionCode::accepted, {}});
        pending_settings.reset(); settings_persist = {}; settings_adapters.clear();
    }

    bool enqueue(Command *command) { return submit(command) == ESP_OK; }

    static uint8_t key_role(uint8_t raw){return Hub::audio_key(raw)?1:2;}
    bool any_keys_active() const {
        const auto& root=*this;
        return std::any_of(root.keys.begin(),root.keys.end(),[](const auto& state){return state.phase==KeyPhase::down;});
    }
    void stop_key(uint8_t raw){
        auto& root=*this;auto& state=root.keys[raw];
        if(state.phase==KeyPhase::down&&state.emitted)root.key_cleanup_roles|=key_role(raw);
        state.phase=KeyPhase::up;state.waiting=state.emitted=false;++state.instance;
    }
    void stop_holds(){
        auto& root=*this;
        for(size_t raw=0;raw<root.keys.size();++raw)root.stop_key(static_cast<uint8_t>(raw));
    }
    void apply_emergency_release(){
        const auto generation=key_generation.load();
        if(generation!=applied_key_generation){
            if(scoped_mode&&local_coordinator.load())activity_hub.release_key();
            stop_holds();applied_key_generation=generation;
        }
        for(size_t raw=0;raw<keys.size();++raw){const auto released=(*release_generations)[raw].load();
            if(released==keys[raw].release_generation)continue;
            if(scoped_mode&&local_coordinator.load())activity_hub.release_key(static_cast<uint8_t>(raw));
            else stop_key(static_cast<uint8_t>(raw));
            keys[raw].release_generation=released;
        }
        for(auto i=deferred.begin();i!=deferred.end();){
            const bool input=(*i)->kind==Kind::Key||(*i)->kind==Kind::KeyRepeat||
                ((*i)->kind==Kind::KeyEdges);
            if(!input||(*i)->key_generation==applied_key_generation){++i;continue;}
            free_reservation(**i);finish((*i)->reply,{system::AdmissionCode::accepted,{}});i=deferred.erase(i);
        }
    }
    struct KeyRecipient {Adapter* engine;Device device;};
    std::optional<KeyRecipient> resolve_key_role(bool audio,Command& command){
        command.discovery_sides=0;const uint8_t raw=audio?0x41:0;
        std::optional<system::QualifiedDeviceName> target;
        if(!local_coordinator.load()&&owner_context){if(!owner_context->selected)return {};target=audio?Hub::audio_recipient(owner_context->audio,owner_context->display):owner_context->source;}
        else if(scoped_mode)target=activity_hub.key_target(raw);
        if(target){
            if(target->port.octopus_id!=octopus||target->port.consortium_id!=consortium)return {};
            auto* engine=adapter(target->port.tentacle_id);if(!engine||!connector_configured(target->port.tentacle_id))return {};
            auto* device=engine->hub.find_device(target->osd_name,target->port.side==system::Side::front?BusSide::Front:BusSide::Back);
            if(engine->hub.pending_resolution()){command.lookup_adapter=target->port.tentacle_id;command.discovery_sides=engine->hub.pending_resolution();return {};}
            if(device)return KeyRecipient{engine,*device};
            return {};
        }
        if(scoped_mode||owner_context)return {};
        auto* device=hub.find_device(hub.key_name(raw));
        if(hub.pending_resolution()){command.lookup_adapter=0;command.discovery_sides=hub.pending_resolution();return {};}
        return device?std::optional<KeyRecipient>{{this,*device}}:std::nullopt;
    }
    bool clean_keys(Command& command){
        const auto roles=key_cleanup_roles;if(!roles)return true;
        uint8_t active=0;for(size_t raw=0;raw<keys.size();++raw)if(keys[raw].phase==KeyPhase::down)active|=key_role(static_cast<uint8_t>(raw));
        std::optional<KeyRecipient> audio,source;
        if((roles|active)&1){audio=resolve_key_role(true,command);if(command.discovery_sides)return false;}
        if((roles|active)&2){source=resolve_key_role(false,command);if(command.discovery_sides)return false;}
        const auto same=[](const auto& left,const auto& right){return left&&right&&left->engine==right->engine&&left->device.bus==right->device.bus&&left->device.address==right->device.address;};
        const bool release_audio=(roles&1)&&!(active&1)&&audio&&!((active&2)&&same(audio,source));
        const bool release_source=(roles&2)&&!(active&2)&&source&&!((active&1)&&same(audio,source));
        const auto release=[](const auto& recipient,bool is_audio){auto& engine=*recipient->engine;engine.emitting_key_cleanup=true;engine.hub.transmit_release(recipient->device,is_audio);engine.emitting_key_cleanup=false;};
        if(release_audio)release(audio,true);
        else if((roles&1)&&!(active&1)&&audio)audio->engine->hub.key_released(true);
        if(release_source&&!(release_audio&&same(audio,source)))release(source,false);
        key_cleanup_roles=0;return true;
    }
    void schedule_key_cleanup(){
        if(!key_cleanup_roles||key_cleanup_queued)return;
        auto* command=new(std::nothrow)Command{Kind::KeyCleanup};if(!command)return;
        if(submit(command)==ESP_OK)key_cleanup_queued=true;
    }
    void refresh_audio_hold(){
        bool down=false;for(size_t raw=0;raw<keys.size();++raw)if(keys[raw].phase==KeyPhase::down&&Hub::audio_key(static_cast<uint8_t>(raw))){down=true;break;}
        std::optional<system::QualifiedDeviceName> target;
        if(!local_coordinator.load()&&owner_context)target=Hub::audio_recipient(owner_context->audio,owner_context->display);
        else if(scoped_mode)target=activity_hub.key_target(0x41);
        for(uint8_t id=0;id<amity::max_tentacles;++id)if(auto* engine=adapter(id))engine->hub.owner_audio_held(down&&(target?(target->port.octopus_id==octopus&&target->port.tentacle_id==id):(!scoped_mode&&id==0)));
    }
    bool send_held_key(uint8_t raw,Command& command){
        auto& state=keys[raw];
        if(input_auth.controller_id&&!owner_authorized(input_auth,false)){stop_key(raw);return true;}
        auto recipient=resolve_key_role(Hub::audio_key(raw),command);if(command.discovery_sides)return false;
        if(!recipient){state.phase=KeyPhase::failed;state.waiting=false;if(state.emitted)key_cleanup_roles|=key_role(raw);return true;}
        if(!state.emitted)state.mapped=recipient->engine->hub.translated_key(raw);
        auto* flight=recipient->engine->reserve_flight();
        if(!flight){state.phase=KeyPhase::failed;state.waiting=false;if(state.emitted)key_cleanup_roles|=key_role(raw);return true;}
        flight->key=raw;flight->instance=state.instance;
        auto& engine=*recipient->engine;engine.sending_key=flight;engine.emitting_user_key=true;
        const auto* previous=engine.executing_auth;engine.executing_auth=input_auth.controller_id?&(command.kind==Kind::KeyEdges?command.input().state.auth:input_auth):nullptr;
        const bool sent=engine.hub.transmit_key(recipient->device,state.mapped,Hub::audio_key(raw));
        engine.executing_auth=previous;engine.sending_key=nullptr;engine.emitting_user_key=false;
        if(sent){state.waiting=true;state.emitted=true;}else{flight->used=false;state.phase=KeyPhase::failed;state.waiting=false;if(state.emitted)key_cleanup_roles|=key_role(raw);}
        return true;
    }


    bool owner_activity_pending() const {
        const auto& root=*this;
        for(uint8_t id=0;id<amity::max_tentacles;++id)
            if(auto* engine=const_cast<Impl&>(root).adapter(id))for(const auto& flight:engine->flights)if(flight.used&&flight.activity&&flight.completion.result.load(std::memory_order_acquire)==cec::TxResult::Pending)return true;
        return false;
    }
    bool key_cleanup_pending()const{
        for(uint8_t id=0;id<amity::max_tentacles;++id)if(auto* engine=const_cast<Impl*>(this)->adapter(id))for(const auto& flight:engine->flights)
            if(flight.used&&flight.cleanup&&flight.completion.result.load(std::memory_order_acquire)==cec::TxResult::Pending)return true;
        return false;
    }
    static bool owner_activity(const system::OwnedIntent& intent) { return intent.activity_step; }
    static bool owner_activity(const Command& command) { return command.kind==Kind::Owned&&owner_activity(command.action().owned); }
    static bool activity_request(const Command& command) {
        return command.kind==Kind::OwnerActivity||command.kind==Kind::Select||command.kind==Kind::Standby||command.kind==Kind::Configure||
            (command.kind==Kind::Owned&&command.action().owned.kind==system::OwnedIntentKind::activity)||
            (command.kind==Kind::Key&&command.value==0x40&&command.activity_started);
    }
    bool queued_owner_activity() const {
        return std::any_of(deferred.begin(),deferred.end(),[](const auto& command){return owner_activity(*command);});
    }
    bool activity_setup_in_progress() const {
        const auto& root=*this;
        return root.activity_requests.load()||root.setup_running.load();
    }
    void refresh_activity_setup() {
        setup_running.store(setup_gates_input&&(control_hub().activity_pending()||step_pending));
    }

    void continue_keys(){
        for(size_t raw=0;raw<keys.size();++raw)if(keys[raw].phase==KeyPhase::down&&!keys[raw].waiting){
            Command command{Kind::KeyRepeat,static_cast<int>(raw)};
            command.key_generation=applied_key_generation;command.expected_selection=keys[raw].instance;
            if(!send_held_key(static_cast<uint8_t>(raw),command)){
                if(xSemaphoreTake(command_slots,0)!=pdTRUE)continue;
                auto pending=std::unique_ptr<Command>(new(std::nothrow)Command(std::move(command)));
                if(!pending){xSemaphoreGive(command_slots);continue;}
                keys[raw].waiting=true;deferred.push_back(std::move(pending));
            }
        }
    }

    bool update_settling() {
        xSemaphoreTake(lock, portMAX_DELAY);
        const bool changed = cached.activity_settling != activity_settling(esp_timer_get_time());
        xSemaphoreGive(lock); return changed;
    }

    void fail_activity(int index, const char *reason) {
        last_activity_error = reason;
        xSemaphoreTake(lock, portMAX_DELAY);
        (void)index;
        xSemaphoreGive(lock);
    }


    bool apply_activity(int index,bool gate_input=true) {
        if(index < -1||index>=static_cast<int>(control_hub().activity_count())){fail_activity(index,"Activity configuration changed; choose the activity again.");return true;}
        const auto ready=[&]{
            if(scoped_mode)activity_hub.release_key();else stop_holds();
            last_activity_error.clear();setup_gates_input=gate_input;setup_running.store(gate_input);setting_activity=true;
        };
        setting_activity=control_hub().activity_pending();
        const bool applied=index<0?control_hub().standby(ready):control_hub().select_activity(index,ready);
        setting_activity=false;
        if(control_hub().pending_resolution()||control_hub().activity_pending()||step_pending)return false;
        if(!applied){fail_activity(index,"Required equipment could not be resolved.");return true;}
        return true;
    }













    bool stale_key(const Command &command) const {
        return command.key_generation != applied_key_generation ||
            command.release_generation != keys[static_cast<uint8_t>(command.value)].release_generation ||
            (command.activity_bound && command.activity_generation != control_hub().activity_generation()) ||
            (command.selection_bound && command.expected_selection != activity_selection.load());
    }
    bool current_received(const Command& command) {
        auto* engine=adapter(command.received().received_adapter);
        return engine&&command.received().intent_generation==intent_generation.load()&&command.received().received_boot==local_boot&&
            command.received().received_generation==engine->received_generation&&
            command.received().received_activity==engine->hub.activity_generation()&&command.received().received_wire==engine->owned_wire_generation.load()&&
            command.received().received_binding==(command.received().received_frame.source()<15?engine->binding_wire_generation[command.received().received_side==BusSide::Front?0:1][command.received().received_frame.source()].load():0)&&
            uint64_t(esp_timer_get_time())<command.received().received_deadline;
    }
    bool execute_key_edges(Command& command){
        const bool native=command.kind==Kind::Key&&!scoped_mode;
        InputState local;
        if(native){local.edge_count=command.repeat?1:2;local.edges[0]={static_cast<uint8_t>(command.value),true};local.edges[1]={static_cast<uint8_t>(command.value),false};}
        const auto& intent=native?local:command.input().state;
        if(!command.input_started){
            const auto expected=intent.edges[0].down?system::OwnedAuthority::input:system::OwnedAuthority::cleanup;
            if(!intent.edge_count||intent.edge_count>2||(!native&&(intent.auth.authority!=expected||!owner_authorized(intent.auth)||intent.owner!=octopus||intent.consortium!=consortium))){finish(command.reply,{system::AdmissionCode::unauthorized,"Original input authority is unavailable"});return true;}
            if(!native)observe_controller_boot(intent.auth.controller_boot);
            if(input_auth.controller_boot!=intent.auth.controller_boot||input_auth.link_id!=intent.auth.link_id){
                stop_holds();xSemaphoreTake(lock,portMAX_DELAY);
                for(uint8_t id=0;id<amity::max_tentacles;++id)if(auto* engine=adapter(id)){engine->received_controller_boot=intent.auth.controller_boot;engine->received_link=intent.auth.link_id;}
                xSemaphoreGive(lock);
            }
            input_auth=intent.auth;input_auth.deadline_ms=0;input_auth.authority=system::OwnedAuthority::input;command.input_started=true;
        }else if(!native&&!owner_authorized(intent.auth,false)){finish(command.reply,{system::AdmissionCode::unauthorized,"Original input authority is unavailable"});return true;}
        if(command.key_generation!=applied_key_generation||(input_activity_generation&&intent.generation!=input_activity_generation)){finish(command.reply,{system::AdmissionCode::accepted,{}});return true;}
        for(;command.edge_cursor<intent.edge_count;++command.edge_cursor){const auto edge=intent.edges[command.edge_cursor];
            if(edge.down){
                if(command.setup_blocked||activity_setup_in_progress())continue;
                if(keys[edge.key].phase!=KeyPhase::up)continue;
                auto recipient=resolve_key_role(Hub::audio_key(edge.key),command);if(command.discovery_sides)return false;
                if(!recipient)continue;
                auto& state=keys[edge.key];state.phase=KeyPhase::down;state.waiting=state.emitted=false;state.mapped=edge.key;++state.instance;
                if(!send_held_key(edge.key,command))return false;
            }else{stop_key(edge.key);Command cleanup_command{Kind::KeyCleanup};clean_keys(cleanup_command);}
        }
        finish(command.reply,{system::AdmissionCode::accepted,{}});return true;
    }
    void finish_pending_down(const InputState& up){
        if(up.edge_count!=1||up.edges[0].down)return;
        for(auto& pending:deferred)if(pending->kind==Kind::KeyEdges&&
            pending->input().state.edge_count==1&&pending->input().state.edges[0].down&&pending->input().state.edges[0].key==up.edges[0].key&&pending->input().state.generation==up.generation){
            pending->input().state.edge_count=2;pending->input().state.edges[1]={up.edges[0].key,false};
        }
    }
    system::OwnedDispatchResult dispatch_key_edges(system::OwnedIntent& intent){
        if(!local_coordinator.load())return true;
        if(!intent.edge_count||intent.edge_count>2)return false;
        const auto first=intent.edges[0];const bool held=pressed_owner[first.key]!=0xff;
        if(first.down&&intent.edge_count==1&&held)return true;
        system::OctopusId owner;
        if(held){const auto slot=pressed_owner[first.key];if(slot<key_owners.size())owner=key_owners[slot];}
        else if(first.down)owner=intent.target.port.octopus_id;
        else return true;
        if(!owner)return false;
        intent.target.port={consortium,owner,0,system::Side::front};intent.target.osd_name.clear();
        auto position=std::find(key_owners.begin(),key_owners.end(),owner);
        const bool inserted=position==key_owners.end();
        if(inserted){if(key_owners.size()==4)return false;key_owners.push_back(owner);position=key_owners.end()-1;}
        const auto slot=static_cast<uint8_t>(position-key_owners.begin());
        if(!intent.edges[intent.edge_count-1].down)pressed_owner[first.key]=0xff;
        bool admitted=false;
        if(owner==octopus){
            system::OwnedAuthorization auth{owned_controller,local_boot,local_boot,{},owned_config_revision,0};auth.authority=first.down?system::OwnedAuthority::input:system::OwnedAuthority::cleanup;
            auto command=std::unique_ptr<Command>(new(std::nothrow)InputCommand(intent,auth));if(!command)return false;
            finish_pending_down(command->input().state);
            command->key_generation=key_generation.load();command->setup_blocked=activity_setup_in_progress();
            if(execute_key_edges(*command))admitted=true;
            else if(xSemaphoreTake(command_slots,0)==pdTRUE){command->input().state.auth.deadline_ms=0;deferred.push_back(std::move(command));admitted=true;}
        }else{
            const bool release=intent.edge_count==1&&!first.down;
            const auto started=esp_timer_get_time();
            admitted=owned_sender&&owned_sender(intent);
            const auto elapsed=static_cast<long long>(esp_timer_get_time()-started);
            if(release){
                if(elapsed>100000)
                    ESP_LOGW(TAG,"Key release stage=owner_send key=%u owner=%s generation=%llu elapsed_us=%lld admitted=%d",static_cast<unsigned>(first.key),owner.str().c_str(),static_cast<unsigned long long>(intent.generation),elapsed,admitted);
                else
                    ESP_LOGI(TAG,"Key release stage=owner_send key=%u owner=%s generation=%llu elapsed_us=%lld admitted=%d",static_cast<unsigned>(first.key),owner.str().c_str(),static_cast<unsigned long long>(intent.generation),elapsed,admitted);
            }else{
                if(elapsed>100000)
                    ESP_LOGW(TAG,"Key transfer stage=owner_send key=%u owner=%s generation=%llu phase=%s edge_count=%u elapsed_us=%lld admitted=%d",static_cast<unsigned>(first.key),owner.str().c_str(),static_cast<unsigned long long>(intent.generation),intent.edge_count==2?"tap":"down",static_cast<unsigned>(intent.edge_count),elapsed,admitted);
                else
                    ESP_LOGI(TAG,"Key transfer stage=owner_send key=%u owner=%s generation=%llu phase=%s edge_count=%u elapsed_us=%lld admitted=%d",static_cast<unsigned>(first.key),owner.str().c_str(),static_cast<unsigned long long>(intent.generation),intent.edge_count==2?"tap":"down",static_cast<unsigned>(intent.edge_count),elapsed,admitted);
            }
        }
        if(admitted)pressed_owner[first.key]=intent.edges[intent.edge_count-1].down?slot:0xff;
        else if(inserted)key_owners.pop_back();
        return admitted;
    }

    bool execute(Command &command) {
        if(command.kind==Kind::KeyEdges)return execute_key_edges(command);
        if(command.kind==Kind::Owned){
            if(command.local_activity_step&&command.action().activity_instance!=activity_instance)return true;
            auto* engine=adapter(command.action().owned.target.port.tentacle_id);
            if(command.owned_waiting){
                if(owner_activity_pending())return false;
                if(!engine||!(command.local_activity_step?local_activity_authorized(command.action().auth,command.action().activity_instance):engine->owner_authorized(command.action().auth,false))){
                    complete_owned(command,{system::AdmissionCode::unauthorized,"Original controller authority is unavailable"});return true;
                }
                if(engine->source_input_next(command.action().owned,command.action().owned_result,command.owned_input_announced)){
                    if(!command.local_activity_step&&command.action().owned.generation!=engine->received_generation){
                        complete_owned(command,{system::AdmissionCode::stale,"Original source input was superseded"});return true;
                    }
                    // The upstream selection has completed (or was dropped).
                    // Continue with the original Back source operation, once.
                    // No frame or failed submission is retained for retry.
                    command.owned_input_announced=true;
                    command.owned_waiting=false;
                    return execute(command);
                }
                complete_owned(command,command.action().owned_result);return true;
            }
            const auto result=engine?engine->execute_owned(command.action().owned,command.action().auth,false,command.owned_input_announced,command.action().activity_instance,command.local_activity_step):
                system::AdmissionResult{system::AdmissionCode::invalid,"Unknown HDMI adapter"};

            if(result.code==system::AdmissionCode::pending){command.discovery_sides=engine->hub.pending_resolution();return false;}
            if(owner_activity(command)&&(owner_activity_pending()||
               engine->source_input_next(command.action().owned,result,command.owned_input_announced))){
                command.action().owned_result=result;command.owned_waiting=true;command.discovery_sides=0;
                return false;
            }
            complete_owned(command,result);return true;
        }
        if(command.kind==Kind::ScopedConfigure){
            if(!activity_hub.configure_scoped(command.configuration().activities)){
                finish(command.reply,{system::AdmissionCode::stale,"The current activity changed before configuration apply"});return true;
            }
        }
        if(command.kind==Kind::MaintenanceBegin){
            if(!command.activity_started){
                if(scoped_mode&&local_coordinator.load())activity_hub.release_key();
                stop_holds();command.activity_started=true;
            }
            if(key_cleanup_roles||key_cleanup_pending())return false;
        }
        if (owned_command(command)) return true;
        switch (command.kind) {
        case Kind::Configure:
            setup_gates_input=true;
            setting_activity=true;
            if (!control_hub().configure(command.configuration().activities,[&]{if(scoped_mode)activity_hub.release_key();else stop_holds();})) {
                setting_activity=false;
                command.discovery_sides=control_hub().pending_resolution();return false;
            }
            setting_activity=false;
            step_pending=false;pending_step.reset();
            break;
        case Kind::Select:
            if(key_cleanup_roles||key_cleanup_pending())return false;
            if(!command.activity_started){++activity_instance;if(command.action().owned.generation)activity_hub.selection_generation(command.action().owned.generation);}
            command.activity_started=true;if(!apply_activity(command.value,command.receipt()!=0)){command.discovery_sides=control_hub().pending_resolution();return false;}
            break;
        case Kind::OwnerActivity:
            if(!accepted_activity_authorized(command.action().auth)||(command.activity_started&&command.action().activity_instance!=activity_instance.load())){
                if(command.activity_started){activity_hub.cancel_activity();step_pending=false;pending_step.reset();}
                last_activity_error="Original controller authority is unavailable";break;
            }
            if(!command.activity_started){stage_owner_selection(command);command.activity_started=true;}
            if(key_cleanup_roles||key_cleanup_pending())return false;
            if(!command.activity_completed){
                if(!apply_activity(command.value,!command.action().owned.repair)){command.discovery_sides=control_hub().pending_resolution();return false;}
                command.activity_completed=true;
            }
            // These contexts are the result of the admitted local recipe,
            // independent of whether its original transport scope still lives.
            owner_context=command.action().owned;
            for(;command.action().context_cursor<amity::max_tentacles;++command.action().context_cursor)if(connector_configured(command.action().context_cursor))if(auto* engine=adapter(command.action().context_cursor)){
                auto context=command.action().owned;context.kind=system::OwnedIntentKind::context;context.activity_step=false;context.source_changed=false;
                context.target.port={consortium,octopus,command.action().context_cursor,system::Side::front};context.target.osd_name.clear();
                engine->hub.execute_owned(context);
                if(engine->hub.pending_resolution()){command.discovery_sides=engine->hub.pending_resolution();return false;}
            }
            break;
        case Kind::Key:
            if(stale_key(command))break;
            if(command.value==0x40){command.activity_started=true;if(!apply_activity(-1)){command.discovery_sides=control_hub().pending_resolution();return false;}break;}
            if(command.setup_blocked||activity_setup_in_progress())break;
            if(scoped_mode){activity_hub.press_key(static_cast<uint8_t>(command.value),command.repeat);break;}
            if(!execute_key_edges(command))return false;
            break;
        case Kind::KeyCleanup:
            if(!clean_keys(command))return false;
            key_cleanup_queued=false;break;
        case Kind::KeyRepeat:{
            auto& state=keys[static_cast<uint8_t>(command.value)];
            if(command.key_generation!=applied_key_generation||state.instance!=command.expected_selection||state.phase!=KeyPhase::down)break;
            if(!send_held_key(static_cast<uint8_t>(command.value),command))return false;
            break;
        }
        case Kind::Release:
            // A completed timed web request belongs to its original activity
            // selection. It must not release a key newly pressed after setup.
            if(command.selection_bound&&(command.activity_generation!=control_hub().activity_generation()||command.expected_selection!=activity_selection.load()||command.key_generation!=key_generation.load()))break;
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
            {Command cleanup_command{Kind::KeyCleanup};clean_keys(cleanup_command);}
            break;
        case Kind::Standby:
            if(key_cleanup_roles||key_cleanup_pending())return false;
            if(!command.activity_started){++activity_instance;if(command.action().owned.generation)activity_hub.selection_generation(command.action().owned.generation);}
            command.activity_started=true;if(!apply_activity(-1,command.receipt()!=0)){command.discovery_sides=control_hub().pending_resolution();return false;}
            break;
        case Kind::Discover: requested_scan_sides = 3; if(scoped_mode)request_owned_discovery(); break;
        case Kind::Received: {
            auto* engine=adapter(command.received().received_adapter);
            if(!current_received(command))break;
            const bool previous=engine->pacing_receive;engine->pacing_receive=true;
            command.discovery_sides=engine->hub.receive(command.received().received_side,command.received().received_frame,command.received().received_step);
            engine->pacing_receive=previous;
            command.received().received_step=engine->hub.receive_step();
            if(command.discovery_sides)return false;
            break;
        }
        default: break; // Handled by remote_command above.
        }
        record_selection(command);
        return true;
    }
    void record_selection(const Command& command){if(command.receipt())processed_selection=command.receipt();}

    std::optional<system::QualifiedDeviceName> selector(const std::string& name)const{
        for(const auto& item:active_selectors)if(item.value==name)return item.target;
        return {};
    }
    void broadcast_selection(Command& command){
        if(command.selection_sent||!scoped_mode||!local_coordinator.load())return;
        command.selection_sent=true;
        const auto state=activity_hub.snapshot();const bool selected=command.kind==Kind::Select;
        const auto next=selected&&command.value>=0&&static_cast<size_t>(command.value)<state.activities.size()?state.activities[command.value]:Activity{};
        shared_generation=std::max(shared_generation,activity_hub.scoped_generation());
        if(command.receipt()||command.kind==Kind::ScopedConfigure)++shared_generation;
        command.action().owned.generation=shared_generation;
        // The main is also an owner. A fresh same-target selection advances
        // input identity even though its local repair keeps the same context.
        input_activity_generation=shared_generation;
        system::OwnedIntent intent;intent.kind=system::OwnedIntentKind::activity;intent.generation=shared_generation;intent.config_revision=owned_config_revision;
        intent.selected=selected;intent.repair=!command.receipt();intent.reselect=command.receipt()&&!command.route_transition;
        intent.source=selector(next.source);intent.audio=selector(next.audio);intent.display=selector(next.display);intent.switch_device=selector(next.switch_device);intent.input=next.switch_input;
        if(command.receipt()){shared_selection=intent;shared_selection_receipt=command.receipt();unadmitted_selection_owners.clear();}
        std::vector<system::OctopusId> sent;
        auto owners=activity_owners;for(const auto& item:active_selectors)if(std::find(owners.begin(),owners.end(),item.target.port.octopus_id)==owners.end())owners.push_back(item.target.port.octopus_id);
        for(const auto owner:owners){
            if(owner==octopus||std::find(sent.begin(),sent.end(),owner)!=sent.end())continue;
            sent.push_back(owner);intent.target.port={consortium,owner,0,system::Side::front};
            const bool admitted=owned_sender&&owned_sender(intent);
            if(admitted&&!intent.repair)unadmitted_selection_owners.erase(std::remove(unadmitted_selection_owners.begin(),unadmitted_selection_owners.end(),owner),unadmitted_selection_owners.end());
            else if(command.receipt()){
                unadmitted_selection_owners.push_back(owner);
                ESP_LOGW(TAG,"Activity delivery not admitted owner=%s generation=%llu",owner.str().c_str(),static_cast<unsigned long long>(intent.generation));
            }
        }
    }
    bool admit_unadmitted_selection(system::OctopusId owner){
        if(!shared_selection||shared_selection_receipt!=activity_selection.load())return false;
        auto selection=*shared_selection;selection.config_revision=owned_config_revision;
        selection.target.port={consortium,owner,0,system::Side::front};selection.target.osd_name.clear();
        if(!owned_sender||!owned_sender(selection))return false;
        unadmitted_selection_owners.erase(std::remove(unadmitted_selection_owners.begin(),unadmitted_selection_owners.end(),owner),unadmitted_selection_owners.end());
        return true;
    }
    void retry_unadmitted_selection(){
        if(unadmitted_selection_owners.empty()||!scoped_mode||!local_coordinator.load()||authority_transition.load()||maintenance.load()||retiring.load())return;
        // One nonblocking admission per owner turn; a full owner FIFO must not
        // require a new link-ready callback or delay another owner's progress.
        const auto owner=unadmitted_selection_owners.front();
        if(!admit_unadmitted_selection(owner))std::rotate(unadmitted_selection_owners.begin(),unadmitted_selection_owners.begin()+1,unadmitted_selection_owners.end());
    }
    bool refresh_selection_context(system::OctopusId owner){
        if(!shared_selection||owner==octopus)return activity_hub.refresh_owned_context(owner);
        if(std::find(activity_owners.begin(),activity_owners.end(),owner)==activity_owners.end()&&
           std::none_of(active_selectors.begin(),active_selectors.end(),[&](const auto& item){return item.target.port.octopus_id==owner;}))return true;
        // A newer accepted selection is queued. Its broadcast must establish
        // context; recapture must never resurrect the superseded selection.
        if(shared_selection_receipt!=activity_selection.load())return true;
        return admit_unadmitted_selection(owner);
    }
    void stage_owner_selection(Command& command){
        command.action().activity_instance=activity_instance.fetch_add(1)+1;
        const auto& intent=command.action().owned;std::vector<system::ScopedSelector> selectors;
        activity_auth=command.action().auth;activity_auth.deadline_ms=0;
        for(uint8_t id=0;id<amity::max_tentacles;++id)if(auto* engine=adapter(id)){
            engine->received_generation=intent.generation;
        }
        const auto before=activity_hub.snapshot();
        if(before.current_activity>=0&&static_cast<size_t>(before.current_activity)<before.activities.size()){
            const auto& previous=before.activities[before.current_activity];
            for(const auto& name:{previous.display,previous.source,previous.audio,previous.switch_device})
                for(const auto& entry:active_selectors)if(entry.value==name&&std::none_of(selectors.begin(),selectors.end(),[&](const auto& old){return old.value==entry.value;}))selectors.push_back(entry);
        }
        const auto value=[&](const std::optional<system::QualifiedDeviceName>& ref){
            if(!ref)return std::string{};
            const auto key=ref->port.octopus_id.str()+":"+std::to_string(ref->port.tentacle_id)+":"+std::to_string(static_cast<unsigned>(ref->port.side))+":"+ref->osd_name;
            if(std::none_of(selectors.begin(),selectors.end(),[&](const auto& old){return old.value==key;}))selectors.push_back({key,*ref});
            return key;
        };
        Activity next;next.display=value(intent.display);next.source=value(intent.source);next.audio=value(intent.audio);next.switch_device=value(intent.switch_device);next.switch_input=intent.input;
        const bool selected=intent.selected;
        if(!scoped_mode){scoped_mode=true;activity_hub.scoped_transport({},[this](auto& step){return dispatch_owned(step);},[this](const auto& name){return lookup_owned(name);},system::CecPortRef{consortium,octopus,0,system::Side::front});}
        activity_hub.scoped_owner(octopus);std::vector<system::CecPortRef> ports;
        for(uint8_t id=0;id<amity::max_tentacles;++id)if(connector_configured(id))ports.push_back({consortium,octopus,id,system::Side::front});
        activity_hub.scoped_ports(std::move(ports));active_selectors=selectors;projection_dirty=true;
        for(uint8_t id=0;id<amity::max_tentacles;++id)if(auto* engine=adapter(id))engine->hub.qualified_names();
        command.value=activity_hub.stage_activity(std::move(next),std::move(selectors),selected,intent.generation,intent.reselect||intent.repair);
    }

    void accept(Command *raw) {
        std::unique_ptr<Command> command(raw);
        if(command->reply&&command->reply->canceled.load()&&command->kind!=Kind::SettingsCommit){free_reservation(*command);return;}
        if(command->kind==Kind::Owned&&command->action().owned.kind==system::OwnedIntentKind::activity){
            const auto& intent=command->action().owned;
            if(command->action().auth.authority!=system::OwnedAuthority::control||!owner_authorized(command->action().auth)||
               intent.target.port.octopus_id!=octopus||intent.target.port.consortium_id!=consortium){finish(command->reply,{system::AdmissionCode::unauthorized,"Original controller authority is unavailable"});free_reservation(*command);return;}
            if(!intent.generation){finish(command->reply,{system::AdmissionCode::invalid,"Missing original activity generation"});free_reservation(*command);return;}
            observe_controller_boot(command->action().auth.controller_boot);
            if(intent.repair&&accepted_selection&&intent.generation<accepted_selection->generation){
                finish(command->reply,{system::AdmissionCode::stale,"Original activity repair was superseded"});free_reservation(*command);return;
            }
            if(!intent.repair&&accepted_selection){
                if(intent.generation<=accepted_selection->generation){
                    const bool duplicate=intent.generation==accepted_selection->generation&&same_selection(intent,*accepted_selection);
                    if(duplicate)refresh_input_authority(command->action().auth);
                    finish(command->reply,duplicate?system::AdmissionResult{system::AdmissionCode::accepted,{}}:
                        system::AdmissionResult{system::AdmissionCode::stale,"Original activity was superseded or changed"});
                    free_reservation(*command);return;
                }
            }
            if(!intent.repair){
                accepted_selection=intent;
                command->action().activity_receipt=activity_selection.fetch_add(1)+1;
                command->selection_reservation=true;++activity_requests;
            }
            key_generation.fetch_add(1);apply_emergency_release();input_activity_generation=command->action().owned.generation;
            refresh_input_authority(command->action().auth);
            command->kind=Kind::OwnerActivity;finish(command->reply,{system::AdmissionCode::accepted,{}});command->reply.reset();
        }
        if(command->kind==Kind::Select||command->kind==Kind::Standby){apply_emergency_release();broadcast_selection(*command);}
        if(command->kind==Kind::KeyEdges){
            finish_pending_down(command->input().state);
            if(execute(*command)){free_reservation(*command);return;}
            finish(command->reply,{system::AdmissionCode::pending,"Original owner lookup is in progress"});command->input().state.auth.deadline_ms=0;deferred.push_back(std::move(command));return;
        }
        if(command->kind==Kind::Owned){
            auto* engine=adapter(command->action().owned.target.port.tentacle_id);
            if(command->action().owned.kind==system::OwnedIntentKind::context&&(control_hub().activity_pending()||activity_requests.load())){
                const auto result=engine?engine->execute_owned(command->action().owned,command->action().auth,true):system::AdmissionResult{system::AdmissionCode::invalid,"Unknown HDMI adapter"};
                if(!result){free_reservation(*command);finish(command->reply,result);return;}
                finish(command->reply,{system::AdmissionCode::pending,"Original owner activity is in progress"});command->owned_deferred=true;command->action().auth.deadline_ms=0;deferred.push_back(std::move(command));return;
            }
            if(owner_activity(*command)&&(owner_activity_pending()||queued_owner_activity())){
                const auto result=engine?engine->execute_owned(command->action().owned,command->action().auth,true,false,command->action().activity_instance,command->local_activity_step):
                    system::AdmissionResult{system::AdmissionCode::invalid,"Unknown HDMI adapter"};
                if(!result){free_reservation(*command);complete_owned(*command,result);return;}

            }else if(execute(*command)){free_reservation(*command);return;}
            if(!command->owned_deferred)finish(command->reply,{system::AdmissionCode::pending,"Original owner action is in progress"});
            command->owned_deferred=true;
            if(command->discovery_sides||command->owned_waiting)command->action().auth.deadline_ms=0;
            deferred.push_back(std::move(command));return;
        }
        if(command->kind==Kind::OwnedBind&&command->management().controller==owned_controller&&(owner_activity_pending()||owner_activity_commands.load()||hub.activity_pending())){
            deferred.push_back(std::move(command));return;
        }
        const bool earlier_activity=std::any_of(deferred.begin(),deferred.end(),[](const auto& pending){return activity_request(*pending);});
        if(activity_request(*command)&&(earlier_activity||control_hub().activity_pending()||
            ((command->kind==Kind::Select||command->kind==Kind::Standby)&&
             step_pending))){
            deferred.push_back(std::move(command));return;
        }
        if(!execute(*command)){deferred.push_back(std::move(command));return;}
        free_reservation(*command);
    }

    bool resume_discovered() {
        bool changed = false,owner_blocked=false,activity_blocked=false,lookup_blocked=false;
        for (auto i = deferred.begin(); i != deferred.end();) {
            if((*i)->activity_canceled){free_reservation(**i);i=deferred.erase(i);changed=true;continue;}
            if((*i)->kind==Kind::Owned&&(*i)->local_activity_step&&(*i)->action().activity_instance!=activity_instance){
                free_reservation(**i);i=deferred.erase(i);changed=true;continue;
            }
            if((*i)->kind==Kind::Received&&!current_received(**i)){
                free_reservation(**i);i=deferred.erase(i);changed=true;continue;
            }
            // Only callers which actually waited for find_device() join this
            // order. A ready lookup cannot pass an earlier pending lookup on
            // another side; cached commands still execute immediately.
            const bool lookup_waiter=((*i)->discovery_sides&3u)!=0;
            if(lookup_waiter&&lookup_blocked){++i;continue;}
            if(lookup_waiter)lookup_blocked=true;
            const bool owner_step=owner_activity(**i),activity=activity_request(**i);
            if(owner_step&&owner_blocked){++i;continue;}
            if(activity&&activity_blocked){++i;continue;}
            if(owner_step)owner_blocked=true;
            if(activity)activity_blocked=true;
            if((*i)->kind==Kind::Owned){
                auto* engine=adapter((*i)->action().owned.target.port.tentacle_id);
                if((*i)->action().owned.kind==system::OwnedIntentKind::context&&(control_hub().activity_pending()||activity_requests.load())){++i;continue;}
                if(owner_step&&!(*i)->owned_waiting&&owner_activity_pending()){++i;continue;}
                if(engine&&(engine->pending_scan_sides()&(*i)->discovery_sides)){++i;continue;}
            }else if((*i)->kind==Kind::KeyEdges||(*i)->kind==Kind::KeyCleanup||(*i)->kind==Kind::KeyRepeat||(*i)->kind==Kind::Key){
                auto* engine=adapter((*i)->lookup_adapter);if(engine&&(engine->pending_scan_sides()&(*i)->discovery_sides)){++i;continue;}
            }else if((*i)->kind==Kind::Received){
                auto* engine=adapter((*i)->received().received_adapter);
                if(engine&&(engine->pending_scan_sides()&(*i)->discovery_sides)){++i;continue;}
            }else if((*i)->kind==Kind::OwnedBind){
                if((*i)->management().controller==owned_controller&&(owner_activity_pending()||owner_activity_commands.load()||hub.activity_pending())){++i;continue;}
            }else if(activity){
                if(((*i)->kind==Kind::Select||(*i)->kind==Kind::Standby)&&
                   step_pending&&!control_hub().activity_pending()){++i;continue;}
                if(!scoped_mode&&(pending_scan_sides()&(*i)->discovery_sides)){++i;continue;}
            }else if(!scoped_mode&&(pending_scan_sides()&(*i)->discovery_sides)){++i;continue;}
            auto command = std::move(*i);
            i = deferred.erase(i);
            if(!execute(*command)){
                if(lookup_waiter&&!(command->discovery_sides&3u))lookup_blocked=false;
                // Keep the semantic program counter in arrival order. Keys
                // and received frames still advance independently past it.
                i=deferred.insert(i,std::move(command));++i;continue;
            }
            free_reservation(*command);
            if(lookup_waiter)lookup_blocked=false;
            if(owner_step)owner_blocked=false;
            if(activity)activity_blocked=false;
            changed = true;
        }
        return changed;
    }

    static bool same_selection(const system::OwnedIntent& a,const system::OwnedIntent& b){
        return a.selected==b.selected&&a.reselect==b.reselect&&a.source==b.source&&a.audio==b.audio&&a.display==b.display&&
            a.switch_device==b.switch_device&&a.input==b.input;
    }
    void cancel_owner_activity(){
        ++activity_instance;activity_hub.cancel_activity();step_pending=false;pending_step.reset();
        for(auto& command:deferred)if(activity_request(*command)||command->local_activity_step)command->activity_canceled=true;
    }
    bool purge_canceled_activity(){
        bool changed=false;
        for(auto i=deferred.begin();i!=deferred.end();)if((*i)->activity_canceled){
            finish((*i)->reply,{system::AdmissionCode::unauthorized,"Original activity authority changed"});
            free_reservation(**i);i=deferred.erase(i);changed=true;
        }else ++i;
        return changed;
    }
    void observe_controller_boot(system::BootId boot){
        if(accepted_controller_boot&&accepted_controller_boot!=boot){
            cancel_owner_activity();accepted_selection.reset();key_generation.fetch_add(1);apply_emergency_release();
        }
        accepted_controller_boot=boot;
    }
    void refresh_input_authority(const system::OwnedAuthorization& auth){
        input_auth=auth;input_auth.deadline_ms=0;input_auth.authority=system::OwnedAuthority::input;
        xSemaphoreTake(lock,portMAX_DELAY);
        for(uint8_t id=0;id<amity::max_tentacles;++id)if(auto* engine=adapter(id)){
            engine->received_controller_boot=auth.controller_boot;engine->received_link=auth.link_id;
        }
        xSemaphoreGive(lock);
    }
    bool accepted_activity_authorized(const system::OwnedAuthorization& auth)const{
        return !maintenance.load()&&!retiring.load()&&!authority_transition.load()&&
            auth.controller_id==owned_controller&&auth.owner_boot==local_boot&&auth.config_revision==owned_config_revision&&
            (local_coordinator.load()?auth.controller_boot==local_boot:auth.controller_boot==accepted_controller_boot);
    }
    std::optional<Device> lookup_owned_locked(const system::QualifiedDeviceName& name) const {
        for(const auto& report:owned_reports)if(report.owner==name.port.octopus_id&&report.adapter==name.port.tentacle_id){
            if(!name.osd_name.empty())for(const auto& device:report.devices)if(device.side==name.port.side&&device.osd_name==name.osd_name)
                return Device{name.osd_name,name.port.side==system::Side::front?BusSide::Front:BusSide::Back,
                    device.logical_address,device.physical_address,device.device_type,device.vendor_id,true};
        }
        return {};
    }
    std::optional<Device> lookup_owned(const system::QualifiedDeviceName& name) const {
        xSemaphoreTake(lock,portMAX_DELAY);auto device=lookup_owned_locked(name);xSemaphoreGive(lock);return device;
    }
    bool action_result(const StepResult& result){
        if(!step_pending||!pending_step)return false;
        const auto& expected=pending_step->request;const auto& actual=result.request;
        if(expected.kind!=actual.kind||expected.target!=actual.target||expected.generation!=actual.generation)return false;
        pending_step=result;step_pending=false;
        using C=system::AdmissionCode;
        switch(result.result.code){
        case C::invalid:case C::stale:case C::expired:case C::unauthorized:case C::unsupported:
            last_activity_error=result.result.reason.empty()?"An Amity could not complete the activity command":result.result.reason;
            break;
        default:break;
        }
        activity_hub.owned_step_result(actual,result.result);
        return true;
    }
    system::OwnedDispatchResult dispatch_owned(system::OwnedIntent& intent) {
        using K=system::OwnedIntentKind;
        intent.config_revision=owned_config_revision;
        intent.activity_step=setting_activity&&intent.kind!=K::key_edges&&intent.kind!=K::context&&intent.kind!=K::discover;
        if(intent.kind==K::key_edges)return dispatch_key_edges(intent);
        if(intent.activity_step){
            if(step_pending)return false;
            pending_step=StepResult{intent,{system::AdmissionCode::pending,{}}};step_pending=true;
        }
        const auto dispatched=[&](bool admitted,std::optional<bool> present={},bool pending=false){
            if(!admitted&&intent.activity_step){step_pending=false;pending_step.reset();}
            return system::OwnedDispatchResult{admitted,present,pending};
        };
        if(intent.target.port.octopus_id==octopus){
            auto* engine=adapter(intent.target.port.tentacle_id);if(!engine)return dispatched(false);
            system::OwnedAuthorization auth=activity_auth;
            if(local_coordinator.load()){auth.controller_id=owned_controller;auth.controller_boot=auth.owner_boot=local_boot;auth.config_revision=owned_config_revision;auth.link_id={};}
            auth.authority=system::OwnedAuthority::control;
            if(intent.activity_step){
                auto* request=new(std::nothrow)ActionCommand{Kind::Owned};if(!request)return dispatched(false);
                request->action().owned=intent;request->action().auth=auth;request->action().activity_instance=activity_instance.load();request->local_activity_step=true;return dispatched(submit(request)==ESP_OK,{},true);
            }
            const auto result=engine->execute_owned(intent,auth);
            if(result.code==system::AdmissionCode::pending){
                auto* request=new(std::nothrow)ActionCommand{Kind::Owned};if(!request)return false;
                request->action().owned=intent;request->action().auth=auth;return dispatched(submit(request)==ESP_OK,{},true);
            }
            const bool admitted=result.code==system::AdmissionCode::accepted||result.code==system::AdmissionCode::overloaded||result.code==system::AdmissionCode::unresolved;
            return dispatched(admitted,result.source_present);
        }
        // Distribution carries selections and input edges. Original recipe
        // steps execute only on this owner's adapters.
        if(intent.activity_step||intent.kind==K::power||intent.kind==K::source_input||intent.kind==K::switch_input||intent.kind==K::announce_active_source||intent.kind==K::query_audio||intent.kind==K::standby_wire)return dispatched(false);
        const bool admitted=owned_sender&&owned_sender(intent);
        return dispatched(admitted);
    }
    bool activity_settling(uint64_t){return control_hub().activity_pending()||step_pending||owner_activity_pending()||key_cleanup_roles;}
    void request_owned_discovery(){
        std::vector<system::CecPortRef> ports;
        for(const auto& selector:active_selectors){auto port=selector.target.port;port.side=system::Side::front;
            if(std::find(ports.begin(),ports.end(),port)!=ports.end())continue;
            ports.push_back(port);system::OwnedIntent intent;intent.kind=system::OwnedIntentKind::discover;
            intent.target.port=port;intent.generation=control_hub().scoped_generation();dispatch_owned(intent);
        }
    }
    void save_report(system::OwnedAdapterSnapshot report){
        auto old=std::find_if(owned_reports.begin(),owned_reports.end(),[&](const auto& r){return r.owner==report.owner&&r.adapter==report.adapter;});
        if(old==owned_reports.end())owned_reports.push_back(std::move(report));else *old=std::move(report);
    }
    bool receive_reports(){
        xSemaphoreTake(lock,portMAX_DELAY);
        const bool changed=reports_dirty||!pending_reports.empty();
        reports_dirty=changed;
        for(auto& report:pending_reports)save_report(std::move(report));
        pending_reports.clear();
        xSemaphoreGive(lock);
        return changed;
    }
    void publish(){
        std::vector<system::OwnedAdapterSnapshot> changed_reports;
        Snapshot notification;
        xSemaphoreTake(lock,portMAX_DELAY);
        bool observations_changed=reports_dirty;reports_dirty=false;
        for(uint8_t id=0;id<amity::max_tentacles;++id)if(auto* engine=adapter(id))if(connector_configured(id)){
            auto found=std::find_if(owned_reports.begin(),owned_reports.end(),[&](const auto& report){return report.owner==octopus&&report.adapter==id;});
            const bool added=found==owned_reports.end();
            if(added){owned_reports.emplace_back();found=owned_reports.end()-1;found->owner=octopus;found->boot=local_boot;found->adapter=id;}
            bool changed=engine->hub.update_observation(*found)||added;
            if(found->discovering!=engine->scan_running||found->discovery_generation!=engine->discovery_generation){
                found->discovering=engine->scan_running;found->discovery_generation=engine->discovery_generation;changed=true;
            }
            if(changed){observations_changed=true;changed_reports.push_back(*found);}
        }
        const bool configuration_changed=cached.applied_config_revision!=owned_config_revision||cached.applied_presentation_revision!=owned_presentation_revision;
        bool changed=control_hub().update_snapshot(cached,!scoped_mode);
        const auto assign=[&](auto& previous,const auto& value){if(!(previous==value)){previous=value;changed=true;}};
        bool discovering=false;uint32_t discovery_generation=0,tx_errors=0,rx_dropped=0;
        for(uint8_t id=0;id<amity::max_tentacles;++id)if(auto* engine=adapter(id))if(connector_configured(id)){
            discovering=discovering||engine->scan_running;discovery_generation+=engine->discovery_generation;
            const auto front_stats=engine->front.statistics(),back_stats=engine->back.statistics();
            tx_errors+=engine->dropped_tx+front_stats.tx_errors+back_stats.tx_errors;
            rx_dropped+=front_stats.rx_dropped+back_stats.rx_dropped;
        }
        assign(cached.discovering,discovering);assign(cached.discovery_generation,discovery_generation);
        assign(cached.tx_errors,tx_errors);assign(cached.rx_dropped,rx_dropped);
        if(scoped_mode){
            assign(cached.ready,std::any_of(owned_reports.begin(),owned_reports.end(),[](const auto& report){return report.ready;}));
            if(observations_changed||configuration_changed||projection_dirty){
                projection_dirty=false;
                std::vector<Device> devices;
                for(const auto& selector:active_selectors)if(auto device=lookup_owned_locked(selector.target)){device->name=selector.value;devices.push_back(std::move(*device));}
                if(devices!=cached.devices){cached.devices=std::move(devices);changed=true;}
            }
            if(cached.current_activity>=0&&static_cast<size_t>(cached.current_activity)<cached.activities.size()){
                const auto& activity=cached.activities[cached.current_activity];
                const auto& audio=Hub::audio_recipient(activity.audio,activity.display);
                for(const auto& selector:active_selectors)if(selector.value==audio)for(const auto& report:owned_reports)if(report.owner==selector.target.port.octopus_id&&report.adapter==selector.target.port.tentacle_id)
                    activity_hub.scoped_audio_status(report.audio_status_known,report.muted,report.volume);
            }
            // The activity Hub owns fallback/Standby audio state; copying only
            // these scalars also preserves it when no reported recipient exists.
            const auto audio=activity_hub.audio_status();
            assign(cached.audio_status_known,audio.known);assign(cached.muted,audio.muted);assign(cached.volume,audio.volume);
        }
        assign(cached.activity_settling,activity_settling(esp_timer_get_time()));assign(cached.activity_error,last_activity_error);
        assign(cached.applied_config_revision,owned_config_revision.load());assign(cached.applied_presentation_revision,owned_presentation_revision);
        assign(cached.processed_activity_selection,processed_selection);cached_revision=owned_config_revision;
        if(scoped_publication_pending){scoped_publication_pending=false;--scoped_configurations;}
        for(uint8_t id=0;id<amity::max_tentacles;++id){auto& a=cached_adapters[id];a.tentacle_id=id;a.configured=connector_configured(id);a.observed_front_physical.reset();a.effective_front_physical.reset();
            if(a.configured)if(auto* engine=adapter(id)){const auto pa=engine->hub.front_physical_address();if(pa!=0xffff)a.effective_front_physical=pa;a.observed_front_physical=engine->hub.detected_front_physical_address();}
        }
        auto observer=std::atomic_load(&observer_slot);auto cb=changed?callback:decltype(callback){};
        if(cb)notification=cached;
        xSemaphoreGive(lock);
        if(observer)for(const auto& report:changed_reports)(*observer)(report);
        if(cb)cb(notification);
    }
    void complete_owned(Command& command,const system::AdmissionResult& result){
        if(command.local_activity_step){
            if(command.action().activity_instance!=activity_instance)return;
            if(!local_activity_authorized(command.action().auth,command.action().activity_instance))return;
            if(command.action().owned.kind==system::OwnedIntentKind::source_input&&result.code==system::AdmissionCode::accepted&&result.source_present.value_or(false)){
                // The source step has completed its actual CEC transmission.
                // Commit this owner's original next context before causal RX.
                if(auto* engine=adapter(command.action().owned.target.port.tentacle_id)){
                    auto context=command.action().owned;context.kind=system::OwnedIntentKind::context;context.activity_step=false;engine->hub.execute_owned(context);
                }
            }
            action_result({command.action().owned,result});
        }
        if(!command.owned_deferred)finish(command.reply,result);
    }
    void refresh_configuration_revision(system::OctopusId controller,uint64_t revision){
        const auto previous=owned_config_revision.load();
        if(owned_controller!=controller){cancel_owner_activity();accepted_selection.reset();accepted_controller_boot={};}
        if(owned_controller==controller){
            const auto refresh=[&](system::OwnedAuthorization& auth){if(auth.controller_id==controller&&auth.config_revision==previous)auth.config_revision=revision;};
            refresh(input_auth);refresh(activity_auth);
            if(owner_context)owner_context->config_revision=revision;
            if(shared_selection)shared_selection->config_revision=revision;
            if(accepted_selection)accepted_selection->config_revision=revision;
            for(auto& pending:deferred)if(pending->kind==Kind::KeyEdges)refresh(pending->input().state.auth);
        }
        xSemaphoreTake(lock,portMAX_DELAY);owned_controller=controller;owned_config_revision=revision;xSemaphoreGive(lock);
    }
    bool owned_command(Command& command){
        using K=Kind;
        if(command.reply&&command.reply->canceled.load()&&command.kind!=K::SettingsCommit)return true;
        switch(command.kind){
        case K::ScopedConfigure:
            // Unchanged current intent keeps its owner-local input and source
            // context while unrelated saved rows and their revision advance.
            if(!scoped_mode){if(local_coordinator.load())pressed_owner.assign(256,0xff);else pressed_owner.clear();key_owners.clear();}
            scoped_mode=true;legacy_enabled=true;
            refresh_configuration_revision(command.management().controller,command.management().config_revision);
            owned_sender=std::move(command.configuration().sender);active_selectors=command.configuration().selectors;projection_dirty=true;
            activity_owners=std::move(command.configuration().owners);
            unadmitted_selection_owners.erase(std::remove_if(unadmitted_selection_owners.begin(),unadmitted_selection_owners.end(),[&](const auto owner){
                return std::find(activity_owners.begin(),activity_owners.end(),owner)==activity_owners.end()&&
                    std::none_of(active_selectors.begin(),active_selectors.end(),[&](const auto& item){return item.target.port.octopus_id==owner;});
            }),unadmitted_selection_owners.end());
            owned_presentation_revision=command.management().presentation_revision;
            shared_generation=std::max(shared_generation,activity_hub.scoped_generation());
            // Discovery belongs to the physical owner/adapter, independently
            // of which saved activities currently reference its equipment.
            activity_hub.scoped_transport(command.configuration().selectors,[this](auto& intent){return dispatch_owned(intent);},[this](const auto& name){return lookup_owned(name);},system::CecPortRef{consortium,octopus,0,system::Side::front},false);
            activity_hub.scoped_owner(octopus);
            {std::vector<system::CecPortRef> ports;for(uint8_t id=0;id<amity::max_tentacles;++id)if(connector_configured(id))ports.push_back({consortium,octopus,id,system::Side::front});activity_hub.scoped_ports(std::move(ports));}
            for(uint8_t id=0;id<amity::max_tentacles;++id)if(auto* engine=adapter(id))engine->hub.qualified_names();
            record_selection(command);
            command.configuration_applied=true;scoped_publication_pending=true;
            finish(command.reply,{system::AdmissionCode::accepted,{}});return true;
        case K::OwnedDiscover:{system::OwnedIntent intent;intent.kind=system::OwnedIntentKind::discover;
            intent.target.port={consortium,command.management().controller,static_cast<uint8_t>(command.value),system::Side::front};
            intent.generation=control_hub().scoped_generation();dispatch_owned(intent);return true;}
        case K::OwnedContext:{
            const bool current=command.management().config_revision==owned_config_revision;
            const bool published=current&&refresh_selection_context(command.management().controller);
            if(command.management().context_completion)command.management().context_completion(!current?system::OwnedContextResult::stale:published?system::OwnedContextResult::published:system::OwnedContextResult::failed);
            finish(command.reply,published?system::AdmissionResult{system::AdmissionCode::accepted,{}}:
                system::AdmissionResult{system::AdmissionCode::overloaded,"Current equipment context could not enter the owner transport"});
            return true;}
        case K::OwnedBind:
            if(owned_controller!=command.management().controller)for(uint8_t id=0;id<amity::max_tentacles;++id)if(auto* engine=adapter(id)){engine->owned_wire_generation.fetch_add(1);engine->stop_holds();engine->received_generation=0;}
            refresh_configuration_revision(command.management().controller,command.management().config_revision);legacy_enabled=true;
            for(uint8_t id=0;id<amity::max_tentacles;++id)if(auto* engine=adapter(id))engine->hub.qualified_names();
            finish(command.reply,{system::AdmissionCode::accepted,{}});return true;
        case K::OwnedFence:
            if(step_pending&&pending_step&&pending_step->request.target.port.octopus_id!=octopus&&(!command.management().controller||pending_step->request.target.port.octopus_id==command.management().controller)){
                last_activity_error="The equipment owner disconnected before activity setup completed";
                action_result({pending_step->request,{system::AdmissionCode::unauthorized,last_activity_error}});
            }
            if(!command.management().controller||command.management().controller==owned_controller)for(uint8_t id=0;id<amity::max_tentacles;++id)if(auto* engine=adapter(id))if(!command.management().link||command.management().link==engine->received_link){engine->stop_holds();}
            finish(command.reply,{system::AdmissionCode::accepted,{}});return true;
        case K::AuthorityTransition:
            cancel_owner_activity();accepted_selection.reset();accepted_controller_boot={};
            if(!command.repeat)unadmitted_selection_owners.clear();
            activity_hub.release_key();for(uint8_t id=0;id<amity::max_tentacles;++id)if(auto* engine=adapter(id))engine->stop_holds();
            local_coordinator.store(command.repeat);coordinator_epoch=command.management().config_revision;authority_transition.store(false);
            finish(command.reply,{system::AdmissionCode::accepted,{}});return true;
        case K::Retire:
            cancel_owner_activity();accepted_selection.reset();accepted_controller_boot={};
            unadmitted_selection_owners.clear();
            activity_hub.release_key();legacy_enabled=false;for(uint8_t id=0;id<amity::max_tentacles;++id)if(auto* engine=adapter(id))engine->stop_holds();passive_ports();
            finish(command.reply,{system::AdmissionCode::accepted,{}});return true;
        case K::MaintenanceBegin:
            cancel_owner_activity();accepted_selection.reset();
            maintenance_started=true;maintenance_applied.store(wires_idle());
            finish(command.reply,{system::AdmissionCode::accepted,{}});return true;
        case K::MaintenanceEnd:maintenance_started=false;maintenance.store(false);maintenance_applied.store(false);finish(command.reply,{system::AdmissionCode::accepted,{}});return true;
        case K::SettingsCommit:
            if(pending_settings||maintenance_started||activity_setup_in_progress()||control_hub().activity_pending()||step_pending||
               control_hub().current_activity()>=0||any_keys_active()){command.reply->settings_result=ESP_ERR_INVALID_STATE;settings_gate.store(false);finish(command.reply,{system::AdmissionCode::overloaded,"Stop the activity before changing settings"});return true;}
            pending_settings=command.reply;settings_boot=command.management().settings_boot;settings_adapters=command.management().settings_adapters;settings_deadline=command.management().settings_deadline;settings_persist=std::move(command.management().persist);settings_paused=true;maintenance_started=true;return true;
        default:return false;
        }
    }
    static void run(void* context){
        auto& p=*static_cast<Impl*>(context);
        if(!p.owner_started){p.owner_started=true;if(p.local_coordinator.load()&&(!p.consortium||!p.octopus))p.legacy_enabled=true;else p.passive_ports();}
        for(;;){
            if(p.pending_settings)p.complete_settings(uint64_t(esp_timer_get_time())/1000);
            if(p.maintenance_started)p.maintenance_applied.store(p.wires_idle());
            bool changed=p.receive_reports(),pending=false;
            changed=p.purge_canceled_activity()||changed;
            if(p.legacy_enabled&&!p.maintenance_started&&!p.retiring.load()){
                p.apply_emergency_release();p.schedule_key_cleanup();
                // A reply may already be waiting when its preceding TX
                // completes. Resume that original activity step first, so RX
                // observes the activity established by the completed command.
                for(uint8_t id=0;id<amity::max_tentacles;++id)if(auto* engine=p.adapter(id))
                    changed=engine->complete_transmissions()||changed;
                changed=p.resume_discovered()||changed;
                p.refresh_audio_hold();
                for(uint8_t id=0;id<amity::max_tentacles;++id)if(auto* engine=p.adapter(id))if(p.connector_configured(id)){
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
                if(command->input_admitted_us){
                    const auto elapsed=static_cast<long long>(esp_timer_get_time()-command->input_admitted_us);
                    if(command->kind==Kind::Release){
                        if(elapsed>100000)
                            ESP_LOGW(TAG,"Key release stage=dequeue key=%u generation=%u elapsed_us=%lld",static_cast<unsigned>(command->value),static_cast<unsigned>(p.control_hub().activity_generation()),elapsed);
                        else
                            ESP_LOGI(TAG,"Key release stage=dequeue key=%u generation=%u elapsed_us=%lld",static_cast<unsigned>(command->value),static_cast<unsigned>(p.control_hub().activity_generation()),elapsed);
                    }else{
                        if(elapsed>100000)
                            ESP_LOGW(TAG,"Key transfer stage=dequeue key=%u generation=%u phase=%s edge_count=%u elapsed_us=%lld",static_cast<unsigned>(command->value),static_cast<unsigned>(p.control_hub().activity_generation()),command->repeat?"down":"tap",command->repeat?1u:2u,elapsed);
                        else
                            ESP_LOGI(TAG,"Key transfer stage=dequeue key=%u generation=%u phase=%s edge_count=%u elapsed_us=%lld",static_cast<unsigned>(command->value),static_cast<unsigned>(p.control_hub().activity_generation()),command->repeat?"down":"tap",command->repeat?1u:2u,elapsed);
                    }
                }
                const bool initial_configuration=command->kind==Kind::ScopedConfigure||
                    (command->kind==Kind::Configure&&p.local_coordinator.load());
                if((p.legacy_enabled||initial_configuration)&&!p.maintenance_started&&!p.retiring.load()){
                    if(initial_configuration)p.legacy_enabled=true;
                    p.accept(command);
                }
                else{p.free_reservation(*command);if(!p.owned_command(*command)&&command->kind==Kind::Configure&&p.local_coordinator.load()&&!p.maintenance_started){p.legacy_enabled=true;p.hub.configure(std::move(command->configuration().activities));}delete command;}
                changed=true;
            }
            for(uint8_t id=0;id<amity::max_tentacles;++id)if(auto* engine=p.adapter(id)){
                changed=engine->complete_transmissions()||changed;

            }
            if(p.legacy_enabled&&!p.maintenance_started&&!p.retiring.load())p.continue_keys();
            p.schedule_key_cleanup();changed=p.update_settling()||changed;
            p.refresh_activity_setup();
            p.retry_unadmitted_selection();changed=p.purge_canceled_activity()||changed;
            if(changed)p.publish();
        }
    }

};

HubService::Impl::ManagementCommand& HubService::Impl::Command::management(){return static_cast<ManagementCommand&>(*this);}
const HubService::Impl::ManagementCommand& HubService::Impl::Command::management()const{return static_cast<const ManagementCommand&>(*this);}
HubService::Impl::ConfigurationCommand& HubService::Impl::Command::configuration(){return static_cast<ConfigurationCommand&>(*this);}
const HubService::Impl::ConfigurationCommand& HubService::Impl::Command::configuration()const{return static_cast<const ConfigurationCommand&>(*this);}
HubService::Impl::ActionCommand& HubService::Impl::Command::action(){return static_cast<ActionCommand&>(*this);}
const HubService::Impl::ActionCommand& HubService::Impl::Command::action()const{return static_cast<const ActionCommand&>(*this);}
HubService::Impl::ReceivedCommand& HubService::Impl::Command::received(){return static_cast<ReceivedCommand&>(*this);}
const HubService::Impl::ReceivedCommand& HubService::Impl::Command::received()const{return static_cast<const ReceivedCommand&>(*this);}
HubService::Impl::InputCommand& HubService::Impl::Command::input(){return static_cast<InputCommand&>(*this);}
const HubService::Impl::InputCommand& HubService::Impl::Command::input()const{return static_cast<const InputCommand&>(*this);}

void HubService::Adapter::stop_holds(){owner.stop_holds();}
void HubService::Adapter::invalidate_binding(BusSide side, uint8_t address) {
        if(address<15)binding_wire_generation[side==BusSide::Front?0:1][address].fetch_add(1);
    }
HubService::Adapter::Flight* HubService::Adapter::reserve_flight() {for(auto& flight:flights)if(!flight.used){flight.used=true;flight.key=-1;flight.instance=0;flight.activity=flight.cleanup=false;return &flight;}return nullptr;}
bool HubService::Adapter::complete_transmissions() {
        bool changed=false;auto& root=owner;
        for(auto& flight:flights){if(!flight.used)continue;const auto result=flight.completion.result.load(std::memory_order_acquire);if(result==cec::TxResult::Pending)continue;
            flight.used=false;
            if(flight.key>=0){auto& state=root.keys[static_cast<size_t>(flight.key)];if(state.instance==flight.instance){state.waiting=false;if(result!=cec::TxResult::Success){state.phase=Impl::KeyPhase::failed;if(state.emitted)root.key_cleanup_roles|=Impl::key_role(static_cast<uint8_t>(flight.key));}}}
            if(flight.activity)--activity_outstanding;
            changed=true;
        }
        return changed;
    }
bool HubService::Adapter::transmissions_pending() const {
        for (const auto &flight : flights) if (flight.used) return true;
        return false;
    }
bool HubService::Adapter::activity_step_pending() const {
        if(adapter_id==0&&owner.scoped_mode&&owner.step_pending)return true;
        return std::any_of(flights.begin(),flights.end(),[](const auto& flight){
            return flight.used&&flight.activity&&flight.completion.result.load(std::memory_order_acquire)==cec::TxResult::Pending;
        });
    }
bool HubService::Adapter::send(BusSide side, uint8_t destination, const uint8_t *data, size_t size) {
        if (size > 15 || !size || destination > 15) return false;
        const uint8_t address = side == BusSide::Front ? front_address : back_address;
        // Give Physical Address accepts an unregistered initiator because its
        // response is broadcast. This still permits startup determination if
        // all back logical-address candidates are occupied or allocation is busy.
        if (address == 15 && !(side == BusSide::Back && destination < 15 && size == 1 && data[0] == 0x83)) {
            if(pacing_receive&&(data[0]==0x36||data[0]==0x86))ESP_LOGI(TAG,
                "Source policy TX adapter=%u side=%s destination=%u opcode=%02x admitted=0 reason=unregistered",
                adapter_id,side==BusSide::Front?"front":"back",destination,data[0]);
            return false;
        }
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
            const auto generation=binding_wire_generation[port][destination].load();
            fence = {&binding_wire_generation[port][destination], generation};
        }
        if (data[0] == 0x44 && !setting_activity) {
            fence.secondary_generation = &owner.key_generation;
            fence.expected_secondary_generation = owner.applied_key_generation;
        }
        if(executing_activity_step){
            const auto& root=owner;
            fence.secondary_generation=&root.activity_instance;
            fence.expected_secondary_generation=executing_activity_instance;
        }else if(executing_auth) {
            fence.secondary_generation=&owned_wire_generation;
            fence.expected_secondary_generation=owned_wire_generation.load();
            uint64_t deadline=executing_auth->deadline_ms;
            const auto& root=owner;
            auto current=std::atomic_load(&root.deadline_slot);
            if(current){
                const auto authority_deadline=(*current)(*executing_auth);
                if(!authority_deadline)return false;
                if(authority_deadline!=UINT64_MAX)deadline=deadline?std::min(deadline,authority_deadline):authority_deadline;
            }
            if(deadline&&deadline!=UINT64_MAX&&data[0]!=0x45)fence.expires_at_us=deadline>UINT64_MAX/1000?UINT64_MAX:deadline*1000;
        }
        if(emitting_user_key){const auto& root=owner;fence.tertiary_generation=&root.key_generation;fence.expected_tertiary_generation=root.applied_key_generation;}
        const unsigned attempts = data[0] == 0x44 ? 1 : 3;
        const bool paced=setting_activity||pacing_receive||emitting_key_cleanup;
        auto *flight = sending_key && data[0] == 0x44 ? sending_key : paced ? reserve_flight() : nullptr;
        const bool sent = flight ? bus.send_async_guarded(frame, fence, flight->completion, attempts) :
            !paced && bus.send_guarded(frame, fence, attempts);
        if(pacing_receive&&(data[0]==0x36||data[0]==0x86))ESP_LOGI(TAG,
            "Source policy TX adapter=%u side=%s destination=%u opcode=%02x admitted=%d",
            adapter_id,side==BusSide::Front?"front":"back",destination,data[0],sent);
        if (flight && !sent) flight->used = false;
        if(sent&&paced){flight->activity=setting_activity||pacing_receive;flight->cleanup=emitting_key_cleanup;if(flight->activity)++activity_outstanding;}
        if (!sent) ++dropped_tx;
        return sent;
    }
uint8_t HubService::Adapter::scan_side(BusSide side) { return side == BusSide::Front ? 1 : 2; }
uint8_t HubService::Adapter::pending_scan_sides() const { return requested_scan_sides | (scan_running ? discovery.sides : 0); }
bool HubService::Adapter::request_lookup_scan(BusSide side) {
        const auto mask = scan_side(side);
        if (pending_scan_sides() & mask) return true;
        const auto index = side == BusSide::Front ? 0 : 1;
        if (esp_timer_get_time() - last_scan[index] < 120000000) return false;
        requested_scan_sides |= mask;
        return true;
    }
bool HubService::Adapter::request_scan(BusSide side) { return request_lookup_scan(side); }
void HubService::Adapter::request_scan() {
        // The owner-local qualified path always supplies a side. Retain the
        // unqualified transport entry for the original direct Hub callers.
        request_lookup_scan(BusSide::Front); request_lookup_scan(BusSide::Back);
    }
bool HubService::Adapter::receive() {
        bool received = false;
        cec::Frame frame;
        for (const auto side : {BusSide::Front, BusSide::Back}) {
            auto &bus = side == BusSide::Front ? front : back;
            // Bounded draining prevents a noisy bus starving user commands.
            for (unsigned n = 0; n < 32 && bus.receive(frame); ++n) {
                ESP_LOGD(TAG, "%s RX %02x:%02x (%u)", side == BusSide::Front ? "front" : "back",
                         frame.data[0], frame.size > 1 ? frame.data[1] : 0, frame.size);
                if(frame.size>=4&&frame.data[1]==0x82&&frame.destination()==15&&frame.source()<15)
                    ESP_LOGI(TAG,"CEC Active Source RX adapter=%u side=%s sender=%u front=%u back=%u",
                        adapter_id,side==BusSide::Front?"front":"back",frame.source(),front_address,back_address);
                const bool previous=pacing_receive;pacing_receive=true;
                const auto pending = hub.receive(side, frame);
                pacing_receive=previous;
                if (pending) {
                    auto& root=owner;
                    if (xSemaphoreTake(root.command_slots,0)==pdTRUE) {
                        auto command=std::unique_ptr<Impl::Command>(new(std::nothrow)Impl::ReceivedCommand(Impl::Kind::Received));
                        if(!command){xSemaphoreGive(root.command_slots);ESP_LOGW(TAG,"Received HDMI-CEC lookup dropped because owner allocation failed");received=true;continue;}
                        command->received().received_frame=frame;command->received().received_side=side;command->received().received_adapter=adapter_id;
                        command->received().received_step=hub.receive_step();
                        command->received().received_generation=received_generation;command->received().received_activity=hub.activity_generation();
                        command->received().intent_generation=root.intent_generation.load();
                        command->received().received_boot=root.local_boot;
                        command->received().received_binding=frame.source()<15?binding_wire_generation[side==BusSide::Front?0:1][frame.source()].load():0;
                        command->received().received_wire=owned_wire_generation.load();
                        command->received().received_deadline=uint64_t(esp_timer_get_time())+120000000;
                        command->discovery_sides=pending;root.deferred.push_back(std::move(command));
                    } else ESP_LOGW(TAG,"Received HDMI-CEC lookup dropped because owner IPC is full");
                }
                received = true;
            }
        }
        return received;
    }
bool HubService::Adapter::discovery_pending() const {
        return requested_scan_sides || discovery.phase != DiscoveryPhase::Idle;
    }
void HubService::Adapter::begin_claim(BusSide side) {
        discovery.phase = side == BusSide::Back ? DiscoveryPhase::ClaimBack : DiscoveryPhase::ClaimFront;
        discovery.candidate = 0;
        discovery.nacks = 0;
    }
void HubService::Adapter::start_discovery() {
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
        owner.publish();
    }
void HubService::Adapter::finish_claim(BusSide side) {
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
void HubService::Adapter::claim_tick(BusSide side) {
        static constexpr uint8_t back_candidates[] = {0, 14};
        static constexpr uint8_t front_candidates[] = {4, 8, 11, 12, 13};
        const auto *candidates = side == BusSide::Back ? back_candidates : front_candidates;
        const unsigned count = side == BusSide::Back ? sizeof(back_candidates) : sizeof(front_candidates);
        auto &address = side == BusSide::Back ? back_address : front_address;
        auto &bus = side == BusSide::Back ? back : front;
        const auto physical = hub.front_physical_address();
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
                owner.publish();
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
void HubService::Adapter::begin_reply_window(bool back_side, int64_t now) {
        discovery.phase = back_side ? DiscoveryPhase::BackReplies : DiscoveryPhase::FinalReplies;
        discovery.deadline = now + 5000000;
        discovery.quiet_since = now;
    }
void HubService::Adapter::scan_tick(BusSide side, int64_t now) {
        auto &bus = side == BusSide::Back ? back : front;
        const auto own = side == BusSide::Back ? back_address : front_address;
        const bool determining = side == BusSide::Back && hub.front_physical_address() == 0xffff;
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
void HubService::Adapter::discovery_tick(bool received) {
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
                hub.set_addresses(front_address, back_address, hub.front_physical_address());
                if (discovery.sides & 1) begin_claim(BusSide::Front);
                else finish_discovery();
            } else {
                finish_discovery();
            }
            break;
        case DiscoveryPhase::Idle: break;
        }
    }
void HubService::Adapter::finish_discovery() {
        hub.finish_discovery();
        ++discovery_generation;
        scan_running = false;
        discovery.phase = DiscoveryPhase::Idle;
        owner.publish();
    }
bool HubService::Adapter::local_activity_authorized(const system::OwnedAuthorization& auth,uint32_t instance)const {
        const auto& root=owner;
        return instance==root.activity_instance.load()&&root.accepted_activity_authorized(auth);
    }
bool HubService::Adapter::owner_authorized(const system::OwnedAuthorization& auth,bool arrival) const {
        const auto& root=owner;
        if(!root.owned_controller || auth.controller_id!=root.owned_controller || auth.owner_boot!=root.local_boot ||
           auth.config_revision!=root.owned_config_revision)return false;
        if(arrival && auth.deadline_ms && uint64_t(esp_timer_get_time())/1000>=auth.deadline_ms)return false;
        auto check=std::atomic_load(&root.authorizer_slot);
        auto current=auth;if(!arrival)current.deadline_ms=0;
        return !check || (*check)(current);
    }
bool HubService::Adapter::source_input_next(const system::OwnedIntent& intent,const system::AdmissionResult& result,bool input_announced) const {
        return !input_announced&&hub.source_input_announcement_here(intent)&&result.source_present.value_or(false)&&
            (result.code==system::AdmissionCode::accepted||result.code==system::AdmissionCode::overloaded);
    }
system::AdmissionResult HubService::Adapter::execute_owned(const system::OwnedIntent& intent,const system::OwnedAuthorization& auth,bool validate_only,bool input_announced,uint32_t instance,bool local_step) {
        auto& root=owner;
        using K=system::OwnedIntentKind;
        if(auth.authority!=system::OwnedAuthority::control)return {system::AdmissionCode::unauthorized,"Original action has the wrong authority class"};
        if(!(local_step?local_activity_authorized(auth,instance):owner_authorized(auth)))return {system::AdmissionCode::unauthorized,"Original controller authority is unavailable"};
        if(intent.target.port.octopus_id!=root.octopus || intent.target.port.consortium_id!=root.consortium ||
           intent.target.port.tentacle_id!=adapter_id || !root.connector_configured(adapter_id))
            return {system::AdmissionCode::invalid,"The selected HDMI adapter is unavailable"};
        if(root.maintenance.load()||root.retiring.load())return {system::AdmissionCode::unauthorized,"This Amity is unavailable while updating or removed"};
        if(!intent.generation)return {system::AdmissionCode::invalid,"Missing original input generation"};
        if(!local_step)root.observe_controller_boot(auth.controller_boot);
        if(!local_step&&(received_controller_boot!=auth.controller_boot || received_link!=auth.link_id)) {
            stop_holds();received_generation=0;
            xSemaphoreTake(root.lock,portMAX_DELAY);
            received_controller_boot=auth.controller_boot;received_link=auth.link_id;
            xSemaphoreGive(root.lock);
        }
        if(validate_only){
            if(!local_step&&intent.generation<received_generation)return {system::AdmissionCode::stale,"Activity input was superseded"};
            return {system::AdmissionCode::accepted,{}};
        }
        if(!local_step&&intent.generation<received_generation)return {system::AdmissionCode::stale,"Activity input was superseded"};
        if(!local_step)received_generation=intent.generation;
        if(intent.kind==K::context&&intent.selected&&intent.source&&intent.source->port.octopus_id==intent.target.port.octopus_id&&intent.source->port.tentacle_id==intent.target.port.tentacle_id){
            hub.find_device(intent.source->osd_name,intent.source->port.side==system::Side::front?BusSide::Front:BusSide::Back);
            if(hub.pending_resolution())return {system::AdmissionCode::pending,"Original owner lookup is in progress"};
        }
        if(intent.kind==K::power||intent.kind==K::source_input||intent.kind==K::switch_input||intent.kind==K::query_audio){
            const auto* device=hub.find_device(intent.target.osd_name,intent.target.port.side==system::Side::front?BusSide::Front:BusSide::Back);
            if(hub.pending_resolution())return {system::AdmissionCode::pending,"Original owner lookup is in progress"};
            if(!device)return intent.kind==K::source_input?
                system::AdmissionResult{system::AdmissionCode::unresolved,"The named source is absent from this owner",false}:
                system::AdmissionResult{system::AdmissionCode::unresolved,"The named equipment is absent from this owner"};
        }
        struct SettingScope {bool& value;bool previous;~SettingScope(){value=previous;}} setting_scope{setting_activity,setting_activity};
        if(Impl::owner_activity(intent))setting_activity=true;
        if(intent.kind==K::discover){requested_scan_sides=3;return {system::AdmissionCode::accepted,{}};}
        executing_auth=&auth;executing_activity_step=local_step;executing_activity_instance=instance;
        bool admitted=false;
        if(!input_announced&&hub.source_input_announcement_here(intent))admitted=hub.announce_owned_input();
        else admitted=hub.execute_owned(intent);
        executing_auth=nullptr;executing_activity_step=false;
        if(intent.kind==K::context&&admitted){root.owner_context=intent;root.input_activity_generation=std::max(root.input_activity_generation,intent.generation);}
        if(hub.pending_resolution())return {system::AdmissionCode::pending,"Original owner lookup is in progress"};
        const auto present=intent.kind==K::source_input?std::optional<bool>{true}:std::nullopt;
        return admitted?system::AdmissionResult{system::AdmissionCode::accepted,{},present}:
            system::AdmissionResult{system::AdmissionCode::overloaded,"Original action could not enter the CEC FIFO or resolve its equipment",present};
    }

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
            if(!engine){engine.reset(new Adapter(p,pin.tentacle_id));engine->fallback_physical=front_physical[pin.tentacle_id];}
            bus=side==system::Side::front?&engine->front:&engine->back;
        }
        p.native_ports[p.native_port_count++] = {pin.tentacle_id, side, bus, pin.gpio};
    }
    return p.start_owner();
}
std::vector<AdapterRuntimeState> HubService::adapter_runtime_snapshot() const {
    auto& p = *impl_; xSemaphoreTake(p.lock, portMAX_DELAY);
    std::vector<AdapterRuntimeState> snapshot(p.cached_adapters.begin(), p.cached_adapters.end());
    xSemaphoreGive(p.lock); return snapshot;
}
esp_err_t HubService::commit_adapter_settings(system::BootId boot,
        std::vector<AdapterAddressChange> adapters, uint64_t deadline,
        std::function<esp_err_t()> persist) {
    auto& p = *impl_;
    if (!boot || !persist || adapters.empty() || adapters.size() > amity::max_tentacles) return ESP_ERR_INVALID_ARG;
    bool used[amity::max_tentacles]{};
    for (const auto& change : adapters) {
        if (change.tentacle_id >= amity::max_tentacles || used[change.tentacle_id]) return ESP_ERR_INVALID_ARG;
        used[change.tentacle_id] = true;
    }
    const auto now = uint64_t(esp_timer_get_time()) / 1000;
    if (deadline <= now) return ESP_ERR_TIMEOUT;
    auto reply = std::make_shared<Impl::Reply>();
    if (!reply->done) return ESP_ERR_NO_MEM;
    auto command = std::unique_ptr<Impl::Command>(new (std::nothrow) Impl::ManagementCommand{Impl::Kind::SettingsCommit});
    if (!command) return ESP_ERR_NO_MEM;
    command->reply = reply; command->management().settings_boot = boot;
    command->management().settings_adapters = std::move(adapters); command->management().settings_deadline = deadline; command->management().persist = std::move(persist);
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
        // applied and acknowledged, even if storage crosses the deadline.
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
    auto *c = new (std::nothrow) Impl::ConfigurationCommand{Impl::Kind::Configure};
    if (!c) return ESP_ERR_NO_MEM;
    c->configuration().activities = std::move(activities);
    return impl_->submit(c, commit);
}
esp_err_t HubService::request_activity(int index,uint32_t* accepted,std::optional<system::ActivityId> expected) { return request_activity(index,true,accepted,expected); }
esp_err_t HubService::repair_activity() {
    auto& p=*impl_;if(!p.lock)return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(p.lock,portMAX_DELAY);
    const int target=p.activity_setup_in_progress()?p.reserved_target.load():p.cached.current_activity;
    std::optional<system::ActivityId> expected;
    if(target>=0){
        if(static_cast<size_t>(target)>=p.cached.activities.size()){xSemaphoreGive(p.lock);return ESP_ERR_INVALID_STATE;}
        expected=p.cached.activities[target].activity_id;
    }
    xSemaphoreGive(p.lock);
    return request_activity(target,false,nullptr,expected);
}
uint32_t HubService::activity_selection() const { return impl_->activity_selection.load(); }
esp_err_t HubService::request_activity(int index, bool notify_selection,uint32_t* accepted_selection,std::optional<system::ActivityId> expected) {
    uint32_t receipt=0;
    auto &p = *impl_; if (!p.lock) return ESP_ERR_INVALID_STATE;
    if (!p.local_coordinator.load()) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(p.lock, portMAX_DELAY);
    if(p.scoped_configurations.load()){xSemaphoreGive(p.lock);return ESP_ERR_INVALID_STATE;}
    if (index < -1 || index >= static_cast<int>(p.cached.activities.size())) { xSemaphoreGive(p.lock); return ESP_ERR_INVALID_ARG; }
    if(expected&&(index<0||p.cached.activities[index].activity_id!=*expected)){xSemaphoreGive(p.lock);return ESP_ERR_INVALID_STATE;}
    const int target=p.activity_setup_in_progress()?p.reserved_target.load():p.cached.current_activity;
    auto *command = new (std::nothrow) Impl::ActionCommand{index < 0 ? Impl::Kind::Standby : Impl::Kind::Select, index};
    if (command) command->route_transition = index != target;
    const auto error = p.submit(command, [&] {
        p.reserved_target.store(index);p.key_generation.fetch_add(1);
        if (notify_selection) command->action().activity_receipt=receipt=p.activity_selection.fetch_add(1)+1;
        return ESP_OK;
    });
    if(error==ESP_OK&&accepted_selection)*accepted_selection=receipt;
    xSemaphoreGive(p.lock); return error;
}
int HubService::activity_target() const {
    auto &p = *impl_; if (!p.lock) return -1;
    xSemaphoreTake(p.lock, portMAX_DELAY);
    const auto target = p.activity_setup_in_progress()?p.reserved_target.load():p.cached.current_activity;
    xSemaphoreGive(p.lock); return target;
}
bool HubService::select_activity(int index) { return request_activity(index) == ESP_OK; }
bool HubService::activity_setup_in_progress()const{return impl_->activity_setup_in_progress();}
bool HubService::press_key(uint8_t key, bool repeat) {
    if (!impl_->task||!impl_->local_coordinator.load()) return false;
    if(impl_->activity_setup_in_progress())return true;
    if (key == 0x40) return standby();
    auto *command = new (std::nothrow) Impl::Command{Impl::Kind::Key, key, repeat};
    if (command) {
        command->key_generation = impl_->key_generation.load();
        command->release_generation = (*impl_->release_generations)[key].load();
    }
    return impl_->enqueue(command);
}
bool HubService::press_key_for_activity(uint8_t key, uint32_t activity_generation, bool repeat) {
    if (!impl_->task||!impl_->local_coordinator.load()) return false;
    if(impl_->activity_setup_in_progress())return true;
    auto *command = new (std::nothrow) Impl::Command{Impl::Kind::Key, key, repeat};
    if (command) {
        command->key_generation = impl_->key_generation.load();
        command->release_generation = (*impl_->release_generations)[key].load();
        command->activity_bound = true;
        command->activity_generation = activity_generation;
    }
    return impl_->enqueue(command);
}
uint32_t HubService::input_epoch()const{return impl_->key_generation.load();}
bool HubService::press_timed_key(uint8_t key,uint32_t generation,uint32_t selection,uint32_t input_epoch){
    if(!impl_->task||!impl_->local_coordinator.load())return false;
    if(input_epoch!=impl_->key_generation.load())return true;
    if(impl_->activity_setup_in_progress())return true;
    if(key==0x40)return standby();
    auto* command=new(std::nothrow)Impl::Command{Impl::Kind::Key,key,true};
    if(command){command->key_generation=input_epoch;command->release_generation=(*impl_->release_generations)[key].load();command->activity_bound=command->selection_bound=true;command->activity_generation=generation;command->expected_selection=selection;}
    return impl_->enqueue(command);
}
bool HubService::release_timed_key(uint8_t key,uint32_t generation,uint32_t selection,uint32_t input_epoch){
    auto* command=new(std::nothrow)Impl::Command{Impl::Kind::Release,key};
    if(command){command->key_generation=input_epoch;command->activity_bound=command->selection_bound=true;command->activity_generation=generation;command->expected_selection=selection;}
    // This is exact cleanup in the existing bounded IPC. Wait for a cleanup
    // reservation instead of applying an unfenced emergency raw-key release.
    return impl_->submit(command,{},portMAX_DELAY)==ESP_OK;
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
    (*impl_->release_generations)[key].fetch_add(1);
    ESP_LOGI(TAG,"Key release stage=fallback key=%u",static_cast<unsigned>(key));
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
    ++impl_->activity_instance;
    auto* command = new (std::nothrow) Impl::ManagementCommand{Impl::Kind::AuthorityTransition};
    if (command) { command->repeat = local; command->management().config_revision = epoch; }
    const auto result = impl_->request(command, 5000);
    if (!result) { if (result.code != system::AdmissionCode::expired) impl_->authority_transition.store(false); return ESP_ERR_INVALID_STATE; }
    return ESP_OK;
}
esp_err_t HubService::retire_membership() {
    impl_->retiring.store(true);++impl_->activity_instance;
    impl_->local_coordinator.store(false);
    const auto result = impl_->request(new (std::nothrow) Impl::ManagementCommand{Impl::Kind::Retire}, 5000);
    return result ? ESP_OK : result.code == system::AdmissionCode::expired ? ESP_ERR_TIMEOUT : ESP_ERR_INVALID_STATE;
}
esp_err_t HubService::begin_maintenance() {
    if (xSemaphoreTake(impl_->configuration_gate, 0) != pdTRUE) return ESP_ERR_INVALID_STATE;
    if (impl_->settings_gate.load()) { xSemaphoreGive(impl_->configuration_gate); return ESP_ERR_INVALID_STATE; }
    impl_->maintenance.store(true);++impl_->activity_instance;
    xSemaphoreGive(impl_->configuration_gate);
    if (impl_->maintenance_applied.load()) return ESP_OK;
    const auto result = impl_->request(new (std::nothrow) Impl::ManagementCommand{Impl::Kind::MaintenanceBegin}, 5000);
    if(!result&&!impl_->maintenance_started.load())impl_->maintenance.store(false);
    return result ? ESP_OK : result.code == system::AdmissionCode::expired ? ESP_ERR_TIMEOUT : ESP_ERR_INVALID_STATE;
}
void HubService::end_maintenance() {
    impl_->request(new (std::nothrow) Impl::ManagementCommand{Impl::Kind::MaintenanceEnd});
}
bool HubService::maintenance_ready() const { return impl_->maintenance_applied.load(); }

esp_err_t HubService::configure_scoped(std::vector<Activity> activities,std::vector<system::ScopedSelector> selectors,
        OwnedSender sender,system::OctopusId controller,uint64_t revision,std::function<esp_err_t()> commit,uint64_t presentation_revision,std::vector<system::OctopusId> owners,std::function<bool(const Snapshot&)> can_commit){
    if(!controller||!revision)return ESP_ERR_INVALID_ARG;
    auto* c=new(std::nothrow)Impl::ConfigurationCommand{Impl::Kind::ScopedConfigure};if(!c)return ESP_ERR_NO_MEM;
    c->configuration().activities=std::move(activities);c->configuration().selectors=std::move(selectors);c->configuration().sender=std::move(sender);c->management().controller=controller;c->management().config_revision=revision;c->management().presentation_revision=presentation_revision;c->configuration().owners=std::move(owners);
    auto& p=*impl_;if(!p.lock){delete c;return ESP_ERR_INVALID_STATE;}
    const auto result=p.submit(c,[&]{
        xSemaphoreTake(p.lock,portMAX_DELAY);
        const auto validate=[&]{
        const auto& state=p.cached;
        if(p.activity_setup_in_progress()||p.owner_activity_commands.load()||state.activity_settling||p.scoped_configurations.load()||
           state.processed_activity_selection!=p.activity_selection.load()||(can_commit&&!can_commit(state)))return false;
        if(state.current_activity>=0){
            if(static_cast<size_t>(state.current_activity)>=state.activities.size()||p.owned_controller!=controller)return false;
            const auto& current=state.activities[state.current_activity];
            if(!current.activity_id)return false;
            const auto next=std::find_if(c->configuration().activities.begin(),c->configuration().activities.end(),[&](const auto& a){return a.activity_id==current.activity_id;});
            if(next==c->configuration().activities.end()||std::count_if(c->configuration().activities.begin(),c->configuration().activities.end(),[&](const auto& a){return a.activity_id==current.activity_id;})!=1||
               next->name!=current.name||next->display!=current.display||next->source!=current.source||next->audio!=current.audio||
               next->switch_device!=current.switch_device||next->switch_input!=current.switch_input)return false;
            const auto target=[](const auto& choices,const std::string& value)->std::optional<system::QualifiedDeviceName>{for(const auto& choice:choices)if(choice.value==value)return choice.target;return {};};
            for(const auto& value:{current.display,current.source,current.audio,current.switch_device})
                if(target(p.active_selectors,value)!=target(c->configuration().selectors,value))return false;
        }
        return true;
        };
        const bool admitted=validate();
        if(admitted){c->configuration_reservation=true;++p.scoped_configurations;}
        xSemaphoreGive(p.lock);
        if(!admitted)return ESP_ERR_INVALID_STATE;
        // The reservation keeps selection stable while persistence performs
        // its existing owner/network work; snapshots and owner IPC stay live.
        return commit?commit():ESP_OK;
    });
    return result;
}
system::AdmissionResult HubService::submit_owned(const system::OwnedIntent& intent,const system::OwnedAuthorization& auth,uint32_t timeout){
    auto* c=Impl::owned_request(intent,auth);if(!c)return {system::AdmissionCode::overloaded,"Owner request allocation failed"};
    if(!timeout){const auto result=impl_->submit(c);return result==ESP_OK?system::AdmissionResult{system::AdmissionCode::accepted,{}}:system::AdmissionResult{system::AdmissionCode::overloaded,"Owner input IPC is full"};}
    return impl_->request(c,timeout);
}
esp_err_t HubService::bind_owned_controller(system::OctopusId controller,uint64_t revision){
    if(!controller||!revision)return ESP_ERR_INVALID_ARG;
    auto* c=new(std::nothrow)Impl::ManagementCommand{Impl::Kind::OwnedBind};if(!c)return ESP_ERR_NO_MEM;c->management().controller=controller;c->management().config_revision=revision;
    xSemaphoreTake(impl_->lock,portMAX_DELAY);if(impl_->owned_controller!=controller)++impl_->activity_instance;xSemaphoreGive(impl_->lock);
    const auto result=impl_->request(c,5000);return result?ESP_OK:ESP_ERR_INVALID_STATE;
}
void HubService::set_owned_observer(std::function<void(const system::OwnedAdapterSnapshot&)> observer){
    std::atomic_store(&impl_->observer_slot,observer?std::make_shared<const Impl::OwnedObserver>(std::move(observer)):std::shared_ptr<const Impl::OwnedObserver>{});
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
void HubService::forget_owned_snapshots(system::OctopusId owner){
    auto& p=*impl_;xSemaphoreTake(p.lock,portMAX_DELAY);
    const auto matching=[&](const auto& report){return report.owner==owner;};
    p.owned_reports.erase(std::remove_if(p.owned_reports.begin(),p.owned_reports.end(),matching),p.owned_reports.end());
    p.pending_reports.erase(std::remove_if(p.pending_reports.begin(),p.pending_reports.end(),matching),p.pending_reports.end());
    p.reports_dirty=true;
    xSemaphoreGive(p.lock);
}
bool HubService::refresh_owned_context(system::OctopusId owner,uint64_t revision,std::function<void(system::OwnedContextResult)> completed){
    if(!owner||!revision)return false;
    auto* c=new(std::nothrow)Impl::ManagementCommand{Impl::Kind::OwnedContext};if(!c)return false;c->management().controller=owner;c->management().config_revision=revision;c->management().context_completion=std::move(completed);
    // The link task requests a fresh state capture; it must not wait for the
    // owner task. Runtime marks context ready only when the resulting current
    // context is admitted to the authenticated link, ahead of ordinary work.
    return impl_->submit(c)==ESP_OK;
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
    auto* c=new(std::nothrow)Impl::ManagementCommand{Impl::Kind::OwnedFence};if(!c)return false;c->management().controller=controller;c->management().link=link;
    return bool(p.request(c));
}
bool HubService::discover_owned(system::OctopusId owner,uint8_t adapter){
    if(!owner||adapter>=amity::max_tentacles)return false;
    auto* c=new(std::nothrow)Impl::ManagementCommand{Impl::Kind::OwnedDiscover,adapter};if(!c)return false;c->management().controller=owner;return impl_->submit(c)==ESP_OK;
}
uint64_t HubService::applied_configuration_revision()const{
    auto& p=*impl_;xSemaphoreTake(p.lock,portMAX_DELAY);const auto revision=p.cached_revision;xSemaphoreGive(p.lock);return revision;
}
uint64_t HubService::executing_configuration_revision()const{return impl_->owned_config_revision.load();}
}
