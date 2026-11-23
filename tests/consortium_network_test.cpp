/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

// The consortium network hooks of components/amity_matter/consortium_network.cpp,
// built whole against the CHIP and core fakes and driven through the hooks
// install_consortium_network_hooks() registers. The file is included so the
// scenarios can check the staging state the hooks keep. The CHIP task runs
// driver work at once; the association result arrives on a timer.
#include "../components/amity_matter/consortium_network.cpp"
#include "amity_matter_fakes.hpp"
#include "core_fakes.hpp"
#include "scenario_main.hpp"
#include "seams.hpp"
#include <condition_variable>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace network_test {

namespace wifi = chip::DeviceLayer::NetworkCommissioning;
using amity::consortium::RuntimeNetworkHooks;

constexpr uint64_t never = std::numeric_limits<uint64_t>::max();

RuntimeNetworkHooks hooks;

uint64_t clock_ms() { return uint64_t(amity_fake::now_us() / 1000); }

// The station interface. Its IPv4 address arrives at ip_at.
esp_netif_t *const station = reinterpret_cast<esp_netif_t *>(0x5a);
bool netif_present = true, netif_up = true;
uint64_t ip_at = never;

// The pinned driver keeps one network and connects asynchronously.
struct Driver {
    std::vector<std::string> networks;
    bool null_iterator = false;
    unsigned iterators = 0, releases = 0;
    wifi::Status remove_status = wifi::Status::kSuccess;
    wifi::Status add_status = wifi::Status::kSuccess;
    uint8_t add_index = 0;
    // The driver's admission of the new network takes this long.
    uint64_t admission_ms = 0;
    // The association result arrives this long after the connect, or never.
    uint64_t association_delay = 5000;
    bool association_ok = true;
    uint64_t associated_at = never, reverted_at = never;
    CHIP_ERROR commit_result = CHIP_NO_ERROR;
    unsigned connects = 0, removes = 0, adds = 0;
    std::string removed, added_ssid, added_password, connected;
    wifi::ESPWiFiDriver::ConnectCallback *callback = nullptr;
    esp_timer_handle_t association = nullptr;
} driver;

class Networks final : public wifi::NetworkIterator {
public:
    size_t Count() override { return driver.networks.size(); }
    bool Next(wifi::Network &item) override {
        if (next_ >= driver.networks.size()) return false;
        const auto &ssid = driver.networks[next_++];
        std::memcpy(item.networkID, ssid.data(), ssid.size());
        item.networkIDLen = uint8_t(ssid.size());
        return true;
    }
    void Release() override {
        ++driver.releases;
        delete this;
    }

private:
    size_t next_ = 0;
};

std::string text(chip::ByteSpan span) { return {reinterpret_cast<const char *>(span.data()), span.size()}; }

void associate(void *) {
    driver.associated_at = clock_ms();
    auto *callback = driver.callback;
    driver.callback = nullptr;
    amity_fake::run_as_chip_task([&] {
        callback->OnResult(driver.association_ok ? wifi::Status::kSuccess : wifi::Status::kOtherConnectionFailure,
                           chip::CharSpan(), 0);
    });
}

// Each driver call runs on the CHIP task. Delays never run there, nor with
// the stack lock held, and each poll waits at most 100 ms.
void script() {
    using wifi::ESPWiFiDriver;
    amity_seam::install<&esp_netif_get_handle_from_ifkey>([](const char *key) {
        CHECK(std::strcmp(key, "WIFI_STA_DEF") == 0);
        return netif_present ? station : nullptr;
    });
    amity_seam::install<&esp_netif_is_netif_up>([](esp_netif_t *netif) {
        CHECK(netif == station);
        return netif_up;
    });
    amity_seam::install<&esp_netif_get_ip_info>([](esp_netif_t *netif, esp_netif_ip_info_t *info) {
        CHECK(netif == station);
        info->ip.addr = clock_ms() >= ip_at ? 0x0201a8c0 : 0;
        return ESP_OK;
    });
    amity_seam::install<&esp_ptr_internal>([](const void *) { return true; });
    amity_seam::install<&ESPWiFiDriver::GetNetworks>([](ESPWiFiDriver &) -> wifi::NetworkIterator * {
        CHECK(amity_fake::on_chip_task());
        if (driver.null_iterator) return nullptr;
        ++driver.iterators;
        return new Networks;
    });
    amity_seam::install<&ESPWiFiDriver::RemoveNetwork>(
        [](ESPWiFiDriver &, chip::ByteSpan id, chip::MutableCharSpan &, uint8_t &) {
            CHECK(amity_fake::on_chip_task());
            ++driver.removes;
            driver.removed = text(id);
            return driver.remove_status;
        });
    amity_seam::install<&ESPWiFiDriver::AddOrUpdateNetwork>(
        [](ESPWiFiDriver &, chip::ByteSpan ssid, chip::ByteSpan credentials, chip::MutableCharSpan &, uint8_t &index) {
            CHECK(amity_fake::on_chip_task());
            ++driver.adds;
            driver.added_ssid = text(ssid);
            driver.added_password = text(credentials);
            amity_fake::advance_ms(driver.admission_ms);
            index = driver.add_index;
            return driver.add_status;
        });
    amity_seam::install<&ESPWiFiDriver::ConnectNetwork>(
        [](ESPWiFiDriver &, chip::ByteSpan id, ESPWiFiDriver::ConnectCallback *callback) {
            CHECK(amity_fake::on_chip_task() && callback);
            ++driver.connects;
            driver.connected = text(id);
            driver.callback = callback;
            if (driver.association_delay == never) return;
            const esp_timer_create_args_t args{associate, nullptr, ESP_TIMER_TASK, "association", false};
            CHECK(esp_timer_create(&args, &driver.association) == ESP_OK);
            CHECK(esp_timer_start_once(driver.association, driver.association_delay * 1000) == ESP_OK);
        });
    amity_seam::install<&ESPWiFiDriver::RevertConfiguration>([](ESPWiFiDriver &) {
        CHECK(amity_fake::chip_stack_locked());
        driver.reverted_at = clock_ms();
        return CHIP_NO_ERROR;
    });
    amity_seam::install<&ESPWiFiDriver::CommitConfiguration>([](ESPWiFiDriver &) {
        CHECK(amity_fake::chip_stack_locked());
        return driver.commit_result;
    });
    amity_seam::install<&amity::consortium::runtime_network_hooks>([](RuntimeNetworkHooks captured) { hooks = std::move(captured); });
    amity_fake::chip_work(amity_fake::ChipWork::immediate);
    amity_fake::on_delay([](TickType_t ticks) {
        CHECK(!amity_fake::on_chip_task() && !amity_fake::chip_stack_locked());
        CHECK(ticks > 0 && ticks <= pdMS_TO_TICKS(100));
    });
}

void begin() {
    script();
    CHECK(install_consortium_network_hooks() == ESP_OK);
    CHECK(hooks.export_credentials && hooks.stage_connect && hooks.commit && hooks.rollback && hooks.get_hostname &&
          hooks.set_hostname);
}

unsigned reverts() { return unsigned(amity_seam::count("chip::DeviceLayer::NetworkCommissioning::ESPWiFiDriver::RevertConfiguration")); }
unsigned commits() { return unsigned(amity_seam::count("chip::DeviceLayer::NetworkCommissioning::ESPWiFiDriver::CommitConfiguration")); }
bool transaction_free() { return !xSemaphoreGetMutexHolder(transaction); }

// The association succeeds and IPv4 arrives at 6 s or at 34.9 s: staging
// succeeds without an early rollback or retry, inside the one 35 s budget,
// and releases the transaction.
void delayed_address(uint64_t ready) {
    begin();
    ip_at = ready;
    CHECK(hooks.stage_connect("unit-network", "unit-password") == ESP_OK);
    CHECK(staged && !pending && reverts() == 0 && driver.connects == 1);
    CHECK(clock_ms() >= ready && clock_ms() < 35000 && amity_fake::delays() > 0);
    CHECK(transaction_free());
    CHECK(driver.added_ssid == "unit-network" && driver.added_password == "unit-password" &&
          driver.connected == "unit-network");
}
void address_at_6s() { delayed_address(6000); }
void address_at_34900ms() { delayed_address(34900); }

// An address present at the association completes without polling.
void immediate_address() {
    begin();
    ip_at = 5000;
    CHECK(hooks.stage_connect("unit-network", "") == ESP_OK);
    CHECK(staged && amity_fake::delays() == 0 && reverts() == 0 && driver.added_password.empty());
}

// A failed association rolls back at once, at 5 s, without waiting for DHCP.
void failed_association() {
    begin();
    driver.association_ok = false;
    CHECK(hooks.stage_connect("unit-network", "unit-password") == ESP_ERR_TIMEOUT);
    CHECK(reverts() == 1 && driver.reverted_at == 5000 && amity_fake::delays() == 0 && !staged && transaction_free());
}

// Without an association result, staging times out and rolls back at 35 s.
void absent_callback() {
    begin();
    driver.association_delay = never;
    CHECK(hooks.stage_connect("unit-network", "unit-password") == ESP_ERR_TIMEOUT);
    CHECK(reverts() == 1 && driver.reverted_at == 35000 && amity_fake::delays() == 0 && !staged);
}

// A missing, on-deadline or late address rolls back at the original deadline
// with one connect.
void address_deadline(uint64_t ready) {
    begin();
    ip_at = ready;
    CHECK(hooks.stage_connect("unit-network", "unit-password") == ESP_ERR_TIMEOUT);
    CHECK(reverts() == 1 && driver.reverted_at == 35000 && driver.connects == 1 && !staged && transaction_free());
}
void address_never() { address_deadline(never); }
void address_at_deadline() { address_deadline(35000); }
void address_after_deadline() { address_deadline(35100); }

// Driver admission, association and DHCP share one budget.
void single_budget() {
    begin();
    driver.admission_ms = 3000;
    ip_at = 36000;
    CHECK(hooks.stage_connect("unit-network", "unit-password") == ESP_ERR_TIMEOUT);
    CHECK(driver.associated_at == 8000 && driver.reverted_at == 35000 && driver.connects == 1);
}

// An association between poll steps still rolls back at the deadline itself:
// the last poll waits only for the time that remains.
void deadline_between_polls() {
    begin();
    driver.association_delay = 5050;
    CHECK(hooks.stage_connect("unit-network", "unit-password") == ESP_ERR_TIMEOUT);
    CHECK(driver.associated_at == 5050 && driver.reverted_at == 35000 && driver.connects == 1);
}

// A failed removal of the stored network or a failed addition returns
// INVALID_STATE without pending or staged state or connection work, and
// releases the transaction.
void admission_failure(bool remove) {
    begin();
    driver.networks = {"older-network"};
    if (remove)
        driver.add_status = wifi::Status::kOutOfRange;
    else
        driver.remove_status = wifi::Status::kNetworkIDNotFound;
    CHECK(hooks.stage_connect("unit-network", "unit-password") == ESP_ERR_INVALID_STATE);
    CHECK(!pending && !staged && driver.connects == 0 && reverts() == 0 && amity_fake::delays() == 0);
    CHECK(transaction_free() && driver.removes == 1 && driver.adds == (remove ? 1u : 0u));
    CHECK(driver.iterators == 1 && driver.releases == 1);
}
void remove_failure() { admission_failure(false); }
void add_failure() { admission_failure(true); }

// The stored network is removed unless it is the one being staged; a driver
// holding several networks, no iterator or a network at another index fails
// admission.
void stored_networks() {
    begin();
    driver.networks = {"unit-network"};
    ip_at = 5000;
    CHECK(hooks.stage_connect("unit-network", "pw") == ESP_OK && driver.removes == 0);
    hooks.rollback();
    CHECK(!staged);
    ip_at = never;
    driver.networks = {"older-network"};
    driver.association_ok = false;
    CHECK(hooks.stage_connect("unit-network", "pw") == ESP_ERR_TIMEOUT && driver.removed == "older-network");
    driver.networks = {"one", "two"};
    CHECK(hooks.stage_connect("unit-network", "pw") == ESP_ERR_INVALID_STATE);
    driver.networks.clear();
    driver.null_iterator = true;
    CHECK(hooks.stage_connect("unit-network", "pw") == ESP_ERR_INVALID_STATE);
    driver.null_iterator = false;
    driver.add_index = 1;
    CHECK(hooks.stage_connect("unit-network", "pw") == ESP_ERR_INVALID_STATE && driver.connects == 2);
    CHECK(driver.iterators == driver.releases && driver.iterators == 4 && transaction_free() && !pending);
}

// Bad credentials are refused before any network work; so is staging while
// staged, while a connection is pending or while the station has an address.
void refused() {
    begin();
    for (const auto &[ssid, password] : std::vector<std::pair<std::string, std::string>>{
             {"", "pw"}, {std::string(33, 's'), "pw"}, {"unit", std::string(65, 'p')},
             {std::string("un\0it", 5), "pw"}, {"unit", std::string("p\0w", 3)}})
        CHECK(hooks.stage_connect(ssid, password) == ESP_ERR_INVALID_ARG);
    CHECK(amity_seam::count("esp_netif_get_handle_from_ifkey") == 0 && driver.adds == 0);
    CHECK(hooks.stage_connect(std::string(32, 's'), std::string(64, 'p')) == ESP_ERR_TIMEOUT);
    ip_at = 0;
    CHECK(hooks.stage_connect("unit-network", "pw") == ESP_ERR_INVALID_STATE && driver.adds == 1);
    ip_at = never;
    // The earlier attempt left no result, so its connection is still pending.
    driver.association_delay = never;
    CHECK(hooks.stage_connect("unit-network", "pw") == ESP_ERR_TIMEOUT && pending);
    CHECK(hooks.stage_connect("unit-network", "pw") == ESP_ERR_INVALID_STATE && driver.adds == 2);
    // Its late result ends it; the next attempt drains that result first.
    amity_fake::run_as_chip_task([] { callback.OnResult(wifi::Status::kSuccess, chip::CharSpan(), 0); });
    CHECK(!pending);
    driver.association_ok = false;
    driver.association_delay = 1000;
    const auto before = clock_ms();
    CHECK(hooks.stage_connect("unit-network", "pw") == ESP_ERR_TIMEOUT && driver.reverted_at == before + 1000);
    driver.association_ok = true;
    driver.association_delay = 5000;
    ip_at = clock_ms() + 5000;
    CHECK(hooks.stage_connect("unit-network", "pw") == ESP_OK && staged);
    netif_up = false;
    CHECK(hooks.stage_connect("unit-network", "pw") == ESP_ERR_INVALID_STATE && driver.adds == 4);
}

// Commit needs staging and an address, commits the driver on the CHIP task
// and ends staging; a failed commit keeps it. Rollback reverts the driver and
// ends staging.
void commit_and_rollback() {
    begin();
    CHECK(hooks.commit() == ESP_ERR_INVALID_STATE && commits() == 0);
    ip_at = 6000;
    CHECK(hooks.stage_connect("unit-network", "pw") == ESP_OK && staged);
    driver.commit_result = CHIP_ERROR_INTERNAL;
    CHECK(hooks.commit() == ESP_FAIL && staged && commits() == 1);
    netif_up = false;
    CHECK(hooks.commit() == ESP_ERR_INVALID_STATE && staged && commits() == 1);
    netif_up = true;
    driver.commit_result = CHIP_NO_ERROR;
    CHECK(hooks.commit() == ESP_OK && !staged && commits() == 2 && transaction_free());
    CHECK(hooks.commit() == ESP_ERR_INVALID_STATE && commits() == 2);
    ip_at = clock_ms() + 5000;
    CHECK(hooks.stage_connect("unit-network", "pw") == ESP_OK && staged);
    const auto before = reverts();
    hooks.rollback();
    CHECK(!staged && reverts() == before + 1 && transaction_free());
}

// While another task holds the transaction, commit, rollback and staging do
// nothing.
void transaction_busy() {
    begin();
    std::mutex gate;
    std::condition_variable changed;
    bool held = false, done = false;
    std::thread holder([&] {
        CHECK(xSemaphoreTake(transaction, 0) == pdTRUE);
        std::unique_lock<std::mutex> lock(gate);
        held = true;
        changed.notify_all();
        changed.wait(lock, [&] { return done; });
        CHECK(xSemaphoreGive(transaction) == pdTRUE);
    });
    {
        std::unique_lock<std::mutex> lock(gate);
        changed.wait(lock, [&] { return held; });
    }
    CHECK(hooks.commit() == ESP_ERR_INVALID_STATE);
    hooks.rollback();
    CHECK(hooks.stage_connect("unit-network", "pw") == ESP_ERR_INVALID_STATE);
    CHECK(reverts() == 0 && commits() == 0 && driver.adds == 0);
    {
        std::lock_guard<std::mutex> lock(gate);
        done = true;
    }
    changed.notify_all();
    holder.join();
    CHECK(transaction_free());
}

// Driver work runs at once when the caller holds the stack lock, only on an
// internal-RAM stack, and fails when the CHIP task refuses it.
void driver_calls() {
    begin();
    ip_at = 5000;
    CHECK(hooks.stage_connect("unit-network", "pw") == ESP_OK);
    const auto scheduled = amity_fake::scheduled_chip_work();
    amity_seam::install<&esp_ptr_internal>([](const void *) { return false; });
    CHECK(hooks.commit() == ESP_FAIL && commits() == 0 && staged);
    {
        esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
        CHECK(hooks.commit() == ESP_ERR_INVALID_STATE && commits() == 0);
        amity_seam::install<&esp_ptr_internal>([](const void *) { return true; });
        CHECK(hooks.commit() == ESP_OK && commits() == 1 && !staged);
    }
    CHECK(amity_fake::scheduled_chip_work() == scheduled + 1);
    ip_at = clock_ms() + 5000;
    CHECK(hooks.stage_connect("unit-network", "pw") == ESP_OK);
    amity_fake::fail_chip_scheduling(1);
    CHECK(hooks.commit() == ESP_FAIL && staged && commits() == 1);
}

// Credentials are exported only with a usable address, read under the stack
// lock, at their stored lengths.
void exported_credentials() {
    begin();
    amity_seam::install<&esp_wifi_get_config>([](wifi_interface_t interface, wifi_config_t *config) {
        CHECK(interface == WIFI_IF_STA && amity_fake::chip_stack_locked());
        std::memset(config, 0, sizeof(*config));
        std::memcpy(config->sta.ssid, "home", 4);
        std::memset(config->sta.password, 'p', sizeof(config->sta.password));
        return ESP_OK;
    });
    std::string ssid = "old", password = "old";
    CHECK(!hooks.export_credentials(ssid, password) && amity_seam::count("esp_wifi_get_config") == 0);
    ip_at = 0;
    CHECK(hooks.export_credentials(ssid, password) && ssid == "home" && password == std::string(64, 'p'));
    CHECK(!amity_fake::chip_stack_locked());
    amity_seam::install<&esp_wifi_get_config>([](wifi_interface_t, wifi_config_t *) { return ESP_ERR_INVALID_STATE; });
    ssid = password = "kept";
    CHECK(!hooks.export_credentials(ssid, password) && ssid == "kept" && password == "kept");
}

// The hostname hooks delegate to the device settings: reading the hostname,
// and applying an enrollment hostname exactly once with its error returned,
// without touching the network interface.
void hostname_hooks() {
    begin();
    amity_seam::install<&device_settings::hostname>([] { return "amity-den"; });
    CHECK(hooks.get_hostname() == "amity-den");
    for (const auto outcome : {ESP_OK, ESP_FAIL, ESP_ERR_INVALID_STATE, ESP_ERR_TIMEOUT}) {
        amity_seam::install<&device_settings::apply_enrollment_hostname>([outcome](std::string name) {
            CHECK(name == "Amity-Den");
            return outcome;
        });
        amity_seam::clear_calls();
        CHECK(hooks.set_hostname("Amity-Den") == outcome);
        CHECK(amity_seam::calls() == std::vector<std::string>{"device_settings::apply_enrollment_hostname"});
    }
}

// Installing fails without its semaphores and registers nothing.
void install_failure() {
    script();
    amity_fake::fail_queue_creations(1);
    CHECK(install_consortium_network_hooks() == ESP_ERR_NO_MEM);
    CHECK(amity_seam::count("amity::consortium::runtime_network_hooks") == 0);
}

}  // namespace network_test

int main(int argc, char **argv) {
    using namespace network_test;
    return amity_test::run(argc, argv,
                           {{"address-at-6s", address_at_6s},
                            {"address-at-34900ms", address_at_34900ms},
                            {"immediate-address", immediate_address},
                            {"failed-association", failed_association},
                            {"absent-callback", absent_callback},
                            {"address-never", address_never},
                            {"address-at-deadline", address_at_deadline},
                            {"address-after-deadline", address_after_deadline},
                            {"single-budget", single_budget},
                            {"deadline-between-polls", deadline_between_polls},
                            {"remove-failure", remove_failure},
                            {"add-failure", add_failure},
                            {"stored-networks", stored_networks},
                            {"refused", refused},
                            {"commit-and-rollback", commit_and_rollback},
                            {"transaction-busy", transaction_busy},
                            {"driver-calls", driver_calls},
                            {"exported-credentials", exported_credentials},
                            {"hostname-hooks", hostname_hooks},
                            {"install-failure", install_failure}});
}
