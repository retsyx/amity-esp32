/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

// The firmware's startup and main task, main/main.cpp, built whole. The test
// includes the file, so its helpers and globals stay reachable, and boots it
// through app_main() against the core fakes, the SDK fakes of tests/main_fakes
// and strict seams for every other firmware file it calls. The hub is a real
// SystemControl over HubService seams. Each scenario runs in its own process.
#include "core_fakes.hpp"
#include "seams.hpp"
// Declares vTaskDelete before the hook below redefines its calls.
#include "freertos/task.h"

namespace {
// Runs as a task deletes itself, before the deletion.
void deleting_task();
}  // namespace
// vTaskDelete never returns on the target, so the locals of the frame that
// calls it are never destroyed. The hook observes them at that point.
#define vTaskDelete(task) (deleting_task(), vTaskDelete(task))
#include "main.cpp"
#undef vTaskDelete

#include "scenario_main.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <mutex>
#include <new>
#include <set>
#include <string>
#include <thread>
#include <vector>

// Heap blocks allocated on a thread while it tracks allocations, until they
// are freed. A runtime_status() answer owns one, so a live copy is visible.
// Frees run until the process ends, so the lock is never destroyed.
namespace allocations {
thread_local bool tracking = false;
std::atomic_flag busy = ATOMIC_FLAG_INIT;
void *blocks[64];
std::size_t count = 0;

struct Lock {
    Lock() {
        while (busy.test_and_set(std::memory_order_acquire)) std::this_thread::yield();
    }
    ~Lock() { busy.clear(std::memory_order_release); }
};
void track(void *block) {
    if (!tracking) return;
    Lock lock;
    if (count == sizeof(blocks) / sizeof(blocks[0])) amity_test::fail(__FILE__, __LINE__, "too many tracked blocks");
    blocks[count++] = block;
}
void release(void *block) {
    if (!block) return;
    Lock lock;
    for (std::size_t i = 0; i < count; ++i) {
        if (blocks[i] == block) {
            blocks[i] = blocks[--count];
            break;
        }
    }
}
std::size_t live() {
    Lock lock;
    return count;
}
void *allocate(std::size_t size) {
    void *block = std::malloc(size ? size : 1);
    if (block) track(block);
    return block;
}
}  // namespace allocations

void *operator new(std::size_t size) {
    if (void *block = allocations::allocate(size)) return block;
    throw std::bad_alloc();
}
void *operator new[](std::size_t size) {
    if (void *block = allocations::allocate(size)) return block;
    throw std::bad_alloc();
}
void *operator new(std::size_t size, const std::nothrow_t &) noexcept { return allocations::allocate(size); }
void *operator new[](std::size_t size, const std::nothrow_t &) noexcept { return allocations::allocate(size); }
void operator delete(void *block) noexcept {
    allocations::release(block);
    std::free(block);
}
void operator delete[](void *block) noexcept {
    allocations::release(block);
    std::free(block);
}
void operator delete(void *block, std::size_t) noexcept {
    allocations::release(block);
    std::free(block);
}
void operator delete[](void *block, std::size_t) noexcept {
    allocations::release(block);
    std::free(block);
}
void operator delete(void *block, const std::nothrow_t &) noexcept {
    allocations::release(block);
    std::free(block);
}
void operator delete[](void *block, const std::nothrow_t &) noexcept {
    allocations::release(block);
    std::free(block);
}

namespace {

namespace model = amity::system;
using amity::consortium::RuntimeStartup;
using amity::consortium::RuntimeStatus;
using amity_seam::install;
using amity_seam::uninstall;

template <class Id> Id id(unsigned n) {
    char text[33];
    std::snprintf(text, sizeof(text), "%032x", n);
    return *Id::parse(text);
}

const model::ConsortiumId consortium = id<model::ConsortiumId>(0xc0);
const model::OctopusId octopus = id<model::OctopusId>(0x0c);
const model::BootId boot_identity = id<model::BootId>(0xb0);
const std::string boot_text = boot_identity.str();

// The live role of a board that waits for its first setup. Its certificate
// digest lives on the heap, so each copy of a status owns one heap block.
RuntimeStatus awaiting_setup() {
    RuntimeStatus status;
    status.initialized = true;
    status.phase = "awaiting_setup";
    status.leaf_spki_sha256 = std::string(64, '5');
    return status;
}

// What the seams report about the board.
struct Board {
    // The saved role runtime_read_startup reads, and the live role.
    esp_err_t startup_read = ESP_OK;
    RuntimeStartup startup;
    RuntimeStatus status = awaiting_setup();
    bool setup_eligible = true;
    esp_err_t group_start = ESP_OK;
    esp_err_t claim = ESP_OK;
    // The role a claim leaves behind when it fails, if any.
    std::function<void()> claim_lost;
    bool firmware_ready = true;
    // The arguments of each Matter controller, coordinator authority and
    // configuration load setting.
    std::vector<bool> controller, authority, loads;
    esp_err_t load = ESP_OK;
    esp_err_t activation = ESP_OK;
    // The saved configuration web_admin::load installs in the hub.
    config_store::ConfigurationSnapshot configuration;
    amity::Snapshot snapshot;
    std::string hostname = "amity-test";
    // Matter: fabrics, commissioning evidence and the commissioning window.
    esp_err_t matter_start = ESP_OK;
    unsigned fabrics = 0;
    bool enrolled = false;
    bool window_open = false;
    unsigned windows = 0;
    esp_err_t status_result = ESP_OK, open_result = ESP_OK;
    unsigned status_failures = 0, open_failures = 0;
    // Wi-Fi power save, and the errors each call returns.
    wifi_ps_type_t power_save = WIFI_PS_MIN_MODEM;
    bool power_save_ignored = false;
    esp_err_t read_error = ESP_OK, set_error = ESP_OK, verify_error = ESP_OK;
} board;

// The station interface and the mDNS responder Matter shares with the
// administration hostname.
struct Network {
    alignas(8) unsigned char handle[8]{};
    esp_netif_t *station() { return reinterpret_cast<esp_netif_t *>(handle); }
    bool up = false;
    uint32_t address = 0;
    std::vector<esp_ip6_addr_t> ipv6;
    // The addresses of the last alias update, in list order.
    std::vector<esp_ip_addr_t> published;
    std::string hostname = "amity-test";
    bool responder = false;
    std::string responder_hostname = "matter-host";
    std::set<std::string> aliases, services;
    unsigned address_updates = 0;
} network;

// What app_main registered.
struct Registered {
    device_settings::HostnameHandler hostname;
    device_settings::SetupButtonHandler setup = nullptr;
    void *setup_context = nullptr;
    amity::consortium::RuntimeLifecycleHooks lifecycle;
    std::function<esp_err_t()> maintenance_begin;
    std::function<void()> maintenance_end;
    system_services::updates::CohortHooks cohort;
    esp_event_handler_t ip_events = nullptr, wifi_events = nullptr;
    std::function<void(const amity::Snapshot &)> on_change;
    amity_matter_callbacks_t matter{};
} registered;

// The scenario's check as a task deletes itself.
std::function<void()> on_task_delete;
void deleting_task() {
    if (on_task_delete) on_task_delete();
}

// The seam calls made so far, keeping only the named ones, in call order.
std::vector<std::string> calls(std::initializer_list<const char *> names) {
    std::vector<std::string> out;
    for (const auto &call : amity_seam::calls()) {
        for (const char *name : names) {
            if (call == name) out.push_back(call);
        }
    }
    return out;
}
std::size_t count(const char *name) { return amity_seam::count(name); }
// Whether the first call of each named seam follows the first call of the one before.
bool ordered(std::initializer_list<const char *> names) {
    const auto all = amity_seam::calls();
    auto at = all.begin();
    for (const char *name : names) {
        at = std::find(at, all.end(), name);
        if (at == all.end()) return false;
    }
    return true;
}

std::vector<amity_fake::Task> tasks_named(const char *name) {
    std::vector<amity_fake::Task> out;
    for (const auto &task : amity_fake::tasks()) {
        if (task.name == name) out.push_back(task);
    }
    return out;
}
TaskHandle_t task(const char *name) {
    const auto found = tasks_named(name);
    CHECK(found.size() == 1);
    return found.front().handle;
}

// NVS for a ConfigurationStore.
struct Memory final : config_store::ConfigBackend {
    std::map<std::string, std::string> values;
    config_store::ReadResult read(const char *key, std::string &value) override {
        const auto found = values.find(key);
        if (found == values.end()) return config_store::ReadResult::missing;
        value = found->second;
        return config_store::ReadResult::found;
    }
    bool write(const char *key, const std::string &value) override {
        values[key] = value;
        return true;
    }
    bool erase(const char *key) override {
        values.erase(key);
        return true;
    }
};

// A configuration of this board's consortium, which it controls.
config_store::Configuration configuration(std::initializer_list<const char *> labels) {
    config_store::Configuration out;
    out.consortium_id = consortium;
    out.controller_octopus_id = octopus;
    out.octopuses.push_back({octopus, 1});
    unsigned n = 0;
    for (const char *label : labels) {
        config_store::ConfiguredActivity activity;
        activity.activity_id = id<model::ActivityId>(0xa0 + n);
        activity.label = label;
        out.activities.push_back(activity);
        out.activity_order.push_back(activity.activity_id);
        out.external_slots.push_back({static_cast<uint16_t>(n + 1), activity.activity_id, false, false, {}});
        ++n;
    }
    return out;
}

// Saves a configuration through the real ConfigurationStore, the only code
// that makes a ConfigurationSnapshot, with its encoding and digests as seams.
// Each save changes the policy, so revisions count the saves.
config_store::ConfigurationSnapshot saved(const config_store::Configuration &value, uint64_t revision = 1) {
    using namespace config_store;
    static unsigned policies = 0;
    install<&validate_configuration>([](const Configuration &, std::string &) { return true; });
    install<&reconcile_external_slots>([](const Configuration &, Configuration &, std::string &) { return true; });
    install<&executable_policy_hash>([](const Configuration &) { return "policy " + std::to_string(++policies); });
    install<&presentation_hash>([](const Configuration &) { return std::string("presentation"); });
    install<&serialize_configuration>([](const Configuration &saved, bool) {
        return "configuration " + std::to_string(saved.config_revision);
    });
    install<&sha256_hex>([](std::string_view text) {
        char digest[65];
        std::snprintf(digest, sizeof(digest), "%064zx", std::hash<std::string_view>{}(text));
        return std::string(digest);
    });
    Memory memory;
    ConfigurationStore store(memory);
    ConfigurationSnapshot snapshot;
    std::string error;
    for (uint64_t saves = 0; saves < revision; ++saves) {
        CHECK(store.stage(value, snapshot ? snapshot->configuration.revisions() : RevisionPair{}, error) ==
              ConfigResult::ok);
        CHECK(store.commit(snapshot, error) == ConfigResult::ok);
    }
    CHECK(snapshot && snapshot->configuration.config_revision == revision);
    return snapshot;
}

// The hub's view of the configuration seams it reads.
void install_hub() {
    install<&amity::HubService::snapshot>([](const amity::HubService &) { return board.snapshot; });
    install<&config_store::ordered_activities>([](const config_store::Configuration &saved) {
        return saved.activity_order;
    });
    install<&amity::HubService::configure_scoped>(
        [](amity::HubService &, std::vector<amity::Activity>, std::vector<model::ScopedSelector>,
           amity::HubService::OwnedSender, model::OctopusId controller, uint64_t, std::function<esp_err_t()> commit,
           uint64_t, std::vector<model::OctopusId>, std::function<bool(const amity::Snapshot &)>) {
            CHECK(controller == octopus);
            return commit ? commit() : ESP_OK;
        });
}

// Everything app_main calls to boot a board that waits for its first setup.
void install_boot() {
    install_hub();
    install<&config_store::initialize_json_memory>([] {});
    install<&config_store::init>([] { return ESP_OK; });
    install<&device_settings::finish_factory_reset>([] { return ESP_OK; });
    install<&nvs_flash_init>([] { return ESP_OK; });
    install<&device_settings::init>([] { return ESP_OK; });
    install<&device_settings::set_hostname_handler>([](device_settings::HostnameHandler handler) {
        registered.hostname = std::move(handler);
    });
    install<&system_services::start_log_capture>([] {});
    install<&device_settings::set_setup_button_handler>([](device_settings::SetupButtonHandler handler, void *context) {
        registered.setup = handler;
        registered.setup_context = context;
    });
    install<&device_settings::start_reset_button>([](int gpio) {
        CHECK(gpio == amity::board_profile.reset_button);
        return ESP_OK;
    });
    install<&config_store::load_system_identity>([](model::OctopusId &local, model::ConsortiumId &group) {
        local = octopus;
        group = consortium;
        return ESP_OK;
    });
    install<&amity::consortium::runtime_read_startup>([](model::OctopusId local, RuntimeStartup &startup) {
        CHECK(local == octopus);
        startup = board.startup;
        return board.startup_read;
    });
    install<&amity_matter_set_controller_enabled>([](bool enabled) {
        board.controller.push_back(enabled);
        // Disabling the controller frees the responder Matter shares.
        if (!enabled) network.responder = false;
        return ESP_OK;
    });
    install<&amity::HubService::set_system_identity>([](amity::HubService &, model::ConsortiumId group,
                                                        model::OctopusId local) {
        CHECK(group == consortium && local == octopus);
    });
    install<&amity::HubService::set_coordinator_authority>([](amity::HubService &, bool enabled) {
        board.authority.push_back(enabled);
    });
    install<&system_services::boot_id>([] { return boot_text.c_str(); });
    install<&amity::HubService::set_boot_identity>([](amity::HubService &, model::BootId boot) {
        CHECK(boot == boot_identity);
    });
    install<&amity::HubService::start>([](amity::HubService &, const cec::BoardProfile &profile,
                                          const std::array<uint16_t, amity::max_tentacles> &) {
        CHECK(profile.id == amity::board_profile.id);
        return ESP_OK;
    });
    install<&device_settings::adapter_defaults>([] { return device_settings::AdapterDefaults{}; });
    install<&system_services::adapter_settings::install>([](amity::SystemControl &control) { CHECK(&control == &hub); });
    install<&amity::consortium::runtime_settings_hooks>([](amity::consortium::RuntimeSettingsHooks) {});
    install<&amity::consortium::runtime_log_hooks>([](amity::consortium::RuntimeLogHooks) {});
    install<&web_admin::load>([](amity::SystemControl &control, bool defer) {
        CHECK(&control == &hub);
        board.loads.push_back(defer);
        if (board.configuration) {
            CHECK((defer ? control.configure_passive_metadata(board.configuration)
                         : control.configure(board.configuration)) == ESP_OK);
        }
        return board.load;
    });
    install<&amity::consortium::runtime_lifecycle_hooks>([](amity::consortium::RuntimeLifecycleHooks hooks) {
        registered.lifecycle = std::move(hooks);
    });
    install<&system_services::updates::maintenance_hooks>([](std::function<esp_err_t()> begin,
                                                             std::function<void()> end) {
        registered.maintenance_begin = std::move(begin);
        registered.maintenance_end = std::move(end);
    });
    install<&heap_caps_get_free_size>([](uint32_t) -> size_t { return 65536; });
    install<&amity::radio::start>([] { return ESP_OK; });
    install<&amity::consortium::runtime_register_gatt>([] { return ESP_OK; });
    install<&amity_matter_start>([](const amity_matter_callbacks_t *callbacks) {
        registered.matter = *callbacks;
        return board.matter_start;
    });
    install<&esp_wifi_get_ps>([](wifi_ps_type_t *mode) {
        // Power save is readable only after Matter initialized networking.
        CHECK(count("amity_matter_start") == 1);
        const auto error = count("esp_wifi_get_ps") == 1 ? board.read_error : board.verify_error;
        if (error != ESP_OK) return error;
        *mode = board.power_save;
        return ESP_OK;
    });
    install<&esp_wifi_set_ps>([](wifi_ps_type_t mode) {
        CHECK(mode == WIFI_PS_NONE);
        if (board.set_error != ESP_OK) return board.set_error;
        if (!board.power_save_ignored) board.power_save = mode;
        return ESP_OK;
    });
    install<&amity::network_diagnostics::start>([] {});
    install<&amity::consortium::runtime_set_hardware_profile>([](uint8_t supported, uint8_t mask) {
        CHECK(supported == amity::board_profile.tentacle_count && mask == 1);
        return ESP_OK;
    });
    install<&amity::consortium::runtime_start>([](model::ConsortiumId group, model::OctopusId local, model::BootId boot,
                                                  uint64_t) {
        CHECK(group == consortium && local == octopus && boot == boot_identity);
        return board.group_start;
    });
    install<&system_services::updates::start_group_updates>([](model::OctopusId local,
                                                               system_services::updates::CohortHooks hooks) {
        CHECK(local == octopus);
        registered.cohort = std::move(hooks);
    });
    install<&amity::consortium::runtime_configuration_changed>([](uint64_t) {});
    install<&esp_event_handler_register>([](esp_event_base_t base, int32_t event, esp_event_handler_t handler, void *) {
        CHECK(event == ESP_EVENT_ANY_ID);
        if (base == IP_EVENT) registered.ip_events = handler;
        if (base == WIFI_EVENT) registered.wifi_events = handler;
        return ESP_OK;
    });
    install<&amity::HubService::on_change>([](amity::HubService &, std::function<void(const amity::Snapshot &)> callback) {
        registered.on_change = std::move(callback);
    });
    install<&amity::consortium::runtime_status>([] {
        allocations::tracking = true;
        RuntimeStatus status = board.status;
        allocations::tracking = false;
        return status;
    });
    install<&amity_matter_get_status>([](amity_matter_status_t *status) {
        if (board.status_failures) {
            --board.status_failures;
            return ESP_FAIL;
        }
        if (board.status_result != ESP_OK) return board.status_result;
        status->fabric_count = static_cast<uint8_t>(board.fabrics);
        status->commissioning_window = board.window_open;
        std::snprintf(status->manual_code, sizeof(status->manual_code), "34970112332");
        std::snprintf(status->qr_payload, sizeof(status->qr_payload), "MT:Y.K9042C00KA0648G00");
        return ESP_OK;
    });
    install<&system_services::confirm_firmware>([] { return ESP_OK; });
    install<&web_admin::finish_configuration_activation>([] { return board.activation; });
    install<&amity::start_configuration_synchronization>([](amity::SystemControl &control) {
        CHECK(&control == &hub);
        return ESP_OK;
    });
    install<&system_services::firmware_ready>([] { return board.firmware_ready; });
    install<&amity::consortium::runtime_original_setup_eligible>([] { return board.setup_eligible; });
    install<&amity_matter_open_commissioning>([](uint16_t seconds) {
        CHECK(seconds == 300);
        if (board.open_failures) {
            --board.open_failures;
            return ESP_FAIL;
        }
        if (board.open_result != ESP_OK) return board.open_result;
        if (!board.window_open) {
            board.window_open = true;
            ++board.windows;
        }
        return ESP_OK;
    });
    install<&device_settings::hostname>([] { return board.hostname.c_str(); });
}

// The station interface and the shared responder, for the original's
// administration hostname.
void install_network() {
    install<&esp_netif_get_handle_from_ifkey>([](const char *key) {
        CHECK(std::string(key) == "WIFI_STA_DEF");
        return network.station();
    });
    install<&esp_netif_is_netif_up>([](esp_netif_t *station) {
        CHECK(station == network.station());
        return network.up;
    });
    install<&esp_netif_get_ip_info>([](esp_netif_t *station, esp_netif_ip_info_t *info) {
        CHECK(station == network.station());
        info->ip.addr = network.address;
        return ESP_OK;
    });
    install<&esp_netif_get_all_preferred_ip6>([](esp_netif_t *station, esp_ip6_addr_t *addresses) {
        CHECK(station == network.station() && network.ipv6.size() <= CONFIG_LWIP_IPV6_NUM_ADDRESSES);
        std::copy(network.ipv6.begin(), network.ipv6.end(), addresses);
        return static_cast<int>(network.ipv6.size());
    });
    install<&esp_netif_set_hostname>([](esp_netif_t *station, const char *hostname) {
        CHECK(station == network.station());
        network.hostname = hostname;
        return ESP_OK;
    });
    install<&mdns_init>([] {
        network.responder = true;
        return ESP_OK;
    });
    install<&mdns_hostname_get>([](char *hostname) {
        CHECK(network.responder);
        std::snprintf(hostname, 65, "%s", network.responder_hostname.c_str());
        return ESP_OK;
    });
    install<&mdns_hostname_set>([](const char *hostname) {
        CHECK(network.responder);
        network.responder_hostname = hostname;
        return ESP_OK;
    });
    install<&mdns_delegate_hostname_add>([](const char *hostname, const mdns_ip_addr_t *addresses) {
        CHECK(network.responder && !addresses);
        network.aliases.insert(hostname);
        return ESP_OK;
    });
    install<&mdns_delegate_hostname_set_address>([](const char *hostname, const mdns_ip_addr_t *addresses) {
        CHECK(network.responder && network.aliases.count(hostname));
        network.published.clear();
        for (; addresses; addresses = addresses->next) network.published.push_back(addresses->addr);
        ++network.address_updates;
        return ESP_OK;
    });
    install<&mdns_delegate_hostname_remove>([](const char *hostname) {
        CHECK(network.responder);
        network.aliases.erase(hostname);
        network.services.erase(hostname);
        return ESP_OK;
    });
    install<&mdns_service_add_for_host>([](const char *instance, const char *type, const char *protocol,
                                           const char *hostname, uint16_t port, mdns_txt_item_t *, size_t items) {
        CHECK(network.responder && network.aliases.count(hostname));
        CHECK(std::string(instance) == "Amity Home Theater" && std::string(type) == "_https" &&
              std::string(protocol) == "_tcp" && port == 443 && items == 0);
        network.services.insert(hostname);
        return ESP_OK;
    });
    install<&mdns_service_remove_for_host>([](const char *instance, const char *type, const char *protocol,
                                              const char *hostname) {
        CHECK(network.responder);
        CHECK(std::string(instance) == "Amity Home Theater" && std::string(type) == "_https" &&
              std::string(protocol) == "_tcp");
        network.services.erase(hostname);
        return ESP_OK;
    });
}

// The original's user interfaces beside the administration hostname: HTTPS,
// the remote and HomeKit.
void install_interfaces() {
    install_network();
    install<&device_settings::initial_admin_auth>([] { return "private-fixture"; });
    install<&web_admin::start>([](amity::SystemControl &control, const char *hostname, const char *auth) {
        CHECK(&control == &hub && board.hostname == hostname && std::string(auth) == "private-fixture");
        // The remote worker starts after the administration server.
        CHECK(tasks_named("remote_input").empty());
        return ESP_OK;
    });
    install<&ble_remote_set_siri_callback>([](ble_remote_siri_callback_t, void *) {});
    install<&ble_remote_set_disconnect_callback>([](ble_remote_disconnect_callback_t, void *) {});
    install<&ble_remote_start>([](ble_remote_key_callback_t, void *) {
        CHECK(tasks_named("remote_input").size() == 1);
        return ESP_OK;
    });
    install<&amity_homekit::start>([](amity::SystemControl &control, const amity_matter_callbacks_t &) {
        CHECK(&control == &hub);
        return ESP_OK;
    });
}

// Retirement: the steps after the hub's fence.
void install_retirement() {
    install<&amity::HubService::retire_membership>([](amity::HubService &) {
        CHECK(!original_interfaces_enabled.load());
        return ESP_OK;
    });
    install<&web_admin::stop>([] {
        CHECK(!original_interfaces_enabled.load());
        return ESP_OK;
    });
    install<&amity_homekit::stop>([] {
        CHECK(!original_interfaces_enabled.load());
        return ESP_OK;
    });
}

// Saved and live roles.
void member() {
    board.startup.role = RuntimeStartup::member;
    board.startup.consortium_id = consortium;
    board.status.member = true;
    board.status.phase = "member";
    board.setup_eligible = false;
}
void original() {
    board.startup.role = RuntimeStartup::coordinator;
    board.status.coordinator = board.status.authority_ready = true;
    board.status.phase = "coordinator";
    board.setup_eligible = false;
}

// The two app_main settings this build was compiled with.
constexpr bool external_memory =
#if CONFIG_SPIRAM_USE_MALLOC
    true;
#else
    false;
#endif
constexpr bool production_updates =
#if CONFIG_AMITY_PRODUCTION_UPDATES
    true;
#else
    false;
#endif

// What every boot shares: the settings of this build, the tasks and queues,
// and the handlers app_main registers.
void check_boot() {
    CHECK(amity_fake::extmem_limit() == (external_memory ? 32u : 0u));
    CHECK(count("nvs_flash_init") == (production_updates ? 0u : 1u));
    CHECK(ordered({"config_store::initialize_json_memory", "config_store::init", "device_settings::finish_factory_reset",
                   "device_settings::init", "device_settings::set_hostname_handler", "system_services::start_log_capture",
                   "device_settings::set_setup_button_handler", "device_settings::start_reset_button",
                   "config_store::load_system_identity", "amity::consortium::runtime_read_startup",
                   "amity_matter_set_controller_enabled", "amity::HubService::set_system_identity",
                   "amity::HubService::set_coordinator_authority", "system_services::boot_id",
                   "amity::HubService::set_boot_identity", "amity::HubService::start",
                   "system_services::adapter_settings::install", "amity::consortium::runtime_settings_hooks",
                   "amity::consortium::runtime_log_hooks", "web_admin::load", "amity::consortium::runtime_lifecycle_hooks",
                   "system_services::updates::maintenance_hooks", "amity::radio::start",
                   "amity::consortium::runtime_register_gatt", "amity_matter_start", "esp_wifi_get_ps",
                   "amity::network_diagnostics::start", "amity::consortium::runtime_set_hardware_profile",
                   "amity::consortium::runtime_start", "esp_event_handler_register", "amity::HubService::on_change",
                   "system_services::confirm_firmware", "web_admin::finish_configuration_activation"}));
    // The hostname handler and the coordinator authority are in place before
    // Matter starts networking.
    CHECK(ordered({"device_settings::set_hostname_handler", "amity_matter_start"}));
    CHECK(ordered({"amity::HubService::set_coordinator_authority", "amity_matter_start"}));
    CHECK(registered.hostname.target<esp_err_t (*)(const std::string &, const std::string &)>() &&
          *registered.hostname.target<esp_err_t (*)(const std::string &, const std::string &)>() == rename_hostname);
    CHECK(registered.lifecycle.retire.target<esp_err_t (*)()>() &&
          *registered.lifecycle.retire.target<esp_err_t (*)()>() == retire_original_interfaces);
    CHECK(registered.setup == setup_button && registered.wifi_events && registered.on_change &&
          registered.matter.inputs == matter_inputs);
    CHECK(tasks_named("firmware_update").size() == 1 && tasks_named("remote_battery").size() == 1);
    const auto firmware = tasks_named("firmware_update").front();
    CHECK(firmware.with_caps && firmware.stack_bytes == 12288 && firmware.caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    const auto queues = amity_fake::queues();
    CHECK(queues.size() == 2 && queues[0].length == 32 && queues[0].item_size == sizeof(Input) &&
          queues[1].length == 8 && queues[1].item_size == sizeof(VolumeAdjustment));
}

// A fresh board: a passive startup that waits for its first setup with the
// Matter controller enabled, and no user interface.
void boot_unclaimed() {
    install_boot();
    // The setup task starts after the local boot is confirmed.
    install<&system_services::confirm_firmware>([] {
        CHECK(tasks_named("await_setup").empty());
        return ESP_OK;
    });
    app_main();
    check_boot();
    CHECK(board.controller == std::vector<bool>{true});
    CHECK(board.authority == std::vector<bool>{false} && board.loads == std::vector<bool>{true});
    const auto setup = tasks_named("await_setup");
    CHECK(setup.size() == 1 && !setup.front().with_caps && setup.front().stack_bytes == 12288);
    CHECK(!original_interfaces_enabled.load() && !registered.ip_events && tasks_named("remote_input").empty());
    CHECK(amity_fake::delays() == 0);
    CHECK(amity_fake::logged("Ready for BLE setup; no user interface is running"));
}

// A member: a passive startup with the Matter controller disabled and no task
// waiting for setup.
void boot_member() {
    install_boot();
    member();
    app_main();
    check_boot();
    CHECK(board.controller == std::vector<bool>{false});
    CHECK(board.authority == std::vector<bool>{false} && board.loads == std::vector<bool>{true});
    CHECK(tasks_named("await_setup").empty() && tasks_named("remote_input").empty());
    CHECK(!original_interfaces_enabled.load() && count("amity_matter_get_status") == 0);
    CHECK(amity_fake::logged("Ready as a managed device; administration is on the original Amity"));
}

// A proven original with authority starts its user interfaces at boot, without
// waiting for Matter commissioning evidence: the administration hostname, HTTPS,
// one remote worker, the remote and HomeKit, each once.
void boot_original() {
    install_boot();
    install_interfaces();
    original();
    network.up = true;
    network.address = 0x0a00000a;
    network.ipv6 = {{{0xfe80, 0, 0, 1}, 0}, {{0x20010db8, 0, 0, 2}, 0}};
    app_main();
    check_boot();
    CHECK(board.controller == std::vector<bool>{true});
    CHECK(board.authority == std::vector<bool>{true} && board.loads == std::vector<bool>{false});
    CHECK(original_interfaces_enabled.load());
    CHECK((calls({"mdns_service_add_for_host", "web_admin::start", "ble_remote_start", "amity_homekit::start"}) ==
           std::vector<std::string>{"mdns_service_add_for_host", "web_admin::start", "ble_remote_start",
                                    "amity_homekit::start"}));
    CHECK(tasks_named("remote_input").size() == 1 && tasks_named("await_setup").empty());
    CHECK(registered.ip_events && network.services == std::set<std::string>{"amity-test"});
    // The alias carries the station's IPv4 address, then its IPv6 addresses.
    CHECK(network.address_updates == 1 && network.published.size() == 3);
    CHECK(network.published[0].type == ESP_IPADDR_TYPE_V4 && network.published[0].u_addr.ip4.addr == 0x0a00000a);
    CHECK(network.published[1].type == ESP_IPADDR_TYPE_V6 && network.published[1].u_addr.ip6.addr[0] == 0xfe80);
    CHECK(network.published[2].type == ESP_IPADDR_TYPE_V6 && network.published[2].u_addr.ip6.addr[0] == 0x20010db8);
    CHECK(amity_fake::logged("Ready. Administration: https://amity-test.local (or the DHCP address)"));
}

// Matter has not named the shared responder yet: the original names it after
// its octopus ID before adding the alias.
void boot_original_unnamed_responder() {
    install_boot();
    install_interfaces();
    original();
    network.responder_hostname.clear();
    app_main();
    CHECK(network.responder_hostname == "amity-" + octopus.str().substr(24));
    CHECK(ordered({"mdns_hostname_set", "mdns_delegate_hostname_add", "mdns_service_add_for_host"}));
    CHECK(original_interfaces_enabled.load() && network.services == std::set<std::string>{"amity-test"});
}

// The retained interfaces need a running consortium and the authority.
void boot_original_without_consortium() {
    install_boot();
    original();
    board.group_start = ESP_FAIL;
    app_main();
    CHECK(!original_interfaces_enabled.load() && tasks_named("remote_input").empty() && !registered.ip_events);
    CHECK(count("web_admin::start") == 0 && count("amity_homekit::start") == 0);
}
void boot_original_unreadable() {
    install_boot();
    original();
    board.startup_read = ESP_FAIL;
    app_main();
    CHECK(!original_interfaces_enabled.load() && tasks_named("remote_input").empty() && !registered.ip_events);
    CHECK(board.controller == std::vector<bool>{false} && board.loads == std::vector<bool>{true});
}
void boot_original_without_authority() {
    install_boot();
    original();
    board.status.authority_ready = false;
    app_main();
    CHECK(!original_interfaces_enabled.load() && tasks_named("remote_input").empty() && !registered.ip_events);
}

// Retirement: the hub's fence, HTTPS, the administration service, Matter's
// controller and HomeKit, in that order.
const std::vector<std::string> headless_retirement{"amity::HubService::retire_membership", "web_admin::stop",
                                                    "amity_matter_set_controller_enabled", "amity_homekit::stop"};
const std::vector<std::string> published_retirement{"amity::HubService::retire_membership", "web_admin::stop",
                                                     "mdns_service_remove_for_host", "amity_matter_set_controller_enabled",
                                                     "amity_homekit::stop"};
std::vector<std::string> retirement_calls() {
    return calls({"amity::HubService::retire_membership", "web_admin::stop", "mdns_service_remove_for_host",
                  "amity_matter_set_controller_enabled", "amity_homekit::stop"});
}

// A member never published the administration hostname, so neither a refresh,
// a start nor its retirement enters the responder, which it never started.
void retire_headless() {
    install_boot();
    install_retirement();
    member();
    app_main();
    amity_seam::clear_calls();
    refresh_admin_mdns();
    start_admin_mdns();
    CHECK(registered.lifecycle.retire() == ESP_OK);
    CHECK(retirement_calls() == headless_retirement);
    refresh_admin_mdns();
    start_admin_mdns();
    CHECK(amity_seam::calls().size() == headless_retirement.size());
}

// The original's service is removed once, before Matter frees the shared
// responder; nothing publishes it again.
void retire_published() {
    install_boot();
    install_interfaces();
    install_retirement();
    original();
    app_main();
    CHECK(count("mdns_init") == 1 && network.address_updates == 1 && network.services.size() == 1);
    amity_seam::clear_calls();
    CHECK(registered.lifecycle.retire() == ESP_OK);
    CHECK(retirement_calls() == published_retirement && network.services.empty() && !network.responder);
    CHECK(!original_interfaces_enabled.load());
    amity_seam::clear_calls();
    refresh_admin_mdns();
    start_admin_mdns();
    CHECK(registered.lifecycle.retire() == ESP_OK);
    CHECK(retirement_calls() == headless_retirement);
    CHECK(count("mdns_init") == 0 && count("mdns_delegate_hostname_set_address") == 0);
}

// A failed step neither stops the later ones nor hides its error. The first
// error wins.
void retire_with(std::initializer_list<std::pair<const char *, esp_err_t>> failures, esp_err_t expected) {
    install_boot();
    install_interfaces();
    install_retirement();
    original();
    app_main();
    for (const auto &[step, error] : failures) {
        const std::string name = step;
        if (name == "fence") install<&amity::HubService::retire_membership>([error = error](amity::HubService &) { return error; });
        if (name == "web") install<&web_admin::stop>([error = error] { return error; });
        if (name == "matter") {
            install<&amity_matter_set_controller_enabled>([error = error](bool enabled) {
                CHECK(!enabled);
                network.responder = false;
                return error;
            });
        }
        if (name == "homekit") install<&amity_homekit::stop>([error = error] { return error; });
        if (name == "mdns") {
            install<&mdns_service_remove_for_host>([error = error](const char *, const char *, const char *, const char *) {
                CHECK(network.responder);
                return error;
            });
        }
    }
    amity_seam::clear_calls();
    CHECK(registered.lifecycle.retire() == expected);
    CHECK(retirement_calls() == published_retirement);
    // The service is never removed twice.
    amity_seam::clear_calls();
    CHECK(registered.lifecycle.retire() == expected);
    CHECK(count("mdns_service_remove_for_host") == 0);
}
void retire_fence_fails() { retire_with({{"fence", ESP_ERR_TIMEOUT}}, ESP_ERR_TIMEOUT); }
void retire_web_fails() { retire_with({{"web", ESP_ERR_INVALID_STATE}}, ESP_ERR_INVALID_STATE); }
void retire_matter_fails() { retire_with({{"matter", ESP_ERR_NO_MEM}}, ESP_ERR_NO_MEM); }
void retire_homekit_fails() { retire_with({{"homekit", ESP_FAIL}}, ESP_FAIL); }
void retire_errors_ranked() {
    retire_with({{"fence", ESP_ERR_TIMEOUT}, {"web", ESP_ERR_INVALID_STATE}, {"matter", ESP_ERR_NO_MEM},
                 {"homekit", ESP_FAIL}},
                ESP_ERR_TIMEOUT);
}
void retire_later_errors_ranked() {
    retire_with({{"web", ESP_ERR_INVALID_STATE}, {"matter", ESP_ERR_NO_MEM}, {"homekit", ESP_FAIL}}, ESP_ERR_INVALID_STATE);
    install<&amity_matter_set_controller_enabled>([](bool) { return ESP_ERR_NO_MEM; });
    install<&web_admin::stop>([] { return ESP_OK; });
    CHECK(registered.lifecycle.retire() == ESP_ERR_NO_MEM);
}
// A failed removal is reported, not returned.
void retire_mdns_removal_fails() {
    retire_with({{"mdns", ESP_FAIL}}, ESP_OK);
    CHECK(amity_fake::logged("Administration mDNS removal: ESP_FAIL"));
}

// A service that never registered is never removed.
void retire_unregistered() {
    install_boot();
    install_interfaces();
    install_retirement();
    original();
    install<&mdns_service_add_for_host>([](const char *, const char *, const char *, const char *, uint16_t,
                                           mdns_txt_item_t *, size_t) { return ESP_FAIL; });
    app_main();
    CHECK(original_interfaces_enabled.load() && network.services.empty());
    CHECK(amity_fake::logged("Administration discovery is pending; use the DHCP address"));
    amity_seam::clear_calls();
    CHECK(registered.lifecycle.retire() == ESP_OK);
    CHECK(retirement_calls() == headless_retirement);
}

// Retirement waits for a publication in flight, removes what it published, and
// only then lets Matter free the responder.
void retire_in_flight() {
    install_boot();
    install_network();
    install_retirement();
    app_main();
    std::mutex mutex;
    std::condition_variable changed;
    bool adding = false, released = false, at_web = false;
    std::vector<std::string> events;
    const auto event = [&](const char *name) {
        std::lock_guard<std::mutex> lock(mutex);
        events.push_back(name);
        changed.notify_all();
    };
    install<&mdns_service_add_for_host>([&](const char *, const char *, const char *, const char *hostname, uint16_t,
                                            mdns_txt_item_t *, size_t) {
        CHECK(network.responder);
        {
            std::unique_lock<std::mutex> lock(mutex);
            adding = true;
            changed.notify_all();
            CHECK(changed.wait_for(lock, std::chrono::seconds(60), [&] { return released; }));
        }
        network.services.insert(hostname);
        event("mdns");
        return ESP_OK;
    });
    install<&amity::HubService::retire_membership>([&](amity::HubService &) {
        event("retire");
        return ESP_OK;
    });
    install<&web_admin::stop>([&] {
        CHECK(!original_interfaces_enabled.load());
        {
            std::lock_guard<std::mutex> lock(mutex);
            events.push_back("web-stop");
            at_web = true;
        }
        changed.notify_all();
        return ESP_OK;
    });
    install<&mdns_service_remove_for_host>([&](const char *, const char *, const char *, const char *hostname) {
        CHECK(network.responder);
        network.services.erase(hostname);
        event("mdns-remove");
        return ESP_OK;
    });
    install<&amity_matter_set_controller_enabled>([&](bool enabled) {
        CHECK(!enabled);
        network.responder = false;
        event("matter-stop");
        return ESP_OK;
    });
    install<&amity_homekit::stop>([&] {
        event("homekit-stop");
        return ESP_OK;
    });
    // The setup task enabled the interfaces and is publishing them.
    original_interfaces_enabled.store(true);
    std::thread publisher(start_admin_mdns);
    {
        std::unique_lock<std::mutex> lock(mutex);
        CHECK(changed.wait_for(lock, std::chrono::seconds(60), [&] { return adding; }));
    }
    esp_err_t retired = ESP_FAIL;
    std::thread retiring([&] { retired = registered.lifecycle.retire(); });
    {
        std::unique_lock<std::mutex> lock(mutex);
        CHECK(changed.wait_for(lock, std::chrono::seconds(60), [&] { return at_web; }));
        CHECK(!original_interfaces_enabled.load());
        CHECK(std::find(events.begin(), events.end(), "matter-stop") == events.end());
        released = true;
        changed.notify_all();
    }
    publisher.join();
    retiring.join();
    CHECK(retired == ESP_OK && network.services.empty());
    CHECK((events == std::vector<std::string>{"retire", "web-stop", "mdns", "mdns-remove", "matter-stop", "homekit-stop"}));
}

// A short setup press offers Matter setup to a ready, eligible, unconfigured
// board without fabrics; a long one opens the Add Amity window.
void setup_button_matter() {
    install_boot();
    app_main();
    install<&amity::radio::reserve_consortium>([](bool inviter, uint64_t &generation) {
        CHECK(!inviter);
        generation = 77;
        return ESP_OK;
    });
    install<&amity::consortium::runtime_open_enrollment>([](uint64_t generation) {
        CHECK(generation == 77);
        return ESP_OK;
    });
    amity_seam::clear_calls();
    board.firmware_ready = false;
    registered.setup(false, registered.setup_context);
    registered.setup(true, registered.setup_context);
    CHECK((amity_seam::calls() == std::vector<std::string>{"system_services::firmware_ready", "system_services::firmware_ready"}));
    board.firmware_ready = true;
    registered.setup(false, registered.setup_context);
    CHECK(count("amity_matter_open_commissioning") == 1 && count("amity::radio::reserve_consortium") == 0);
    registered.setup(true, registered.setup_context);
    CHECK(count("amity::consortium::runtime_open_enrollment") == 1 && count("amity_matter_open_commissioning") == 1);
    // Another Amity's Add window is open.
    board.setup_eligible = false;
    registered.setup(false, registered.setup_context);
    board.setup_eligible = true;
    // A member, or a board with Matter fabrics.
    board.status.member = true;
    board.status.phase = "member";
    board.setup_eligible = false;
    registered.setup(false, registered.setup_context);
    board.status = awaiting_setup();
    board.setup_eligible = true;
    board.fabrics = 1;
    registered.setup(false, registered.setup_context);
    board.fabrics = 0;
    // A board with activities, running or saved.
    board.snapshot.activities.push_back({});
    registered.setup(false, registered.setup_context);
    board.snapshot.activities.clear();
    CHECK(count("amity_matter_open_commissioning") == 1);
    // A saved configuration without activities or equipment is not configured.
    CHECK(hub.configure_passive_metadata(saved(configuration({}))) == ESP_OK);
    registered.setup(false, registered.setup_context);
    CHECK(count("amity_matter_open_commissioning") == 2);
    CHECK(hub.configure_passive_metadata(saved(configuration({"Watch"}))) == ESP_OK);
    registered.setup(false, registered.setup_context);
    CHECK(count("amity_matter_open_commissioning") == 2);
}

// Add Amity reserves the shared radio, then opens the enrollment window; it
// releases only the generation it acquired when the window does not open, and
// never opens Matter.
void setup_button_add() {
    install_boot();
    app_main();
    esp_err_t reserve = ESP_OK, enrollment = ESP_OK;
    install<&amity::radio::reserve_consortium>([&](bool inviter, uint64_t &generation) {
        CHECK(!inviter);
        generation = reserve == ESP_OK ? 77 : 0;
        return reserve;
    });
    install<&amity::consortium::runtime_open_enrollment>([&](uint64_t generation) {
        CHECK(generation == 77);
        return enrollment;
    });
    install<&amity::radio::release>([](uint64_t generation) { CHECK(generation == 77); });
    const auto press = [] {
        amity_seam::clear_calls();
        registered.setup(true, registered.setup_context);
        return calls({"amity::radio::reserve_consortium", "amity::consortium::runtime_open_enrollment",
                      "amity::radio::release", "amity_matter_open_commissioning"});
    };
    board.firmware_ready = false;
    CHECK(press().empty());
    board.firmware_ready = true;
    board.status.initialized = false;
    CHECK(press().empty());
    board.status.initialized = true;
    reserve = ESP_ERR_INVALID_STATE;
    CHECK((press() == std::vector<std::string>{"amity::radio::reserve_consortium"}));
    reserve = ESP_OK;
    enrollment = ESP_ERR_INVALID_STATE;
    CHECK((press() == std::vector<std::string>{"amity::radio::reserve_consortium",
                                               "amity::consortium::runtime_open_enrollment", "amity::radio::release"}));
    enrollment = ESP_OK;
    CHECK((press() ==
           std::vector<std::string>{"amity::radio::reserve_consortium", "amity::consortium::runtime_open_enrollment"}));
}

// The setup task of a fresh board, which polls every 200 ms.
void install_setup_task() {
    install_interfaces();
    install<&amity_matter_controller_enrollment_complete>([] { return board.enrolled; });
    install<&amity::HubService::transition_coordinator_authority>([](amity::HubService &, bool enabled) {
        CHECK(enabled && board.status.coordinator);
        return ESP_OK;
    });
    install<&amity::consortium::runtime_claim_original_controller>([] {
        if (board.claim != ESP_OK) {
            if (board.claim_lost) board.claim_lost();
            return board.claim;
        }
        board.status.coordinator = board.status.authority_ready = true;
        board.status.phase = "coordinator";
        return ESP_OK;
    });
}
// The setup task polls every 200 ms. It may wait at most limit times, and
// then runs at each wait.
void polls(unsigned limit, std::function<void()> then = {}) {
    amity_fake::on_delay([limit, then = std::move(then)](TickType_t ticks) {
        CHECK(ticks == pdMS_TO_TICKS(200));
        if (amity_fake::delays() > limit) amity_test::fail(__FILE__, __LINE__, "the setup task kept waiting");
        if (then) then();
    });
}
// Runs the setup task until it deletes itself, checking its frame is gone by then.
void run_setup_task() {
    bool deleted = false;
    const long holders = board.configuration ? board.configuration.use_count() + 1 : 0;
    on_task_delete = [&] {
        CHECK(allocations::live() == 0);
        CHECK(!board.configuration || board.configuration.use_count() == holders);
        deleted = true;
    };
    amity_fake::run_task(task("await_setup"));
    CHECK(deleted && tasks_named("await_setup").front().state == amity_fake::Task::State::deleted);
}

// Setup waits for a station that is up with an address, then for Matter
// commissioning evidence, and only then claims the original role, promotes the
// hub, loads and activates the configuration, publishes its revision and starts
// the administration hostname, HTTPS, the remote worker, the remote and HomeKit.
void await_claim() {
    install_boot();
    install_setup_task();
    board.enrolled = true;
    app_main();
    board.configuration = saved(configuration({"Watch"}), 3);
    // The station keeps its last address while it is down.
    network.address = 0x0a00000a;
    amity_seam::clear_calls();
    polls(3, [] {
        CHECK(!original_interfaces_enabled.load() && tasks_named("remote_input").empty());
        if (amity_fake::delays() == 1) {
            CHECK(count("amity_matter_controller_enrollment_complete") == 0);
            network.up = true;
            network.address = 0;
        }
        if (amity_fake::delays() == 2) {
            CHECK(count("amity_matter_controller_enrollment_complete") == 0);
            network.address = 0x0a00000a;
            board.enrolled = false;
        }
        if (amity_fake::delays() == 3) {
            CHECK(count("amity_matter_controller_enrollment_complete") == 1);
            CHECK(count("amity::consortium::runtime_claim_original_controller") == 0);
            board.enrolled = true;
        }
    });
    install<&amity::consortium::runtime_configuration_changed>([](uint64_t revision) { CHECK(revision == 3); });
    run_setup_task();
    CHECK(amity_fake::delays() == 3 && original_interfaces_enabled.load());
    CHECK((calls({"amity::consortium::runtime_claim_original_controller", "amity::HubService::transition_coordinator_authority",
                  "web_admin::load", "web_admin::finish_configuration_activation",
                  "amity::consortium::runtime_configuration_changed", "mdns_service_add_for_host", "web_admin::start",
                  "ble_remote_start", "amity_homekit::start"}) ==
           std::vector<std::string>{"amity::consortium::runtime_claim_original_controller",
                                    "amity::HubService::transition_coordinator_authority", "web_admin::load",
                                    "web_admin::finish_configuration_activation",
                                    "amity::consortium::runtime_configuration_changed", "mdns_service_add_for_host",
                                    "web_admin::start", "ble_remote_start", "amity_homekit::start"}));
    CHECK(board.loads == (std::vector<bool>{true, false}) && tasks_named("remote_input").size() == 1);
    CHECK(amity_fake::logged("Original Amity setup complete. Administration: https://amity-test.local"));
}

// A member, a member decision that wins the claim, a prepared candidate and a
// board that needs recovery never claim the original role.
void await_member() {
    install_boot();
    app_main();
    member();
    polls(0);
    run_setup_task();
    CHECK(!original_interfaces_enabled.load() && tasks_named("remote_input").empty());
}
void await_claim_lost() {
    install_boot();
    install_setup_task();
    app_main();
    network.up = true;
    network.address = 0x0a00000a;
    board.enrolled = true;
    board.claim = ESP_ERR_INVALID_STATE;
    board.claim_lost = member;
    polls(1);
    amity_seam::clear_calls();
    run_setup_task();
    CHECK(count("amity::consortium::runtime_claim_original_controller") == 1 && board.status.member);
    CHECK((calls({"amity::consortium::runtime_claim_original_controller",
                  "amity::HubService::transition_coordinator_authority", "web_admin::load"}) ==
           std::vector<std::string>{"amity::consortium::runtime_claim_original_controller"}));
    CHECK(!original_interfaces_enabled.load() && tasks_named("remote_input").empty());
}
void await_prepared() {
    install_boot();
    install_setup_task();
    app_main();
    network.up = true;
    network.address = 0x0a00000a;
    board.enrolled = true;
    board.status.phase = "prepared";
    board.claim = ESP_ERR_INVALID_STATE;
    polls(1, member);
    amity_seam::clear_calls();
    run_setup_task();
    CHECK(count("amity::consortium::runtime_claim_original_controller") == 1 && amity_fake::delays() == 1);
    CHECK(!original_interfaces_enabled.load() && tasks_named("remote_input").empty());
}
void await_recovery() {
    install_boot();
    app_main();
    board.status.phase = "recovery_required";
    polls(0);
    run_setup_task();
    CHECK(!original_interfaces_enabled.load());
}

}  // namespace

int main(int argc, char **argv) {
    return amity_test::run(argc, argv, {
        {"boot-unclaimed", boot_unclaimed},
        {"boot-member", boot_member},
        {"boot-original", boot_original},
        {"boot-original-unnamed-responder", boot_original_unnamed_responder},
        {"boot-original-without-consortium", boot_original_without_consortium},
        {"boot-original-unreadable", boot_original_unreadable},
        {"boot-original-without-authority", boot_original_without_authority},
        {"retire-headless", retire_headless},
        {"retire-published", retire_published},
        {"retire-fence-fails", retire_fence_fails},
        {"retire-web-fails", retire_web_fails},
        {"retire-matter-fails", retire_matter_fails},
        {"retire-homekit-fails", retire_homekit_fails},
        {"retire-errors-ranked", retire_errors_ranked},
        {"retire-later-errors-ranked", retire_later_errors_ranked},
        {"retire-mdns-removal-fails", retire_mdns_removal_fails},
        {"retire-unregistered", retire_unregistered},
        {"retire-in-flight", retire_in_flight},
        {"setup-button-matter", setup_button_matter},
        {"setup-button-add", setup_button_add},
        {"await-claim", await_claim},
        {"await-member", await_member},
        {"await-claim-lost", await_claim_lost},
        {"await-prepared", await_prepared},
        {"await-recovery", await_recovery},
    });
}
